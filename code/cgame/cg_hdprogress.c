/*
HZM coop [user 2026-10-18] HD HOLD-PROGRESS FAMILY - one drawing path for every timed / hold-to-use indicator.

WHY. The 2D path has no anti-aliasing, so the old DBNO bleed-out ring (a necklace of whole-pixel R_DrawBox squares) and the
scripted heal / placement bars (flat huddraw rectangles) stair-stepped and looked low-definition at 1080p and worse at 4K.

HOW. Everything here is real geometry sent through cgi.R_DrawTrianglePic (the renderer's own triangle blit, both gl1 and
gl2): arcs and straight strokes are strips of triangles at ANY angle, the ends are half-disc fans (round caps), and every
edge carries a feather ~HD_FEATHER screen pixels wide whose alpha comes from ONE texture, textures/hud/coop_hd_ramp.tga
(1024 px smoothstep ramp, white RGB: a tint never leaves a dark or light fringe; docs/tools/gen_hd_ramp.py, shipped through
the fixes pak). Feather is in SCREEN pixels, so an edge is exactly as crisp at 4K as at 720p. Shapes never overlap
themselves (body, caps and feathers tile edge to edge), so a translucent colour has the same alpha everywhere.

FAMILIES. Colour is chosen by the shader NAME a script gives a huddraw quad, so a script author states only WHAT the action
is and the look comes from here (one palette, one place):
    textures/hud/coop_hold_track       the dark channel + halo (any action)
    textures/hud/coop_hold_revive      reviving a teammate / self-revive / being revived   coral red
    textures/hud/coop_hold_heal        medkit on self or a teammate                         soft green
    textures/hud/coop_hold_place       placing sandbags / a deployable                      amber
    textures/hud/coop_hold_use         picking something up, using a station                cream
    textures/hud/coop_hold_objective   plant / defuse / sabotage / capture                  pale blue
A huddraw element with one of these names is drawn by CG_HdHudElement (called from CG_HudDrawElements); its ihuddraw_color
is ignored (alpha and rect are honoured). The fill's width is eased so a 20 Hz script update looks continuous.
The DBNO ring (cg_coopbleed.c) and the weapon pickup hold bar (cg_coopfeed.c) call CG_HdRing / CG_HdBar directly.
*/
#include "cg_local.h"

#define HD_FEATHER 1.4f      // screen pixels
#define HD_S0      0.0005f   // ramp texcoords, kept off the clamped texture edge
#define HD_S1      0.9995f

static qhandle_t s_ramp      = 0;
static int       s_rampTried = 0;

static const vec3_t s_famRgb[HD_FAM_COUNT] = {
    {0.075f, 0.070f, 0.065f},   // track  #131211
    {0.925f, 0.357f, 0.263f},   // revive #ECB... coral red  #EC5B43
    {0.549f, 0.780f, 0.451f},   // heal   #8CC773 (the item readout's pickup green)
    {1.000f, 0.659f, 0.200f},   // place  #FFA833 (the readout's alert amber)
    {0.941f, 0.910f, 0.839f},   // use    #F0E8D6 (the rank / readout cream)
    {0.549f, 0.722f, 0.902f},   // objective #8CB8E6 (the readout's hint blue)
};

qhandle_t CG_HdRampShader(void)
{
    if (!s_rampTried) {
        s_rampTried = 1;
        s_ramp      = cgi.R_RegisterShaderNoMip("textures/hud/coop_hd_ramp");
    }
    return s_ramp;
}

void CG_HdFamilyColor(int fam, float alpha, vec4_t out)
{
    if (fam < 0 || fam >= HD_FAM_COUNT) {
        fam = HD_FAM_USE;
    }
    out[0] = s_famRgb[fam][0];
    out[1] = s_famRgb[fam][1];
    out[2] = s_famRgb[fam][2];
    out[3] = alpha;
}

