/*
===========================================================================
Copyright (C) 2008 the OpenMoHAA team

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

// object.h: Object (used by common TIKIs)

#pragma once

#include "animate.h"

class Object : public Animate
{
public:
    CLASS_PROTOTYPE(Object);

    void EventRemoveObjectModel(Event *ev);
    void EventHandleSpawn(Event *ev);
};

class InteractObject : public Animate
{
private:
    str m_sHitEffect;
    str m_sKilledEffect;

public:
    CLASS_PROTOTYPE(InteractObject);

    InteractObject();
    void Damaged(Event *ev);
    void Killed(Event *ev) override;
    void Setup(Event *ev);
    void EventHitEffect(Event *ev);
    void EventKilledEffect(Event *ev);
    void Archive(Archiver& arc) override;
};

extern Event EV_ThrowObject_Pickup;
extern Event EV_ThrowObject_Throw;

// Fixed in 2.0
//  Before 2.0, ThrowObject was inheriting from Object.
//  This caused issue when spawning the ThrowObject from script.
class ThrowObject : public Animate
{
private:
    int    owner;
    Vector pickup_offset;
    str    throw_sound;

public:
    CLASS_PROTOTYPE(ThrowObject);

    ThrowObject();
    void Touch(Event *ev);
    void Throw(Event *ev);
    void Pickup(Event *ev);
    void PickupOffset(Event *ev);
    void ThrowSound(Event *ev);
    void Archive(Archiver& arc) override;
};

class HelmetObject : public Entity
{
public:
    CLASS_PROTOTYPE(HelmetObject);

    HelmetObject();
    void HelmetTouch(Event *ev);
};

// HZM coop [user 2026-08-17] - DECAPITATION. Mirrors HelmetObject exactly: SOLID_NOT so it can
// never block a player or an AI's path, MASK_VIEWSOLID so it still lands on the world, EV_Stop
// (not EV_Touch) because G_Physics_Toss delivers Stop when a SOLID_NOT toss entity settles, and a
// SHORT self-remove. Never animated - the head renders in its bind pose, which is the whole trick
// that lets a severed head exist without any new art.
class HeadGibObject : public Entity
{
public:
    CLASS_PROTOTYPE(HeadGibObject);

    HeadGibObject();
    void HeadGibStop(Event *ev);

    // HZM coop [user 2026-08-17] - the head MESH sits this far from the entity origin (bind-pose
    // displacement, measured by CoopGoreTryDecapitate). Everything about landing this thing depends
    // on it, so it is carried here rather than recomputed.
    Vector m_vCoopHeadOfs;
    int    m_iCoopSettleTries;
    Vector m_vCoopLastOrigin;   // to detect the allsolid freeze (bug-1915)
    int    m_iCoopStuckFrames;
    void   CoopHeadSettle(Event *ev);
};

// HZM coop [user 2026-09-04] MAGAZINE EJECT. "on reload, magazines drop to the floor and bounce
// realistically ... enemies and allies do it too ... lets make sure people dont be getting stuck on
// them." Same family as HelmetObject / HeadGibObject above, with three deliberate differences:
//
//  - MOVETYPE_BOUNCE, not MOVETYPE_TOSS. G_Physics_Toss gives TOSS backoff 1.0 (g_phys.cpp:1184) -
//    "clip the velocity and stop", no rebound at all; the helmet only LOOKS lively because of its
//    avelocity. BOUNCE gets backoff 1.4 (:1180) plus horizontal damping (:1191) and settles into
//    EV_Stop below speed 40 (:1204). It is the movetype the shipping gore chunks already use
//    (Sentient::CoopGoreThrowChunks, sentient.cpp).
//
//  - NOBODY GETS STUCK, and SOLID_NOT is the whole guarantee. setSolidType(SOLID_NOT) zeroes the
//    entity's CONTENTS, and SV_ClipMoveToEntities skips solid == SOLID_NOT unconditionally BEFORE
//    any contentmask test (server/sv_world.c:555, and again in the sight-trace path at :662). No
//    player move, no AI move, no bullet and no line-of-sight trace can reach one; AI pathing is
//    unaffected for a second reason, it runs on precomputed pathnodes and not on world entities.
//    It still LANDS because G_PushEntity traces with the MOVER'S OWN clipmask (g_phys.cpp:486) -
//    the mover's solidity is irrelevant to its own trace.
//
//  - The clipmask is MASK_SOLID minus the two body bits, and nothing is ADDED to it. Not
//    MASK_VIEWSOLID, which carries CONTENTS_TRIGGER and would strand magazines on the trigger
//    volumes maps are carpeted in - that is exactly what froze severed heads in mid-air
//    (HeadGibObject, 2026-08-18). And not `| CONTENTS_PLAYERCLIP` either: MASK_SOLID does not
//    contain it (bg_public.h:643), so adding it would strand them on invisible playerclip brushes.
//    The two body bits come out so a magazine never comes to rest on a walking soldier's bbox,
//    which is two feet away a second later.
class CoopMagObject : public Entity
{
public:
    CLASS_PROTOTYPE(CoopMagObject);

    CoopMagObject();
    void CoopMagStop(Event *ev);
};
