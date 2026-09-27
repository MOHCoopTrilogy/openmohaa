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
// playerbot_rotation.cpp: Manages bot rotation

#include "playerbot.h"

BotRotation::BotRotation()
{
    m_vAngDelta   = vec_zero;
    m_vAngSpeed   = vec_zero;
    m_vTargetAng  = vec_zero;
    m_vCurrentAng = vec_zero;
    m_bCombatTurn = false;
    m_fTurnMul    = 1.0f;
    m_bPrecise    = false;
}

void BotRotation::SetControlledEntity(Player *newEntity)
{
    controlledEntity = newEntity;
}

float AngleDifference(float ang1, float ang2)
{
    float diff;

    diff = ang1 - ang2;
    if (ang1 > ang2) {
        if (diff > 180.0) {
            diff -= 360.0;
        }
    } else {
        if (diff < -180.0) {
            diff += 360.0;
        }
    }
    return diff;
}

void BotRotation::TurnThink(usercmd_t& botcmd, usereyes_t& eyeinfo)
{
    float diff;
    float deltaDiff;
    float factor;
    float maxChange;
    float maxChangeDelta;
    float minChange;
    float changeSpeed;
    float speed;
    int   i;

    factor      = 1.0;
    maxChange   = 360;
    minChange   = 20;
    changeSpeed = 15.0;
    if (m_bPrecise) {
        // [HZM bot breach] a clearing throw needs the view ON the solved arc: the stock ramp stops turning inside 20
        // deg of the target (m_vAngSpeed decays to 0 there), so the view settled several degrees off and never met it
        minChange = 0;
    }

    // [HZM 2026-09-23] While aiming at an enemy the stock turn (close 10x the gap per second, up to 360 deg/s)
    // landed dead on target within ~0.3s - "they snap on to enemies too quickly" (user). Combat turns use a lower
    // gain and rate cap, so the crosshair visibly swings onto a target and trails a strafing one. Walking/looking
    // around keeps the stock turn. Not CVAR_ARCHIVE: an archived default freezes in omconfig.cfg.
    if (!m_bCombatTurn) {
        // [HZM bot A3] walking/looking-around turns: a touch softer than stock (360 deg/s, 10x gap/s) so heads don't
        // whip round at every path corner. bot_lookTurnMax 360 / bot_lookTurnGain 10 = stock.
        static cvar_t *s_botLookTurnGain = NULL;
        static cvar_t *s_botLookTurnMax  = NULL;
        if (!s_botLookTurnGain) {
            s_botLookTurnGain = gi.Cvar_Get("bot_lookTurnGain", "8", 0);
            s_botLookTurnMax  = gi.Cvar_Get("bot_lookTurnMax", "300", 0);
        }
        factor    = Q_clamp_float(s_botLookTurnGain->value, 1.0f, 10.0f) / 10.0f;
        maxChange = Q_clamp_float(s_botLookTurnMax->value, 90.0f, 360.0f);
    }
    if (m_bCombatTurn) {
        static cvar_t *s_botAimTurnGain = NULL;
        static cvar_t *s_botAimTurnMax  = NULL;
        if (!s_botAimTurnGain) {
            s_botAimTurnGain = gi.Cvar_Get("bot_aimTurnGain", "5", 0);
            s_botAimTurnMax  = gi.Cvar_Get("bot_aimTurnMax", "200", 0);
        }
        factor    = Q_clamp_float(s_botAimTurnGain->value, 1.0f, 10.0f) / 10.0f;
        maxChange = Q_clamp_float(s_botAimTurnMax->value, 45.0f, 360.0f);
        // [HZM bot D1] per-bot turn speed (0.85x..1.15x): some soldiers are simply quicker on the swing
        factor    = Q_clamp_float(factor * m_fTurnMul, 0.1f, 1.0f);
        maxChange = Q_clamp_float(maxChange * m_fTurnMul, 45.0f, 360.0f);
    }

    if (m_vTargetAng[PITCH] > 180) {
        m_vTargetAng[PITCH] -= 360;
    }

    for (i = 0; i < 2; i++) {
        m_vCurrentAng[i] = AngleMod(m_vCurrentAng[i]);
        m_vTargetAng[i]  = AngleMod(m_vTargetAng[i]);

        diff      = AngleDifference(m_vCurrentAng[i], m_vTargetAng[i]);
        deltaDiff = fabs(diff);

        maxChangeDelta = maxChange * level.frametime;
        if (maxChangeDelta > deltaDiff) {
            maxChangeDelta = deltaDiff;
        }

        if (deltaDiff >= minChange) {
            m_vAngSpeed[i] = Q_min(1.0, m_vAngSpeed[i] + changeSpeed * level.frametime);
            maxChangeDelta *= m_vAngSpeed[i];
        } else {
            m_vAngSpeed[i] = Q_max(0.0, m_vAngSpeed[i] - changeSpeed * level.frametime);
        }

        speed = diff * level.frametime * 10 * factor;

        m_vAngDelta[i]   = Q_clamp_float(speed, -maxChangeDelta, maxChangeDelta);
        m_vCurrentAng[i] = AngleMod(m_vCurrentAng[i] - m_vAngDelta[i]);
    }

    if (m_vCurrentAng[PITCH] > 180) {
        m_vCurrentAng[PITCH] -= 360;
    }

    eyeinfo.angles[0] = m_vCurrentAng[0];
    eyeinfo.angles[1] = m_vCurrentAng[1];
    botcmd.angles[0]  = ANGLE2SHORT(m_vCurrentAng[0]) - controlledEntity->client->ps.delta_angles[0];
    botcmd.angles[1]  = ANGLE2SHORT(m_vCurrentAng[1]) - controlledEntity->client->ps.delta_angles[1];
    botcmd.angles[2]  = ANGLE2SHORT(m_vCurrentAng[2]) - controlledEntity->client->ps.delta_angles[2];
}

/*
====================
GetTargetAngles

Return the target angle
====================
*/
const Vector& BotRotation::GetTargetAngles() const
{
    return m_vTargetAng;
}

/*
====================
SetTargetAngles

Set the bot's angle
====================
*/
void BotRotation::SetTargetAngles(Vector vAngles)
{
    m_vTargetAng = vAngles;
}

/*
====================
AimAt

Make the bot face to the specified direction
====================
*/
void BotRotation::AimAt(Vector vPos)
{
    Vector vDelta = vPos - controlledEntity->EyePosition();
    Vector vTarget;

    VectorNormalize(vDelta);
    vectoangles(vDelta, vTarget);

    SetTargetAngles(vTarget);
}
