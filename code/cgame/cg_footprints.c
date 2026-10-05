/*
===========================================================================
HZM coop - FOOTPRINTS in snow, mud and soft dirt (user 2026-10-04: "footprints for snow and mud would be ideal too").

Client-side only. Driven by the footstep the engine already plays (CG_Footstep, cg_specialfx.cpp), so it costs no
network and every client draws every soldier's prints - players and AI alike - from its own animation.

  - stamped where a foot lands: the footstep trace already found the ground under the "Bip01 L/R Foot" tag;
    the print is laid along the model's forward axis, mirrored for the left foot, so a trail alternates L/R and
    follows the walking direction, and stride spacing comes from the real step cadence (walk vs run).
  - surfaces: the footstep surface flag (SURF_SNOW / SURF_MUD / SURF_DIRT) AND a shader-name allowlist, because
    the retail BSPs flag concrete, crates, beams and rubble as snow/dirt. Never wood, stone, metal, sand, water.
  - Omaha is never touched (user hard rule): the five-map list (HZM_WaterWetOmahaMap) refuses the whole map, and
    any omaha/wetsand/shoreline/surf/seabed/ocean/wake shader is refused on every other map.
  - geometry: brushes and curved patches go through the bullet-mark clipper (CG_GetMarkFragments), so the print lies
    on the VISIBLE surface, including under the footstep-only snowclip/mudclip/dirtclip brushes maps lay over the
    ground (m2l1). LOD terrain (gl2's clipper has no terrain path): a 2x3 vertex grid traced onto the collision
    surface, dropped if a corner leaves the plane (a curb, a step) or lands on a refused surface.
  - own ring buffer (cg_footprintMax, default 200, hard cap FP_MAX), its own life (cg_footprintTime, default 45 s,
    fading over the last third) - independent of the bullet-hole pool (cg_marks_max) and cg_marks_add.
  - drawn MULTIPLICATIVELY (shader blendFunc GL_ZERO GL_ONE_MINUS_SRC_COLOR, texture = how much to darken): an
    alpha-blended print brought its own brightness from the light grid and showed as a pale outlined sticker on
    m2l1's dusk snow; a multiply darkens the lightmapped ground by the same fraction in any light.
  - no prints while prone (prone hull is 20 tall), dead, on a ladder, or in water.
  - cg_footprints 0/1 (Graphics menu, ARCHIVE, default 1).
===========================================================================
*/

#include "cg_local.h"
#include "../renderercommon/hzm_waterwet.h"   // HZM_WaterWetOmahaMap (the five-map Omaha list)

#define FP_MAX       256     // hard ceiling of the ring buffer; cg_footprintMax clamps to it
#define FP_ROWS      3
#define FP_COLS      2
#define FP_NUMVERTS  (FP_ROWS * FP_COLS)
#define FP_LENGTH    18.0f   // quad along the foot (the boot fills ~86% of the texture's height: ~15.5 u)
#define FP_WIDTH     9.0f    // quad across the foot (the sole fills ~60% of the width at the ball: ~5.4 u)
#define FP_FORWARD   3.5f    // the foot tag is the ankle, over the back of the boot: centre the print ahead of it
#define FP_LIFT      0.35f   // above the traced ground; polygonOffset in the shader does the rest
#define FP_PLANE_TOL 2.5f    // a corner further than this off the centre's plane = the print would bridge a step
#define FP_DRAWDIST  1800.0f // beyond this a 15-unit print is a pixel or two
#define MAX_FP_MARK_POINTS    384   // CG_GetMarkFragments fills up to cg_marks.c's MAX_MARK_POINTS / FRAGMENTS
#define MAX_FP_MARK_FRAGMENTS 128

enum { FP_SNOW, FP_MUD, FP_DIRT, FP_NUMSURF };

#define FP_MAXPOLYS  12   // world fragments kept per print; more (a dense patch) -> the grid path
#define FP_MAXPV     8

typedef struct {
    int       time;       // cg.time at stamping, 0 = free
    qhandle_t shader;
    float     alpha;      // full-strength alpha for this surface
    vec3_t    center;
    int       leafnum;
    int       numPolys;
    int       numVerts[FP_MAXPOLYS];
    vec3_t    xyz[FP_MAXPOLYS][FP_MAXPV];
    float     st[FP_MAXPOLYS][FP_MAXPV][2];
} footprint_t;

