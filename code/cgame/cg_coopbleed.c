/*
===========================================================================
HZM coop [user 2026-10-05] BLEED-OUT PRESENTATION - replaces the "BLEEDING OUT: Ns" text (dbno.scr slot 27).

Approved design: minimal and mostly wordless.
  1. RING  - a thin ring around the crosshair drains CLOCKWISE as you bleed out, white -> red, pulsing faster in
             the last 10 s. No number unless coop_bleedNumber 1 (a small number under the ring).
  2. SENSES - the tunnel closes and the colour drains as time runs low. Not a new effect: it drives the two
             post-FX passes that already exist (r_ppHealthFrac -> low-health red/beat, r_ppSuppress -> the
             suppression tunnel: vignette radius tightens, edges darken, desaturation rises), so nothing stacks.
             Published from cg_view.c through CG_CoopBleedPostFx. The heartbeat is the script's DBNO bed.
  3. TEAMMATES - a small medic icon over a downed teammate with a mini ring of HIS remaining time, through walls,
             only within coop_bleedIconRange (1200u).
  4. HARDCORE - tunnel/desaturation/heartbeat only: no ring, no icon (CG_CoopHardcoreActive).
  5. CRAWL PENALTY - moving while downed flares the ring (the server flips a toggle bit) instead of an iprint.

DATA: the player's own entityState.beam_entnum, packed by Player::TickCoopBleed (fgame/player.cpp):
bits 0-7 remaining/total (0..255), bits 8-14 total seconds (0 = not bleeding), bit 15 flare toggle. Entity state
reaches teammates (the server broadcasts a bleeding player) and arrives whole for a late joiner.

STYLE: procedural like the hit marker beside it (cg_drawtools.cpp CG_DrawHitMarker): geometry scales off screen
HEIGHT, thickness is rounded to whole pixels, no texture for the ring. The teammate icon reuses the existing
coop medkit HUD art (textures/hud/coop_medkit_icon.tga).
===========================================================================
*/

#include "cg_local.h"

qboolean CG_CoopAimPoint(float *pX, float *pY); // cg_drawtools.cpp - where the crosshair really is

#define BLEED_FLARE_MS 520

typedef struct {
    int   num;
    float rem;   // remaining fraction 1 -> 0
    int   total; // seconds
    int   flare; // toggle bit
} coopBleedEnt_t;

static int   s_selfSeenAt   = 0;
static float s_selfShown    = 1.0f;
static int   s_selfFlareBit = -1;
static int   s_selfFlareAt  = 0;
static float s_selfAppear   = 0.0f;
static float s_ringPhase    = 0.0f;
static int   s_lastDrawTime = 0;

static qboolean CG_BleedDecode(const entityState_t *es, coopBleedEnt_t *out)
{
    int v = es->beam_entnum;
    int tot;

    if (es->eType != ET_PLAYER || (es->eFlags & EF_DEAD)) {
        return qfalse;
    }
    if (v <= 0 || v == ENTITYNUM_NONE) {
        return qfalse;
    }
    tot = (v >> 8) & 127;
    if (tot <= 0) {
        return qfalse;
    }
    out->num   = es->number;
    out->rem   = (float)(v & 255) / 255.0f;
    out->total = tot;
    out->flare = (v >> 15) & 1;
    return qtrue;
}

/* The local (or followed) player's own bleed-out, if any. */
static qboolean CG_BleedSelf(coopBleedEnt_t *out)
{
    int i;

    if (!cg.snap) {
        return qfalse;
    }
    if (cg.snap->ps.pm_flags & PMF_INTERMISSION) {
        return qfalse;
    }
    for (i = 0; i < cg.snap->numEntities; i++) {
        const entityState_t *es = &cg.snap->entities[i];
        if (es->number == cg.snap->ps.clientNum) {
            return CG_BleedDecode(es, out);
        }
    }
    return qfalse;
}