/* -1 = not a hold-progress shader name */
int CG_HdFamilyFromShader(const char *name)
{
    static const char *const names[HD_FAM_COUNT] = {
        "textures/hud/coop_hold_track",  "textures/hud/coop_hold_revive", "textures/hud/coop_hold_heal",
        "textures/hud/coop_hold_place",  "textures/hud/coop_hold_use",    "textures/hud/coop_hold_objective",
    };
    int i;

    if (!name || name[0] != 't' || strncmp(name, "textures/hud/coop_hold_", 23) != 0) {
        return -1;
    }
    for (i = 0; i < HD_FAM_COUNT; i++) {
        size_t n = strlen(names[i]);
        // the script may or may not append .tga
        if (!Q_stricmpn(name, names[i], (int)n) && (name[n] == 0 || !Q_stricmp(name + n, ".tga"))) {
            return i;
        }
    }
    return -1;
}

/* ---- triangle plumbing ------------------------------------------------------------------------------------------- */

static void HD_Tri(float x0, float y0, float s0, float x1, float y1, float s1, float x2, float y2, float s2)
{
    vec2_t p[3], t[3];
    float  cross = (x1 - x0) * (y2 - y0) - (y1 - y0) * (x2 - x0);

    /*
    The renderer culls back faces on this path (measured: a clockwise-on-screen triangle draws, the opposite vanishes - the
    first build lost every arc and every cap that way). Winding is therefore normalised HERE, once, so no shape above has to
    care which way round it walks its vertices.
    */
    if (cross < 0.0f) {
        float tx = x1, ty = y1, ts = s1;
        x1 = x2; y1 = y2; s1 = s2;
        x2 = tx; y2 = ty; s2 = ts;
    }
    p[0][0] = x0; p[0][1] = y0; t[0][0] = s0; t[0][1] = 0.5f;
    p[1][0] = x1; p[1][1] = y1; t[1][0] = s1; t[1][1] = 0.5f;
    p[2][0] = x2; p[2][1] = y2; t[2][0] = s2; t[2][1] = 0.5f;
    cgi.R_DrawTrianglePic((const vec2_t *)p, (const vec2_t *)t, s_ramp);
}

/* quad a-b-c-d (in order round the outline) */
static void HD_Quad(float ax, float ay, float as, float bx, float by, float bs, float cx, float cy, float cs, float dx,
                    float dy, float ds)
{
    HD_Tri(ax, ay, as, bx, by, bs, cx, cy, cs);
    HD_Tri(ax, ay, as, cx, cy, cs, dx, dy, ds);
}

static float HD_Feather(float th)
{
    float f = HD_FEATHER;
    if (f > th * 0.9f) {
        f = th * 0.9f;   // a stroke thinner than its own feather still has a solid core
    }
    return f;
}

/*
Half-disc cap. C = centre of the stroke's end, n = unit direction the cap points (away from the body), b = unit
direction across the stroke, R = half the stroke thickness, f = feather width. The cap spans the same radii as the body
(R - f/2 solid, R + f/2 gone), so cap and body meet along one shared edge.
*/
static void HD_Cap(float cx, float cy, float nx, float ny, float bx, float by, float R, float f)
{
    int   n = (int)ceilf(R * 0.9f) + 8;
    int   i;
    float r1 = R - f * 0.5f, r2 = R + f * 0.5f;
    float px1 = 0, py1 = 0, px2 = 0, py2 = 0;

    if (n > 28) {
        n = 28;
    }
    for (i = 0; i <= n; i++) {
        float phi = -(float)M_PI * 0.5f + (float)M_PI * (float)i / (float)n;
        float c = cosf(phi), s = sinf(phi);
        float dx = nx * c + bx * s, dy = ny * c + by * s;
        float x1 = cx + dx * r1, y1 = cy + dy * r1, x2 = cx + dx * r2, y2 = cy + dy * r2;

        if (i > 0) {
            HD_Tri(cx, cy, HD_S1, px1, py1, HD_S1, x1, y1, HD_S1);
            HD_Quad(px1, py1, HD_S1, px2, py2, HD_S0, x2, y2, HD_S0, x1, y1, HD_S1);
        }
        px1 = x1; py1 = y1; px2 = x2; py2 = y2;
    }
}