// cg_marks.c (not in cg_local.h): the renderer's decal clipper, brush + patch surfaces in both renderers
int CG_GetMarkFragments(
    int             numVerts,
    const vec3_t   *pVerts,
    const vec3_t    vProjection,
    const vec3_t   *pPointBuffer,
    markFragment_t *pFragmentBuffer,
    float           fRadiusSquared
);

typedef struct {
    int    time[2];
    vec3_t pos[2];
} footprintEnt_t;

static footprint_t    s_fp[FP_MAX];
static int            s_fpNext;
static footprintEnt_t s_fpEnt[MAX_GENTITIES];
static qboolean       s_fpOmaha;
static int            s_fpStamped;   // since map start, for the debug line
static qhandle_t      s_fpShader[FP_NUMSURF][2];   // [surface][0 = allied rubber sole, 1 = axis hobnail]

cvar_t *cg_footprints;
cvar_t *cg_footprintTime;
cvar_t *cg_footprintMax;
cvar_t *cg_footprintDebug;

static const float s_fpAlpha[FP_NUMSURF]       = {1.0f, 0.95f, 0.6f};
static const char *const s_fpSurfName[FP_NUMSURF] = {"snow", "mud", "dirt"};

void CG_InitFootprints(void)
{
    cg_footprints     = cgi.Cvar_Get("cg_footprints", "1", CVAR_ARCHIVE);
    cg_footprintTime  = cgi.Cvar_Get("cg_footprintTime", "45", 0);
    cg_footprintMax   = cgi.Cvar_Get("cg_footprintMax", "200", 0);
    cg_footprintDebug = cgi.Cvar_Get("cg_footprintDebug", "0", 0);

    memset(s_fp, 0, sizeof(s_fp));
    memset(s_fpEnt, 0, sizeof(s_fpEnt));
    s_fpNext    = 0;
    s_fpStamped = 0;
    s_fpOmaha   = HZM_WaterWetOmahaMap(cgs.mapname);

    // own shader names + private texture paths (scripts/coop_footprints.shader, the bug-922 isolation recipe)
    s_fpShader[FP_SNOW][0] = cgi.R_RegisterShader("coop_footprint_snow_allied");
    s_fpShader[FP_SNOW][1] = cgi.R_RegisterShader("coop_footprint_snow_axis");
    s_fpShader[FP_MUD][0]  = cgi.R_RegisterShader("coop_footprint_mud_allied");
    s_fpShader[FP_MUD][1]  = cgi.R_RegisterShader("coop_footprint_mud_axis");
    // dry soft dirt reuses the mud prints at a lower alpha (s_fpAlpha)
    s_fpShader[FP_DIRT][0] = s_fpShader[FP_MUD][0];
    s_fpShader[FP_DIRT][1] = s_fpShader[FP_MUD][1];
}

static const char *const s_fpDeny[] = {
    "omaha", "wetsand", "shore", "surf", "seabed", "ocean", "wake", "sand", "clip", "rubble", "rock", "road",
    "gravel", "brick", "stone", "wall", "conc", "wood", "floor", "flr", "plaster", "facade", "tile", "cliff", "ice",
    "frozen", "roof", "tree", "log", "crate", "plank", "corrugat", "metal", "iron", "water", "puddle"};

// qtrue when the shader name carries a refused token (conservative substring match: "surf" also refuses "surface",
// "tree" "street" - a missed print is fine, a print on the wrong ground is not)
static qboolean CG_FootprintDenied(int shaderNum)
{
    baseshader_t *bs;
    char          name[MAX_QPATH];
    int           i;

    if (shaderNum < 0 || !(bs = cgi.GetShader(shaderNum)) || !bs->shader[0]) {
        return qtrue;
    }
    Q_strncpyz(name, bs->shader, sizeof(name));
    Q_strlwr(name);
    for (i = 0; i < (int)ARRAY_LEN(s_fpDeny); i++) {
        if (strstr(name, s_fpDeny[i])) {
            return qtrue;
        }
    }
    return qfalse;
}

