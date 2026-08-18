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

// object.cpp : Object (used by common TIKIs)

#include "g_local.h"
#include "object.h"
#include "sentient.h"
#include "misc.h"
#include "explosion.h"
#include "gibs.h"
#include "specialfx.h"
#include "g_phys.h"
#include "game.h"
#include "../script/scriptexception.h"

Event EV_Object_HandleSetModel
(
    "handlespawn",
    EV_DEFAULT,
    NULL,
    NULL,
    "Internal usage",
    EV_NORMAL
);

CLASS_DECLARATION(Animate, Object, NULL) {
    {NULL, NULL}
};

Event EV_InteractObject_Setup
(
    "_setup",
    EV_DEFAULT,
    NULL,
    NULL,
    "Sets up an object."
);

Event EV_InteractObject_KilledEffect
(
    "killedeffect",
    EV_DEFAULT,
    NULL,
    NULL,
    "Sets the tiki it will spawn when it's destroyed",
    EV_NORMAL
);

Event EV_InteractObject_HitEffect
(
    "hiteffect",
    EV_DEFAULT,
    NULL,
    NULL,
    "Sets the tiki it will spawn when it's hit",
    EV_NORMAL
);

CLASS_DECLARATION(Animate, InteractObject, "interactobject") {
    {&EV_Damage,                      &InteractObject::Damaged          },
    {&EV_Killed,                      &InteractObject::Killed           },
    {&EV_InteractObject_Setup,        &InteractObject::Setup            },
    {&EV_InteractObject_HitEffect,    &InteractObject::EventHitEffect   },
    {&EV_InteractObject_KilledEffect, &InteractObject::EventKilledEffect},
    {NULL,                            NULL                              }
};

InteractObject::InteractObject() {}

void InteractObject::Setup(Event *ev)
{
    if (!health) {
        // Set the bounding box as health
        health = (maxs - mins).length();
    }

    max_health = health;
    deadflag   = DEAD_NO;

    NewAnim("idle");
    link();
}

void InteractObject::EventHitEffect(Event *ev)
{
    m_sHitEffect = ev->GetString(1);
}

void InteractObject::EventKilledEffect(Event *ev)
{
    m_sKilledEffect = ev->GetString(1);
}

void InteractObject::Damaged(Event *ev)
{
    if (m_sHitEffect.length()) {
        // Spawn a temporary hit effect
        Animate *temp = new Animate();
        temp->PostEvent(EV_Remove, 1.f);
        temp->setModel(m_sHitEffect);
        temp->NewAnim("idle");
    }

    Entity::DamageEvent(ev);
}

void InteractObject::Killed(Event *ev)
{
    Entity     *ent;
    Entity     *attacker;
    Vector      dir;
    const char *name;

    takedamage = DAMAGE_NO;
    deadflag   = DEAD_NO;
    setSolidType(SOLID_NOT);
    edict->s.renderfx |= RF_DONTDRAW;

    if (edict->solid == SOLID_NOT || edict->solid == SOLID_TRIGGER) {
        edict->r.svFlags |= SVF_NOCLIENT;
    }

    if (m_sKilledEffect.length()) {
        // Spawn a temporary killed effect
        Animate *temp = new Animate();
        temp->PostEvent(EV_Remove, 1.f);
        temp->setModel(m_sKilledEffect);
        temp->NewAnim("idle");
    }

    attacker = ev->GetEntity(1);
    if (killtarget.c_str() && killtarget[0]) {
        // Kill all targets
        for (ent = G_FindTarget(NULL, killtarget.c_str()); ent; ent = G_FindTarget(ent, killtarget.c_str())) {
            ent->PostEvent(EV_Remove, 0);
        }
    }

    if (target.c_str() && target[0]) {
        // Activate all targets
        for (ent = G_FindTarget(NULL, target.c_str()); ent; ent = G_FindTarget(ent, target.c_str())) {
            Event *event = new Event(EV_Activate);
            event->AddEntity(attacker);

            ent->ProcessEvent(event);
        }
    }

    // Remove ourself
    PostEvent(EV_Remove, 0);
}

