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
// Nature effects

#include "cg_local.h"
#include "cg_commands.h"

cvar_t *cg_rain;
cvar_t *cg_rain_drawcoverage;

void RainTouch(ctempmodel_t *ct, trace_t *trace)
{
    Vector norm, neworg;

    ct->ent.hModel    = cgi.R_RegisterModel("splash_z.spr");
    ct->cgd.velocity  = vec_zero;
    ct->cgd.accel     = vec_zero;
    ct->killTime      = cg.time + 400;
    norm              = trace->plane.normal;
    norm.x            = -norm.x;
    norm.y            = -norm.y;
    ct->cgd.angles    = norm.toAngles();
    ct->ent.scale     = 0.3f;
    ct->cgd.scaleRate = 4.0;
    ct->cgd.flags |= T_FADE;

    neworg = trace->endpos + norm * 0.2f;
    VectorCopy(neworg, ct->ent.origin);
}

void ClientGameCommandManager::RainTouch(Event *ev)
{
    // Nothing to do
}

void ClientGameCommandManager::InitializeRainCvars()
{
    int i;

    cg_rain = cgi.Cvar_Get("cg_rain", "1", CVAR_ARCHIVE);
    cg_rain_drawcoverage =
        cgi.Cvar_Get("cg_rain_drawcoverage", "0", CVAR_SAVEGAME | CVAR_RESETSTRING | CVAR_SYSTEMINFO);

    cg.rain.density    = 0.0;
    cg.rain.speed      = 2048.0f;
    cg.rain.length     = 90.0f;
    cg.rain.min_dist   = 512.0f;
    cg.rain.width      = 1.0f;
    cg.rain.speed_vary = 512;
    cg.rain.slant      = 50;

    for (i = 0; i < MAX_RAIN_SHADERS; i++) {
        cg.rain.shader[i][0] = 0;
    }

    cg.rain.numshaders = 0;
}

// HZM coop - set when the legacy entity-based rain (a map rain brush) renders, so CG_RainGlobal below stands
// down and we never double up rain on maps that already author it.
int g_lastRainEntityTime = 0;

/*
====================
CG_RainBrushDropVisible

HZM coop [tester report, bug-3466] "rain goes through roofs". Every roof test the coop rain has (the per-drop sky
gate, the wall clamp) lives in CG_RainGlobal, the path for maps WITHOUT rain brushes. A map that ships its own
func_rain brushes is drawn by the retail CG_Rain below, which tested nothing: a drop starts anywhere in the brush
box - roofs, upper floors and the street are all inside it - and its streak runs to the brush bottom through
whatever is there (57 of 368 roofed standing spots on m5l1b are inside a rain brush or its drift).

Same two tests as the brushless path, per drop: straight up from the drop's start must reach sky
(coop_rainSkyStrict 1 = must land on a SURF_SKY surface), and the streak is cut at the first solid it meets.
coop_rainBrushRoof 0 = the retail behaviour. e3l1 / e3l2 are left exactly as shipped (the user's standing rule
for the Omaha-family maps is that their look is not touched without asking).
====================
*/
static qboolean CG_RainBrushDropVisible(const vec3_t vStart, vec3_t vEnd, int *piLife)
{
    static cvar_t *pOn = NULL, *pStrict = NULL;
    vec3_t         vZero = {0.0f, 0.0f, 0.0f};
    vec3_t         vSkyEnd;
    trace_t        tr;

    if (!pOn) {
        pOn     = cgi.Cvar_Get("coop_rainBrushRoof", "1", CVAR_ARCHIVE);
        pStrict = cgi.Cvar_Get("coop_rainSkyStrict", "1", CVAR_ARCHIVE);
    }
    if (!pOn->integer) {
        return qtrue;
    }
    // not cached: a static could outlive the map it was worked out for, and this is nothing next to two traces
    if (strstr(cgs.mapname, "e3l1") || strstr(cgs.mapname, "e3l2")) {
        return qtrue;
    }

    // SKY GATE: straight up from the drop's own start
    VectorCopy(vStart, vSkyEnd);
    vSkyEnd[2] += 16384.0f;
    cgi.CM_BoxTrace(&tr, vStart, vSkyEnd, vZero, vZero, 0, MASK_SOLID, qfalse);
    // Here the mapper already said "it rains in this box", so a drop is refused only on positive evidence of a
    // roof: the column above it hits a solid that is NOT sky. Open void above (nothing hit) still rains - the
    // strict "must land on a sky surface" rule (coop_rainSkyStrict) is for the brushless path, which has no box.
    (void)pStrict;
    if (!(tr.surfaceFlags & SURF_SKY) && tr.fraction < 0.999f) {
        return qfalse;
    }

    // WALL CLAMP: cut the streak at the first solid surface (roof, wall, ground)
    cgi.CM_BoxTrace(&tr, vStart, vEnd, vZero, vZero, 0, MASK_SOLID, qfalse);
    if (tr.fraction < 1.0f) {
        if (tr.fraction < 0.1f) {
            return qfalse;
        }
        VectorCopy(tr.endpos, vEnd);
        *piLife = (int)(*piLife * tr.fraction);
        if (*piLife < 50) {
            return qfalse;
        }
    }
    return qtrue;
}

