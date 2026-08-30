/*
===========================================================================
Copyright (C) 2023 the OpenMoHAA team

This file is part of OpenMoHAA source code.

OpenMoHAA source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

OpenMoHAA source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with OpenMoHAA source code; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/

#include "cg_local.h"

// Absolute ceilings for the view-weapon recoil offset, in world units. Constants rather
// than multiples of the per-shot kick, so that "can this reach the camera?" has an answer
// that does not depend on weapon class, cvar values, or how long the trigger was held.
// HZM coop - how long the vault flourish runs, ms. The server-side vault is an INSTANT
// (one velocity assignment), so this duration is invented client-side and is the only
// thing that gives the move a shape. ~420ms is about how long the ballistic arc from
// vFwd*150 + z*310 actually takes to clear a chest-high obstacle.
#define COOP_VAULT_MS 420

#define RECOIL_MAX_UNITS 3.0f
#define RECOIL_MAX_BACK  1.1f
#include "cg_parsemsg.h"

//============================================================================

/*
=================
CG_CalcVrect

Sets the coordinates of the rendered window
=================
*/
static void CG_CalcVrect(void)
{
    int size;

    // the intermission should allways be full screen
    if (cg.snap->ps.pm_flags & PMF_INTERMISSION) {
        size = 100;
    } else {
        // bound normal viewsize
        if (cg_viewsize->integer < 30) {
            cgi.Cvar_Set("viewsize", "30");
            size = 30;
        } else if (cg_viewsize->integer > 100) {
            cgi.Cvar_Set("viewsize", "100");
            size = 100;
        } else {
            size = cg_viewsize->integer;
        }
    }
    cg.refdef.width = cgs.glconfig.vidWidth * size / 100;
    cg.refdef.width &= ~1;

    cg.refdef.height = cgs.glconfig.vidHeight * size / 100;
    cg.refdef.height &= ~1;

    cg.refdef.x = (cgs.glconfig.vidWidth - cg.refdef.width) / 2;
    cg.refdef.y = (cgs.glconfig.vidHeight - cg.refdef.height) / 2;
}

//==============================================================================

// HZM coop - STAGED THIRD-PERSON ADS envelopes (see CG_UpdateAdsStage, updated once per frame in
// CG_CalcViewValues; same first-order ease style as the ADS zoom s_adsZoomCur in CG_CalcFov).
// s_adsShoulderEnv: 0 = normal chase framing -> 1 = over-the-RIGHT-SHOULDER aim framing (ADS held in 3P).
// s_adsFpEnv:       0 = shoulder framing     -> 1 = collapsed onto the head (the wheel-up handoff into
//                   first-person irons; the view flips to first person once it crosses cg_adsFpFlip).
static float s_adsShoulderEnv = 0.0f;
static float s_adsFpEnv       = 0.0f;
// s_shoulderSideSign: +1 = right shoulder, -1 = left. MOUSE3 while shoulder-aiming toggles the
// archived cg_adsShoulderRight cvar (CG_CheckCaptureKey, cg_ui.cpp); this eases toward the target
// so the camera SWEEPS across the back to the other shoulder instead of snapping.
static float s_shoulderSideSign = 1.0f;

// HZM coop - THIRD-PERSON FREE CAM envelope + capture state (see CG_UpdateFreecam, updated once per frame
// in CG_CalcViewValues right after CG_UpdateAdsStage; same first-order ease as the ADS envelopes above).
// s_freecamEnv: 0 = normal chase camera -> 1 = free orbit fully applied. The ORBIT ANGLES themselves live
// in the CLIENT exe (cl_main.cpp camera_offset, accumulated from mouse deltas in CL_MouseMove while the
// cgame publishes cg_freecamCapture 1) and are read here through the existing cgi.get_camera_offset()
// import - no new cgame ABI. ADS/turret/scope/etc drop the capture (mouse aims the player again) and the
// env eases the applied orbit back behind the shoulder rather than snapping.
static float    s_freecamEnv     = 0.0f;

// HZM coop [user 07-29] DBNO CAMERA envelope. 0 = normal framing, 1 = fully "downed" framing.
// Same eased-envelope idiom as s_adsShoulderEnv / s_freecamEnv so going down and being revived
// glide rather than cut - a hard jump of the eye by ~30 units reads as a teleport.
static float    s_dbnoCamEnv     = 0.0f;
// HZM coop [user 2026-08-19] SHELL SHOCK DIZZINESS - "camera should have a dizziness effect
// that lasts a bit longer". The server stuffs `set coop_dizzy <0..1>` on a near blast (same
// trigger as the tinnitus ring); the view sways on decaying sinusoids for
// coop_dizzyTime * severity seconds. Consumed-and-cleared so each blast restarts it.
static float s_dizzySev   = 0.0f;
static int   s_dizzyStart = 0;

// HZM coop [user 2026-08-19] RELOAD CAMERA SWAY - "when you reload i'd like the camera to move
// with it... gun gets lifted up to pull out mag, cam goes up, mag goes in, cam back down".
// State-driven, no per-gun timing tables: while ps.iViewModelAnim is any reload state the view
// eases up (fast rise, like the gun being hefted), and eases back down once the reload anim
// ends - so it tracks single-loaders (RELOAD_SINGLE loops) and interrupted reloads for free.
// coop_reloadSway = peak pitch in degrees (0 disables).

// HZM coop [user 2026-08-21] CAMERA MOTION: ONE SCALE, ONE CEILING.
//
// Everything below moves the CAMERA rather than the weapon. That is a different risk class: a
// weapon that overshoots looks wrong, a camera that overshoots makes people ill. Two rules hold the
// whole stack:
//
//   * coop_camMotion scales every camera-motion term here, so the user has ONE knob and 0 restores
//     a perfectly rigid camera.
//   * CoopCamClamp() bounds the SUM. Individually these terms are all small; the failure mode is
//     six of them peaking together (landing hard, hurt, sprinting, turning) and there was nothing
//     stopping that. A ceiling in world units and degrees is checkable; "each term is small" is not.
//
// The clamp is applied ONCE to the accumulated offset rather than per term, because per-term clamps
// are exactly how a sum stays "within budget" while being six times the budget.
static float CoopCamMotion(void)
{
    static cvar_t *pOn = NULL;
    if (!pOn) {
        pOn = cgi.Cvar_Get("coop_camMotion", "1", CVAR_ARCHIVE);
    }
    if (pOn->value < 0.0f) {
        return 0.0f;
    }
    return (pOn->value > 2.0f) ? 2.0f : pOn->value;
}

// Bound an accumulated camera offset. maxUnits is the ceiling on translation.
static void CoopCamClamp(vec3_t ofs, float maxUnits)
{
    float len = VectorLength(ofs);
    if (len > maxUnits && len > 0.0001f) {
        VectorScale(ofs, maxUnits / len, ofs);
    }
}

// [user 2026-08-20] ONE master switch for the whole weapon-feel batch (landing dip, footfall
// bob, idle breathing, handling shake). Five feel layers shipped in a single batch and none of
// them had been judged individually, so there was no way to answer "is this better than before"
// without a rebuild per layer. `coop_weaponFeel 0` restores the pre-batch viewmodel exactly.
static qboolean CoopWeaponFeelOn(void)
{
    static cvar_t *pFeel = NULL;
    if (!pFeel) {
        pFeel = cgi.Cvar_Get("coop_weaponFeel", "1", CVAR_ARCHIVE);
    }
    return (qboolean)(pFeel->integer != 0);
}

// [2026-08-21] THE FEEL BUDGET. The weapon position before any feel layer touches it, and
// whether it was captured this frame (it is not on paths that skip the weapon offset).
// Authored, deliberately-large stows (medkit, weapon collision, DBNO eye drop) accumulate here
// and are EXEMPT from the jitter budget - clamping them broke all three.
// The ROLL channel needs its own ceiling. Translation is bounded by the feel budget, but roll has
// five independent contributors - lean, the low-health limp, injury sway, the CalcViewValues lean
// and the new strafe bank - and nothing bounded their sum. Hurt + limping + leaning + strafing
// stacks past 7 degrees of horizon tilt, which is the same "six of them peaking together" failure
// the translation budget exists to prevent.
static float    s_fRollBase = 0.0f;
static qboolean s_bRollBase = qfalse;
static vec3_t   s_vFeelExempt = {0, 0, 0};
static vec3_t   s_vFeelBase = {0, 0, 0};
// Surface-visibility probe published by cg_modelanim.c at submit time: 2 bits per surface
// (exists, hidden) for lefthand / garandhand / viewsleeves / triggerhand / sleeves.
extern int      g_iCoopSurfMask;
static vec3_t   s_vTraceVM   = {0, 0, 0};   // final viewmodel origin, published for coop_adsTrace
static qboolean s_bTraceVMok = qfalse;
static vec3_t   s_vTraceBone[8] = {{0,0,0},{0,0,0},{0,0,0},{0,0,0},
                                   {0,0,0},{0,0,0},{0,0,0},{0,0,0}}; // eyes/spine/hand/gun/LClav/LArm/RClav/RArm
static qboolean s_bTraceBone = qfalse;
static qboolean s_bFeelBase = qfalse;
// HZM coop [user 2026-08-21] "slight fov snap when shooting". A small, fast fov widening on
// discharge. Set by the shot detector, consumed in CG_CalcFov. Kept SMALL and SHORT: fov is the
// most sickness-prone channel there is, and a slow fov move reads as a zoom rather than a kick.
static float s_fovPunch   = 0.0f;
static float s_shakeAmp   = 0.0f;   // handling-tremor AMPLITUDE, eased. The oscillation itself
                                    // is stateless - computed at apply time. See bug note below.
static float s_reloadLift = 0.0f;   // eased pitch, degrees; advanced once per frame
static float s_reloadRoll = 0.0f;   // roll companion, eased separately

// HZM coop [user 2026-08-20] DYNAMIC RELOAD FEEL. v1 timed the entire camera excursion off a
// hardcoded 900ms while the viewmodel reload clips actually run 0.63s (shotgun fill) to 4.80s
// (bazooka). So on EVERY gun the camera reached full lift in the first 19-56% of the reload and
// then PARKED there - up to 2.5 seconds of motionless held camera on a Kar98 - which is precisely
// why "no reload feels different from the last one". The phase is now normalised against the
// animation's REAL length, read once per animation instance: Garand 2.00s, Kar98 3.37s, Thompson
// 2.77s diverge with no per-gun table and no randomness.
static void CG_ReloadFeelAdvance(void)
{
    static cvar_t *pRS = NULL, *pWt = NULL, *pRetime = NULL, *pMax = NULL, *pDbg = NULL;
    static int     s_iLastSlot = -1;
    static float   s_fAnimLen  = 0.9f; // seconds, cached per animation instance
    static float   s_fWeight   = 1.0f; // per-class amplitude, latched on the edge (also
                                       // scales the handling shake below)
    float          fTarget, fRate, fDt, k, fPhase, fPeak;
    int            iAnim, iSlot, iClass;
    qboolean       bAlive;

    if (!pRS) {
        pRS     = cgi.Cvar_Get("coop_reloadSway", "1.6", CVAR_ARCHIVE);
        pWt     = cgi.Cvar_Get("coop_reloadWeight", "1", CVAR_ARCHIVE);
        pRetime = cgi.Cvar_Get("coop_reloadRetime", "1", CVAR_ARCHIVE);
        pMax    = cgi.Cvar_Get("coop_reloadSwayMax", "3.0", CVAR_ARCHIVE);
        pDbg    = cgi.Cvar_Get("coop_reloadDebug", "0", 0); // never archive a diagnostic (TRAPS T7)
    }
    if (pRS->value <= 0.0f || !cg.snap) {
        // s_shakeAmp MUST be cleared here too. CG_ApplyReloadFeel adds it unconditionally and is
        // called independently of coop_reloadSway, so leaving it set here re-opens bug-1984 -
        // the permanent camera tremor - via a different door.
        s_shakeAmp   = 0.0f;
        s_reloadLift = s_reloadRoll = 0.0f;
        s_iLastSlot  = -1;
        return;
    }

    // a spectator keeps STAT_HEALTH at max, so health alone is not "alive"
    bAlive = (qboolean)(cg.snap->ps.stats[STAT_HEALTH] > 0
                        && !(cg.snap->ps.pm_flags & (PMF_SPECTATING | PMF_INTERMISSION)));

    iAnim  = cg.snap->ps.iViewModelAnim;
    iSlot  = cgi.anim ? cgi.anim->g_iCurrentVMAnimSlot : -1;
    iClass = cg.snap->ps.stats[STAT_EQUIPPED_WEAPON];

    // ANIMATION-START EDGE. The slot advances on every (re)start - a new state, a weapon change,
    // and a forced restart per shell - so per-shell pumping comes free; the edge is only needed
    // to refresh the cached length and weight.
    if (iSlot != s_iLastSlot) {
        s_iLastSlot = iSlot;
        s_fAnimLen  = 0.9f;
        s_fWeight   = 1.0f;

        if (pRetime->integer && iSlot >= 0 && cg.pPlayerFPSModel && cgi.anim) {
            int   idx  = cgi.anim->g_VMFrameInfo[iSlot].index;
            int   idle = cgi.Anim_NumForName(cg.pPlayerFPSModel, "idle");
            float len  = (idx >= 0) ? cgi.Anim_Time(cg.pPlayerFPSModel, idx) : 0.0f;
            // the viewmodel code SILENTLY substitutes "idle" when <prefix>_reload does not
            // resolve; normalising against an idle length would make the ramp crawl with no
            // visible diagnostic, so fall back to the old constant and band-limit the absurd
            if (idx >= 0 && idx != idle && len > 0.2f && len < 6.0f) {
                s_fAnimLen = len;
            }
        }
        if (pWt->integer) {
            // the same per-class weights the shipped weapon-lag spring uses: an already-networked
            // signal, so a Thompson and a BAR diverge without inventing a 69-gun table
            if (iClass & WEAPON_CLASS_PISTOL) {
                s_fWeight = 0.55f;
            } else if (iClass & WEAPON_CLASS_SMG) {
                s_fWeight = 0.80f;
            } else if (iClass & WEAPON_CLASS_RIFLE) {
                s_fWeight = 1.10f;
            } else if (iClass & WEAPON_CLASS_MG) {
                s_fWeight = 1.50f;
            } else if (iClass & WEAPON_CLASS_HEAVY) {
                s_fWeight = 1.35f;
            }
        }
        if (pDbg->integer && iAnim >= VM_ANIM_RECHAMBER && iAnim <= VM_ANIM_PUTAWAY) {
            cgi.Printf("^~^~^ WFEEL state=%d slot=%d len=%.3f class=0x%x wt=%.2f peak=%.2f\n", iAnim,
                       iSlot, s_fAnimLen, iClass, s_fWeight, pRS->value * s_fWeight);
        }
    }

    // PHASE from the viewmodel's own duration counter: reset on the same edge and only ever
    // accumulated, so it is monotone within an instance
    fPhase = (cgi.anim && s_fAnimLen > 0.0f)
                 ? (float)cgi.anim->g_iCurrentVMDuration * 0.001f / s_fAnimLen
                 : 1.0f;
    if (fPhase < 0.0f) {
        fPhase = 0.0f;
    } else if (fPhase > 1.0f) {
        fPhase = 1.0f;
    }
    // [user 2026-08-20] "reload speeds for all guns based on calm vs stressed" / "I think we
    // should also try to make weapon movement feel dynamic, no reload should feel the same".
    //
    // The TIMING is deliberately not touched. The viewmodel clock is client-side while the real
    // reload completion is server-side, so scaling only the visible half desynchronises the
    // animation from when ammo actually returns - and slowing a reload under stress punishes the
    // player at the exact moment they are already losing. What changes is the CHARACTER: a rattled
    // player's hands travel further and settle less tidily over the identical duration.
    fPeak = pRS->value * s_fWeight;
    {
        static cvar_t *pStrAmt = NULL;
        if (!pStrAmt) { pStrAmt = cgi.Cvar_Get("coop_wfeelStressAmt", "0.45", CVAR_ARCHIVE); }
        fPeak *= (1.0f + pStrAmt->value * CoopWFeelStress());
    }

    if (!bAlive || cg.renderingThirdPerson || (cg.snap->ps.pm_flags & PMF_CAMERA_VIEW)) {
        // in 3P/cutscene/dead the duration counter does not advance either, so decay the envelope
        // out rather than tracking a frozen phase that would snap on the way back to first person
        fTarget = 0.0f;
        fRate   = 4.5f;
    } else if (iAnim == VM_ANIM_RELOAD) {
        // two perceived beats across the WHOLE animation, then the hands come back down BEFORE the
        // state ends - this is the fix for the parked plateau
        if (fPhase < 0.28f) {
            fTarget = fPeak * (0.62f * (fPhase / 0.28f));
        } else if (fPhase < 0.62f) {
            fTarget = fPeak * (0.62f + 0.38f * ((fPhase - 0.28f) / 0.34f));
        } else {
            fTarget = fPeak * (1.00f - 0.75f * ((fPhase - 0.62f) / 0.38f));
        }
        fRate = (fTarget > s_reloadLift) ? 7.0f : 4.5f;
    } else if (iAnim == VM_ANIM_RELOAD_SINGLE) {
        // per-shell PUMP rather than a flat hold: rises and falls inside each shell's own clip
        fTarget = fPeak * 0.70f * (float)sin(3.14159265f * fPhase);
        fRate   = (fTarget > s_reloadLift) ? 9.0f : 6.0f;
    } else if (iAnim == VM_ANIM_RELOAD_END) {
        // the cock. v1 parked at a flat negative target for the whole state, and reload_end runs
        // about a second - so it was a three-quarter-second downward STARE. Impulse and return.
        float dd = 1.0f - fPhase;
        fTarget  = fPeak * -0.28f * dd * dd;
        fRate    = 11.0f;
    } else if (iAnim == VM_ANIM_PULLOUT) {
        // [user 2026-08-20] "camera movement to go along with pulling out guns... make it feel
        // like it has weight". Bringing a weapon up is the clearest weight cue in the game: the
        // muzzle climbs into view, overshoots as the arms decelerate, and settles. A BAR should
        // cost more to raise than a pistol, which is exactly what the class weight already says.
        // Rise fast, overshoot at ~0.55, settle by the end.
        if (fPhase < 0.55f) {
            fTarget = fPeak * 0.85f * (fPhase / 0.55f);
        } else {
            fTarget = fPeak * 0.85f * (1.0f - (fPhase - 0.55f) / 0.45f) * 0.55f;
        }
        fRate = (fTarget > s_reloadLift) ? 9.0f : 5.5f;
    } else if (iAnim == VM_ANIM_PUTAWAY) {
        // stowing: the muzzle drops away, so the view settles DOWNWARD and recovers as it clears
        fTarget = fPeak * -0.45f * (float)sin(3.14159265f * fPhase);
        fRate   = 8.0f;
    } else if (iAnim == VM_ANIM_RECHAMBER) {
        // working a bolt: a short sharp dip and return, sized off the same weight
        float dd = 1.0f - fPhase;
        fTarget  = fPeak * -0.34f * dd * dd;
        fRate    = 12.0f;
    } else {
        fTarget = 0.0f;
        fRate   = 4.5f;
    }

    fDt = cg.frametime * 0.001f;
    k   = fRate * fDt;
    if (k > 1.0f) {
        k = 1.0f; // MANDATORY. Unclamped this DIVERGES: the frame clamp is 200ms on a listen host
                  // but 5000ms for a client of a remote server, which at rate 11 gives k = 55 and
                  // a camera dive of about -21 degrees.
    }
    s_reloadLift += (fTarget - s_reloadLift) * k;
    s_reloadRoll += (fTarget * 0.30f - s_reloadRoll) * k * 0.8f; // roll lags the pitch slightly

    // HANDLING SHAKE. Only the AMPLITUDE is state; the oscillation is recomputed from scratch
    // at apply time.
    //
    // [user 2026-08-20 - bug] The first version added the sine DIRECTLY INTO s_reloadLift, which
    // is the persistent eased state rather than an output. That is a feedback loop with three
    // separate failure modes, and it shipped: (a) the next frame the ease saw the tremor as part
    // of the value it was tracking and fought it; (b) the entry gate |s_reloadLift| > 0.01 was
    // held true BY THE TREMOR ITSELF, so once a reload started the shake never stopped; (c) the
    // return-to-rest snap |s_reloadLift| < 0.004 could therefore never fire either. Net effect:
    // after the first reload of a map, a permanent ~2.75 Hz tremor on the CAMERA lasting until
    // map change - reported as the gun being jittery and the whole game feeling unsmooth. Never
    // write a periodic term into the state variable an exponential ease is tracking.
    {
        static cvar_t *pShake = NULL;
        float          fShakeTarget = 0.0f;
        if (!pShake) {
            pShake = cgi.Cvar_Get("coop_weaponShake", "0.10", CVAR_ARCHIVE);
        }
        // gate on the ANIMATION being live, not on the envelope magnitude the shake feeds
        if (CoopWeaponFeelOn() && pShake->value > 0.0f && bAlive && !cg.renderingThirdPerson
            && iAnim >= VM_ANIM_RECHAMBER && iAnim <= VM_ANIM_PUTAWAY) {
            fShakeTarget = pShake->value * s_fWeight
                           * (0.35f + 0.65f * (fabs(fTarget) / (fPeak + 0.001f)))
                           * (1.0f + 1.2f * CoopWFeelStress()); // rattled hands are less tidy
        }
        s_shakeAmp += (fShakeTarget - s_shakeAmp) * k;
        if (s_shakeAmp < 0.0005f && fShakeTarget == 0.0f) {
            s_shakeAmp = 0.0f;
        }
    }

    if (s_reloadLift > pMax->value) {
        s_reloadLift = pMax->value;
    } else if (s_reloadLift < -pMax->value) {
        s_reloadLift = -pMax->value;
    }
    if (fabs(s_reloadLift) < 0.004f && fabs(fTarget) < 0.004f) {
        s_reloadLift = s_reloadRoll = 0.0f;
    }
}

static void CG_ApplyReloadFeel(vec3_t vAngles)
{
    float fLift = s_reloadLift, fRoll = s_reloadRoll;

    // stateless tremor: fixed frequencies, so there is no phase to drift and nothing to latch
    if (s_shakeAmp > 0.0f) {
        float t = cg.time * 0.001f;
        fLift += s_shakeAmp * ((float)sin(t * 17.3f) * 0.6f + (float)sin(t * 27.9f) * 0.4f);
        fRoll += s_shakeAmp * 0.5f * (float)sin(t * 21.1f);
    }
    if (fLift == 0.0f && fRoll == 0.0f) {
        return;
    }
    // ADS and the native scope are separate predicates - CG_AimingDownSights returns FALSE while
    // STAT_INZOOM, so the sniper (the worst case) needs the second term or it goes undamped.
    // Scale AFTER the ease, never inside the target, or releasing ADS mid-reload steps.
    if (CG_AimingDownSights() || cg.snap->ps.stats[STAT_INZOOM]) {
        static cvar_t *pAds = NULL;
        float          fCap = 0.30f;
        if (!pAds) {
            pAds = cgi.Cvar_Get("coop_reloadSwayAds", "0.15", CVAR_ARCHIVE);
        }
        fLift *= pAds->value;
        fRoll = 0.0f; // zero roll under sights: it tilts the sight picture
        if (fLift > fCap) {
            fLift = fCap;
        } else if (fLift < -fCap) {
            fLift = -fCap;
        }
    }
    vAngles[0] -= fLift;
    vAngles[2] += fRoll;
}

static void CG_ApplyShellShock(vec3_t vAngles)
{
    static cvar_t *pDz  = NULL;
    static cvar_t *pDzT = NULL;
    float          fT, fDur, fDecay, fS;

    if (!pDz) {
        pDz  = cgi.Cvar_Get("coop_dizzy", "0", 0);
        pDzT = cgi.Cvar_Get("coop_dizzyTime", "4.2", CVAR_ARCHIVE);
    }
    if (pDz->value > 0.0f) {
        if (pDz->value > s_dizzySev || cg.time > s_dizzyStart + 5000) {
            s_dizzySev   = pDz->value;
            s_dizzyStart = cg.time;
        }
        cgi.Cvar_Set("coop_dizzy", "0");
    }
    if (s_dizzySev <= 0.0f) {
        return;
    }
    fDur = pDzT->value * s_dizzySev;
    if (fDur < 1.2f) {
        fDur = 1.2f;
    }
    fT = (cg.time - s_dizzyStart) * 0.001f;
    if (fT >= fDur) {
        s_dizzySev = 0.0f;
        return;
    }
    fDecay = 1.0f - (fT / fDur);
    fDecay *= fDecay;
    fS = s_dizzySev * fDecay;
    vAngles[2] += sin(fT * 5.3f) * 5.5f * fS;
    vAngles[0] += sin(fT * 3.9f + 1.3f) * 2.8f * fS;
    vAngles[1] += sin(fT * 2.9f + 2.1f) * 1.6f * fS;
}

/*
=================
CG_UpdateDbnoCam

HZM coop [user 07-29] - eases s_dbnoCamEnv toward the DBNO state. Driven by coop_dbnoView, the
same per-client cvar the script stuffs for the DBNO audio fade and the post-process vignette, so
the camera can never disagree with the rest of the downed presentation.
=================
*/
static void CG_UpdateDbnoCam(void)
{
    static cvar_t *pDbnoV = NULL, *pSpeed = NULL;
    float          tgt, dt, rate, step;

    if (!pDbnoV) { pDbnoV = cgi.Cvar_Get("coop_dbnoView",     "0",  0); }
    if (!pSpeed) { pSpeed = cgi.Cvar_Get("cg_dbnoCamSpeed",   "6",  CVAR_ARCHIVE); }

    tgt  = (pDbnoV->integer) ? 1.0f : 0.0f;
    dt   = (cg.frametime > 0) ? (float)cg.frametime / 1000.0f : 0.0f;
    rate = (pSpeed->value > 0.0f) ? pSpeed->value : 6.0f;

    // ease back UP faster than down: being revived should feel like getting picked up, while
    // going down should feel like collapsing into the dirt.
    step = dt * ((tgt < s_dbnoCamEnv) ? rate * 1.6f : rate);
    if (step > 1.0f) { step = 1.0f; }
    s_dbnoCamEnv += (tgt - s_dbnoCamEnv) * step;
    if (s_dbnoCamEnv > tgt - 0.003f && s_dbnoCamEnv < tgt + 0.003f) {
        s_dbnoCamEnv = tgt; // settle
    }
}
static qboolean s_freecamCapture = qfalse;

/*
===============
CG_OffsetThirdPersonView

===============
*/
#define CAMERA_MINIMUM_DISTANCE 40

// HZM coop [user 2026-08-07] COVER CAMERA LIFT - shared by both view paths.
// The crouched cover eye sits level with the obstacle, so the crosshair points into the cover and
// the player has to tilt up to place shots. coop_coverView is mirrored from the server (change-only
// stufftext, same as coop_limpView); coop_coverViewRaise is how far to lift, live and archived.
// THIRD PERSON ONLY, per user: cover force-switches the view to 3P, so that is the only path that
// runs while in cover. First person is deliberately left completely untouched - applying it there
// as well is what left a stale lift behind on exit and threw the camera at the ceiling.
float CG_CoopCoverViewLift(void)
{
    static cvar_t *pCoverView  = NULL;
    static cvar_t *pCoverRaise = NULL;

    if (!pCoverView)  { pCoverView  = cgi.Cvar_Get("coop_coverView",      "0",  0); }
    if (!pCoverRaise) { pCoverRaise = cgi.Cvar_Get("coop_coverViewRaise", "16", CVAR_ARCHIVE); }

    {
        float lift = pCoverView->integer ? pCoverRaise->value : 0.0f;

        // [user 2026-08-21] "when ads from behind cover you don't actually look down the sights
        // like you do when you actually go ADS."
        //
        // This lift exists so a player in cover can see OVER it, and it raises the eye by
        // coop_coverViewRaise - 16 units by default, about a head. But every per-gun ADS tune in
        // s_adsGunTune was dialled with the eye at its normal height, so while the lift is applied
        // the sights are aligned for an eye that is 16 units below where the camera actually is.
        // No per-gun value can fix that: the tune table has no cover column, and adding one would
        // mean re-dialling all 45 rows for a second pose.
        //
        // So the lift yields to the sights instead. It stays at full strength at the hip and
        // through the over-the-shoulder stage - which is where seeing over cover matters - and
        // eases to nothing as the weapon comes up to the irons, on the same factor as the rest of
        // the ADS pose, so it moves as one motion rather than as a second correction.
        if (lift != 0.0f && CG_AdsForceFirstPerson()) {
            lift *= (1.0f - CG_AdsPoseFactor());
        }
        return lift;
    }
}

static void CG_OffsetThirdPersonView(void)
{
    vec3_t        forward;
    vec3_t        right;
    vec3_t        original_camera_position;
    vec3_t        new_vieworg;
    trace_t       trace;
    vec3_t        min, max;
    float        *look_offset;
    float        *target_angles;
    float        *target_position;
    vec3_t        delta;
    vec3_t        original_angles;
    qboolean      lookactive, resetview;
    static vec3_t saved_look_offset;
    vec3_t        camera_offset;
    float         fCamDist, fCamSide, fCamHeight, fCamVert;
    qboolean      bTurret3p;
    qboolean      bMinDistTrip; // camera closer than REQUESTED (a real wall), not merely close

    // [user 2026-08-07] lift the eye BEFORE the chase camera is derived from it, so third person
    // gets the same cover raise as first person.
    cg.refdef.vieworg[2] += CG_CoopCoverViewLift();

    target_angles   = cg.refdefViewAngles;
    target_position = cg.refdef.vieworg;

    // HZM coop - 3P ON A MOUNTED TURRET (jeep .30cal / MG42 / halftrack): normally the turret's bound
    // server camera owns the view, which in third person left the view at the gun's eye-bone INSIDE
    // the drawn player model ("camera glitched inside my body"). When rendering third person on a
    // turret we chase instead: this flag takes the turret camera's ANGLES as the aim reference
    // (authoritative gun aim, interpolated in cg_predict.c) and skips the CF_CAMERA_* transition
    // adjustments below (they configure the bound-camera view, not a chase). The stock pull-back and
    // MASK_CAMERASOLID traces then frame the gunner like any other 3P view. First-person players are
    // untouched (the PMF_CAMERA_VIEW copy in CG_CalcViewValues still runs for them).
    bTurret3p = ((cg.predicted_player_state.pm_flags & PMF_TURRET)
                 && (cg.predicted_player_state.pm_flags & PMF_CAMERA_VIEW)) ? qtrue : qfalse;

    // HZM coop - staged 3P ADS framing blend: ease the chase framing toward the over-the-shoulder AIM
    // framing while s_adsShoulderEnv is up (ADS held), then collapse toward the head as s_adsFpEnv rises
    // (mouse-wheel-up handoff into first-person irons). Live-tune cg_adsShoulderDist/Side/Up.
    fCamDist   = cg_cameradist->value;
    // [user 2026-08-21] SIGNED, so MOUSE3 swaps shoulders in EVERY third-person view - plain chase
    // and behind cover - not only while shoulder-aiming. The sign is eased in CG_UpdateAdsStage and
    // runs unconditionally, so the swap sweeps rather than cuts wherever it is used.
    fCamSide   = cg_camerasideoffset->value * s_shoulderSideSign;
    fCamHeight = cg_cameraheight->value;
    fCamVert   = cg_cameraverticaldisplacement->value;
    // HZM coop - FREE CAM framing: while the free orbit is up, pull the camera out to cg_freecamDist and
    // centre it (no shoulder side-bias while circling the character). Applied BEFORE the ADS blends below
    // so the shoulder framing wins the handoff as its envelope rises. Pivot/height stay the chase cam's.
    if (s_freecamEnv > 0.001f) {
        static cvar_t *pFcDist = NULL;
        if (!pFcDist) { pFcDist = cgi.Cvar_Get("cg_freecamDist", "100", CVAR_ARCHIVE); }
        fCamDist += (pFcDist->value - fCamDist) * s_freecamEnv;
        fCamSide += (0.0f - fCamSide) * s_freecamEnv;
    }
    // HZM coop [user 07-29] DBNO CHASE: downed, the stock framing pivots off a standing eye height
    // and leaves the camera hovering well above a body lying in the dirt - you end up looking down at
    // your own back instead of down the pistol, which is exactly the aim the user needs while crawling.
    // Drop the pivot to near ground level, shorten the pull-back (a long boom clips through the floor
    // when the pivot is this low) and reduce the vertical lift so the camera sits BEHIND the body
    // rather than over it. Applied before the ADS/freecam blends so those still win the handoff.
    if (s_dbnoCamEnv > 0.001f) {
        static cvar_t *pDbDist = NULL, *pDbHeight = NULL, *pDbVert = NULL, *pDbSide = NULL;
        // [user 07-29] Defaults are the values the user tuned in-game and asked to keep. Height is 16,
        // not the 6 he had set: the mis-placed eye drop above was adding a further +10 to this same
        // pivot while he was tuning, so folding it in here reproduces the framing he actually saw.
        if (!pDbDist)   { pDbDist   = cgi.Cvar_Get("cg_dbnoCamDist",   "100", CVAR_ARCHIVE); }
        if (!pDbHeight) { pDbHeight = cgi.Cvar_Get("cg_dbnoCamHeight", "16",  CVAR_ARCHIVE); }
        if (!pDbVert)   { pDbVert   = cgi.Cvar_Get("cg_dbnoCamVert",   "-30", CVAR_ARCHIVE); }
        if (!pDbSide)   { pDbSide   = cgi.Cvar_Get("cg_dbnoCamSide",   "14",  CVAR_ARCHIVE); }
        fCamDist   += (pDbDist->value   - fCamDist)   * s_dbnoCamEnv;
        fCamHeight += (pDbHeight->value - fCamHeight) * s_dbnoCamEnv;
        fCamVert   += (pDbVert->value   - fCamVert)   * s_dbnoCamEnv;
        fCamSide   += (pDbSide->value   - fCamSide)   * s_dbnoCamEnv;
    }
    if (s_adsShoulderEnv > 0.001f) {
        static cvar_t *pShDist = NULL, *pShSide = NULL, *pShUp = NULL;
        if (!pShDist) { pShDist = cgi.Cvar_Get("cg_adsShoulderDist", "45", CVAR_ARCHIVE); }
        if (!pShSide) { pShSide = cgi.Cvar_Get("cg_adsShoulderSide", "26", CVAR_ARCHIVE); }
        if (!pShUp)   { pShUp   = cgi.Cvar_Get("cg_adsShoulderUp",   "20", CVAR_ARCHIVE); }
        // [user 2026-08-27] "camera is too close when im ads in third person and turn to look
        // behind me, so I cant really tell". The 45u shoulder framing is tuned for a standing
        // player; lying down it sits almost on top of a body that is now 20u tall and sweeping
        // through 180 degrees, so the flip is unreadable at the exact moment you need to see it.
        // Prone (belly or back) pulls the camera back and lifts it.
        {
            static cvar_t *pPrDist = NULL, *pPrUp = NULL;
            if (!pPrDist) { pPrDist = cgi.Cvar_Get("cg_adsShoulderProneDist", "85", CVAR_ARCHIVE); }
            if (!pPrUp)   { pPrUp   = cgi.Cvar_Get("cg_adsShoulderProneUp",   "38", CVAR_ARCHIVE); }
            if (cg.predicted_player_state.pm_flags & PMF_VIEW_PRONE) {
                fCamDist   += (pPrDist->value - fCamDist)   * s_adsShoulderEnv;
                fCamSide   += (pShSide->value * s_shoulderSideSign - fCamSide) * s_adsShoulderEnv;
                fCamHeight += (pPrUp->value   - fCamHeight) * s_adsShoulderEnv;
                fCamVert   += (0.0f           - fCamVert)   * s_adsShoulderEnv;
            } else {
        fCamDist   += (pShDist->value - fCamDist)   * s_adsShoulderEnv;
        // side target is signed: MOUSE3 swaps shoulders (s_shoulderSideSign eased in CG_UpdateAdsStage)
        fCamSide   += (pShSide->value * s_shoulderSideSign - fCamSide) * s_adsShoulderEnv;
        fCamHeight += (pShUp->value   - fCamHeight) * s_adsShoulderEnv;
        fCamVert   += (0.0f           - fCamVert)   * s_adsShoulderEnv;
            }
        }
    }
    if (s_adsFpEnv > 0.001f) {
        // fly the camera in to (nearly) the head; the 3P->1P flip happens at cg_adsFpFlip of this ease
        fCamDist   += (2.0f - fCamDist)   * s_adsFpEnv;
        fCamSide   += (0.0f - fCamSide)   * s_adsFpEnv;
        fCamHeight += (0.0f - fCamHeight) * s_adsFpEnv;
        fCamVert   += (0.0f - fCamVert)   * s_adsFpEnv;
    }

    if (bTurret3p) {
        // chase the GUN's aim: the turret camera angles track the barrel exactly
        VectorCopy(cg.camera_angles, target_angles);
    } else {
        // see if angles are absolute
        if (cg.predicted_player_state.camera_flags & CF_CAMERA_ANGLES_ABSOLUTE) {
            VectorClear(target_angles);
        }

        // see if we need to ignore yaw
        if (cg.predicted_player_state.camera_flags & CF_CAMERA_ANGLES_IGNORE_YAW) {
            target_angles[YAW] = 0;
        }

        // see if we need to ignore pitch
        if (cg.predicted_player_state.camera_flags & CF_CAMERA_ANGLES_IGNORE_PITCH) {
            target_angles[PITCH] = 0;
        }

        // offset the current angles by the camera offset
        VectorSubtract(target_angles, cg.predicted_player_state.camera_offset, target_angles);
    }

    // Get the position of the camera after any needed rotation
    look_offset = cgi.get_camera_offset(&lookactive, &resetview);

    // HZM coop - THIRD-PERSON FREE CAM orbit: look_offset IS the client-side orbit accumulator (mouse
    // deltas routed there by CL_MouseMove while cg_freecamCapture is up). Add it yaw+pitch onto the
    // chase angles, scaled by the envelope so engaging ADS (or any other capture drop) EASES the camera
    // back behind the shoulder instead of snapping. Everything downstream - the pull-back along the
    // orbit direction, the MASK_CAMERASOLID wall traces and the wall-pitch fallback - is inherited, so
    // the orbit gets the stock collision handling for free. Pitch is clamped ~+/-85 (no pole flip);
    // the legacy look path below is skipped while we own the offset (saved_look_offset kept synced so
    // handing back is seamless).
    if (s_freecamEnv > 0.001f) {
        target_angles[YAW] += look_offset[YAW] * s_freecamEnv;
        target_angles[PITCH] += look_offset[PITCH] * s_freecamEnv;
        if (target_angles[PITCH] > 85) {
            target_angles[PITCH] = 85;
        } else if (target_angles[PITCH] < -85) {
            target_angles[PITCH] = -85;
        }
        VectorCopy(look_offset, saved_look_offset);
    } else if ((!resetview) && ((cg.predicted_player_state.camera_flags & CF_CAMERA_ANGLES_ALLOWOFFSET) || (lookactive))) {
        VectorSubtract(look_offset, saved_look_offset, camera_offset);
        VectorAdd(target_angles, camera_offset, target_angles);
        if (target_angles[PITCH] > 90) {
            target_angles[PITCH] = 90;
        } else if (target_angles[PITCH] < -90) {
            target_angles[PITCH] = -90;
        }
    } else {
        VectorCopy(look_offset, saved_look_offset);
    }

    target_angles[YAW]   = AngleNormalize360(target_angles[YAW]);
    target_angles[PITCH] = AngleNormalize180(target_angles[PITCH]);

    // Move reference point up

    target_position[2] += fCamHeight; // HZM coop - staged-ADS blended framing (== cg_cameraheight when idle)

    VectorCopy(target_position, original_camera_position);

    // Move camera back from reference point

    AngleVectors(target_angles, forward, right, NULL);

    // HZM coop [user 2026-08-21] SWAP SHOULDERS ON AN ARC, NOT THROUGH THE BODY.
    // "using middle mouse to switch shoulders in ANY view... camera should move from left to right
    // behind the player so you should effectively see the back of your player as it moves, right now
    // it shifts over and kinda just warps."
    //
    // The side offset was already eased, so the swap was not a teleport - but easing it alone moves
    // the camera in a STRAIGHT LATERAL LINE from +side to -side, and that line passes straight through
    // the player's head at the midpoint. The model is drawn from the inside for a frame or two and the
    // eye reads the whole move as a glitch rather than as travel, which is why an eased swap still
    // looked like a warp.
    //
    // A real camera would swing AROUND the player. Bulging the chase distance by how far through the
    // swap we are turns the straight chord into an arc that bows out behind: fully out at either
    // shoulder the bulge is zero and framing is exactly as before, at the halfway point the camera is
    // furthest back and centred, looking at the back of the soldier. One extra term, no new state -
    // the existing eased sign already carries the progress.
    {
        static cvar_t *pShArc = NULL;
        float          fArc;

        if (!pShArc) { pShArc = cgi.Cvar_Get("cg_adsShoulderArc", "26", CVAR_ARCHIVE); }
        fArc = 1.0f - fabs(s_shoulderSideSign);   // 0 at either shoulder, 1 mid-sweep
        if (fArc > 0.001f) {
            fCamDist += fArc * pShArc->value;
        }
    }

VectorMA(target_position, -fCamDist, forward, new_vieworg);

    new_vieworg[2] += fCamVert;

    // HZM coop - shift the camera to the right shoulder (over-the-shoulder third person)
    VectorMA(new_vieworg, fCamSide, right, new_vieworg);

    // Create a bounding box for our camera

    min[0] = -5;
    min[1] = -5;
    min[2] = -5;

    max[0] = 5;
    max[1] = 5;
    max[2] = 5;

    // Make sure camera does not collide with anything
    // HZM coop - bTurret3p: trace against the WORLD only (cliptoentities false). The gunner's head sits
    // inside the mounted gun's/vehicle's collision, so an entity-clipping trace is startsolid and pins
    // the camera at the head ("camera under the receiver" bug). World brushes still clip - the camera
    // never goes through terrain/walls; it may briefly intersect the vehicle model, which reads fine.
    CG_Trace(
        &trace,
        cg.playerHeadPos,
        min,
        max,
        new_vieworg,
        0,
        MASK_CAMERASOLID,
        qfalse,
        bTurret3p ? qfalse : qtrue,
        "ThirdPersonTrace 1"
    );

    VectorCopy(trace.endpos, target_position);

    // calculate distance from end position to head position
    VectorSubtract(target_position, cg.playerHeadPos, delta);
    // kill any negative z difference in delta
    if (delta[2] < CAMERA_MINIMUM_DISTANCE) {
        delta[2] = 0;
    }
    // HZM coop [user 2026-08-21] THE ADS JOLT. Gate this fallback on OBSTRUCTION, not PROXIMITY.
    //
    // Stock premise: with cg_cameradist 65 the only way the camera ends up within 40 units of the head
    // is a wall, so crank the pitch up to 90 and look down at the player from above. That premise is
    // invalidated by our own staged ADS blend, which DELIBERATELY drives fCamDist toward 2.0 as the
    // shoulder view flies in to the eye (see the s_adsFpEnv block above). So this fires on the blend
    // itself - measured at s_adsFpEnv ~0.057 flying in, about 6ms after the wheel, and releasing again
    // around ~0.31 on the way out. While it holds, the camera sits fCamDist units straight UP; when it
    // releases, the view snaps from pivot+38.9*z to pivot-38.9*forward in a single frame with no ease.
    // That is a ~55 unit teleport, and it is the jolt.
    //
    // It also explains the older "scrolling up to go ads puts the camera behind the players head"
    // report, which was previously attributed to cover ordering and fixed there without touching this.
    //
    // Seven earlier fixes missed it because they all targeted the ANIMATION half - crossblend, zoom
    // timing, pose easing. This is neither animation nor the 3P->1P flip: the world FOV and the weapon
    // shift are both driven by CG_AdsPoseFactor and are provably continuous across the flip.
    //
    // The honest question is not "is the camera close?" but "is the camera closer than we ASKED for?".
    // new_vieworg still holds the requested position here (the trace above wrote target_position, not
    // new_vieworg), so the request is free to measure. During ADS the request shrinks with the blend,
    // so the blend can never trip this; a real wall still trips it at any stage, and the primary
    // MASK_CAMERASOLID traces above are untouched, so the camera still cannot pass through geometry.
    {
        vec3_t vWant;
        float  fWant, fLimit;

        VectorSubtract(new_vieworg, cg.playerHeadPos, vWant);
        fWant  = VectorLength(vWant) * 0.9f;
        fLimit = (fWant < CAMERA_MINIMUM_DISTANCE) ? fWant : (float)CAMERA_MINIMUM_DISTANCE;
        bMinDistTrip = (VectorLength(delta) < fLimit) ? qtrue : qfalse;
    }
    if (bMinDistTrip) {
        VectorNormalize(delta);
        /*
      // see if we are going straight up
      if ( ( delta[ 2 ] > 0.75 ) && ( height > 0.85f * cg.predicted_player_state.viewheight ) )
         {
         // we just need to lower our start position slightly, since we are on top of the player
         new_vieworg[ 2 ] -= 16;
	      CG_Trace(&trace, cg.playerHeadPos, min, max, new_vieworg, 0, MASK_CAMERASOLID, qfalse, true, "ThirdPersonTrace 2" );
	      VectorCopy(trace.endpos, target_position);
         }
      else
*/
        {
            // we are probably up against the wall so we want the camera to pitch up on top of the player
            // save off the original angles
            VectorCopy(target_angles, original_angles);
            // start cranking up the target angles, pitch until we are the correct distance away from the player
            while (target_angles[PITCH] < 90) {
                target_angles[PITCH] += 2;

                AngleVectors(target_angles, forward, right, NULL);

                VectorMA(original_camera_position, -fCamDist, forward, new_vieworg);

                new_vieworg[2] += fCamVert;

                // HZM coop - keep the right-shoulder offset in the wall-pitch fallback too
                VectorMA(new_vieworg, fCamSide, right, new_vieworg);

                CG_Trace(
                    &trace,
                    cg.playerHeadPos,
                    min,
                    max,
                    new_vieworg,
                    0,
                    MASK_CAMERASOLID,
                    qfalse,
                    bTurret3p ? qfalse : qtrue, // HZM coop - world-only on turrets (see Trace 1)
                    "ThirdPersonTrace 3"
                );

                VectorCopy(trace.endpos, target_position);

                // calculate distance from end position to head position
                VectorSubtract(target_position, cg.playerHeadPos, delta);
                // kill any negative z difference in delta
                if (delta[2] < 0) {
                    delta[2] = 0;
                }
                if (VectorLength(delta) >= CAMERA_MINIMUM_DISTANCE) {
                    target_angles[PITCH] = (0.25f * target_angles[PITCH]) + (0.75f * original_angles[PITCH]);
                    // set the pitch to be that of the angle we are currently looking
                    //target_angles[ PITCH ] = original_angles[ PITCH ];
                    break;
                }
            }
            if (target_angles[PITCH] > 90) {
                // if we failed, go with the original angles
                target_angles[PITCH] = original_angles[PITCH];
            }
        }
    }
}

// HZM coop - HOLD BREATH (steady aim) state. Updated each frame in CG_OffsetFirstPersonView; read by the
// HUD via CG_GetBreathState. While ADS, holding the run/walk key (Shift / BUTTON_RUN) suppresses the ADS
// sway for up to cg_breathHoldTime seconds, then a cg_breathCooldown-second recharge before it's usable.
static int      s_breathRemainMs    = -1; // ms of breath left (-1 = uninitialised)
static int      s_breathCooldownEnd = 0;  // cg.time when the recharge ends (0 = not recharging)
static int      s_breathLastTime    = 0;  // cg.time at last update (for dt)
static qboolean s_breathSteady      = qfalse; // is breath actively steadying THIS frame
static qboolean s_breathWasSteady   = qfalse; // previous frame's steady state (edge-detect for sounds)

// HZM coop - FREE-AIM DEADZONE state. The mouse drives the AIM (ps.viewangles) as normal, but the CAMERA is
// held inside a small box behind the aim: s_fa* is the aim-vs-camera offset (degrees), accumulated from the
// per-frame aim delta and clamped to the box. Set in CG_CalcViewValues; read by the crosshair + view weapon.
static float    s_faYaw      = 0.0f, s_faPitch     = 0.0f;
static float    s_faPrevYaw  = 0.0f, s_faPrevPitch = 0.0f;
static qboolean s_faInit     = qfalse;
static float    s_faCamYaw   = 0.0f, s_faCamPitch  = 0.0f; // smoothed (weighted) camera angles
static qboolean s_faCamInit  = qfalse;

// HZM coop - SUPPRESSION (under-fire) FX intensity 0..1. Bumped by CG_AddSuppression (near-miss bullet
// zings, from cg_parsemsg.cpp) + by taking damage (in CG_CalcFov), decays each frame, published to the
// renderer as r_ppSuppress. File-static so both CG_AddSuppression and CG_CalcFov share it.
static float    s_coopSuppress = 0.0f;

// HZM coop [2026-08-20] hoisted from CG_OffsetFirstPersonView so the feel-context scalar can
// read it. Still only ADVANCED there, so it is first-person-only - see CG_FeelStressAdvance.
static float    s_spEnvCur  = 0.0f;    // sprint lower envelope, mirrored for sprint-to-fire
static float    s_spStam    = 9999.0f; // client mirror of the stamina pool (seconds)
static float    s_spStamMax = 5.0f;    // the max it was clamped against this frame

// HZM coop [user 08-02] - ON-HIT BLOOD intensity 0..1. Distinct from suppression: suppression is the
// sustained "under fire" state (near-misses count), this fires only when a round actually LANDS on the
// local player. Decayed in CG_CalcFov, published to the renderer as r_ppHit.
static float    s_coopHit = 0.0f;

// Bump the suppression intensity (clamped to 1). Called when an enemy round cracks past the listener.

// HZM coop [user 2026-08-21] WEAPON HANDLING FOLEY.
//
// Almost every handling motion this file performs made no sound at all: sprinting, crouching, going
// to sights, switching weapons, the idle inspect, the low-ammo mag check, bumping the muzzle into a
// wall, landing, and pulling the trigger on an empty gun. The animation half was built; this is the
// audible half.
//
// Anti-repetition is a no-immediate-repeat draw rather than a bag shuffle: with only four takes per
// slot a bag guarantees a cycle, which on a repeated action (crouch-spam) reads as a fixed loop.
// Rejecting only the previous index keeps it unpredictable while never doubling a take back to back.
//
// Channel is auto (S_StartLocalSound), so these never cut another cue and are never cut by one -
// handling foley overlapping a reload is correct. Rate limiting therefore has to live here.
static void CoopGunFoley(const char *act, int cooldownMs)
{
    static cvar_t *pOn = NULL;
    static int     s_last[8];   // last index played, per action slot
    static int     s_next[8];   // earliest time this slot may fire again
    static unsigned s_seed = 2463534242u;
    const char    *cls;
    char           name[64];
    int            slot, idx, iClass;

    if (!pOn) {
        pOn = cgi.Cvar_Get("coop_gunFoley", "1", CVAR_ARCHIVE);
    }
    if (pOn->value <= 0.0f || !cg.snap || cg.snap->ps.stats[STAT_HEALTH] <= 0
        || cg.renderingThirdPerson) {
        return;
    }
    // one cooldown slot per action, keyed off the first two characters - cheap and collision-free
    // across the six action codes actually in use (hsoft hhard grab safe magck dry).
    // [2026-08-21] The old hash ((act[0] + act[1]*3) & 7) was NOT collision-free as its comment
    // claimed: "hhard" and "magck" both land on slot 0, so the wall-bump/sprint-start cue and the
    // idle inspect shared a cooldown AND a no-repeat index and silently suppressed each other.
    // An explicit table cannot drift the way an ad-hoc hash can.
    if      (!strcmp(act, "hsoft")) { slot = 0; }
    else if (!strcmp(act, "hhard")) { slot = 1; }
    else if (!strcmp(act, "grab"))  { slot = 2; }
    else if (!strcmp(act, "safe"))  { slot = 3; }
    else if (!strcmp(act, "magck")) { slot = 4; }
    else if (!strcmp(act, "dry"))   { slot = 5; }
    else                            { slot = 6; }
    if (cg.time < s_next[slot]) {
        return;
    }
    s_next[slot] = cg.time + cooldownMs;

    iClass = cg.snap->ps.stats[STAT_EQUIPPED_WEAPON];
    if (iClass & WEAPON_CLASS_PISTOL) {
        cls = "pistol";
    } else if (iClass & WEAPON_CLASS_SMG) {
        cls = "smg";
    } else if (iClass & (WEAPON_CLASS_MG | WEAPON_CLASS_HEAVY)) {
        cls = "mg";
    } else {
        cls = "rifle"; // also the fallback for anything untyped
    }

    s_seed ^= s_seed << 13;
    s_seed ^= s_seed >> 17;
    s_seed ^= s_seed << 5;
    idx = (int)(s_seed % 4u) + 1;
    if (idx == s_last[slot]) {
        idx = (idx % 4) + 1; // never the same take twice running
    }
    s_last[slot] = idx;

    Com_sprintf(name, sizeof(name), "coop_gf_%s_%s%02d", cls, act, idx);
    cgi.S_StartLocalSound(name, qfalse);
}

// Per-frame edge detection for the handling events whose state is reachable globally. The rest are
// hooked inline where their own state lives.
static void CoopGunFoleyThink(void)
{
    static qboolean s_init = qfalse;
    static float    s_pAds = 0.0f, s_pCrouch = 0.0f, s_pSprint = 0.0f;
    static int      s_pWpn = -2, s_pAnim = -1;
    float           ads, crouch;
    int             iWpn, iAnim;

    if (!cg.snap) {
        return;
    }
    ads    = CG_AdsPoseFactor();
    crouch = CG_AdsCrouchBlend();
    iWpn   = (cg.snap->ps.activeItems[1] >= 0) ? cg.snap->ps.activeItems[1] : -1;
    iAnim  = cg.snap->ps.iViewModelAnim;

    // seed on the first live frame so a map load, a respawn or a 3P toggle cannot fire a burst of
    // edges for transitions that happened while this was not running
    if (!s_init) {
        s_init    = qtrue;
        s_pAds    = ads;
        s_pCrouch = crouch;
        s_pSprint = s_spEnvCur;
        s_pWpn    = iWpn;
        s_pAnim   = iAnim;
        return;
    }

    if (ads > 0.5f && s_pAds <= 0.5f) {
        // [2026-08-21] "safe" takes were only ever sliced for the RIFLE class - there is no
        // coop_gf_pistol_safeNN / smg / mg. Asking for one made Alias_FindRandom return NULL and the
        // sound layer then tried to register the alias NAME as a file path: silence on ADS-in for
        // three of the four weapon classes, plus a registration warning. Route the classes that have
        // no safety take to the soft handling take, which every class does have.
        {
            int iCls = cg.snap->ps.stats[STAT_EQUIPPED_WEAPON];
            CoopGunFoley((iCls & (WEAPON_CLASS_PISTOL | WEAPON_CLASS_SMG | WEAPON_CLASS_MG
                                  | WEAPON_CLASS_HEAVY)) ? "hsoft" : "safe",
                         220);   // shouldering: a small mechanical settle
        }
    } else if (ads <= 0.5f && s_pAds > 0.5f) {
        CoopGunFoley("hsoft", 220);
    }
    s_pAds = ads;

    if (crouch > 0.5f && s_pCrouch <= 0.5f) {
        CoopGunFoley("hsoft", 260);
    } else if (crouch <= 0.5f && s_pCrouch > 0.5f) {
        CoopGunFoley("hsoft", 260);
    }
    s_pCrouch = crouch;

    // sprint: the weapon is thrown into the run carry, then settles coming out of it
    if (s_spEnvCur > 0.55f && s_pSprint <= 0.55f) {
        CoopGunFoley("hhard", 400);
    } else if (s_spEnvCur <= 0.35f && s_pSprint > 0.35f) {
        CoopGunFoley("hsoft", 400);
    }
    s_pSprint = s_spEnvCur;

    if (iWpn != s_pWpn && s_pWpn != -2) {
        CoopGunFoley("grab", 200);
    }
    s_pWpn = iWpn;

    // dry fire: the viewmodel plays its fire animation with nothing left in the clip
    if (iAnim != s_pAnim && (iAnim == VM_ANIM_FIRE || iAnim == VM_ANIM_FIRE_SECONDARY)
        && cg.snap->ps.stats[STAT_MAXCLIPAMMO] > 0 && cg.snap->ps.stats[STAT_CLIPAMMO] == 0) {
        CoopGunFoley("dry", 150);
    }
    if (iAnim != s_pAnim && iAnim == VM_ANIM_PULLOUT) {
        CoopGunFoley("grab", 200);
    }
    s_pAnim = iAnim;
}

/*
=================================================================================================
HZM coop [user 2026-08-21] LANDING SEVERITY - ONE detector, three consumers.

Before this, three separate places each re-detected a landing, on THREE DIFFERENT SIGNALS with
different gates, and could edge on different frames:

  * the weapon dip      - groundEntityNum + velZ < -180, gated on CoopWeaponFeelOn, NO reseed
  * the camera absorb   - the same expression, but additionally force-refreshing its own history
                          whenever !walking, which can destroy its edge on the touchdown frame and
                          make it miss a landing the weapon dip catches
  * the landing sound   - cg.bFPSOnGround != ps.walking, a different variable entirely, with no
                          velocity threshold at all, running even in third person and while dead

and the weapon dip carried the bug the camera's 250ms reseed was written to fix: die airborne with
s_lastVelZ ~ -700 and that value survives into the respawn, firing a full-strength slam.

Severity is now computed HERE, by whoever runs first in the frame, and latched with a timestamp;
the other two read the latch. Deliberately NOT advanced from CG_DrawActiveFrame like the ADS and
stress envelopes: those run three stages upstream of CG_PredictPlayerState, and a landing is an
EDGE, not an envelope - velocity[2] goes -700 to 0 in a single frame, so a one-frame-stale read is
1.0 versus 0.0. It also must not carry the `if (cg.frametime <= 0) return;` guard those use, because
landing on a zero-dt frame would then silently drop the whole effect on exactly the frames the
network is worst.

TIERS: 0 light / 1 medium / 2 hard. The thresholds are impact speed, not damage - a survivable drop
from a roof should still read as heavy.
=================================================================================================
*/
static float s_landSev  = 0.0f;   // 0..1, latched at touchdown
static int   s_landTime = 0;      // cg.time of that touchdown
static int   s_landTier = 0;      // 0/1/2
static int   s_landSeen = 0;      // last frame this ran, for the staleness reseed

static void CoopLandingDetect(void)
{
    static float    s_lvVelZ  = 0.0f;
    static qboolean s_lvGround = qtrue;
    static qboolean s_lvInit   = qfalse;
    qboolean bGround;
    float    vz;

    if (!cg.snap) {
        return;
    }
    if (s_landSeen == cg.time) {
        return;                       // already computed by an earlier consumer this frame
    }

    bGround = (cg.predicted_player_state.groundEntityNum != ENTITYNUM_NONE) ? qtrue : qfalse;
    vz      = cg.predicted_player_state.velocity[2];

    // STALENESS RESEED, matching the camera-motion block: these statics only advance on the live
    // first-person path, so third person, death, spectating and cutscenes freeze them. Without this
    // a player who died mid-fall returns with s_lvVelZ still at -700 and lands a phantom slam.
    if (!s_lvInit || (cg.time - s_landSeen) > 250) {
        s_lvInit   = qtrue;
        s_lvGround = bGround;
        s_lvVelZ   = vz;
        s_landSev  = 0.0f;
        s_landTier = 0;
        s_landSeen = cg.time;
        return;
    }
    s_landSeen = cg.time;

    if (bGround && !s_lvGround && s_lvVelZ < -180.0f) {
        float f = (-s_lvVelZ - 180.0f) / 520.0f;
        if (f > 1.0f) { f = 1.0f; }
        s_landSev  = f;
        s_landTime = cg.time;
        s_landTier = (f >= 0.72f) ? 2 : ((f >= 0.32f) ? 1 : 0);
    }
    s_lvGround = bGround;
    s_lvVelZ   = vz;
}

// Severity of the landing that happened within the last `windowMs`, else 0. Consumers read this
// rather than re-detecting, so all three fire on the same frame with the same number.
static float CoopLandingSeverity(int windowMs)
{
    if (!s_landTime || (cg.time - s_landTime) > windowMs) {
        return 0.0f;
    }
    return s_landSev;
}

static int CoopLandingTier(int windowMs)
{
    if (!s_landTime || (cg.time - s_landTime) > windowMs) {
        return 0;
    }
    return s_landTier;
}

// Exposed for the LANDING SOUND, which lives in cg_modelanim.c. That is the third consumer of the
// same event; it used to detect on cg.bFPSOnGround != ps.walking - a different variable, with no
// velocity threshold at all - so it could fire on a frame neither of the other two agreed with, and
// it played at a hardcoded volume 1.0 whether you stepped off a kerb or fell off a roof.
float CG_GetLandingSeverity(void)
{
    return CoopLandingSeverity(150);
}

/*
HZM coop [user 2026-08-21] VAULT ENVELOPE - one definition, two consumers (viewmodel + camera).
Edge-detects the coop_vaultView COUNTER the server stuffs on each vault. The vault itself is an
instant - a single-frame velocity assignment with no server-side duration - so the shape is invented
here and this function is the only place that knows it. Returns 0..1.
*/
static float CoopVaultEnv(void)
{
    static cvar_t *pVault = NULL, *pVaultAmt = NULL;
    static int     s_vaultSeen = -1;
    static int     s_vaultAt   = 0;
    static int     s_lastLook  = 0;
    float          fV = 0.0f, t;

    if (!pVault)    { pVault    = cgi.Cvar_Get("coop_vaultView", "0", 0); }
    if (!pVaultAmt) { pVaultAmt = cgi.Cvar_Get("coop_vaultAmount", "1.0", CVAR_ARCHIVE); }

    // STALENESS RESYNC. This function only advances on the live first-person path, so third person,
    // death, spectating and cutscenes freeze s_vaultSeen. Vault while in third person, come back, and
    // the counter has moved - which would read as an edge and fire a flourish for a vault that
    // finished seconds ago. If we have not looked recently, resync WITHOUT firing. Same discipline as
    // the camera-motion and sprint reseeds.
    if (s_lastLook && (cg.time - s_lastLook) > 250) {
        s_vaultSeen = pVault->integer;
        s_vaultAt   = 0;
    }
    s_lastLook = cg.time;

    if (pVault->integer != s_vaultSeen) {
        if (s_vaultSeen >= 0) {
            s_vaultAt = cg.time;   // a real edge, not the first frame we looked at the cvar
        }
        s_vaultSeen = pVault->integer;
    }
    if (!s_vaultAt) {
        return 0.0f;
    }
    if (cg.time - s_vaultAt >= COOP_VAULT_MS) {
        s_vaultAt = 0;
        return 0.0f;
    }
    // fast down over the first 30%, then a slower eased return. Asymmetric on purpose: equal in and
    // out reads as a bob rather than as letting go of the gun and re-gripping it.
    t = (float)(cg.time - s_vaultAt) / (float)COOP_VAULT_MS;
    if (t < 0.30f) {
        fV = t / 0.30f;
    } else {
        fV = 1.0f - ((t - 0.30f) / 0.70f);
        fV *= fV;
    }
    return fV * pVaultAmt->value;
}

void CG_AddSuppression(float amount)
{
    if (amount <= 0.0f) {
        return;
    }
    s_coopSuppress += amount;
    if (s_coopSuppress > 1.0f) {
        s_coopSuppress = 1.0f;
    }
    CG_HudFadeTouch(); // HZM coop - under fire: bring the HUD chrome back
}

// HZM coop - HEAT HAZE intensity 0..1. Bumped by CG_AddHeat (nearby explosions, from cg_parsemsg.cpp),
// decays each frame, published to the renderer as r_ppHeat. Same pattern as suppression.
static float    s_coopHeat = 0.0f;

void CG_AddHeat(float amount)
{
    if (amount <= 0.0f) {
        return;
    }
    s_coopHeat += amount;
    if (s_coopHeat > 1.0f) {
        s_coopHeat = 1.0f;
    }
}

// HZM coop - LOCALIZED MUZZLE HEAT 0..1. A SEPARATE channel from s_coopHeat: gunfire bumps THIS (via
// CG_AddMuzzleHeat from cg_parsemsg.cpp), and it's published as r_ppMuzzleHeat to drive the tight,
// gun-anchored heat shimmer in the renderer (NOT the fullscreen explosion warp). Decays on the same fade.
static float s_coopMuzzleHeat = 0.0f;

void CG_AddMuzzleHeat(float amount)
{
    if (amount <= 0.0f) {
        return;
    }
    // HZM coop - FLOOR, not accumulate: each shot brings the muzzle heat UP TO 'amount' (then it decays),
    // so EVERY gun shows the same per-shot heat regardless of fire rate. The old additive version stacked
    // fast guns (Thompson) up to max while slow guns (handgun/BAR) barely registered. coop_heatGun = the
    // shared per-shot level for all guns.
    if (amount > s_coopMuzzleHeat) {
        s_coopMuzzleHeat = amount;
    }
    if (s_coopMuzzleHeat > 1.0f) {
        s_coopMuzzleHeat = 1.0f;
    }
}

// HZM coop - TRANSIENT DYNAMIC LIGHTS (muzzle flashes + explosions). A small ring buffer of short-lived
// omni dlights; CG_AddCoopDynamicLight pushes one (origin/color/radius/life-ms), CG_AddCoopDynamicLights is
// called once per frame during the scene build and re-adds each still-alive light via cgi.R_AddLightToScene
// with a linear fade. Lets gunfire + blasts actually light the world. Gated by coop_dynLights.
#define COOP_MAX_DLIGHTS 48
typedef struct { vec3_t org; float r, g, b; float radius; int start; int life; } coopDLight_t;
static coopDLight_t s_coopDLights[COOP_MAX_DLIGHTS];
static int          s_coopDLightHead = 0;

void CG_AddCoopDynamicLight(const vec3_t org, float r, float g, float b, float radius, int life_ms)
{
    coopDLight_t *dl;
    if (life_ms <= 0 || radius <= 0.0f) {
        return;
    }
    dl = &s_coopDLights[s_coopDLightHead % COOP_MAX_DLIGHTS];
    s_coopDLightHead++;
    VectorCopy(org, dl->org);
    dl->r      = r;
    dl->g      = g;
    dl->b      = b;
    dl->radius = radius;
    dl->start  = cg.time;
    dl->life   = life_ms;
}

void CG_AddCoopDynamicLights(void)
{
    cvar_t *pOn = cgi.Cvar_Get("coop_dynLights", "1", CVAR_ARCHIVE);
    int     i;

    if (!pOn || !pOn->integer) {
        return;
    }
    for (i = 0; i < COOP_MAX_DLIGHTS; i++) {
        coopDLight_t *dl = &s_coopDLights[i];
        int           age;
        float         f;

        if (dl->life <= 0) {
            continue;
        }
        age = cg.time - dl->start;
        if (age < 0 || age >= dl->life) {
            dl->life = 0; // expired
            continue;
        }
        f = 1.0f - ((float)age / (float)dl->life); // linear fade out
        cgi.R_AddLightToScene(dl->org, dl->radius * f, dl->r * f, dl->g * f, dl->b * f, 0);
    }
}

// HZM coop - ENVIRONMENT REVERB: where the MAP authors no reverb, auto-pick one from the surroundings -
// an up-trace tells outdoor (open sky -> dry/open) vs indoor (ceiling -> room/hall by height). Only runs
// when coop_autoReverb is on AND the map isn't driving reverb (cg_snapshot yields to us then). Applies via
// cgi.S_SetReverb only on a state CHANGE so the EFX isn't re-triggered every frame. Needs s_reverb 1.
void CG_UpdateEnvReverb(void)
{
    static int s_lastEnvPreset = -1;
    static int s_lastEnvTime   = 0;
    cvar_t    *pAR = cgi.Cvar_Get("coop_autoReverb", "1", CVAR_ARCHIVE);
    vec3_t     zero = {0.0f, 0.0f, 0.0f};
    vec3_t     start, end;
    trace_t    tr;
    int        preset;
    float      level;

    if (!pAR || !pAR->integer || !cg.snap) {
        return;
    }
    if (cg.snap->ps.reverb_type != eax_generic) { // a trigger_reverb / soundman zone owns it - yield
        s_lastEnvPreset = -1;
        return;
    }
    if (cg.time - s_lastEnvTime < 250) { // throttle the trace to ~4/sec
        return;
    }
    s_lastEnvTime = cg.time;

    VectorCopy(cg.refdef.vieworg, start);
    end[0] = start[0];
    end[1] = start[1];
    end[2] = start[2] + 2048.0f;
    cgi.CM_BoxTrace(&tr, start, end, zero, zero, 0, MASK_SOLID, qfalse);

    if ((tr.surfaceFlags & SURF_SKY) || tr.fraction >= 0.999f) {
        preset = eax_generic; level = 0.0f;                                   // open sky / no ceiling -> dry
    } else {
        float ceilDist = tr.fraction * 2048.0f;
        // HZM coop - retuned 2026-07-04: 'room' (0.4s decay) was inaudible under gunfire and
        // most MOHAA interiors have <256u ceilings, so everything indoor sounded dry. Hard
        // WWII masonry reads as stoneroom (2.3s decay); genuinely big volumes get auditorium.
        if (ceilDist < 256.0f)      { preset = eax_stoneroom;  level = 0.38f; } // low/small room
        else if (ceilDist < 640.0f) { preset = eax_stoneroom;  level = 0.55f; } // medium room
        else                        { preset = eax_auditorium; level = 0.50f; } // tall / large space
    }

    if (preset != s_lastEnvPreset) {
        cgi.S_SetReverb(preset, level);
        s_lastEnvPreset = preset;
    }
}

// Returns the current free-aim offset (degrees) the crosshair/weapon should follow; qtrue if non-zero.
qboolean CG_GetFreeAim(float *outYaw, float *outPitch)
{
    if (outYaw)   { *outYaw   = s_faYaw; }
    if (outPitch) { *outPitch = s_faPitch; }
    return (s_faYaw != 0.0f || s_faPitch != 0.0f) ? qtrue : qfalse;
}

// Returns breath info for the HUD. outFrac: 0..1 (breath remaining, or recharge progress while cooling
// down). outCooldown: true while recharging.
// [vet 2026-08-28] Forward declaration. The definition is at the bottom of this file but the first
// call site is here, so MSVC fell back to an implicit `extern int` (C4013) while the real symbol is
// static - it linked by luck, with the compiler guessing at a signature it could not see.
static int CG_BreathHoldMs(void);

qboolean CG_GetBreathState(float *outFrac, qboolean *outCooldown)
{
    cvar_t *pHold   = cgi.Cvar_Get("cg_breathHoldTime", "7", CVAR_ARCHIVE);
    int     iHoldMs = CG_BreathHoldMs();

    if (iHoldMs < 100) {
        iHoldMs = 100;
    }
    if (outCooldown) {
        *outCooldown = (s_breathCooldownEnd != 0) ? qtrue : qfalse;
    }
    if (outFrac) {
        if (s_breathCooldownEnd != 0) {
            cvar_t *pCool   = cgi.Cvar_Get("cg_breathCooldown", "5", CVAR_ARCHIVE);
            int     iCoolMs = (int)((pCool ? pCool->value : 5.0f) * 1000.0f);
            if (iCoolMs < 100) {
                iCoolMs = 100;
            }
            *outFrac = 1.0f - ((float)(s_breathCooldownEnd - cg.time) / (float)iCoolMs);
        } else {
            *outFrac = (s_breathRemainMs < 0) ? 1.0f : ((float)s_breathRemainMs / (float)iHoldMs);
        }
        if (*outFrac < 0.0f) { *outFrac = 0.0f; }
        if (*outFrac > 1.0f) { *outFrac = 1.0f; }
    }
    return qtrue;
}

// HZM coop - true only while the player is ACTIVELY holding breath to steady the sights this frame (drives
// the extra "focus-in" ADS vignette darkening in cg_drawtools).
qboolean CG_IsBreathSteady(void)
{
    return s_breathSteady;
}

/*
===============
CG_OffsetFirstPersonView

===============
*/
void CG_OffsetFirstPersonView(refEntity_t *pREnt, qboolean bUseWorldPosition)
{
    // HZM coop [user 2026-08-02] bug-1291 - eased 0..1 low-health limp envelope, driven by the
    // server-stuffed coop_limpView. Static so it survives between frames; file-local so nothing
    // else can drive the camera limp behind the server's back.
    static float s_limpEnv = 0.0f;
    float     *origin;
    centity_t *pCent;
    dtiki_t   *tiki;
    int        iTag;
    int        i;
    int        iMask;
    vec3_t     vDelta;
    float      mat[3][3];
    vec3_t     vOldOrigin;
    vec3_t     vStart, vEnd, vMins, vMaxs;
    vec3_t     vVelocity;
    trace_t    trace;

    VectorSet(vMins, -6, -6, -6);
    VectorSet(vMaxs, 6, 6, 6);

    //
    //
    //
    //
    origin = cg.refdef.vieworg;

    pCent = &cg_entities[cg.predicted_player_state.clientNum];

    tiki = cgi.R_Model_GetHandle(cgs.model_draw[pCent->currentState.modelindex]);
    iTag = cgi.Tag_NumForName(tiki, "eyes bone");
    if (iTag != -1) {
        if (bUseWorldPosition) {
            orientation_t oHead;
            float         mat3[3][3];
            vec3_t        vHeadAng, vDelta;

            VectorCopy(pCent->lerpOrigin, origin);
            AxisCopy(pREnt->axis, mat);
            oHead = cgi.TIKI_Orientation(pREnt, iTag);

            for (i = 0; i < 3; i++) {
                VectorMA(origin, oHead.origin[i], mat[i], origin);
            }

            R_ConcatRotations(oHead.axis, mat, mat3);
            MatrixToEulerAngles(mat3, vHeadAng);
            AnglesSubtract(vHeadAng, cg.refdefViewAngles, vDelta);
            VectorMA(cg.refdefViewAngles, cg.fEyeOffsetFrac, vDelta, cg.refdefViewAngles);
            VectorCopy(vHeadAng, cg.refdefViewAngles);
        } else {
            orientation_t oHead;
            vec3_t        vHeadAng;

            VectorCopy(pCent->lerpOrigin, origin);
            AxisCopy(pREnt->axis, mat);
            MatrixToEulerAngles(mat, vHeadAng);
            oHead = cgi.TIKI_Orientation(pREnt, iTag);

            for (i = 0; i < 3; i++) {
                VectorMA(origin, oHead.origin[i], mat[i], origin);
            }

            {
                // Changed in 2.0 - slightly less angle
                float leanRoll = (cg_target_game >= TG_MOHTA) ? 0.2f : 0.3f;
                // HZM coop - while ADS, damp the lean view-roll so the iron sights stay aligned instead of
                // tilting off-screen (lean+ADS stays usable). cg_adsLeanRoll: 1 = full lean tilt, 0 = none
                // while aiming (sights dead level). Live-tunable.
                // [user 2026-08-20] "stay in the same exact ads from a camera/gun standpoint but
                // just lean left or right". The registered default was "1.0" - FULL lean tilt
                // while aiming - even though the null-fallback beside it says 0.25, which is what
                // the author intended. The damper has therefore been inert since it was written.
                // Rolling the view rolls the world under a level sight picture, so it is the part
                // that makes leaning unusable with sights; the LATERAL peek (the eye pivot below,
                // cg_adsLeanShift) is the part worth keeping and is left at full strength.
                if (CG_AimingDownSights()) {
                    cvar_t *pALR = cgi.Cvar_Get("cg_adsLeanRoll", "0", CVAR_ARCHIVE);
                    leanRoll *= (pALR ? pALR->value : 0.0f);
                }
                cg.refdefViewAngles[2] += cg.predicted_player_state.fLeanAngle * leanRoll;
            }
        }
    } else {
        cgi.DPrintf("CG_OffsetFirstPersonView warning: Couldn't find 'eyes bone' for player\n");
    }

    VectorCopy(origin, vOldOrigin);

    if (!cg.predicted_player_state.walking || (!(cg.predicted_player_state.pm_flags & PMF_FROZEN) && !(cg.predicted_player_state.pm_flags & PMF_NO_MOVE))) {
        VectorCopy(cg.predicted_player_state.velocity, vVelocity);
    } else {
        //
        // Added in OPM
        //  When frozen, there must be no movement at all
        VectorClear(vVelocity);
    }

    if (bUseWorldPosition) {
        iMask = MASK_VIEWSOLID;
    } else {
        float  fTargHeight;
        float  fHeightDelta, fHeightChange;
        float  fPhase, fVel;
        vec3_t vDelta;
        vec3_t vPivotPoint;
        vec3_t vForward, vLeft;

        origin[0]    = cg.predicted_player_state.origin[0];
        origin[1]    = cg.predicted_player_state.origin[1];
        fTargHeight  = cg.predicted_player_state.origin[2] + cg.predicted_player_state.viewheight;
        fHeightDelta = fTargHeight - cg.fCurrentViewHeight;

        if (fabs(fHeightDelta) < 0.1 || !cg.fCurrentViewHeight) {
            cg.fCurrentViewHeight = fTargHeight;
        } else {
            if (fHeightDelta > 32.f) {
                fHeightDelta          = 32.f;
                cg.fCurrentViewHeight = fTargHeight - 32.0;
            } else if (fHeightDelta < -32.f) {
                fHeightDelta          = -32.f;
                cg.fCurrentViewHeight = fTargHeight + 32.0;
            }

            fHeightChange = cg.frametime / 1000.0 * fHeightDelta * 12.5;
            if (!cg.predicted_player_state.walking) {
                fHeightChange += fHeightChange;
            }

            if (fabs(fHeightDelta) < fabs(fHeightChange)) {
                fHeightChange = fHeightDelta;
            }

            cg.fCurrentViewHeight += fHeightChange;
        }

        origin[2]      = cg.fCurrentViewHeight;
        vPivotPoint[0] = cg.refdefViewAngles[0];
        vPivotPoint[1] = cg.refdefViewAngles[1];
        vPivotPoint[2] = 0.0;
        AngleVectorsLeft(vPivotPoint, vForward, vLeft, NULL);

        VectorCopy(origin, vStart);

        if (cg.predicted_player_state.pm_type != PM_CLIMBWALL) {
            if (cg.refdefViewAngles[0] > 0.0) {
                vStart[2] -= (cg.fCurrentViewHeight - cg.predicted_player_state.origin[2]) * 0.4;
            } else {
                vStart[2] -= (cg.fCurrentViewHeight - cg.predicted_player_state.origin[2]) * 0.2;
            }
        } else {
            vStart[2] -= (cg.fCurrentViewHeight - cg.predicted_player_state.origin[2]) * 0.15;
        }

        VectorSubtract(origin, vStart, vDelta);
        RotatePointAroundVector(vEnd, vLeft, vDelta, cg.refdefViewAngles[0] * 0.4);
        VectorAdd(vStart, vEnd, origin);

        if (cg.predicted_player_state.fLeanAngle) {
            // HZM coop - this is the LATERAL lean eye-shift (pivots the eye sideways). While ADS it slides
            // the iron-sight picture off-screen, so damp it by cg_adsLeanShift (0 = sight stays centred, you
            // still peek the world via the small roll; 1 = full lean shift). Live-tunable. Aim stays usable.
            float fLeanAmt = cg.predicted_player_state.fLeanAngle;
            if (CG_AimingDownSights()) {
                cvar_t *pALS = cgi.Cvar_Get("cg_adsLeanShift", "1.0", CVAR_ARCHIVE);
                fLeanAmt *= (pALS ? pALS->value : 0.2f);
            }
            VectorCopy(origin, vStart);
            vStart[2] -= 28.7f;

            VectorSubtract(origin, vStart, vDelta);
            RotatePointAroundVector(vEnd, vForward, vDelta, fLeanAmt);
            VectorAdd(vStart, vEnd, origin);
        }

        // HZM coop: do NOT apply walking view-bob to a GLUED rider (vehicle passenger). A glued player has
        // PMF_NO_MOVE and inherits the vehicle's velocity, so the stock bob (amplitude = speed) would bob the
        // camera at ~vehicle speed while "walking" (groundEntityNum = the vehicle). The bob is added to the
        // view AFTER the origin, so the smooth-and-locked truck appears to hop against the bobbing camera -
        // this was the "truck steps forward in little jumps" stutter (noclip cured it by clearing walking).
        // Riders can't self-move, so no footstep bob belongs here; let it decay to zero (else branch).
        // HZM coop [user 2026-08-02] bug-1291 - LOW-HEALTH LIMP, first-person half.
        // "you should see the limp in first person (camera should imitate that as you move)".
        // Driven ENTIRELY by coop_limpView, which the SERVER stuffs to the owning client on change
        // (Player::TickLimp). The client deliberately does NOT re-derive a health threshold: if it
        // did, a server admin setting coop_limp 0 would stop the body limping while every remote
        // camera kept limping on its own compiled-in default.
        // Eased rather than switched so the gait arrives as a consequence, not a mode flip.
        {
            static cvar_t *pLimpView = NULL;
            static cvar_t *pLimpSpd  = NULL;
            float          tgt, dt;
            if (!pLimpView) { pLimpView = cgi.Cvar_Get("coop_limpView", "0", 0); }
            if (!pLimpSpd)  { pLimpSpd  = cgi.Cvar_Get("cg_limpCamSpeed", "4", CVAR_ARCHIVE); }
            tgt = (pLimpView && pLimpView->integer) ? 1.0f : 0.0f;
            dt  = cg.frametime / 1000.0f;
            if (dt < 0.0f) { dt = 0.0f; } else if (dt > 0.25f) { dt = 0.25f; }
            s_limpEnv += (tgt - s_limpEnv) * dt * (pLimpSpd ? pLimpSpd->value : 4.0f);
            if (s_limpEnv < 0.0f) { s_limpEnv = 0.0f; } else if (s_limpEnv > 1.0f) { s_limpEnv = 1.0f; }
        }

        if (cg.predicted_player_state.walking && !(cg.predicted_player_state.pm_flags & PMF_NO_MOVE)) {
            fVel   = VectorLength(vVelocity);
            fPhase = fVel * 0.0015 + 0.9;
            // LIMP DRAG: the bad foot's half-cycle takes longer, the good foot's is quicker, so the
            // step TIMING is uneven and not just the depth. sin(phase - 0.94) is the foot-parity
            // signal (see the vertical dip below). Clamped well above zero so the phase can never
            // stall or run backwards, which would read as a freeze rather than a limp.
            if (s_limpEnv > 0.0f) {
                float fDrag = cgi.Cvar_Get("cg_limpDrag", "0.25", CVAR_ARCHIVE)->value;
                float fMul  = 1.0f - s_limpEnv * fDrag * (float)sin(cg.fCurrentViewBobPhase - 0.94);
                if (fMul < 0.35f) { fMul = 0.35f; } else if (fMul > 1.65f) { fMul = 1.65f; }
                fPhase *= fMul;
            }
            cg.fCurrentViewBobPhase += (cg.frametime / 1000.0 + cg.frametime / 1000.0) * M_PI * fPhase;

            if (cg.fCurrentViewBobAmp) {
                cg.fCurrentViewBobAmp = fVel;
            } else {
                cg.fCurrentViewBobAmp = fVel * 0.5;
            }

            if (cg.predicted_player_state.fLeanAngle != 0.0) {
                cg.fCurrentViewBobAmp *= 0.75;
            }

            cg.fCurrentViewBobAmp *= (1.0 - fabs(cg.refdefViewAngles[0]) * (1.0 / 90.0) * 0.5) * 0.5;

            // HZM coop - HEAD BOB shaping (user request: present in moderation, stronger while
            // sprinting). Scales the stock bob amplitude: cg_headbobScale is the overall feel,
            // speed beyond the base run speed (sprint) swells it further, and aiming down
            // sights / native scopes damp it hard so the sight picture stays usable.
            // cg_headbob 0 restores the untouched vanilla bob.
            if (cgi.Cvar_Get("cg_headbob", "1", CVAR_ARCHIVE)->integer) {
                float fBobScale = cgi.Cvar_Get("cg_headbobScale", "1.35", CVAR_ARCHIVE)->value;

                if (fVel > 300.0f) {
                    // base run tops out ~287 (sv_runspeed); anything past this is sprint
                    float fSprintFrac = (fVel - 300.0f) / 80.0f;
                    if (fSprintFrac > 1.0f) {
                        fSprintFrac = 1.0f;
                    }
                    fBobScale *= 1.0f + 0.85f * fSprintFrac;
                }

                if (CG_AimingDownSights() || cg.snap->ps.stats[STAT_INZOOM]) {
                    fBobScale *= 0.25f;
                }

                cg.fCurrentViewBobAmp *= fBobScale;
            }
        } else if (cg.fCurrentViewBobAmp > 0.0) {
            cg.fCurrentViewBobAmp -=
                (cg.frametime / 1000.0 * cg.fCurrentViewBobAmp) + (cg.frametime / 1000.0 * cg.fCurrentViewBobAmp);

            if (cg.fCurrentViewBobAmp < 0.1) {
                cg.fCurrentViewBobAmp = 0.0;
            }
        }

        if (cg.fCurrentViewBobAmp > 0.0) {
            fPhase = sin(cg.fCurrentViewBobPhase) * cg.fCurrentViewBobAmp * 0.03;

            if (fPhase > 16.0) {
                fPhase = 16.0;
            } else if (fPhase < -16.0) {
                fPhase = -16.0;
            }

            VectorMA(origin, fPhase, vLeft, origin);

            // Because of the fabs(), this vertical term has period PI - one lobe per FOOTSTEP - so
            // the SIGN of sin(phase - 0.94) is foot parity. That makes the limp a continuous
            // modulation of the existing dip rather than a new oscillator: one foot's dip goes deep,
            // the other's stays shallow. Deliberately NOT floor(phase/PI)%2 parity, which steps
            // discontinuously and pops. Placed BEFORE the MASK_PLAYERSOLID traces below, so a deep
            // dip can never punch the eye through a floor or a waterline.
            {
                float fFootSign = (float)sin(cg.fCurrentViewBobPhase - 0.94);

                fPhase = (fabs(fFootSign) - 0.5) * cg.fCurrentViewBobAmp * 0.06;

                if (s_limpEnv > 0.0f) {
                    float fDepth = cgi.Cvar_Get("cg_limpDepth", "0.55", CVAR_ARCHIVE)->value;
                    fPhase *= 1.0f + s_limpEnv * fDepth * fFootSign;
                }
            }

            if (fPhase > 16.0) {
                fPhase = 16.0;
            } else if (fPhase < -16.0) {
                fPhase = -16.0;
            }

            origin[2] += fPhase;

            // LIMP ROLL: the favoured-leg lean. Scaled by the bob amplitude it belongs to, so it
            // fades out with the bob instead of snapping off at full strength the frame the movement
            // key is released, and damped under sights by the same convention the head-bob shaper
            // uses - the sight picture must stay usable in the state the player most needs to aim.
            if (s_limpEnv > 0.0f) {
                float fRoll  = cgi.Cvar_Get("cg_limpRoll", "1.2", CVAR_ARCHIVE)->value;
                float fScale = cg.fCurrentViewBobAmp / 287.0f; // sv_runspeed-ish reference
                if (fScale > 1.0f) { fScale = 1.0f; }
                if (CG_AimingDownSights() || cg.snap->ps.stats[STAT_INZOOM]) {
                    fRoll *= cgi.Cvar_Get("cg_limpRollAds", "0.25", CVAR_ARCHIVE)->value;
                }
                cg.refdefViewAngles[2] +=
                    s_limpEnv * fRoll * fScale * (float)sin(cg.fCurrentViewBobPhase - 0.94);
            }
        }

        iMask = MASK_PLAYERSOLID;
    }

    vStart[0] = cg.predicted_player_state.origin[0];
    vStart[1] = cg.predicted_player_state.origin[1];
    vStart[2] = cg.predicted_player_state.origin[2] + cg.predicted_player_state.viewheight;
    vEnd[0]   = cg.predicted_player_state.origin[0];
    vEnd[1]   = cg.predicted_player_state.origin[1];
    vEnd[2]   = origin[2];

    CG_Trace(
        &trace, vStart, vMins, vMaxs, vEnd, cg.snap->ps.clientNum, iMask, qfalse, qtrue, "FirstPerson Height Check"
    );

    VectorCopy(trace.endpos, vStart);
    vEnd[0] = origin[0];
    vEnd[1] = origin[1];
    vEnd[2] = trace.endpos[2];
    CG_Trace(&trace, vStart, vMins, vMaxs, vEnd, cg.snap->ps.clientNum, iMask, 0, 1, "FirstPerson Lateral Check");

    VectorCopy(trace.endpos, origin);
    VectorSubtract(origin, vOldOrigin, vDelta);

    // HZM coop [user 2026-08-21] WEAPON MASS: let the gun TRAIL the camera instead of being welded
    // to it. This line used to hand the viewmodel 100% of the camera excursion every frame - head
    // bob, lean, the collision traces, everything - so the gun was rigidly locked to the eye and
    // could not read as having any weight of its own.
    //
    // The obvious idea (add a bigger, phase-lagged bob to the viewmodel) is a NO-OP, and it is worth
    // recording why so it is not tried again: the camera bob and the viewmodel's own sway are both
    // sin() of the SAME variable, cg.fCurrentViewBobPhase, so they are the same frequency by
    // construction. Two co-frequency sinusoids sum to a single sinusoid at that frequency - changing
    // the lag rotates the resultant and changes nothing about how correlated the two are. With the
    // amplitudes as shipped (0.03 vs 0.005, a 6:1 ratio) the existing M_PI/10 lag moves the result by
    // about 2.5 degrees. Invisible.
    //
    // The only lever that actually decouples them is this transfer. Handing over a FRACTION and
    // feeding the remainder through a one-pole lag makes the weapon's response depend on both the
    // amplitude and the FREQUENCY of the camera move: slow moves still carry the gun almost exactly,
    // while fast ones (bob, a flinch, a hard landing) leave it behind for a beat and let it catch up.
    // That frequency dependence is what mass looks like.
    //
    // Applied BEFORE the feel-budget base capture below, so it costs nothing from the 9u/4u jitter
    // ceiling and cannot starve the landing dip or weapon lag. Full transfer at coop_weaponMass 0
    // reproduces the old behaviour exactly.
    {
        static cvar_t *pMass = NULL, *pMassRate = NULL;
        static vec3_t  s_vMassLag = {0, 0, 0};
        float          fMass, fDtM, kM;

        if (!pMass)     { pMass     = cgi.Cvar_Get("coop_weaponMass",     "0.35", CVAR_ARCHIVE); }
        if (!pMassRate) { pMassRate = cgi.Cvar_Get("coop_weaponMassRate", "14.0", CVAR_ARCHIVE); }

        fMass = pMass->value;
        if (fMass < 0.0f) { fMass = 0.0f; }
        if (fMass > 0.9f) { fMass = 0.9f; }   // never fully decouple: the gun must not swim

        // [user 2026-08-21] THIS WAS THE "SHOULDER POP". The user identified it by bisection after
        // five of my theories failed - camera motion, the 3P flip, the min-distance fallback, the
        // animation root and the surface pop were all measured out, and the cause was a filter I had
        // added BETWEEN those things, which is why probing either side never saw it.
        //
        // vDelta is NOT a per-frame motion. It is the TOTAL camera offset for this frame (bob, lean,
        // collision traces) - and that total includes the ADS pose change. So low-passing it made the
        // arms trail the aim transition by roughly 110ms and lurch as they caught up: the shoulders
        // moving while the camera holds still, exactly as reported.
        //
        // Scale the effect out with the ADS pose. Mass is a hip-fire and movement quality - it is
        // what makes a carried weapon feel heavy - but while aiming the weapon must track the view
        // exactly or the sight picture swims. Using the eased pose factor means there is no step at
        // either end: full mass at the hip, none at the sights, and a smooth ramp between.
        fMass *= (1.0f - CG_AdsPoseFactor());
        if (fMass < 0.0f) { fMass = 0.0f; }

        // dt clamped like every other integrator in this file - cg.frametime is unbounded for a
        // client of a remote server, and an unclamped k would overshoot the target on a hitch.
        fDtM = cg.frametime / 1000.0f;
        if (fDtM > 0.1f) { fDtM = 0.1f; }
        kM = fDtM * pMassRate->value;
        if (kM > 1.0f) { kM = 1.0f; }

        // [user 2026-08-21] "spawned in and my gun is violently shaking nonstop" - the first version
        // of this shipped an OSCILLATOR. It did:
        //     s_vMassLag += fMass * vDelta;   then bled s_vMassLag back into vDelta
        // which is precisely the pattern TRAPS bans after bug-1984: writing a PERIODIC term into a
        // state variable that an exponential ease then tracks. vDelta carries the head bob, so the
        // accumulator was fed a sine every frame and pumped. It was also dimensionally wrong -
        // vDelta is a POSITION offset (origin - vOldOrigin, both from this frame), not a per-frame
        // increment, so accumulating it had no meaning in the first place.
        //
        // The correct filter TRACKS the target instead of accumulating it. s_vMassLag is a plain
        // one-pole low-pass of vDelta, so it is bounded by vDelta's own range and cannot pump; the
        // weapon then gets a blend of the instantaneous offset and the lagged one. fMass 0 reproduces
        // the old rigid transfer exactly, fMass 1 would be fully lagged.
        if (fMass > 0.001f && cg.frametime > 0) {
            vec3_t vBlend;

            s_vMassLag[0] += (vDelta[0] - s_vMassLag[0]) * kM;
            s_vMassLag[1] += (vDelta[1] - s_vMassLag[1]) * kM;
            s_vMassLag[2] += (vDelta[2] - s_vMassLag[2]) * kM;

            vBlend[0] = vDelta[0] * (1.0f - fMass) + s_vMassLag[0] * fMass;
            vBlend[1] = vDelta[1] * (1.0f - fMass) + s_vMassLag[1] * fMass;
            vBlend[2] = vDelta[2] * (1.0f - fMass) + s_vMassLag[2] * fMass;
            VectorCopy(vBlend, vDelta);
        } else {
            VectorCopy(vDelta, s_vMassLag);   // stay tracked so re-enabling cannot step
        }
    }

    VectorAdd(pREnt->origin, vDelta, pREnt->origin);

    if (!bUseWorldPosition) {
        VectorCopy(cg.refdefViewAngles, vDelta);
        vDelta[0] *= 0.5;
        vDelta[2] *= 0.75;

        AngleVectorsLeft(vDelta, mat[0], mat[1], mat[2]);

        CG_CalcViewModelMovement(
            cg.fCurrentViewBobPhase, cg.fCurrentViewBobAmp, vVelocity, vDelta
        );

        VectorMA(pREnt->origin, vDelta[0], mat[0], pREnt->origin);
        VectorMA(pREnt->origin, vDelta[1], mat[1], pREnt->origin);
        VectorMA(pREnt->origin, vDelta[2], mat[2], pREnt->origin);

        // [2026-08-21] THE FEEL BUDGET, part 1 of 2: remember where the weapon sits before any
        // of the feel layers touch it. Everything from here to the clamp below adds to this
        // position, and NOTHING bounded the total - each layer clamped only itself, which is
        // precisely how a sum stays "within budget" while being ten times the budget.
        VectorCopy(pREnt->origin, s_vFeelBase);
        VectorClear(s_vFeelExempt);
        s_bFeelBase = qtrue;
        s_fRollBase = cg.refdefViewAngles[2];
        s_bRollBase = qtrue;

        // HZM coop - ADS SWAY + RECOIL. Both are applied to the view weapon (hands + gun) ONLY - they move
        // the weapon model in view space, NOT the actual aim/bullet direction, so they're immersion-only and
        // never fight the player's input. mat[0]=forward, mat[1]=left, mat[2]=up.
        {
            static float s_recoil   = 0.0f;  // current recoil displacement (units), decays to 0
            static int   s_lastClip = -1;    // STAT_CLIPAMMO last frame (to detect a shot)
            static int   s_lastWpn  = -2;    // active weapon last frame (ignore reload/switch ammo jumps)
            cvar_t      *pSway      = cgi.Cvar_Get("cg_adsSway", "0.4", CVAR_ARCHIVE);
            cvar_t      *pSwaySpd   = cgi.Cvar_Get("cg_adsSwaySpeed", "1.0", CVAR_ARCHIVE);
            cvar_t      *pRecoil    = cgi.Cvar_Get("cg_adsRecoil", "0.5", CVAR_ARCHIVE);
            qboolean     bAds       = CG_AimingDownSights();
            int          iClip      = cg.snap ? cg.snap->ps.stats[STAT_CLIPAMMO] : 0;
            int          iWpn       = (cg.snap && cg.snap->ps.activeItems[1] >= 0) ? cg.snap->ps.activeItems[1] : -1;
            int          iClass     = cg.snap ? cg.snap->ps.stats[STAT_EQUIPPED_WEAPON] : 0;
            // scoped sniper (native zoom): the gun model is hidden by the scope, so breath + sway act on the
            // VIEW instead of the gun model. Restricted to rifles so binoculars don't trigger it.
            qboolean     bScoped    = (cg.snap && cg.snap->ps.stats[STAT_HEALTH] > 0
                                       && cg.snap->ps.stats[STAT_INZOOM]
                                       && !(cg.snap->ps.pm_flags & PMF_CAMERA_VIEW)
                                       && (iClass & WEAPON_CLASS_RIFLE)) ? qtrue : qfalse;
            float        fClassKick = 1.0f;
            // [user 2026-08-27] BRACED: a supported gun barely wanders and barely climbs. Sway takes
            // the deepest cut because its dying is the strongest kinesthetic tell in every game that
            // ships this - you FEEL the support before you read any icon.
            float        fBrace     = CG_CoopBrace();
            float        fBrSway    = 1.0f, fBrShove = 1.0f;
            if (fBrace > 0.0f) {
                cvar_t *pBrSway  = cgi.Cvar_Get("coop_braceSway", "0.10", CVAR_ARCHIVE);
                cvar_t *pBrShove = cgi.Cvar_Get("coop_braceShove", "0.9", CVAR_ARCHIVE);
                fBrSway  = 1.0f - fBrace * (1.0f - (pBrSway  ? pBrSway->value  : 0.10f));
                fBrShove = 1.0f - fBrace * (1.0f - (pBrShove ? pBrShove->value : 0.9f));
            }


            // per-class recoil feel: bigger guns climb harder
            if (iClass & WEAPON_CLASS_PISTOL)      { fClassKick = 0.6f; }
            else if (iClass & WEAPON_CLASS_SMG)    { fClassKick = 0.85f; }
            else if (iClass & WEAPON_CLASS_RIFLE)  { fClassKick = 1.35f; }
            else if (iClass & WEAPON_CLASS_MG)     { fClassKick = 1.5f; }
            else if (iClass & WEAPON_CLASS_HEAVY)  { fClassKick = 1.3f; }
            // [bug-2133] applied AFTER the class chain - it used to sit before it and was simply
            // overwritten, so the brace never damped the visible punch for any real firearm.
            // [user 2026-08-27] per-gun weight on top of the per-class constant, from the same
            // authored table - so two rifles in the same class no longer punch identically.
            {
                static cvar_t *pHW = NULL;
                if (!pHW) { pHW = cgi.Cvar_Get("coop_kickHeft", "0.55", CVAR_ARCHIVE); }
                fClassKick *= 1.0f + CoopGunHeft() * pHW->value;
            }
            fClassKick *= fBrShove;

            // HOLD BREATH (steady aim): while ADS, holding the run/walk key (Shift / BUTTON_RUN) suppresses
            // the sway for up to cg_breathHoldTime sec, then a cg_breathCooldown-sec recharge. Shift still
            // walks normally when NOT aiming (we only READ the button). Timestamps use cg.time so the
            // recharge elapses by wall-clock even if this isn't called every frame.
            {
                cvar_t   *pHold   = cgi.Cvar_Get("cg_breathHoldTime", "7", CVAR_ARCHIVE);
                cvar_t   *pCool   = cgi.Cvar_Get("cg_breathCooldown", "5", CVAR_ARCHIVE);
                int       iHoldMs = CG_BreathHoldMs();
                int       iCoolMs = (int)((pCool ? pCool->value : 5.0f) * 1000.0f);
                usercmd_t bcmd;
                qboolean  bShift;
                int       dt;

                if (iHoldMs < 100) { iHoldMs = 100; }
                if (iCoolMs < 100) { iCoolMs = 100; }
                if (s_breathRemainMs < 0) { s_breathRemainMs = iHoldMs; }

                dt = cg.time - s_breathLastTime;
                if (dt < 0 || dt > 500) { dt = 0; } // clamp pauses / map loads
                s_breathLastTime = cg.time;

                // BUTTON_RUN is the run/walk SPEED state, not the raw key: with default "always run" it is
                // SET while running and CLEARED while the walk key (Shift) is held - so "Shift held" = the
                // bit is CLEAR. Hold Shift -> walk -> hold breath (steady).
                cgi.GetUserCmd(cgi.GetCurrentCmdNumber(), &bcmd);
                bShift         = (bcmd.buttons & BUTTON_RUN) ? qfalse : qtrue;
                s_breathSteady = qfalse;

                if (s_breathCooldownEnd != 0) {
                    if (cg.time >= s_breathCooldownEnd) {
                        s_breathCooldownEnd = 0;
                        s_breathRemainMs    = iHoldMs; // recharge complete
                    }
                // [bug-2133] the REAL ADS button, not the mount-forced predicate. CG_AimingDownSights
                // now returns true while mounted, which silently handed mounted players a client-side
                // breath hold the SERVER never granted: steady sway and the inhale cue with no actual
                // accuracy behind it, quietly draining the hold budget so it was empty when they did
                // press ADS. The pose and FOV consumers keep using bAds.
                } else if (((bcmd.buttons & BUTTON_COOPADS) || bScoped) && bShift && s_breathRemainMs > 0) {
                    s_breathSteady = qtrue;
                    s_breathRemainMs -= dt;
                    if (s_breathRemainMs <= 0) {
                        s_breathRemainMs    = 0;
                        s_breathCooldownEnd = cg.time + iCoolMs; // ran out -> start recharge
                    }
                }

                // breath SFX on the edges: inhale when you start holding, exhale when it ends (released,
                // ran out, or left ADS). Local 2D sounds = each player only hears their own breath.
                if (s_breathSteady && !s_breathWasSteady) {
                    cgi.S_StartLocalSound("sound/coop_breath/breath_in.wav", qfalse);
                } else if (!s_breathSteady && s_breathWasSteady) {
                    cgi.S_StartLocalSound("sound/coop_breath/breath_out.wav", qfalse);
                }
                s_breathWasSteady = s_breathSteady;

                // AUDIO DUCK: while holding breath, gently fade MUSIC + AMBIENT bed DOWN ("focus"), then
                // restore when the breath ends. We deliberately do NOT touch s_volume (the master SFX bus) so
                // GUNSHOTS and other effects stay at full volume - only the music/ambience recede. cg_breathDuck
                // = ducked level (fraction of base, 1 = no duck, lower = deeper). Base captured at duck start.
                {
                    static float s_breathDuck = 0.0f;       // 0 = normal, 1 = fully ducked
                    static float s_baseMus = -1.0f, s_baseAmb = -1.0f;
                    float        duckTarget = s_breathSteady ? 1.0f : 0.0f;
                    float        dstep      = (cg.frametime > 0) ? ((float)cg.frametime / 1000.0f) * 4.0f : 1.0f;

                    if (dstep > 1.0f) { dstep = 1.0f; }
                    s_breathDuck += (duckTarget - s_breathDuck) * dstep;
                    if (s_breathDuck < 0.0f) { s_breathDuck = 0.0f; }

                    if (s_breathDuck > 0.001f) {
                        cvar_t *pDuck   = cgi.Cvar_Get("cg_breathDuck", "0.7", CVAR_ARCHIVE);
                        float   duckMin = pDuck ? pDuck->value : 0.7f;
                        float   mul;
                        if (s_baseMus < 0.0f) { // capture base once, at duck start
                            s_baseMus = cgi.Cvar_Get("s_musicvolume", "0.9", 0)->value;
                            s_baseAmb = cgi.Cvar_Get("s_ambientvolume", "0.6", 0)->value;
                        }
                        mul = 1.0f - s_breathDuck * (1.0f - duckMin); // 1.0 -> duckMin
                        cgi.Cvar_Set("s_musicvolume", va("%g", s_baseMus * mul));
                        cgi.Cvar_Set("s_ambientvolume", va("%g", s_baseAmb * mul));
                    } else if (s_baseMus >= 0.0f) { // fully recovered -> restore exact base, clear
                        cgi.Cvar_Set("s_musicvolume", va("%g", s_baseMus));
                        cgi.Cvar_Set("s_ambientvolume", va("%g", s_baseAmb));
                        s_baseMus = -1.0f;
                        s_baseAmb = -1.0f;
                    }
                }
            }

            // SWAY - gentle breathing drift while aiming (figure-8), UNLESS holding breath (steady)
            if (bAds && !s_breathSteady && pSway && pSway->value > 0.0f) {
                float fSpd = pSwaySpd ? pSwaySpd->value : 1.0f;
                float t    = cg.time * 0.001f * fSpd;
                // [user 2026-08-27] braced: the wander dies back to a third. The gun going still is
                // the strongest tell that the support took hold - stronger than any icon or sound.
                VectorMA(pREnt->origin, sin(t * 1.1f) * pSway->value * fBrSway,        mat[1], pREnt->origin); // L/R
                VectorMA(pREnt->origin, sin(t * 1.7f + 0.6f) * pSway->value * 0.7f * fBrSway, mat[2], pREnt->origin); // U/D
            }

            // SCOPE SWAY (snipers) - the gun is hidden behind the scope, so wobble the VIEW so the scope
            // picture gently wanders; holding breath (steady) stops it (a steady shot is then dead-on).
            // Visual only, in DEGREES (cg_scopeSway). cg_modelanim.c rebuilds the view axis from
            // cg.refdefViewAngles right after this returns, so the change reaches the rendered scope.
            if (bScoped && !s_breathSteady) {
                cvar_t *pScopeSway = cgi.Cvar_Get("cg_scopeSway", "0.25", CVAR_ARCHIVE);
                float   fMag       = pScopeSway ? pScopeSway->value : 0.25f;
                if (fMag > 0.0f) {
                    float fSpd = pSwaySpd ? pSwaySpd->value : 1.0f;
                    float t    = cg.time * 0.001f * fSpd;
                    cg.refdefViewAngles[0] += sin(t * 1.7f + 0.6f) * fMag * 0.7f; // pitch (U/D)
                    cg.refdefViewAngles[1] += sin(t * 1.1f) * fMag;              // yaw (L/R)
                }
            }

            // RECOIL - detect a shot as a small drop in clip ammo on the SAME weapon (ignores reloads /
            // weapon switches), accumulate a kick (capped for sustained auto), then decay back to rest.
            // Holding breath (steady) softens the visual kick too.
            if (iWpn == s_lastWpn && s_lastClip >= 0 && iClip < s_lastClip && (s_lastClip - iClip) <= 4) {
                {
                    static cvar_t *pFP = NULL;
                    if (!pFP) { pFP = cgi.Cvar_Get("coop_fovPunch", "1.0", CVAR_ARCHIVE); }
                    s_fovPunch += 0.55f * fClassKick * pFP->value * (float)(s_lastClip - iClip);
                    if (s_fovPunch > 2.2f) { s_fovPunch = 2.2f; }
                }
                CG_NoteLocalFire(); // timestamp OUR shot. Flesh impacts are broadcast for EVERY
                                    // shooter, so without this a teammate killing someone next to
                                    // us would splatter blood on OUR weapon.
                // [user 2026-08-20] HIP FIRE KICKS TOO. This was gated on bAds, so the gun only
                // recoiled while aiming - and most shooting in this game is from the hip, which is
                // exactly where the weapon read as weightless. Hip kick is LARGER than ADS (the
                // weapon is not braced against the shoulder) but decays out of a pose nobody is
                // aiming with, so it costs no accuracy feel.
                if (pRecoil && pRecoil->value > 0.0f) {
                    // [user 2026-08-20 - bug] "guns kinda clip into the camera when you hold
                    // down the trigger". The sustained-fire cap was 6x the PER-SHOT kick, so it
                    // scaled with the same three factors the kick did: on an MG that is
                    // 0.5 * 1.5 * 1.35 * 6 = 6.1 units of standing displacement, 3.0 of it
                    // straight back into the near plane. A cap expressed as a multiple of the
                    // thing it is capping is not a cap. It is now an absolute ceiling in world
                    // units - the only frame in which "does it touch the camera" is a question.
                    float fHip    = bAds ? 1.0f : 1.35f;
                    float fMax    = pRecoil->value * fClassKick * fHip * 6.0f;
                    float fBreath = s_breathSteady ? 0.5f : 1.0f;
                    if (fMax > RECOIL_MAX_UNITS) {
                        fMax = RECOIL_MAX_UNITS;
                    }
                    s_recoil += pRecoil->value * fClassKick * fBreath * fHip * (float)(s_lastClip - iClip);
                    if (s_recoil > fMax) { s_recoil = fMax; }
                }
            }
            s_lastClip = iClip;
            s_lastWpn  = iWpn;

            // [user 2026-08-27] "bonus points would be if we could get the guns model up against the
            // object that is triggering it". Push the weapon out along its own forward axis and settle
            // it down while mounted: the gun visibly reaches the surface and rests on it instead of
            // floating in the same place it always sits. Eased by the envelope, so it travels there
            // over the mount rather than teleporting.
            if (fBrace > 0.0f) {
                // [user 2026-08-27] "your second hand on the weapon disappears when you brace". The
                // 7u forward push was walking the weapon out past the support hand - the two are drawn
                // as separate entities and only the one carrying this offset moved, so the off hand was
                // left behind and clipped out. A much smaller push keeps them together; the sense of
                // reaching the surface now comes from the settle rather than from raw travel.
                // [user 2026-08-27] rest it against the REAL surface: the server publishes how far
                // the support actually was, so the weapon reaches the sandbag it is on rather than a
                // guessed constant. Clamped hard - the off hand is a separate entity and a long push
                // walks the gun out of it (that was the vanishing-hand bug).
                cvar_t *pFwd  = cgi.Cvar_Get("coop_braceGunFwd", "2.0", CVAR_ARCHIVE);
                cvar_t *pDn   = cgi.Cvar_Get("coop_braceGunDown", "1.2", CVAR_ARCHIVE);
                cvar_t *pRest = cgi.Cvar_Get("coop_braceRest", "0", 0);
                cvar_t *pMax  = cgi.Cvar_Get("coop_braceGunReach", "4.5", CVAR_ARCHIVE);
                float   fKick = CG_CoopBraceKick();
                float   fReach = (pFwd ? pFwd->value : 2.0f);
                if (pRest && pRest->integer > 0) {
                    float fSurf = (float)pRest->integer * 0.12f; // nearer support = less reach needed
                    if (fSurf > fReach) { fReach = fSurf; }
                }
                if (pMax && fReach > pMax->value) { fReach = pMax->value; }
                VectorMA(pREnt->origin, fBrace * fReach, mat[0], pREnt->origin);
                VectorMA(pREnt->origin, -fBrace * (pDn ? pDn->value : 1.2f),  mat[2], pREnt->origin);
                // the settle: the weapon drops onto the surface and rebounds
                if (fKick > 0.0f) {
                    VectorMA(pREnt->origin, -fKick * 3.4f, mat[2], pREnt->origin);
                    VectorMA(pREnt->origin, -fKick * 1.2f, mat[0], pREnt->origin);
                }
            }

            if (s_recoil > 0.0f) {
                // gun kicks UP (muzzle climb) and BACK toward the camera, then recovers. The
                // backward term saturates INDEPENDENTLY of the cap above: muzzle climb may grow
                // freely without ever intersecting the view, travel toward the eye may not.
                float fBack = s_recoil * 0.5f;
                if (fBack > RECOIL_MAX_BACK) {
                    fBack = RECOIL_MAX_BACK;
                }
                VectorMA(pREnt->origin,  s_recoil * 0.8f, mat[2], pREnt->origin); // up
                VectorMA(pREnt->origin, -fBack,           mat[0], pREnt->origin); // back toward camera
                // ASYMMETRIC RECOVERY. A single fast decay reads as springy and light: real weight
                // is a sharp kick and a SLOW return, and the heavier the weapon the slower it
                // settles. Divide the recovery rate by the class kick, so a pistol snaps back at
                // ~15/s and an MG crawls at ~6/s off the same one constant.
                {
                    float fRec = 9.0f / (fClassKick > 0.1f ? fClassKick : 1.0f);
                    // [user 2026-08-27] BRACED RECOIL, portrayed rather than removed. A rested gun
                    // still jolts - the round does not care what the bipod is on - but the support
                    // absorbs it instead of the shooter, so the recovery is fast and lands back on
                    // the SAME point of aim. Keeping the kick and tripling the return rate is what
                    // reads as braced; deleting the kick would just read as a broken gun.
                    fRec *= 1.0f + fBrace * 2.2f;
                    s_recoil -= s_recoil * (cg.frametime / 1000.0f) * fRec;
                    if (s_recoil < 0.002f) {
                        s_recoil = 0.0f;
                    }
                }
            }

            // HZM coop [user 2026-08-20] LANDING + FOOTFALL. Nothing sells a heavy object like it
            // reacting to the body carrying it. A landing is detected from the predicted player
            // state - a real downward speed that ends with the player on the ground - and drives a
            // dip-and-settle on the gun scaled by both the fall speed and the weapon class. The
            // footfall term is a slow bob that only exists while actually moving on the ground, so
            // a standing player sees none of it.
            if (CoopWeaponFeelOn()) {
                static float s_landDip   = 0.0f;   // eased, units
                static float s_lastVelZ  = 0.0f;
                static int   s_lastGround = 1;
                // CLAMPED. The phase integrators below (s_stepPhase, s_brPhase) accumulate dt, and
                // cg.frametime is clamped to 5000ms - not 200 - for a client of a remote server.
                // An unclamped 5s hitch would advance the footfall phase by ~20 radians, which is
                // the very phase teleport those integrators were introduced to prevent.
                float        fDt2  = cg.frametime / 1000.0f;
                if (fDt2 > 0.1f) { fDt2 = 0.1f; }
                int          bGround = (cg.predicted_player_state.groundEntityNum != ENTITYNUM_NONE);
                float        fSpeed;

                // [2026-08-21] consume the SHARED latch instead of re-detecting. This is the first
                // consumer in the frame, so it is the one that computes it. The old local detector
                // had no staleness reseed and carried the die-airborne-then-respawn phantom slam.
                CoopLandingDetect();
                if (s_landTime == cg.time && s_landSev > 0.0f) {
                    // TIER depth: a hop, a drop and a fall should not all dip the same amount.
                    // 1.0 / 1.35 / 1.8 on top of the existing severity ramp, so light landings are
                    // unchanged and only the harder ones grow.
                    static const float kTierDepth[3] = {1.0f, 1.35f, 1.8f};
                    s_landDip += s_landSev * 3.2f * fClassKick * kTierDepth[s_landTier];
                }
                s_lastGround = bGround;
                s_lastVelZ   = cg.predicted_player_state.velocity[2];

                fSpeed = (float)sqrt(cg.predicted_player_state.velocity[0] * cg.predicted_player_state.velocity[0]
                                     + cg.predicted_player_state.velocity[1] * cg.predicted_player_state.velocity[1]);
                if (s_landDip > 0.001f) {
                    VectorMA(pREnt->origin, -s_landDip, mat[2], pREnt->origin); // the gun drops
                    VectorMA(pREnt->origin, -s_landDip * 0.35f, mat[0], pREnt->origin);
                    s_landDip -= s_landDip * fDt2 * (7.0f / (fClassKick > 0.1f ? fClassKick : 1.0f));
                    if (s_landDip < 0.002f) {
                        s_landDip = 0.0f;
                    }
                }
                if (bGround && fSpeed > 40.0f && !bScoped) {
                    // footfall bob: amplitude from speed, frequency from speed, weight from class.
                    // Halved in ADS so it never fights the sight picture.
                    static cvar_t *pStep      = NULL;
                    static float   s_stepPhase = 0.0f;
                    float          amp, ph;
                    if (!pStep) {
                        pStep = cgi.Cvar_Get("cg_weaponFootfall", "1", CVAR_ARCHIVE);
                    }
                    if (pStep->value > 0.0f) {
                        amp = (fSpeed / 300.0f) * 0.55f * fClassKick * pStep->value * (bAds ? 0.5f : 1.0f);
                        if (amp > 1.2f) {
                            amp = 1.2f;
                        }
                        // [user 2026-08-20 - bug] "gun seems like it kinda stutters while you
                        // are walking". This was ph = cg.time * 0.001f * (4.0f + fSpeed*0.012f)
                        // - phase computed as elapsed_time x current_frequency. When frequency
                        // changes, that expression jumps by elapsed_time x delta_frequency, and
                        // elapsed_time only ever grows: five minutes into a map, a speed change
                        // of ONE unit displaces the phase by 300 * 0.012 = 3.6 radians - over
                        // half a cycle - in a single frame. Walking speed changes every frame,
                        // so the bob teleported around the sine continuously, and it got worse
                        // the longer the map ran. Integrate the phase instead: frequency then
                        // sets the RATE and may change freely without moving the current
                        // position. The breathing term below had the same bug, keyed on health.
                        // [user 2026-08-27] LOCK THE GUN BOB TO THE BOOTS. This integrator ran on its
                        // own clock, so the weapon bobbed at one rate while the footstep sounds fired
                        // on another - the two drifted in and out of phase and the walk never quite
                        // felt like it belonged to the body. The engine already has the right clock on
                        // the wire: ps.bobCycle is the 0-255 phase pmove steps footfalls from (it fires
                        // one when bit 128 flips), so reading it makes the gun move WITH the stride
                        // instead of near it. Falls back to the old integrator if locking is off.
                        {
                            static cvar_t *pLock = NULL;
                            if (!pLock) { pLock = cgi.Cvar_Get("coop_bobLock", "1", CVAR_ARCHIVE); }
                            if (pLock->integer && cg.snap) {
                                s_stepPhase = ((float)cg.snap->ps.bobCycle / 256.0f) * 2.0f * M_PI;
                            } else {
                                s_stepPhase += fDt2 * (4.0f + fSpeed * 0.012f);
                            }
                        }
                        if (s_stepPhase > 62831.85f) {
                            s_stepPhase -= 62831.85f; // 10k cycles; keeps float precision sane
                        }
                        ph = s_stepPhase;
                        VectorMA(pREnt->origin, amp * (float)sin(ph), mat[2], pREnt->origin);
                        VectorMA(pREnt->origin, amp * 0.45f * (float)sin(ph * 0.5f), mat[1], pREnt->origin);
                    }
                }
                // HZM coop [user 2026-08-20] MEDKIT STOW.
                // "you probably wouldnt have a gun out when doing that". The third-person torso
                // state (COOP_SELFHEAL) already existed; this is the first-person half. The server
                // stuffs coop_medkitView for the duration - the same per-client pattern coop_dbnoView
                // and coop_limpView use - and the weapon sinks out of frame and eases back.
                //
                // The alive test is a SAFETY, not decoration: medkit.scr clears the flag on all four
                // of its exit paths, but if a player is ever removed mid-heal the clear cannot run,
                // and a permanently invisible weapon is a far worse bug than a missing animation.
                // HZM coop [user 2026-08-21] VAULT - the HANDS half.
                //
                // "Vaulting still doesn't seem like im really moving over something, feels more like
                // sliding and its between the camera and animation." It slid because nothing
                // presented it: the entire mechanic server-side is one velocity assignment, with no
                // camera work, no viewmodel work, and no observable state.
                //
                // The server now stuffs coop_vaultView as an incrementing COUNTER on each vault (an
                // instant has no duration to describe, so there is no "off" to send). Any change is
                // an edge: latch the time and run the whole envelope here. A dropped or duplicated
                // command therefore degrades to a missed or doubled flourish, never a stuck view.
                //
                // Shape: the gun drops hard and fast as both hands go to the obstacle, holds while
                // you cross, then comes back up more slowly than it left. Asymmetric on purpose -
                // equal in and out reads as a bob rather than as letting go and re-gripping.
                //
                // Registered as an AUTHORED STOW (mirrored into s_vFeelExempt) exactly like the
                // medkit below. It is a deliberate large pose, not jitter, so the 9u/4u budget must
                // not scale it - clamping only this half is what desynchronised the camera and the
                // gun in the DBNO regression.
                {
                    float fV = CoopVaultEnv();

                    if (fV > 0.001f) {
                        VectorMA(pREnt->origin, -fV * 22.0f, mat[2], pREnt->origin);
                        VectorMA(pREnt->origin, -fV * 9.0f,  mat[0], pREnt->origin);
                        VectorMA(s_vFeelExempt, -fV * 22.0f, mat[2], s_vFeelExempt);
                        VectorMA(s_vFeelExempt, -fV * 9.0f,  mat[0], s_vFeelExempt);
                    }
                }

                {
                    static cvar_t *pMed = NULL;
                    static float   s_medStow = 0.0f;
                    float          tgt2, k2;
                    if (!pMed) { pMed = cgi.Cvar_Get("coop_medkitView", "0", 0); }
                    tgt2 = (pMed->integer && cg.snap && cg.snap->ps.stats[STAT_HEALTH] > 0)
                               ? 1.0f : 0.0f;
                    k2 = fDt2 * 6.0f;
                    if (k2 > 1.0f) { k2 = 1.0f; }
                    s_medStow += (tgt2 - s_medStow) * k2;
                    if (s_medStow < 0.001f && tgt2 == 0.0f) { s_medStow = 0.0f; }
                    if (s_medStow > 0.001f) {
                        VectorMA(pREnt->origin, -s_medStow * 26.0f, mat[2], pREnt->origin);
                        VectorMA(pREnt->origin, -s_medStow * 6.0f,  mat[0], pREnt->origin);
                        // authored pose, not jitter - exempt from the feel budget
                        VectorMA(s_vFeelExempt, -s_medStow * 26.0f, mat[2], s_vFeelExempt);
                        VectorMA(s_vFeelExempt, -s_medStow * 6.0f,  mat[0], s_vFeelExempt);
                    }
                }

                // HZM coop [user 2026-08-21] TRANSLATIONAL WEAPON LAG. The shipped weapon lag
                // responds to TURNING only, so starting, stopping and reversing a strafe moved the
                // gun as though it were welded to the eye. Drive an offset from the player's
                // ACCELERATION (the frame-to-frame velocity delta) so the weapon trails a start and
                // overshoots a stop, harder for a heavier gun. Sharp strafe reversals throw it most,
                // which is exactly where the welded feel was most obvious.
                {
                    static cvar_t *pTL = NULL;
                    static vec3_t  s_lastVel = {0, 0, 0};
                    static vec3_t  s_lagOfs  = {0, 0, 0};
                    vec3_t         accel, want;
                    float          kL;

                    if (!pTL) { pTL = cgi.Cvar_Get("coop_weaponLagMove", "1.0", CVAR_ARCHIVE); }
                    if (pTL->value > 0.0f && fDt2 > 0.0001f) {
                        VectorSubtract(cg.predicted_player_state.velocity, s_lastVel, accel);
                        VectorScale(accel, 1.0f / fDt2, accel); // units per second per second
                        VectorCopy(cg.predicted_player_state.velocity, s_lastVel);

                        // the gun lags BEHIND the acceleration, so the offset opposes it
                        want[0] = -DotProduct(accel, mat[0]) * 0.00035f * fClassKick * pTL->value;
                        want[1] = -DotProduct(accel, mat[1]) * 0.00035f * fClassKick * pTL->value;
                        want[2] = -DotProduct(accel, mat[2]) * 0.00025f * fClassKick * pTL->value;
                        if (bAds) {
                            VectorScale(want, 0.3f, want);
                        }
                        kL = fDt2 * 9.0f;
                        if (kL > 1.0f) { kL = 1.0f; }
                        s_lagOfs[0] += (want[0] - s_lagOfs[0]) * kL;
                        s_lagOfs[1] += (want[1] - s_lagOfs[1]) * kL;
                        s_lagOfs[2] += (want[2] - s_lagOfs[2]) * kL;
                        CoopCamClamp(s_lagOfs, 2.5f);
                        VectorMA(pREnt->origin, s_lagOfs[0], mat[0], pREnt->origin);
                        VectorMA(pREnt->origin, s_lagOfs[1], mat[1], pREnt->origin);
                        VectorMA(pREnt->origin, s_lagOfs[2], mat[2], pREnt->origin);
                    } else {
                        VectorCopy(cg.predicted_player_state.velocity, s_lastVel);
                    }
                }

                // HZM coop [user 2026-08-20] CROUCH / STAND WEIGHT.
                // "Crouching and standing up could feel more realistic too."
                //
                // The engine moves the view height on a crouch, but nothing REACTS to it - the
                // weapon is carried down and up as though bolted to the eye, which is what makes it
                // read as a camera slide rather than a body movement. Drive a dip off the RATE of
                // the crouch blend (already eased for the ADS pose, so this costs one subtraction):
                // going down, the weapon lags and sinks; coming up, it lags and rises. Heavier
                // weapons lag more, and it settles with its own decay rather than tracking the
                // blend, so the reaction outlives the transition slightly the way real mass does.
                //
                // Translation channel, so it never touches the aim ray.
                {
                    static cvar_t  *pCr   = NULL;
                    static qboolean s_crInit = qfalse;
                    static float    s_crPrev = 0.0f;
                    static float    s_crVel  = 0.0f;
                    float           cb, d;

                    if (!pCr) { pCr = cgi.Cvar_Get("coop_crouchWeight", "1.0", CVAR_ARCHIVE); }
                    cb = CG_AdsCrouchBlend();
                    // s_crPrev only advances HERE, but CG_AdsCrouchBlend() advances every frame
                    // from CG_DrawActiveFrame - including while dead, in third person and in a
                    // cutscene, when this function is not called at all. Without these two guards
                    // the whole missed transition arrived as ONE frame delta: dying crouched and
                    // respawning standing gave d = -1.0 and a full-scale kick on every respawn.
                    if (!s_crInit) {
                        s_crInit = qtrue;
                        s_crPrev = cb;
                        s_crVel  = 0.0f;
                    }
                    d  = cb - s_crPrev;
                    if (d > 0.2f) { d = 0.2f; } else if (d < -0.2f) { d = -0.2f; }
                    s_crPrev = cb;

                    if (pCr->value > 0.0f && fDt2 > 0.0f) {
                        // impulse proportional to how fast the pose is changing
                        s_crVel += d * 26.0f * fClassKick * pCr->value;
                    }
                    if (s_crVel > 4.0f) { s_crVel = 4.0f; }
                    else if (s_crVel < -4.0f) { s_crVel = -4.0f; }

                    if (s_crVel > 0.0005f || s_crVel < -0.0005f) {
                        VectorMA(pREnt->origin, -s_crVel * 0.9f, mat[2], pREnt->origin);
                        VectorMA(pREnt->origin, -s_crVel * 0.35f, mat[0], pREnt->origin);
                        s_crVel -= s_crVel * fDt2
                                   * (6.5f / (fClassKick > 0.1f ? fClassKick : 1.0f));
                        if (s_crVel < 0.002f && s_crVel > -0.002f) { s_crVel = 0.0f; }
                    }
                }

                // HZM coop [user 2026-08-20] SPRINT-TO-FIRE, IDLE INSPECT and LOW-AMMO TELL.
                // All three ride pREnt->origin (translation), which is the only aim-honest channel:
                // it moves the gun, never the aim ray. None of them exists in third person, because
                // this whole function is skipped there.
                //
                // NOTE ON ROTATION: an "inspect" that turns the weapon toward the camera is NOT
                // possible here. The arms and the gun are SEPARATE render entities - every feel
                // layer in this file writes pREnt->origin only, and the sole rotation in the
                // pipeline (the ADS tune) is applied to the gun alone and capped near 12 degrees
                // precisely because more visibly detaches it from the hand. Rotating at
                // inspect-scale angles would pull the gun out of the player's grip. So the inspect
                // is expressed as a raise, a pull toward the eye and a lateral drift - readable as
                // "having a look at it" without ever desynchronising gun from hands.
                {
                    static cvar_t *pInsp = NULL, *pS2F = NULL, *pLowA = NULL;
                    static int     s_actTime   = 0;     // last frame the player DID something
                    static int     s_inspNext  = 0;     // when the next inspect may fire
                    static float   s_inspEnv   = 0.0f;  // 0..1 eased
                    static int     s_inspEnd   = 0;     // when the current inspect stops holding
                    static int     s_inspStart = 0;     // when it began - phase source for the sweep
                    static unsigned s_inspSeed = 2463534242u;
                    static int     s_lastWpn2  = -2;
                    static float   s_spEnvPrev = 0.0f;
                    static float   s_s2fEnv    = 0.0f;  // sprint-to-fire recovery, 0..1
                    usercmd_t      icmd;
                    int            iWpn2, iClip2, iMaxClip2;
                    qboolean       bBusy, bFiring;

                    if (!pInsp) { pInsp = cgi.Cvar_Get("coop_idleInspect", "1", CVAR_ARCHIVE); }
                    if (!pS2F)  { pS2F  = cgi.Cvar_Get("coop_sprintToFire", "1", CVAR_ARCHIVE); }
                    // [user 2026-08-22] RETIRED. "I don't think low ammo tell works honestly might
                    // as well turn it off too." The user is right, and the arithmetic says why: at full
                    // strength (empty clip) it is a 0.9 unit lateral cant plus a 0.45 unit bob, inside a
                    // feel budget of 9 units - about a tenth of the smallest thing that budget was written
                    // to bound. It was never going to be legible, so it is cost without signal.
                    //
                    // Default flipped to 0, AND a stale archived 1 is cleared once - the same fossil problem
                    // as coop_idleBolt (TRAPS T7 / bug-1990). This shipped default-ON, so every existing
                    // player carries `seta coop_lowAmmoTell "1"` in their saved config, and Cvar_Get keeps an
                    // existing value while updating only the reset string - a default change alone would
                    // reach nobody who has already played. pLowA is a static, so the clear runs once per
                    // session and `coop_lowAmmoTell 1` still works at the console afterwards if this is ever
                    // revisited with an amplitude that can actually be seen.
                    if (!pLowA) {
                        pLowA = cgi.Cvar_Get("coop_lowAmmoTell", "0", CVAR_ARCHIVE);
                        if (pLowA->integer) {
                            cgi.Cvar_Set("coop_lowAmmoTell", "0");
                        }
                    }

                    // GetUserCmd returns qfalse WITHOUT writing ucmd when the command has wrapped
                    // out of CMD_BACKUP, so the struct must be zeroed first or buttons is stack
                    // garbage and the inspect fires or cancels at random.
                    memset(&icmd, 0, sizeof(icmd));
                    cgi.GetUserCmd(cgi.GetCurrentCmdNumber(), &icmd);
                    iWpn2     = (cg.snap && cg.snap->ps.activeItems[1] >= 0) ? cg.snap->ps.activeItems[1] : -1;
                    iClip2    = cg.snap ? cg.snap->ps.stats[STAT_CLIPAMMO] : -1;
                    iMaxClip2 = cg.snap ? cg.snap->ps.stats[STAT_MAXCLIPAMMO] : 0;
                    bFiring   = (icmd.buttons & (BUTTON_ATTACKLEFT | BUTTON_ATTACKRIGHT)) ? qtrue : qfalse;

                    // "busy" = anything that must cancel an inspect. Fire, ADS, sprint and weapon
                    // switch are all readable THIS frame - usercmds are built before the frame is
                    // drawn, and CL_CmdButtons latches a press so even a sub-frame tap is caught.
                    // The viewmodel anim state is included too, but it is SNAPSHOT data: `reload` is
                    // a server console command, not a usercmd bit, so a reload cancel arrives one
                    // round trip late (100-300 ms on a remote server). That is accepted rather than
                    // hidden - the inspect amplitude is small enough that a late cancel is a slight
                    // drift, not a gun across the face.
                    bBusy = (qboolean)(bFiring || bAds || bScoped || fSpeed > 40.0f
                                       || iWpn2 != s_lastWpn2
                                       || (cg.snap && cg.snap->ps.iViewModelAnim != VM_ANIM_IDLE
                                           && (cg.snap->ps.iViewModelAnim < VM_ANIM_IDLE_0
                                               || cg.snap->ps.iViewModelAnim > VM_ANIM_IDLE_2)));
                    s_lastWpn2 = iWpn2;

                    if (bBusy || !bGround) {
                        s_actTime  = cg.time;
                        s_inspEnd  = 0;
                        s_inspNext = 0;
                    }
                    if (s_actTime == 0) { s_actTime = cg.time; }

                    // ---- IDLE INSPECT ------------------------------------------------------------
                    // 15 s base, randomised so it is not metronomic. The interval is rolled ONCE when
                    // the timer arms and then held: re-rolling per frame would make it jitter, and a
                    // frame hitch would re-roll it.
                    if (pInsp->integer && !bBusy && bGround) {
                        if (s_inspNext == 0) {
                            s_inspSeed ^= s_inspSeed << 13;
                            s_inspSeed ^= s_inspSeed >> 17;
                            s_inspSeed ^= s_inspSeed << 5;
                            // 15 s, plus 0..14 s of spread
                            s_inspNext = s_actTime + 15000 + (int)(s_inspSeed % 14000u);
                        }
                        if (s_inspEnd == 0 && cg.time > s_inspNext
                            && CoopWFeelStress() < 0.25f) { // only when genuinely calm
                            {
                            // [user 2026-08-27] longer by default now that the gesture shows BOTH
                            // flanks with a pause on each - 2200ms was sized for a single look.
                            static cvar_t *pInspTime = NULL;
                            if (!pInspTime) { pInspTime = cgi.Cvar_Get("coop_inspectTime", "3600", CVAR_ARCHIVE); }
                            s_inspEnd   = cg.time + (int)pInspTime->value;
                            s_inspStart = cg.time;
                        }
                            CoopGunFoley("magck", 500); // turning it over in the hands
                        }
                    }
                    if (s_inspEnd != 0 && cg.time > s_inspEnd) {
                        s_inspEnd  = 0;
                        s_inspNext = 0;
                        s_actTime  = cg.time;
                    }
                    {
                        float tgt = (s_inspEnd != 0) ? 1.0f : 0.0f;
                        float k   = fDt2 * (tgt > s_inspEnv ? 3.2f : 6.0f); // in slow, out fast
                        if (k > 1.0f) { k = 1.0f; }
                        s_inspEnv += (tgt - s_inspEnv) * k;
                        if (s_inspEnv < 0.001f && tgt == 0.0f) { s_inspEnv = 0.0f; }
                    }
                    if (s_inspEnv > 0.001f) {
                        // raise, draw toward the eye, and drift laterally - a look-over, not a spin
                        // [user 2026-08-21] "Gun inspection (at least for stg44) makes the back
                        // of the gun (stock) clip into the camera so you see inside the gun."
                        //
                        // The first version pulled the weapon 2.2u TOWARD the eye, which is what
                        // a real inspect does - but the view weapon already rests only a few
                        // units ahead of the camera, and a long gun (StG 44, rifles, the BAR)
                        // has its stock at the back of that. Any rearward travel puts the stock
                        // through the near plane and you see the inside of the receiver.
                        //
                        // Moved AWAY from the eye instead. It cannot be a close inspection in a
                        // first-person view this tight; raising and turning the weapon reads as
                        // "having a look at it" and cannot clip. Scaled DOWN by class weight,
                        // not up: the heavy guns are the long ones.
                        float e = s_inspEnv * (1.15f - 0.25f * fClassKick);
                        if (e < 0.0f) {
                            e = 0.0f;
                        }
                        // [user 2026-08-21] "when the inspection of gun happens its a little too low,
                        // it should turn but be raised up closer to I guess where the eyes are."
                        // The raise was 2.2u, which barely lifts it off the resting carry - the gun
                        // rolled to show its side while still sitting at hip-ish height, so it read
                        // as the gun twitching rather than being brought up for a look.
                        // coop_inspectRaise is that lift, and the forward push scales with it so a
                        // bigger raise does not bring the receiver back toward the near clip plane.
                        {
                            static cvar_t *pRaise = NULL;
                            float          fUp;

                            if (!pRaise) {
                                pRaise = cgi.Cvar_Get("coop_inspectRaise", "5.5", CVAR_ARCHIVE);
                            }
                            // [user 2026-08-21] "I think we should also have the gun come towards the
                            // player too with that, almost like we are rolling it and bringing it
                            // towards us to see the side of the gun better."
                            //
                            // HISTORY THAT MATTERS: an early version DID pull toward the eye and was
                            // reverted, because on a long gun the stock came through the near clip
                            // plane and you saw inside the receiver ("makes the back of the gun clip
                            // into the camera"). It then pushed AWAY, which is safe but reads as
                            // holding the gun out at arm's length rather than bringing it up to look.
                            //
                            // What changed is the RAISE. At the old 2.2u lift the weapon was still at
                            // carry height and directly ahead, so any rearward travel met the camera.
                            // Lifted to ~5-7u it sits above that line and can be drawn in without the
                            // stock reaching the near plane. So the pull is back, as a TUNABLE with a
                            // conservative default: coop_inspectPull, positive = toward the eye. If a
                            // long gun clips again, lower it - the clipping is real, not hypothetical.
                            static cvar_t *pPull = NULL;
                            float          fPull;

                            if (!pPull) {
                                pPull = cgi.Cvar_Get("coop_inspectPull", "5.5", CVAR_ARCHIVE);
                            }
                            fUp   = pRaise->value;
                            fPull = pPull->value;
                            // heavier guns are the long ones - draw them in less, they clip sooner
                            // Was scaled down hard by weapon weight, which pushed exactly the guns
                            // the user inspects most (STG44 is in the MG class) furthest away. Keep a
                            // mild reduction for clip safety on long guns, not a big one.
                            fPull *= (1.05f - 0.10f * fClassKick);
                            if (fPull < 0.0f) { fPull = 0.0f; }

                            // [user 2026-08-28] NEAR-PLANE GUARD. "the hand gets too close to the
                            // screen which causes it to clip with the camera". The pull was 5.5 units
                            // toward the eye while r_znear is 4, so the model was allowed to cross the
                            // near plane by construction - and the HANDS lead the weapon, so they cross
                            // first and clip before the gun visibly does.
                            //
                            // Measure where the pull actually leaves the model along the view axis and
                            // give up whatever part of it would breach a margin in front of the near
                            // plane. Clamping the RESULT rather than lowering the cvar means the
                            // inspect keeps its full travel wherever there is room for it, and only
                            // gives ground on the long guns that genuinely run out.
                            {
                                static cvar_t *pMin = NULL;
                                float fFwd, fLimit, fAllow;

                                if (!pMin) { pMin = cgi.Cvar_Get("coop_inspectMinDist", "7", CVAR_ARCHIVE); }

                                fFwd   = DotProduct(pREnt->origin, mat[0]) - DotProduct(cg.refdef.vieworg, mat[0]);
                                fLimit = pMin->value;
                                fAllow = fFwd - fLimit;            // how much pull there is room for
                                if (fAllow < 0.0f) { fAllow = 0.0f; }
                                if (e * fPull > fAllow) { fPull = fAllow / ((e > 0.001f) ? e : 1.0f); }
                            }

                            VectorMA(pREnt->origin, e * fUp,    mat[2], pREnt->origin); // raise toward the eyes
                            VectorMA(pREnt->origin, e * -fPull, mat[0], pREnt->origin); // and IN toward the player

                            // [user 2026-08-21, bug-2016] "no it still goes way off to the right side
                            // ... you can barely see the gun when its being inspected. Which control
                            // should I adjust" - and the answer was that NO control would fix it,
                            // because the inspect was fighting the feel budget and losing.
                            //
                            // The budget clamps the summed viewmodel offset to 9u, UNIFORMLY scaling
                            // every layer. The inspect alone asks ~9.4u (5.5 up, 5.5 back, 6.5 left)
                            // and shares that 9u with breathing, sway, bob and mass lag. So raising
                            // coop_inspectCentre raised the total LENGTH, which raised the scale-down
                            // factor, which cancelled most of the gain: the control saturated. That is
                            // exactly why turning it up did not move the gun.
                            //
                            // The fix is the one the budget already provides for deliberate large
                            // poses: register as an AUTHORED STOW, like the medkit stow, the weapon
                            // collision retract and the DBNO eye drop. Exempt offsets are subtracted
                            // before the clamp and added back after, so they pass through intact.
                            //
                            // The RAISE and the CENTRE are exempted - they frame the shot and are the
                            // two the user is asking for. The PULL is deliberately NOT: the 4u rearward
                            // cap is the guard that stops a long gun's stock coming through the near
                            // clip plane, and the history comment above records that clipping as real,
                            // not hypothetical. Keeping the pull inside the budget keeps that guard.
                            VectorMA(s_vFeelExempt, e * fUp, mat[2], s_vFeelExempt);
                            // [user 2026-08-21, with a reference image] The target is the weapon held
                            // CLOSE and CENTRED with its left flank toward the eye. The view weapon
                            // rests to the RIGHT of screen centre, so bringing it into frame means
                            // moving it LEFT - mat[1] is the left vector here (AngleVectorsLeft at the
                            // basis build), so this is positive. The old fixed 1.4 was far too small
                            // to centre anything; it is now the tunable that frames the shot.
                            {
                                static cvar_t *pCtr = NULL;
                                if (!pCtr) { pCtr = cgi.Cvar_Get("coop_inspectCentre", "6.5", CVAR_ARCHIVE); }
                                VectorMA(pREnt->origin, e * pCtr->value, mat[1], pREnt->origin);
                                // authored stow - see the note on the raise above. Without this the
                                // centring is the first thing the 9u clamp scales away.
                                VectorMA(s_vFeelExempt, e * pCtr->value, mat[1], s_vFeelExempt);
                            }
                        }

                        // [user 2026-08-21] "for gun idle animation maybe rotating the gun so you are
                        // seeing the left side of it/showcasing it may be another anim we could do".
                        //
                        // The inspect only ever TRANSLATED - raise, push out, drift sideways - which
                        // reads as the gun being moved rather than being looked at. Turning it is what
                        // showcases it, and it costs nothing extra: this is the same eased envelope,
                        // so the raise and the turn are one gesture.
                        //
                        // ROTATION IS FREE OF THE FEEL BUDGET, which clamps pREnt->origin only. It is
                        // also free of the camera: this function derives the view angles from
                        // pREnt->axis EARLY (AxisCopy near the top), so rotating it here - at the end,
                        // after that read - moves the weapon and provably not the view. Same reason
                        // the per-gun ADS sight rotation is safe.
                        //
                        // Yaw about the weapon's own up axis brings the LEFT flank toward the eye; a
                        // little roll stops it looking like a turntable. Scaled by the same class
                        // weight as the translation, so a long heavy gun turns less than a pistol.
                        {
                            static cvar_t *pInspRot = NULL;
                            float          fYaw, fRoll;
                            int            iRow;

                            if (!pInspRot) {
                                pInspRot = cgi.Cvar_Get("coop_inspectTurn", "1.25", CVAR_ARCHIVE);
                            }
                            // [user 2026-08-21] "thats not what I meant, I think I mean the gun would
                            // ROLL to the right so that the left side of the gun is facing upward
                            // more. Youre basically looking at the left side of the gun."
                            //
                            // First pass yawed about the weapon's UP axis, which swings the muzzle
                            // across your view - the gun turns to point sideways. Wrong gesture. The
                            // one asked for is a ROLL about the weapon's own FORWARD axis, which
                            // keeps the barrel pointing where it was and rotates the receiver so the
                            // left flank tips up into view. That is how you actually look at a gun
                            // you are holding.
                            //
                            // Roll leads; a small yaw remains only to angle it slightly toward the
                            // eye rather than presenting a flat side-on profile. NEGATIVE
                            // coop_inspectTurn rolls the other way if the flank tips away instead of
                            // toward you - the sign depends on the rig's handedness, which is not
                            // something to guess at.
                            // [user 2026-08-27] BOTH SIDES. The roll only ever went one way, so you
                            // only ever saw the left flank - the right side of the weapon is modelled
                            // and textured (the reload animations already swing these models through
                            // every angle) and was simply never presented.
                            //
                            // The sign is now swept across the inspect window with a DWELL at each
                            // extreme rather than a plain sine, which would race through both sides
                            // without ever resting on either: hold left, roll through centre, hold
                            // right, return. coop_inspectBothSides 0 restores the single-sided look.
                            {
                                static cvar_t *pBoth = NULL;
                                float          p, sgn;

                                if (!pBoth) { pBoth = cgi.Cvar_Get("coop_inspectBothSides", "1", CVAR_ARCHIVE); }
                                sgn = 0.0f;
                                if (pBoth->integer && s_inspEnd > s_inspStart) {
                                    // [user 2026-08-27] "isn't very fluid between flipping sides...
                                    // and it's really abrupt when it goes back to its base position".
                                    //
                                    // Both came from the same thing: the roll was still at FULL
                                    // deflection when the window expired, so the envelope's fast
                                    // release (6/s, correct for a cancel) yanked ~50 degrees of roll
                                    // home in a sixth of a second. The gesture now returns itself to
                                    // neutral before the window ends, so the release has nothing left
                                    // to undo, and the side-to-side cross is given twice the time it
                                    // had. Five phases: ease out to the left flank, hold, cross,
                                    // hold, ease home - all smoothstepped, none of them linear.
                                    p = (float)(cg.time - s_inspStart) / (float)(s_inspEnd - s_inspStart);
                                    if (p < 0.0f) { p = 0.0f; } else if (p > 1.0f) { p = 1.0f; }
                                    if (p < 0.12f) {
                                        float t = p / 0.12f;
                                        sgn = t * t * (3.0f - 2.0f * t);          // out to the left
                                    } else if (p < 0.34f) {
                                        sgn = 1.0f;                               // hold, left flank
                                    } else if (p < 0.58f) {
                                        float t = (p - 0.34f) / 0.24f;            // cross over
                                        sgn = 1.0f - 2.0f * (t * t * (3.0f - 2.0f * t));
                                    } else if (p < 0.80f) {
                                        sgn = -1.0f;                              // hold, right flank
                                    } else {
                                        float t = (p - 0.80f) / 0.20f;            // ease home to level
                                        sgn = -1.0f + (t * t * (3.0f - 2.0f * t));
                                    }
                                } else {
                                    sgn = 1.0f;
                                }
                                fRoll = e * 42.0f * pInspRot->value * sgn;
                                fYaw  = e * 8.0f  * pInspRot->value * sgn;
                            }

                            // [user 2026-08-21] "when the idle side gun view comes up its practically
                            // off screen... all the way to the right. which is a problem with most of
                            // the guns."
                            //
                            // Rotating pREnt->axis rotates the model about the ENTITY ORIGIN, which
                            // sits at the player - not at the gun. The weapon hangs off
                            // tag_weapon_right, well forward and to the right of that origin, so a
                            // 42 degree roll swept it through a large arc and flung it off screen.
                            // The further the tag is from the origin the worse the swing, which is
                            // why it affected most weapons rather than one.
                            //
                            // Rotate about the WEAPON'S OWN POSITION instead: sample the tag's world
                            // position before and after the rotation and translate the entity by the
                            // difference, so the gun spins in place rather than orbiting the player.
                            // TIKI_Orientation returns a MODEL-space offset, so it is unaffected by
                            // the axis change - the two samples differ only through the axis, which
                            // is exactly the correction needed.
                            if (fRoll > 0.05f || fRoll < -0.05f) {
                                static int    s_iWpnTag  = -2;
                                static int    s_iWpnTiki = 0;
                                vec3_t        vUp, vFwd, vTmp, vBefore, vAfter, vFix, vTagOfs;
                                orientation_t oW;
                                int           r;

                                if (s_iWpnTiki != (int)(size_t)pREnt->tiki) {
                                    s_iWpnTag  = cgi.Tag_NumForName(pREnt->tiki, "tag_weapon_right");
                                    s_iWpnTiki = (int)(size_t)pREnt->tiki;
                                }

                                // Capture the tag's MODEL-space offset ONCE. It does not depend on
                                // the axis, so the before/after difference must come from applying
                                // the SAME offset through the old and new axis. Re-querying after the
                                // rotation (as a first attempt did) risks a cached result and yields
                                // a zero correction - which is exactly "the fix did nothing".
                                VectorClear(vBefore);
                                VectorClear(vTagOfs);
                                if (s_iWpnTag >= 0) {
                                    oW = cgi.TIKI_Orientation(pREnt, s_iWpnTag);
                                    VectorCopy(oW.origin, vTagOfs);
                                    for (r = 0; r < 3; r++) {
                                        VectorMA(vBefore, vTagOfs[r], pREnt->axis[r], vBefore);
                                    }
                                }

                                VectorCopy(mat[0], vFwd);
                                for (iRow = 0; iRow < 3; iRow++) {
                                    RotatePointAroundVector(vTmp, vFwd, pREnt->axis[iRow], fRoll);
                                    VectorCopy(vTmp, pREnt->axis[iRow]);
                                }
                                VectorCopy(mat[2], vUp);
                                for (iRow = 0; iRow < 3; iRow++) {
                                    RotatePointAroundVector(vTmp, vUp, pREnt->axis[iRow], fYaw);
                                    VectorCopy(vTmp, pREnt->axis[iRow]);
                                }

                                if (s_iWpnTag >= 0) {
                                    VectorClear(vAfter);
                                    for (r = 0; r < 3; r++) {
                                        VectorMA(vAfter, vTagOfs[r], pREnt->axis[r], vAfter);
                                    }
                                    VectorSubtract(vBefore, vAfter, vFix);
                                    VectorAdd(pREnt->origin, vFix, pREnt->origin);
                                }

                                // [2026-08-21] WHERE DOES THE GUN ACTUALLY END UP ON SCREEN?
                                // Framing was being tuned by eye across a round trip each time. This
                                // projects the weapon tag into normalised screen space, so the values
                                // can be computed instead of guessed: sx/sy are -1..1 with 0,0 dead
                                // centre. sx near +1 means it is off the right edge - the exact
                                // complaint - and the correction is then arithmetic, not another try.
                                {
                                    static cvar_t *pTr2 = NULL;
                                    if (!pTr2) { pTr2 = cgi.Cvar_Get("coop_adsTrace", "0", 0); }
                                    if (pTr2->integer && s_iWpnTag >= 0) {
                                        vec3_t vW, vRel, vF2, vR2, vU2;
                                        float  fd, sx, sy;

                                        VectorCopy(pREnt->origin, vW);
                                        for (r = 0; r < 3; r++) {
                                            VectorMA(vW, vTagOfs[r], pREnt->axis[r], vW);
                                        }
                                        VectorSubtract(vW, cg.refdef.vieworg, vRel);
                                        AngleVectors(cg.refdefViewAngles, vF2, vR2, vU2);
                                        fd = DotProduct(vRel, vF2);
                                        if (fd > 0.5f) {
                                            float ta = (float)tan(cg.refdef.fov_x * 0.5f * M_PI / 180.0);
                                            float tb = (float)tan(cg.refdef.fov_y * 0.5f * M_PI / 180.0);
                                            sx = DotProduct(vRel, vR2) / (fd * (ta > 0.001f ? ta : 1.0f));
                                            sy = DotProduct(vRel, vU2) / (fd * (tb > 0.001f ? tb : 1.0f));
                                            cgi.Printf("^~^~^ INSPECT e=%.2f sx=%.3f sy=%.3f dist=%.1f "
                                                       "roll=%.1f\n", e, sx, sy, fd, fRoll);
                                        }
                                    }
                                }
                            }
                        }
                    }

                    // ---- SPRINT-TO-FIRE ----------------------------------------------------------
                    // COSMETIC ONLY, as the user specified: the weapon looks unready, it still fires
                    // normally. Modulates the EXISTING sprint-lower envelope rather than replacing
                    // it. When that envelope drops (sprint ended) the weapon overshoots slightly and
                    // settles, more slowly the heavier it is.
                    if (pS2F->integer) {
                        if (s_spEnvPrev - s_spEnvCur > 0.02f) {
                            float add = (s_spEnvPrev - s_spEnvCur) * 2.2f;
                            s_s2fEnv += add;
                            if (s_s2fEnv > 1.0f) { s_s2fEnv = 1.0f; }
                        }
                        s_spEnvPrev = s_spEnvCur;
                        if (s_s2fEnv > 0.001f) {
                            float e2 = s_s2fEnv * fClassKick;
                            VectorMA(pREnt->origin, -e2 * 1.5f, mat[2], pREnt->origin);
                            VectorMA(pREnt->origin, -e2 * 0.9f, mat[0], pREnt->origin);
                            VectorMA(pREnt->origin,  e2 * 0.6f, mat[1], pREnt->origin);
                            s_s2fEnv -= s_s2fEnv * fDt2
                                        * (5.5f / (fClassKick > 0.1f ? fClassKick : 1.0f));
                            if (s_s2fEnv < 0.002f) { s_s2fEnv = 0.0f; }
                        }
                    }

                    // ---- LOW-AMMO TELL -----------------------------------------------------------
                    // The guard is the whole feature. Weapon::ClipAmmo returns -1 for anything with
                    // no clip (grenades, knife, binoculars, mine detector) and STAT_MAXCLIPAMMO is 0
                    // for them, so a naive "clip <= 25% of max" reads -1 <= 0 = TRUE and every one of
                    // those items would sit at a permanent maximum tell. Require BOTH a real clip
                    // size and a non-negative count - the same shape the recoil detector above uses.
                    // EASED. The amplitude is continuous in ammo but the GATE is binary, so
                    // applying it raw stepped the weapon sideways in a single frame every time
                    // the trigger was released, the fire anim ended, or ADS was left.
                    {
                        static float s_lowEnv = 0.0f;
                        float        lowTgt = 0.0f, kLow;
                        if (pLowA->integer && iMaxClip2 > 0 && iClip2 >= 0 && !bAds && !bScoped
                            && !bFiring && cg.snap && cg.snap->ps.iViewModelAnim == VM_ANIM_IDLE) {
                            float frac2 = (float)iClip2 / (float)iMaxClip2;
                            if (frac2 < 0.25f) {
                                lowTgt = (0.25f - frac2) / 0.25f;
                            }
                        }
                        kLow = fDt2 * 5.0f;
                        if (kLow > 1.0f) { kLow = 1.0f; }
                        s_lowEnv += (lowTgt - s_lowEnv) * kLow;
                        if (s_lowEnv < 0.001f && lowTgt == 0.0f) { s_lowEnv = 0.0f; }
                        if (s_lowEnv > 0.001f) {
                            // grows as the clip empties; a canted "how many left?" hold rather than a
                            // motion, so it reads at a glance without demanding attention
                            static float s_lowPhase = 0.0f;
                            float        lowAmt = s_lowEnv;
                            s_lowPhase += fDt2 * 1.6f;
                            if (s_lowPhase > 62831.85f) { s_lowPhase -= 62831.85f; }
                            VectorMA(pREnt->origin, lowAmt * 0.9f, mat[1], pREnt->origin);
                            VectorMA(pREnt->origin,
                                     lowAmt * 0.45f * (float)sin(s_lowPhase), mat[2], pREnt->origin);
                        }
                    }
                }

                // HZM coop [user 2026-08-20] INJURED / STRESSED HAND TREMOR.
                // "when you are injured I think gun shaking should be more prevelant when ads and
                // also when not but that can also be based on compose/stress we build."
                //
                // This is a SECOND injury effect and it deliberately uses a different channel from
                // the one that already ships. coop_injurySway (further down this file) writes
                // cg.refdefViewAngles - an ANGULAR layer. Angular layers are not aim-honest: bullets
                // leave along ps->viewangles, which the sway never touches, so the camera lies by
                // exactly the sway amount. In the hip that only means the crosshair is off. Under
                // SIGHTS it is worse, because the iron sights are rebuilt from the already-swayed
                // angles - so the sight picture stays perfectly aligned and lies invisibly. Making
                // the angular sway stronger in ADS, which is what was literally asked for, would
                // have made aiming while hurt silently inaccurate with no visible tell.
                //
                // So the ADS half rides pREnt->origin instead. Translating the weapon moves the gun,
                // not the aim ray: the sights visibly drift off the target and the player can SEE
                // that they are unsteady, which is the actual goal. It is therefore allowed to be
                // strong under sights, unlike every other perturbation here.
                //
                // Phase is integrated, never time x frequency (see the footfall note above), and the
                // three frequencies are mutually detuned so it never reads as a metronome.
                if (!bScoped) {
                    static cvar_t *pInj = NULL, *pInjAds = NULL;
                    static float   s_injPhase = 0.0f;
                    float          hpF, sev, amp3;

                    if (!pInj)    { pInj    = cgi.Cvar_Get("coop_injuryShake", "1.0", CVAR_ARCHIVE); }
                    if (!pInjAds) { pInjAds = cgi.Cvar_Get("coop_injuryShakeAds", "1.15", CVAR_ARCHIVE); }

                    // reuse the published, peak-calibrated health fraction rather than a fourth
                    // health tracker - it already handles DBNO (0.02 while downed)
                    hpF = cgi.Cvar_Get("r_ppHealthFrac", "1", 0)->value;
                    if (hpF < 0.0f) { hpF = 0.0f; } else if (hpF > 1.0f) { hpF = 1.0f; }

                    // being hurt is the driver; general stress adds to it but cannot create it on
                    // its own, so a healthy player sprinting under fire does not get shaky hands
                    sev = (1.0f - hpF) * (0.65f + 0.35f * CoopWFeelStress());

                    if (pInj->value > 0.0f && sev > 0.01f) {
                        amp3 = sev * sev * 0.9f * pInj->value * (bAds ? pInjAds->value : 1.0f);
                        if (amp3 > 1.6f) { amp3 = 1.6f; }
                        s_injPhase += fDt2 * 7.3f;
                        if (s_injPhase > 62831.85f) { s_injPhase -= 62831.85f; }
                        VectorMA(pREnt->origin, amp3 * (float)sin(s_injPhase), mat[2], pREnt->origin);
                        VectorMA(pREnt->origin, amp3 * 0.7f * (float)sin(s_injPhase * 1.37f + 0.9f),
                                 mat[1], pREnt->origin);
                        VectorMA(pREnt->origin, amp3 * 0.35f * (float)sin(s_injPhase * 0.61f + 2.1f),
                                 mat[0], pREnt->origin);
                    }
                }

                // IDLE BREATHING. A slow drift that is always present out of ADS, grows when hurt,
                // and is suppressed while holding breath. Small enough to be felt rather than seen.
                if (!bScoped) {
                    static cvar_t *pBr      = NULL;
                    static cvar_t *pBrAds   = NULL; // HZM coop PART F - how much ADS damping releases under stress
                    static float   s_brPhase = 0.0f;
                    float          hp, amp2, t2, fRattle;
                    if (!pBr) {
                        pBr = cgi.Cvar_Get("cg_weaponBreath", "1", CVAR_ARCHIVE);
                    }
                    if (!pBrAds) {
                        pBrAds = cgi.Cvar_Get("coop_stressBreathAds", "0.85", CVAR_ARCHIVE);
                    }
                    hp = (cg.snap && cg.snap->ps.stats[STAT_MAXHEALTH] > 0)
                             ? (float)cg.snap->ps.stats[STAT_HEALTH] / (float)cg.snap->ps.stats[STAT_MAXHEALTH]
                             : 1.0f;
                    if (hp < 0.0f) {
                        hp = 0.0f;
                    } else if (hp > 1.0f) {
                        hp = 1.0f;
                    }
                    // HZM coop [user 2026-08-24] PART F - STRESS PERTURBATION.
                    //
                    // Breathing was keyed on HEALTH ALONE, so an unhurt player being shot at,
                    // sprinting and out of breath had exactly the same steady hands as one
                    // standing alone in an empty room. CoopWFeelStress() is the project's single
                    // rattled scalar (suppression, health, stamina, speed) - read it rather than
                    // invent a second input.
                    //
                    // Deliberately DIFFERENT from the injury shake a few lines above, which does
                    // `sev = (1 - hp) * (0.65 + 0.35 * stress)` so stress can amplify shaky hands
                    // but never create them on an unhurt man. That gate is right for HANDS and
                    // wrong for BREATHING: exertion and fear move your chest whether or not you
                    // have been hit. So this one takes the MAX instead of the product.
                    //
                    // MAX, not sum: health is already a stress input (weight 0.25), so adding the
                    // two would double-count it and compound into an amplitude neither was tuned
                    // for. Whichever is worse wins, which keeps the old health-only curve intact
                    // as a floor.
                    fRattle = 1.0f - hp;
                    {
                        float fStr = CoopWFeelStress();
                        if (fStr > fRattle) {
                            fRattle = fStr;
                        }
                    }

                    // ADS damping is no longer a flat 0.35. [user addendum 2026-08-20] "when you
                    // are injured I think gun shaking should be more prevelant when ads and also
                    // when not". Everything else in this file damps hard under sights because
                    // sight-picture disturbance is normally unwanted; for stress it IS the point,
                    // so the damping RELEASES as the player gets more rattled. Calm and aiming
                    // stays exactly as steady as it is today - at fRattle 0 this is still 0.35.
                    //
                    // COSMETIC ONLY, and it must stay that way: this moves the view weapon, never
                    // the aim vector, so rounds still go where the reticle points. Making it
                    // functional is a combat-balance change and a separate decision.
                    {
                        float fAdsMul = 0.35f + 0.65f * fRattle * (pBrAds ? pBrAds->value : 0.85f);
                        if (fAdsMul > 1.0f) {
                            fAdsMul = 1.0f;
                        }
                        amp2 = 0.30f * pBr->value * (1.0f + fRattle * 1.6f) * (bAds ? fAdsMul : 1.0f);
                    }
                    if (s_breathSteady) {
                        amp2 *= 0.15f;
                    }
                    // accumulated, not time x frequency - see the footfall note above. Health
                    // changes less often than speed, but a phase jump on every hit is a visible
                    // snap at exactly the moment the player is being shot at.
                    s_brPhase += fDt2 * (1.15f + fRattle * 0.9f); // hurt OR rattled = faster breathing
                    if (s_brPhase > 62831.85f) {
                        s_brPhase -= 62831.85f;
                    }
                    t2 = s_brPhase;
                    VectorMA(pREnt->origin, amp2 * (float)sin(t2), mat[2], pREnt->origin);
                    VectorMA(pREnt->origin, amp2 * 0.4f * (float)sin(t2 * 0.7f), mat[1], pREnt->origin);
                    // PART F - THE HANDS NEVER SETTLE TWICE IN THE SAME PLACE.
                    //
                    // The two terms above sit at a 1 : 0.7 frequency ratio, which is RATIONAL
                    // (10:7), so the figure-8 closes on itself every 10 cycles and the rest point
                    // repeats exactly. A third term at ~1/pi of the base is irrational against
                    // both, so the composite path never closes and the hands never return to
                    // precisely the same point.
                    //
                    // MODULATE, DO NOT REPLACE: this rides the SAME accumulator and the same
                    // amplitude. No new phase, no new state, no second oscillator - two wobbles
                    // at two frequencies in the one state that needs precision is the exact
                    // failure mode the plan called out. It also costs nothing when calm, because
                    // it is scaled by fRattle and skipped outright below the threshold.
                    if (fRattle > 0.002f) {
                        VectorMA(pREnt->origin,
                                 amp2 * 0.22f * fRattle * (float)sin(t2 * 0.31831f),
                                 mat[1], pREnt->origin);
                    }

                    // HZM coop [user 2026-08-24] HIT FLINCH - the weapon jolts when YOU are hit.
                    //
                    // The AI has had a hit reaction since coop_aiHitReact; the player had a viewkick and
                    // nothing else, so rounds landing on you disturbed the camera but never the hands.
                    //
                    // TRANSLATION ONLY, and that is a constraint rather than a shortcut: the FPS arms and
                    // the gun are separate render entities, and the per-gun ADS tune is the only rotation
                    // in this system - capped near 12 degrees precisely because more visibly detaches the
                    // gun from the hands (bug-105). A flinch-scale rotation would pull the weapon out of
                    // the player's grip.
                    //
                    // Direction is real, not decorative. STAT_DAMAGEDIR is damage_yaw in TENTHS OF A
                    // DEGREE, 0..3600 (player.cpp:8830), a bearing relative to facing - so a hit from the
                    // left shoves the muzzle right and one from behind shoves it forward, which reads as
                    // being pushed BY the round rather than as a generic shake.
                    //
                    // Cosmetic: view weapon only, never the aim vector. Being shot must not silently
                    // become an accuracy penalty - that is a combat-balance change and a separate call.
                    {
                        static cvar_t *pFl    = NULL;
                        static float   s_flAmp = 0.0f;   // envelope, units
                        static float   s_flFwd = 0.0f, s_flSide = 0.0f; // unit direction of the last hit
                        static int     s_flLastDir = -99999;
                        static int     s_flLastHp  = -1;
                        int            iDir, iHp;

                        if (!pFl) { pFl = cgi.Cvar_Get("coop_hitFlinch", "1.0", CVAR_ARCHIVE); }

                        iDir = cg.snap ? cg.snap->ps.stats[STAT_DAMAGEDIR] : 0;
                        iHp  = cg.snap ? cg.snap->ps.stats[STAT_HEALTH] : 0;

                        // Re-seed rather than fire when the gate is shut, so re-entering play does not
                        // flinch on a delta that accumulated while dead or spectating (bug-2003's fix 4).
                        if (iHp <= 0
                            || (cg.snap->ps.pm_flags & (PMF_SPECTATING | PMF_INTERMISSION | PMF_CAMERA_VIEW))) {
                            s_flLastDir = iDir;
                            s_flLastHp  = iHp;
                            s_flAmp     = 0.0f;
                        } else {
                            // TWO edges, deliberately. The bearing alone misses a second hit from the same
                            // direction (identical value, no change); the health drop alone misses a hit
                            // that does no damage. Either one is a hit.
                            qboolean bHit = qfalse;
                            float    fMag = 0.35f;

                            if (s_flLastHp >= 0 && iHp < s_flLastHp) {
                                float lost = (float)(s_flLastHp - iHp);
                                // STAT_HEALTH is a PERCENTAGE (player.cpp:8402), not raw HP, so this is
                                // already normalised and does not need dividing by coop_health - the exact
                                // unit slip that left the suppression severity term ~7.5x too weak.
                                fMag = 0.30f + lost * 0.055f;
                                if (fMag > 1.10f) { fMag = 1.10f; }
                                bHit = qtrue;
                            } else if (iDir != s_flLastDir) {
                                bHit = qtrue;
                            }
                            s_flLastDir = iDir;
                            s_flLastHp  = iHp;

                            if (bHit && pFl->value > 0.0f) {
                                float bearing = ((float)iDir / 10.0f) * (float)M_PI / 180.0f;
                                // away from the shot: the round pushes you off it
                                s_flFwd  = -(float)cos(bearing);
                                s_flSide = -(float)sin(bearing);
                                fMag *= pFl->value * (1.0f + 0.5f * CoopWFeelStress());
                                if (fMag > s_flAmp) { s_flAmp = fMag; } // a bigger hit wins, no stacking
                            }
                        }

                        if (s_flAmp > 0.0005f) {
                            VectorMA(pREnt->origin, s_flAmp * s_flFwd * 0.9f,  mat[0], pREnt->origin);
                            VectorMA(pREnt->origin, s_flAmp * s_flSide * 1.1f, mat[1], pREnt->origin);
                            VectorMA(pREnt->origin, s_flAmp * -0.45f,          mat[2], pREnt->origin);
                            // fast one-sided decay with a floor - a flinch is a snap, not a wobble.
                            s_flAmp -= s_flAmp * fDt2 * 7.5f;
                            if (s_flAmp < 0.002f) { s_flAmp = 0.0f; }
                        }
                    }
                }
            }

            // HZM coop - WEAPON WEIGHT / LAG. The gun TRAILS the camera when you turn, then springs back to
            // rest, for a sense of mass. Driven by the frame-to-frame look delta (cg.refdefViewAngles); per
            // class weight (pistols snappy, MGs heavy); reduced in ADS; skipped while scoped. VISUAL ONLY -
            // only nudges the view-weapon origin, never aim/bullets, and eases to 0 so the resting pose (incl.
            // the baked per-gun ADS alignment) is untouched.
            {
                static qboolean s_lagInit   = qfalse;
                static float    s_prevPitch = 0.0f, s_prevYaw = 0.0f;
                static float    s_lagX = 0.0f, s_lagY = 0.0f; // current trail offset (units): X=L/R, Y=U/D
                static float    s_lagVX = 0.0f, s_lagVY = 0.0f; // [weight 4] the velocity state that
                                                               // makes it a real second-order spring
                cvar_t *pLagDamp = cgi.Cvar_Get("cg_weaponLagDamping", "0.62", CVAR_ARCHIVE);
                cvar_t *pLagRef  = cgi.Cvar_Get("cg_weaponLagRefFps", "60", CVAR_ARCHIVE);
                cvar_t *pLagHeft = cgi.Cvar_Get("cg_weaponLagHeft", "0.55", CVAR_ARCHIVE);
                cvar_t *pLag    = cgi.Cvar_Get("cg_weaponLag", "0.7", CVAR_ARCHIVE);        // units of trail / deg
                cvar_t *pLagMax = cgi.Cvar_Get("cg_weaponLagMax", "3.5", CVAR_ARCHIVE);     // clamp (units)
                cvar_t *pLagStf = cgi.Cvar_Get("cg_weaponLagStiffness", "7", CVAR_ARCHIVE); // spring rate (1/s)
                cvar_t *pLagAds = cgi.Cvar_Get("cg_weaponLagADS", "0.35", CVAR_ARCHIVE);    // lag scale while ADS
                float   fGain   = pLag ? pLag->value : 0.7f;
                float   fMaxLag = pLagMax ? pLagMax->value : 3.5f;
                float   fStiff  = pLagStf ? pLagStf->value : 7.0f;
                float   fWeight = 1.0f;
                // [user 2026-08-27] BRACED damps the lag spring - DAMPED, never frozen. A gun pinned
                // rigid to the crosshair reads as a turret, which is the complaint every game that
                // over-tuned this got; leaving some trail is what keeps it feeling like a weapon.
                {
                    float fBr = CG_CoopBrace();
                    if (fBr > 0.0f) {
                        cvar_t *pBrLag = cgi.Cvar_Get("coop_braceLag", "0.18", CVAR_ARCHIVE);
                        fGain *= 1.0f - fBr * (1.0f - (pBrLag ? pBrLag->value : 0.18f));
                    }
                }
                float   dt      = (cg.frametime > 0) ? (cg.frametime / 1000.0f) : 0.0f;
                float   dPitch, dYaw, k, tX, tY;

                if (iClass & WEAPON_CLASS_PISTOL)      { fWeight = 0.55f; }
                else if (iClass & WEAPON_CLASS_SMG)    { fWeight = 0.8f; }
                else if (iClass & WEAPON_CLASS_RIFLE)  { fWeight = 1.1f; }
                else if (iClass & WEAPON_CLASS_MG)     { fWeight = 1.5f; }
                else if (iClass & WEAPON_CLASS_HEAVY)  { fWeight = 1.35f; }

                if (bAds) { fGain *= (pLagAds ? pLagAds->value : 0.35f); }

                dPitch = cg.refdefViewAngles[0] - s_prevPitch;
                dYaw   = cg.refdefViewAngles[1] - s_prevYaw;
                while (dPitch >  180.0f) { dPitch -= 360.0f; }
                while (dPitch < -180.0f) { dPitch += 360.0f; }
                while (dYaw   >  180.0f) { dYaw   -= 360.0f; }
                while (dYaw   < -180.0f) { dYaw   += 360.0f; }
                s_prevPitch = cg.refdefViewAngles[0];
                s_prevYaw   = cg.refdefViewAngles[1];

                k = fStiff * dt;
                if (k > 1.0f) { k = 1.0f; }

                if (!s_lagInit || dt <= 0.0f || fabs(dPitch) > 45.0f || fabs(dYaw) > 45.0f) {
                    // first frame / teleport / map change: snap to rest, don't lurch
                    s_lagInit = qtrue;
                    s_lagX = s_lagY = 0.0f;
                    s_lagVX = s_lagVY = 0.0f;
                } else if (fGain > 0.0f && !bScoped) {
                    // [weight 4] TWO BUGS AND A LIMIT, all in these four lines.
                    //
                    // FRAMERATE: the target was built from a per-FRAME angle delta, so the same mouse
                    // movement produced a trail four times longer at 60fps than at 250 - every tuning
                    // number in this block was silently calibrated to one machine. The target is now
                    // built from angular VELOCITY and re-scaled to a reference rate, so it means the
                    // same thing on every machine.
                    //
                    // FIRST ORDER: `pos += (target - pos) * k` can only ever approach. A real mass
                    // overshoots and comes back, and that overshoot-and-catch is precisely the motion
                    // the eye reads as inertia - a first-order spring cannot express it at any gain.
                    // Second order now: a velocity state, a spring constant and a damping ratio.
                    //
                    // [weight 5] ASYMMETRIC. Heavy things displace easily and realign reluctantly, so
                    // the spring is stiffer while the gun is being pushed away from rest than while it
                    // is returning. Symmetric springs read as rubber; this reads as weight.
                    float fRefFps = (pLagRef && pLagRef->value > 10.0f) ? pLagRef->value : 60.0f;
                    float fRateX  = -dYaw / dt;
                    float fRateY  =  dPitch / dt;
                    float fHeft   = CoopGunHeft();
                    float fZeta   = (pLagDamp ? pLagDamp->value : 0.62f);
                    float fOmega, fAccel;

                    // [vet, bug-2141] CLAMP THE RATE, NOT THE WEIGHTED TARGET. Clamping after the
                    // weight multiply meant that on any brisk turn - and a mouse turn is routinely
                    // 300-1500 deg/s - every weapon saturated at the same 3.5 and the weight term was
                    // discarded entirely. All that survived was omega, which heft LOWERS, so a lower
                    // omega covered less of an identical target and the heavy guns trailed LESS than
                    // the pistol. The cue was inverted at exactly the speeds people actually turn at.
                    // Clamping the input rate instead keeps the ceiling proportional to weight, so
                    // heft raises the amplitude AND slows the settle, which is what mass does.
                    {
                        float fRateMax = (fGain > 0.0001f) ? (fMaxLag * fRefFps / fGain) : 1e9f;
                        if (fRateX >  fRateMax) { fRateX =  fRateMax; }
                        else if (fRateX < -fRateMax) { fRateX = -fRateMax; }
                        if (fRateY >  fRateMax) { fRateY =  fRateMax; }
                        else if (fRateY < -fRateMax) { fRateY = -fRateMax; }
                    }
                    tX = fRateX * fGain * fWeight / fRefFps;
                    tY = fRateY * fGain * fWeight / fRefFps;

                    // heavier = a lower natural frequency: slower to move, slower to settle
                    fOmega = fStiff / (1.0f + fHeft * (pLagHeft ? pLagHeft->value : 0.55f));

                    // X
                    fAccel = (tX - s_lagX) * fOmega * fOmega - s_lagVX * 2.0f * fZeta * fOmega;
                    if ((tX - s_lagX) * s_lagVX < 0.0f) { fAccel *= 1.35f; } // displacing: stiffer
                    s_lagVX += fAccel * dt;
                    s_lagX  += s_lagVX * dt;
                    // Y
                    fAccel = (tY - s_lagY) * fOmega * fOmega - s_lagVY * 2.0f * fZeta * fOmega;
                    if ((tY - s_lagY) * s_lagVY < 0.0f) { fAccel *= 1.35f; }
                    s_lagVY += fAccel * dt;
                    s_lagY  += s_lagVY * dt;

                    // the clamp bounds the POSITION too - an underdamped spring can otherwise ring
                    // out past the authored maximum on a fast flick
                    // the rail scales with weight too - a shared ceiling would re-flatten everything
                    // the rate clamp above just preserved, plus a little headroom for the overshoot
                    // that makes it read as inertia in the first place
                    {
                        float fRail = fMaxLag * fWeight * 1.15f;
                        if (s_lagX >  fRail) { s_lagX =  fRail; s_lagVX = 0.0f; }
                        else if (s_lagX < -fRail) { s_lagX = -fRail; s_lagVX = 0.0f; }
                        if (s_lagY >  fRail) { s_lagY =  fRail; s_lagVY = 0.0f; }
                        else if (s_lagY < -fRail) { s_lagY = -fRail; s_lagVY = 0.0f; }
                    }
                } else {
                    s_lagX += (0.0f - s_lagX) * k; // scoped/disabled: ease out
                    s_lagY += (0.0f - s_lagY) * k;
                    s_lagVX = s_lagVY = 0.0f;
                }

                VectorMA(pREnt->origin, s_lagX, mat[1], pREnt->origin); // left/right trail
                VectorMA(pREnt->origin, s_lagY, mat[2], pREnt->origin); // up/down trail
            }

            // HZM coop - FREE-AIM: shift the gun toward the off-centre reticle so the weapon visibly leads to
            // that side as the aim drifts within the deadzone box (cg_freeAimGun = units of shift per degree).
            {
                cvar_t *pFAG = cgi.Cvar_Get("cg_freeAimGun", "0.18", CVAR_ARCHIVE);
                float   fg   = pFAG ? pFAG->value : 0.18f;
                if (fg != 0.0f) {
                    VectorMA(pREnt->origin,  s_faYaw   * fg, mat[1], pREnt->origin); // toward reticle L/R
                    VectorMA(pREnt->origin, -s_faPitch * fg, mat[2], pREnt->origin); // toward reticle U/D
                }
            }

            // HZM coop - SPRINT gun-lower. While sprinting (Shift held + NOT aiming + moving forward +
            // stamina left), lower the view weapon: a downward dip + pull BACK + slight down-tilt, plus a
            // slow run bob, eased in/out. The "sprinting" signal is recomputed here from
            // the same inputs the SERVER uses (TickSprint): the walk-key (BUTTON_RUN clear = Shift), NOT
            // aiming (ADS/scope keeps breath-hold + a normal weapon), NOT the Alt walk key (BUTTON_COOPWALK),
            // forwardmove > 0, and a client-side mirror of the stamina pool (same cvars as the server, so it
            // tracks the listen-server host closely). VISUAL ONLY - nudges the view-weapon origin along the
            // view basis, never aim/bullets, and eases to zero so the resting pose is preserved.
            {
                static qboolean s_spInit = qfalse;
                static float    s_spEnv  = 0.0f;   // 0 = rest, 1 = fully lowered; eased in/out
                static int      s_spLastRun = 0;   // see the staleness reseed immediately below
                cvar_t  *pSpOn   = cgi.Cvar_Get("coop_sprint", "1", CVAR_ARCHIVE);
                cvar_t  *pSpStam = cgi.Cvar_Get("coop_sprintStamina", "5", CVAR_ARCHIVE);
                cvar_t  *pSpRegen= cgi.Cvar_Get("coop_sprintRegen", "0.6", CVAR_ARCHIVE);
                cvar_t  *pLower  = cgi.Cvar_Get("cg_sprintLower", "1", CVAR_ARCHIVE);        // 0 = off
                cvar_t  *pLowAmt = cgi.Cvar_Get("cg_sprintLowerAmount", "3.0", CVAR_ARCHIVE);// dip (units)
                cvar_t  *pLowBack= cgi.Cvar_Get("cg_sprintLowerBack", "1.4", CVAR_ARCHIVE);  // pull back (units)
                cvar_t  *pLowTilt= cgi.Cvar_Get("cg_sprintLowerTilt", "1.2", CVAR_ARCHIVE);  // down-tilt (units)
                float    fMaxStam = pSpStam ? pSpStam->value : 5.0f;
                float    fRegen   = pSpRegen ? pSpRegen->value : 0.6f;
                float    dt       = (cg.frametime > 0) ? (cg.frametime / 1000.0f) : 0.0f;
                qboolean bAiming  = (bAds || bScoped) ? qtrue : qfalse;
                qboolean bAlive   = (cg.snap && cg.snap->ps.stats[STAT_HEALTH] > 0) ? qtrue : qfalse;
                qboolean bWantSprint = qfalse;
                // HZM coop [user 2026-08-26] CRAWL gun-lower rides the SAME envelope as sprint: while
                // prone and actually moving, the server strips the fire buttons (player.cpp,
                // coop_proneMoveNoFire) - this dip is the visual telling you WHY the trigger is dead.
                // Same 30/15 u/s hysteresis as the server so the two agree.
                qboolean bWantCrawl = qfalse;
                {
                    static qboolean s_crawlLatch = qfalse;
                    if (cg.predicted_player_state.pm_flags & PMF_VIEW_PRONE) {
                        vec_t *cv = cg.predicted_player_state.velocity;
                        float  c2 = cv[0] * cv[0] + cv[1] * cv[1];
                        if (c2 > 30.0f * 30.0f)      { s_crawlLatch = qtrue; }
                        else if (c2 < 15.0f * 15.0f) { s_crawlLatch = qfalse; }
                    } else {
                        s_crawlLatch = qfalse;
                    }
                    bWantCrawl = s_crawlLatch;
                }
                usercmd_t scmd;
                float    fEnvTarget, k;

                // [2026-08-21] STALENESS RESEED. These statics advance ONLY on the live first-person
                // path, so third person, death, spectating and cutscene cameras all freeze them. The
                // camera-motion block below has carried a 250ms reseed for exactly this reason since
                // a player who died airborne came back and got a full-strength landing slam; the
                // sprint envelope never got the same treatment. Without it, sprinting into an ADS
                // handoff or a death leaves s_spEnv near 1.0 and the client stamina mirror drifting
                // upward while the server drains it, so the lowered-gun pose can reappear at full
                // amplitude on a player who is walking. Re-seed rather than carry a stale envelope.
                if (s_spInit && (cg.time - s_spLastRun) > 250) {
                    s_spEnv  = 0.0f;
                    s_spStam = fMaxStam;
                }
                s_spLastRun = cg.time;

                if (fMaxStam < 0.1f) { fMaxStam = 0.1f; }
                if (s_spStam > fMaxStam) { s_spStam = fMaxStam; } // clamp mirror to current max
                s_spStamMax = fMaxStam; // published for CG_GetStamina

                cgi.GetUserCmd(cgi.GetCurrentCmdNumber(), &scmd);

                // walk-key state (Shift): BUTTON_RUN clear; Alt walk = BUTTON_COOPWALK set forces walk
                if (pSpOn && pSpOn->value != 0.0f && bAlive && !cg.renderingThirdPerson
                    && !(scmd.buttons & BUTTON_RUN) && !(scmd.buttons & BUTTON_COOPWALK)
                    && !bAiming && scmd.forwardmove > 0) {
                    bWantSprint = qtrue;
                }

                // mirror the server stamina drain/regen so the lower stops when the pool is exhausted
                if (dt > 0.0f && dt < 0.5f) {
                    if (bWantSprint && s_spStam > 0.0f) {
                        s_spStam -= dt;
                        if (s_spStam < 0.0f) { s_spStam = 0.0f; }
                    } else if (!bWantSprint) {
                        s_spStam += dt * fRegen;
                        if (s_spStam > fMaxStam) { s_spStam = fMaxStam; }
                    }
                }

                fEnvTarget = ((bWantSprint && s_spStam > 0.0f) || bWantCrawl) ? 1.0f : 0.0f;
                if (!s_spInit || dt <= 0.0f || dt >= 0.5f) {
                    s_spInit = qtrue;
                    s_spEnv  = fEnvTarget;
                } else {
                    k = (fEnvTarget > s_spEnv ? 8.0f : 6.0f) * dt; // ease in a touch faster than out
                    if (k > 1.0f) { k = 1.0f; }
                    s_spEnv += (fEnvTarget - s_spEnv) * k;
                }
                if (s_spEnv < 0.0f) { s_spEnv = 0.0f; }
                if (s_spEnv > 1.0f) { s_spEnv = 1.0f; }
                // mirrored for the sprint-to-fire recovery, which lives in the feel block
                // ABOVE this one and therefore reads it one frame stale. 16 ms; harmless for
                // an edge detector that only needs to see the envelope fall.
                s_spEnvCur = s_spEnv;

                if (pLower && pLower->value != 0.0f && s_spEnv > 0.001f) {
                    float fDip  = pLowAmt ? pLowAmt->value : 3.0f;
                    float fBack = pLowBack ? pLowBack->value : 1.4f;
                    float fTilt = pLowTilt ? pLowTilt->value : 1.2f;
                    // a slow run bob so the lowered gun sways with the stride
                    // [2026-08-21] DRIVE THE SPRINT DIP OFF THE GAIT, not off the wall clock.
                    //
                    // This was sin(cg.time * 0.001f * 8.0f) - the time*frequency form this project
                    // banned after bug-1983/1984/1985. It was benign in the narrow sense that the
                    // frequency is a hard constant so the phase cannot teleport, but it is a
                    // free-running 1.273 Hz oscillator with no relationship to the stride: at sprint
                    // speed the gait runs about 1.47 Hz, so the two BEAT at ~0.2 Hz and the sprint
                    // dip swells and fades on a ~5 second cycle that matches nothing on screen. It
                    // also never scaled with speed, which is most of why sprinting read as "the gun
                    // is tilted down" rather than "I am running".
                    //
                    // cg.fCurrentViewBobPhase is the integrated stride phase the camera bob already
                    // uses, and fabs(sin(phase - 0.94)) is the project's established one-lobe-per-
                    // FOOTSTEP shaper (see the vertical head-bob term). Sharing that one oscillator
                    // means the dip is locked to the footfalls and can never beat against them.
                    float fBob  = (float)fabs(sin(cg.fCurrentViewBobPhase - 0.94)) * 0.5f + 0.75f;

                    VectorMA(pREnt->origin, -s_spEnv * fDip * fBob, mat[2], pREnt->origin); // dip DOWN
                    VectorMA(pREnt->origin, -s_spEnv * fBack,       mat[0], pREnt->origin); // pull BACK
                    VectorMA(pREnt->origin, -s_spEnv * fTilt,       mat[1], pREnt->origin); // slight side dip

                    // [user 2026-08-21] "is there a way to make first person sprinting look like we
                    // are actually sprinting with our gun, right now it kinda just tilts down".
                    //
                    // The lowered carry pose above is a STATIC offset - it says "gun is down" and
                    // nothing else, which is exactly what reads as a tilt rather than a run. This is
                    // the moving half: the weapon swings across and pumps fore/aft with the stride,
                    // the way a carried rifle does when someone runs.
                    //
                    // Deliberately placed HERE, beside the dip, rather than in CG_CalcViewModelMovement
                    // where it would sit outside the feel budget. Splitting one effect across two
                    // independent ceilings means the clamp scales the dip and not the pump, so their
                    // ratio would drift with landing/lag/tremor activity that has nothing to do with
                    // sprinting. One budget owns both halves.
                    //
                    // Driven from the SAME fabs(sin(phase - 0.94)) footfall shaper as the dip, so the
                    // two are one oscillator and cannot beat. The lateral term uses the raw sine (it
                    // alternates with the foot, left then right); the fore/aft term uses the folded
                    // one at double rate, which is the actual pump. Sized small: the whole sprinting
                    // sum is already near the 9-unit ceiling with the dip, weapon lag and footfall.
                    {
                        static cvar_t *pPump = NULL;
                        float          fSwing, fPump;

                        if (!pPump) { pPump = cgi.Cvar_Get("coop_sprintPump", "1.0", CVAR_ARCHIVE); }
                        if (pPump->value > 0.001f) {
                            fSwing = (float)sin(cg.fCurrentViewBobPhase - 0.94) * s_spEnv * pPump->value;
                            fPump  = (fBob - 1.0f) * s_spEnv * pPump->value;
                            VectorMA(pREnt->origin, fSwing * 1.15f, mat[1], pREnt->origin); // across
                            VectorMA(pREnt->origin, fPump  * 1.60f, mat[0], pREnt->origin); // fore/aft
                            VectorMA(pREnt->origin, fSwing * 0.35f, mat[2], pREnt->origin); // slight lift
                        }
                    }
                }
            }

            // HZM coop - WEAPON COLLISION: when the muzzle is about to poke through a wall / doorframe, pull
            // the view weapon BACK toward you and dip it DOWN so it doesn't clip into geometry (and reads as
            // bracing the gun in tight quarters). A short forward trace from the eye along the true view dir
            // measures the gap; the closer the wall, the more the weapon retracts, eased in (snappy) / out
            // (softer). World-only trace (cliptoentities = false) so teammates don't trigger it. VISUAL ONLY -
            // moves the view-weapon origin, never aim/bullets. coop_weaponCollision 0 = off; Reach = trace
            // length (units); Back/Dip = max retract/drop at a flush wall.
            {
                static qboolean s_wcInit = qfalse;
                static float    s_wcEnv  = 0.0f;   // 0 = clear, 1 = wall flush against the muzzle; eased
                cvar_t  *pWcOn   = cgi.Cvar_Get("coop_weaponCollision", "1", CVAR_ARCHIVE);
                cvar_t  *pWcReach= cgi.Cvar_Get("coop_weaponCollisionReach", "30", CVAR_ARCHIVE); // trace len
                cvar_t  *pWcBack = cgi.Cvar_Get("coop_weaponCollisionBack", "9", CVAR_ARCHIVE);    // max pull-back
                cvar_t  *pWcDip  = cgi.Cvar_Get("coop_weaponCollisionDip", "4", CVAR_ARCHIVE);     // max down-dip
                qboolean bAlive2 = (cg.snap && cg.snap->ps.stats[STAT_HEALTH] > 0) ? qtrue : qfalse;
                float    dt2     = (cg.frametime > 0) ? (cg.frametime / 1000.0f) : 0.0f;
                float    fReach  = pWcReach ? pWcReach->value : 30.0f;
                float    fTarget = 0.0f;

                if (pWcOn && pWcOn->value != 0.0f && bAlive2 && !cg.renderingThirdPerson && fReach > 1.0f) {
                    vec3_t  wcZero = {0.0f, 0.0f, 0.0f};
                    vec3_t  wcFwd, wcEnd;
                    trace_t wcTr;
                    AngleVectors(cg.refdefViewAngles, wcFwd, NULL, NULL);
                    VectorMA(origin, fReach, wcFwd, wcEnd);
                    CG_Trace(&wcTr, origin, wcZero, wcZero, wcEnd, cg.snap->ps.clientNum,
                             MASK_SOLID, qfalse, qfalse, "WeaponCollision");
                    if (wcTr.fraction < 1.0f) {
                        fTarget = 1.0f - wcTr.fraction; // closer wall -> stronger retract
                    }
                }

                if (!s_wcInit || dt2 <= 0.0f || dt2 >= 0.5f) {
                    s_wcInit = qtrue;
                    s_wcEnv  = fTarget;
                } else {
                    float kk = (fTarget > s_wcEnv ? 16.0f : 9.0f) * dt2; // snap in fast, ease out softer
                    if (kk > 1.0f) { kk = 1.0f; }
                    s_wcEnv += (fTarget - s_wcEnv) * kk;
                }
                if (s_wcEnv < 0.0f) { s_wcEnv = 0.0f; }
                if (s_wcEnv > 1.0f) { s_wcEnv = 1.0f; }

                {
                    // the muzzle meeting a wall is a hard mechanical event and had no sound
                    static float s_wcPrev = 0.0f;
                    if (s_wcEnv > 0.45f && s_wcPrev <= 0.45f) {
                        CoopGunFoley("hhard", 450);
                    }
                    s_wcPrev = s_wcEnv;
                }
                if (s_wcEnv > 0.001f) {
                    float fBack = pWcBack ? pWcBack->value : 9.0f;
                    float fDip  = pWcDip ? pWcDip->value : 4.0f;
                    VectorMA(pREnt->origin, -s_wcEnv * fBack, mat[0], pREnt->origin); // pull BACK toward camera
                    VectorMA(pREnt->origin, -s_wcEnv * fDip,  mat[2], pREnt->origin); // dip DOWN (muzzle drops)
                    // EXEMPT from the feel budget - an authored retract (default 9u back), not
                    // jitter. Clamped, the muzzle went back through walls at under half strength.
                    VectorMA(s_vFeelExempt, -s_wcEnv * fBack, mat[0], s_vFeelExempt);
                    VectorMA(s_vFeelExempt, -s_wcEnv * fDip,  mat[2], s_vFeelExempt);
                }
            }
        }
    }

    // HZM coop [user 07-29] DBNO EYE HEIGHT (bug-1238). The script pins the player at
    // `modheight "prone"` while downed (dbno.scr:443), but prone viewheight still sits well above a
    // body collapsed on its side - first person reads as kneeling, not bleeding out.
    //
    // APPLIED HERE, AT THE END, AND NOWHERE ELSE. This value has been placed wrong twice:
    //   * in CG_CalcViewValues - discarded, because the eyes-bone block in this function does
    //     `VectorCopy(pCent->lerpOrigin, origin)` and rebuilds the eye from the model tag. It was
    //     not inert though: it silently moved the THIRD-person pivot and the camera trace start.
    //   * mid-function, right after the eyes-bone block - also discarded, because the view-height
    //     smoothing below it does a hard ASSIGNMENT, `origin[2] = cg.fCurrentViewHeight`, computed
    //     from predicted_player_state viewheight. Anything written before that is simply gone.
    // Everything that writes origin[2] has now run, so this is the only safe place.
    //
    // The view model moves by the SAME amount. This call chain runs backwards from most engines:
    // cg_modelanim.c positions the first-person model FIRST, then calls in here to derive the CAMERA
    // from its eyes bone (pREnt is that model, handed to R_AddRefEntityToScene right after we
    // return). Dropping only the camera leaves the gun behind and it climbs the screen as the eye
    // sinks; dropping both keeps the weapon at its usual screen position while the whole rig sits
    // lower in the world - which is the point.
    //
    // The vec3_origin guard mirrors the caller's own check at cg_modelanim.c:1967, where an
    // untouched model.origin of exactly zero is the sentinel for "fall back to s1->origin".
    if (s_dbnoCamEnv > 0.001f) {
        static cvar_t *pDbEye = NULL;
        float          drop;

        if (!pDbEye) { pDbEye = cgi.Cvar_Get("cg_dbnoEyeDrop", "50", CVAR_ARCHIVE); }
        drop = pDbEye->value * s_dbnoCamEnv;

        // [user 07-29] DO NOT let the cosmetic drop carry the eye through a surface. The underwater
        // detection lives in CG_CalcFov, which runs at the END of CG_CalcViewValues - i.e. BEFORE
        // CG_AddPacketEntities, and therefore before this drop is applied. So it tests the UNDROPPED
        // eye: push the camera under the waterline here and the engine has already concluded you are
        // dry. No FOV warp, no r_ppUnderwater, no fog - and MOHAA water is one-sided, so you get a
        // clean impossible view of the seabed while lying on the beach.
        //
        // Re-running the detection after the drop would be worse: it would slam the full underwater
        // warp over the screen while the player is lying in dry sand. The drop is a COSMETIC offset,
        // so the honest fix is that it must not leave the volume the player is actually in. Trace it
        // and stop at whatever it would have crossed - water surface or floor.
        if (drop > 0.0f) {
            trace_t tr;
            vec3_t  from, to;

            VectorCopy(origin, from);
            VectorCopy(origin, to);
            to[2] -= drop;

            CG_Trace(&tr, from, vec3_origin, vec3_origin, to, cg.snap->ps.clientNum,
                     MASK_CAMERASOLID, qfalse, qfalse, "DbnoEyeDrop");
            if (tr.fraction < 1.0f) {
                // back off a hair so the eye sits just short of the plane, never coplanar with it
                drop *= tr.fraction;
                drop -= 2.0f;
                if (drop < 0.0f) { drop = 0.0f; }
            }
        }

        origin[2] -= drop;
        if (pREnt && !VectorCompare(pREnt->origin, vec3_origin)) {
            pREnt->origin[2] -= drop;
            // [user 2026-08-21] "you broke the dbno cam... revert it back". EXEMPT from the feel
            // budget. That clamp bounds the summed jitter layers to 9u, but this is an authored
            // 50u pose and the CAMERA above takes the full drop unconditionally - so clamping only
            // the weapon half made the gun climb the screen as the eye sank, which is precisely the
            // regression the note further up this file records as already fixed once. The budget
            // cannot tell an authored stow from jitter; it has to be told.
            s_vFeelExempt[2] -= drop;
        }
    }

    // reload feel is applied HERE, not at the CG_CalcViewValues tail. The camera pitch is
    // baked into the ARMS bone controller earlier in the frame, so a write at the tail is
    // picked up by the gun and the IRON SIGHTS follow the lie; a write here reaches only
    // the camera, because the view axis is rebuilt from these angles right after we return
    // (the same route the scope sway already uses). This site is also reachable only in
    // live first person, alive and non-cutscene, so it inherits those gates instead of
    // hand-written ones that would have to stay in lockstep with two 3P deciders.
    if (!bUseWorldPosition && cgi.Cvar_Get("coop_reloadHook", "1", CVAR_ARCHIVE)->integer) {
        CG_ApplyReloadFeel(cg.refdefViewAngles);
    }

    // [2026-08-21] THE FEEL BUDGET, part 2 of 2. Clamp the ACCUMULATED feel offset once, here,
    // after every layer has had its say. Two separate ceilings, because they fail differently:
    //   * the component TOWARD THE EYE (-mat[0]) is what pushes the weapon through the near clip
    //     plane and out the back of the camera, so it gets the tighter bound;
    //   * total magnitude is bounded too, so the weapon cannot be flung out of frame sideways.
    // Measured worst case before this: ~14 units back and ~16 down in ordinary play (sprint into a
    // wall, land, crouch, hurt), ~25 back with the medkit stow also running.
    // [2026-08-21] publish the final viewmodel origin for the ADS trace. The first trace pass
    // measured the CAMERA and found it perfectly still (dPos 0.00 on every frame) while the user
    // still saw a jolt - because the complaint is about the GUN, which is this entity, not the eye.
    // HZM coop [user 2026-08-21] VIEWMODEL ANTI-POP - the actual "ADS jolt", finally measured.
    //
    // A live trace (coop_adsTrace) settled a bug that survived nine reasoned fixes. Every single ADS
    // entry produced this, on the exact frame the viewmodel animation index changed from idle to the
    // ADS pose:
    //     pose=0.610  vmanim=1  z=-21.98  dVM=0.001
    //     pose=0.856  vmanim=2  z=+23.35  dVM=45.533   <- one frame
    // Three entries, three jumps: 45.5, 40.3 and 42.4 units. The CAMERA meanwhile never moved at all
    // (dPos 0.00 on every frame), and the pose factor decayed as a clean exponential - which is
    // exactly why nothing aimed at the camera, the 3P flip, the zoom clock or the crossblend ever
    // touched it. The two animations simply place the weapon at origins ~40 units apart, and the
    // crossblend interpolates the POSE while the origin snaps.
    //
    // Fixed generically rather than per-animation, because NOTHING in this system legitimately moves
    // the weapon 40 units in one frame. Every authored large offset - the medkit stow at 26u, the
    // weapon-collision retract, the DBNO drop at 50u - is EASED, so its per-frame delta is small. A
    // single-frame step past the threshold is by definition a discontinuity, so absorb it into an
    // offset and bleed that off over ~120ms. Small deltas pass through completely untouched, so this
    // cannot flatten any motion that was already smooth.
    //
    // Runs before the feel-budget capture below, so the absorbed offset is not itself clamped and
    // cannot steal amplitude from the landing dip or weapon lag.
    if (pREnt) {
        static cvar_t *pPop = NULL, *pPopRate = NULL;
        static vec3_t  s_vPopPrev = {0, 0, 0};
        static vec3_t  s_vPopOfs  = {0, 0, 0};
        static int     s_iPopLast = 0;

        // [user 2026-08-21] DEFAULTED OFF after testing: "vmantipop was how it was before" - i.e. with
        // it ON the transition was WORSE. Smoothing a 45-unit step does not remove it, it converts a
        // one-frame pop into a ~110ms SLIDE of the whole weapon, which is more visible, not less.
        // The jump is a DATA problem (two authorings of the STG44 aim pose with roots ~45u apart,
        // see fps_anims_mg.txt mp44_charge), and it is fixed there. Kept as a tunable because the
        // filter itself is sound for a genuine one-frame discontinuity - just not for this one.
        if (!pPop)     { pPop     = cgi.Cvar_Get("coop_vmAntiPop", "0", CVAR_ARCHIVE); }
        if (!pPopRate) { pPopRate = cgi.Cvar_Get("coop_vmAntiPopRate", "9.0", CVAR_ARCHIVE); }

        if (pPop->value > 0.0f && s_iPopLast && (cg.time - s_iPopLast) < 250 && cg.frametime > 0) {
            vec3_t vStep;
            float  fStep;

            VectorSubtract(pREnt->origin, s_vPopPrev, vStep);
            fStep = VectorLength(vStep);
            if (fStep > pPop->value) {
                // absorb the WHOLE step: the gun stays where it was this frame, then catches up
                VectorAdd(s_vPopOfs, vStep, s_vPopOfs);
            }
            {
                float k = (cg.frametime / 1000.0f) * pPopRate->value;
                if (k > 1.0f) { k = 1.0f; }
                VectorMA(s_vPopOfs, -k, s_vPopOfs, s_vPopOfs);
            }
        } else {
            VectorClear(s_vPopOfs);
        }
        VectorCopy(pREnt->origin, s_vPopPrev);   // track the RAW target, not the corrected one
        s_iPopLast = cg.time;
        VectorSubtract(pREnt->origin, s_vPopOfs, pREnt->origin);

        VectorCopy(pREnt->origin, s_vTraceVM);
        s_bTraceVMok = qtrue;

        // [user 2026-08-21] MEASURE WHERE THINGS ACTUALLY RENDER, not an intermediate.
        //
        // Every previous trace sampled an INPUT to the render - the camera origin, the entity
        // origin - and something downstream could compensate for either. That is how a confident
        // 45-unit measurement turned out not to be the thing on screen.
        //
        // TIKI_Orientation returns a bone's position AFTER the pose is applied, so with the entity
        // origin and axis it gives the real world position of that bone. ForceUpdatePose has already
        // run by the time this function is called (cg_modelanim.c hits it before the call site), so
        // the pose here is the current one.
        //
        // The reported symptom is RELATIVE - "my body jumps upwards but camera stays put" - so what
        // matters is each bone MINUS the eyes bone, which is where the camera sits. If that gap
        // steps on one frame, that is the jolt, and which bone it is says whether it is the torso,
        // the arms or the weapon.
        {
            static cvar_t *pTr = NULL;
            static int     s_iBoneTag[8] = {-2, -2, -2, -2, -2, -2, -2, -2};
            static int     s_iBoneTiki   = 0;
            // [2026-08-21] The user says SHOULDERS. Earlier probes sampled spine, hand and gun and never
                // a shoulder bone at all, which is a gap rather than a result. Clavicle and upper arm
                // are the shoulders; Spine2 is the upper torso they hang off.
                const char    *kBones[8] = {"eyes bone", "Bip01 Spine1", "Bip01 R Hand", "tag_weapon_right",
                                            "Bip01 L Clavicle", "Bip01 L UpperArm",
                                            "Bip01 R Clavicle", "Bip01 R UpperArm"};
            int            b;

            if (!pTr) { pTr = cgi.Cvar_Get("coop_adsTrace", "0", 0); }
            if (pTr->integer && pREnt->tiki) {
                if (s_iBoneTiki != (int)(size_t)pREnt->tiki) {
                    for (b = 0; b < 8; b++) {
                        s_iBoneTag[b] = cgi.Tag_NumForName(pREnt->tiki, kBones[b]);
                    }
                    s_iBoneTiki = (int)(size_t)pREnt->tiki;
                }
                for (b = 0; b < 8; b++) {
                    VectorClear(s_vTraceBone[b]);
                    if (s_iBoneTag[b] >= 0) {
                        orientation_t o = cgi.TIKI_Orientation(pREnt, s_iBoneTag[b]);
                        int           r;

                        VectorCopy(pREnt->origin, s_vTraceBone[b]);
                        for (r = 0; r < 3; r++) {
                            VectorMA(s_vTraceBone[b], o.origin[r], pREnt->axis[r], s_vTraceBone[b]);
                        }
                    }
                }
                s_bTraceBone = qtrue;

                {
                    static int s_iDumped = 0;
                    if (s_iDumped != (int)(size_t)pREnt->tiki) {
                        int k;
                        s_iDumped = (int)(size_t)pREnt->tiki;
                        for (k = 0; k < NUM_BONE_CONTROLLERS; k++) {
                            int         tg   = pREnt->bone_tag ? pREnt->bone_tag[k] : -1;
                            const char *nm   = (tg >= 0) ? cgi.Tag_NameForNum(pREnt->tiki, tg) : "<unset>";
                            cgi.Printf("^~^~^ ADSSLOT %d tag=%d fpsBone='%s'\n",
                                       k, tg, nm ? nm : "<null>");
                        }
                    }
                }
            }
        }
    }
    if (pREnt && s_bFeelBase) {
        vec3_t vFeel;
        float  back, len;

        VectorSubtract(pREnt->origin, s_vFeelBase, vFeel);
        // subtract the AUTHORED stows before clamping, add them back after. Without this the
        // budget truncated the medkit stow, the weapon-collision retract and the DBNO eye drop -
        // three deliberate poses whose whole job is to be large.
        VectorSubtract(vFeel, s_vFeelExempt, vFeel);
        back = -DotProduct(vFeel, mat[0]); // positive = toward the eye
        if (back > 4.0f) {
            VectorMA(vFeel, back - 4.0f, mat[0], vFeel); // give back the excess only
        }
        len = VectorLength(vFeel);
        if (len > 9.0f && len > 0.0001f) {
            VectorScale(vFeel, 9.0f / len, vFeel);
        }
        VectorAdd(vFeel, s_vFeelExempt, vFeel);
        VectorAdd(s_vFeelBase, vFeel, pREnt->origin);
        s_bFeelBase = qfalse;
    }
    if (s_bRollBase) {
        float dRoll = cg.refdefViewAngles[2] - s_fRollBase;
        if (dRoll > 6.0f) {
            cg.refdefViewAngles[2] = s_fRollBase + 6.0f;
        } else if (dRoll < -6.0f) {
            cg.refdefViewAngles[2] = s_fRollBase - 6.0f;
        }
        s_bRollBase = qfalse;
    }

    // HZM coop [user 2026-08-21] CAMERA MOTION - "momentum and acceleration to movement would make
    // it feel less like a moving camera... what else for weightiness and not feeling like a moving
    // camera?" These are the CAMERA half. They do not touch the aim ray: bullets leave along
    // ps->viewangles, and only the roll term writes refdefViewAngles (roll does not steer a bullet).
    if (!bUseWorldPosition && CoopCamMotion() > 0.0f && cg.snap
        && cg.snap->ps.stats[STAT_HEALTH] > 0
        && !(cg.snap->ps.pm_flags & (PMF_SPECTATING | PMF_INTERMISSION | PMF_CAMERA_VIEW))) {
        static float  s_camLand   = 0.0f;   // landing absorb, units, eased
        static float  s_camLandV  = 0.0f;   // its velocity, so it recovers like a knee not a spring
        static float  s_camRoll   = 0.0f;   // strafe/turn bank, degrees, eased
        static float  s_stepPh    = 0.0f;   // INTEGRATED walk phase
        static float  s_lastVelZ2 = 0.0f;
        static int    s_lastGnd2  = 1;
        static float  s_lastYaw   = 0.0f;
        static float  s_turnLag   = 0.0f;
        vec3_t        camOfs;
        float         dt, spd, fwd, side, tgtRoll, k, yawDelta;
        int           bGnd;
        vec3_t        vRight, vFwd;
        float         scale = CoopCamMotion();

        static int s_camLastFrame = 0;

        dt = cg.frametime / 1000.0f;
        if (dt > 0.1f) {
            dt = 0.1f; // a hitch must never teleport an integrator (TRAPS)
        }
        VectorClear(camOfs);

        // RE-SEED after any frame this block did not run (dead, third person, spectating,
        // cutscene). Every static below only advances HERE, so without this the whole skipped
        // interval arrives as one frame delta - die while airborne and s_lastGnd2 = 0 with
        // s_lastVelZ2 ~ -700 survives into the respawn, firing a full-strength landing slam on
        // every such respawn. Exactly the bug the crouch-weight guard was added for.
        if (cg.time - s_camLastFrame > 250) {
            s_lastGnd2  = (cg.predicted_player_state.groundEntityNum != ENTITYNUM_NONE);
            s_lastVelZ2 = cg.predicted_player_state.velocity[2];
            s_lastYaw   = cg.refdefViewAngles[1];
            s_camLand   = 0.0f;
            s_camLandV  = 0.0f;
            s_camRoll   = 0.0f;
        }
        s_camLastFrame = cg.time;

        // A glued vehicle rider INHERITS the vehicle velocity and the vehicle IS their ground
        // entity, so a walking bob would run at vehicle speed - the "truck steps forward in little
        // jumps" stutter this file already guards the stock bob against. Turrets likewise: the
        // server owns that camera.
        if ((cg.snap->ps.pm_flags & (PMF_NO_MOVE | PMF_TURRET))
            || !cg.predicted_player_state.walking) {
            s_lastGnd2  = (cg.predicted_player_state.groundEntityNum != ENTITYNUM_NONE);
            s_lastVelZ2 = cg.predicted_player_state.velocity[2];
            s_lastYaw   = cg.refdefViewAngles[1];
        }

        bGnd = (cg.predicted_player_state.groundEntityNum != ENTITYNUM_NONE);
        spd  = (float)sqrt(cg.predicted_player_state.velocity[0] * cg.predicted_player_state.velocity[0]
                           + cg.predicted_player_state.velocity[1] * cg.predicted_player_state.velocity[1]);

        // ---- LANDING ABSORB -------------------------------------------------------------------
        // The weapon already dips on landing; the camera did not flinch at all, which is most of why
        // a hard fall reads as the floor moving rather than the body arriving. Modelled as a damped
        // spring so it COMPRESSES and recovers (a knee) instead of easing linearly (a lift).
        // [2026-08-21] READ THE SHARED LATCH, do not re-detect. This block used to run its own copy
        // of the same test, but it also force-refreshes s_lastGnd2/s_lastVelZ2 whenever !walking
        // (above) - and airborne IS !walking, so if `walking` had not yet flipped true on the
        // touchdown frame its edge was destroyed and it missed landings the weapon dip caught. One
        // detector, one number, same frame for all three consumers.
        //
        // TIER: 1.0 / 1.45 / 2.0 on the impulse. Note the ceiling that actually binds is
        // CoopCamClamp(camOfs, 4.5) below, NOT the +-6 state clamp - raising that state clamp is a
        // no-op, which is why the hard tier also lifts the clamp for the duration of the landing
        // (see the CoopCamClamp call site) rather than just asking for a bigger number here.
        CoopLandingDetect();
        if (s_landTime == cg.time && s_landSev > 0.0f) {
            static const float kTierKick[3] = {1.0f, 1.45f, 2.0f};
            s_camLandV -= s_landSev * 62.0f * scale * kTierKick[s_landTier];
        }
        s_lastGnd2  = bGnd;
        s_lastVelZ2 = cg.predicted_player_state.velocity[2];
        if (s_camLand != 0.0f || s_camLandV != 0.0f) {
            // dt is clamped to 0.1, but an EXPLICIT damping multiplier (1 - 11*dt) goes negative
            // at dt > 1/11 and the 2x2 update matrix picks up an eigenvalue above 1 - it grows
            // ~13% per frame while the frame rate stays under ~11 FPS, then whips when it
            // recovers. An exponential decay is unconditionally stable at any dt.
            float dtS = (dt > 0.033f) ? 0.033f : dt;
            s_camLandV += (-s_camLand * 145.0f) * dtS;
            s_camLandV *= (float)exp(-11.0f * dtS);
            s_camLand  += s_camLandV * dtS;
            // the integrator STATE needs its own bound - CoopCamClamp only bounds the output
            if (s_camLand < -6.0f)  { s_camLand = -6.0f; }
            else if (s_camLand > 6.0f) { s_camLand = 6.0f; }
            if (s_camLandV < -120.0f) { s_camLandV = -120.0f; }
            else if (s_camLandV > 120.0f) { s_camLandV = 120.0f; }
            if (s_camLand > -0.01f && s_camLand < 0.01f && s_camLandV > -0.5f && s_camLandV < 0.5f) {
                s_camLand = s_camLandV = 0.0f;
            }
            camOfs[2] += s_camLand;

            // HZM coop [user 2026-08-21] VAULT - the BODY half, paired with the hands half on the
            // viewmodel. The player is already rising ballistically from the server's velocity
            // assignment, so ADDING lift here would just make it floatier - the opposite of the
            // complaint. Instead the eye LAGS the rise: hold the camera slightly below where the
            // ballistic arc has already put it, then let it catch up. That is what hauling your own
            // weight over something feels like, as against being levitated over it.
            //
            // Translation only, deliberately. A pitch or roll flourish here would look good and lie:
            // any angular layer on refdefViewAngles moves the crosshair off the true aim by about
            // 20 pixels per degree, and a vault ends with you looking at whatever is on the far side
            // of the obstacle - the exact moment the reticle must be honest.
            //
            // Small on purpose: this goes through CoopCamClamp with every other camera term, and the
            // vault frequently ends in a landing, so it must leave room for the landing tier rather
            // than eating the whole budget just before one.
            camOfs[2] -= CoopVaultEnv() * 2.6f;
        }

        // ---- CROUCH / STAND, ON THE CAMERA -----------------------------------------------------
        // [user 2026-08-21] "When you crouch and stand up the hands/gun moves like you are, but it
        // doesnt feel like the body is actually standing up or crouching."
        //
        // Exactly right: coop_crouchWeight moved the WEAPON, and the engine slides the eye height,
        // but the eye slide is a clean interpolation with no overshoot - so it reads as a camera
        // being lowered on a rail rather than a body folding at the knees. Drive a small spring off
        // the RATE of the crouch blend: going down the camera keeps going a little past the target
        // and settles back up, coming up it lags and then catches. That overshoot is the whole tell.
        {
            static float s_camCrPrev = 0.0f;
            static float s_camCrVel  = 0.0f;
            static qboolean s_camCrInit = qfalse;
            float cbC = CG_AdsCrouchBlend();
            float dC;
            if (!s_camCrInit) { s_camCrInit = qtrue; s_camCrPrev = cbC; }
            dC = cbC - s_camCrPrev;
            if (dC > 0.2f) { dC = 0.2f; } else if (dC < -0.2f) { dC = -0.2f; }
            s_camCrPrev = cbC;
            s_camCrVel += dC * 34.0f * scale;
            if (s_camCrVel > 3.5f) { s_camCrVel = 3.5f; }
            else if (s_camCrVel < -3.5f) { s_camCrVel = -3.5f; }
            if (s_camCrVel > 0.001f || s_camCrVel < -0.001f) {
                camOfs[2] -= s_camCrVel;
                s_camCrVel -= s_camCrVel * dt * 7.5f;
                if (s_camCrVel < 0.002f && s_camCrVel > -0.002f) { s_camCrVel = 0.0f; }
            }
        }

        // ---- STRAFE / TURN BANK ---------------------------------------------------------------
        // The biggest cosmetic win available, and this engine has none at all: a body leans into
        // lateral movement and into a hard turn. Roll only - it tilts the horizon, it cannot steer a
        // bullet, and it is the one angular channel that is honest about that.
        AngleVectors(cg.refdefViewAngles, vFwd, vRight, NULL);
        side = DotProduct(cg.predicted_player_state.velocity, vRight);
        fwd  = DotProduct(cg.predicted_player_state.velocity, vFwd);
        (void)fwd;
        yawDelta = AngleSubtract(cg.refdefViewAngles[1], s_lastYaw);
        s_lastYaw = cg.refdefViewAngles[1];
        if (dt > 0.0001f) {
            yawDelta /= dt; // degrees per second
        }
        if (yawDelta > 220.0f) { yawDelta = 220.0f; }
        else if (yawDelta < -220.0f) { yawDelta = -220.0f; }

        tgtRoll = 0.0f;
        if (bGnd && cg.predicted_player_state.walking
            && !(cg.snap->ps.pm_flags & (PMF_NO_MOVE | PMF_TURRET))) {
            tgtRoll += (side / 300.0f) * 1.5f;      // lean into a strafe
        }
        tgtRoll += (yawDelta / 220.0f) * 0.9f;      // and into a hard turn
        tgtRoll *= scale;
        if (CG_AimingDownSights() || cg.snap->ps.stats[STAT_INZOOM]) {
            tgtRoll *= 0.25f; // damped under sights, like every other perturbation
        }
        k = dt * 7.0f;
        if (k > 1.0f) {
            k = 1.0f; // two-sided ease: MUST clamp, it has no floor to rescue an overshoot
        }
        s_camRoll += (tgtRoll - s_camRoll) * k;
        if (s_camRoll > 3.5f) { s_camRoll = 3.5f; }
        else if (s_camRoll < -3.5f) { s_camRoll = -3.5f; }

        // ---- FOOTSTEP-SHAPED BOB ---------------------------------------------------------------
        // Not a sine. A walking gait is a fast DROP as the foot takes the load and a slower rise as
        // it unloads, so a symmetric sine reads as floating no matter how it is tuned. Phase is
        // INTEGRATED (never time x frequency - that bug shipped once already, see TRAPS), so the
        // cadence can change with speed without the pose ever jumping.
        //
        // NOTE: this is gait-SHAPED, not synchronised to the footstep SOUND. Those are emitted by the
        // legs animation server-side, and there is no client hook to phase-lock to. Matching the
        // shape gets most of the read; true sync would need a footstep event on the wire.
        if (bGnd && spd > 40.0f && !(cg.snap->ps.stats[STAT_INZOOM])
            && cg.predicted_player_state.walking
            && !(cg.snap->ps.pm_flags & (PMF_NO_MOVE | PMF_TURRET))) {
            float cyc, drop, amp;
            s_stepPh += dt * (1.35f + spd * 0.0042f);
            if (s_stepPh > 10000.0f) {
                s_stepPh -= 10000.0f;
            }
            cyc = s_stepPh - (float)floor(s_stepPh); // 0..1 within a stride
            // fast fall over the first 35% of the stride, slow recovery over the rest
            if (cyc < 0.35f) {
                drop = -(cyc / 0.35f);
            } else {
                drop = -(1.0f - ((cyc - 0.35f) / 0.65f));
            }
            amp = (spd / 300.0f) * 1.5f * scale;
            if (amp > 2.2f) {
                amp = 2.2f;
            }
            if (CG_AimingDownSights()) {
                amp *= 0.35f;
            }
            camOfs[2] += drop * amp;
        }

        // ---- TURN INERTIA ----------------------------------------------------------------------
        // Default OFF. This is the one term that can read as INPUT LATENCY, which is the cardinal
        // sin in a shooter, so it ships disabled and opt-in rather than tuned-down-and-on.
        {
            static cvar_t *pTI = NULL;
            if (!pTI) {
                pTI = cgi.Cvar_Get("coop_camTurnInertia", "0", CVAR_ARCHIVE);
            }
            if (pTI->value > 0.0f) {
                float want = (yawDelta / 220.0f) * 0.8f * pTI->value * scale;
                float kk   = dt * 14.0f;
                if (kk > 1.0f) {
                    kk = 1.0f;
                }
                s_turnLag += (want - s_turnLag) * kk;
                if (s_turnLag > 1.2f) { s_turnLag = 1.2f; }
                else if (s_turnLag < -1.2f) { s_turnLag = -1.2f; }
                VectorMA(camOfs, -s_turnLag, vRight, camOfs);
            } else {
                s_turnLag = 0.0f;
            }
        }

        // ---- THE CEILING -----------------------------------------------------------------------
        // [2026-08-21] LANDING TIER HEADROOM. 4.5 is the ceiling on the SUM of every camera term
        // (crouch, bob, roll-bank, turn lag, landing), and it is what actually binds - the landing
        // spring's own +-6 state clamp never gets to matter. So a hard landing has to be given room
        // here or it cannot be deeper than a light one no matter what impulse it is handed.
        //
        // Lifted only for the ~450ms a landing is live, and only by the tier: light landings keep
        // the normal ceiling exactly. It is a temporary widening of a shared budget rather than a
        // permanent raise, so the other camera terms are unaffected outside the landing window.
        {
            float fClampMax = 4.5f;
            int   iTier     = CoopLandingTier(450);

            if (iTier == 1) {
                fClampMax = 5.6f;
            } else if (iTier == 2) {
                fClampMax = 7.2f;
            }
            CoopCamClamp(camOfs, fClampMax);
        }
        // TRACED. This runs after the MASK_PLAYERSOLID height/lateral traces earlier in the
        // function, so an unclipped offset could punch the eye through a floor, a low ceiling or
        // a waterline - the rule this file states for its own dip block and which the DBNO eye
        // drop already obeys. Scale by the hit fraction and hold off the surface.
        if (camOfs[0] != 0.0f || camOfs[1] != 0.0f || camOfs[2] != 0.0f) {
            trace_t trCam;
            vec3_t  vCamEnd;
            VectorAdd(origin, camOfs, vCamEnd);
            CG_Trace(&trCam, origin, vec3_origin, vec3_origin, vCamEnd, cg.snap->ps.clientNum,
                     MASK_CAMERASOLID, qfalse, qtrue, "CamMotion");
            if (trCam.fraction < 1.0f) {
                float f = trCam.fraction - 0.15f;
                if (f < 0.0f) {
                    f = 0.0f;
                }
                VectorScale(camOfs, f, camOfs);
            }
        }
        // [2026-08-21] CARRY THE EXTRA LANDING DEPTH TO THE GUN TOO, and exempt it.
        //
        // camOfs moves the CAMERA only. That is fine at the normal 4.5 ceiling because the effect is
        // small, but a hard-landing dip that is genuinely deep would sink the eye while the weapon
        // stayed put - so the gun climbs the screen exactly as it did in the DBNO eye-drop
        // regression. Hand the weapon the same vertical drop, and mirror it into s_vFeelExempt so
        // the 9u/4u jitter budget treats it as an authored pose rather than clamping it and
        // desynchronising the two halves again (that is what broke three shipped features once).
        //
        // Only the portion BEYOND the normal ceiling is transferred: the everyday camera motion
        // (bob, crouch, turn lag) should keep moving the eye relative to the gun, which is what
        // makes it read as a head rather than a tripod.
        if (pREnt && camOfs[2] < 0.0f) {
            float fExtra = -camOfs[2] - 4.5f;

            if (fExtra > 0.0f) {
                pREnt->origin[2] -= fExtra;
                s_vFeelExempt[2] -= fExtra;
            }
        }
        VectorAdd(origin, camOfs, origin);
        cg.refdefViewAngles[2] += s_camRoll;
    }

    VectorCopy(origin, cg.playerHeadPos);
}

/*
====================
CG_CalcFov

Fixed fov at intermissions, otherwise account for fov variable and zooms.
====================
*/
#define WAVE_AMPLITUDE 1
#define WAVE_FREQUENCY 0.4

/*
====================
CG_AimingDownSights

HZM coop - single source of truth for "the player is aiming down the iron sights": holding the
secondary-attack button (RMB), alive, NOT in a native scope/zoom (snipers/binoculars use STAT_INZOOM),
and not in a camera view. Used by the ADS zoom (CG_CalcFov) and the third->first person ADS switch.
====================
*/
// [vet, bug-2141] ONE definition of the hold budget. The weight penalty was written into
// CG_GetBreathState, which has no callers anywhere in the tree, while the mechanic that actually
// runs kept its own unmodified copy - so a Panzerschreck held its breath exactly as long as a Luger
// and the feature was entirely inert. Both sites now call this.
static int CG_BreathHoldMs(void)
{
    static cvar_t *pH = NULL;
    int            ms;

    if (!pH) { pH = cgi.Cvar_Get("cg_breathHoldTime", "7", CVAR_ARCHIVE); }
    ms = (int)(pH->value * 1000.0f * (1.0f - CoopGunHeft() * 0.35f));
    return (ms < 100) ? 100 : ms;
}

// [weight 6, vet bug-2142] MUZZLE DROOP - the ANGLE lives here, the rotation is applied to the
// WEAPON entity in cg_modelanim.c.
//
// The first cut rotated pREnt->axis in CG_OffsetFirstPersonView, which looked right and was wrong:
// that entity is the first-person PLAYER model and its origin is at the FEET, so rotating its axis
// swung the whole rig - arms, weapon and every tag-attached prop - through an arc on a ~60 unit
// lever. The dominant result was a parasitic forward shove roughly twice the size of the drop it was
// trying to produce, and because it never touched pREnt->origin the feel budget could not see it.
// This function's own history records the identical failure for the idle inspect thirty lines away.
//
// Rotation about the GRIP is what a drooping muzzle is, and the weapon entity's origin already sits
// at tag_weapon_right - so the fix is to rotate there, beside the per-gun ADS tune, where the pivot
// is correct by construction and costs no compensation at all.
float CG_CoopDroopAngle(void)
{
    static cvar_t *pDroop = NULL, *pDroopMove = NULL;
    static float   s_droop = 0.0f;
    static int     s_last  = -1;
    float          fTgt, fSpd, fDt;

    if (!pDroop) {
        pDroop     = cgi.Cvar_Get("coop_droop", "2.6", CVAR_ARCHIVE);     // degrees at full weight
        pDroopMove = cgi.Cvar_Get("coop_droopMove", "1.8", CVAR_ARCHIVE); // extra while moving
    }
    if (s_last == cg.time) {
        return s_droop;   // once per frame, not once per caller
    }
    s_last = cg.time;

    fSpd = 0.0f;
    if (cg.snap) {
        fSpd = (float)sqrt((double)(cg.snap->ps.velocity[0] * cg.snap->ps.velocity[0]
                                  + cg.snap->ps.velocity[1] * cg.snap->ps.velocity[1]));
    }
    fTgt = CoopGunHeft() * (pDroop->value + pDroopMove->value * (fSpd > 200.0f ? 1.0f : fSpd / 200.0f));
    // ADS cancels it outright: the sights are up because the player is deliberately holding them
    // there, and fighting their aim would be the one unforgivable version of this.
    fTgt *= (1.0f - CG_AdsPoseFactor());

    fDt = (cg.frametime > 0) ? ((float)cg.frametime / 1000.0f) : 0.0f;
    fDt *= 4.0f;
    if (fDt > 1.0f) { fDt = 1.0f; }
    s_droop += (fTgt - s_droop) * fDt;
    return s_droop;
}

qboolean CG_AimingDownSights(void)
{
    usercmd_t cmd;

    if (!cg.snap || cg.snap->ps.stats[STAT_HEALTH] <= 0) {
        return qfalse;
    }
    if (cg.snap->ps.stats[STAT_INZOOM]) {
        return qfalse;
    }
    if (cg.snap->ps.pm_flags & PMF_CAMERA_VIEW) {
        return qfalse;
    }

    // [user 2026-08-27] MOUNTING FORCES ADS - "we hit Use to mount when we see the icon, that forces
    // you in very smooth ads". Returning true here routes the mount through the WHOLE existing ADS
    // pipeline rather than a parallel one, so the pose, FOV and framing all ease in on the envelope
    // that is already tuned: the smoothness is inherited, not re-implemented.
    // [user 2026-08-27] MOUNTING IS AIMING. The user's framing: "have the mounting system just force
    // ADS on the weapon using what is tuned" - so the mount routes through the EXISTING per-gun ADS
    // pipeline rather than a parallel pose, and inherits the sight picture, FOV and easing already
    // dialled in for every weapon. Nothing new to tune, and the transition is smooth for free.
    //
    // This is not the earlier dead-button problem: back then the mount held ADS true forever with no
    // way out. Now Use unmounts, so coming out of the aim is one keypress and the button is live
    // again the moment you do.
    // [bug 2026-08-28] Key on the BINARY mount flag, not on the eased envelope crossing 0.5. Testing
    // the envelope meant the ADS pose only started once the brace was half-risen and then eased again
    // on its own curve - three serial eases, so the FOV arrived long before the sight picture and the
    // mount read as a zoom rather than an aim. The flag flips the instant you mount; the existing
    // per-gun ADS envelope supplies all the smoothing, which is the whole point of routing through it.
    {
        static cvar_t *pMnt = NULL;
        if (!pMnt) { pMnt = cgi.Cvar_Get("coop_braceMounted", "0", 0); }
        if (pMnt->integer) {
            return qtrue;
        }
    }
    cgi.GetUserCmd(cgi.GetCurrentCmdNumber(), &cmd);
    return (cmd.buttons & BUTTON_COOPADS) ? qtrue : qfalse; // ADS on its own button, decoupled from secondary-fire/bash
}

/*
====================
HZM coop - STAGED THIRD-PERSON ADS

For cg_3rd_person players, holding the ADS button no longer snaps straight to first person: it eases
the chase camera into an over-the-right-shoulder AIM view (stage 0). While still holding ADS, one
mouse-wheel-up notch (captured in CG_CheckCaptureKey, cg_ui.cpp - the weapnext/weapprev bind does NOT
run) sets cg_adsStage 1: the camera flies in to the head and the view flips to the full first-person
iron-sight ADS (all existing ADS behavior: world/gun fov, breath-hold, sway). Wheel-down while held
returns to the shoulder view symmetrically. Releasing ADS resets the stage and eases (fast) back to
the normal chase framing. First-person players (cg_3rd_person 0) keep today's instant behavior.
The stage lives in the cg_adsStage cvar (0 = shoulder, 1 = irons) - NO new usercmd button bits.
====================
*/
// HZM coop - SCOPED weapons (native zoom): the shoulder stage must never engage for them - the
// ADS button toggles the server zoom (player.cpp ToggleZoom on BUTTON_COOPADS), so 3P ADS goes
// STRAIGHT to the scope like it always did. Without this bypass the shoulder ease fought the
// zoom's forced first-person for a few frames on scope-in/out ("glitches"). List = every trilogy
// weapon TIK with zoom, plus our binocular-type items.
static qboolean CG_ActiveWeaponHasScope(void)
{
    const char *wpn;

    if (!cg.snap || cg.snap->ps.activeItems[1] < 0) {
        return qfalse;
    }
    wpn = CG_ConfigString(CS_WEAPONS + cg.snap->ps.activeItems[1]);
    if (!wpn || !wpn[0]) {
        return qfalse;
    }
    // [user 2026-08-21] VARIANT-SAFE - a scoped rifle SKIN is "<Base Gun> (<Finish>)" and would
    // otherwise not register as scoped at all, which silently re-enables the staged shoulder ADS on
    // a sniper (bug-256 forces it off for exactly these guns).
    {
        static char vb[64];
        if (CoopStripSkinSuffix(wpn, vb, sizeof(vb))) {
            wpn = vb;
        }
    }
    return (!Q_stricmp(wpn, "KAR98 - Sniper") || !Q_stricmp(wpn, "Springfield '03 Sniper")
            || !Q_stricmp(wpn, "Enfield L42A1") || !Q_stricmp(wpn, "SVT 40") || !Q_stricmp(wpn, "G 43")
            || !Q_stricmp(wpn, "FG 42") || !Q_stricmp(wpn, "Bombing Run") || strstr(wpn, "inocular") != NULL)
               ? qtrue
               : qfalse;
}

static qboolean CG_AdsStagedOn(void)
{
    // staged shoulder ADS is a third-person feature; cg_adsShoulder 0 restores the instant 3P->1P snap
    static cvar_t *pOn = NULL;
    if (!pOn) { pOn = cgi.Cvar_Get("cg_adsShoulder", "1", CVAR_ARCHIVE); }
    if (CG_ActiveWeaponHasScope()) {
        return qfalse; // snipers/scoped: straight to the scope, no shoulder stage
    }
    // [user 2026-08-21] "When behind cover and I hold right mouse I go straight into ADS now, it's
    // supposed to start in over shoulder with the scroll functionality to go in/out of ADS and
    // middle mouse to switch shoulders."
    //
    // Cover is a THIRD-PERSON state, but it is entered through PMF_COOP_COVER rather than by the
    // player setting cg_3rd_person - so this returned false while covered, CG_AdsForceFirstPerson
    // took its "no staged system in play -> instant first person" branch, and aiming from cover
    // snapped straight to the irons. Before the cover/ADS ordering fix that was masked, because the
    // cover force simply overrode the camera afterwards; fixing the ordering exposed it.
    //
    // Treating cover as a staged-ADS state is what makes the wheel work there too:
    // CG_AdsShoulderWheelActive is built on this same predicate, so the scroll capture follows.
    if (pOn->integer && cg.snap && (cg.snap->ps.pm_flags & PMF_COOP_COVER)) {
        return qtrue;
    }
    return (pOn->integer && cg_3rd_person->integer) ? qtrue : qfalse;
}

// HZM coop - the wheel capture (cg_ui.cpp CG_CheckCaptureKey) must use the exact same decision,
// so a scoped rifle keeps its normal wheel (weapon switch) while aiming.
qboolean CG_AdsShoulderWheelActive(void)
{
    return (CG_AdsStagedOn() && CG_AimingDownSights()) ? qtrue : qfalse;
}

static int CG_AdsStage(void)
{
    // 0 = over-the-shoulder aim, 1 = first-person irons. Written by the wheel capture + the reset below.
    static cvar_t *pStage = NULL;
    if (!pStage) { pStage = cgi.Cvar_Get("cg_adsStage", "0", 0); } // runtime state - deliberately NOT archived
    return pStage->integer;
}

// Once per frame from CG_CalcViewValues, BEFORE the third-person decision: advance both stage envelopes
// (same first-order ease as s_adsZoomCur below) and reset the stage when the ADS button is released.
static void CG_UpdateAdsStage(void)
{
    qboolean       bAds    = CG_AimingDownSights();
    qboolean       bStaged = CG_AdsStagedOn();
    float          fShoulderTgt, fFpTgt, dt, rate, step;
    static cvar_t *pSpeed = NULL;

    if (!pSpeed) { pSpeed = cgi.Cvar_Get("cg_adsShoulderSpeed", "10", CVAR_ARCHIVE); }

    // release resets the stage so every fresh ADS hold starts at the shoulder view
    if (!bAds && CG_AdsStage() != 0) {
        cgi.Cvar_Set("cg_adsStage", "0");
    }

    fShoulderTgt = (bAds && bStaged) ? 1.0f : 0.0f;
    fFpTgt       = (bAds && bStaged && CG_AdsStage() >= 1) ? 1.0f : 0.0f;

    dt   = (cg.frametime > 0) ? (float)cg.frametime / 1000.0f : 0.0f;
    rate = (pSpeed->value > 0.0f) ? pSpeed->value : 10.0f;

    // ease-OUT (release / wheel-down) runs 1.5x faster: "snap back with a fast ease, not a hard cut"
    step = dt * ((fShoulderTgt < s_adsShoulderEnv) ? rate * 1.5f : rate);
    if (step > 1.0f) { step = 1.0f; }
    s_adsShoulderEnv += (fShoulderTgt - s_adsShoulderEnv) * step;
    if (s_adsShoulderEnv > fShoulderTgt - 0.003f && s_adsShoulderEnv < fShoulderTgt + 0.003f) {
        s_adsShoulderEnv = fShoulderTgt; // settle
    }

    step = dt * ((fFpTgt < s_adsFpEnv) ? rate * 1.5f : rate);
    if (step > 1.0f) { step = 1.0f; }
    s_adsFpEnv += (fFpTgt - s_adsFpEnv) * step;
    if (s_adsFpEnv > fFpTgt - 0.003f && s_adsFpEnv < fFpTgt + 0.003f) {
        s_adsFpEnv = fFpTgt; // settle
    }

    // HZM coop - MOUSE3 SHOULDER SWAP: ease the side sign toward +1 (right) / -1 (left) per the
    // archived cg_adsShoulderRight cvar (toggled by MOUSE3 in CG_CheckCaptureKey while shouldered).
    // Slightly slower than the shoulder ease so the sweep across the back reads as a camera move,
    // not a cut. Runs unconditionally so a swap done mid-aim also settles while NOT aiming.
    {
        static cvar_t *pRight = NULL;
        float          fSignTgt;
        if (!pRight) { pRight = cgi.Cvar_Get("cg_adsShoulderRight", "1", CVAR_ARCHIVE); }
        fSignTgt = pRight->integer ? 1.0f : -1.0f;

        // HZM coop [user 2026-08-22, bug-2055 phase 2] COVER PROPOSES, THE PLAYER DISPOSES.
        // "the camera keeps going behind me instead of looking at me and the opening."
        // Nothing was ever wired: the shoulder side came only from the archived preference, so in
        // cover the camera sat on whichever shoulder the player normally uses regardless of which
        // way the opening faced. This is the override designed in wallcover_plan_v1 §5.4 and never
        // built - which is why fixing the side solver alone would NOT have fixed the camera, and
        // why this symptom would have come straight back.
        //
        // THREE RULES, each with a bug behind it:
        //  1. NEVER write cg_adsShoulderRight. It is CVAR_ARCHIVE and it is the player's standing
        //     preference; gameplay code silently rewriting a saved setting is its own defect
        //     ("a setting is a promise", 21-user-preferences.md). Override the TARGET, not the cvar.
        //  2. NEVER re-target mid-sweep. The arc term (fArc, ~line 742) bows the camera out behind
        //     the player during a shoulder sweep; starting a swap on top of an ADS fly-in composes
        //     two eases into one motion and reads as the warp bug-1992 was filed for. So the
        //     override only takes effect while both envelopes are at rest.
        //  3. An explicit MOUSE3 swap WINS for the rest of that cover session - the player's
        //     deliberate act always beats the automatic pick. The latch clears when cover drops,
        //     so the auto-pick resumes next time you take cover.
        //
        // coop_coverSide arrives as a change-only stufftext from player.cpp. It is integer-valued
        // with no embedded quote (T8.1) and the whole coop_ namespace is prefix-allowed by
        // cg_servercmds_filter.cpp:173, so the wire is sound - the same form coop_coverView already
        // uses successfully.
        {
            static cvar_t *pCovAuto = NULL, *pCovSide = NULL;
            static int     s_coverManual   = 0;   // player overrode the auto-pick this cover session
            static int     s_wasCovered    = 0;
            static float   s_lastSignTgt   = 0.0f;
            int            bCovered, iSide;

            if (!pCovAuto) { pCovAuto = cgi.Cvar_Get("coop_coverAutoShoulder", "1", CVAR_ARCHIVE); }
            if (!pCovSide) { pCovSide = cgi.Cvar_Get("coop_coverSide", "0", 0); }

            bCovered = (cg.snap && (cg.snap->ps.pm_flags & PMF_COOP_COVER)) ? 1 : 0;
            iSide    = pCovSide->integer;

            // leaving cover clears the manual latch, so the auto-pick is armed again next time
            if (!bCovered && s_wasCovered) {
                s_coverManual = 0;
            }
            // MOUSE3 while covered = a deliberate swap. Detect it as a change to the archived
            // preference rather than hooking the key, so it cannot fight cg_ui.cpp's key claim.
            if (bCovered && s_wasCovered && fSignTgt != s_lastSignTgt) {
                s_coverManual = 1;
            }
            s_wasCovered  = bCovered;
            s_lastSignTgt = fSignTgt;

            // [bug-2055 phase 2] PROVE THE WIRE. wallcover_plan_v1 §5.4 flagged coop_coverSide as
            // unverified on the client: it is stuffed change-only from the server and only the
            // predictor has ever read it, so nothing has ever confirmed it ARRIVES. Everything
            // above is dead code if it does not. Edge-triggered, so it costs one line per side
            // change and nothing while covered. Pairs with the server's COVERSIDE have= field:
            // server have=N with no matching client line here means the wire dropped it.
            {
                static int s_sideSeen = -99;

                if (iSide != s_sideSeen) {
                    s_sideSeen = iSide;
                    if (cgi.Cvar_Get("coop_coverProbe", "1", 0)->integer) {
                        cgi.Printf("^~^~^ COVERSIDE-CLIENT side=%d covered=%d manual=%d "
                                   "shEnv=%.2f fpEnv=%.2f\n",
                                   iSide, bCovered, s_coverManual, s_adsShoulderEnv, s_adsFpEnv);
                    }
                }
            }

            if (pCovAuto->integer && bCovered && !s_coverManual && iSide != 0
                && s_adsShoulderEnv <= 0.003f && s_adsFpEnv <= 0.003f) {
                // side +1 = opening on the LEFT  -> put the camera on the LEFT shoulder  (-1)
                // side -1 = opening on the RIGHT -> put the camera on the RIGHT shoulder (+1)
                fSignTgt = -(float)iSide;
            }
        }

        step     = dt * rate * 0.6f;
        if (step > 1.0f) { step = 1.0f; }
        s_shoulderSideSign += (fSignTgt - s_shoulderSideSign) * step;
        if (s_shoulderSideSign > fSignTgt - 0.003f && s_shoulderSideSign < fSignTgt + 0.003f) {
            s_shoulderSideSign = fSignTgt; // settle
        }
    }

    // HZM coop - mirror the shoulder-aim state to the SERVER via userinfo (u_shoulderaim): the stage is
    // a pure client concept (cvar + envelopes), but the aimed-walk movement slowdown must be applied by
    // the server (ClientMove). CVAR_USERINFO means the client engine auto-sends a reliable userinfo
    // update whenever the value changes (rare: ADS press/release in 3P + wheel stage flips).
    {
        static cvar_t *pMirror    = NULL;
        int            inShoulder = (bAds && bStaged && CG_AdsStage() == 0) ? 1 : 0;
        if (!pMirror) { pMirror = cgi.Cvar_Get("u_shoulderaim", "0", CVAR_USERINFO); }
        if (pMirror->integer != inShoulder) {
            cgi.Cvar_Set("u_shoulderaim", va("%d", inShoulder));
        }
    }
}

// HZM coop - shoulder-stage camera envelope (0 = normal chase, 1 = fully in the shoulder AIM framing).
// Exposed so 2D effects (ADS vignette/DoF in cg_drawtools.cpp) can follow the shoulder stage exactly.
float CG_AdsShoulderFrac(void)
{
    return s_adsShoulderEnv;
}

/*
====================
HZM coop - THIRD-PERSON FREE CAM (cg_freecam)

With cg_freecam 1 and cg_3rd_person 1, the mouse orbits the chase camera FREELY around the character
(full 360 yaw, pitch ~+/-85) WITHOUT turning the character: the model keeps its facing, WASD stays
relative to that frozen facing (the legs statemap plays the proper strafe/backpedal anims), and you can
fly the camera around to look at your soldier from the front. MOUSE OWNERSHIP: while eligible we publish
cg_freecamCapture 1 and the CLIENT (CL_MouseMove, cl_input.cpp) routes mouse deltas into the previously
vestigial camera_offset/camera_active look channel instead of cl.viewangles; the orbit is read back here
through the existing cgi.get_camera_offset() import and applied inside CG_OffsetThirdPersonView (so it
inherits the stock camera collision). Holding ADS drops the capture instantly - the mouse aims the player
again and the EXISTING staged shoulder-ADS takes over - while s_freecamEnv eases the applied orbit back
behind the shoulder at the same rate the shoulder framing rises (one camera gesture, no snap). Scoped
rifles, turrets, cutscene/statemap cameras, spectating, DBNO and death all drop the orbit the same way.
exe+cgame pair: ships with the matching client (CL_MouseMove capture routing).
====================
*/
static qboolean CG_FreecamEligible(void)
{
    static cvar_t *pOn = NULL, *pDbnoV = NULL;
    playerState_t *ps;

    if (!pOn)    { pOn    = cgi.Cvar_Get("cg_freecam", "0", CVAR_ARCHIVE); }
    if (!pDbnoV) { pDbnoV = cgi.Cvar_Get("coop_dbnoView", "0", 0); }

    if (!cg.snap) {
        return qfalse;
    }
    // HZM coop [232] - IN COVER the mouse aims DIRECTLY (no orbit capture). Historical: cover used
    // to force the orbit because the old sustain trace was view-dependent (bug-303); sustain has
    // been ANCHORED since cover v2, and since [226] the server view tracks the camera anyway -
    // layering the orbit offset on top of that composited aim DOUBLE-applied the pitch, so the
    // camera could wedge past vertical with the clamps fighting the mouse ("stuck looking up in
    // cover, mouse-down won't work" - user, bug-327). With no capture the camera simply chases
    // the live aim: crosshair always true, blindfire/peek aim exactly where you look, and every
    // pitch limit is the engine's own +/-85.
    if (cg.predicted_player_state.pm_flags & PMF_COOP_COVER) {
        return qfalse;
    }
    if (!pOn->integer || !cg_3rd_person->integer) {
        return qfalse;
    }
    ps = &cg.predicted_player_state;
    if (cg.snap->ps.stats[STAT_HEALTH] <= 0) {
        return qfalse; // dead / waiting to respawn: the mouse drives the normal death view
    }
    if (cg.snap->ps.stats[STAT_INZOOM]) {
        return qfalse; // native scope/binoculars force first person (reticle = true aim)
    }
    if (ps->pm_flags & (PMF_CAMERA_VIEW | PMF_TURRET | PMF_SPECTATING | PMF_INTERMISSION | PMF_FROZEN)) {
        return qfalse; // server-owned views: script/turret cameras, spectate, intermission, freeze
    }
    if (ps->camera_flags
        & (CF_CAMERA_ANGLES_ABSOLUTE | CF_CAMERA_ANGLES_IGNORE_YAW | CF_CAMERA_ANGLES_IGNORE_PITCH
           | CF_CAMERA_ANGLES_ALLOWOFFSET)) {
        return qfalse; // statemap camera types (CAMERA_FRONT/SIDE/TOPDOWN...) own the 3P framing
    }
    if (ps->camera_offset[YAW] != 0.0f || ps->camera_offset[PITCH] != 0.0f) {
        return qfalse; // ditto - the server is driving a seat/state look offset (vehicle looks etc.)
    }
    if (pDbnoV->integer) {
        return qfalse; // DBNO forces first person (bleed-out view)
    }
    if (CG_AimingDownSights()) {
        return qfalse; // ADS hold hands the mouse back for real aiming (shoulder/irons/scope, as today)
    }
    if (CG_AdsForceFirstPerson()) {
        return qfalse; // still first person (wheel-up irons fly-out after release): wait for the 3P view
    }
    return qtrue;
}

// Exposed for the crosshair (cg_drawtools.cpp): while the free orbit owns the mouse the camera direction
// is NOT the aim direction, so the crosshair (incl. the 3P true-aim projection) would lie - hide it.
qboolean CG_FreecamCaptureActive(void)
{
    return s_freecamCapture;
}

// Once per frame from CG_CalcViewValues (right after CG_UpdateAdsStage): decide the mouse capture, ease
// the orbit envelope, and retire the client orbit accumulator once it has fully eased out.
static void CG_UpdateFreecam(void)
{
    qboolean       bWant = CG_FreecamEligible();
    float          fTgt  = bWant ? 1.0f : 0.0f;
    float          dt, rate, step;
    static cvar_t *pSpeed = NULL, *pCapture = NULL;

    // same transition rate as the shoulder-ADS ease, so the ADS handoff (orbit swinging home while the
    // shoulder framing rises) reads as ONE camera gesture
    if (!pSpeed)   { pSpeed   = cgi.Cvar_Get("cg_adsShoulderSpeed", "10", CVAR_ARCHIVE); }
    if (!pCapture) { pCapture = cgi.Cvar_Get("cg_freecamCapture", "0", 0); } // runtime state - never archived

    // publish the capture flag for the client input layer (CL_MouseMove). Compared against the LIVE cvar
    // (not a cached bool) so it self-heals if anything else reset it (CG_Shutdown, a stray console set).
    s_freecamCapture = bWant;
    if (pCapture->integer != (bWant ? 1 : 0)) {
        cgi.Cvar_Set("cg_freecamCapture", bWant ? "1" : "0");
    }

    dt   = (cg.frametime > 0) ? (float)cg.frametime / 1000.0f : 0.0f;
    rate = (pSpeed->value > 0.0f) ? pSpeed->value : 10.0f;

    // ease-out (handoff/release) runs 1.5x faster, mirroring the shoulder envelope
    step = dt * ((fTgt < s_freecamEnv) ? rate * 1.5f : rate);
    if (step > 1.0f) { step = 1.0f; }
    s_freecamEnv += (fTgt - s_freecamEnv) * step;
    if (s_freecamEnv > fTgt - 0.003f && s_freecamEnv < fTgt + 0.003f) {
        s_freecamEnv = fTgt; // settle
    }

    // fully eased out and not wanted: zero the client orbit accumulator (through the same live pointer
    // the orbit reads) so the NEXT activation starts centred behind the character. No visual change at
    // this point - the applied offset is already accumulator * 0.
    if (!bWant && s_freecamEnv <= 0.0f) {
        float   *ofs;
        qboolean la, rv;
        ofs = cgi.get_camera_offset(&la, &rv);
        if (ofs[0] != 0.0f || ofs[1] != 0.0f || ofs[2] != 0.0f) {
            VectorClear(ofs);
        }
    }
}

/*
====================
CG_AdsForceFirstPerson

HZM coop - "the ADS system wants a FIRST-person view this frame". Replaces the raw CG_AimingDownSights()
term in BOTH third-person deciders (cg.renderingThirdPerson in CG_CalcViewValues AND bThirdPerson in
cg_modelanim.c CG_ModelAnim) so camera and own-model draw stay in lockstep. Without the staged system
(first-person players / cg_adsShoulder 0) this is exactly CG_AimingDownSights() = today's behavior;
with it, first person engages only once the wheel-up fly-in envelope crosses cg_adsFpFlip (and eases
back out through the same threshold, so release/wheel-down leaves first person smoothly too).
====================
*/
qboolean CG_AdsForceFirstPerson(void)
{
    static cvar_t *pFlip = NULL;
    if (!pFlip) { pFlip = cgi.Cvar_Get("cg_adsFpFlip", "0.7", CVAR_ARCHIVE); }

    if (CG_AimingDownSights() && !CG_AdsStagedOn()) {
        return qtrue; // no staged 3P system in play -> instant first-person ADS (stock coop behavior)
    }
    // staged: first person while the fly-in envelope is past the flip point (works easing in AND out)
    return (s_adsFpEnv > pFlip->value) ? qtrue : qfalse;
}

/*
=================================================================================================
HZM coop - PRECIP TYPE (bug-1206). The engine has exactly ONE precipitation system and it drives
rain, snow AND our sandstorms through the same cg.rain.* configstrings, so "is it precipitating"
is NOT the same question as "is it raining". The lens effects below used to classify by SPEED
alone (>800 = rain, <=800 = snow), which is wrong for a dust storm: coop_mod/weather.scr's
coop_weather_sandLook sets level.rain_speed 900 for driving near-horizontal dust, so a sandstorm
scored as RAIN and the renderer drew water beads + trickle streaks down the lens in the middle of
a desert dust storm.

The weather TYPE is already published per-client and is authoritative: the server sets
level.rain_shader, which arrives as CS_RAIN_SHADER and is stored in cg.rain.currentShader
(cg_main.c CS_RAIN_SHADER). The three looks in weather.scr use distinct bases:
    rain -> "textures/rain"        snow -> "textures/snow"        sand -> "textures/coop_sand"
Native SP maps use "textures/rain" / "textures/snow" / "textures/snowflurry" too, so keying on
the shader name covers scripted and native weather alike. Speed remains the FALLBACK for any map
that sets a precip shader we do not recognise, so stock behaviour is unchanged everywhere else.

NOTE this replaces the old workaround where weather.scr poked "r_ppRainDrops 0" on sand/snow
maps: that is a server-side setcvar, so it only ever reached a LISTEN HOST (remote coop clients
kept their beads), and because r_ppRainDrops is CVAR_ARCHIVE it also stomped the player's own
Advanced Graphics setting for the rest of the session.
=================================================================================================
*/
typedef enum {
    PRECIP_NONE = 0,
    PRECIP_RAIN,
    PRECIP_SNOW,
    PRECIP_DUST
} coopPrecipType_t;

static coopPrecipType_t CG_CoopPrecipType(void)
{
    const char *sh;

    if (cg.rain.density <= 0.0f) {
        return PRECIP_NONE;
    }

    sh = cg.rain.currentShader;
    if (sh && sh[0]) {
        // dust/sand first - it is the case the speed heuristic gets wrong
        if (strstr(sh, "coop_sand") || strstr(sh, "sandstorm") || strstr(sh, "dust")) {
            return PRECIP_DUST;
        }
        if (strstr(sh, "snow")) {
            return PRECIP_SNOW;
        }
        if (strstr(sh, "rain")) {
            return PRECIP_RAIN;
        }
    }

    // unrecognised precip shader (or none published yet) -> stock speed heuristic
    return (cg.rain.speed > 800.0f) ? PRECIP_RAIN : PRECIP_SNOW;
}

static int CG_CalcFov(void)
{
    float x;
    float phase;
    float v;
    int   contents;
    float fov_x, fov_y;
    int   inwater;
    float fov_ratio;

    fov_ratio = (float)cg.refdef.width / (float)cg.refdef.height * (3.0 / 4.0);
    if (fov_ratio == 1) {
        fov_x = cg.camera_fov;
    } else {
        fov_x = RAD2DEG(atan(tan(DEG2RAD(cg.camera_fov / 2.0)) * fov_ratio)) * 2.0;
    }

    // HZM coop - AIM DOWN SIGHTS. While the secondary-attack button (RMB) is held: zoom the WORLD
    // fov (cg_adsZoom) and give the view weapon its OWN fov via r_weaponfovx (renderergl1 renders the
    // gun with that projection). cg_adsGunZoom blends the gun between "constant size" (0, never clips)
    // and "zooms fully with the world" (1, sights align but the rear can clip) - default ~0.5 = a
    // closer/bigger gun. Done in cgame because the server "fov" command does not affect the live view.
    {
        float        fWeaponFov  = fov_x;  // un-zoomed (aspect-adjusted) fov; weapon default = world default
        static float s_adsZoomCur = 1.0f;  // eased ADS world-zoom factor (1 = none) - smooth, not instant
        float        fTarget      = 1.0f;
        float        step;

        // ADS is gated off while a native scope/zoom is active (snipers) - see CG_AimingDownSights.
        if (CG_AimingDownSights()) {
            if (cg.renderingThirdPerson) {
                // HZM coop - staged 3P ADS: while the camera is still THIRD person (shoulder stage /
                // fly-in) apply only the milder shoulder zoom; the moment the wheel-up handoff flips to
                // first person the target switches to the full cg_adsZoom and the ease masks the cut.
                static cvar_t *pShZoom = NULL;
                if (!pShZoom) { pShZoom = cgi.Cvar_Get("cg_adsShoulderZoom", "0.9", CVAR_ARCHIVE); }
                if (pShZoom->value < 1.0f && pShZoom->value >= 0.2f) {
                    fTarget = pShZoom->value;
                }
            } else if (cg_adsZoom && cg_adsZoom->value < 1.0f && cg_adsZoom->value >= 0.2f) {
                fTarget = cg_adsZoom->value;
                // HZM coop - holding breath (steady) zooms in slightly MORE for focus; eases back when it ends.
                if (s_breathSteady) {
                    cvar_t *pBZ = cgi.Cvar_Get("cg_breathZoom", "0.85", CVAR_ARCHIVE);
                    fTarget *= (pBZ ? pBZ->value : 0.85f);
                }
            }
        }
        // [user 2026-08-21] "Still not smooth when transitioning out of ADS (testing with STG44).
        // Still snappy."
        //
        // THE ZOOM WAS NEVER UNIFIED. The pose (rotation + screen shift) was put on one eased factor,
        // but this kept its own independent 12/s ease - and on RELEASE that is roughly 100 ms while
        // the pose takes ~350 ms at 8.5/s. The world therefore zoomed back out three times faster
        // than the gun moved, and for a weapon whose per-gun rotation is small the zoom IS the
        // transition: the StG 44's standing tune is 2.5 degrees of yaw and a 0.04 shift, so almost
        // everything the eye sees on that gun was the fast zoom snapping back.
        //
        // Now derived from the same factor, so zoom, rotation and shift are one motion with one
        // curve - fast in, slow out - by construction rather than by matching three numbers.
        //
        // The TARGET has to be latched. fTarget falls to 1.0 the instant the ADS button is released,
        // so interpolating toward a live target would collapse the zoom to 1.0 in a single frame -
        // the very snap being fixed. Hold the last aimed-at value and let the factor decay across it.
        {
            static float s_adsZoomTgt = 1.0f;
            if (CG_AimingDownSights()) {
                // [user 2026-08-21] "holding shift when ADS used to be smooth to zoom, now its
                // instant." Driving the zoom off the ADS factor fixed the in/out snap, but it also
                // removed the only easing a change made WHILE ALREADY AIMING had. Holding breath
                // multiplies fTarget by cg_breathZoom, and with the factor already at 1.0 that
                // landed in a single frame. Ease the TARGET as well: the factor still owns going to
                // and from the sights, and this owns anything that changes once you are there.
                float kz = (cg.frametime > 0) ? ((float)cg.frametime / 1000.0f) * 8.0f : 0.0f;
                if (kz > 1.0f) { kz = 1.0f; }
                s_adsZoomTgt += (fTarget - s_adsZoomTgt) * kz;
                if (s_adsZoomTgt > fTarget - 0.002f && s_adsZoomTgt < fTarget + 0.002f) {
                    s_adsZoomTgt = fTarget;
                }
            }
            s_adsZoomCur = 1.0f + (s_adsZoomTgt - 1.0f) * CG_AdsPoseFactor();
            if (s_adsZoomCur > 0.9995f) {
                s_adsZoomCur = 1.0f; // exact, so the weapon-fov gate below closes cleanly
            }
        }
        (void)step;

        if (s_adsZoomCur < 0.999f) { // any zoom (including mid-transition)
            float fGunZoom = cg_adsGunZoom ? cg_adsGunZoom->value : 0.0f;
            if (fGunZoom < 0.0f) { fGunZoom = 0.0f; } else if (fGunZoom > 1.0f) { fGunZoom = 1.0f; }
            // blend the weapon fov toward the (eased) zoomed world fov by fGunZoom
            fWeaponFov = fov_x + (fov_x * s_adsZoomCur - fov_x) * fGunZoom;
            // zoom the world by the eased factor
            fov_x *= s_adsZoomCur;
        }

        // HZM coop [user 2026-08-21] fov snap on discharge, decayed here. Applied to the WORLD fov
        // only, never the weapon fov: widening both would scale the gun with the world and the
        // kick would vanish. Widening the world alone pushes the scene away from the muzzle,
        // which is what a recoil impulse actually looks like.
        if (s_fovPunch > 0.001f) {
            // RELATIVE. An absolute +2.2 degrees is a 2.75% nudge at an 80 degree world fov but
            // 14-22% under a scope, which reads as a zoom pop on every bolt-rifle shot.
            fov_x *= (1.0f + s_fovPunch / 80.0f);
            s_fovPunch -= s_fovPunch * ((cg.frametime > 200 ? 200.0f : (float)cg.frametime) / 1000.0f) * 13.0f;
            if (s_fovPunch < 0.01f) {
                s_fovPunch = 0.0f;
            }
        }

        // tell the renderer the weapon fov (== world fov when not aiming, so it uses one projection)
        cgi.Cvar_Set("r_weaponfovx", va("%g", fWeaponFov));

        // HZM coop - drive the renderer's ADS screen-shift from the cgame tune cvars. Standing uses
        // cg_adsShiftX/Y; while crouched ADD cg_adsCrouchShiftX/Y so the whole weapon view (hands + gun)
        // can be slid toward centre to match the standing sight picture. r_weaponshiftx/y are only
        // consumed by the renderer during ADS, so writing them every frame is harmless.
        {
            const char         *adsWpn = "";
            const adsGunTune_t *adsT;
            float               fShiftX, fShiftY;

            if (cg.snap && cg.snap->ps.activeItems[1] >= 0) {
                adsWpn = CG_ConfigString(CS_WEAPONS + cg.snap->ps.activeItems[1]);
            }
            // baked per-gun shift; tune mode / un-tabled guns fall back to the global cg_adsShift* cvars
            // [user 2026-08-20] the shift is now scaled by the SAME factor as the rotation, so
            // hands+gun and the sight rotation move as one rigid object. The crouch extra rides
            // the eased crouch blend instead of binary PMF_DUCKED.
            {
                float fAdsF = CG_AdsPoseFactor();
                float fCrB  = CG_AdsCrouchBlend();
                adsT    = (cg_adsTune && cg_adsTune->integer) ? NULL : CG_FindAdsTune(adsWpn);
                fShiftX = adsT ? adsT->sShiftX : (cg_adsShiftX ? cg_adsShiftX->value : 0.0f);
                fShiftY = adsT ? adsT->sShiftY : (cg_adsShiftY ? cg_adsShiftY->value : 0.0f);
                fShiftX += (adsT ? adsT->cShiftX : (cg_adsCrouchShiftX ? cg_adsCrouchShiftX->value : 0.0f)) * fCrB;
                fShiftY += (adsT ? adsT->cShiftY : (cg_adsCrouchShiftY ? cg_adsCrouchShiftY->value : 0.0f)) * fCrB;
                fShiftX *= fAdsF;
                fShiftY *= fAdsF;
                // settle to EXACT zero. These are printed with %g, so a residual 1.2e-09 would
                // still read as a non-zero shift to anything testing the cvar.
                if (fabs(fShiftX) < 0.0001f) { fShiftX = 0.0f; }
                if (fabs(fShiftY) < 0.0001f) { fShiftY = 0.0f; }
                cgi.Cvar_Set("r_weaponshiftx", va("%g", fShiftX));
                cgi.Cvar_Set("r_weaponshifty", va("%g", fShiftY));
            }
        }
    }

    // HZM coop - publish the player health fraction (0..1) to the renderer's low-health screen effect.
    // The renderer ramps it + applies the desaturate/red-vignette; cgame just reports it each frame.
    // Dead (health <= 0) reports 1.0 so the respawn/spectator view is never tinted.
    {
        int            h    = cg.snap ? cg.snap->ps.stats[STAT_HEALTH] : 0;
        static int     s_peakHealth = 0; // highest health seen THIS life = the self-calibrating "full" mark
        float          frac = 1.0f;
        // hoisted (bug-1290): the downed flag is needed BEFORE the peak update, for the revive edge below
        static cvar_t *pDbnoV = NULL;
        if (!pDbnoV) { pDbnoV = cgi.Cvar_Get("coop_dbnoView", "0", 0); }
        // Neither STAT_MAXHEALTH (hardwired 100, player.cpp:7461) nor coop_health (750) reliably equals the
        // player's ACTUAL current max - coop spawns can be 100 / 250 / 750, and DBNO sets 9999. Dividing by
        // a fixed 750 made "full health" read as e.g. 100/750 = 0.13 -> a permanent red filter even at full
        // HP. Instead track the PEAK health observed this life: at full, current==peak -> frac 1 -> no red,
        // whatever the real max is. Red only ramps once health is actually LOST. Reset on death/spectate so
        // it recalibrates to the next spawn's full value.
        // [2026-08-02] VERIFIED SOUND - do not "fix" this tracker. It was reported (by an automated
        // audit) as latching on DBNO's `healthonly 9999`, giving a permanent full-strength vignette
        // after any revive. That is FALSE: Entity::EventSetHealthOnly (entity.cpp) clamps
        // `if (health > max_health) health = max_health`, so 9999 becomes max_health, and
        // player.cpp:8113 writes stats[STAT_HEALTH] = health / max_health * 100 - a NORMALISED
        // 0..100 percentage that can never exceed 100. The peak therefore caps at 100 on its own and
        // cannot latch. Resetting it on revive would be an actual regression: a player revived at
        // 100/750 real HP reads STAT_HEALTH 13, which SHOULD show as heavily injured; re-seeding the
        // peak to 13 would report frac 1.0 and hide genuine low health exactly when it matters.
        if (h <= 0) {
            s_peakHealth = 0;
        } else if (h > s_peakHealth) {
            s_peakHealth = h;
        }
        if (h > 0 && s_peakHealth > 0) {
            int cur = (h > s_peakHealth) ? s_peakHealth : h;
            frac = (float)cur / (float)s_peakHealth;
            if (frac < 0.0f) { frac = 0.0f; } else if (frac > 1.0f) { frac = 1.0f; }
        }
        // DBNO carry-over: a downed player's health is reset to 100 (reads as 'full'), so force the injury to
        // near-max while downed - the screen should be a bleeding-out haze. dbno.scr flags it per-client via
        // coop_dbnoView (1 = down, 0 = up/revived/dead), stuffed exactly like the DBNO audio fade.
        if (pDbnoV && pDbnoV->integer) { frac = 0.02f; }
        cgi.Cvar_Set("r_ppHealthFrac", va("%g", frac));
    }

    // HZM coop - SUPPRESSION FX: decay s_coopSuppress each frame, spike it when the local player takes
    // damage (health drop), and publish it to the renderer's suppression pass as r_ppSuppress. Near-miss
    // bullet "zings" also spike it via CG_AddSuppression (cg_parsemsg.cpp). Dead players never suppress.
    {
        static int s_lastSuppTime   = 0;
        static int s_lastSuppHealth  = 0;
        int        h        = cg.snap ? cg.snap->ps.stats[STAT_HEALTH] : 0;
        // [2026-08-20] UNIT BUG, shipped. This read `coop_health` (750) as the denominator, with a
        // comment claiming "real coop max, so the spike is proportional to the hit". But STAT_HEALTH
        // is NOT hit points - fgame assigns it a 0..100 PERCENTAGE:
        //     healthfrac = (health / max_health * 100.0f);
        //     client->ps.stats[STAT_HEALTH] = healthfrac;          (player.cpp:8402, :8417)
        // so dividing a percentage delta by 750 made `lost` top out at 100/750 = 0.133 instead of
        // 1.0. The severity term was therefore ~7.5x too weak and effectively dead: the `lost * 2.5`
        // suppression spike could only ever contribute 0.33, and the `lost * 3.0` blood spike 0.40.
        // Every hit read as the same small flinch regardless of how hard it landed.
        //
        // STAT_MAXHEALTH is hardwired to 100 (player.cpp:8435) and is the matching scale, so use it
        // and fall back to 100 rather than to coop_health.
        //
        // CAVEAT for anything else built on this detector: STAT_HEALTH is hijacked by vehicles.
        // player.cpp:8404-8408 reports the VEHICLE's health as the player's while riding one, so
        // boarding a damaged vehicle (m1l3a/m1l3b jeep, t2l2 halftrack) looks like a huge instant
        // hit and dismounting looks like an instant heal. There is no PMF_VEHICLE flag to gate on;
        // if that becomes a problem, detect the ride some other way rather than widening this test.
        int        maxH     = (cg.snap && cg.snap->ps.stats[STAT_MAXHEALTH] > 0)
                                  ? cg.snap->ps.stats[STAT_MAXHEALTH]
                                  : 100;
        float      dt;
        cvar_t    *pFade = cgi.Cvar_Get("coop_suppressFade", "1.4", CVAR_ARCHIVE);
        float      fade  = (pFade && pFade->value > 0.1f) ? pFade->value : 0.9f;

        if (s_lastSuppTime == 0) { s_lastSuppTime = cg.time; }
        dt = (cg.time - s_lastSuppTime) / 1000.0f;
        s_lastSuppTime = cg.time;
        if (dt < 0.0f) { dt = 0.0f; } else if (dt > 0.5f) { dt = 0.5f; }

        // taking fire = a health drop since last frame; scale the spike by how big the hit was
        // [2026-08-21] STAT_HEALTH is hijacked by vehicles: player.cpp:8404-8408 reports the
        // VEHICLE health as the player. Boarding a damaged jeep/halftrack (m1l3a/m1l3b/t2l2)
        // therefore looks like a massive single-frame hit - which the units fix above turned
        // from a harmless 0.08 into a saturated full-screen blood flash. No real hit moves
        // health by more than a quarter of the bar between two snapshots, so treat that as a
        // context switch and mute it rather than flashing the screen.
        // [2026-08-21] the first attempt gated on the SIZE of the drop, which silently swallowed
        // every grenade, panzerschreck, tank shell and mine - no flinch and no blood on exactly
        // the hits that most need feedback. There IS a vehicle signal already on the wire:
        // player.cpp publishes STAT_VEHICLE_HEALTH whenever the player is in one. Gate on that
        // instead, and mute the frame the stat transitions (board/dismount).
        {
            static int s_lastVeh = 0;
            int        veh = cg.snap ? cg.snap->ps.stats[STAT_VEHICLE_HEALTH] : 0;
            if (veh != 0 || s_lastVeh != veh) {
                s_lastSuppHealth = h; // riding, or just boarded/dismounted: resync, do not flinch
            }
            s_lastVeh = veh;
        }
        if (h > 0 && maxH > 0 && s_lastSuppHealth > 0 && h < s_lastSuppHealth) {
            // The severity scaling below has never actually been felt in play - the unit bug above
            // held `lost` under 0.133 for the whole life of the feature - so the authored constants
            // are unproven at their intended magnitude. coop_hitSeverity scales just the severity
            // term (never the flat flinch), so it can be dialled without a rebuild; 0 restores the
            // old, effectively-flat behaviour.
            static cvar_t *pSev = NULL;
            float          lost = (float)(s_lastSuppHealth - h) / (float)maxH;
            float          sev;
            if (!pSev) { pSev = cgi.Cvar_Get("coop_hitSeverity", "1.0", CVAR_ARCHIVE); }
            sev = lost * (pSev->value < 0.0f ? 0.0f : pSev->value);
            if (sev > 1.0f) { sev = 1.0f; }
            CG_AddSuppression(0.25f + sev * 2.5f); // small flinch on any hit, scaled by severity

            // HZM coop [user 08-02] ON-HIT BLOOD spikes off the SAME health-drop detector, so there is
            // one source of truth for "I just got hit". Fast attack (straight to a level proportional to
            // the wound) then a slow decay below - a hit should register instantly and linger, unlike
            // suppression which ramps with sustained fire.
            {
                float bloodHit = 0.35f + sev * 3.0f;
                if (bloodHit > 1.0f) { bloodHit = 1.0f; }
                if (bloodHit > s_coopHit) { s_coopHit = bloodHit; }
            }
        }
        s_lastSuppHealth = h;

        // HZM coop [2026-08-03, bug-1307] the old test here was `h <= 0` with a comment claiming
        // "dead / spectating". Spectating was NOT covered: Player::Spectator() sets
        // deadflag = DEAD_NO and health = max_health (fgame/player.cpp), so a spectator's
        // STAT_HEALTH stays at max and the guard missed them entirely. Use the pm_flags test.
        // Logged separately as a latent defect - the scripted floor below would otherwise make
        // it reachable in play.
        {
            qboolean bAlive = (qboolean)(h > 0 && cg.snap
                                         && !(cg.snap->ps.pm_flags & (PMF_SPECTATING | PMF_INTERMISSION)));

            // dead / spectating / intermission: clear it so the respawn view is never tinted
            if (!bAlive) { s_coopSuppress = 0.0f; }

            s_coopSuppress -= dt / fade;
            if (s_coopSuppress < 0.0f) { s_coopSuppress = 0.0f; }

            // HZM coop - SCRIPTED SUPPRESSION (bug-1307). coop_suppHold (0..1) is a
            // server-stuffed FLOOR held for the duration of a set piece (the e2l1 glider flak
            // run, an artillery barrage). Applied AFTER the decay so the value cannot oscillate
            // with frame rate, and gated on bAlive so a spectator or a corpse is never tinted.
            // Stuffed per-client on CHANGE only - see coop_mod/main.scr::coop_setSuppression.
            // Flags stay 0, never CVAR_ARCHIVE - bug-1202: archiving coop_dbnoView once
            // persisted a forced max-injury effect to disk.
            {
                static cvar_t *pSuppHold = NULL;
                if (!pSuppHold) { pSuppHold = cgi.Cvar_Get("coop_suppHold", "0", 0); }
                if (bAlive && pSuppHold->value > s_coopSuppress) {
                    s_coopSuppress = (pSuppHold->value > 1.0f) ? 1.0f : pSuppHold->value;
                }
            }

            // One-shot spike, e.g. a single flak burst. Self-consuming, so it can never stick
            // even if a clearing stuff is lost.
            {
                static cvar_t *pSuppBump = NULL;
                if (!pSuppBump) { pSuppBump = cgi.Cvar_Get("coop_suppBump", "0", 0); }
                if (pSuppBump->value > 0.0f) {
                    if (bAlive) { CG_AddSuppression(pSuppBump->value); }
                    cgi.Cvar_Set("coop_suppBump", "0");
                }
            }

            cgi.Cvar_Set("r_ppSuppress", va("%g", s_coopSuppress));

            // HZM coop [user 08-02] ON-HIT BLOOD decay + publish. Shares this block's dt and
            // health read. coop_hitBloodFade defaults slower than suppression's 1.4s so the
            // splats linger a beat. Same bAlive gate (bug-1307) - the old h <= 0 test had the
            // identical spectator hole.
            {
                cvar_t *pHitFade = cgi.Cvar_Get("coop_hitBloodFade", "2.2", CVAR_ARCHIVE);
                float   hitFade  = (pHitFade && pHitFade->value > 0.1f) ? pHitFade->value : 2.2f;

                if (!bAlive) { s_coopHit = 0.0f; }   // dead/spectating: never tint the respawn view

                s_coopHit -= dt / hitFade;
                if (s_coopHit < 0.0f) { s_coopHit = 0.0f; }

                cgi.Cvar_Set("r_ppHit", va("%g", s_coopHit));
            }
        }
    }

    // HZM coop - HEAT HAZE: decay s_coopHeat each frame + publish r_ppHeat (spiked near explosions via
    // CG_AddHeat in cg_parsemsg.cpp). Eases back over coop_heatFade seconds.
    {
        static int s_lastHeatTime = 0;
        float      dt2;
        cvar_t    *pHF   = cgi.Cvar_Get("coop_heatFade", "1.3", CVAR_ARCHIVE);
        float      hfade = (pHF && pHF->value > 0.1f) ? pHF->value : 1.3f;

        if (s_lastHeatTime == 0) { s_lastHeatTime = cg.time; }
        dt2 = (cg.time - s_lastHeatTime) / 1000.0f;
        s_lastHeatTime = cg.time;
        if (dt2 < 0.0f) { dt2 = 0.0f; } else if (dt2 > 0.5f) { dt2 = 0.5f; }

        s_coopHeat -= dt2 / hfade;
        if (s_coopHeat < 0.0f) { s_coopHeat = 0.0f; }
        cgi.Cvar_Set("r_ppHeat", va("%g", s_coopHeat));

        // LOCALIZED muzzle heat decays on the same fade and publishes only its INTENSITY (r_ppMuzzleHeat).
        // The hot-spot position/size (r_ppMuzzleX/Y/Radius) are renderer-side tunable cvars - we don't write
        // them here so the user can dial the gun hot-spot without the cgame stomping it every frame.
        s_coopMuzzleHeat -= dt2 / hfade;
        if (s_coopMuzzleHeat < 0.0f) { s_coopMuzzleHeat = 0.0f; }
        cgi.Cvar_Set("r_ppMuzzleHeat", va("%g", s_coopMuzzleHeat));
    }

    // HZM coop - RAIN ON LENS: publish r_ppRainWet (0..1) to the renderer's rain-droplet pass. Wetness =
    // how hard it's raining (networked cg.rain.density) AND whether the player is under OPEN SKY (a single
    // up-trace; beads don't collect indoors). Eased so beads ACCUMULATE stepping into rain and DRY when you
    // take cover. Same publish pattern as the heat/suppression bridges above.
    {
        static int   s_lastWetTime = 0;
        static float s_rainWet     = 0.0f;
        float        dtw, target = 0.0f, k;

        if (s_lastWetTime == 0) { s_lastWetTime = cg.time; }
        dtw = (cg.time - s_lastWetTime) / 1000.0f;
        s_lastWetTime = cg.time;
        if (dtw < 0.0f) { dtw = 0.0f; } else if (dtw > 0.5f) { dtw = 0.5f; }

        // HZM coop - RAIN ONLY: the precip system drives rain, snow AND sandstorms. Water-on-lens only makes
        // sense for RAIN, so key on the weather TYPE published by the server (CG_CoopPrecipType, above) -
        // snow and dust both leave the screen dry. Was a bare `cg.rain.speed > 800` test, which classified
        // the sandstorm look (rain_speed 900) as rain and beaded up the lens during a dust storm (bug-1206).
        if (CG_CoopPrecipType() == PRECIP_RAIN) {
            trace_t trw;
            vec3_t  vUpEnd, vZ = {0.0f, 0.0f, 0.0f};
            VectorCopy(cg.refdef.vieworg, vUpEnd);
            vUpEnd[2] += 4096.0f;
            cgi.CM_BoxTrace(&trw, cg.refdef.vieworg, vUpEnd, vZ, vZ, 0, MASK_SOLID, qfalse);
            if ((trw.surfaceFlags & SURF_SKY) || trw.fraction >= 0.999f) {
                target = cg.rain.density * 2.5f;   // ~0.4 peak -> 1.0 (r_ppRainAmount is the final dial)
                if (target > 1.0f) { target = 1.0f; }
            }
        }

        // ease: wet up over ~0.6s, dry out over ~2.5s (beads linger after you reach cover)
        k = (target > s_rainWet) ? 1.6f : 0.4f;
        s_rainWet += (target - s_rainWet) * (1.0f - exp(-k * dtw));
        if (s_rainWet < 0.0f) { s_rainWet = 0.0f; }
        cgi.Cvar_Set("r_ppRainWet", va("%g", s_rainWet));

        // [user 08-07] FROST-ON-LENS REMOVED - "lets remove the frozen effect on screen for when
        // it snows it looks really bad". The publisher that drove it (an eased r_ppFrostAmt off the
        // PRECIP_SNOW branch + a sky trace) is deleted here, so the effect is inert on every client
        // regardless of a stale archived r_ppFrost: tr_postprocess.c gates the draw on
        // r_ppFrostAmt > 0.001 and nothing writes it any more. frost_fp.glsl and its shaderProgram
        // are left in place for a future re-do; only the signal and its UI toggle are gone.
    }

    x = cg.refdef.width / tan(fov_x / 360 * M_PI);
    fov_y = atan2(cg.refdef.height, x);
    fov_y = fov_y * 360 / M_PI;

    // warp if underwater
    contents = CG_PointContents(cg.refdef.vieworg, -1);
    if (contents & (CONTENTS_WATER | CONTENTS_SLIME | CONTENTS_LAVA)) {
        phase = cg.time / 1000.0 * WAVE_FREQUENCY * M_PI * 2;
        v     = WAVE_AMPLITUDE * sin(phase);
        fov_x += v;
        fov_y -= v;
        inwater = qtrue;
    } else {
        inwater = qfalse;
    }

    // HZM gl2 post-FX (bug-1158): publish the SAME underwater/slime/lava detection this function
    // already computes above (for the existing FOV-warp) as an eased 0..1 fraction, matching the
    // r_ppRainWet/r_ppHeat publish idiom just above - a decaying-toward-target value the renderer
    // reads to drive an underwater screen distortion. Eased rather than a hard on/off so a surface
    // crossing doesn't snap the screen warp instantly; the target itself is the boolean detection
    // that already exists, so this adds no new water-detection logic, only the publish + ease.
    {
        static int   s_lastWetUwTime = 0;
        static float s_underwater    = 0.0f;
        float        dtu, targetUw = inwater ? 1.0f : 0.0f, ku;

        if (s_lastWetUwTime == 0) { s_lastWetUwTime = cg.time; }
        dtu = (cg.time - s_lastWetUwTime) / 1000.0f;
        s_lastWetUwTime = cg.time;
        if (dtu < 0.0f) { dtu = 0.0f; } else if (dtu > 0.5f) { dtu = 0.5f; }

        // fast in (~0.3s, the moment you break the surface), slower out (~1s, matches the FOV-warp
        // easing character rather than snapping the instant you resurface)
        ku = (targetUw > s_underwater) ? 3.3f : 1.0f;
        s_underwater += (targetUw - s_underwater) * (1.0f - exp(-ku * dtu));
        if (s_underwater < 0.0f) { s_underwater = 0.0f; }
        if (s_underwater > 1.0f) { s_underwater = 1.0f; }
        cgi.Cvar_Set("r_ppUnderwater", va("%g", s_underwater));
    }

    // set it
    cg.refdef.fov_x    = fov_x;
    cg.refdef.fov_y    = fov_y;
    cg.fRefFovXCos     = cos(fov_x / 114.0f);
    cg.fRefFovXSin     = sin(fov_x / 114.0f);
    cg.fRefFovYCos     = cos(fov_y / 114.0f);
    cg.fRefFovYSin     = sin(fov_y / 114.0f);
    cg.zoomSensitivity = cg.refdef.fov_y / 75.0;
    return inwater;
}

/*
===============
CG_SetupFog

Prepares fog values for rendering
===============
*/
void CG_SetupFog() {
    cg.refdef.farplane_distance = cg.farplane_distance;
    cg.refdef.farplane_bias = cg.farplane_bias;
    cg.refdef.farplane_color[0] = cg.farplane_color[0];
    cg.refdef.farplane_color[1] = cg.farplane_color[1];
    cg.refdef.farplane_color[2] = cg.farplane_color[2];
    cg.refdef.farplane_cull = cg.farplane_cull;
    cg.refdef.skybox_farplane = cg.skyboxFarplane;
    cg.refdef.renderTerrain = cg.renderTerrain;
    cg.refdef.farclipOverride = cg.farclipOverride;
    cg.refdef.farplaneColorOverride[0] = cg.farplaneColorOverride[0];
    cg.refdef.farplaneColorOverride[1] = cg.farplaneColorOverride[1];
    cg.refdef.farplaneColorOverride[2] = cg.farplaneColorOverride[2];
}

/*
===============
CG_SetupPortalSky

Sets portalsky values for rendering
===============
*/
void CG_SetupPortalSky() {
    cg.refdef.sky_alpha = cg.sky_alpha;
    cg.refdef.sky_portal = cg.sky_portal;
    VectorCopy(cg.sky_axis[0], cg.refdef.sky_axis[0]);
    VectorCopy(cg.sky_axis[1], cg.refdef.sky_axis[1]);
    VectorCopy(cg.sky_axis[2], cg.refdef.sky_axis[2]);
    VectorMA(cg.sky_origin, cg.skyboxSpeed, cg.refdef.vieworg, cg.refdef.sky_origin);
}

/*
===============
CG_CalcViewValues

Sets cg.refdef view values
===============
*/
static int CG_CalcViewValues(void)
{
    playerState_t *ps;
    float          SoundAngles[3];

    memset(&cg.refdef, 0, sizeof(cg.refdef));

    // calculate size of 3D view
    CG_CalcVrect();
    CG_SetupFog();

    ps = &cg.predicted_player_state;

    VectorCopy(ps->origin, cg.refdef.vieworg);
    VectorCopy(ps->viewangles, cg.refdefViewAngles);

    // HZM coop - FREE-AIM + CAMERA WEIGHT (Hell Let Loose feel). The mouse still drives the AIM
    // (ps->viewangles) directly - bullets and the server are unchanged, aim stays responsive. What gets
    // "weight" is the CAMERA: (1) a small deadzone box holds the view while the aim drifts a few degrees, then
    // (2) the camera is SMOOTHED (low-passed) toward that target so the view glides with mass instead of
    // snapping. Works in BOTH first and third person (the 3rd-person chase cam inherits cg.refdefViewAngles).
    // Disabled in ADS / sniper scope / on a turret / camera views so precise aiming stays direct.
    {
        cvar_t *pFA = cgi.Cvar_Get("cg_freeAim", "1", CVAR_ARCHIVE);
        if (pFA && pFA->value > 0.0f) {
            cvar_t   *pBY  = cgi.Cvar_Get("cg_freeAimBoxYaw", "3", CVAR_ARCHIVE);
            cvar_t   *pBP  = cgi.Cvar_Get("cg_freeAimBoxPitch", "2", CVAR_ARCHIVE);
            cvar_t   *pRet = cgi.Cvar_Get("cg_freeAimReturn", "3", CVAR_ARCHIVE);
            cvar_t   *pSm  = cgi.Cvar_Get("cg_freeAimSmooth", "10", CVAR_ARCHIVE); // camera low-pass: lower = heavier/smoother
            float     boxY = pBY ? pBY->value : 3.0f;
            float     boxP = pBP ? pBP->value : 2.0f;
            float     ret  = pRet ? pRet->value : 3.0f;
            float     sm   = pSm ? pSm->value : 10.0f;
            float     dt   = (cg.frametime > 0) ? (cg.frametime / 1000.0f) : 0.0f;
            usercmd_t faCmd;
            qboolean  bActive;
            float     dY, dP, k, tgtY, tgtP, dcy, dcp;

            cgi.GetUserCmd(cgi.GetCurrentCmdNumber(), &faCmd);
            bActive = (cg.snap->ps.stats[STAT_HEALTH] > 0 && !(ps->pm_flags & PMF_CAMERA_VIEW)
                       && !(ps->pm_flags & PMF_TURRET) && !cg.snap->ps.stats[STAT_INZOOM]
                       && !(faCmd.buttons & BUTTON_COOPADS))
                          ? qtrue
                          : qfalse;

            if (!s_faInit) {
                s_faPrevYaw   = ps->viewangles[1];
                s_faPrevPitch = ps->viewangles[0];
                s_faInit      = qtrue;
            }
            dY = ps->viewangles[1] - s_faPrevYaw;
            dP = ps->viewangles[0] - s_faPrevPitch;
            while (dY >  180.0f) { dY -= 360.0f; }
            while (dY < -180.0f) { dY += 360.0f; }
            while (dP >  180.0f) { dP -= 360.0f; }
            while (dP < -180.0f) { dP += 360.0f; }
            s_faPrevYaw   = ps->viewangles[1];
            s_faPrevPitch = ps->viewangles[0];

            if (bActive && fabs(dY) < 45.0f && fabs(dP) < 45.0f) {
                s_faYaw   += dY;
                s_faPitch += dP;
                if (s_faYaw   >  boxY) { s_faYaw   =  boxY; } else if (s_faYaw   < -boxY) { s_faYaw   = -boxY; }
                if (s_faPitch >  boxP) { s_faPitch =  boxP; } else if (s_faPitch < -boxP) { s_faPitch = -boxP; }
                if (ret > 0.0f) { // gentle recenter so the aim drifts back toward centre when idle
                    k = ret * dt;
                    if (k > 1.0f) { k = 1.0f; }
                    s_faYaw   -= s_faYaw * k;
                    s_faPitch -= s_faPitch * k;
                }
            } else {
                s_faYaw   = 0.0f; // inactive: no deadzone offset (direct aim for ADS/scope/turret)
                s_faPitch = 0.0f;
            }

            // target camera = aim minus the (boxed) offset; SMOOTH the camera toward it for weight.
            tgtY = ps->viewangles[1] - s_faYaw;
            tgtP = ps->viewangles[0] - s_faPitch;
            if (!s_faCamInit) {
                s_faCamYaw   = tgtY;
                s_faCamPitch = tgtP;
                s_faCamInit  = qtrue;
            }
            dcy = tgtY - s_faCamYaw;
            dcp = tgtP - s_faCamPitch;
            while (dcy >  180.0f) { dcy -= 360.0f; }
            while (dcy < -180.0f) { dcy += 360.0f; }
            while (dcp >  180.0f) { dcp -= 360.0f; }
            while (dcp < -180.0f) { dcp += 360.0f; }
            // snap (no weight) when inactive or on a big jump; otherwise low-pass at the smoothing rate
            if (!bActive || fabs(dcy) > 45.0f || fabs(dcp) > 45.0f || sm <= 0.0f) {
                k = 1.0f;
            } else {
                k = sm * dt;
                if (k > 1.0f) { k = 1.0f; }
            }
            s_faCamYaw   += dcy * k;
            s_faCamPitch += dcp * k;

            cg.refdefViewAngles[1] = s_faCamYaw;
            cg.refdefViewAngles[0] = s_faCamPitch;
        } else {
            s_faYaw = s_faPitch = 0.0f;
            s_faCamInit = qfalse;
        }
    }

    // HZM coop - lean view-ROLL (the tilt). This is the FP path that actually runs (the earlier damp in
    // CG_OffsetFirstPersonView's bUseWorldPosition branch is not the first-person path). Damp it while ADS
    // by cg_adsLeanRoll so the sights stay level/aligned instead of tilting off. Live-tunable.
    {
        float fLeanRollScale = 1.0f;
        if (CG_AimingDownSights()) {
            cvar_t *pALR = cgi.Cvar_Get("cg_adsLeanRoll", "1.0", CVAR_ARCHIVE);
            fLeanRollScale = pALR ? pALR->value : 0.25f;
        }
        // HZM coop [user 2026-08-22, bug-2055 phase 2] SUPPRESS THE LEAN ROLL IN 3P COVER.
        // In first person a lean roll is right - the head tilts with the body. In a third-person
        // chase it rolls the WHOLE WORLD around a body the player can see is not tilting, which
        // reads as a camera bug rather than a lean. Scaled, not cut, and behind a cvar so it is a
        // live-tunable feel decision instead of a guess (wallcover_plan_v1 §5.5).
        {
            static cvar_t *pCovRoll = NULL;
            float          fRoll = fLeanRollScale;

            if (!pCovRoll) { pCovRoll = cgi.Cvar_Get("coop_coverLeanRoll", "0.25", CVAR_ARCHIVE); }
            if (cg.renderingThirdPerson && cg.snap && (cg.snap->ps.pm_flags & PMF_COOP_COVER)) {
                fRoll *= pCovRoll->value;
            }
            cg.refdefViewAngles[2] += ps->fLeanAngle * 0.1 * fRoll;
        }
    }

    // HZM coop - INJURED SWAY: when hurt, the view drifts in a slow, woozy figure-eight (NOT a jolt/shake).
    // Tracks the same self-calibrating peak-health idea as the low-health vignette: below coop_injuryStart of
    // peak HP, a gentle sine sway on roll+pitch grows as you bleed out. coop_injurySway scales the amount
    // (degrees); 0 disables. Two detuned harmonics keep it organic rather than a clean metronome wobble.
    {
        static int   s_swayPeak = 0;
        cvar_t      *pSwayAmt   = cgi.Cvar_Get("coop_injurySway", "1.0", CVAR_ARCHIVE);
        cvar_t      *pSwayStart = cgi.Cvar_Get("coop_injuryStart", "0.5", CVAR_ARCHIVE);
        int          h          = cg.snap ? cg.snap->ps.stats[STAT_HEALTH] : 0;
        float        amt        = pSwayAmt ? pSwayAmt->value : 1.0f;

        if (h <= 0) {
            s_swayPeak = 0;
        } else if (h > s_swayPeak) {
            s_swayPeak = h;
        }
        if (h > 0 && s_swayPeak > 0 && amt > 0.0f) {
            float start = pSwayStart ? pSwayStart->value : 0.5f;
            float frac  = (float)h / (float)s_swayPeak;
            // DBNO carry-over: a downed player's health resets to 'full', so force near-max injury AND extra
            // wooziness (coop_dbnoSwayMult) so aiming the downed pistol is genuinely hard. dbno.scr flags it
            // per-client via coop_dbnoView (same per-client stuff as the DBNO audio fade).
            {
                static cvar_t *pDbnoV = NULL, *pDbnoMul = NULL;
                if (!pDbnoV)   { pDbnoV   = cgi.Cvar_Get("coop_dbnoView", "0", 0); }
                if (!pDbnoMul) { pDbnoMul = cgi.Cvar_Get("coop_dbnoSwayMult", "1.6", CVAR_ARCHIVE); }
                if (pDbnoV && pDbnoV->integer) {
                    frac = 0.02f;
                    amt *= (pDbnoMul && pDbnoMul->value > 0.0f) ? pDbnoMul->value : 1.6f;
                }
            }
            if (start <= 0.0f) { start = 0.5f; }
            if (frac < start) {
                // the lower the HP, the WORSE it gets: accelerate the ramp (quadratic blend) so a light wound
                // barely sways but bleeding out near death is a heavy woozy drift. ramp 0 at threshold -> 1 at death.
                float ramp = (start - frac) / start;
                float injury;
                float t = cg.time * 0.001f;
                if (ramp < 0.0f) { ramp = 0.0f; } else if (ramp > 1.0f) { ramp = 1.0f; }
                injury = 0.30f * ramp + 0.70f * ramp * ramp;
                // roll: up to ~2.2 deg near death; pitch: ~1.3 deg, detuned -> woozy lissajous
                cg.refdefViewAngles[2] += (float)(sin(t * 0.95) + 0.45 * sin(t * 1.7 + 1.1)) * 2.2f * injury * amt;
                cg.refdefViewAngles[0] += (float)(sin(t * 0.70 + 0.6)) * 1.3f * injury * amt;
            }
        }
    }

    if (cg.snap->ps.stats[STAT_HEALTH] > 0) {
        VectorSubtract(cg.refdefViewAngles, cg.predicted_player_state.damage_angles, cg.refdefViewAngles);
        cg.refdefViewAngles[0] += cg.viewkick[0];
        cg.refdefViewAngles[1] += cg.viewkick[1];

        if (cg.viewkick[0] || cg.viewkick[1]) {
            int   i;
            float fDecay;

            for (i = 0; i < 2; i++) {
                fDecay = cg.viewkick[i] * cg.viewkickRecenter;
                if (fDecay > cg.viewkickMaxDecay) {
                    fDecay = cg.viewkickMaxDecay;
                } else if (fDecay < -cg.viewkickMaxDecay) {
                    fDecay = -cg.viewkickMaxDecay;
                }

                if (fabs(fDecay) < cg.viewkickMinDecay) {
                    if (fDecay > 0.0) {
                        fDecay = cg.viewkickMinDecay;
                    } else {
                        fDecay = -cg.viewkickMinDecay;
                    }
                }

                if (cg.viewkick[i] > 0.0) {
                    cg.viewkick[i] -= fDecay * (float)cg.frametime / 1000.0;
                    if (cg.viewkick[i] < 0.0) {
                        cg.viewkick[i] = 0.0;
                    }
                } else {
                    cg.viewkick[i] -= fDecay * (float)cg.frametime / 1000.0;
                    if (cg.viewkick[i] > 0.0) {
                        cg.viewkick[i] = 0.0;
                    }
                }
            }
        }
    }

    // FIXME: fffx screen shake on win32 builds?


    // add error decay
    if (cg_errorDecay->value > 0) {
        int   t;
        float f;

        t = cg.time - cg.predictedErrorTime;
        f = (cg_errorDecay->value - t) / cg_errorDecay->value;
        if (f > 0 && f < 1) {
            VectorMA(cg.refdef.vieworg, f, cg.predictedError, cg.refdef.vieworg);
        } else {
            cg.predictedErrorTime = 0;
        }
    }

    // calculate position of player's head
    cg.refdef.vieworg[2] += cg.predicted_player_state.viewheight;
    // save off the position of the player's head
    VectorCopy(cg.refdef.vieworg, cg.playerHeadPos);

    // Set the aural position of the player
    VectorCopy(cg.playerHeadPos, cg.SoundOrg);

    // Set the aural axis of the player
    VectorCopy(cg.refdefViewAngles, SoundAngles);
    // yaw is purposely inverted because of the miles sound system
    // Commented out in OPM
    //  Useless as SDL audio/AL is used
    //SoundAngles[YAW] = -SoundAngles[YAW];
    AnglesToAxis(SoundAngles, cg.SoundAxis);

    // decide on third person view
    // HZM coop - STAGED ADS: in third person, holding ADS eases into an over-the-shoulder aim view
    // (CG_OffsetThirdPersonView blend); a wheel-up notch while held flies the camera in and flips to the
    // full FIRST-person iron-sight ADS (CG_AdsForceFirstPerson). First-person players are unchanged
    // (instant ADS). TURRETS: a mounted turret's bound server camera (per-gun TIKI viewOffset,
    // weapturret.cpp) owns the view for FIRST-person players only. Third-person players keep the chase:
    // the PMF_CAMERA_VIEW copy below is skipped for turret cameras in 3P and CG_OffsetThirdPersonView
    // chases the gun's aim (bTurret3p). Script/cutscene cameras (no PMF_TURRET) always win, any view.
    {
        static cvar_t *pDbnoV = NULL;
        if (!pDbnoV) { pDbnoV = cgi.Cvar_Get("coop_dbnoView", "0", 0); }
        CG_UpdateAdsStage(); // advance the shoulder/first-person envelopes + handle the release reset
        CG_UpdateFreecam();  // HZM coop - free-cam orbit: decide mouse capture + ease the orbit envelope
        CG_UpdateDbnoCam();  // HZM coop [user 07-29] - ease the downed-camera envelope
        cg.renderingThirdPerson = (cg_3rd_person->integer && !CG_AdsForceFirstPerson()) ? qtrue : qfalse;
        // HZM coop - a NATIVE zoom (sniper scope / binoculars, STAT_INZOOM) also forces FIRST person:
        // in third person the scope reticle implies the eye-line while the camera sits off-shoulder,
        // so long-range shots land visibly off the reticle ("3rd-person snipers way inaccurate").
        // Scoping snaps to first person (reticle = true aim), releasing the zoom returns the view.
        // EXCEPT on mounted turrets: VehicleTurretGun force-"zooms" its gunner (ToggleZoom(80),
        // vehicleturret.cpp ~:950) purely to pin the fov - fov 80 is no magnification and there is
        // no reticle-accuracy problem on an MG, so the jeep .30cal/halftrack must keep the chase.
        if (ps->stats[STAT_INZOOM] && !(ps->pm_flags & PMF_TURRET)) { cg.renderingThirdPerson = qfalse; }
        // DBNO forces FIRST person (you're crawling / bleeding out - the downed pistol + bleed-out vignette
        // read in 1st person). Returns to your chosen view the instant you're revived / dead / respawned.
        // HZM coop [user 07-29] DBNO no longer FORCES first person. It used to ("the downed pistol +
        // bleed-out vignette read in 1st person"), but that took the choice away - the user plays
        // downed in third person deliberately, and with the ground-level DBNO framing above it is the
        // better aiming view. Keep the old behaviour available as an opt-in rather than deleting it.
        {
            static cvar_t *pDbnoForce1p = NULL;
            if (!pDbnoForce1p) { pDbnoForce1p = cgi.Cvar_Get("cg_dbnoForceFirstPerson", "0", CVAR_ARCHIVE); }
            if (pDbnoV && pDbnoV->integer && pDbnoForce1p->integer) { cg.renderingThirdPerson = qfalse; }
        }
        // HZM coop - IN COVER forces THIRD person (the pose/peek only reads from outside; user:
        // "1st person cover should auto shift to third"). Server drops PMF_COOP_COVER the frame
        // cover ends, so a first-person player snaps straight back to first person on exit.
        // [user 2026-08-20] "...scrolling up to go ads when behind cover puts the camera behind
        // the players head, not actually down their sights properly". The cover force ran AFTER
        // the ADS handoff had already chosen first person, so it silently overrode it. This is
        // the same ordering collision the native scope hit (see the note directly below); that
        // one was fixed for STAT_INZOOM only, and the coop ADS handoff - a second, later route
        // to first person - never got the same treatment. CG_AdsForceFirstPerson is documented
        // as the single decider the camera AND the own-model draw must both use, so the mirror
        // of this line in cg_modelanim.c changes with it (turret-camera-regression rule 2).
        if ((ps->pm_flags & PMF_COOP_COVER) && !CG_AdsForceFirstPerson()) { cg.renderingThirdPerson = qtrue; }
        // HZM coop [237] - NATIVE ZOOM IS FINAL: re-assert first person AFTER every 3P force above.
        // The cover force-3P ran after the zoom force, so scoping while covered/peeking left the
        // camera in third person with the scope overlay drawn over the back of your own head
        // ("scope is looking into the back of the players head" - user). A scoped RMB must read
        // EXACTLY like first person from any 3P mode; turrets keep their fake fov-80 zoom chase.
        if (ps->stats[STAT_INZOOM] && !(ps->pm_flags & PMF_TURRET)) { cg.renderingThirdPerson = qfalse; }
        // HZM coop [232] - the cover orbit-seed / pitch un-jam machinery that lived here was
        // REMOVED: cover no longer captures the mouse into the orbit at all (CG_FreecamEligible)
        // - the aim is live, the camera chases it, and the engine's own +/-85 pitch clamp is the
        // only limit (bug-327: layered orbit + composited aim double-applied pitch = stuck-up).
        // HZM coop - mirror the FINAL view mode to the server (u_view3p userinfo, u_shoulderaim
        // pattern): the manned-turret code un-filters the WORLD gun for third-person gunners
        // (SVF_NOTSINGLECLIENT is a server-side send filter the client cannot override).
        {
            static cvar_t *pV3 = NULL;
            int            v3  = cg.renderingThirdPerson ? 1 : 0;
            if (!pV3) { pV3 = cgi.Cvar_Get("u_view3p", "0", CVAR_USERINFO); }
            if (pV3->integer != v3) { cgi.Cvar_Set("u_view3p", va("%d", v3)); }
        }
        // HZM coop - staged ADS: the breath-hold machinery only updates in the first-person view-weapon
        // path (CG_OffsetFirstPersonView), which does not run in third person - clear it so a stale
        // "steady" can't leak breath-zoom/vignette into the shoulder stage.
        if (cg.renderingThirdPerson) { s_breathSteady = qfalse; }
    }

    if (cg.renderingThirdPerson) {
        // back away from character
        CG_OffsetThirdPersonView();
    }

    // if we are in a camera view, we take our audio cues directly from the camera
    // HZM coop - EXCEPT a turret camera while rendering third person: the chase framing from
    // CG_OffsetThirdPersonView stands (otherwise the gun-eye camera lands INSIDE the drawn player
    // model). Script/cutscene cameras never set PMF_TURRET and keep winning unconditionally.
    if ((ps->pm_flags & PMF_CAMERA_VIEW)
        && !(cg.renderingThirdPerson && (ps->pm_flags & PMF_TURRET))) {
        // Set the aural position to that of the camera
        VectorCopy(cg.camera_origin, cg.refdef.vieworg);

        // Set the aural axis to the camera's angles
        VectorCopy(cg.camera_angles, cg.refdefViewAngles);

        if (cg_protocol >= PROTOCOL_MOHTA_MIN && (ps->pm_flags & PMF_DAMAGE_ANGLES)) {
            // Handle camera shake
            VectorSubtract(cg.refdefViewAngles, cg.predicted_player_state.damage_angles, cg.refdefViewAngles);
        }

        if (ps->camera_posofs[0] || ps->camera_posofs[1] || ps->camera_posofs[2]) {
            vec3_t vAxis[3], vOrg;
            AnglesToAxis(cg.refdefViewAngles, vAxis);
            MatrixTransformVector(ps->camera_posofs, vAxis, vOrg);
            VectorAdd(cg.refdef.vieworg, vOrg, cg.refdef.vieworg);
        }

        // copy view values
        VectorCopy(cg.refdef.vieworg, cg.currentViewPos);
        VectorCopy(cg.refdefViewAngles, cg.currentViewAngles);
        // since 2.0: also copy location data for sound
        VectorCopy(cg.refdef.vieworg, cg.SoundOrg);
        AnglesToAxis(cg.refdefViewAngles, cg.SoundAxis);
    }

    // position eye reletive to origin
    // HZM coop [user 2026-08-19] shell-shock dizziness sways the FINAL view angles for every
    // path (bug-1942: the first hook landed inside the PMF_CAMERA_VIEW camera_posofs branch,
    // which never runs in normal first-person play - the effect was stone dead).
    CG_ReloadFeelAdvance(); // state advances on every view path, every frame
    if (cgi.Cvar_Get("coop_reloadHook", "1", CVAR_ARCHIVE)->integer == 0) {
        CG_ApplyReloadFeel(cg.refdefViewAngles); // v1 placement, kept as the rollback
    } // HZM coop [user 2026-08-19] reload camera follow
    CG_ApplyShellShock(cg.refdefViewAngles);
    AnglesToAxis(cg.refdefViewAngles, cg.refdef.viewaxis);

    if (cg.hyperspace) {
        cg.refdef.rdflags |= RDF_NOWORLDMODEL | RDF_HYPERSPACE;
    }

    // field of view
    return CG_CalcFov();
}

void CG_EyePosition(vec3_t *o_vPos)
{
    (*o_vPos)[0] = cg.playerHeadPos[0];
    (*o_vPos)[1] = cg.playerHeadPos[1];
    (*o_vPos)[2] = cg.playerHeadPos[2];
}

void CG_EyeOffset(vec3_t *o_vOfs)
{
    (*o_vOfs)[0] = cg.playerHeadPos[0] - cg.predicted_player_state.origin[0];
    (*o_vOfs)[1] = cg.playerHeadPos[1] - cg.predicted_player_state.origin[1];
    (*o_vOfs)[2] = cg.playerHeadPos[2] - cg.predicted_player_state.origin[2];
}

void CG_EyeAngles(vec3_t *o_vAngles)
{
    (*o_vAngles)[0] = cg.refdefViewAngles[0];
    (*o_vAngles)[1] = cg.refdefViewAngles[1];
    (*o_vAngles)[2] = cg.refdefViewAngles[2];
}

float CG_SensitivityScale()
{
    return cg.zoomSensitivity;
}

void CG_AddLightShow()
{
    int i;
    float fSlopeY, fSlopeZ;
    float x, y, z;
    vec3_t vOrg;
    float r, g, b;
    float fMax;

    fSlopeY = tan(cg.refdef.fov_x * 0.5);
    fSlopeZ = tan(cg.refdef.fov_y * 0.5);

    for (i = 0; i < cg_acidtrip->integer; i++) {
        x = pow(random(), 1.0 / 3.0) * 2048.0;
        y = crandom() * x * fSlopeY;
        z = crandom() * x * fSlopeZ;

        VectorCopy(cg.refdef.vieworg, vOrg);
        VectorMA(vOrg, x, cg.refdef.viewaxis[0], vOrg);
        VectorMA(vOrg, y, cg.refdef.viewaxis[1], vOrg);
        VectorMA(vOrg, z, cg.refdef.viewaxis[2], vOrg);

        r = random();
        g = random();
        b = random();

        fMax = Q_max(r, Q_max(g, b));
        r /= fMax;
        g /= fMax;
        b /= fMax;

        cgi.R_AddLightToScene(vOrg, (rand() & 0x1FF) + 0x80, r, g, b, 0);
    }
}

qboolean CG_FrustumCullSphere(const vec3_t vPos, float fRadius) {
    vec3_t delta;
    float fDotFwd, fDotSide, fDotUp;

    VectorSubtract(vPos, cg.refdef.vieworg, delta);

    fDotFwd = DotProduct(delta, cg.refdef.viewaxis[0]);
    if (-fRadius >= fDotFwd) {
        return qtrue;
    }

    if (cg.refdef.farplane_distance && cg.refdef.farplane_distance + fRadius <= fDotFwd) {
        return qtrue;
    }

    fDotSide = DotProduct(delta, cg.refdef.viewaxis[1]);
    if (fDotSide < 1.f) {
        fDotSide = -fDotSide;
    }

    if (fDotSide * cg.fRefFovXCos - fDotFwd * cg.fRefFovXSin >= fRadius) {
        return qtrue;
    }

    fDotUp = DotProduct(delta, cg.refdef.viewaxis[2]);
    if (fDotUp < 0.f) {
        fDotUp = -fDotUp;
    }

    if (fDotUp * cg.fRefFovYCos - fDotFwd * cg.fRefFovYSin >= fRadius) {
        return qtrue;
    }

    return qfalse;
}

//=========================================================================

/*
=================
CG_DrawActiveFrame

Generates and draws a game scene and status information at the given time.
=================
*/
/*
==============
CG_UpdateScriptedAudioDucks

HZM coop [user 08-06] bug-1502 - server-triggered, CLIENT-CAPTURED duck for s_musicvolume and
s_ambientvolume, called every frame from CG_DrawActiveFrame (so it runs regardless of view/weapon
state - the breath-hold duck above lives inside CG_OffsetFirstPersonView, which is NOT guaranteed to
run in every game state, e.g. during a cinematic where the player has no active viewmodel).

WHY: a map script (maps/m3l1a.scr's beach ramp-drop, global/RoomTransform.scr's secret-room reveal)
cannot read a client's live cvar value back - stufftext has no $cvar substitution (confirmed: the
tokenizer/exec pipeline has zero '$' handling anywhere) - so a script-side "restore to 0.9/0.6" is
really "reset to the coded default", clobbering whatever the player actually set on the Music/
Ambience sliders. This mirrors the ALREADY-CORRECT pattern used by the breath-hold duck just above
(capture the live value client-side at duck-start, restore to that captured value at duck-end) but
driven by a handful of whitelisted marker cvars (cg_servercmds_filter.cpp) a map script can stufftext
instead of a per-frame gameplay state.

TWO fully independent channels on purpose: m3l1a.scr threads its music duck at map start and its
ambient duck minutes later at the ramp-drop, so a single shared "one duck active" state would make
the second trigger's capture silently no-op against a re-entrancy guard armed by the first.
==============
*/
static void CG_UpdateScriptedAudioDucks(void)
{
    static qboolean s_musicDuckActive = qfalse, s_musicDuckLastTrig = qfalse;
    static float    s_musicDuckBase   = -1.0f;
    static qboolean s_ambDuckActive = qfalse, s_ambDuckLastTrig = qfalse;
    static float    s_ambDuckBase   = -1.0f;

    cvar_t  *pMusicTrig, *pMusicTarget, *pMusicInDur, *pMusicOutDur, *pMusicCvar;
    cvar_t  *pAmbTrig, *pAmbTarget, *pAmbInDur, *pAmbOutDur, *pAmbCvar;
    qboolean bMusicTrig, bAmbTrig;
    float    fDur, fStep, fCur, fTarget, fNext;
    qboolean bDone;

    // ---- music channel (s_musicvolume) ----
    pMusicTrig = cgi.Cvar_Get("coop_duckMusicTrigger", "0", 0);
    bMusicTrig = pMusicTrig->integer != 0;

    if (bMusicTrig && !s_musicDuckLastTrig && !s_musicDuckActive) {
        s_musicDuckBase   = cgi.Cvar_Get("s_musicvolume", "0.9", 0)->value;
        s_musicDuckActive = qtrue;
    }
    s_musicDuckLastTrig = bMusicTrig;

    if (s_musicDuckActive) {
        pMusicTarget = cgi.Cvar_Get("coop_duckMusicTarget", "1", 0);
        pMusicInDur  = cgi.Cvar_Get("coop_duckMusicInDur", "4", 0);
        pMusicOutDur = cgi.Cvar_Get("coop_duckMusicOutDur", "12", 0);
        pMusicCvar   = cgi.Cvar_Get("s_musicvolume", "0.9", 0);

        fDur = bMusicTrig ? pMusicInDur->value : pMusicOutDur->value;
        if (fDur < 0.05f) { fDur = 0.05f; }
        fStep = (cg.frametime > 0) ? ((float)cg.frametime / 1000.0f) / fDur : 1.0f;
        if (fStep > 1.0f) { fStep = 1.0f; }

        fCur    = pMusicCvar->value;
        fTarget = bMusicTrig ? pMusicTarget->value : s_musicDuckBase;
        fNext   = fCur + (fTarget - fCur) * fStep;
        bDone   = (fabs(fTarget - fNext) < 0.001f);
        if (bDone) { fNext = fTarget; }
        cgi.Cvar_Set("s_musicvolume", va("%g", fNext));

        if (!bMusicTrig && bDone) {
            s_musicDuckActive = qfalse;
            s_musicDuckBase   = -1.0f;
        }
    }

    // ---- ambient channel (s_ambientvolume) ----
    pAmbTrig = cgi.Cvar_Get("coop_duckAmbientTrigger", "0", 0);
    bAmbTrig = pAmbTrig->integer != 0;

    if (bAmbTrig && !s_ambDuckLastTrig && !s_ambDuckActive) {
        s_ambDuckBase   = cgi.Cvar_Get("s_ambientvolume", "0.6", 0)->value;
        s_ambDuckActive = qtrue;
    }
    s_ambDuckLastTrig = bAmbTrig;

    if (s_ambDuckActive) {
        pAmbTarget = cgi.Cvar_Get("coop_duckAmbientTarget", "0", 0);
        pAmbInDur  = cgi.Cvar_Get("coop_duckAmbientInDur", "4", 0);
        pAmbOutDur = cgi.Cvar_Get("coop_duckAmbientOutDur", "12", 0);
        pAmbCvar   = cgi.Cvar_Get("s_ambientvolume", "0.6", 0);

        fDur = bAmbTrig ? pAmbInDur->value : pAmbOutDur->value;
        if (fDur < 0.05f) { fDur = 0.05f; }
        fStep = (cg.frametime > 0) ? ((float)cg.frametime / 1000.0f) / fDur : 1.0f;
        if (fStep > 1.0f) { fStep = 1.0f; }

        fCur    = pAmbCvar->value;
        fTarget = bAmbTrig ? pAmbTarget->value : s_ambDuckBase;
        fNext   = fCur + (fTarget - fCur) * fStep;
        bDone   = (fabs(fTarget - fNext) < 0.001f);
        if (bDone) { fNext = fTarget; }
        cgi.Cvar_Set("s_ambientvolume", va("%g", fNext));

        if (!bAmbTrig && bDone) {
            s_ambDuckActive = qfalse;
            s_ambDuckBase   = -1.0f;
        }
    }
}

/*
==============
CG_SyncWussPk3Count

HZM coop [user 08-06] bug-1508 - "Wuss.pk3" challenge: mirror the client's session-cumulative
unique-sound-registration count (s_sfxCount, plain internal cvar written by snd_dma_new.cpp every
time a new sound registers - can fire many times per second during a busy load) into a SEPARATE,
THROTTLED cvar that IS CVAR_USERINFO. Only re-Cvar_Set-ing coop_wussCount once every 10s (and only
when the value actually changed) bounds this to at most one reliable-command resend per 10 seconds
no matter how fast sounds register - deliberately avoiding the reliable-command-flood class of bug
this project already hit once (bug-670, ~373 commands in one join burst overran the 512-deep ring
and self-dropped the client). coop_mod/challenges.scr reads this back via self.userinfo.
==============
*/
static void CG_SyncWussPk3Count(void)
{
    static int s_lastSyncTime = 0;
    static int s_lastCount    = -1;
    cvar_t    *pCount;
    int        count;

    if (cg.time - s_lastSyncTime < 10000) {
        return;
    }
    s_lastSyncTime = cg.time;

    pCount = cgi.Cvar_Get("s_sfxCount", "0", 0);
    count  = pCount->integer;
    if (count == s_lastCount) {
        return;
    }
    s_lastCount = count;

    cgi.Cvar_Set("coop_wussCount", va("%d", count));
}


// =================================================================================================
// HZM coop [user 2026-08-20] ONE FEEL-CONTEXT SCALAR: how rattled is the player, 0..1.
//
// The user asked for reload/handling character to change with "calm vs stressed", for injured hands
// to shake more, and for a shared notion of stress to drive future work. This is that one number.
//
// It is deliberately NOT called "composure": an AI *morale* system already ships
// (coop_moraleEnable, coop_mod/morale.scr) and two similarly-named scalars in the same logs would be
// a trap. The name matches the pre-existing design note in
// _research/weapon_feel_r1_variation.md section 3, which specified these same inputs.
//
// Everything here is client-side and already networked - nothing new crosses the wire.
//
// Three input choices are worth explaining, because the obvious versions are all wrong:
//
//  * HEALTH is read from the published r_ppHealthFrac rather than from STAT_HEALTH directly. Two
//    reasons. STAT_HEALTH is a 0..100 PERCENTAGE, not hit points (player.cpp:8402), and it is
//    hijacked by vehicles - riding one reports the VEHICLE's health as yours. r_ppHealthFrac is the
//    existing self-calibrating peak tracker, which carries a "VERIFIED SOUND - do not fix this"
//    banner, and it ALSO already applies the DBNO override (0.02 while downed). Reusing it means a
//    downed player reads as maximally stressed for free, instead of reading as perfectly healthy -
//    which is what a fresh STAT_HEALTH read would have done, since dbno.scr sets health to full.
//    It is one frame stale (published from CG_CalcFov, which runs later in the frame). 16 ms.
//
//  * SUPPRESSION is the headline term, per the design note. Also one frame stale, same reason.
//
//  * STAMINA is a client-side re-simulation of the server's pool and is known to diverge - it only
//    advances inside CG_OffsetFirstPersonView, so in third person it regenerates while the server
//    drains it. That is acceptable HERE and only here, because every consumer of this scalar is a
//    first-person viewmodel effect; do not reuse the stamina term for anything a 3P player sees.
//
// Stress SPIKES fast and RECOVERS slowly - the same asymmetry that makes recoil read as weight. A
// symmetric ease reads as a meter, not as adrenaline.
static float s_wfeelStress = 0.0f;
static int   s_wfeelTime   = -1;

float CG_GetSuppression(void)
{
    return s_coopSuppress; // NOTE: one frame stale for readers that run before CG_CalcFov
}

qboolean CG_GetStamina(float *outFrac)
{
    if (!outFrac) {
        return qfalse;
    }
    if (s_spStamMax <= 0.01f) {
        *outFrac = 1.0f;
        return qfalse;
    }
    *outFrac = s_spStam / s_spStamMax;
    if (*outFrac < 0.0f) {
        *outFrac = 0.0f;
    } else if (*outFrac > 1.0f) {
        *outFrac = 1.0f;
    }
    return qtrue;
}

// HZM coop [user 2026-08-27] GUN BRACING - client mirror of the server envelope.
//
// The server owns the truth (geometry traces, hysteresis) and publishes it as coop_braceView 0-100
// on the same change-only stufftext channel cover uses - every pm_flags bit is already allocated, so
// there is no room for a real flag. Change-only means the value arrives in steps, so we ease a LOCAL
// copy toward it: the feel scaling below reads a smooth curve rather than a staircase.
//
// The thunk fires on the RISING edge only, and only once the server has committed (its enter dwell
// already ran), so brushing past a wall cannot machine-gun the sound. Local sound = you hear your own
// gun settle and nobody else's, which is the user's call.
// Is a mount OFFERED right now? Drives the prompt only - never the effects, which follow the
// committed mount below.
qboolean CG_CoopBraceAvail(void)
{
    static cvar_t *pAvail = NULL;
    if (!pAvail) { pAvail = cgi.Cvar_Get("coop_braceAvail", "0", 0); }
    return pAvail->integer ? qtrue : qfalse;
}

float s_braceKickEnv = 0.0f;   // mount-settle impulse, 1 -> 0 over ~0.35s

float CG_CoopBraceKick(void)
{
    return s_braceKickEnv;
}

float CG_CoopBrace(void)
{
    static cvar_t *pView = NULL;
    static float   s_env  = 0.0f;
    static qboolean s_was = qfalse;
    static int     s_last = -1;
    float          fTgt, dt, step;

    if (!pView) { pView = cgi.Cvar_Get("coop_braceView", "0", 0); }

    // a fresh map/respawn leaves the cvar behind; the server re-publishes on any change, and the
    // ease below cannot strand because the target is read every frame.
    // [bug-2133] EASE ONCE PER FRAME. This advanced on every CALL, and it is called well over a
    // dozen times a frame (CG_AimingDownSights alone has ~25 call sites), so the 6/s rise ran at
    // roughly fifteen times its intended rate and the envelope snapped - reproducing the exact
    // stufftext staircase the ease exists to hide, and desyncing it from the server envelope that
    // drives the real spread. Everything now reads one value computed once.
    if (s_last != cg.time) {
        qboolean bOn;
        s_last = cg.time;

        fTgt = (float)pView->integer * 0.01f;
        if (fTgt < 0.0f) { fTgt = 0.0f; } else if (fTgt > 1.0f) { fTgt = 1.0f; }

        dt   = (cg.frametime > 0) ? ((float)cg.frametime / 1000.0f) : 0.0f;
        step = dt * ((fTgt > s_env) ? 6.0f : 3.0f);
        if (s_env < fTgt) {
            s_env += step;
            if (s_env > fTgt) { s_env = fTgt; }
        } else if (s_env > fTgt) {
            s_env -= step;
            if (s_env < fTgt) { s_env = fTgt; }
        }

        bOn = (fTgt > 0.0f) ? qtrue : qfalse;
        // [user 2026-08-27] "it doesn't really feel like theres any weight to bracing the gun and the
        // camera doesnt really respond". Setting a rifle down on something is a small collision: the
        // weapon stops against the surface and the shooter's whole upper body settles onto it. One
        // impulse on the rising edge, decayed fast, drives both the gun and the view below.
        if (bOn && !s_was) { s_braceKickEnv = 1.0f; }
        // The cue now marks a deliberate act (Use pressed), not a surface drifting into range, so it
        // can be a real sound instead of a whisper - "no real noise" was half the reason the whole
        // thing was unreadable.
        if (bOn && !s_was) {
            cgi.S_StartLocalSound("sound/coop_brace/brace_on.wav", qfalse);
        } else if (!bOn && s_was) {
            cgi.S_StartLocalSound("sound/coop_brace/brace_off.wav", qfalse);
        }
        s_was = bOn;
        if (s_braceKickEnv > 0.0f) {
            s_braceKickEnv -= dt * 3.0f;
            if (s_braceKickEnv < 0.0f) { s_braceKickEnv = 0.0f; }
        }
    }
    return s_env;
}

void CG_FeelStressAdvance(void)
{
    static cvar_t *pOn = NULL;
    float          raw, hp, stam, supp, spd, dt, rate;
    qboolean       bAlive;

    if (s_wfeelTime == cg.time) {
        return; // once per frame
    }
    s_wfeelTime = cg.time;
    if (cg.frametime <= 0) {
        return; // skip a zero-length frame, never snap
    }
    if (!pOn) {
        pOn = cgi.Cvar_Get("coop_wfeelStress", "1", CVAR_ARCHIVE);
    }

    // bug-1306's predicate, verbatim. NEVER `health <= 0` alone: Player::Spectator() leaves health
    // at max, so that test misses spectators entirely and they inherit whatever the last live
    // player's state was.
    bAlive = (qboolean)(cg.snap && cg.snap->ps.stats[STAT_HEALTH] > 0
                        && !(cg.snap->ps.pm_flags & (PMF_SPECTATING | PMF_INTERMISSION)));

    raw = 0.0f;
    if (pOn->integer && bAlive
        && !(cg.snap->ps.pm_flags & (PMF_TURRET | PMF_CAMERA_VIEW))) {
        hp = cgi.Cvar_Get("r_ppHealthFrac", "1", 0)->value; // 1 = unhurt, 0.02 = downed
        if (hp < 0.0f) { hp = 0.0f; } else if (hp > 1.0f) { hp = 1.0f; }

        if (!CG_GetStamina(&stam)) {
            stam = 1.0f;
        }

        supp = CG_GetSuppression();
        if (supp < 0.0f) { supp = 0.0f; } else if (supp > 1.0f) { supp = 1.0f; }

        // NOT ps.speed - that is the CURRENT PERMITTED maximum, already scaled down for walking,
        // crouching and aiming, so dividing by it makes a crouched creeping player read as
        // "moving flat out" in the calmest state in the game. Normalise against the real run
        // speed instead.
        spd = 0.0f;
        {
            float runRef = cgi.Cvar_Get("sv_runspeed", "287", 0)->value;
            if (runRef < 1.0f) { runRef = 287.0f; }
            spd = VectorLength(cg.predicted_player_state.velocity) / runRef;
            if (spd > 1.0f) { spd = 1.0f; }
        }

        // weights sum to 1.0, ordered per the design note: under fire is the headline, then how hurt
        // you are, then how winded, then how hard you are moving
        raw = 0.45f * supp + 0.25f * (1.0f - hp) + 0.18f * (1.0f - stam) + 0.12f * spd;

        // and the two things that CALM a person: being braced, and holding your breath
        if (cg.snap->ps.pm_flags & PMF_DUCKED) {
            raw *= 0.85f;
        }
        if (CG_IsBreathSteady()) {
            raw *= 0.60f;
        }
    // [user 2026-08-27] BRACED damper - the TWIN of the server's in TickCoopStress. Identical weight
    // by contract: if these two drift, the sway you feel stops matching the spread you actually get.
    {
    float fBrEnv = CG_CoopBrace();
    if (fBrEnv > 0.0f) {
        // [bug-2133] the twins had drifted (server 0.30, client 0.60) - the felt sway was damped
        // twice as hard as the spread actually was. One shared cvar, so they cannot drift again.
        cvar_t *pBS = cgi.Cvar_Get("coop_braceStress", "0.50", CVAR_ARCHIVE);
        raw *= 1.0f - fBrEnv * (pBS ? pBS->value : 0.50f);
    }
    }
        if (raw < 0.0f) { raw = 0.0f; } else if (raw > 1.0f) { raw = 1.0f; }
    }

    dt   = (float)cg.frametime / 1000.0f;
    if (dt > 0.1f) {
        dt = 0.1f; // a hitch must not teleport the envelope
    }
    rate = (raw > s_wfeelStress) ? 9.0f : 0.8f; // spike fast, bleed off slowly
    {
        float k = dt * rate;
        if (k > 1.0f) {
            k = 1.0f; // MANDATORY - this is a TWO-SIDED ease and has no floor to rescue an overshoot
        }
        s_wfeelStress += (raw - s_wfeelStress) * k;
    }
    if (s_wfeelStress < 0.0005f && raw == 0.0f) {
        s_wfeelStress = 0.0f;
    }

    // debug mirror. Flags 0 - NOT archived (it would latch, TRAPS T7) and NOT userinfo (a value
    // changing every frame would send a userinfo packet every frame).
    cgi.Cvar_Set("coop_wfeelStressCur", va("%.3f", s_wfeelStress));
}

float CoopWFeelStress(void)
{
    return s_wfeelStress;
}


// HZM coop [user 2026-08-20] BLOOD ON THE GUN, and the medkit stow.
//
// "Another idea to explore is blood on guns if you are shooting enemies up super close" /
// "rain washes it off".
//
// The signal is free and needs no protocol change: cg_parsemsg already decodes flesh impacts into
// flesh_impact_pos[] for its own hit sounds, so the client already knows where every bullet hit a
// body. Two qualifiers turn that into "I just shot someone in the face": the impact has to be close
// to the camera, and the local player has to have fired recently - flesh impacts are broadcast for
// EVERY shooter, so without the second test a teammate executing someone beside you would splatter
// your weapon.
//
// Rain uses r_ppRainWet, which the weather system already publishes per-client, so a downpour
// scrubs the gun clean in a few seconds while a dry map keeps it for a couple of minutes.
static float s_gunBlood = 0.0f;
static int   s_lastFireTime = 0;

static int s_foleyAt = 0; // when the mechanical layer is due, 0 = nothing pending

void CG_NoteLocalFire(void)
{
    static cvar_t *pFol = NULL;

    s_lastFireTime = cg.time;

    // HZM coop [user 2026-08-21] WEAPON ACTION FOLEY. "Inject a secondary, crunchy audio file into
    // the shooting script... the physical slide rattling back and the heavy clink of mechanical
    // parts moving milliseconds after the initial explosion."
    //
    // Scheduled rather than played inline: the whole point is that it arrives AFTER the blast, so
    // the ear separates the explosion from the machinery instead of hearing one composite bang.
    // Only ever scheduled for the LOCAL player - it is a first-person feel layer, and broadcasting
    // a second sound per shot for every shooter would be a real bandwidth and voice-count cost on a
    // busy firefight (this project has already had to raise MAX_SOUNDS twice).
    if (!pFol) {
        pFol = cgi.Cvar_Get("coop_actionFoley", "1", CVAR_ARCHIVE);
    }
    // [2026-08-21] RE-ARM rather than "only if nothing pending". The first version ignored a shot
    // while one was queued, so any weapon cycling faster than ~1090 RPM - the MG42 - lost
    // alternate layers unpredictably. Re-arming keeps the layer locked to the LAST shot, which on
    // a fast weapon reads as a continuous mechanical rattle rather than a stutter, and a floor
    // stops it retriggering faster than the sample can be heard.
    if (pFol->value > 0.0f && (s_foleyAt == 0 || cg.time + 55 > s_foleyAt + 45)) {
        s_foleyAt = cg.time + 55; // ms after the shot
    }
}

// Called once per frame beside the other feel updates.
// HZM coop [user 2026-08-28] TIME OF DAY / DAYLIGHT GRADE.
//
// MOHAA cannot relight the world. Lightmaps are baked; BSP surfaces carry no per-style lightmap set
// at all (`styles` appears zero times in either renderer's BSP loader), so lightstyles drive only
// entity dlights and particle colour - setting style 0 changes nothing. Every worldspawn lighting key
// (sunlight, suncolor, ambientlight, sundirection) is declared with a NULL handler and discarded. And
// r_mapOverBrightBits is baked in at lightmap load and is CVAR_LATCH, needing a vid_restart that
// crashes gl2 (bug-1181).
//
// What DOES move every frame is the gl2 grade in RB_ToneMap, which is applied to the world before the
// HUD is drawn. So time of day is a GRADE, not a relight: exposure down, saturation down, temperature
// cooled. Combined with $world farplane_color - which on gl2 also tints the SKYBOX itself, since the
// sky shell is drawn through the forward global fog - that is a convincing dusk with no baked data
// touched and no reload.
//
// One scalar serves two features deliberately: a scripted day->dusk->night cycle, and 'it looks
// brighter when the weather is clear'. They are the same knob at different values.
// HZM coop [found 2026-08-28] HEADSHOT CUE, now shooter-only. The server used to play this with
// attacker->Sound(..., CHAN_LOCAL), which LOOKS local but ends in SV_Sound - a loop over every active
// client - and CHAN_LOCAL then makes it 2D at full volume with no attenuation. All four players in a
// coop game heard every headshot as if it were their own. The server now bumps a per-client counter
// instead; a change means THIS player got the headshot.
static void CG_CoopHeadshotCueThink(void)
{
    static cvar_t *pCue = NULL;
    static int     s_last = -1;

    if (!pCue) { pCue = cgi.Cvar_Get("coop_hsCue", "0", 0); }

    if (s_last < 0 || pCue->integer < s_last) {
        s_last = pCue->integer;   // first sight, or the counter reset on a map change - do not fire
        return;
    }
    if (pCue->integer != s_last) {
        s_last = pCue->integer;
        cgi.S_StartLocalSound("coop_headshot", qtrue);   // C file - commandManager is C++ only
    }
}


void CG_CoopDaylightThink(void)
{
    static cvar_t *pDay = NULL, *pGrade = NULL, *pTmap = NULL;
    static float   s_cur   = -1.0f;   // last published, so we write cvars only on change
    static qboolean s_gradeOn = qfalse;
    float          d, expo, cont, sat, temp;
    qboolean       bFirst;

    if (!pDay) {
        // NOT archived. Time of day is scripted/server-driven runtime state, not a user preference -
        // archiving it meant a night value written once (by a script, or over rcon) SURVIVED THE
        // RESTART and the player came back to a permanently dim world with no obvious cause. The
        // server republishes the real value on every connect, so nothing is lost by not saving it.
        pDay   = cgi.Cvar_Get("coop_daylight", "1", 0);
        pGrade = cgi.Cvar_Get("r_ppGrade", "0", CVAR_ARCHIVE);
        pTmap  = cgi.Cvar_Get("r_ppTonemap", "0", CVAR_ARCHIVE);
    }

    bFirst = (s_cur < -0.5f) ? qtrue : qfalse;
    d = pDay->value;
    if (d > 1.0f) { d = 1.0f; } else if (d < 0.0f) { d = 0.0f; }
    if (fabs(d - s_cur) < 0.002f) {
        return;   // nothing changed - do not touch cvars every frame
    }
    s_cur = d;

    // At full daylight, restore the engine's own defaults EXACTLY and switch the grade path back off,
    // so a player who never triggers a night cycle renders bit-identically to before this existed.
    // NOTE the `|| bFirst`: the r_pp* grade cvars ARE archived (they are legitimate user settings),
    // so a night grade written in a previous session comes back at full strength on the next launch.
    // Without this, a stale archived grade could never be cleared - s_gradeOn starts false, so the
    // restore below was skipped exactly when it was needed most, and the world stayed dim forever.
    if (d >= 0.999f) {
        if (s_gradeOn || bFirst) {
            cgi.Cvar_Set("r_ppExposure",   "0.889971");
            cgi.Cvar_Set("r_ppContrast",   "0.951289");
            cgi.Cvar_Set("r_ppSaturation", "1.031519");
            cgi.Cvar_Set("r_ppTemp",       "0");
            cgi.Cvar_Set("r_ppTonemap",    "0");
            s_gradeOn = qfalse;
        }
        return;
    }

    // The grade block in RB_ToneMap is gated on (r_tonemapMode==1 || r_ppTonemap || r_ppGrade) and all
    // three default to 0, so the path has to be armed or every value below is silently ignored.
    // [FIX 2026-08-28] ARM WITH r_ppTonemap, NOT r_ppGrade. r_ppGrade is a PRESET SELECTOR, not an
    // enable flag: RB_ToneMap switches on its integer and cases 1-4 OVERWRITE exposure, contrast,
    // saturation and temperature with fixed war-film looks. Setting it to 1 to 'arm' the path therefore
    // selected the Neutral preset (expo 1.0, sat 1.0, temp 0) and threw away every value written just
    // above - the cvars read back correctly while the renderer used the preset, so it looked like the
    // grade was not reaching the screen at all. r_ppTonemap enables the same block and leaves the manual
    // values alone, and r_ppGrade must be held at 0 so `default: break` keeps them.
    if (!s_gradeOn) {
        cgi.Cvar_Set("r_ppTonemap", "1");
        s_gradeOn = qtrue;
    }
    if (pGrade->integer) {
        cgi.Cvar_Set("r_ppGrade", "0");   // a preset would override everything below
    }

    // night <- d -> day. Contrast RISES slightly into night: dropping exposure alone reads as a grey
    // wash rather than darkness, because the lightmap's own ambient floor does not scale with it.
    expo = 0.30f + (0.889971f - 0.30f) * d;
    cont = 1.05f + (0.951289f - 1.05f) * d;
    sat  = 0.45f + (1.031519f - 0.45f) * d;
    temp = -0.18f * (1.0f - d);   // cool the shadows; moonlight is blue, not grey

    cgi.Cvar_Set("r_ppExposure",   va("%g", expo));
    cgi.Cvar_Set("r_ppContrast",   va("%g", cont));
    cgi.Cvar_Set("r_ppSaturation", va("%g", sat));
    cgi.Cvar_Set("r_ppTemp",       va("%g", temp));
}


static void CG_ActionFoleyThink(void)
{
    int iClass;

    if (!s_foleyAt || cg.time < s_foleyAt) {
        return;
    }
    s_foleyAt = 0;
    if (!cg.snap || cg.snap->ps.stats[STAT_HEALTH] <= 0 || cg.renderingThirdPerson) {
        return;
    }
    iClass = cg.snap->ps.stats[STAT_EQUIPPED_WEAPON];
    // NOTE: S_StartLocalSound hardcodes CHAN_MENU internally and ignores the alias channel, so
    // this shares a voice with the breath-hold cues and will cut them. Accepted for now because
    // the alternative is a positional S_StartSound, which would need a real sfxHandle and would
    // reintroduce the distance falloff this layer deliberately does not want. Flagged rather than
    // hidden: if the breath-hold cue starts getting clipped in play, this is why.
    if (iClass & (WEAPON_CLASS_RIFLE | WEAPON_CLASS_MG | WEAPON_CLASS_HEAVY)) {
        cgi.S_StartLocalSound("coop_wpn_action_heavy", qfalse);
    } else {
        cgi.S_StartLocalSound("coop_wpn_action_light", qfalse);
    }
}

void CG_NoteFleshImpact(const vec3_t pos)
{
    static cvar_t *pOn = NULL;
    float          d;
    vec3_t         v;

    if (!pOn) {
        pOn = cgi.Cvar_Get("coop_gunBlood", "1", CVAR_ARCHIVE);
    }
    if (pOn->value <= 0.0f || !cg.snap) {
        return;
    }
    // must be OUR shot: flesh impacts are broadcast for every shooter in earshot
    if (cg.time - s_lastFireTime > 350) {
        return;
    }
    VectorSubtract(pos, cg.refdef.vieworg, v);
    d = VectorLength(v);
    if (d > 150.0f) {
        return; // "super close" - roughly 12 feet at 1 unit ~ 1 inch
    }
    // full strength in your face, tapering to nothing at the range limit
    s_gunBlood += (1.0f - (d / 150.0f)) * 0.55f * pOn->value;
    if (s_gunBlood > 1.0f) {
        s_gunBlood = 1.0f;
    }
}

float CG_GunBlood(void)
{
    return s_gunBlood;
}

static void CG_GunBloodDecay(void)
{
    static int s_last = 0;
    float      dt, rate, wet;

    if (s_last == 0) {
        s_last = cg.time;
    }
    dt     = (cg.time - s_last) / 1000.0f;
    s_last = cg.time;
    if (dt <= 0.0f || s_gunBlood <= 0.0f) {
        return;
    }
    if (dt > 0.5f) {
        dt = 0.5f;
    }
    // reset on the death edge - a fresh life starts with a clean weapon
    {
        static int s_wasAlive = 1;
        int        alive = (cg.snap && cg.snap->ps.stats[STAT_HEALTH] > 0) ? 1 : 0;
        if (!alive && s_wasAlive) {
            s_gunBlood = 0.0f;
        }
        s_wasAlive = alive;
    }
    wet  = cgi.Cvar_Get("r_ppRainWet", "0", 0)->value; // 0 dry .. 1 soaked
    if (wet < 0.0f) { wet = 0.0f; } else if (wet > 1.0f) { wet = 1.0f; }
    // dry: ~2 minutes to fade. Rain: seconds.
    rate = 0.008f + wet * 0.30f;
    s_gunBlood -= rate * dt;
    if (s_gunBlood < 0.0f) {
        s_gunBlood = 0.0f;
    }
}

// HZM coop [user 2026-08-20] THE ONE ADS FACTOR.
//
// Before this, the ADS pose was eased in THREE places on THREE schedules - the sight rotation in
// cg_modelanim.c (15/s in, 8.5/s out), the world zoom here (12/s), and the screen shift not at all -
// so the gun could never translate and rotate as one rigid object at any tuning. Worse, the
// rotation's ease lived inside CG_ModelAnim's first-person weapon-tag branch, which does not run in
// third person, in cover, on a cutscene camera, or while dead: the factor FROZE at its last value
// and was re-applied at full strength on the first frame the branch ran again.
//
// This advances once per frame from CG_DrawActiveFrame, before anything reads it, so there is no
// frame-order skew between the consumers and no state in which it stops tracking.
//
// Two details are load-bearing:
//   * the exact-settle snap. A pure exponential never reaches its target, and the tune workbench
//     (cg_adsTune / adssave) assumes a steady ADS hold applies the dialled value EXACTLY. Without
//     the snap every captured per-gun tune would be silently off by ~0.2%.
//   * a zero-length frame is SKIPPED, not snapped. The idiom this replaces was
//     `(cg.frametime > 0) ? dt*rate : 1.0f`, which jumps straight to the target on a zero-dt frame -
//     and cg.frametime is 0 whenever CL_AdjustTimeDelta nudges server time backwards, i.e. exactly
//     during the packet-loss jitter where a snap is least wanted.
static float s_adsFactorCur  = 0.0f;
static float s_adsCrouchCur  = 0.0f;
static int   s_adsFactorTime = -1;

void CG_AdsFactorAdvance(void)
{
    float tgt, rate, st, dt;

    if (s_adsFactorTime == cg.time) {
        return; // already advanced this frame
    }
    s_adsFactorTime = cg.time;
    if (cg.frametime <= 0) {
        return; // skip, never snap
    }
    dt = (float)cg.frametime / 1000.0f;

    // ADS pose. Turrets excluded: the server owns that camera and STAT_CLIPAMMO/activeItems are the
    // turret's while manning one, so the per-gun tune lookup would miss anyway.
    tgt = 0.0f;
    if (cg.snap && CG_AimingDownSights() && !(cg.snap->ps.pm_flags & PMF_TURRET)) {
        tgt = 1.0f;
    }
    rate = (tgt > s_adsFactorCur) ? 15.0f : 8.5f;
    // [user 2026-08-27] and a heavy weapon takes longer to get there. Only the RAISE is slowed -
    // dropping out of the sights is the shooter letting go, which weight does not much affect.
    {
        static cvar_t *pHeft = NULL;
        if (!pHeft) { pHeft = cgi.Cvar_Get("coop_adsHeft", "0.45", CVAR_ARCHIVE); }
        if (tgt > s_adsFactorCur && pHeft->value > 0.0f) {
            rate *= 1.0f - CoopGunHeft() * pHeft->value;
            if (rate < 3.0f) { rate = 3.0f; }
        }
    }
    st   = dt * rate;
    if (st > 1.0f) {
        st = 1.0f; // MANDATORY: the frame clamp is 5s for a client of a remote server
    }
    s_adsFactorCur += (tgt - s_adsFactorCur) * st;
    if (fabs(s_adsFactorCur - tgt) < 0.002f) {
        s_adsFactorCur = tgt;
    }

    // CROUCH blend. PMF_DUCKED is binary, and the crouch tune is large - up to 38.5 degrees of yaw
    // on the Garand, 43 on the shotgun - so crouching or standing WHILE AIMING used to apply or
    // remove the whole correction in a single frame. Ease it.
    tgt = (cg.predicted_player_state.pm_flags & PMF_DUCKED) ? 1.0f : 0.0f;
    st  = dt * 10.0f;
    if (st > 1.0f) {
        st = 1.0f;
    }
    s_adsCrouchCur += (tgt - s_adsCrouchCur) * st;
    if (fabs(s_adsCrouchCur - tgt) < 0.002f) {
        s_adsCrouchCur = tgt;
    }
}

// HZM coop [user 2026-08-27] PER-GUN HANDLING, WITHOUT INVENTING A SINGLE NUMBER.
//
// Sixty-nine weapons shared one aim-in speed and one recoil weight per class, so a Luger came up
// exactly as fast as a Panzerschreck. The obvious fix is a hand-tuned handling column per gun - 69
// judgement calls, every one of them mine and none of them authoritative.
//
// There is better data already in the box. The recoil table extracted from the retail TIKs is the
// original developers' own statement of how violent each weapon is, and heft and recoil are the same
// physical fact: the shotgun they gave 20 degrees of climb is the gun that should be slowest to the
// shoulder, and the Sten they gave 0.9 is the fastest. So handling is DERIVED from their numbers
// rather than guessed at from mine - one table, two systems, and nothing to keep in sync by hand.
float CoopGunHeft(void)
{
    // The SERVER owns the lookup and publishes the result, because only it has the weapon's model
    // name - the client sees the display name ("Mauser KAR 98K") while the table is keyed by the
    // file name ("kar98"), and normalising between the two would be a guess that silently fails on
    // whichever guns do not follow the pattern. Same change-only channel the cover and brace state
    // already use, so it costs one stufftext per weapon change.
    static cvar_t *pHeft = NULL;
    float          v;

    if (!pHeft) { pHeft = cgi.Cvar_Get("coop_gunHeft", "0", 0); }
    v = (float)pHeft->integer * 0.01f;
    if (v < 0.0f) { v = 0.0f; } else if (v > 1.0f) { v = 1.0f; }
    return v;
}
float CG_AdsPoseFactor(void)
{
    return s_adsFactorCur;
}

float CG_AdsCrouchBlend(void)
{
    return s_adsCrouchCur;
}

void CG_DrawActiveFrame(int serverTime, int frameTime, stereoFrame_t stereoView, qboolean demoPlayback)
{
    cg.time         = serverTime;
    cg.frametime    = frameTime;
    cg.demoPlayback = demoPlayback;

    // HZM coop bug-1502 - run every frame regardless of view/weapon state (see function banner).
    CG_AdsFactorAdvance(); // must precede every consumer - see the banner on the function
    CG_FeelStressAdvance(); // one shared "how rattled is the player" scalar, same rule
    CG_GunBloodDecay();     // blood on the weapon fades, and much faster in the rain
    CG_ActionFoleyThink();  // the mechanical layer, a few tens of ms behind the shot
    CoopGunFoleyThink();    // handling foley: sprint, crouch, ADS, switch, dry fire
    CG_CoopDaylightThink(); // time-of-day grade (see banner) - cheap, early-outs when unchanged
    CG_CoopHeadshotCueThink(); // shooter-only headshot cue (see banner)
    CG_UpdateScriptedAudioDucks();
    // HZM coop bug-1508 - throttled internally, safe to call every frame (see function banner).
    CG_SyncWussPk3Count();

    // any looped sounds will be respecified as entities
    // are added to the render list
    cgi.S_ClearLoopingSounds();

    // clear all the render lists
    cgi.R_ClearScene();

    // set up cg.snap and possibly cg.nextSnap
    CG_ProcessSnapshots();

    // if we haven't received any snapshots yet, all
    // we can draw is the information screen
    if (!cg.snap || (cg.snap->snapFlags & SNAPFLAG_NOT_ACTIVE)) {
        return;
    }
    CG_RagdollFrame(); // HZM coop - ragdoll: step sims + push poses BEFORE entities are added


    // this counter will be bumped for every valid scene we generate
    cg.clientFrame++;

    // set cg.frameInterpolation
    if (cg.nextSnap && r_lerpmodels->integer) {
        int delta;

        delta = (cg.nextSnap->serverTime - cg.snap->serverTime);
        if (delta == 0) {
            cg.frameInterpolation = 0;
        } else {
            cg.frameInterpolation = (float)(cg.time - cg.snap->serverTime) / delta;
        }
    } else {
        cg.frameInterpolation = 0; // actually, it should never be used, because
        // no entities should be marked as interpolating
    }

    //
    // Added in OPM
    //  Clamp the fov to avoid artifacts
    if (cg_fov->value < 65) {
        cgi.Cvar_Set("cg_fov", "65");
    } else if (cg_fov->value > 120) {
        cgi.Cvar_Set("cg_fov", "120");
    }

    // update cg.predicted_player_state
    CG_PredictPlayerState();

    // build cg.refdef
    CG_CalcViewValues();

    // display the intermission
    if (cg.snap->ps.pm_flags & PMF_INTERMISSION) {
        if (cgs.gametype != GT_SINGLE_PLAYER) {
            CG_ScoresDown_f();
        } else if (cg.bIntermissionDisplay) {
            if (cg.nextSnap) {
                if (cg_protocol >= PROTOCOL_MOHTA_MIN) {
                    cvar_t* pMission = cgi.Cvar_Get("g_mission", "", CVAR_ARCHIVE);

                    if (cgi.Cvar_Get("g_success", "", 0)->integer) {
                        switch (pMission->integer)
                        {
                        default:
                        case 0:
                            cgi.UI_ShowMenu("mission_success_1", 0);
                            cgi.Cvar_Set("g_t2l1", "1");
                            break;
                        case 2:
                            cgi.UI_ShowMenu("mission_success_2", 0);
                            cgi.Cvar_Set("g_t3l1", "1");
                            break;
                        case 3:
                            cgi.UI_ShowMenu("mission_success_3", 0);
                            break;
                        }
                    } else {
                        switch (pMission->integer)
                        {
                        default:
                        case 0:
                            cgi.UI_ShowMenu("mission_failed_1", 0);
                            break;
                        case 2:
                            cgi.UI_ShowMenu("mission_failed_2", 0);
                            break;
                        case 3:
                            cgi.UI_ShowMenu("mission_failed_3", 0);
                            break;
                        }
                    }
                } else {
                    if (cgi.Cvar_Get("g_success", "", 0)->integer) {
                        cgi.UI_ShowMenu("StatsScreen_Success", qfalse);
                    } else {
                        cgi.UI_ShowMenu("StatsScreen_Failed", qfalse);
                    }
                }
            }
        } else {
            cgi.SendClientCommand("stats");
        }

        cg.bIntermissionDisplay = qtrue;
    } else if (cg.bIntermissionDisplay) {
        if (cgs.gametype != GT_SINGLE_PLAYER) {
            CG_ScoresUp_f();
        } else {
            if (cg_protocol >= PROTOCOL_MOHTA_MIN) {
                cvar_t* pMission = cgi.Cvar_Get("g_mission", "", CVAR_ARCHIVE);

                if (cgi.Cvar_Get("g_success", "", 0)->integer) {
                    switch (pMission->integer)
                    {
                    default:
                    case 0:
                        cgi.UI_HideMenu("mission_success_1", qtrue);
                        break;
                    case 2:
                        cgi.UI_HideMenu("mission_success_2", qtrue);
                        break;
                    case 3:
                        cgi.UI_HideMenu("mission_success_3", qtrue);
                        break;
                    }
                } else {
                    switch (pMission->integer)
                    {
                    default:
                    case 0:
                        cgi.UI_HideMenu("mission_failed_1", qtrue);
                        break;
                    case 2:
                        cgi.UI_HideMenu("mission_failed_2", qtrue);
                        break;
                    case 3:
                        cgi.UI_HideMenu("mission_failed_3", qtrue);
                        break;
                    }
                }
            } else {
                if (cgi.Cvar_Get("g_success", "", 0)->integer) {
                    cgi.UI_HideMenu("StatsScreen_Success", qtrue);
                } else {
                    cgi.UI_HideMenu("StatsScreen_Failed", qtrue);
                }
            }
        }

        cg.bIntermissionDisplay = qfalse;
    } else {
        // Added in OPM
        //  In vanilla, scores are updated because when pressing a key,
        //  it is sent every frame.
        //  In OPM, an event for a key is sent only once, even when it's being held.
        if (cgs.gametype != GT_SINGLE_PLAYER && cg.showScores) {
            CG_ScoresDown_f();
        }
    }

    // Added in OPM
    CG_ProcessPlayerModel();

    // build the render lists
    if (!cg.hyperspace) {
        CG_AddPacketEntities(); // after calcViewValues, so predicted player state is correct
        CG_AddMarks();
    }

    // finish up the rest of the refdef
    CG_SetupPortalSky();

    // HZM coop [user 2026-08-21] ADS TRANSITION TRACE - coop_adsTrace 1.
    //
    // Nine attempts at the "leaving ADS jolts" report have now failed, every one of them reasoned
    // from static reading. Two of those attempts were built on premises that turned out to be false
    // (that ADS is the `charge` animation - only 24 of 481 weapon tikis declare one, none of them a
    // rifle; and that the third-person min-distance fallback was firing - which cannot even run for a
    // player whose cg_3rd_person is 0). Stop theorising and MEASURE.
    //
    // This runs at the very end of the frame, after CG_AddPacketEntities has let
    // CG_OffsetFirstPersonView rebuild the view, so cg.refdef.vieworg is final. It records the
    // frame-to-frame movement of the actual camera. Whatever frame the jolt happens on, dPos spikes
    // on that frame - that is true regardless of which subsystem caused it, which is the entire point
    // of measuring instead of guessing. Everything else on the line is context to identify the cause.
    if (cg.snap) {
        static cvar_t *pTrace = NULL;
        static vec3_t  s_vPrevView = {0, 0, 0};
        static vec3_t  s_vPrevVM   = {0, 0, 0};
        static float   s_fPrevYaw = 0.0f, s_fPrevPitch = 0.0f;
        static qboolean s_bPrev = qfalse;

        if (!pTrace) { pTrace = cgi.Cvar_Get("coop_adsTrace", "0", 0); }
        if (pTrace->integer) {
            vec3_t   vD;
            float    dPos, dYaw, dPitch;
            qboolean bAds = CG_AimingDownSights();

            VectorSubtract(cg.refdef.vieworg, s_vPrevView, vD);
            dPos   = VectorLength(vD);
            dYaw   = cg.refdefViewAngles[YAW]   - s_fPrevYaw;
            dPitch = cg.refdefViewAngles[PITCH] - s_fPrevPitch;
            while (dYaw >  180.0f) { dYaw -= 360.0f; }
            while (dYaw < -180.0f) { dYaw += 360.0f; }

            // print while ADS is held, and for the whole release transition; a big dPos while
            // standing still with no input is the jolt frame.
            // [2026-08-21] QUIET MODE. The previous version printed EVERY frame, which with
            // logfile 2 (flush per line) is ~70 synchronous disk writes a second - and the thing it
            // was measuring was frame hitches. The instrument was a suspect in its own reading.
            // Now it prints only on a HITCH or an ANIM CHANGE: a few dozen lines instead of 1537, so
            // if reload still stalls at near-zero logging overhead, that result means something.
            {
                static int s_iPrevAnim = -1;
                static int s_iPrevMask = -1;
                int        iNowAnim    = cg.snap->ps.iViewModelAnim;
                float      fP          = CG_AdsPoseFactor();
                qboolean   bHitch      = (cg.frametime > 55) ? qtrue : qfalse;
                qboolean   bAnimEdge   = (iNowAnim != s_iPrevAnim) ? qtrue : qfalse;
                qboolean   bSurfEdge   = (g_iCoopSurfMask != s_iPrevMask) ? qtrue : qfalse;
                /* EVERY frame while an ADS transition is live - that is the window the jolt is in,
                   and it is short, so the volume stays sane while nothing is missed inside it. */
                qboolean   bInTrans    = (fP > 0.004f && fP < 0.996f) ? qtrue : qfalse;

                s_iPrevAnim = iNowAnim;
                s_iPrevMask = g_iCoopSurfMask;
                if (bHitch || bAnimEdge || bSurfEdge || bInTrans) {
                {
                    vec3_t vVM;
                    float  dVM = 0.0f;

                    if (s_bTraceVMok) {
                        vec3_t vD2;
                        VectorSubtract(s_vTraceVM, s_vPrevVM, vD2);
                        dVM = VectorLength(vD2);
                    }
                    (void)vVM;
                    // Bone positions RELATIVE TO THE EYES BONE. The camera sits on that bone, so
                    // these are literally "how far is my body from my viewpoint" - and the reported
                    // symptom is relative ("body jumps upwards but camera stays put"), so a step in
                    // THESE is the jolt. Which of the three moves says whether it is torso, arms or
                    // weapon, which none of the earlier traces could distinguish.
                    {
                        vec3_t vSp, vHd, vWp;

                        VectorSubtract(s_vTraceBone[1], s_vTraceBone[0], vSp);
                        VectorSubtract(s_vTraceBone[2], s_vTraceBone[0], vHd);
                        VectorSubtract(s_vTraceBone[3], s_vTraceBone[0], vWp);
                        {
                            vec3_t vLC, vLA, vRC, vRA;

                            VectorSubtract(s_vTraceBone[4], s_vTraceBone[0], vLC);
                            VectorSubtract(s_vTraceBone[5], s_vTraceBone[0], vLA);
                            VectorSubtract(s_vTraceBone[6], s_vTraceBone[0], vRC);
                            VectorSubtract(s_vTraceBone[7], s_vTraceBone[0], vRA);
                            cgi.Printf("^~^~^ ADSTRACE t=%d dt=%d ads=%d pose=%.3f vmanim=%d "
                                       "surf=0x%03x fovx=%.2f yaw=%.2f pit=%.2f dPos=%.2f dVM=%.2f "
                                       "spZ=%.2f hdZ=%.2f gnZ=%.2f "
                                       "LclavZ=%.2f LarmZ=%.2f RclavZ=%.2f RarmZ=%.2f "
                                       "Larm=%.1f,%.1f,%.1f Rarm=%.1f,%.1f,%.1f\n",
                                       cg.time, cg.frametime, (int)bAds, CG_AdsPoseFactor(),
                                       cg.snap->ps.iViewModelAnim, g_iCoopSurfMask,
                                       cg.refdef.fov_x, cg.refdefViewAngles[YAW],
                                       cg.refdefViewAngles[PITCH], dPos, dVM,
                                       vSp[2], vHd[2], vWp[2],
                                       vLC[2], vLA[2], vRC[2], vRA[2],
                                       vLA[0], vLA[1], vLA[2], vRA[0], vRA[1], vRA[2]);
                        }
                    }
                    VectorCopy(s_vTraceVM, s_vPrevVM);
                }
                }
            }
            s_bPrev = bAds;
        }
        VectorCopy(cg.refdef.vieworg, s_vPrevView);
        s_fPrevYaw   = cg.refdefViewAngles[YAW];
        s_fPrevPitch = cg.refdefViewAngles[PITCH];
    }

    cg.refdef.time = cg.time;
    memcpy(cg.refdef.areamask, cg.snap->areamask, sizeof(cg.refdef.areamask));

    // update audio positions
    cgi.S_Respatialize(cg.snap->ps.clientNum, cg.SoundOrg, cg.SoundAxis);

    // make sure the lagometerSample and frame timing isn't done twice when in stereo
    if (stereoView != STEREO_RIGHT) {
        CG_AddLagometerFrameInfo();
    }

    CG_UpdateTestEmitter();
    CG_AddPendingEffects();

    if (!cg_hidetempmodels->integer) {
        CG_AddTempModels();
    }

    if (vss_draw->integer) {
        CG_AddVSSSources();
    }

    CG_AddBulletTracers();
    CG_AddBulletImpacts();
    CG_AddBeams();
    CG_AddCoopDynamicLights(); // HZM coop - transient muzzle/explosion dlights
    CG_UpdateEnvReverb();       // HZM coop - auto indoor/outdoor reverb (where the map sets none)

    if (cg_acidtrip->integer) {
        // lol disco
        CG_AddLightShow();
    }

    // actually issue the rendering calls
    CG_DrawActive(stereoView);

    if (cg_stats->integer) {
        cgi.Printf("cg.clientFrame:%i\n", cg.clientFrame);
    }
}