void InteractObject::Archive(Archiver& arc)
{
    Animate::Archive(arc);

    arc.ArchiveString(&m_sHitEffect);
    arc.ArchiveString(&m_sKilledEffect);
}

/*****************************************************************************/
/*QUAKED func_throwobject (0 0.25 0.5) (-16 -16 0) (16 16 32)

This is an object you can pickup and throw at people
******************************************************************************/

Event EV_ThrowObject_Pickup
(
    "pickup",
    EV_DEFAULT,
    "es",
    "entity tag_name",
    "Picks up this throw object and attaches it to the entity.",
    EV_NORMAL
);
Event EV_ThrowObject_Throw
(
    "throw",
    EV_DEFAULT,
    "efeF",
    "owner speed targetent grav",
    "Throw this throw object.",
    EV_NORMAL
);
Event EV_ThrowObject_PickupOffset
(
    "pickupoffset",
    EV_DEFAULT,
    "v",
    "pickup_offset",
    "Sets the pickup_offset.",
    EV_NORMAL
);
Event EV_ThrowObject_ThrowSound
(
    "throwsound",
    EV_DEFAULT,
    "s",
    "throw_sound",
    "Sets the sound to play when object is thrown.",
    EV_NORMAL
);

CLASS_DECLARATION(Animate, ThrowObject, "func_throwobject") {
    {&EV_Touch,                    &ThrowObject::Touch       },
    {&EV_ThrowObject_Pickup,       &ThrowObject::Pickup      },
    {&EV_ThrowObject_Throw,        &ThrowObject::Throw       },
    {&EV_ThrowObject_PickupOffset, &ThrowObject::PickupOffset},
    {&EV_ThrowObject_ThrowSound,   &ThrowObject::ThrowSound  },
    {NULL,                         NULL                      }
};

ThrowObject::ThrowObject()
{
    if (LoadingSavegame) {
        // Archive function will setup all necessary data
        return;
    }
    pickup_offset = vec_zero;
}

void ThrowObject::PickupOffset(Event *ev)
{
    pickup_offset = edict->s.scale * ev->GetVector(1);
}

void ThrowObject::ThrowSound(Event *ev)
{
    throw_sound = ev->GetString(1);
}

void ThrowObject::Touch(Event *ev)
{
    Entity *other;

    if (movetype != MOVETYPE_BOUNCE) {
        return;
    }

    other = ev->GetEntity(1);
    assert(other);

    if (other->isSubclassOf(Teleporter)) {
        return;
    }

    if (other->entnum == owner) {
        return;
    }

    if (throw_sound.length()) {
        StopLoopSound();
    }

    if (other->takedamage) {
        other->Damage(
            this,
            G_GetEntity(owner),
            size.length() * velocity.length() / 400,
            origin,
            velocity,
            level.impact_trace.plane.normal,
            32,
            0,
            MOD_THROWNOBJECT
        );
    }

    Damage(this, this, max_health, origin, velocity, level.impact_trace.plane.normal, 32, 0, MOD_THROWNOBJECT);
}

void ThrowObject::Throw(Event *ev)
{
    Entity   *owner;
    Sentient *targetent;
    float     speed;
    float     traveltime;
    float     vertical_speed;
    Vector    target;
    Vector    dir;
    float     grav;
    Vector    xydir;
    Event    *e;

    owner = ev->GetEntity(1);
    assert(owner);

    if (!owner) {
        ScriptError("owner == NULL");
        return;
    }

    speed = ev->GetFloat(2);
    if (!speed) {
        speed = 1;
    }

    targetent = (Sentient *)ev->GetEntity(3);
    assert(targetent);
    if (!targetent) {
        ScriptError("targetent == NULL");
        return;
    }

    if (ev->NumArgs() == 4) {
        grav = ev->GetFloat(4);
    } else {
        grav = 1;
    }

    e = new Event(EV_Detach);
    ProcessEvent(e);

    this->owner       = owner->entnum;
    edict->r.ownerNum = owner->entnum;

    gravity = grav;

    if (targetent->IsSubclassOfSentient()) {
        target = targetent->origin;
        target.z += targetent->viewheight;
    } else {
        target = targetent->centroid;
    }

    setMoveType(MOVETYPE_BOUNCE);
    setSolidType(SOLID_BBOX);
    edict->clipmask = MASK_PROJECTILE;

    dir            = target - origin;
    xydir          = dir;
    xydir.z        = 0;
    traveltime     = xydir.length() / speed;
    vertical_speed = (dir.z / traveltime) + (0.5f * gravity * sv_gravity->value * traveltime);
    xydir.normalize();

    // setup ambient flying sound
    if (throw_sound.length()) {
        LoopSound(throw_sound.c_str());
    }

    velocity   = speed * xydir;
    velocity.z = vertical_speed;

    angles = velocity.toAngles();
    setAngles(angles);

    avelocity.x = crandom() * 200;
    avelocity.y = crandom() * 200;
    takedamage  = DAMAGE_YES;
}