/*
The surface class of a trace hit, or -1. Flag AND name: the retail BSPs carry SURF_SNOW on concrete, crates, ibeams and
plaster, and SURF_DIRT on rubble, cobbles, roads and walls (survey of every retail BSP, 2026-10-04).
*/
static int CG_FootprintSurface(int surfaceFlags, int shaderNum)
{
    static const char *const snowOk[] = {"snow", "foxhole", "crater", "trench"};
    static const char *const dirtOk[] = {"dirt", "earth", "furroughed", "mud", "foxhole", "grass", "grnd", "field",
                                         "trench", "shellhole", "battaglia"};
    baseshader_t *bs;
    char          name[MAX_QPATH];
    int           i;
    int           type;

    type = surfaceFlags & MASK_SURF_TYPE;
    if (type != SURF_SNOW && type != SURF_MUD && type != SURF_DIRT) {
        return -1;
    }
    if (shaderNum < 0) {
        return -1;
    }
    bs = cgi.GetShader(shaderNum);
    if (!bs || !bs->shader[0]) {
        return -1;
    }
    Q_strncpyz(name, bs->shader, sizeof(name));
    Q_strlwr(name);

    // the footstep-only clip brushes maps lay over snow/mud/dirt (m2l1 walks on common/snowclip everywhere); the
    // print is projected onto the visible surface under/around it by CG_FootprintLay
    if (strstr(name, "snowclip") || strstr(name, "mudclip") || strstr(name, "dirtclip")) {
        return type == SURF_SNOW ? FP_SNOW : (type == SURF_MUD ? FP_MUD : FP_DIRT);
    }

    for (i = 0; i < (int)ARRAY_LEN(s_fpDeny); i++) {
        if (strstr(name, s_fpDeny[i])) {
            return -1;
        }
    }
    if (type == SURF_MUD) {
        return FP_MUD;
    }
    if (type == SURF_SNOW) {
        for (i = 0; i < (int)ARRAY_LEN(snowOk); i++) {
            if (strstr(name, snowOk[i])) {
                return FP_SNOW;
            }
        }
        return -1;
    }
    for (i = 0; i < (int)ARRAY_LEN(dirtOk); i++) {
        if (strstr(name, dirtOk[i])) {
            return FP_DIRT;
        }
    }
    return -1;
}

/*
qtrue when the trace hit a clip brush (common/snowclip, mudclip, dirtclip...). Maps lay these invisible, footstep-
flagged brushes over the real ground (m2l1 walks on snowclip everywhere), so the print belongs on what is under it.
*/
static qboolean CG_FootprintOnClip(const trace_t *tr)
{
    baseshader_t *bs;
    char          name[MAX_QPATH];

    if (tr->shaderNum < 0) {
        return qfalse;
    }
    bs = cgi.GetShader(tr->shaderNum);
    if (!bs) {
        return qfalse;
    }
    Q_strncpyz(name, bs->shader, sizeof(name));
    Q_strlwr(name);
    return strstr(name, "clip") ? qtrue : qfalse;
}

/*
A world trace that passes through clip brushes: a trace ignores the brush it starts inside, so on a clip hit it
restarts half a unit into it and goes on to the visible ground under it. No ground under the clip = fraction 1.
*/
static void CG_FootprintTrace(trace_t *tr, const vec3_t start, const vec3_t end)
{
    vec3_t from, dir;
    int    i;

    VectorCopy(start, from);
    VectorSubtract(end, start, dir);
    VectorNormalize(dir);
    for (i = 0; i < 4; i++) {
        cgi.CM_BoxTrace(tr, from, end, vec3_origin, vec3_origin, 0, CONTENTS_SOLID, qfalse);
        if (i > 0 && !tr->allsolid) {
            tr->startsolid = qfalse;   // started inside the clip on purpose
        }
        if (tr->allsolid || tr->startsolid || tr->fraction >= 1.0f || !CG_FootprintOnClip(tr)) {
            return;
        }
        VectorMA(tr->endpos, 0.5f, dir, from);
    }
    tr->fraction = 1.0f;   // clip on clip on clip: give up
}