/* ---- pill (horizontal bar with round ends) ----------------------------------------------------------------------- */

/* outer bounds x,y,w,h; col is set by the caller's R_SetColor */
static void HD_PillGeom(float x, float y, float w, float h)
{
    float R, f, xl, xr, cy, y0, y1, y2, y3;

    if (h < 1.0f || w < 1.0f) {
        return;
    }
    if (w < h) {      // narrower than tall: a disc of the width
        float d = w;
        y += (h - d) * 0.5f;
        h = d;
    }
    R  = h * 0.5f;
    f  = HD_Feather(h);
    xl = x + R;
    xr = x + w - R;
    cy = y + R;
    y0 = y - f * 0.5f; y1 = y + f * 0.5f; y2 = y + h - f * 0.5f; y3 = y + h + f * 0.5f;

    if (xr > xl + 0.01f) {
        HD_Quad(xl, y0, HD_S0, xr, y0, HD_S0, xr, y1, HD_S1, xl, y1, HD_S1);
        HD_Quad(xl, y1, HD_S1, xr, y1, HD_S1, xr, y2, HD_S1, xl, y2, HD_S1);
        HD_Quad(xl, y2, HD_S1, xr, y2, HD_S1, xr, y3, HD_S0, xl, y3, HD_S0);
    }
    HD_Cap(xl, cy, -1.0f, 0.0f, 0.0f, 1.0f, R, f);
    HD_Cap(xr, cy, 1.0f, 0.0f, 0.0f, -1.0f, R, f);
}

void CG_HdPill(float x, float y, float w, float h, const vec4_t col)
{
    if (!CG_HdRampShader() || col[3] <= 0.003f) {
        return;
    }
    cgi.R_SetColor(col);
    HD_PillGeom(x, y, w, h);
    cgi.R_SetColor(NULL);
}

/* ---- arc stroke -------------------------------------------------------------------------------------------------- */

/* angles in radians, clockwise from 12 o'clock; rm = centre-line radius; th = stroke thickness */
static void HD_ArcGeom(float cx, float cy, float rm, float th, float a0, float a1, int caps)
{
    float R = th * 0.5f, f = HD_Feather(th);
    float rr[4], ss[4] = {HD_S0, HD_S1, HD_S1, HD_S0};
    float span, step, sa0 = a0, sa1 = a1;
    int   n, i, k;
    float inset = 0.0f;

    if (rm < 1.0f || th < 0.5f || a1 <= a0) {
        return;
    }
    rr[0] = rm - R - f * 0.5f; rr[1] = rm - R + f * 0.5f; rr[2] = rm + R - f * 0.5f; rr[3] = rm + R + f * 0.5f;
    if (rr[0] < 0.0f) {
        rr[0] = 0.0f;
    }

    if (caps) {
        // the caps are part of the nominal extent: shorten the body by R at each end
        inset = R / rm;
        if (a1 - a0 <= 2.0f * inset) {   // too short for a body: one disc at the middle
            float am = 0.5f * (a0 + a1), px = cx + sinf(am) * rm, py = cy - cosf(am) * rm;
            float tx = cosf(am), ty = sinf(am), bx = sinf(am), by = -cosf(am);
            HD_Cap(px, py, tx, ty, bx, by, R, f);
            HD_Cap(px, py, -tx, -ty, -bx, -by, R, f);
            return;
        }
        sa0 = a0 + inset;
        sa1 = a1 - inset;
    }

    span = sa1 - sa0;
    // chord error under ~0.06 px
    step = 2.0f * acosf(1.0f - 0.06f / (rm + R));
    if (step < 0.01f) {
        step = 0.01f;
    }
    n = (int)ceilf(span / step);
    if (n < 1) {
        n = 1;
    }
    for (i = 0; i < n; i++) {
        float aa = sa0 + span * (float)i / (float)n, ab = sa0 + span * (float)(i + 1) / (float)n;
        float sx0 = sinf(aa), cy0 = cosf(aa), sx1 = sinf(ab), cy1 = cosf(ab);

        for (k = 0; k < 3; k++) {
            HD_Quad(cx + sx0 * rr[k], cy - cy0 * rr[k], ss[k], cx + sx1 * rr[k], cy - cy1 * rr[k], ss[k],
                    cx + sx1 * rr[k + 1], cy - cy1 * rr[k + 1], ss[k + 1], cx + sx0 * rr[k + 1], cy - cy0 * rr[k + 1],
                    ss[k + 1]);
        }
    }
    if (caps) {
        float tx, ty, bx, by;

        tx = cosf(sa0); ty = sinf(sa0); bx = sinf(sa0); by = -cosf(sa0);
        HD_Cap(cx + bx * rm, cy + by * rm, -tx, -ty, bx, by, R, f);   // start: points back along the arc
        tx = cosf(sa1); ty = sinf(sa1); bx = sinf(sa1); by = -cosf(sa1);
        HD_Cap(cx + bx * rm, cy + by * rm, tx, ty, -bx, -by, R, f);
    }
}

