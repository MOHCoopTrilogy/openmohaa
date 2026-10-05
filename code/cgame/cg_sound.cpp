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

// DESCRIPTION:
// Sound function

#include "cg_local.h"

//=========================================================================================================
// HZM coop [user 2026-10-05] "In the actual MP modes with bots, you hear them heal themselves, get injured and
// various other sounds - really should only be locational."
//
// THE MECHANISM. Every server sound goes out through SV_Sound, a loop over EVERY active client, and the client
// renders CHAN_LOCAL as a 2D listener-positioned sound at full volume with no attenuation
// (snd_openal_new.cpp S_OPENAL_StartSound -> Start2DSound + CHANNEL_FLAG_LOCAL_LISTENER). So any CHAN_LOCAL
// sound attached to ANOTHER player's entity plays dead-centre in your head at any distance:
//   * `<player> playlocalsound <alias>` (one-shot) is gi.Sound(..., CHAN_LOCAL, ..., maxdist 96) - "local"
//     in name only (player.cpp Player::PlayLocalSound); a bot's injury cough (tinnitus.scr), tinnitus, typing
//     keys, ... reached every client;
//   * `<player> playsound <alias>` where the alias is authored on the `local` channel - the medkit cloth and
//     exhale foley (medkit.scr / mp_medkits.scr), the stings, the announcer lines - reached every client once
//     PER player it was played on (the "loop over $player" broadcast pattern), stacked.
// The loop sound of the same kind was already filtered by CG_LoopSoundIsForeignLocal (cg_ents.c); the headshot
// cue was moved off this path (cg_view.c CG_CoopHeadshotCueThink). This closes the one-shot path for good.
//
// RULE, for a CHAN_LOCAL one-shot on a CLIENT entity that is not us:
//   * BODY sound (sound/characters/, coop_injury/, coop_breath/ - a man coughing, gasping, rustling a medkit,
//     exhaling): play it POSITIONALLY on his entity on CHAN_BODY, falloff to 800u (1200u for injury/voice).
//   * private cue (playlocalsound's maxdist-96 signature: tinnitus, shellshock, typing keys, hit confirm):
//     drop - it is his.
//   * anything else (the broadcast pattern - stings, announcer, objective/victory lines): keep ONE copy per
//     sound per 150 ms across all entities, so it plays once, unpositioned, like before - just not N times,
//     and a dead or spectating player (whom the script skipped) still hears it.
// Our own entity's CHAN_LOCAL sounds are untouched.
//=========================================================================================================
static int s_hzmLocalLast[MAX_SOUNDS];

// cg_hzmForeignLocal 0 = retail behaviour (A/B switch for the proof run); cg_hzmForeignLocalDebug 1 = one
// "^~^~^ FLOCAL" line per CHAN_LOCAL sound with the decision and the distance to the emitting entity.
static void CG_HzmFLDebug(const server_sound_t *s, const char *act, const char *name)
{
    static cvar_t *pDbg = NULL;
    vec3_t         d;
    float          dist = -1.f;

    if (!pDbg) {
        pDbg = cgi.Cvar_Get("cg_hzmForeignLocalDebug", "0", 0);
    }
    if (!pDbg->integer) {
        return;
    }
    if (s->entity_number >= 0 && s->entity_number < MAX_GENTITIES) {
        VectorSubtract(cg_entities[s->entity_number].lerpOrigin, cg.SoundOrg, d);
        dist = VectorLength(d);
    }
    cgi.Printf(
        "^~^~^ FLOCAL %s ent=%d me=%d dist=%.0f min=%.0f max=%.0f name=%s\n",
        act,
        s->entity_number,
        cg.snap ? cg.snap->ps.clientNum : -1,
        dist,
        s->min_dist,
        s->maxDist,
        name ? name : "?"
    );
}