void ThrowObject::Pickup(Event *ev)
{
    Entity *ent;
    Event  *e;
    str     bone;

    ent = ev->GetEntity(1);

    assert(ent);
    if (!ent) {
        return;
    }
    bone = ev->GetString(2);

    setOrigin(pickup_offset);

    e = new Event(EV_Attach);
    e->AddEntity(ent);
    e->AddString(bone);
    ProcessEvent(e);

    edict->s.renderfx &= ~RF_FRAMELERP;
}

void ThrowObject::Archive(Archiver& arc)
{
    Animate::Archive(arc);

    arc.ArchiveInteger(&owner);
    arc.ArchiveVector(&pickup_offset);
    arc.ArchiveString(&throw_sound);
}

CLASS_DECLARATION(Entity, HelmetObject, "helmetobject") {
    // HZM coop: G_Physics_Toss delivers EV_Stop (not EV_Touch) when a SOLID_NOT toss entity
    // lands (g_phys.cpp ~1162), so this settle handler was unreachable dead code.
    {&EV_Stop, &HelmetObject::HelmetTouch},
    {NULL,     NULL                      }
};

HelmetObject::HelmetObject()
{
    // HZM coop: popped helmets vanished after 5s, easy to miss mid-firefight
    static cvar_t *g_helmetlife = NULL;

    if (LoadingSavegame) {
        return;
    }

    setSolidType(SOLID_NOT);
    setMoveType(MOVETYPE_TOSS);
    setSize(Vector(-2, -2, -2), Vector(2, 2, 2));
    edict->clipmask = MASK_VIEWSOLID;

    if (!g_helmetlife) {
        g_helmetlife = gi.Cvar_Get("g_helmetlife", "30", 0);
    }

    PostEvent(EV_Remove, g_helmetlife->value > 0 ? g_helmetlife->value : 5);
}

Event EV_CoopHeadSettle
(
    "_coop_head_settle",
    EV_DEFAULT,
    NULL,
    NULL,
    "HZM coop - trace the severed head down onto the floor and park it",
    EV_NORMAL
);

CLASS_DECLARATION(Entity, HeadGibObject, "headgibobject") {
    {&EV_Stop,           &HeadGibObject::HeadGibStop  },
    {&EV_CoopHeadSettle, &HeadGibObject::CoopHeadSettle},
    {NULL,               NULL                         }
};