/* ---- composite shapes -------------------------------------------------------------------------------------------- */

/*
A full ring, drained clockwise like the old one: the filled arc runs from (1 - rem) of a turn round to 12 o'clock, the
empty part opens at the top. haloA is the dark outline (a thin white arc vanishes on snow without it).
*/
void CG_HdRing(float cx, float cy, float r, float th, float rem, const vec4_t fill, float trackA, float haloA)
{
    vec4_t col;
    float  two = 2.0f * (float)M_PI;

    if (!CG_HdRampShader() || r < 2.0f || th < 1.0f) {
        return;
    }
    if (rem > 1.0f) {
        rem = 1.0f;
    }

    // dark halo, a pixel and a half proud of the ring on each side
    if (haloA > 0.003f) {
        col[0] = col[1] = col[2] = 0.0f;
        col[3] = haloA;
        cgi.R_SetColor(col);
        HD_ArcGeom(cx, cy, r, th + 3.0f, 0.0f, two, 0);
    }
    if (trackA > 0.003f) {
        col[0] = s_famRgb[HD_FAM_TRACK][0];
        col[1] = s_famRgb[HD_FAM_TRACK][1];
        col[2] = s_famRgb[HD_FAM_TRACK][2];
        col[3] = trackA;
        cgi.R_SetColor(col);
        HD_ArcGeom(cx, cy, r, th, 0.0f, two, 0);
    }
    if (rem > 0.002f && fill[3] > 0.003f) {
        cgi.R_SetColor(fill);
        if (rem > 0.985f) {
            HD_ArcGeom(cx, cy, r, th, 0.0f, two, 0);
        } else {
            HD_ArcGeom(cx, cy, r, th, (1.0f - rem) * two, two, 1);
        }
    }
    cgi.R_SetColor(NULL);
}

/* Track + fill bar. frac 0..1 across the track's width; the channel is inset so the dark rim shows round the fill. */
void CG_HdBar(float x, float y, float w, float h, float frac, int fam, float alpha)
{
    vec4_t col;
    float  inset, fx, fy, fw, fh;

    if (!CG_HdRampShader() || alpha <= 0.003f || w < 2.0f || h < 2.0f) {
        return;
    }
    if (frac < 0.0f) {
        frac = 0.0f;
    } else if (frac > 1.0f) {
        frac = 1.0f;
    }

    col[0] = col[1] = col[2] = 0.0f;
    col[3] = 0.55f * alpha;
    cgi.R_SetColor(col);
    HD_PillGeom(x - 1.5f, y - 1.5f, w + 3.0f, h + 3.0f);

    col[0] = s_famRgb[HD_FAM_TRACK][0];
    col[1] = s_famRgb[HD_FAM_TRACK][1];
    col[2] = s_famRgb[HD_FAM_TRACK][2];
    col[3] = 0.88f * alpha;
    cgi.R_SetColor(col);
    HD_PillGeom(x, y, w, h);

    inset = h * 0.16f;
    if (inset < 1.0f) {
        inset = 1.0f;
    }
    fx = x + inset;
    fy = y + inset;
    fh = h - 2.0f * inset;
    fw = (w - 2.0f * inset) * frac;
    if (fw >= 1.0f && fh >= 1.0f) {
        CG_HdFamilyColor(fam, alpha, col);
        cgi.R_SetColor(col);
        HD_PillGeom(fx, fy, fw, fh);
        // a soft sheen on the top third (inside the fill only, so the silhouette is untouched)
        if (fh >= 5.0f && fw > fh) {
            col[0] = col[1] = col[2] = 1.0f;
            col[3] = 0.16f * alpha;
            cgi.R_SetColor(col);
            HD_PillGeom(fx + fh * 0.30f, fy + fh * 0.14f, fw - fh * 0.60f, fh * 0.30f);
        }
    }
    cgi.R_SetColor(NULL);
}

