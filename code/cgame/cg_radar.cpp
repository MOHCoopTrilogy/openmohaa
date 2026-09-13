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
#include "cg_radar.h"

void CG_InitRadar()
{
    int i;

    cg.radarShaders[0] = cgi.R_RegisterShader("textures/hud/radar_allies.tga");
    cg.radarShaders[1] = cgi.R_RegisterShader("textures/hud/radar_axis.tga");

    for (i = 0; i < MAX_CLIENTS; i++) {
        cg.radars[i].time          = 0;
        cg.radars[i].lastSpeakTime = 0;
    }

    cgi.CL_InitRadar(cg.radars, cg.radarShaders, cg.snap->ps.clientNum);
}

bool CG_InTeamGame(centity_t *ent)
{
    return ent->currentState.solid && cg.clientinfo[ent->currentState.number].team;
}

bool CG_SameTeam(centity_t *ent)
{
    return cg.clientinfo[ent->currentState.number].team == cg.clientinfo[cg.snap->ps.clientNum].team;
}

bool CG_IsTeamGame()
{
    return cgs.gametype >= GT_TEAM;
}

bool CG_ValidRadarClient(centity_t *ent)
{
    if (!cg.snap) {
        return qfalse;
    }

    if (!CG_IsTeamGame()) {
        return false;
    }

    if (!CG_InTeamGame(&cg_entities[cg.snap->ps.clientNum])) {
        return false;
    }

    if (!CG_InTeamGame(ent)) {
        return false;
    }

    return CG_SameTeam(ent);
}

int CG_RadarIcon()
{
    switch (cg.clientinfo[cg.snap->ps.clientNum].team) {
    case TEAM_ALLIES:
        return 0;
    default:
    case TEAM_AXIS:
        return 1;
    }
}

void CG_UpdateRadarClient(centity_t *ent)
{
    radarClient_t *radar;

    radar = &cg.radars[ent->currentState.number];
    if (!CG_ValidRadarClient(ent)) {
        radar->time = 0;
        return;
    }

    radar->time       = cg.time;
    radar->teamShader = CG_RadarIcon();
    if (cg.snap->ps.clientNum == ent->currentState.number) {
        radar->origin[0] = cg.refdef.vieworg[0];
        radar->origin[1] = cg.refdef.vieworg[1];
        radar->axis[0]   = cg.refdef.viewaxis[0][0];
        radar->axis[1]   = cg.refdef.viewaxis[0][1];
        VectorNormalize2D(radar->axis);
    } else {
        float axis[2];

        radar->origin[0] = ent->currentState.origin[0];
        radar->origin[1] = ent->currentState.origin[1];
        YawToAxis(ent->currentState.angles[1], axis);

        radar->axis[0] = axis[0];
        radar->axis[1] = axis[1];
        VectorNormalize2D(radar->axis);
    }
}

void CG_ReadNonPVSClient(radarUnpacked_t *radarUnpacked)
{
    radarClient_t *radar;
    float          axis[2];

    // HZM coop [user 2026-09-13] top compass bar: keep a RAW copy of every packet, taken BEFORE the validity
    // test below. CG_ValidRadarClient needs currentState.solid on BOTH last-seen cg_entities entries, and
    // those go stale: a teammate who died in view and respawned out of the snapshot (Killed sets SOLID_NOT),
    // or who was last seen in a coop notsolid window, is dropped here although the packet is fresh. The
    // server already sent only same-team, solid clients (sv_snapshot.c), so the copy leaks nothing; the bar
    // re-checks team and age itself (cg_drawtools.cpp). The stock radar path below is unchanged.
    if (radarUnpacked->clientNum >= 0 && radarUnpacked->clientNum < MAX_CLIENTS) {
        static cvar_t *pRange = NULL;
        compassMate_t *mate   = &cg.compassMates[radarUnpacked->clientNum];

        if (!pRange) {
            pRange = cgi.Cvar_Get("com_radar_range", "1024", 0); // registered by the exe (common.c); flags untouched
        }
        mate->time      = cg.time;
        mate->origin[0] = radarUnpacked->x;
        mate->origin[1] = radarUnpacked->y;
        mate->yaw       = radarUnpacked->yaw;
        mate->clamped   = qfalse;
        // SV_PackNonPVSClient clamps a delta beyond com_radar_range onto the rim: direction real, distance not
        if (cg.snap && pRange->value > 0.0f) {
            float dx = radarUnpacked->x - cg.snap->ps.origin[0];
            float dy = radarUnpacked->y - cg.snap->ps.origin[1];

            if (dx * dx + dy * dy >= pRange->value * pRange->value * 0.94f) { // 0.97 of the range, squared
                mate->clamped = qtrue;
            }
        }
    }

    if (!CG_ValidRadarClient(&cg_entities[radarUnpacked->clientNum])) {
        return;
    }

    radar             = &cg.radars[radarUnpacked->clientNum];
    radar->time       = cg.time;
    radar->teamShader = CG_RadarIcon();
    // copy origin
    radar->origin[0] = radarUnpacked->x;
    radar->origin[1] = radarUnpacked->y;
    // copy yaw
    YawToAxis(radarUnpacked->yaw, axis);
    radar->axis[0] = axis[0];
    radar->axis[1] = axis[1];
    VectorNormalize2D(radar->axis);
}

void CG_UpdateRadar()
{
    int i;

    for (i = 0; i < MAX_CLIENTS; i++) {
        if (cg.radars[i].time) {
            if (!CG_ValidRadarClient(&cg_entities[i])) {
                cg.radars[i].time = 0;
            }
        }
    }
}

void CG_RadarClientSpeaks(int num)
{
    if (!CG_ValidRadarClient(&cg_entities[num])) {
        return;
    }

    cg.radars[num].lastSpeakTime = cg.time;
}