// HZM coop [bug-3881] RAIN TO THE GROUND. A streak is drawn from the drop (its head) back up its path for
// rain_length units, and the whole streak was removed the moment the head reached the surface the drop lands on. So
// over the last streak-length above the ground the rain thinned out to nothing: no streak ever covered the ground
// itself, and at head height a standing player saw (82 / length) of the full density - 91 % with ordinary rain
// (length 90), 32 % in the downpour tier (length 260): "the rain stays near the top of the screen". The drop now
// lives one streak length longer and is drawn with BEAM_HZM_TAILRUN (cg_beam.cpp): the head stops at the surface,
// the tail runs on into it. Nothing is drawn beyond the surface, so no streak can show under a roof or a floor.
// coop_rainToGround 1 (default; 0 = as before). Returns the beam flags and stretches *piLife.
static int CG_HZMRainTailRun(const vec3_t vStart, const vec3_t vEnd, int *piLife)
{
    static cvar_t *pOn = NULL;
    vec3_t         vD;
    float          fLen, fTail;

    if (!pOn) { pOn = cgi.Cvar_Get("coop_rainToGround", "1", 0); }
    fTail = (float)(int)cg.rain.length; // what CG_CreateBeam keeps (its streak-length parameter is an int)
    if (!pOn->integer || fTail <= 0.0f || *piLife < 1) {
        return BEAM_INVERTED_FAST;
    }
    VectorSubtract(vEnd, vStart, vD);
    fLen = VectorLength(vD);
    if (fLen < 1.0f) {
        return BEAM_INVERTED_FAST;
    }
    *piLife = (int)((float)*piLife * (fLen + fTail) / fLen + 0.5f); // same speed, one streak length further
    return BEAM_INVERTED_FAST | BEAM_HZM_TAILRUN;
}

// HZM coop [bug-3881] THE FLOOR. The brushless rain's drops fell to 256 units below the eye and were removed there
// whatever was under them, so from a wall, a hill, a tower or an upstairs window the rain stopped in mid-air in a
// flat sheet below the viewer. A drop that reaches that level with nothing solid under it now carries on along its
// own line, at its own speed, down to the first solid or to coop_rainFloor units below the eye (default 1024;
// 256 = as before). One extra trace, and only for a drop that found no ground - none on level ground. A drop that
// found no ground and falls flatter than 60 degrees from the vertical is not drawn: it started just above the old
// floor and was a short sideways dash there (level ground removes the same drops by itself). Snow has the same
// floor and gets the same way down (a flake may live 20 s instead of 10 for it); a flat-flying flake is left as it
// was. Returns qfalse for a drop that is not to be drawn.
static qboolean CG_HZMRainFloor(const vec3_t vStart, vec3_t vEnd, int *piLife)
{
    static cvar_t *pFloor = NULL;
    vec3_t         vZero = {0.0f, 0.0f, 0.0f};
    vec3_t         vDir, vFar;
    float          fFloor, fLen, fMore, fLife;
    trace_t        tr;

    if (!pFloor) { pFloor = cgi.Cvar_Get("coop_rainFloor", "1024", 0); }
    fFloor = pFloor->value;
    if (fFloor > 4096.0f) { fFloor = 4096.0f; }
    if (fFloor <= 256.0f) {
        return qtrue;
    }
    VectorSubtract(vEnd, vStart, vDir);
    fLen = VectorNormalize(vDir);
    if (fLen < 1.0f || *piLife < 1) {
        return qfalse;
    }
    if (vDir[2] > -0.5f) {
        // flatter than 60 degrees from the vertical. A rain streak is not drawn (see above); a snowflake (streak
        // under 32 units: wind-driven flakes fly flat by design) is left exactly as it was
        return (cg.rain.length >= 32.0f) ? qfalse : qtrue;
    }
    fMore = (fFloor - 256.0f) / -vDir[2];
    VectorMA(vEnd, fMore, vDir, vFar);
    cgi.CM_BoxTrace(&tr, vEnd, vFar, vZero, vZero, 0, MASK_SOLID, qfalse);
    if (tr.startsolid) {
        return qtrue;
    }
    fMore *= tr.fraction;
    fLife = (float)*piLife * (fLen + fMore) / fLen;
    if (fLife > 20000.0f) {
        // a slow snowflake: the way down from the old floor may take up to 10 s more than the 10 s a drop was
        // allowed before, at the flake's own speed; it stops where it gets to in that time
        fMore = fLen * (20000.0f / (float)*piLife) - fLen;
        fLife = 20000.0f;
        if (fMore < 0.0f) {
            return qtrue;
        }
    }
    VectorMA(vEnd, fMore, vDir, vEnd);
    *piLife = (int)fLife;
    return qtrue;
}