/*
Lay one print. ground = a point on the surface, normal = its plane, fwd = the foot's forward (any length, any
pitch), left = mirror the texture, axis = hobnail sole. Returns qfalse when the spot is refused.
*/
static qboolean CG_FootprintLay(const vec3_t ground, const vec3_t normal, const vec3_t fwdIn, int surf, qboolean left,
                                qboolean axis, int entnum, qboolean fromClip)
{
    footprint_t  nfp;
    footprint_t *fp = &nfp;
    vec3_t       fwd, right, center, p, start, end;
    trace_t      tr;
    float        d, jit;
    unsigned int h;
    int          r, c, i, max;

    if (normal[2] < 0.7f) {
        return qfalse;   // steeper than ~45 degrees: no one leaves a flat print there
    }

    // forward projected into the ground plane
    d = DotProduct(fwdIn, normal);
    VectorMA(fwdIn, -d, normal, fwd);
    if (VectorNormalize(fwd) < 0.01f) {
        return qfalse;
    }
    CrossProduct(fwd, normal, right);
    VectorNormalize(right);

    // no two prints identical: +-4 degrees of yaw and +-15 % strength from a hash of time, entity and foot
    h   = (unsigned int)(cg.time * 2654435761u) ^ (unsigned int)((entnum + 7) * 40503u) ^ (left ? 0x9e37u : 0u);
    h  ^= h >> 13;
    h  *= 0x5bd1e995u;
    h  ^= h >> 15;
    jit = ((float)(h & 1023) / 1023.0f) * 2.0f - 1.0f;
    VectorScale(fwd, 1.0f, p);
    VectorMA(p, jit * 0.07f, right, fwd);   // ~4 degrees
    VectorNormalize(fwd);
    CrossProduct(fwd, normal, right);
    VectorNormalize(right);

    VectorMA(ground, FP_FORWARD, fwd, center);

    memset(fp, 0, sizeof(*fp));

    // 1) brushes and curved patches: the same projected-decal clipper as the bullet marks (CG_ImpactMark), so the
    //    print follows the VISIBLE surface - under a snowclip, over a curb, across two faces. World fragments only.
    {
        vec3_t         quad[4], proj, pointBuf[MAX_FP_MARK_POINTS];
        markFragment_t frags[MAX_FP_MARK_FRAGMENTS];
        int            nf, f, v;

        for (i = 0; i < 4; i++) {
            VectorMA(center, FP_LENGTH * ((i == 0 || i == 1) ? 0.5f : -0.5f), fwd, quad[i]);
            VectorMA(quad[i], FP_WIDTH * ((i == 1 || i == 2) ? 0.5f : -0.5f), right, quad[i]);
        }
        VectorScale(normal, -32.0f, proj);
        nf = CG_GetMarkFragments(
            4,
            (const vec3_t *)quad,
            proj,
            (const vec3_t *)pointBuf,
            frags,
            0.25f * (FP_LENGTH * FP_LENGTH + FP_WIDTH * FP_WIDTH)
        );
        float fd[MAX_FP_MARK_FRAGMENTS], ref, best;
        int   nw = 0;

        // each world fragment's mean height off the print plane; the reference is the fragment closest to the
        // print's own height (the visible ground, under a clip too), and anything more than 4 u off it - a lower
        // step, a curb top - is dropped
        ref  = 0.0f;
        best = 1e9f;
        for (f = 0; f < nf; f++) {
            fd[f] = 0.0f;
            if (frags[f].iIndex != 0 || frags[f].numPoints < 3) {
                continue;
            }
            nw++;
            for (v = 0; v < frags[f].numPoints; v++) {
                VectorSubtract(pointBuf[frags[f].firstPoint + v], center, p);
                fd[f] += DotProduct(p, normal);
            }
            fd[f] /= frags[f].numPoints;
            if (fabs(fd[f] - (fromClip ? -4.0f : 0.0f)) < best) {
                best = fabs(fd[f] - (fromClip ? -4.0f : 0.0f));
                ref  = fd[f];
            }
        }
        if (nw > FP_MAXPOLYS) {
            nf = 0;   // a dense curved patch: the traced grid copes better than a print cut into pieces
        }
        for (f = 0; f < nf && fp->numPolys < FP_MAXPOLYS; f++) {
            if (frags[f].iIndex != 0 || frags[f].numPoints < 3) {
                continue;   // terrain (gl1) and brush entities: not here
            }
            if (fabs(fd[f] - ref) > 2.0f * FP_PLANE_TOL) {
                continue;
            }
            fp->numVerts[fp->numPolys] = frags[f].numPoints > FP_MAXPV ? FP_MAXPV : frags[f].numPoints;
            for (v = 0; v < fp->numVerts[fp->numPolys]; v++) {
                float s1, t1;
                VectorCopy(pointBuf[frags[f].firstPoint + v], p);
                VectorCopy(p, fp->xyz[fp->numPolys][v]);
                VectorSubtract(p, center, p);
                s1 = 0.5f + DotProduct(p, right) / FP_WIDTH;
                t1 = 0.5f - DotProduct(p, fwd) / FP_LENGTH;
                fp->st[fp->numPolys][v][0] = left ? 1.0f - s1 : s1;
                fp->st[fp->numPolys][v][1] = t1;
            }
            fp->numPolys++;
        }
    }

    // 2) no brush under it (LOD terrain - gl2's clipper has no terrain path): a 2x3 grid traced onto the collision
    //    surface; dropped if a corner leaves the plane (a step) or lands on a refused surface
    if (!fp->numPolys) {
        vec3_t grid[FP_ROWS * FP_COLS];
        float  gst[FP_ROWS * FP_COLS][2];
        static const int quads[2][4] = {
            {0, 1, 3, 2},
            {2, 3, 5, 4}
        };

        float d0 = 0.0f;

        for (r = 0; r < FP_ROWS; r++) {
            for (c = 0; c < FP_COLS; c++) {
                i = r * FP_COLS + c;
                // row 0 = toe (t 0, the top of the image), row 2 = heel; col 0 = the foot's left side (s 0)
                VectorMA(center, FP_LENGTH * (0.5f - 0.5f * r), fwd, p);
                VectorMA(p, FP_WIDTH * (c ? 0.5f : -0.5f), right, p);
                VectorMA(p, 8.0f, normal, start);
                VectorMA(p, fromClip ? -24.0f : -8.0f, normal, end);
                CG_FootprintTrace(&tr, start, end);
                if (tr.startsolid || tr.allsolid || tr.fraction >= 1.0f) {
                    return qfalse;
                }
                VectorSubtract(tr.endpos, center, p);
                d = DotProduct(p, normal);
                if (!i) {
                    d0 = d;
                } else if (fabs(d - d0) > FP_PLANE_TOL) {
                    return qfalse;   // the corners do not share a plane: a step or a curb
                }
                if (CG_FootprintSurface(tr.surfaceFlags, tr.shaderNum) < 0
                    && (!fromClip || CG_FootprintDenied(tr.shaderNum))) {
                    // a corner on stone, wood, a road... (under a snow/mud clip the ground itself is often unflagged:
                    // then only a refused name stops it)
                    return qfalse;
                }
                VectorMA(tr.endpos, FP_LIFT, normal, grid[i]);
                gst[i][0] = left ? (float)(1 - c) : (float)c;
                gst[i][1] = 0.5f * r;
            }
        }
        for (r = 0; r < 2; r++) {
            fp->numVerts[r] = 4;
            for (c = 0; c < 4; c++) {
                VectorCopy(grid[quads[r][c]], fp->xyz[r][c]);
                fp->st[r][c][0] = gst[quads[r][c]][0];
                fp->st[r][c][1] = gst[quads[r][c]][1];
            }
        }
        fp->numPolys = 2;
    }

    fp->time   = cg.time ? cg.time : 1;
    fp->shader = s_fpShader[surf][axis ? 1 : 0];
    fp->alpha  = s_fpAlpha[surf] * (1.0f - 0.15f * (float)((h >> 10) & 1023) / 1023.0f);
    VectorCopy(center, fp->center);
    VectorMA(center, 4.0f, normal, p);
    fp->leafnum = cgi.CM_PointLeafnum(p);

    // a ring buffer: past the cap the oldest print is overwritten - only now, so a refused spot never costs a print
    max = cg_footprintMax->integer;
    if (max < 16) {
        max = 16;
    } else if (max > FP_MAX) {
        max = FP_MAX;
    }
    if (s_fpNext >= max) {
        s_fpNext = 0;
    }
    s_fp[s_fpNext] = nfp;
    fp             = &s_fp[s_fpNext];
    s_fpNext++;
    s_fpStamped++;

    if (cg_footprintDebug->integer) {
        cgi.Printf(
            "^~^~^ FOOTPRINT add n=%d ent=%d foot=%c surf=%s sole=%s at %.0f %.0f %.0f polys %d\n",
            s_fpStamped,
            entnum,
            left ? 'L' : 'R',
            s_fpSurfName[surf],
            axis ? "hobnail" : "rubber",
            center[0],
            center[1],
            center[2],
            fp->numPolys
        );
    }
    return qtrue;
}

