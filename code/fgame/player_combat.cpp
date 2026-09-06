/*
===========================================================================
Copyright (C) 2015 the OpenMoHAA team

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

// player_combat.cpp: Player combat system and combat utility functions
//

#include "player.h"
#include "weaputils.h"

static Entity *FindClosestEntityInRadius(Vector origin, Vector forward, float fov, float maxdist)
{
    float    dist, dot;
    float    fovdot = cos(fov * 0.5 * M_PI / 180.0);
    Entity  *ent;
    Entity  *bestent  = NULL;
    int      bestdist = 999999;
    qboolean valid_entity;

    // Find closest enemy in radius
    ent = findradius(NULL, origin, maxdist);

    while (ent) {
        valid_entity = false;

        if (ent->flags & FL_AUTOAIM) {
            valid_entity = true;
        }

        if (valid_entity) {
            // Check to see if the enemy is closest to us
            Vector delta = (ent->centroid) - origin;

            dist = delta.length();

            if (dist < bestdist) {
                delta.normalize();

                // It's close, now check to see if it's in our FOV.
                dot = DotProduct(forward, delta);

                if (dot > fovdot) {
                    trace_t trace;
                    // Do a trace to see if we can get to it
                    trace = G_Trace(
                        origin, vec_zero, vec_zero, ent->centroid, NULL, MASK_OPAQUE, false, "FindClosestEntityInRadius"
                    );

                    if ((trace.ent && trace.entityNum == ent->entnum) || (trace.fraction == 1)) {
                        // dir = delta;
                        bestent  = ent;
                        bestdist = dist;
                    }
                }
            }
        }
        ent = findradius(ent, origin, maxdist);
    }
    return bestent;
}

Vector Player::GunTarget(bool bNoCollision, const vec3_t position, const vec3_t forward)
{
    Vector  vForward;
    Vector  vOut;
    Vector  vDest;
    trace_t trace;
    solid_t prev_solid = SOLID_BBOX;

    if (bNoCollision) {
        AngleVectors(m_vViewAng, vForward, NULL, NULL);
        vOut = m_vViewPos + vForward * 1024.0f;

        return vOut;
    } else if (m_pVehicle) {
        AngleVectors(m_vViewAng, vForward, NULL, NULL);
        vDest = m_vViewPos + vForward * 4096.0f;

        prev_solid = m_pVehicle->edict->solid;

        m_pVehicle->setSolidType(SOLID_NOT);

        if (m_pVehicle->IsSubclassOfVehicle()) {
            m_pVehicle->SetSlotsNonSolid();
        }

        trace = G_Trace(m_vViewPos, vec_zero, vec_zero, vDest, this, MASK_OPAQUE, qfalse, "Player::GunTarget");

        vOut = trace.endpos;
    } else {
        // HZM coop [user 08-06] bug-1504 - a turret-mounted player (e.g. e2l3's finale "mortar")
        // was capped at the same 1024u trace distance as normal player aiming, while the m_pVehicle
        // branch above already correctly extends to 4096u. weapturret.cpp then triangulates the
        // barrel's aim angle as (this capped point - the turret's own pivot origin), so at range the
        // barrel visibly diverges from the crosshair: at 1024u the point is still on the correct
        // sightline from the player's EYE, but the pivot-to-point angle picks up real parallax error
        // from the eye/pivot offset - error that shrinks the FARTHER out the reference point is, so
        // capping it artificially close makes the effect worse, not better. e2l3's tank engagements
        // run 2500-5000+u, well past the old cap. Matches the vehicle case's distance exactly (same
        // constant, same reasoning) - only affects m_pTurret; normal player aiming keeps 1024u,
        // unchanged.
        float fMaxDist = m_pTurret ? 4096.0f : 1024.0f;
        AngleVectors(m_vViewAng, vForward, NULL, NULL);
        vDest = m_vViewPos + vForward * fMaxDist;

        trace = G_Trace(m_vViewPos, vec_zero, vec_zero, vDest, this, MASK_GUNTARGET, qfalse, "Player::GunTarget");

        vOut = trace.endpos;
        if (m_pTurret) {
            if ((Vector(trace.endpos) - m_vViewPos).lengthSquared() < 16384) {
                vOut = vDest;
            }
        }
    }

    if (m_pVehicle) {
        m_pVehicle->setSolidType(prev_solid);

        if (m_pVehicle->IsSubclassOfVehicle()) {
            m_pVehicle->SetSlotsSolid();
        }
    }

    return vOut;
}

void Player::PlayerReload(Event *ev)
{
    Weapon *weapon;

    if (deadflag) {
        return;
    }

    weapon = GetActiveWeapon(WEAPON_MAIN);

    if (!weapon) {
        return;
    }

    if (weapon->CheckReload(FIRE_PRIMARY)) {
        weapon->SetShouldReload(true);
    }
}

void Player::EventCorrectWeaponAttachments(Event *ev)
{
    // HZM coop [user 2026-09-04] QUICK-DRAW SIDEARM. The loop below relocates ANY weapon child from
    // tag_weapon_left onto tag_weapon_right - carrying attach_offset with it - and the parked long
    // gun legitimately lives on tag_weapon_left, so left alone this is how both guns end up fused
    // in one hand.
    //
    // EXIT FIRST, THEN LET THE CLEANUP RUN. This is deliberately NOT an early return: suppressing
    // the cleanup is exactly how a genuinely stranded gun stays stranded. By the time the loop
    // executes, CoopQDrawExit has already put the primary back on tag_weapon_right properly, so
    // the cleanup finds nothing of ours to move and does its normal job for everything else.
    //
    // Reachable from RAISE_WEAPON's entrycommands (five statemap sites), from
    // Player::DropCurrentWeapon, and from script at coop_mod/itemhandler.scr (the coop reward-item
    // path - note its preceding `deactivateweapon "dual"` resolves to WEAPON_MAIN, because
    // WeaponHandNameToNum falls through to atoi("dual") == 0).
    if (m_bCoopQDrawActive) {
        CoopQDrawExit(qfalse, "correctweaponattachments");
    }

    int      iChild;
    int      iNumChildren;
    int      iTagRight;
    int      iTagLeft;
    qboolean iUseAngles;
    Vector   vOffset;
    Entity  *pChild;

    iTagRight    = gi.Tag_NumForName(edict->tiki, "tag_weapon_right");
    iTagLeft     = gi.Tag_NumForName(edict->tiki, "tag_weapon_left");
    iNumChildren = numchildren;

    for (int i = 0; i < MAX_MODEL_CHILDREN && iNumChildren; i++) {
        iChild = children[i];

        if (iChild == ENTITYNUM_NONE) {
            continue;
        }

        pChild = G_GetEntity(iChild);
        if (!pChild) {
            continue;
        }

        if (pChild->edict->s.tag_num == iTagLeft || pChild->edict->s.tag_num == iTagRight) {
            if (!pChild->IsSubclassOfWeapon()) {
                // Remove entities like ammoclip
                pChild->PostEvent(EV_Remove, 0);
                iNumChildren--;
            } else if (pChild->edict->s.tag_num == iTagLeft) {
                iUseAngles = pChild->edict->s.attach_use_angles;
                vOffset    = pChild->edict->s.attach_offset;

                // reattach to the right tag
                pChild->detach();
                pChild->attach(entnum, iTagRight, iUseAngles, vOffset);
            }
        }
    }
}