// HZM coop [bug-3881] RAIN'S SHARE OF THE BEAM POOL. Rain and snow stop spawning while coop_rainMaxBeams beams
// (default 4096 = the whole pool as it was) are alive; the pool itself is 8192 now (cg_commands.h). The spawn loops
// leave at the first drop that finds the share full, so a pool-bound snowfall no longer traces thousands of flakes
// a second that could never be created.
static qboolean CG_HZMRainPoolFull(void)
{
    extern int     cg_hzmLiveBeams;
    static cvar_t *pMax = NULL;
    int            iMax;

    if (!pMax) { pMax = cgi.Cvar_Get("coop_rainMaxBeams", "4096", 0); }
    iMax = pMax->integer;
    if (iMax < 256) { iMax = 256; } else if (iMax > MAX_BEAMS - 256) { iMax = MAX_BEAMS - 256; }
    return (cg_hzmLiveBeams >= iMax) ? qtrue : qfalse;
}

void CG_Rain(centity_t *cent)
{
    int         iLife;
    int         iBeamFlags;
    vec3_t      mins, maxs;

    g_lastRainEntityTime = cg.time; // a rain brush exists on this map
    vec3_t      vOmins, vOmaxs, vOe;
    float       fcolor[4];
    vec3_t      vStart, vEnd;
    int         iNumSpawn;
    int         i;
    int         iRandom;
    float       fDensity;
    vec3_t      vLength;
    const char *shadername;

    fcolor[0] = 1.0;
    fcolor[1] = 1.0;
    fcolor[2] = 1.0;
    fcolor[3] = 1.0;

    if (!cg_rain->integer || paused->integer) {
        return;
    }

    cgi.R_ModelBounds(cgs.inlineDrawModel[cent->currentState.modelindex], mins, maxs);

    // Fixed in 2.0
    //  Use cg.refdef.vieworg instead of cg.snap.ps.origin

    vOmins[0] = mins[0] + cent->lerpOrigin[0];
    if (vOmins[0] < cg.refdef.vieworg[0] - cg.rain.min_dist) {
        vOmins[0] = cg.refdef.vieworg[0] - cg.rain.min_dist;
    }

    vOmins[1] = mins[1] + cent->lerpOrigin[1];
    if (vOmins[1] < cg.refdef.vieworg[1] - cg.rain.min_dist) {
        vOmins[1] = cg.refdef.vieworg[1] - cg.rain.min_dist;
    }

    vOmins[2] = mins[2] + cent->lerpOrigin[2];

    vOmaxs[0] = maxs[0] + cent->lerpOrigin[0];
    if (vOmaxs[0] > cg.refdef.vieworg[0] + cg.rain.min_dist) {
        vOmaxs[0] = cg.refdef.vieworg[0] + cg.rain.min_dist;
    }

    vOmaxs[1] = maxs[1] + cent->lerpOrigin[1];
    if (vOmaxs[1] > cg.refdef.vieworg[1] + cg.rain.min_dist) {
        vOmaxs[1] = cg.refdef.vieworg[1] + cg.rain.min_dist;
    }

    vOmaxs[2] = maxs[2] + cent->lerpOrigin[2];

    if (vOmins[0] > vOmaxs[0]) {
        return;
    }

    if (vOmins[1] > vOmaxs[1]) {
        return;
    }

    if (cg_rain_drawcoverage->integer) {
        cgi.R_DebugLine(
            Vector(vOmins[0], vOmins[1], vOmins[2]), Vector(vOmaxs[0], vOmins[1], vOmins[2]), 1.0, 0.0, 0.0, 1.0
        );

        cgi.R_DebugLine(
            Vector(vOmaxs[0], vOmins[1], vOmins[2]), Vector(vOmaxs[0], vOmaxs[1], vOmins[2]), 1.0, 0.0, 0.0, 1.0
        );

        cgi.R_DebugLine(
            Vector(vOmaxs[0], vOmaxs[1], vOmins[2]), Vector(vOmins[0], vOmaxs[1], vOmins[2]), 1.0, 0.0, 0.0, 1.0
        );

        cgi.R_DebugLine(
            Vector(vOmins[0], vOmaxs[1], vOmins[2]), Vector(vOmins[0], vOmins[1], vOmins[2]), 1.0, 0.0, 0.0, 1.0
        );
    }

    VectorSubtract(vOmaxs, vOmins, vOe);
    fDensity  = cg.rain.density / 200.0;
    iNumSpawn = (int)(sqrt(vOe[0] * vOe[1]) * fDensity);
    if (iNumSpawn > MAX_BEAMS) {
        iNumSpawn = MAX_BEAMS;
    }
    {
        extern int   CG_HZMRainSpawnCount(int iPerFrame, float *pCarry);
        static float sCarryBrush = 0.0f;
        iNumSpawn = CG_HZMRainSpawnCount(iNumSpawn, &sCarryBrush); // HZM coop [bug-3558] by elapsed time
    }

    iRandom = rand();

    if (cg.rain.numshaders) {
        shadername = cg.rain.shader[iRandom % cg.rain.numshaders];
    } else {
        shadername = cg.rain.shader[0];
    }

    for (i = 0; i < iNumSpawn; ++i) {
        if (CG_HZMRainPoolFull()) {
            break; // HZM coop [bug-3881]
        }
        {
            extern int CG_HZMRainSeed(void);
            int        iSeed = CG_HZMRainSeed(); // HZM coop [bug-3552] a fresh seed for every drop
            if (iSeed >= 0) { iRandom = iSeed; }
        }
        vStart[0] = (float)(iRandom % (int)(vOe[0] + 1.0)) + vOmins[0];
        iRandom   = ((214013 * iRandom + 2531011) >> 16) & 0x7FFF;
        vStart[1] = (float)(iRandom % (int)(vOe[1] + 1.0)) + vOmins[1];
        iRandom   = ((214013 * iRandom + 2531011) >> 16) & 0x7FFF;
        // Fixed in 2.0
        //  Use random height instead of the max height to avoid waiting for it to fall down
        vStart[2] = (float)(iRandom % (int)(vOe[2] + 1.0)) + vOmins[2];

        VectorSubtract(cg.refdef.vieworg, vStart, vLength);
        vLength[2] = 0;

        if (VectorLengthSquared(vLength) > Square(cg.rain.min_dist)) {
            continue;
        }

        iRandom = ((214013 * iRandom + 2531011) >> 16) & 0x7FFF;

        vEnd[0] = (float)(iRandom % cg.rain.slant) + vStart[0] + vss_wind_x->value;
        iRandom = ((214013 * iRandom + 2531011) >> 16) & 0x7FFF;
        vEnd[1] = (float)(iRandom % cg.rain.slant) + vStart[1] + vss_wind_y->value;
        vEnd[2] = vOmins[2];

        // Fixed in 2.0
        //  Use individual particle's origin to determine the life rather than the bounding box
        iLife = (int)((vStart[2] - vOmins[2]) / ((float)(iRandom % cg.rain.speed_vary) + cg.rain.speed) * 1000.0);
        if (iLife > 10000) {
            // Fixed in 2.0
            //  Rain particles must not last longer than 10 seconds
            iLife = 10000;
        }

        // HZM coop [bug-3466] the brush path had no world test at all
        if (!CG_RainBrushDropVisible(vStart, vEnd, &iLife)) {
            continue;
        }
        {
            extern qboolean CG_HZMRainTooNearX(const vec3_t vStart, const vec3_t vEnd);
            if (CG_HZMRainTooNearX(vStart, vEnd)) {
                continue; // HZM coop [2026-10-12] near cull (see CG_HZMRainTooNear)
            }
        }
        iBeamFlags = CG_HZMRainTailRun(vStart, vEnd, &iLife); // HZM coop [bug-3881] the streak runs into the surface

        CG_CreateBeam(
            vStart,
            vec_zero,
            0,
            1,
            1.0,
            cg.rain.width,
            iBeamFlags,
            1000.0,
            iLife,
            qtrue,
            vEnd,
            0,
            0,
            0,
            1,
            0,
            shadername,
            fcolor,
            0,
            0.0,
            cg.rain.length,
            1.0,
            0,
            "raineffect"
        );
    }
}