/*
Called from CG_Footstep with the ground trace it already made.
*/
void CG_FootprintStep(const char *szTagName, centity_t *ent, refEntity_t *pREnt, int iRunning, const trace_t *stepTrace)
{
    const trace_t  *trace = stepTrace;
    footprintEnt_t *fe;
    vec3_t          bmins, bmaxs, delta;
    int             surf, foot, num;
    qboolean        left;

    if (!cg_footprints || !cg_footprints->integer || s_fpOmaha) {
        return;
    }
    if (!szTagName || !ent || !pREnt || !trace || iRunning == -1) {
        return;   // the jump-off footstep (no tag) and ladders
    }
    if (trace->entityNum != ENTITYNUM_WORLD || trace->startsolid) {
        return;   // never on movers, trucks or another soldier
    }
    if (ent->currentState.eFlags & EF_DEAD) {
        return;
    }
    num = ent->currentState.number;
    if (num < 0 || num >= MAX_GENTITIES) {
        return;
    }

    // prone: the prone hull is 20 tall (bg_pmove.cpp), crouch 54-60, standing 94; the local player also has the flag
    if (ent->currentState.solid && ent->currentState.solid != SOLID_BMODEL) {
        IntegerToBoundingBox(ent->currentState.solid, bmins, bmaxs);
        if (bmaxs[2] < 40.0f) {
            return;
        }
    }
    if (cg.snap && num == cg.snap->ps.clientNum && (cg.predicted_player_state.pm_flags & PMF_VIEW_PRONE)) {
        return;
    }

    surf = CG_FootprintSurface(trace->surfaceFlags, trace->shaderNum);
    if (surf < 0) {
        if (cg_footprintDebug->integer > 1 && trace->shaderNum >= 0) {
            // QA: which ground was refused (flag + shader), to tune the allowlist
            baseshader_t *bs = cgi.GetShader(trace->shaderNum);
            cgi.Printf(
                "^~^~^ FOOTPRINT skip ent=%d flags=%x shader=%s\n",
                num,
                trace->surfaceFlags & MASK_SURF_TYPE,
                bs ? bs->shader : "?"
            );
        }
        return;
    }
    VectorCopy(trace->endpos, delta);
    delta[2] += 2.0f;
    if (CG_PointContents(delta, -1) & MASK_WATER) {
        return;
    }

    left = (strstr(szTagName, " L ") || strstr(szTagName, " l ")) ? qtrue : qfalse;
    foot = left ? 0 : 1;

    // the same foot landing again within 6 units (turning on the spot, an anim re-fire) does not stack prints
    fe = &s_fpEnt[num];
    VectorSubtract(trace->endpos, fe->pos[foot], delta);
    if (fe->time[foot] && cg.time >= fe->time[foot] && cg.time - fe->time[foot] < 2000
        && VectorLengthSquared(delta) < 36.0f) {
        return;
    }

    if (CG_FootprintLay(
            trace->endpos,
            trace->plane.normal,
            pREnt->axis[0],
            surf,
            left,
            (ent->currentState.eFlags & EF_AXIS) ? qtrue : qfalse,
            num,
            CG_FootprintOnClip(trace)
        )) {
        fe->time[foot] = cg.time;
        VectorCopy(trace->endpos, fe->pos[foot]);
    }
}