/*
Called from cg_view.c where the two post-FX signals are published. pHealthFrac: only touched while bleeding
(the caller has already forced its DBNO value); pSuppress: raised to the bleed tunnel, never lowered.
*/
void CG_CoopBleedPostFx(float *pHealthFrac, float *pSuppress)
{
    coopBleedEnt_t b;
    float          prog, t;

    if (!CG_BleedSelf(&b)) {
        return;
    }
    prog = 1.0f - b.rem;

    if (pHealthFrac) {
        // r_ppLowHealthStart 0.5: 0.30 -> the red haze starts at ~60% and reaches 100% as the timer ends.
        // (The old fixed 0.02 sat at ~97% from the first second, so the screen had nowhere left to go.)
        *pHealthFrac = 0.30f * b.rem;
    }
    if (pSuppress) {
        // the suppression tunnel: gentle at first, steep at the end (its shader is already steeper at the top)
        t = 0.10f + 0.80f * (float)pow(prog, 1.6);
        if (t > 0.92f) {
            t = 0.92f;
        }
        if (t > *pSuppress) {
            *pSuppress = t;
        }
    }
}

/* world -> screen pixels; qfalse when behind the eye */
static qboolean CG_BleedProject(const vec3_t p, float *sx, float *sy)
{
    vec3_t d;
    float  x, y, z, tx, ty;

    VectorSubtract(p, cg.refdef.vieworg, d);
    z = DotProduct(d, cg.refdef.viewaxis[0]);
    if (z < 8.0f) {
        return qfalse;
    }
    x  = DotProduct(d, cg.refdef.viewaxis[1]);
    y  = DotProduct(d, cg.refdef.viewaxis[2]);
    tx = (float)tan(DEG2RAD((cg.refdef.fov_x > 1.0f ? cg.refdef.fov_x : 90.0f) * 0.5f));
    ty = (float)tan(DEG2RAD((cg.refdef.fov_y > 1.0f ? cg.refdef.fov_y : 73.0f) * 0.5f));
    *sx = cg.refdef.x + cg.refdef.width * 0.5f * (1.0f - x / (z * tx));
    *sy = cg.refdef.y + cg.refdef.height * 0.5f * (1.0f - y / (z * ty));
    return qtrue;
}

/* remaining-time colour: white -> red as the bleed-out runs */
static void CG_BleedColor(float rem, float a, vec4_t col)
{
    float k = 1.0f - rem;
    col[0]  = 1.0f;
    col[1]  = 1.0f - 0.80f * k;
    col[2]  = 1.0f - 0.85f * k;
    col[3]  = a;
}

/*
A ring of whole-pixel boxes stepped round the circle, the hit marker's recipe. The track (full circle, dim) is
drawn first; the fill is the arc from (1 - rem) of a turn clockwise from 12 o'clock round to 12 o'clock, so the
empty part opens at the top and sweeps clockwise.
*/
static void CG_BleedRing(float cx, float cy, float r, float th, float rem, const vec4_t fill, float trackA)
{
    float step, a0, a, end;
    vec4_t col;

    if (r < 2.0f || th < 1.0f) {
        return;
    }
    step = (th * 0.55f) / r;
    if (step < 0.01f) {
        step = 0.01f;
    }

    if (trackA > 0.003f) {
        col[0] = col[1] = col[2] = 0.10f;
        col[3] = trackA;
        cgi.R_SetColor(col);
        for (a = 0.0f; a < 2.0f * (float)M_PI; a += step) {
            cgi.R_DrawBox(cx + (float)sin(a) * r - th * 0.5f, cy - (float)cos(a) * r - th * 0.5f, th, th);
        }
    }

    if (rem > 0.002f && fill[3] > 0.003f) {
        cgi.R_SetColor(fill);
        a0  = (1.0f - rem) * 2.0f * (float)M_PI;
        end = 2.0f * (float)M_PI;
        for (a = a0; a < end; a += step) {
            cgi.R_DrawBox(cx + (float)sin(a) * r - th * 0.5f, cy - (float)cos(a) * r - th * 0.5f, th, th);
        }
        // the leading edge lands exactly on 12 o'clock
        cgi.R_DrawBox(cx - th * 0.5f, cy - r - th * 0.5f, th, th);
    }
    cgi.R_SetColor(NULL);
}