// HZM coop [bug-3552] A DIFFERENT DROP EVERY TIME. Both spawn loops draw every coordinate of every drop from one
// 15-bit generator ((214013 * x + 2531011) >> 16 & 0x7FFF), seeded once a frame. That map runs into a loop of 135
// states after about 300 steps, so one frame could produce only ~96 different drops however many were asked for -
// the rest were exact copies on the same paths. Ordinary rain asks for ~50 a frame and never showed it; anything
// denser (the downpour tier, snow) drew stacks of copies: dotted trails, a solid white blade by the eye, and a
// visible density that followed the frame rate, not the count. Each drop now starts from a fresh seed of a 32-bit
// xorshift; the loop's own arithmetic after that is untouched. coop_rainSeed 1 (default; 0 = one seed a frame).
int CG_HZMRainSeed(void)
{
    static cvar_t      *pSeed = NULL;
    static unsigned int uState = 0;

    if (!pSeed) { pSeed = cgi.Cvar_Get("coop_rainSeed", "1", 0); }
    if (!pSeed->integer) {
        return -1;
    }
    if (!uState) {
        uState = ((unsigned int)rand() << 16) ^ (unsigned int)rand() ^ (unsigned int)cg.time ^ 0x9E3779B9u;
        if (!uState) { uState = 1u; }
    }
    uState ^= uState << 13;
    uState ^= uState >> 17;
    uState ^= uState << 5;
    return (int)((uState >> 9) & 0x7FFF);
}

