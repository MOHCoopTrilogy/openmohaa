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

// actor_idle.cpp

#include "actor.h"

void Actor::InitIdle(GlobalFuncs_t *func)
{
    func->BeginState                 = &Actor::Begin_Idle;
    func->ThinkState                 = &Actor::Think_Idle;
    func->PassesTransitionConditions = &Actor::PassesTransitionConditions_Idle;
    func->IsState                    = &Actor::IsIdleState;
}

void Actor::Begin_Idle(void)
{
    m_csMood = m_csIdleMood;
    ClearPath();
}

void Actor::IdleThink(void)
{
    IdlePoint();
    IdleLook();

    if (PathExists() && PathComplete()) {
        ClearPath();
    }

    if (m_bAutoAvoidPlayer && !PathExists()) {
        // HZM coop [user 07-16]: retail hardcoded entity 0 (the SP player / listen HOST), so remote
        // coop clients could never nudge an ally out of a doorway ("paratroopers stuck standing in
        // doorways blocking you"). Yield to the NEAREST living player instead - the callee already
        // requires them to be a teammate, within 48u, and pushing toward us.
        Sentient *pNearest    = NULL;
        float     fBestDistSq = 1e30f;
        int       i;

        for (i = 0; i < game.maxclients; i++) {
            gentity_t *ed = &g_entities[i];
            if (!ed->inuse || !ed->entity || !ed->client) {
                continue;
            }
            Sentient *pl = static_cast<Sentient *>(ed->entity);
            if (pl->IsDead()) {
                continue;
            }
            float fDistSq = (pl->origin - origin).lengthSquared();
            if (fDistSq < fBestDistSq) {
                fBestDistSq = fDistSq;
                pNearest    = pl;
            }
        }
        SetPathToNotBlockSentient(pNearest);
    }

    if (PathExists()) {
        Anim_WalkTo(ANIM_MODE_PATH);
        if (PathDist() > 128.0) {
            FaceMotion();
        } else {
            IdleTurn();
        }
    } else {
        Anim_Idle();
        IdleTurn();
    }

    PostThink(true);
}

void Actor::Think_Idle(void)
{
    if (!RequireThink()) {
        return;
    }

    UpdateEyeOrigin();
    m_pszDebugState = "";

    CheckForThinkStateTransition();
    IdleThink();
}