/*
=================
HeadGibObject::CoopHeadSettle   (HZM coop [user 2026-08-17])

"severed heads do not seem to still land."

Two attempts at fixing this through the collision box failed, so this stops relying on the box at
all. The gib draws a whole body in bind pose with only the head visible, so the head MESH is
displaced from the entity ORIGIN by m_vCoopHeadOfs - and physics only ever knows about the origin.

This traces DOWNWARD FROM THE VISIBLE HEAD, and when it finds floor within reach it places the
entity so that the head - not the origin - rests just above that floor, then stops the physics. It
re-posts itself a few times a second while the head is still moving, so it catches the landing
whether the box behaved or not, and it gives up after a bounded number of tries so a head thrown
off a cliff cannot poll forever.
=================
*/
void HeadGibObject::CoopHeadSettle(Event *ev)
{
    Vector  headPos, start, end;
    trace_t tr;

    if (movetype == MOVETYPE_NONE) {
        return; // already parked
    }

    m_iCoopSettleTries++;
    if (m_iCoopSettleTries > 120) {
        return; // ~30s of falling: it is not coming back, let the fade remove it
    }

    headPos = origin + m_vCoopHeadOfs;

    //
    // [user 2026-08-18] THE ALLSOLID FREEZE - bug-1915, and the real reason four earlier fixes each
    // changed nothing. g_phys.cpp already documents this for bug-923: an allsolid trace returns
    // fraction 0 and a ZEROED plane, so the `normal[2] > 0.7` ground test can never pass and the
    // entity's position freezes at its drop origin while avelocity keeps spinning it. That unstick
    // was written for MOVETYPE_TOSS items and EXPLICITLY EXCLUDES gibs.
    //
    // Our head hits it every time, because its collision box is deliberately offset ~60 units UP
    // onto the head mesh - which leaves the ORIGIN down at the corpse's feet, at floor level and
    // frequently in solid. So the head froze the moment it spawned and the mesh drew a head-height
    // above that frozen origin: "floats where the body was". The solid type, the settle think and
    // the clipmask all governed how it FELL, which is why fixing each of them was invisible.
    //
    // Detect the freeze the way bug-923 does - the origin not moving - and snap the HEAD, not the
    // origin, down onto real ground. MASK_SOLID excludes CONTENTS_BODY, so unlike the first version
    // of this think it cannot park on the corpse it just came off.
    //
    if ((origin - m_vCoopLastOrigin).lengthSquared() < 1.0f) {
        m_iCoopStuckFrames++;
    } else {
        m_iCoopStuckFrames = 0;
        m_vCoopLastOrigin  = origin;
    }

    if (m_iCoopStuckFrames >= 3) {
        tr = G_Trace(headPos, Vector(-2, -2, -2), Vector(2, 2, 2), headPos - Vector(0, 0, 8192),
                     this, MASK_SOLID, qfalse, "HeadGibObject::CoopHeadSettle_stuck");
        if (!tr.startsolid && tr.fraction < 1.0f) {
            setOrigin(Vector(tr.endpos) + Vector(0, 0, 3) - m_vCoopHeadOfs);
        }
        velocity  = vec_zero;
        avelocity = vec_zero;
        setMoveType(MOVETYPE_NONE);
        return;
    }

    // normal landing: once the head itself is falling onto ground, rest it there
    if (velocity[2] <= 0.0f) {
        start = headPos + Vector(0, 0, 2);
        end   = headPos - Vector(0, 0, 6);
        tr    = G_Trace(start, Vector(-2, -2, -2), Vector(2, 2, 2), end, this, MASK_SOLID, qfalse,
                        "HeadGibObject::CoopHeadSettle");
        if (tr.fraction < 1.0f && !tr.startsolid && !tr.allsolid && tr.plane.normal[2] > 0.7f) {
            setOrigin(Vector(tr.endpos) + Vector(0, 0, 3) - m_vCoopHeadOfs);
            velocity  = vec_zero;
            avelocity = vec_zero;
            setMoveType(MOVETYPE_NONE);
            return;
        }
    }

    PostEvent(EV_CoopHeadSettle, 0.1f);
}