static qboolean CG_HzmSoundIsBody(const char *name, float *pMaxDist)
{
    if (!name) {
        return qfalse;
    }
    if (Q_stristr(name, "sound/coop_injury/") || Q_stristr(name, "sound/coop_breath/")
        || Q_stristr(name, "sound/dialogue/")) {
        *pMaxDist = 1200.f;
        return Q_stristr(name, "sound/dialogue/multiplayer/") ? qfalse : qtrue;   // MP announcer lines are not body
    }
    if (Q_stristr(name, "sound/characters/")) {
        *pMaxDist = 800.f;
        return qtrue;
    }
    return qfalse;
}

// qtrue = handled (played or dropped) here; qfalse = play normally.
static qboolean CG_HzmForeignLocalSound(server_sound_t *sound)
{
    const char *cs;
    float       fMax = 800.f, fMin, fVol;
    int         idx = sound->sound_index;

    static cvar_t *pOn = NULL;

    if (sound->channel != CHAN_LOCAL || !cg.snap) {
        return qfalse;
    }
    if (idx < 0 || idx >= MAX_SOUNDS) {
        return qfalse;
    }
    if (!pOn) {
        pOn = cgi.Cvar_Get("cg_hzmForeignLocal", "1", 0);
    }
    if (!pOn->integer) {
        CG_HzmFLDebug(sound, "off", CG_ConfigString(CS_SOUNDS + idx));
        return qfalse;
    }
    // A PLAYER entity, bots included. Not `< cgs.maxclients`: playerbots get client slots ABOVE sv_maxclients
    // (the proof run's bots were entity 8+ with sv_maxclients 8), which that test missed.
    if (sound->entity_number >= 0 && sound->entity_number < MAX_GENTITIES
        && sound->entity_number != cg.snap->ps.clientNum
        && cg_entities[sound->entity_number].currentState.eType == ET_PLAYER) {
        cs = CG_ConfigString(CS_SOUNDS + idx);
        if (CG_HzmSoundIsBody(cs, &fMax)) {
            fMin = sound->min_dist;
            if (fMin <= 0.f || fMin > fMax * 0.25f) {
                fMin = fMax * 0.15f;
            }
            fVol = sound->volume;
            if (fVol < 0.f || fVol > 1.5f) {
                fVol = (fVol < 0.f) ? 1.f : 1.5f;
            }
            cgi.S_StartSound(
                sound->origin, sound->entity_number, CHAN_BODY, cgs.sound_precache[idx], fVol, fMin, sound->pitch, fMax,
                sound->streamed
            );
            CG_HzmFLDebug(sound, "positional", cs);
            return qtrue;
        }
        if (sound->maxDist > 0.f && sound->maxDist <= 100.f) {
            CG_HzmFLDebug(sound, "drop_private", cs);
            return qtrue;   // playlocalsound one-shot of somebody else: private, not ours
        }
    }

    // broadcast pattern: one copy per sound per 150 ms, whichever entity it rode in on (our own included).
    // Our own playlocalsound one-shots (maxdist 96) are exempt - a typewriter can repeat a key faster than that.
    if (sound->maxDist > 0.f && sound->maxDist <= 100.f) {
        CG_HzmFLDebug(sound, "own_private", CG_ConfigString(CS_SOUNDS + idx));
        return qfalse;
    }
    if (s_hzmLocalLast[idx] && cg.time >= s_hzmLocalLast[idx] && cg.time - s_hzmLocalLast[idx] < 150) {
        CG_HzmFLDebug(sound, "drop_dup", CG_ConfigString(CS_SOUNDS + idx));
        return qtrue;
    }
    s_hzmLocalLast[idx] = cg.time ? cg.time : 1;
    CG_HzmFLDebug(sound, "play_once", CG_ConfigString(CS_SOUNDS + idx));
    return qfalse;
}

void CG_ProcessSound(server_sound_t *sound)
{
    if (sound->stop_flag) {
        cgi.S_StopSound(sound->entity_number, sound->channel);
    } else {
        if (CG_HzmForeignLocalSound(sound)) {
            return;
        }
        cgi.S_StartSound(
            sound->origin,
            sound->entity_number,
            sound->channel,
            cgs.sound_precache[sound->sound_index],
            sound->volume,
            sound->min_dist,
            sound->pitch,
            sound->maxDist,
            sound->streamed
        );
    }
}
