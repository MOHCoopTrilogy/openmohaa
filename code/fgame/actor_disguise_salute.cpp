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

// actor_disguise_salute.cpp

#include "actor.h"

// HZM bug-1631 disguise tracer (defined in actor_disguise_common.cpp)
extern void CoopDisgTrace(Actor *self, const char *tag);
extern void CoopDisgTraceTick(Actor *self, const char *tag);

void Actor::InitDisguiseSalute(GlobalFuncs_t *func)
{
    func->ThinkState                 = &Actor::Think_DisguiseSalute;
    func->BeginState                 = &Actor::Begin_DisguiseSalute;
    func->EndState                   = &Actor::End_DisguiseSalute;
    func->ResumeState                = &Actor::Resume_DisguiseSalute;
    func->SuspendState               = &Actor::Suspend_DisguiseSalute;
    func->FinishedAnimation          = &Actor::FinishedAnimation_DisguiseSalute;
    func->PassesTransitionConditions = &Actor::PassesTransitionConditions_Disguise;
    func->IsState                    = &Actor::IsDisguiseState;
}

void Actor::Begin_DisguiseSalute(void)
{
    CoopDisgTrace(this, "BEGIN_SAL");
    Com_Printf("Saluting guy....\n");

    m_csMood = STRING_BORED;
    assert(m_Enemy);

    if (!m_Enemy) {
        SetThinkState(THINKSTATE_IDLE, THINKLEVEL_IDLE);
        return;
    }

    if ((EnemyIsDisguised() || (m_Enemy->flags & FL_NOTARGET)) && level.m_bAlarm != qtrue) {
        SetDesiredYawDest(m_Enemy->origin);
        SetDesiredLookDir(m_Enemy->origin - origin);

        DesiredAnimation(ANIM_MODE_NORMAL, STRING_ANIM_DISGUISE_SALUTE_SCR);
    } else {
        SetThinkState(THINKSTATE_ATTACK, THINKLEVEL_IDLE);
    }
}

void Actor::End_DisguiseSalute(void)
{
    CoopDisgTrace(this, "END_SAL");
    m_iNextDisguiseTime = level.inttime + m_iDisguisePeriod;
}

void Actor::Resume_DisguiseSalute(void)
{
    CoopDisgTrace(this, "RESUME_SAL");
    Begin_DisguiseSalute();
}

void Actor::Suspend_DisguiseSalute(void)
{
    CoopDisgTrace(this, "SUSP_SAL");
    End_DisguiseSalute();
}

void Actor::Think_DisguiseSalute(void)
{
    NoPoint();
    ContinueAnimation();
    UpdateEnemy(2000);
    CoopDisgTraceTick(this, "TICK_SAL");

    assert(m_Enemy != NULL);

    if (!m_Enemy) {
        SetThinkState(THINKSTATE_IDLE, THINKLEVEL_IDLE);
        return;
    }

    if (!EnemyIsDisguised() && !(m_Enemy->flags & FL_NOTARGET)) {
        // HZM 2026-08-11 PROBE (bug-1707 follow-up). A saluting guard turning on a player the
        // engine still reports as disguised has now been measured four times on m6l1c, always
        // from an actor named ai_alarm. EnemyIsDisguised() can return false for three unrelated
        // reasons and static reading has not settled which one fires here, so print all three
        // sub-conditions at the exact instant of the transition instead of guessing again.
        {
            Com_Printf("^~^~^ SALATK %s cacheDisg=%d liveDisg=%d force=%d think=%d threat=%d alarm=%d hasDisg=%d\n",
                       TargetName().c_str(),
                       m_bEnemyIsDisguised ? 1 : 0,
                       m_Enemy->m_bIsDisguised ? 1 : 0,
                       m_bForceAttackPlayer ? 1 : 0,
                       (int)m_ThinkState,
                       m_PotentialEnemies.GetCurrentThreat(),
                       level.m_bAlarm ? 1 : 0,
                       m_Enemy->m_bHasDisguise ? 1 : 0);
        }
        SetThinkState(THINKSTATE_ATTACK, THINKLEVEL_IDLE);
        return;
    }

    if (level.m_bAlarm == qtrue) {
        SetThinkState(THINKSTATE_ATTACK, THINKLEVEL_IDLE);
        return;
    }

    SetDesiredYawDest(m_Enemy->origin);
    SetDesiredLookDir(m_Enemy->origin - origin);

    PostThink(true);
}

void Actor::FinishedAnimation_DisguiseSalute(void)
{
    SetThinkState(THINKSTATE_IDLE, THINKLEVEL_IDLE);
}