HeadGibObject::HeadGibObject()
{
    // HZM coop [user 2026-08-17] - lifetime is deliberately SHORT. The first decap attempt
    // (bug-856) left 30s entities behind and, on a count-scaled horde, the survivors piled up and
    // hitched the fixed-rate server sim - which surfaced as every AI stuttering and not shooting.
    static cvar_t *pLife = NULL;

    if (LoadingSavegame) {
        return;
    }

    // [user 2026-08-17] "severed heads didnt land on the ground". SOLID_NOT was the whole
    // problem: a non-solid entity does not collide with the world, so MOVETYPE_TOSS just fell
    // forever and no amount of tuning the settle trace could catch it - the head was never going
    // to stop. This is the engine's OWN gib recipe from gibs.cpp:64-65 (MOVETYPE_GIB +
    // SOLID_BBOX), which falls, lands, and does not block players. Copy the working recipe rather
    // than invent a third physics setup.
    setSolidType(SOLID_BBOX);
    setMoveType(MOVETYPE_GIB);
    // [user 2026-08-17] "the head does come off but it clips thru the ground". The box was +/-3, so
    // it settled with the ORIGIN only 3 units above the floor - and a head is far bigger than that,
    // so most of the mesh ended up buried. Dropping mins.z well below the origin makes the box rest
    // higher and lifts the whole head clear. Width is widened to match a head rather than a marble,
    // so it also stops rolling into thin geometry. coop_decapHeadRise tunes the sit height live if
    // it still looks buried or now floats.
    // A provisional box only - CoopGoreTryDecapitate re-sizes this onto the head's bind-pose
    // position the moment the model is known, which is what actually makes it land correctly.
    setSize(Vector(-6, -6, -6), Vector(6, 6, 6));
    // [user 2026-08-17] "heads still float when they get shot off and then eventually disappear",
    // AFTER the SOLID_BBOX + MOVETYPE_GIB fix was genuinely compiled in. The physics setup was
    // right by then; the CLIPMASK was not. MASK_VIEWSOLID includes CONTENTS_TRIGGER
    // (bg_public.h:642), so the head's falling trace stopped on the first TRIGGER BRUSH it touched
    // - and maps are carpeted in trigger volumes, many of them covering the whole play area. The
    // head hit one immediately, stopped in mid-air, and sat there until its lifetime expired.
    // The engine's own Gib sets NO clipmask at all and so inherits Entity's default MASK_SOLID
    // (entity.cpp:1749), which is solid + playerclip + fence and no triggers. Copy the recipe that
    // works rather than keep a mask that was only ever meant for view traces.
    edict->clipmask = MASK_SOLID;

    if (!pLife) {
        pLife = gi.Cvar_Get("coop_decapLife", "0", CVAR_ARCHIVE); // 0 = persist like a corpse
    }

    m_vCoopHeadOfs      = vec_zero; // filled in by CoopGoreTryDecapitate once the model is known
    m_iCoopSettleTries  = 0;
    m_vCoopLastOrigin   = vec_zero;
    m_iCoopStuckFrames  = 0;
    // [user 2026-08-18] The settle think is SCHEDULED AGAIN. It was unscheduled on 08-17 because it
    // parked the head on the corpse - but the real defect there was tracing with MASK_VIEWSOLID,
    // and MASK_SOLID excludes CONTENTS_BODY so it cannot hit a corpse at all. It has to exist:
    // MOVETYPE_GIB is excluded from the bug-923 allsolid unstick in g_phys.cpp, and our offset
    // collision box puts the head's ORIGIN at floor level, so it hits that freeze every single
    // time (bug-1915). The think is what detects it and snaps the head down to real ground.
    PostEvent(EV_CoopHeadSettle, 0.1f);
    // [user 2026-08-17] "I think they should last way longer before they despawn" - and corpses in
    // this mod already persist for the whole map (coop_corpseLife 0 = keep forever), so a head
    // evaporating next to a body that does not was inconsistent anyway.
    //
    // coop_decapLife 0 (the new default) = NO timer at all: the head stays like the corpse does.
    // What keeps that from becoming the entity leak that the 4-second original was guarding against
    // is a global CAP on live heads instead - see CoopDecapRegisterHead in sentient.cpp, which fades
    // the oldest once coop_decapMax are on the ground. Bounded either way, but bounded by COUNT
    // rather than by a stopwatch, which is what actually matters to the entity pool.
    if (pLife->value > 0) {
        Event *fade = new Event(EV_Fade);
        fade->AddFloat(1.5f); // seconds to fade out, then it removes itself
        PostEvent(fade, pLife->value);
    }
}

void HeadGibObject::HeadGibStop(Event *ev)
{
    avelocity = vec_zero;
    setMoveType(MOVETYPE_NONE);
}

void HelmetObject::HelmetTouch(Event *ev)
{
    avelocity = vec_zero;

    angles.x = 0;
    angles.z = 0;
    setAngles(angles);
    // Stop moving
    setMoveType(MOVETYPE_NONE);

    // HZM coop: landing clatter (alias exists in all three games' ubersounds, takes 1-3)
    Sound("grenade_bounce_metal", CHAN_BODY);
}
