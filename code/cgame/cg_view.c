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
static float s_reloadLift = 0.0f;

static void CG_ApplyReloadSway(vec3_t vAngles)
{
    static cvar_t *pRS = NULL;
    float          fTarget, fRate, fDt;
    int            iAnim;

    if (!pRS) {
        pRS = cgi.Cvar_Get("coop_reloadSway", "1.6", CVAR_ARCHIVE);
    }
    if (pRS->value <= 0.0f || !cg.snap) {
        s_reloadLift = 0.0f;
        return;
    }
    iAnim   = cg.snap->ps.iViewModelAnim;
    fTarget = (iAnim >= 6 && iAnim <= 8) ? pRS->value : 0.0f; // VM_ANIM_RELOAD / _SINGLE / _END
    fDt     = cg.frametime * 0.001f;
    fRate   = (fTarget > s_reloadLift) ? 7.0f : 4.5f; // heft up quicker than it settles
    s_reloadLift += (fTarget - s_reloadLift) * fRate * fDt;
    vAngles[0] -= s_reloadLift;          // pitch up with the lifted gun
    vAngles[2] += s_reloadLift * 0.3f;   // a touch of roll so it reads as body motion
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

    return pCoverView->integer ? pCoverRaise->value : 0.0f;
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
    fCamSide   = cg_camerasideoffset->value;
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
        fCamDist   += (pShDist->value - fCamDist)   * s_adsShoulderEnv;
        // side target is signed: MOUSE3 swaps shoulders (s_shoulderSideSign eased in CG_UpdateAdsStage)
        fCamSide   += (pShSide->value * s_shoulderSideSign - fCamSide) * s_adsShoulderEnv;
        fCamHeight += (pShUp->value   - fCamHeight) * s_adsShoulderEnv;
        fCamVert   += (0.0f           - fCamVert)   * s_adsShoulderEnv;
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
    if (VectorLength(delta) < CAMERA_MINIMUM_DISTANCE) {
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

// HZM coop [user 08-02] - ON-HIT BLOOD intensity 0..1. Distinct from suppression: suppression is the
// sustained "under fire" state (near-misses count), this fires only when a round actually LANDS on the
// local player. Decayed in CG_CalcFov, published to the renderer as r_ppHit.
static float    s_coopHit = 0.0f;

// Bump the suppression intensity (clamped to 1). Called when an enemy round cracks past the listener.
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
qboolean CG_GetBreathState(float *outFrac, qboolean *outCooldown)
{
    cvar_t *pHold   = cgi.Cvar_Get("cg_breathHoldTime", "7", CVAR_ARCHIVE);
    int     iHoldMs = (int)((pHold ? pHold->value : 7.0f) * 1000.0f);

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
                if (CG_AimingDownSights()) {
                    cvar_t *pALR = cgi.Cvar_Get("cg_adsLeanRoll", "1.0", CVAR_ARCHIVE);
                    leanRoll *= (pALR ? pALR->value : 0.25f);
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

            // per-class recoil feel: bigger guns climb harder
            if (iClass & WEAPON_CLASS_PISTOL)      { fClassKick = 0.6f; }
            else if (iClass & WEAPON_CLASS_SMG)    { fClassKick = 0.85f; }
            else if (iClass & WEAPON_CLASS_RIFLE)  { fClassKick = 1.35f; }
            else if (iClass & WEAPON_CLASS_MG)     { fClassKick = 1.5f; }
            else if (iClass & WEAPON_CLASS_HEAVY)  { fClassKick = 1.3f; }

            // HOLD BREATH (steady aim): while ADS, holding the run/walk key (Shift / BUTTON_RUN) suppresses
            // the sway for up to cg_breathHoldTime sec, then a cg_breathCooldown-sec recharge. Shift still
            // walks normally when NOT aiming (we only READ the button). Timestamps use cg.time so the
            // recharge elapses by wall-clock even if this isn't called every frame.
            {
                cvar_t   *pHold   = cgi.Cvar_Get("cg_breathHoldTime", "7", CVAR_ARCHIVE);
                cvar_t   *pCool   = cgi.Cvar_Get("cg_breathCooldown", "5", CVAR_ARCHIVE);
                int       iHoldMs = (int)((pHold ? pHold->value : 7.0f) * 1000.0f);
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
                } else if ((bAds || bScoped) && bShift && s_breathRemainMs > 0) {
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
                VectorMA(pREnt->origin, sin(t * 1.1f) * pSway->value,        mat[1], pREnt->origin); // L/R
                VectorMA(pREnt->origin, sin(t * 1.7f + 0.6f) * pSway->value * 0.7f, mat[2], pREnt->origin); // U/D
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
                if (bAds && pRecoil && pRecoil->value > 0.0f) {
                    float fMax    = pRecoil->value * fClassKick * 6.0f;
                    float fBreath = s_breathSteady ? 0.5f : 1.0f;
                    s_recoil += pRecoil->value * fClassKick * fBreath * (float)(s_lastClip - iClip);
                    if (s_recoil > fMax) { s_recoil = fMax; }
                }
            }
            s_lastClip = iClip;
            s_lastWpn  = iWpn;

            if (s_recoil > 0.0f) {
                // gun kicks UP (muzzle climb) and BACK toward the camera, then recovers
                VectorMA(pREnt->origin,  s_recoil * 0.8f, mat[2], pREnt->origin); // up
                VectorMA(pREnt->origin, -s_recoil * 0.5f, mat[0], pREnt->origin); // back toward camera
                // framerate-independent decay back to zero
                s_recoil -= s_recoil * (cg.frametime / 1000.0f) * 9.0f;
                if (s_recoil < 0.002f) { s_recoil = 0.0f; }
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
                cvar_t *pLag    = cgi.Cvar_Get("cg_weaponLag", "0.7", CVAR_ARCHIVE);        // units of trail / deg
                cvar_t *pLagMax = cgi.Cvar_Get("cg_weaponLagMax", "3.5", CVAR_ARCHIVE);     // clamp (units)
                cvar_t *pLagStf = cgi.Cvar_Get("cg_weaponLagStiffness", "7", CVAR_ARCHIVE); // spring rate (1/s)
                cvar_t *pLagAds = cgi.Cvar_Get("cg_weaponLagADS", "0.35", CVAR_ARCHIVE);    // lag scale while ADS
                float   fGain   = pLag ? pLag->value : 0.7f;
                float   fMaxLag = pLagMax ? pLagMax->value : 3.5f;
                float   fStiff  = pLagStf ? pLagStf->value : 7.0f;
                float   fWeight = 1.0f;
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
                } else if (fGain > 0.0f && !bScoped) {
                    tX = -dYaw   * fGain * fWeight; // gun trails opposite the turn
                    tY =  dPitch * fGain * fWeight;
                    if (tX >  fMaxLag) { tX =  fMaxLag; } else if (tX < -fMaxLag) { tX = -fMaxLag; }
                    if (tY >  fMaxLag) { tY =  fMaxLag; } else if (tY < -fMaxLag) { tY = -fMaxLag; }
                    s_lagX += (tX - s_lagX) * k;
                    s_lagY += (tY - s_lagY) * k;
                } else {
                    s_lagX += (0.0f - s_lagX) * k; // scoped/disabled: ease out
                    s_lagY += (0.0f - s_lagY) * k;
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
                static float    s_spStam = 9999.0f; // client mirror of the stamina pool (seconds)
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
                usercmd_t scmd;
                float    fEnvTarget, k;

                if (fMaxStam < 0.1f) { fMaxStam = 0.1f; }
                if (s_spStam > fMaxStam) { s_spStam = fMaxStam; } // clamp mirror to current max

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

                fEnvTarget = (bWantSprint && s_spStam > 0.0f) ? 1.0f : 0.0f;
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

                if (pLower && pLower->value != 0.0f && s_spEnv > 0.001f) {
                    float fDip  = pLowAmt ? pLowAmt->value : 3.0f;
                    float fBack = pLowBack ? pLowBack->value : 1.4f;
                    float fTilt = pLowTilt ? pLowTilt->value : 1.2f;
                    // a slow run bob so the lowered gun sways with the stride
                    float fBob  = (float)sin(cg.time * 0.001f * 8.0f) * 0.25f + 1.0f; // 0.75 .. 1.25

                    VectorMA(pREnt->origin, -s_spEnv * fDip * fBob, mat[2], pREnt->origin); // dip DOWN
                    VectorMA(pREnt->origin, -s_spEnv * fBack,       mat[0], pREnt->origin); // pull BACK
                    VectorMA(pREnt->origin, -s_spEnv * fTilt,       mat[1], pREnt->origin); // slight side dip
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

                if (s_wcEnv > 0.001f) {
                    float fBack = pWcBack ? pWcBack->value : 9.0f;
                    float fDip  = pWcDip ? pWcDip->value : 4.0f;
                    VectorMA(pREnt->origin, -s_wcEnv * fBack, mat[0], pREnt->origin); // pull BACK toward camera
                    VectorMA(pREnt->origin, -s_wcEnv * fDip,  mat[2], pREnt->origin); // dip DOWN (muzzle drops)
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
        }
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
        // ease toward the target so ADS zooms in QUICKLY but smoothly (no jarring instant snap)
        step = (cg.frametime > 0) ? ((float)cg.frametime / 1000.0f) * 12.0f : 1.0f;
        if (step > 1.0f) { step = 1.0f; }
        s_adsZoomCur += (fTarget - s_adsZoomCur) * step;
        if (s_adsZoomCur > fTarget - 0.003f && s_adsZoomCur < fTarget + 0.003f) {
            s_adsZoomCur = fTarget; // settle
        }

        if (s_adsZoomCur < 0.999f) { // any zoom (including mid-transition)
            float fGunZoom = cg_adsGunZoom ? cg_adsGunZoom->value : 0.0f;
            if (fGunZoom < 0.0f) { fGunZoom = 0.0f; } else if (fGunZoom > 1.0f) { fGunZoom = 1.0f; }
            // blend the weapon fov toward the (eased) zoomed world fov by fGunZoom
            fWeaponFov = fov_x + (fov_x * s_adsZoomCur - fov_x) * fGunZoom;
            // zoom the world by the eased factor
            fov_x *= s_adsZoomCur;
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
            adsT    = (cg_adsTune && cg_adsTune->integer) ? NULL : CG_FindAdsTune(adsWpn);
            fShiftX = adsT ? adsT->sShiftX : (cg_adsShiftX ? cg_adsShiftX->value : 0.0f);
            fShiftY = adsT ? adsT->sShiftY : (cg_adsShiftY ? cg_adsShiftY->value : 0.0f);
            if (cg.predicted_player_state.pm_flags & PMF_DUCKED) {
                fShiftX += adsT ? adsT->cShiftX : (cg_adsCrouchShiftX ? cg_adsCrouchShiftX->value : 0.0f);
                fShiftY += adsT ? adsT->cShiftY : (cg_adsCrouchShiftY ? cg_adsCrouchShiftY->value : 0.0f);
            }
            cgi.Cvar_Set("r_weaponshiftx", va("%g", fShiftX));
            cgi.Cvar_Set("r_weaponshifty", va("%g", fShiftY));
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
        cvar_t    *pSuppMax = cgi.Cvar_Get("coop_health", "750", CVAR_ARCHIVE);
        int        maxH     = pSuppMax ? (int)pSuppMax->value : 750; // real coop max, so the spike is proportional to the hit
        float      dt;
        cvar_t    *pFade = cgi.Cvar_Get("coop_suppressFade", "1.4", CVAR_ARCHIVE);
        float      fade  = (pFade && pFade->value > 0.1f) ? pFade->value : 0.9f;

        if (s_lastSuppTime == 0) { s_lastSuppTime = cg.time; }
        dt = (cg.time - s_lastSuppTime) / 1000.0f;
        s_lastSuppTime = cg.time;
        if (dt < 0.0f) { dt = 0.0f; } else if (dt > 0.5f) { dt = 0.5f; }

        // taking fire = a health drop since last frame; scale the spike by how big the hit was
        if (h > 0 && maxH > 0 && s_lastSuppHealth > 0 && h < s_lastSuppHealth) {
            float lost = (float)(s_lastSuppHealth - h) / (float)maxH;
            CG_AddSuppression(0.25f + lost * 2.5f); // small flinch on any hit, scaled by severity (real max)

            // HZM coop [user 08-02] ON-HIT BLOOD spikes off the SAME health-drop detector, so there is
            // one source of truth for "I just got hit". Fast attack (straight to a level proportional to
            // the wound) then a slow decay below - a hit should register instantly and linger, unlike
            // suppression which ramps with sustained fire.
            {
                float bloodHit = 0.35f + lost * 3.0f;
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
        cg.refdefViewAngles[2] += ps->fLeanAngle * 0.1 * fLeanRollScale;
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
        if (ps->pm_flags & PMF_COOP_COVER) { cg.renderingThirdPerson = qtrue; }
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
    CG_ApplyReloadSway(cg.refdefViewAngles); // HZM coop [user 2026-08-19] reload camera follow
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

void CG_DrawActiveFrame(int serverTime, int frameTime, stereoFrame_t stereoView, qboolean demoPlayback)
{
    cg.time         = serverTime;
    cg.frametime    = frameTime;
    cg.demoPlayback = demoPlayback;

    // HZM coop bug-1502 - run every frame regardless of view/weapon state (see function banner).
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