/* ---- huddraw hook ------------------------------------------------------------------------------------------------ */

#define HD_MAXEL 256
static float s_easeW[HD_MAXEL];
static int   s_easeAt[HD_MAXEL];

void CG_HdHoldReset(int i)
{
    if (i >= 0 && i < HD_MAXEL) {
        s_easeW[i] = -1.0f;
    }
}

/* One scripted element. x,y,w,h in screen pixels (after align / virtual-screen scaling), alpha already faded. */
void CG_HdHudElement(int i, float x, float y, float w, float h, float alpha, int fam)
{
    if (!CG_HdRampShader() || alpha <= 0.003f) {
        return;
    }
    if (fam == HD_FAM_TRACK) {
        // the track element carries the whole bar: halo + channel, no fill (the fill is its own element)
        vec4_t col;

        col[0] = col[1] = col[2] = 0.0f;
        col[3] = 0.55f * alpha;
        cgi.R_SetColor(col);
        HD_PillGeom(x - 1.5f, y - 1.5f, w + 3.0f, h + 3.0f);
        col[0] = s_famRgb[HD_FAM_TRACK][0];
        col[1] = s_famRgb[HD_FAM_TRACK][1];
        col[2] = s_famRgb[HD_FAM_TRACK][2];
        col[3] = 0.88f * alpha;
        cgi.R_SetColor(col);
        HD_PillGeom(x, y, w, h);
        cgi.R_SetColor(NULL);
        return;
    }
    // a fill: eased on the way up (a 20 Hz script update reads as continuous), instant on the way down / a restart
    if (i >= 0 && i < HD_MAXEL) {
        float dt = (s_easeAt[i] && cg.time > s_easeAt[i]) ? (float)(cg.time - s_easeAt[i]) * 0.001f : 0.0f;

        s_easeAt[i] = cg.time ? cg.time : 1;
        if (s_easeW[i] < 0.0f || w < s_easeW[i] - 0.5f || dt > 0.25f) {
            s_easeW[i] = w;
        } else {
            float k = dt * 24.0f;
            s_easeW[i] += (w - s_easeW[i]) * (k > 1.0f ? 1.0f : k);
        }
        w = s_easeW[i];
    }
    {
        // the script draws the fill on the SAME rect as the track: inset it so the dark rim shows round it (as CG_HdBar)
        vec4_t col;
        float  inset = h * 0.16f, fx, fy, fw, fh;

        if (inset < 1.0f) {
            inset = 1.0f;
        }
        fx = x + inset;
        fy = y + inset;
        fw = w - 2.0f * inset;
        fh = h - 2.0f * inset;
        if (fw < 1.0f || fh < 1.0f) {
            return;
        }
        CG_HdFamilyColor(fam, alpha, col);
        cgi.R_SetColor(col);
        HD_PillGeom(fx, fy, fw, fh);
        if (fh >= 5.0f && fw > fh) {
            col[0] = col[1] = col[2] = 1.0f;
            col[3] = 0.16f * alpha;
            cgi.R_SetColor(col);
            HD_PillGeom(fx + fh * 0.30f, fy + fh * 0.14f, fw - fh * 0.60f, fh * 0.30f);
        }
        cgi.R_SetColor(NULL);
    }
}
