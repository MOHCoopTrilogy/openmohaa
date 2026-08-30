/*
===========================================================================
Copyright (C) 2024 the OpenMoHAA team

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

// player_animation.cpp: Animation utility functions
//

#include "player.h"
#include "g_phys.h"
#include "portableturret.h"

extern Event EV_Player_AnimLoop_Torso;
extern Event EV_Player_AnimLoop_Legs;

void Player::EndAnim_Legs(Event *ev)
{
    animdone_Legs = true;

    if (IsRepeatType(m_iPartSlot[legs])) {
        SetAnimDoneEvent(EV_Player_AnimLoop_Legs, m_iPartSlot[legs]);
    }

    EvaluateState();
}

void Player::EndAnim_Torso(Event *ev)
{
    animdone_Torso = true;

    if (IsRepeatType(m_iPartSlot[torso])) {
        SetAnimDoneEvent(EV_Player_AnimLoop_Torso, m_iPartSlot[torso]);
    }

    EvaluateState();
}

void Player::EndAnim_Pain(Event *ev)
{
    animdone_Pain = true;
}

void Player::SetPartAnim(const char *anim, bodypart_t slot)
{
    int animnum;

    if (getMoveType() == MOVETYPE_NOCLIP) {
        if (slot) {
            StopPartAnimating(torso);
            return;
        }

        anim = "idle";
    }

    animnum = gi.Anim_NumForName(edict->tiki, anim);
    if (animnum == CurrentAnim(m_iPartSlot[slot]) && partAnim[slot] == anim) {
        return;
    }

    if (animnum == -1) {
        Event *ev;

        if (slot) {
            ev = new Event(EV_Player_AnimLoop_Torso);
        } else {
            ev = new Event(EV_Player_AnimLoop_Legs);
        }

        PostEvent(ev, level.frametime);
        gi.DPrintf("^~^~^ Warning: Can't find player animation '%s'.\n", anim);

        return;
    }

    if (m_fPartBlends[slot] < 0.5f) {
        SetAnimDoneEvent(NULL, m_iPartSlot[slot]);

        partBlendMult[slot] = gi.Anim_CrossTime(edict->tiki, animnum);
        if (partBlendMult[slot] > 0.0f) {
            m_iPartSlot[slot] ^= 1;
            partBlendMult[slot] = 1.0f / partBlendMult[slot];
            partOldAnim[slot]   = partAnim[slot];
            m_fPartBlends[slot] = 1.0f;
        } else {
            partOldAnim[slot]   = "";
            m_fPartBlends[slot] = 0.0f;
        }
    }

    if (slot != legs) {
        animdone_Torso = false;
    } else {
        animdone_Legs = false;
    }

    edict->s.frameInfo[m_iPartSlot[slot]].index = gi.Anim_NumForName(edict->tiki, "idle");

    partAnim[slot] = anim;

    if (slot != legs) {
        NewAnim(animnum, EV_Player_AnimLoop_Torso, m_iPartSlot[slot]);
    } else {
        NewAnim(animnum, EV_Player_AnimLoop_Legs, m_iPartSlot[legs]);
    }

    RestartAnimSlot(m_iPartSlot[slot]);
}

static float g_fPartBlendTime[2] = {0.2f, 0.2f};

void Player::StopPartAnimating(bodypart_t part)
{
    if (partAnim[part] == "") {
        return;
    }

    if (m_fPartBlends[part] < 0.5f) {
        SetAnimDoneEvent(NULL, m_iPartSlot[part]);

        m_iPartSlot[part] ^= 1;
        partOldAnim[part]   = partAnim[part];
        m_fPartBlends[part] = 1.0f;
    }

    partAnim[part]      = "";
    partBlendMult[part] = 1.0f / g_fPartBlendTime[part];

    StopAnimating(m_iPartSlot[part]);

    if (part != legs) {
        animdone_Torso = false;
    } else {
        animdone_Legs = false;
    }
}

void Player::PausePartAnim(bodypart_t part)
{
    Pause(m_iPartSlot[part], 1);
    Pause(m_iPartSlot[part] ^ 1, 1);
}

int Player::CurrentPartAnim(bodypart_t part) const
{
    if (partAnim[part] == "") {
        return -1;
    }

    return CurrentAnim(m_iPartSlot[part]);
}

void Player::AdjustAnimBlends(void)
{
    int      iPartSlot;
    int      iOldPartSlot;
    float    fWeightTotal;
    qboolean playTurretAnim = false;

    if (movetype == MOVETYPE_PORTABLE_TURRET) {
        PortableTurret *turret = static_cast<PortableTurret *>(m_pTurret.Pointer());

        if (turret->IsReloading()) {
            if (partAnim[legs] != "mg42tripod_reload") {
                SetPartAnim("mg42tripod_reload", legs);
                StopAnimating(m_iPartSlot[torso]);
                StopAnimating(m_iPartSlot[torso] ^ 1);
                partAnim[torso] = "";
            }
        } else if (turret->IsSettingUp()) {
            if (partAnim[legs] != "mg42tripod_setup") {
                SetPartAnim("mg42tripod_setup", legs);
                StopAnimating(m_iPartSlot[torso]);
                StopAnimating(m_iPartSlot[torso] ^ 1);
                partAnim[torso] = "";
            }
        } else if (turret->IsPackingUp()) {
            if (partAnim[legs] != "mg42tripod_packup") {
                SetPartAnim("mg42tripod_packup", legs);
                StopAnimating(m_iPartSlot[torso]);
                StopAnimating(m_iPartSlot[torso] ^ 1);
                partAnim[torso] = "";
            }
        } else {
            playTurretAnim = true;
        }
    }

    if (playTurretAnim) {
        PortableTurret *turret = static_cast<PortableTurret *>(m_pTurret.Pointer());

        if (partAnim[legs] != "mg42tripod_aim") {
            StopPartAnimating(legs);
        }

        if (partAnim[torso] != "mg42tripod_aim") {
            StopPartAnimating(torso);
        }

        partAnim[legs]  = "mg42tripod_aim";
        partAnim[torso] = "mg42tripod_aim";

        float pitch, yaw;
        float legsPitchAngle, torsoPitchAngle;
        float pitchAlpha, yawAlpha;
        float legsYawAngle, torsoYawAngle;
        float weight;
        int   animNum;
        str   vertLegsStr, vertTorsoStr;
        str   horzLegsStr, horzTorsoStr;
        str   anim;

        pitch = AngleSubtract(turret->GetGroundPitch(), turret->angles[0]);
        yaw   = AngleSubtract(turret->GetStartYaw(), turret->angles[1]);

        if (pitch >= 0) {
            legsPitchAngle  = 0.0;
            torsoPitchAngle = 15.0;
            vertLegsStr     = "_straight";
            vertTorsoStr    = "_up15";
        } else {
            legsPitchAngle  = -15.0;
            torsoPitchAngle = 0.0;
            vertLegsStr     = "_down15";
            vertTorsoStr    = "_straight";
        }

        pitchAlpha = (pitch - legsPitchAngle) / (torsoPitchAngle - legsPitchAngle);
        pitchAlpha = Q_clamp_float(pitchAlpha, 0, 1);

        if (yaw < -20) {
            legsYawAngle  = -40.0;
            torsoYawAngle = -20.0;
            horzLegsStr   = "_left40";
            horzTorsoStr  = "_left20";
        } else if (yaw < 0) {
            legsYawAngle  = -20.0;
            torsoYawAngle = 0.0;
            horzLegsStr   = "_left20";
            horzTorsoStr  = "_straight";
        } else if (yaw <= 20) {
            legsYawAngle  = 0.0;
            torsoYawAngle = 20.0;
            horzLegsStr   = "_straight";
            horzTorsoStr  = "_right20";
        } else {
            legsYawAngle  = 20.0;
            torsoYawAngle = 40.0;
            horzLegsStr   = "_right20";
            horzTorsoStr  = "_right40";
        }

        yawAlpha = (yaw - legsYawAngle) / (torsoYawAngle - legsYawAngle);
        yawAlpha = Q_clamp_float(yawAlpha, 0, 1);

        anim    = "mg42tripod_aim" + horzLegsStr + vertLegsStr;
        weight  = (1 - pitchAlpha) * (1 - yawAlpha);
        animNum = gi.Anim_NumForName(edict->tiki, anim.c_str());
        if (animNum == -1) {
            gi.DPrintf("^~^~^ Warning: Can't find player animation '%s'.\n", anim.c_str());
            anim    = "mg42tripod_aim_straight_straight";
            animNum = gi.Anim_NumForName(edict->tiki, anim.c_str());
        }

        if (animNum == CurrentAnim(0)) {
            SetWeight(0, weight);
        } else {
            NewAnim(animNum, 0, weight);
        }

        anim    = "mg42tripod_aim" + horzLegsStr + vertTorsoStr;
        weight  = (1.0 - yawAlpha) * pitchAlpha;
        animNum = gi.Anim_NumForName(edict->tiki, anim.c_str());
        if (animNum == -1) {
            gi.DPrintf("^~^~^ Warning: Can't find player animation '%s'.\n", anim.c_str());
            anim    = "mg42tripod_aim_straight_straight";
            animNum = gi.Anim_NumForName(edict->tiki, anim.c_str());
        }

        if (animNum == CurrentAnim(1)) {
            SetWeight(1, weight);
        } else {
            NewAnim(animNum, 1, weight);
        }

        anim    = "mg42tripod_aim" + horzTorsoStr + vertTorsoStr;
        weight  = pitchAlpha * yawAlpha;
        animNum = gi.Anim_NumForName(edict->tiki, anim.c_str());
        if (animNum == -1) {
            gi.DPrintf("^~^~^ Warning: Can't find player animation '%s'.\n", anim.c_str());
            anim    = "mg42tripod_aim_straight_straight";
            animNum = gi.Anim_NumForName(edict->tiki, anim.c_str());
        }

        if (animNum == CurrentAnim(2)) {
            SetWeight(2, weight);
        } else {
            NewAnim(animNum, 2, weight);
        }

        anim    = "mg42tripod_aim" + horzTorsoStr + vertTorsoStr;
        weight  = (1.0 - pitchAlpha) * yawAlpha;
        animNum = gi.Anim_NumForName(edict->tiki, anim.c_str());
        if (animNum == -1) {
            gi.DPrintf("^~^~^ Warning: Can't find player animation '%s'.\n", anim.c_str());
            anim    = "mg42tripod_aim_straight_straight";
            animNum = gi.Anim_NumForName(edict->tiki, anim.c_str());
        }

        if (animNum == CurrentAnim(3)) {
            SetWeight(3, weight);
        } else {
            NewAnim(animNum, 3, weight);
        }
    }

    if (deadflag == DEAD_DEAD) {
        if (m_fPainBlend) {
            StopAnimating(ANIMSLOT_PAIN);
            SetWeight(ANIMSLOT_PAIN, 0);
            m_fPainBlend  = 0;
            animdone_Pain = false;
        }
        return;
    }

    iPartSlot    = m_iPartSlot[legs];
    iOldPartSlot = m_iPartSlot[legs] ^ 1;

    if (m_fPartBlends[legs] <= 0.0f) {
        if (partOldAnim[legs] != "") {
            StopAnimating(iOldPartSlot);
        }

        if (partAnim[legs] == "") {
            SetWeight(iPartSlot, 0.0);
        } else {
            SetWeight(iPartSlot, 1.0);
        }
    } else {
        m_fPartBlends[legs] -= level.frametime * partBlendMult[legs];
        if (m_fPartBlends[legs] >= 0.01f) {
            if (partOldAnim[legs] != "") {
                SetWeight(iOldPartSlot, m_fPartBlends[legs]);
            }
            if (partAnim[legs] != "") {
                SetWeight(iPartSlot, 1.0f - m_fPartBlends[legs]);
            }
        } else {
            m_fPartBlends[legs] = 0.0f;
            StopAnimating(iOldPartSlot);
            partOldAnim[legs] = "";

            if (partAnim[legs] == "") {
                SetWeight(iPartSlot, 0.0);
            } else {
                SetWeight(iPartSlot, 1.0);
            }
        }
    }

    iPartSlot    = m_iPartSlot[torso];
    iOldPartSlot = m_iPartSlot[torso] ^ 1;

    if (m_fPartBlends[torso] <= 0.0f) {
        if (partOldAnim[torso] != "") {
            StopAnimating(iOldPartSlot);
            partOldAnim[torso] = "";
        }

        if (partAnim[torso] == "") {
            SetWeight(iPartSlot, 0.0);
            edict->s.actionWeight = 0.0;
        } else {
            SetWeight(iPartSlot, 1.0);
            edict->s.actionWeight = 1.0;
        }
    } else {
        m_fPartBlends[torso] = m_fPartBlends[torso] - level.frametime * partBlendMult[torso];
        if (m_fPartBlends[torso] >= 0.01f) {
            fWeightTotal = 0.0f;

            if (partOldAnim[torso] != "") {
                SetWeight(iOldPartSlot, m_fPartBlends[torso]);
                fWeightTotal += m_fPartBlends[torso];
            }
            if (partAnim[torso] != "") {
                SetWeight(iPartSlot, 1.0 - m_fPartBlends[torso]);
                fWeightTotal += 1.0 - m_fPartBlends[torso];
            }

            edict->s.actionWeight = fWeightTotal;
        } else {
            m_fPartBlends[torso] = 0.0f;
            StopAnimating(iOldPartSlot);
            partOldAnim[torso] = "";

            if (partAnim[torso] == "") {
                SetWeight(iPartSlot, 0.0);
                edict->s.actionWeight = 0.0;
            } else {
                SetWeight(iPartSlot, 1.0);
                edict->s.actionWeight = 1.0;
            }
        }
    }

    // HZM coop [user 2026-08-26] PRONE RELOAD - keep the body flat by zeroing the torso action's
    // RENDER weight, while letting the real per-weapon reload animation run untouched.
    //
    // A RELOAD IS DRIVEN BY THE ANIMATION, NOT BY THE STATEMAP. The player's reload alias carries the
    // server notetracks that do the work: `first reloadweapon` -> Sentient::ReloadWeapon ->
    // Weapon::StartReloading, and `N weaponcommand mainhand clip_fill` -> Weapon::FillAmmoClip, which
    // refills ammo_in_clip and clears ShouldReload. Substituting a different animation drops both.
    // That is what broke the two previous attempts: coop_prone_reload was a bare alias with no
    // notetracks, so the clip never refilled, and Weapon::ShouldReload latches TRUE on an empty clip
    // independently of its own flag (weapon.cpp:3980) - hence 'stuck in the reload and cannot shoot'.
    // The clip really was empty (bug-2113, bug-2109).
    //
    // Weight is safe to touch where the animation is not. Notetracks are queued ONCE by
    // Animate::NewAnim when the animation is set, and ANIMDONE is driven purely by elapsed time, so
    // neither depends on weight - the reload's DURATION is therefore bit-for-bit unchanged, which the
    // substitution approach could never promise (pistol_prone_reload is 1.50s against the Kar98's
    // 3.37s - a 55% speed-up).
    //
    // Zero weight is not a new look: STAND already uses `none : default`, which is StopPartAnimating
    // on the torso, and the prone legs animation is full-body (73 channels - spine, clavicles, arms,
    // hands, both weapon tags). This reproduces the prone pose that already ships.
    //
    // frameInfo weight is networked, so this reaches every client with no cgame change.
    if (m_bCoopProne && (client->ps.pm_flags & PMF_VIEW_PRONE) && currentState_Torso) {
        static cvar_t *pPR = NULL;
        if (!pPR) { pPR = gi.Cvar_Get("coop_proneReloadFlat", "1", CVAR_ARCHIVE); }
        // [user 2026-08-26] RELOAD_PISTOL is EXEMPT: pistols now play their NATIVE prone reload
        // (the one class whose body-space prone reload ships), so hiding the torso would hide the
        // only real prone reload in the game. The revolver chains (WEBLEY/NAGANTREV) stay flat -
        // their per-round loops have no prone variant.
        // [user 2026-08-26] SUPINE holds the WHOLE body: no on-back torso animations exist, so any
        // torso action over the supine base reads as sitting up to fire - the user's one condition
        // for shipping it ('would possibly be fine as long as my torso doesn't sit up every time I
        // fire'). Same weight-zero mechanic as the prone reload: notetracks are queued when the
        // animation is SET and ANIMDONE runs on elapsed time, so rounds still fire and reloads still
        // fill while the body stays flat on its back.
        // NARROWED [user 2026-08-26]: supine aim and fire now play REAL flipped animations
        // (skc_flip.py - the prone anims rolled 180 onto their back), so hiding the whole torso
        // would hide exactly the recoil the flip earned. Only RELOADS stay held flat while on the
        // back - no flipped reload exists yet - which still honours the user's one condition:
        // firing can never sit the torso up, because the fire anim IS a lying-on-back anim now.
        // [spec A7] EXTENDED beyond reloads: every action-class prone/standing anim carries the
        // ROOT rot channel (measured - there is no harmless torso-only case), so any unflipped
        // torso action blending over the flipped base rolls the whole body ~90 degrees onto its
        // side for the anim's length. Until each gets a flipped copy (spec P2/P3), the body holds
        // flat and the alias notetracks still do the work (queued at set, time-driven): bolt
        // rechambers, weapon switches, grenade charge/throw, and reloads.
        // [pass3, bug-2126] gate widened over the flip windows: both flip-out paths clear
        // m_bCoopSupine at the START of the 0.9-1.0s roll, so a still-running hidden action
        // (grenade charge, putaway, rechamber) snapped from weight 0 to 1 unflipped over the
        // rolling body - the same ordering pass2 already fixed for the movement-zero.
        // [pass4, bug-2127] during the flip WINDOWS every torso action hides regardless of
        // prefix: firing mid-flip-out redispatches the ATTACK state, whose rows select the
        // unflipped belly anim (COOP_SUPINE already false) at weight 1 over the rolling
        // on-back body - ~90 deg sideways per shot. Notetracks still fire the rounds.
        // Settled supine keeps the narrow prefix list so flipped fire anims stay visible.
        if ((client->ps.pm_flags & PMF_VIEW_PRONE)
            && (level.time < m_fCoopSupineFlip
                || (m_bCoopSupine
            && (!Q_stricmpn(currentState_Torso->getName(), "RELOAD_", 7)
                || !Q_stricmpn(currentState_Torso->getName(), "PUTAWAY_", 8)
                || !Q_stricmpn(currentState_Torso->getName(), "RAISE_", 6)
                || !Q_stricmpn(currentState_Torso->getName(), "CHARGE_ATTACK_GRENADE", 21)
                || !Q_stricmpn(currentState_Torso->getName(), "RELEASE_ATTACK_GRENADE", 22)
                || !Q_stricmp(currentState_Torso->getName(), "ATTACK_SPRINGFIELD_RECHAMBER")
                || !Q_stricmp(currentState_Torso->getName(), "ATTACK_RIFLE_RECHAMBER"))))) {
            SetWeight(iPartSlot, 0.0f);
            SetWeight(iOldPartSlot, 0.0f);
            edict->s.actionWeight = 0.0f;
            m_sCoopHiddenTorso = partAnim[torso]; // [pass2] remember what we hid - see the tail block
        } else
        if (pPR->integer && !Q_stricmpn(currentState_Torso->getName(), "RELOAD_", 7)
            && Q_stricmp(currentState_Torso->getName(), "RELOAD_PISTOL")) {
            SetWeight(iPartSlot, 0.0f);
            SetWeight(iOldPartSlot, 0.0f); // and the crossblend-out slot
            edict->s.actionWeight = 0.0f;
            m_sCoopHiddenTorso = partAnim[torso]; // [pass3, bug-2126] this branch never latched
                // the tail cover, so every belly-prone rifle/SMG/MG reload still sat the body up
                // during its ~0.2s crossblend-out - the exact defect pass2 fixed one branch above.
        }
    }

    // [pass2, bug-2125] THE CROSSBLEND-OUT TAIL. The hide composite keys on the CURRENT torso
    // state - but when a hidden state ends, its anim moves to the OLD slot and renders at up to
    // ~0.75 weight for the ~0.2s crossblend while the current state is already STAND/aim: the
    // body visibly sat up at the END of every hidden supine action. Keep zeroing the old slot
    // while the anim we hid is the one blending out; release the latch the moment it is not.
    if (m_sCoopHiddenTorso.length()) {
        if (!(m_bCoopProne && (client->ps.pm_flags & PMF_VIEW_PRONE))) {
            // [pass4, bug-2127] stood up mid-hidden-action: the anim is legitimately visible
            // now, so its crossblend-out must not be clipped on the standing body. Flip-outs
            // keep m_bCoopProne until the deferred stand, so the latch still covers them.
            m_sCoopHiddenTorso = "";
        } else if (partOldAnim[torso] == m_sCoopHiddenTorso && m_fPartBlends[torso] > 0.0f) {
            SetWeight(m_iPartSlot[torso] ^ 1, 0.0f);
        } else if (partAnim[torso] != m_sCoopHiddenTorso) {
            m_sCoopHiddenTorso = "";
        }
    }

    if (m_fPainBlend) {
        if (m_sPainAnim == "") {
            StopAnimating(ANIMSLOT_PAIN);
            SetWeight(ANIMSLOT_PAIN, 0);
            m_fPainBlend  = 0;
            animdone_Pain = false;
        } else if (animdone_Pain) {
            m_fPainBlend -= level.frametime * (10.0 / 3.0);
            if (m_fPainBlend < 0.01f) {
                StopAnimating(ANIMSLOT_PAIN);
                SetWeight(ANIMSLOT_PAIN, 0);
                m_fPainBlend  = 0;
                animdone_Pain = false;
            }
        }

        if (m_fPainBlend) {
            int   i;
            float w;

            SetWeight(ANIMSLOT_PAIN, m_fPainBlend);
            w = 1.0 - m_fPainBlend * 0.5;

            for (i = 0; i < ANIMSLOT_PAIN; i++) {
                if (GetWeight(i)) {
                    SetWeight(i, GetWeight(i) * w);
                }
            }

            edict->s.actionWeight *= w;
        }
    }
}

void Player::PlayerAnimDelta(float *vDelta)
{
    float fTimeDelta;
    float fBackTime;
    float vNewDelta[3];
    int   animnum;

    VectorClear(vDelta);

    if (m_fLastDeltaTime >= level.time) {
        return;
    }

    fTimeDelta = level.time - m_fLastDeltaTime;

    animnum = -1;

    if (partAnim[legs] != "") {
        animnum = CurrentAnim(m_iPartSlot[legs]);
    }

    if (animnum != -1) {
        fBackTime = GetTime(m_iPartSlot[legs]) - fTimeDelta;
        if (fBackTime < 0.0f) {
            fBackTime = 0.0f;
        }

        float fTime = GetTime(m_iPartSlot[legs]);

        // get the anim delta
        gi.Anim_DeltaOverTime(edict->tiki, animnum, fBackTime, fTime, vNewDelta);

        VectorMA(vDelta, GetWeight(m_iPartSlot[legs]), vNewDelta, vDelta);
    }

    animnum = -1;

    if (partAnim[torso] != "") {
        animnum = CurrentAnim(m_iPartSlot[torso]);
    }

    if (animnum != -1) {
        fBackTime = GetTime(m_iPartSlot[torso]) - fTimeDelta;
        if (fBackTime < 0.0f) {
            fBackTime = 0.0f;
        }

        float fTime = GetTime(m_iPartSlot[torso]);

        gi.Anim_DeltaOverTime(edict->tiki, animnum, fBackTime, fTime, vNewDelta);

        VectorMA(vDelta, GetWeight(m_iPartSlot[torso]), vNewDelta, vDelta);
    }
}

void Player::EventTestAnim(Event *ev)
{
    str   name;
    float weight;
    int   animNum;

    weight = ev->GetFloat(1);
    if (weight <= 0) {
        SetWeight(ANIMSLOT_TESTANIM, 0);
        StopAnimating(ANIMSLOT_TESTANIM);
        return;
    }

    if (ev->NumArgs() > 1) {
        name    = ev->GetString(1);
        animNum = gi.Anim_NumForName(edict->tiki, name.c_str());
        if (animNum == -1) {
            gi.Printf("Couldn't find anim '%s'\n", name.c_str());
            return;
        }

        NewAnim(animNum, ANIMSLOT_TESTANIM, weight);
        RestartAnimSlot(ANIMSLOT_TESTANIM);
    }

    SetWeight(ANIMSLOT_TESTANIM, weight);
}