// HZM coop [bug-3558] SPAWN BY TIME. The spawn count was per FRAME (sqrt(area) * density / 200 every frame), so a
// 30 fps player drew half the rain of a 60 fps one and a 200 fps one over three times as much - and filled the 4096
// streak pool that much sooner. The per-frame count is now scaled by the frame's length against a reference frame
// rate: coop_rainByTime 1 (default; 0 = the old per-frame count), coop_rainRefFps 125 (the rate at which the count is
// what it was: 125 is the frame cap the mod sets on a hosting machine, start_server.cfg, so the host's rain is unchanged). The fraction is carried to the next frame, so a count under one a frame still spawns; a frame
// longer than four reference frames (a hitch) counts as four, so it never dumps a burst.
int CG_HZMRainSpawnCount(int iPerFrame, float *pCarry)
{
    static cvar_t *pByTime = NULL;
    static cvar_t *pRefFps = NULL;
    float          fRef, fScale, fWant;

    if (!pByTime) { pByTime = cgi.Cvar_Get("coop_rainByTime", "1", 0); }
    if (!pRefFps) { pRefFps = cgi.Cvar_Get("coop_rainRefFps", "125", 0); }
    if (!pByTime->integer) {
        return iPerFrame;
    }
    fRef = pRefFps->value;
    if (fRef < 20.0f) { fRef = 20.0f; } else if (fRef > 500.0f) { fRef = 500.0f; }
    fScale = (float)cg.frametime * fRef / 1000.0f;
    if (fScale < 0.0f) { fScale = 0.0f; } else if (fScale > 4.0f) { fScale = 4.0f; }
    fWant     = (float)iPerFrame * fScale + *pCarry;
    iPerFrame = (int)fWant;
    *pCarry   = fWant - (float)iPerFrame;
    if (iPerFrame > MAX_BEAMS) {
        iPerFrame = MAX_BEAMS;
    }
    return iPerFrame;
}

// HZM coop [2026-10-12] NEAR CULL. A streak that passes within a few units of the eye is drawn as one white blade
// across a strip of the screen - rare in ordinary rain, constant in the downpour tier (sixteen times the drops, long
// streaks) and in the blizzard's driven flakes. A drop whose path - including the streak's own length trailing behind
// the start - comes closer to the viewer than coop_rainNearCull units (default 36, 0 = off) is not spawned. Tested
// against where the viewer is NOW: right for rain (a drop lives ~0.2 s), approximate for a flake that lives a second.
static qboolean CG_HZMRainTooNear(const vec3_t vStart, const vec3_t vEnd)
{
    static cvar_t *pNear = NULL;
    vec3_t         vDir, vA, vTo, vC, vD;
    float          fLen, fT;

    if (!pNear) { pNear = cgi.Cvar_Get("coop_rainNearCull", "36", 0); }
    if (pNear->value <= 0.0f) {
        return qfalse;
    }
    VectorSubtract(vEnd, vStart, vDir);
    fLen = VectorNormalize(vDir);
    VectorMA(vStart, -cg.rain.length, vDir, vA); // the streak trails this far behind the drop
    fLen += cg.rain.length;
    VectorSubtract(cg.refdef.vieworg, vA, vTo);
    fT = DotProduct(vTo, vDir);
    if (fT < 0.0f) { fT = 0.0f; } else if (fT > fLen) { fT = fLen; }
    VectorMA(vA, fT, vDir, vC);
    VectorSubtract(vC, cg.refdef.vieworg, vD);
    return (VectorLengthSquared(vD) < pNear->value * pNear->value) ? qtrue : qfalse;
}

qboolean CG_HZMRainTooNearX(const vec3_t vStart, const vec3_t vEnd)
{
    return CG_HZMRainTooNear(vStart, vEnd);
}