/*
Per frame, after the marks. PVS, distance and frustum culled; two quads per print.
*/
void CG_AddFootprints(void)
{
    footprint_t *fp;
    polyVert_t   verts[FP_MAXPV];
    vec3_t       delta;
    int          i, j, k, viewleaf, life, fadeTime, age, max;
    float        fade;

    if (!cg_footprints || !cg_footprints->integer || s_fpOmaha) {
        return;
    }

    life = (int)(cg_footprintTime->value * 1000.0f);
    if (life < 2000) {
        life = 2000;
    } else if (life > 600000) {
        life = 600000;
    }
    fadeTime = life / 3;

    max = cg_footprintMax->integer;
    if (max < 16) {
        max = 16;
    } else if (max > FP_MAX) {
        max = FP_MAX;
    }

    viewleaf = cgi.CM_PointLeafnum(cg.refdef.vieworg);

    for (i = 0, fp = s_fp; i < FP_MAX; i++, fp++) {
        if (!fp->time) {
            continue;
        }
        age = cg.time - fp->time;
        if (age < 0 || age >= life || i >= max) {
            fp->time = 0;   // expired, time went backwards, or the cap was lowered
            continue;
        }
        VectorSubtract(fp->center, cg.refdef.vieworg, delta);
        if (VectorLengthSquared(delta) > FP_DRAWDIST * FP_DRAWDIST) {
            continue;
        }
        if (!cgi.CM_LeafInPVS(viewleaf, fp->leafnum)) {
            continue;
        }
        if (CG_FrustumCullSphere(fp->center, FP_LENGTH * 0.6f)) {
            continue;
        }

        fade = fp->alpha;
        if (age > life - fadeTime) {
            fade *= (float)(life - age) / (float)fadeTime;
        }

        for (k = 0; k < fp->numPolys; k++) {
            for (j = 0; j < fp->numVerts[k]; j++) {
                VectorCopy(fp->xyz[k][j], verts[j].xyz);
                verts[j].st[0]       = fp->st[k][j][0];
                verts[j].st[1]       = fp->st[k][j][1];
                // multiplicative shader (dst * (1 - src)): the vertex colour is the print's STRENGTH, so it darkens
                // the ground by the same fraction in sun and at night, and the fade is just this scale going to 0
                verts[j].modulate[0] = (byte)(fade * 255.0f);
                verts[j].modulate[1] = (byte)(fade * 255.0f);
                verts[j].modulate[2] = (byte)(fade * 255.0f);
                verts[j].modulate[3] = 255;
            }
            cgi.R_AddPolyToScene(fp->shader, fp->numVerts[k], verts, 0);
        }
    }
}

