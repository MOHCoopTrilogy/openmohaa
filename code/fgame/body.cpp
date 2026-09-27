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

// body.cpp: Dead bodies

#include "animate.h"
#include "body.h"
#include "g_phys.h"
#include "level.h"           // level.vars for the MP-session check (mp_corpse_persist)
#include "scriptvariable.h"  // ScriptVariable / VARIABLE_NONE

extern Event EV_DeathSink; // entity.cpp - the MP sink step (0.2u a frame until EV_Remove)

// HZM-MP-BEGIN(mp_corpse_cap)
// [user 2026-09-25] "5 minutes is good" - MP bodies linger coop_mpCorpseLinger (default 300s), CAPPED so a long fight
// cannot eat the 1024-entity budget (MAX_GENTITIES; a failed G_Spawn is a crash) or flood joining players' snapshots:
// at most coop_mpCorpseMax bodies (default 48, like coop's AI corpse queue), and never within 160 entities of the limit.
// The body pushed out sinks into the ground over the 5s the MP remove takes, instead of popping out of view.
#define MP_BODY_RING 128
static SafePtr<Body> s_mpBodies[MP_BODY_RING];
static int           s_mpBodyHead = 0;

static void MpBodySinkNow(Body *b)
{
    b->CancelEventsOfType(EV_DeathSinkStart);
    b->CancelEventsOfType(EV_DeathSink);
    b->CancelEventsOfType(EV_Remove);
    b->PostEvent(EV_DeathSink, 0);
    b->PostEvent(EV_Remove, 5.0f);
}
// HZM-MP-END(mp_corpse_cap)

CLASS_DECLARATION(Animate, Body, NULL) {
    // [user 2026-09-21] wire the gib handler that shipped DEAD (the table was empty), so a shot corpse gibs.
    // It only ever fires when the body is solid + takedamage, which the constructor does for MP sessions only.
    {&EV_Damage, &Body::Damage},
    {NULL,       NULL          }
};

//=============================================================
//Body::Body
//=============================================================
Body::Body()
{
    edict->s.eType  = ET_MODELANIM;
    edict->clipmask = MASK_DEADSOLID;
    edict->s.eFlags |= EF_DEAD;

    setSolidType(SOLID_NOT);
    setContents(CONTENTS_CORPSE);
    setMoveType(MOVETYPE_NONE);

    float fSink = 5.0f;

    // HZM-MP-BEGIN(mp_corpse_persist)
    // [user 2026-09-21] PERSISTENT SHOOTABLE CORPSES IN MP. The body left behind when a player respawns is
    // normally SOLID_NOT (bullets pass clean through it) and starts sinking after 5s. In an MP session make it
    // SHOOTABLE with the same recipe as a coop AI corpse (Actor::BecomeCorpse, bug-1321): CONTENTS_WEAPONCLIP
    // (the only MASK_SHOT flag absent from player/monster-solid, so it stops bullets without body-blocking) +
    // SOLID_BBOX + a flattened ground-slab bbox (out of bot eye-lines) + takedamage, so a shot gibs it via
    // Body::Damage (now wired above). Also lengthen the linger so bodies actually REMAIN on the battlefield
    // (host cvar coop_mpCorpseLinger, default 300s - capped, see s_mpBodies). MP ONLY: gated on coop_mpRun by type; a coop
    // or SP body stays vanilla SOLID_NOT and non-gorable (bug-792, no gore on a coop player's own body).
    {
        ScriptVariable *pMpRun = level.vars ? level.vars->GetVariable("coop_mpRun") : NULL;
        static cvar_t  *pShoot = NULL, *pLinger = NULL, *pMax = NULL;
        if (!pShoot) {
            pShoot = gi.Cvar_Get("coop_corpseShootable", "1", CVAR_ARCHIVE);
        }
        if (!pLinger) {
            pLinger = gi.Cvar_Get("coop_mpCorpseLinger", "300", CVAR_ARCHIVE);
        }
        if (!pMax) {
            pMax = gi.Cvar_Get("coop_mpCorpseMax", "48", CVAR_ARCHIVE);
        }
        if (pMpRun && pMpRun->GetType() != VARIABLE_NONE && pShoot->integer) {
            setSize(Vector(-32.0f, -32.0f, 0.0f), Vector(32.0f, 32.0f, 16.0f));
            setContents(CONTENTS_WEAPONCLIP);
            setSolidType(SOLID_BBOX);
            takedamage = DAMAGE_YES;
            fSink      = pLinger->value;
            if (fSink < 5.0f) {
                fSink = 5.0f;
            }
            // the cap: this body takes the ring slot of the one placed cap bodies ago - that one sinks now
            const int cap  = Q_clamp(pMax->integer, 8, MP_BODY_RING);
            const int slot = s_mpBodyHead % cap;
            if (s_mpBodies[slot] && s_mpBodies[slot] != this) {
                MpBodySinkNow(s_mpBodies[slot]);
            }
            s_mpBodies[slot] = this;
            s_mpBodyHead++;
            // ...and the entity budget: near the limit, the oldest body still lingering goes early
            if (globals.num_entities > MAX_GENTITIES - 160) {
                for (int i = 1; i <= cap; i++) {
                    Body *old = s_mpBodies[(s_mpBodyHead + i - 1) % cap];
                    if (old && old != this) {
                        MpBodySinkNow(old);
                        s_mpBodies[(s_mpBodyHead + i - 1) % cap] = NULL;
                        break;
                    }
                }
            }
        }
    }
    // HZM-MP-END(mp_corpse_persist)

    PostEvent(EV_DeathSinkStart, fSink);
}

void Body::Damage(Event *ev)
{
    // [user 2026-09-21] SHOT CORPSES MUST NOT VANISH. The stock handler spawned 5 gibs then hideModel()'d the
    // body and killed takedamage on the FIRST hit - so shooting a persistent corpse made it disappear, which
    // the user reported. Now the body STAYS and keeps reacting: it throws a small chunk spray per hit but is
    // never hidden and never stops taking damage. A per-frame cap keeps a shotgun blast (~10 damage events in
    // one frame) from spawning a storm (bug-856 pattern).
    static int s_frame = -1, s_thisFrame = 0;
    Animate   *ent;

    if (!com_blood->integer) {
        return;
    }
    if (s_frame != level.inttime) {
        s_frame     = level.inttime;
        s_thisFrame = 0;
    }
    if (s_thisFrame >= 2) {
        return;
    }
    s_thisFrame++;

    ent = new Animate;
    ent->setModel("fx_rgib5.tik");
    ent->setScale(0.9f);
    ent->setOrigin(centroid);
    ent->NewAnim("idle");
    ent->PostEvent(EV_Remove, 1.0f);

    Sound("snd_decap", CHAN_BODY, 1.0f, 300.0f);
    // deliberately NO hideModel() / takedamage=DAMAGE_NO: the corpse persists and stays shootable.
}