static void CG_BleedDrawSelf(float dt)
{
    static cvar_t *pNum = NULL;
    coopBleedEnt_t b;
    float          cx, cy, r, th, secs, rate, depth, pulse, f, a, rr;
    int            th2, age;
    vec4_t         col;

    if (!pNum) {
        pNum = cgi.Cvar_Get("coop_bleedNumber", "0", CVAR_ARCHIVE);
    }

    if (!CG_BleedSelf(&b)) {
        s_selfAppear   = 0.0f;
        s_selfFlareBit = -1;
        s_selfShown    = 1.0f;
        return;
    }

    // a fresh down: start full and fade in, never flare on the first sight of the toggle bit
    if (s_selfFlareBit < 0 || cg.time - s_selfSeenAt > 1500) {
        s_selfShown    = b.rem;
        s_selfFlareBit = b.flare;
        s_selfAppear   = 0.0f;
    }
    s_selfSeenAt = cg.time;
    if (b.flare != s_selfFlareBit) {
        s_selfFlareBit = b.flare;
        s_selfFlareAt  = cg.time;
    }

    // the field is 1/255 steps at snapshot rate - ease it so the sweep is continuous
    s_selfShown += (b.rem - s_selfShown) * (dt * 10.0f > 1.0f ? 1.0f : dt * 10.0f);
    s_selfAppear += dt / 0.35f;
    if (s_selfAppear > 1.0f) {
        s_selfAppear = 1.0f;
    }

    // pulse: a slow breath, then quick and deep in the last 10 s. Phase is INTEGRATED (TRAPS: view motion),
    // so the rate change never jumps the wave.
    secs = s_selfShown * (float)b.total;
    if (secs <= 10.0f) {
        rate  = 2.4f;
        depth = 0.50f;
    } else {
        rate  = 0.8f;
        depth = 0.14f;
    }
    s_ringPhase += dt * rate * 2.0f * (float)M_PI;
    if (s_ringPhase > 2000.0f * (float)M_PI) {
        s_ringPhase -= 2000.0f * (float)M_PI;
    }
    pulse = 1.0f - depth * (0.5f + 0.5f * (float)sin(s_ringPhase));

    age = cg.time - s_selfFlareAt;
    f   = (s_selfFlareAt && age >= 0 && age < BLEED_FLARE_MS) ? 1.0f - (float)age / (float)BLEED_FLARE_MS : 0.0f;
    f   = f * f;

    if (!CG_CoopAimPoint(&cx, &cy)) {
        cx = cgs.glconfig.vidWidth * 0.5f;
        cy = cgs.glconfig.vidHeight * 0.5f;
    }

    th = cgs.glconfig.vidHeight * 0.0024f;
    if (th < 1.0f) {
        th = 1.0f;
    }
    th  = (float)(int)(th + 0.5f);
    th2 = (int)(th + th * f + 0.5f);   // the flare thickens the stroke by whole pixels
    r   = cgs.glconfig.vidHeight * 0.034f;
    rr  = r * (1.0f + 0.16f * f);

    a = s_selfAppear * (pulse + (1.0f - pulse) * f);
    if (a > 1.0f) {
        a = 1.0f;
    }
    CG_BleedColor(s_selfShown, 0.92f * a, col);
    if (f > 0.0f) {   // flare: hot red-orange
        col[0] = 1.0f;
        col[1] += (0.30f - col[1]) * f;
        col[2] += (0.12f - col[2]) * f;
        col[3] += (1.0f - col[3]) * f;
    }
    CG_BleedRing(cx, cy, rr, (float)th2, s_selfShown, col, 0.45f * s_selfAppear);

    if (pNum->integer) {
        const char *txt = va("%d", (int)ceil(secs - 0.05f));
        float       tw  = cgi.UI_FontStringWidth(cgs.media.hudDrawFont, txt, -1) * cgs.uiHiResScale[0];
        float       tx  = cx - tw * 0.5f;
        float       ty  = cy + r + th * 2.0f + 3.0f * (cgs.glconfig.vidHeight / 480.0f);
        col[3] = 0.85f * s_selfAppear;
        cgi.R_SetColor(col);
        cgi.R_DrawString(
            cgs.media.hudDrawFont, txt, tx / cgs.uiHiResScale[0], ty / cgs.uiHiResScale[1], -1, cgs.uiHiResScale
        );
        cgi.R_SetColor(NULL);
    }
}