/*
footprintfill [n] - test/QA helper: stamps up to n prints (default 200) in ten parallel L/R trails ahead of the view,
through the same surface rules. Visual only, this client only.
*/
void CG_FootprintFill_f(void)
{
    vec3_t  fwd, right, base, p, start, end;
    trace_t tr;
    int     n, k, made, surf;

    n = 200;
    if (cgi.Argc() > 1) {
        n = atoi(cgi.Argv(1));
    }
    if (n < 1 || n > FP_MAX) {
        n = FP_MAX;
    }

    AngleVectors(cg.refdefViewAngles, fwd, NULL, NULL);
    fwd[2] = 0;
    if (VectorNormalize(fwd) < 0.01f) {
        return;
    }
    VectorSet(right, fwd[1], -fwd[0], 0);
    VectorCopy(cg.refdef.vieworg, base);

    made = 0;
    for (k = 0; k < n; k++) {
        int   trail = k % 10;
        int   step  = k / 10;
        float side  = (step & 1) ? 4.0f : -4.0f;

        VectorMA(base, 48.0f + step * 24.0f, fwd, p);
        VectorMA(p, (trail - 4.5f) * 28.0f + side, right, p);
        VectorCopy(p, start);
        start[2] += 8.0f;
        VectorCopy(p, end);
        end[2] -= 400.0f;
        cgi.CM_BoxTrace(&tr, start, end, vec3_origin, vec3_origin, 0, CONTENTS_SOLID, qfalse);   // clips count, like a foot
        if (tr.fraction >= 1.0f || tr.startsolid) {
            continue;
        }
        surf = CG_FootprintSurface(tr.surfaceFlags, tr.shaderNum);
        if (surf < 0) {
            continue;
        }
        if (CG_FootprintLay(tr.endpos, tr.plane.normal, fwd, surf, (step & 1) ? qfalse : qtrue, trail & 1, -1,
                            CG_FootprintOnClip(&tr))) {
            made++;
        }
    }
    cgi.Printf("^~^~^ FOOTPRINT fill asked=%d made=%d\n", n, made);
}