// HZM coop - GLOBAL dynamic precipitation. Vanilla CG_Rain only renders inside a map-placed rain BRUSH
// (most maps don't have one) and never checks for ceilings. This renders rain/snow in a box around the
// player WITHOUT a brush (so dynamic weather works on any map), and SKY-GATES every column with an up-trace
// (SURF_SKY = open sky) so drops never fall indoors / under roofs / overhangs. Driven by the same level.rain_*
// params the coop weather script ramps. Gated by coop_dynRainGlobal so legacy rain-brush maps are untouched.
void CG_RainGlobal(void)
{
    static cvar_t *pGlobal = NULL;
    int            iNumSpawn, i, iRandom, iLife, iSlant, iSpeedVary, iBeamFlags;
    float          fDensity;
    float          fcolor[4];
    vec3_t         vOmins, vOmaxs, vOe, vStart, vEnd, vLength;
    vec3_t         vZero = {0.0f, 0.0f, 0.0f};
    const char    *shadername;
    qboolean       bEntRoof = qfalse; // the player stands under a vehicle / model roof (see the roof test below)

    if (!pGlobal) {
        pGlobal = cgi.Cvar_Get("coop_dynRainGlobal", "1", CVAR_ARCHIVE); // master toggle (off if it misbehaves)
    }
    if (!pGlobal->integer || !cg_rain->integer || paused->integer) {
        return;
    }
    // Driven purely by the networked cg.rain.density so EVERY coop client sees the weather. 0 = clear.
    if (cg.rain.density <= 0.0f || cg.rain.min_dist <= 1.0f) {
        return;
    }
    // A map with its own rain brush drives the entity-based CG_Rain; stand down so we never double the rain.
    if (g_lastRainEntityTime && (cg.time - g_lastRainEntityTime) < 1000) {
        return;
    }

    // [user 07-11] HARD interior / vehicle cull. The per-drop sky-gate below uses cgi.CM_BoxTrace, which
    // only sees the WORLD (BSP) - so a VEHICLE roof (the truck is an ENTITY) or an entity/script-model roof
    // is invisible to it, and rain leaks into the truck and some interiors. Trace UP from the PLAYER (NOT
    // the camera - in 3rd person the camera sits above the roof and would see open sky) with an
    // entity-INCLUSIVE CG_Trace: if anything solid that isn't sky is above the player, kill the rain
    // entirely. The per-drop world gate still handles partial exposure when the player is genuinely outside.
    {
        trace_t roofTr;
        vec3_t  vpStart, vpEnd;
        // HZM coop - demo editor: under the free camera the roof test belongs to the CAMERA (an indoor recorded
        // player must not kill the rain the camera is standing in)
        const float *pRoofOrg = CG_DemoCamActive() ? cg.refdef.vieworg : cg.predicted_player_state.origin;
        vpStart[0] = pRoofOrg[0];
        vpStart[1] = pRoofOrg[1];
        vpStart[2] = pRoofOrg[2] + (CG_DemoCamActive() ? 0.0f : 40.0f); // ~chest height, off the floor
        vpEnd[0]   = vpStart[0];
        vpEnd[1]   = vpStart[1];
        // HZM coop [user 2026-08-24] RAIN INDOORS. The old test read
        //     if (fraction < 0.999 && !SURF_SKY) -> dry
        // i.e. "if we hit something and it is not sky, you are dry" - so when the trace hit NOTHING it
        // fell through and drew rain. "Hit nothing" was being treated as "open sky", which is why any
        // interior taller than the 4096u trace, or with a gap the trace escaped through, rained inside.
        //
        // Inverted: wet only if the trace ACTUALLY LANDS ON SKY. Lengthened to 16384 so a genuinely
        // tall outdoor map still finds its sky brush instead of going falsely dry.
        // coop_rainSkyStrict 0 restores the old behaviour if a map turns out to need it.
        vpEnd[2]   = vpStart[2] + 16384.0f;
        CG_Trace(&roofTr, vpStart, vZero, vZero, vpEnd, cg.snap->ps.clientNum, MASK_SOLID, qfalse, qtrue, "CG_RainRoof");
        {
            static cvar_t *pStrict = NULL;
            static cvar_t *pSeeOut = NULL;
            qboolean       bDry;

            if (!pStrict) { pStrict = cgi.Cvar_Get("coop_rainSkyStrict", "1", CVAR_ARCHIVE); }
            if (!pSeeOut) { pSeeOut = cgi.Cvar_Get("coop_rainSeeOut", "1", CVAR_ARCHIVE); }
            if (pStrict->integer) {
                bDry = !(roofTr.surfaceFlags & SURF_SKY); // no sky overhead, INCLUDING "hit nothing at all"
            } else {
                bDry = roofTr.fraction < 0.999f && !(roofTr.surfaceFlags & SURF_SKY);
            }
            if (bDry) {
                // HZM coop [user 2026-10-12] "when it rains outside and you are standing indoors, you don't actually
                // see the rain outside until you actually go outside". This used to return here: a roof over the
                // PLAYER switched off every drop, the ones falling in the street beyond the window included. Under a
                // world roof that was never needed - the per-drop sky gate below keeps each drop out of every world
                // interior by itself. What the cull was added for is a roof the per-drop trace cannot see (a
                // vehicle or model roof is an entity, the per-drop trace is world-only): there the player now
                // stands in a dry circle and the rain goes on around it. coop_rainSeeOut 0 = the old cull.
                if (!pSeeOut->integer) {
                    return;
                }
                bEntRoof = roofTr.fraction < 0.999f && roofTr.entityNum != ENTITYNUM_WORLD;
            }
        }
    }

    fcolor[0] = fcolor[1] = fcolor[2] = fcolor[3] = 1.0f;

    // player-centred volume (no map brush needed)
    vOmins[0] = cg.refdef.vieworg[0] - cg.rain.min_dist;
    vOmins[1] = cg.refdef.vieworg[1] - cg.rain.min_dist;
    vOmins[2] = cg.refdef.vieworg[2] - 256.0f;
    vOmaxs[0] = cg.refdef.vieworg[0] + cg.rain.min_dist;
    vOmaxs[1] = cg.refdef.vieworg[1] + cg.rain.min_dist;
    vOmaxs[2] = cg.refdef.vieworg[2] + 640.0f;

    VectorSubtract(vOmaxs, vOmins, vOe);
    if (vOe[0] < 1.0f || vOe[1] < 1.0f || vOe[2] < 1.0f) {
        return;
    }

    fDensity  = cg.rain.density / 200.0f;
    iNumSpawn = (int)(sqrt(vOe[0] * vOe[1]) * fDensity);
    if (iNumSpawn > MAX_BEAMS) {
        iNumSpawn = MAX_BEAMS;
    }
    {
        static float sCarryGlobal = 0.0f;
        iNumSpawn = CG_HZMRainSpawnCount(iNumSpawn, &sCarryGlobal); // HZM coop [bug-3558] by elapsed time
    }

    iSlant     = (cg.rain.slant < 1) ? 1 : cg.rain.slant;            // snow uses slant 1; guard div-by-zero
    iSpeedVary = (cg.rain.speed_vary < 1) ? 1 : cg.rain.speed_vary;  // guard div-by-zero

    iRandom    = rand();
    shadername = cg.rain.numshaders ? cg.rain.shader[iRandom % cg.rain.numshaders] : cg.rain.shader[0];

    for (i = 0; i < iNumSpawn; ++i) {
        trace_t skyTr;
        vec3_t  vSkyStart, vSkyEnd;

        if (CG_HZMRainPoolFull()) {
            break; // HZM coop [bug-3881]
        }

        {
            int iSeed = CG_HZMRainSeed(); // HZM coop [bug-3552] a fresh seed for every drop
            if (iSeed >= 0) { iRandom = iSeed; }
        }
        vStart[0] = (float)(iRandom % (int)(vOe[0] + 1.0f)) + vOmins[0];
        iRandom   = ((214013 * iRandom + 2531011) >> 16) & 0x7FFF;
        vStart[1] = (float)(iRandom % (int)(vOe[1] + 1.0f)) + vOmins[1];
        iRandom   = ((214013 * iRandom + 2531011) >> 16) & 0x7FFF;
        vStart[2] = (float)(iRandom % (int)(vOe[2] + 1.0f)) + vOmins[2];

        VectorSubtract(cg.refdef.vieworg, vStart, vLength);
        vLength[2] = 0;
        if (VectorLengthSquared(vLength) > Square(cg.rain.min_dist)) {
            continue;
        }
        if (bEntRoof) {
            // under a vehicle / model roof: nothing within 256 units of the player (a truck bed, a covered boat)
            const float *pOrg = CG_DemoCamActive() ? cg.refdef.vieworg : cg.predicted_player_state.origin; // demo editor
            float dx = vStart[0] - pOrg[0];
            float dy = vStart[1] - pOrg[1];

            if (dx * dx + dy * dy < 256.0f * 256.0f) {
                continue;
            }
        }

        // SKY GATE: trace straight up this column from the DROP's own height; only rain where the sky is
        // genuinely open above it (so it stops dead under roofs/awnings and never falls into interiors).
        // HZM fix: was tracing from cg.refdef.vieworg[2] (the CAMERA height). In 3rd person the camera sits
        // above/behind the player, so when it rose above a building's roof the gate saw open sky and rained
        // even though the drops (spawned around the player, camera-256..+640) were UNDER the roof - the
        // "rain sometimes still goes through interiors" leak. Tracing from vStart[2] checks each drop's
        // actual exposure, camera position irrelevant.
        vSkyStart[0] = vStart[0];
        vSkyStart[1] = vStart[1];
        vSkyStart[2] = vStart[2];
        vSkyEnd[0]   = vStart[0];
        vSkyEnd[1]   = vStart[1];
        // same inversion as the per-player roof gate: a drop is drawn only if the column above it
        // actually reaches SKY. Previously a trace that hit nothing still drew the drop.
        vSkyEnd[2]   = vStart[2] + 16384.0f;
        cgi.CM_BoxTrace(&skyTr, vSkyStart, vSkyEnd, vZero, vZero, 0, MASK_SOLID, qfalse);
        {
            static cvar_t *pStrict2 = NULL;
            if (!pStrict2) { pStrict2 = cgi.Cvar_Get("coop_rainSkyStrict", "1", CVAR_ARCHIVE); }
            if (pStrict2->integer) {
                if (!(skyTr.surfaceFlags & SURF_SKY)) {
                    continue; // no sky in this column -> no drop
                }
            } else if (!(skyTr.surfaceFlags & SURF_SKY) && skyTr.fraction < 0.999f) {
                continue;
            }
        }

        iRandom = ((214013 * iRandom + 2531011) >> 16) & 0x7FFF;
        vEnd[0] = (float)(iRandom % iSlant) + vStart[0] + vss_wind_x->value;
        iRandom = ((214013 * iRandom + 2531011) >> 16) & 0x7FFF;
        vEnd[1] = (float)(iRandom % iSlant) + vStart[1] + vss_wind_y->value;
        vEnd[2] = vOmins[2];

        iLife = (int)((vStart[2] - vOmins[2]) / ((float)(iRandom % iSpeedVary) + cg.rain.speed) * 1000.0f);
        if (iLife > 10000) {
            iLife = 10000;
        }

        // HZM coop - WALL CLAMP: the sky gate above only checks the drop's START column, but rain streaks
        // slant up to rain_slant (250) horizontal units as they fall (+ wind) and beams render THROUGH
        // geometry - so drops spawned legitimately outside a building knifed diagonally through the roof /
        // walls into interiors (user screenshot: rain inside a closed room). Trace along the streak and cut
        // it at the first solid surface so rain visibly stops at walls and roofs. Snow (slant 1) barely
        // drifts, so this mostly no-ops there.
        {
            trace_t wallTr;
            cgi.CM_BoxTrace(&wallTr, vStart, vEnd, vZero, vZero, 0, MASK_SOLID, qfalse);
            if (wallTr.fraction < 1.0f) {
                if (wallTr.fraction < 0.1f) {
                    continue; // streak would be a stub - skip the drop entirely
                }
                VectorCopy(wallTr.endpos, vEnd);
                iLife = (int)(iLife * wallTr.fraction); // die on impact so the visible speed stays constant
                if (iLife < 50) {
                    continue;
                }
            } else if (!CG_HZMRainFloor(vStart, vEnd, &iLife)) {
                continue; // HZM coop [bug-3881] nothing solid above the old floor: see CG_HZMRainFloor
            }
        }

        // HZM coop [bug-3529] END GATE. The sky gate above looks at the column over the drop's START. A slanted drop
        // that starts under open sky next to a building and falls in through a doorway, a window or the open side of
        // a barn ENDS under the roof - seen as stray drops inside rooms once a roof over the player stopped
        // switching all rain off (coop_rainSeeOut). So the column over where the drop ends must reach sky as well.
        // Tested 2 units back up the streak, so a drop that died on a surface is not tested from inside that surface.
        {
            static cvar_t *pEndGate = NULL;

            if (!pEndGate) { pEndGate = cgi.Cvar_Get("coop_rainEndGate", "1", CVAR_ARCHIVE); }
            if (pEndGate->integer) {
                trace_t endTr;
                vec3_t  vBack, vEndUp0, vEndUp1;

                VectorSubtract(vStart, vEnd, vBack);
                VectorNormalizeFast(vBack);
                VectorMA(vEnd, 2.0f, vBack, vEndUp0);
                VectorCopy(vEndUp0, vEndUp1);
                vEndUp1[2] += 16384.0f;
                cgi.CM_BoxTrace(&endTr, vEndUp0, vEndUp1, vZero, vZero, 0, MASK_SOLID, qfalse);
                if (endTr.startsolid || (!(endTr.surfaceFlags & SURF_SKY) && endTr.fraction < 0.999f)) {
                    continue; // lands under cover
                }
            }
        }

        if (CG_HZMRainTooNear(vStart, vEnd)) {
            continue; // HZM coop [2026-10-12] near cull
        }
        iBeamFlags = CG_HZMRainTailRun(vStart, vEnd, &iLife); // HZM coop [bug-3881] the streak runs into the surface
        CG_CreateBeam(
            vStart, vec_zero, 0, 1, 1.0, cg.rain.width, iBeamFlags, 1000.0, iLife, qtrue,
            vEnd, 0, 0, 0, 1, 0, shadername, fcolor, 0, 0.0, cg.rain.length, 1.0, 0, "raineffect"
        );
    }
}