static void CG_BleedDrawMates(void)
{
    static cvar_t   *pRange = NULL;
    static qhandle_t hIcon  = 0;
    static int       s_tried = 0;
    int              i, myTeam;
    float            range;

    if (!pRange) {
        pRange = cgi.Cvar_Get("coop_bleedIconRange", "1200", CVAR_ARCHIVE);
    }
    if (!s_tried) {
        s_tried = 1;
        hIcon   = cgi.R_RegisterShaderNoMip("textures/hud/coop_medkit_icon.tga");
    }
    range = pRange->value;
    if (range <= 0.0f) {
        return;
    }
    myTeam = cg.snap->ps.stats[STAT_TEAM];

    for (i = 0; i < cg.snap->numEntities; i++) {
        const entityState_t *es = &cg.snap->entities[i];
        coopBleedEnt_t       b;
        centity_t           *cent;
        vec3_t               p, d;
        float                dist, a, sx, sy, sz, r, th, secs, pulse;
        vec4_t               col;

        if (es->number == cg.snap->ps.clientNum) {
            continue;
        }
        if (!CG_BleedDecode(es, &b)) {
            continue;
        }
        // teammates only (coop is one team; a spectator sees everyone)
        if (myTeam == TEAM_ALLIES && !(es->eFlags & EF_ALLIES)) {
            continue;
        }
        if (myTeam == TEAM_AXIS && !(es->eFlags & EF_AXIS)) {
            continue;
        }

        cent = &cg_entities[es->number];
        VectorCopy(cent->lerpOrigin, p);
        p[2] += 40.0f;
        VectorSubtract(p, cg.refdef.vieworg, d);
        dist = VectorLength(d);
        if (dist > range) {
            continue;
        }
        a = (range - dist) / 250.0f;            // fade out over the last 250u of the range
        if (a > 1.0f) {
            a = 1.0f;
        }
        if (dist < 112.0f) {                    // standing over him: get out of the way of the revive
            a *= (dist - 48.0f) / 64.0f;
        }
        if (a <= 0.01f) {
            continue;
        }
        if (!CG_BleedProject(p, &sx, &sy)) {
            continue;
        }

        sz = cgs.glconfig.vidHeight * 0.022f * (1.0f - 0.25f * dist / range);
        if (sx < -sz || sy < -sz || sx > cgs.glconfig.vidWidth + sz || sy > cgs.glconfig.vidHeight + sz) {
            continue;
        }

        secs  = b.rem * (float)b.total;
        pulse = 1.0f;
        if (secs <= 10.0f) {
            pulse = 0.65f + 0.35f * (0.5f + 0.5f * (float)sin(cg.time * 0.001f * 2.4f * 2.0f * M_PI));
        }

        if (hIcon) {
            col[0] = col[1] = col[2] = 1.0f;
            col[3] = a * 0.95f;
            cgi.R_SetColor(col);
            cgi.R_DrawStretchPic(sx - sz * 0.5f, sy - sz * 0.5f, sz, sz, 0, 0, 1, 1, hIcon);
            cgi.R_SetColor(NULL);
        }

        th = cgs.glconfig.vidHeight * 0.0019f;
        if (th < 1.0f) {
            th = 1.0f;
        }
        th = (float)(int)(th + 0.5f);
        r  = sz * 0.80f;
        CG_BleedColor(b.rem, 0.95f * a * pulse, col);
        CG_BleedRing(sx, sy, r, th, b.rem, col, 0.40f * a);
    }
}

/* CG_Draw2D, inside the crosshair block (so a cinematic hides it with the crosshair). */
void CG_DrawCoopBleed(void)
{
    float dt;

    dt = (s_lastDrawTime && cg.time > s_lastDrawTime) ? (cg.time - s_lastDrawTime) * 0.001f : 0.0f;
    if (dt > 0.1f) {
        dt = 0.1f;
    }
    s_lastDrawTime = cg.time;

    if (!cg.snap || !cg_hud->integer) {
        return;
    }
    if (cg.snap->ps.pm_flags & (PMF_NO_HUD | PMF_INTERMISSION)) {
        return;
    }
    // Hardcore (the coop host rule): the senses still fade and the heart still beats, but no ring and no icon
    if (CG_CoopHardcoreActive()) {
        return;
    }
    CG_BleedDrawSelf(dt);
    CG_BleedDrawMates();
}
