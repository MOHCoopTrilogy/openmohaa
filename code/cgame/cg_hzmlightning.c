/*
===========================================================================
HZM coop [2026-09-27] REALISTIC LIGHTNING - the client half (docs/proposals/lightning_2026-09-27/plan.md 2.3, 2.6).

The server publishes one strike on CS_HZM_LIGHTNING (28): "v1 <seq> <serverTimeMs> <kind> <yawDeg> <distM> <n> <seed>".
Every client expands it from the seed (same numbers everywhere), so all players see the same flash at the same time:

  envelope   the return strokes of a cloud-to-ground flash (log-normal gaps around 60 ms, ~30 ms decay, one optional
             continuing current) or the broad pulses of an intra-cloud flash, sampled at 240 Hz, then the FLASH LIMITER:
             at most 3 pulses in any rolling second, whatever their depth (vet V3; ltlib.limit_flashes is the twin and
             its self-test is the spec). Always on, not a setting.
  modes      cg_hzmLightningMode (ARCHIVE, player preference): 1 normal, 2 reduced flashing (one soft pulse, half
             amplitude, no bolt), 0 off (thunder only). The rollout switch is cg_hzmLightning (-1 = HZM_LIGHTNING_AUTO).
  renderer   r_hzmLtState (hzm_lightning.h 3) every frame while lit, "0" once after. Nothing is written while no
             strike is lit, so with the switch off the renderer never sees anything but its default "0".
  thunder    per LISTENER: delay = distance / 343 m/s (1 unit = 1 inch), played from 400 units along the strike
             direction so it pans, band by distance (near/mid/far aliases in ubersound/coop_audio.scr), level falling
             with distance. Plays whatever the visual mode (Off = thunder only).
  Omaha      nothing at all on m3l1a/m3l1b (HZM_LightRestoreMapProtected).

All state lives in one struct, reset at map load (renderer-reinit contract: no function statics).
===========================================================================
*/

#include "cg_local.h"
#include "../qcommon/alias.h"   // AliasListNode_t (the thunder alias parameters)
#include "../renderercommon/hzm_light_restore.h"
#include "../renderercommon/hzm_lightning.h"

#define LT_RATE        240                 // envelope samples per second
#define LT_SAMPLES     (LT_RATE * 2)       // 2 s covers every flash (the longest is ~1.2 s)
#define LT_MAX_STROKES 6
#define LT_BOLT_PULSES 2                  // [bug-3172] the bolt's own limit: pulses per rolling second

typedef struct {
    int      seq;             // last configstring seq taken (0 = none)
    int      startTime;       // cg.time of the first stroke (server time + the leader lead-in)
    int      kind;            // 'c' or 'i'
    float    yaw, dist;
    int      seed;
    vec3_t   dir;             // world direction to the lit cloud region
    float    env[LT_SAMPLES]; // limited envelope, 0..~1
    float    envBolt[LT_SAMPLES]; // [bug-3172] the bolt's: the same strokes, limited to LT_BOLT_PULSES a second
    qboolean active;
    qboolean published;       // r_hzmLtState currently holds a non-"0" value
    int      thunderAt;       // cg.time to start the thunder (0 = none pending)
    float    thunderVol;
    char     thunderAlias[32];
    vec3_t   thunderDir;
    unsigned rng;
    // FOG-PULSE HOLD (the fallback, plan section 10): the server runs the quick-fix fog pulses alongside every realistic
    // strike so clients that cannot render it (old cgame, gl1) still see a faint, flash-safe pulse and never today's
    // band. A client that renders the strike itself holds the fog colour it had when the strike arrived until the
    // pulses are over, so it never sees them. Distance/cull changes are never held; the latest colour lands at the end.
    qboolean holdOn;
    int      holdUntil;
    vec3_t   heldColor;
    vec3_t   latestColor;
} hzmLt_t;

static hzmLt_t s_lt;
static cvar_t *cg_hzmLightning;
static cvar_t *cg_hzmLightningMode;
static cvar_t *cg_hzmLightningBolts;
static cvar_t *cg_hzmLightningTune;

// ---------------------------------------------------------------- deterministic random (same on every client)
static float LT_Rand(void)
{
    s_lt.rng = s_lt.rng * 1664525u + 1013904223u;
    return (float)((s_lt.rng >> 8) & 0xFFFFFF) / 16777216.0f;
}

static float LT_Gauss(void)
{
    // Irwin-Hall approximation, enough for timing jitter
    return LT_Rand() + LT_Rand() + LT_Rand() + LT_Rand() - 2.0f;
}

// ---------------------------------------------------------------- the envelope
typedef struct {
    float t, amp, tau, hold, rise;
} ltStroke_t;

static float LT_StrokeAt(const ltStroke_t *st, float t)
{
    float dt = t - st->t;
    float k;

    if (dt < 0.0f) {
        return 0.0f;
    }
    if (st->rise > 0.0f && dt < st->rise) {
        k = dt / st->rise;
        return st->amp * k * k * (3.0f - 2.0f * k);
    }
    dt -= st->rise;
    if (dt < st->hold) {
        return st->amp * (1.0f - 0.45f * dt / st->hold);
    }
    return st->amp * (st->hold > 0.0f ? 0.55f : 1.0f) * expf(-(dt - st->hold) / st->tau);
}

/*
The limiter, the C twin of ltlib.limit_flashes: local peaks are counted on the OUTPUT; the 4th peak inside any rolling
second is merged by holding the envelope at min(previous counted peak, this peak) from that peak to this one, so there
is no downward swing between them for WCAG 2.3.1 to count, however bright the scene makes them.
*/
static void LT_Limit(float *e, int n, int perWindow, int window)
{
    int counted[64];
    int nCounted = 0;
    int lastPeak = -1;
    int i, j, k, recent;
    float fl;

    for (i = 0; i < n - 1; i++) {
        // [bug-3172] a return stroke rises instantly AT sample 0: it is a peak (a rise from the dark before it). The
        // search used to start at 1, so the first stroke was never counted and a flash kept 4 pulses in a second.
        float prev = (i > 0) ? e[i - 1] : 0.0f;
        if (!(e[i] > 0.02f && e[i] > prev && e[i] >= e[i + 1])) {
            continue;
        }
        recent = 0;
        for (k = 0; k < nCounted; k++) {
            if (i - counted[k] < window) {
                recent++;
            }
        }
        if (recent >= perWindow && lastPeak >= 0) {
            // [bug-3172] VALLEY FILL from the last counted peak to this one: each sample is raised to min(the highest
            // level before it, the highest level after it). The old constant floor min(e[lastPeak], e[i]) left an
            // earlier MERGED peak standing above it with a dip after it = one more pulse (two merges in a second).
            float suf[LT_SAMPLES], pre;
            suf[i - lastPeak] = e[i];
            for (j = i - 1; j >= lastPeak; j--) {
                suf[j - lastPeak] = e[j] > suf[j + 1 - lastPeak] ? e[j] : suf[j + 1 - lastPeak];
            }
            pre = e[lastPeak];
            for (j = lastPeak; j <= i; j++) {
                if (e[j] > pre) {
                    pre = e[j];
                }
                fl = pre < suf[j - lastPeak] ? pre : suf[j - lastPeak];
                if (e[j] < fl) {
                    e[j] = fl;
                }
            }
        } else if (nCounted < 64) {
            counted[nCounted++] = i;
            lastPeak            = i;
        }
    }
}

static void LT_BuildEnvelope(int mode, int nStrokes)
{
    ltStroke_t st[LT_MAX_STROKES];
    int        i, count = 0;
    float      t = 0.0f;
    qboolean   ccUsed = qfalse;

    memset(s_lt.env, 0, sizeof(s_lt.env));
    memset(s_lt.envBolt, 0, sizeof(s_lt.envBolt));
    s_lt.rng = (unsigned)s_lt.seed * 2654435761u + 12345u;

    if (mode == 2) {
        // REDUCED FLASHING: one soft pulse, 60 ms rise, 320 ms decay, half amplitude, no flicker
        st[0].t    = 0.0f;
        st[0].amp  = 0.5f;
        st[0].tau  = 0.32f;
        st[0].hold = 0.0f;
        st[0].rise = 0.06f;
        count      = 1;
    } else if (s_lt.kind == 'c') {
        // cloud-to-ground: return strokes, gaps log-normal around 60 ms (Rakov & Uman), ~30 ms decay each, and at
        // most one continuing current (a 60-150 ms held glow) on a subsequent stroke
        for (i = 0; i < nStrokes && i < LT_MAX_STROKES; i++) {
            if (i > 0) {
                float gap = 0.060f * expf(0.6f * LT_Gauss());
                if (gap < 0.015f) {
                    gap = 0.015f;
                } else if (gap > 0.25f) {
                    gap = 0.25f;
                }
                t += gap;
            }
            st[count].t    = t;
            st[count].amp  = (i == 0) ? 1.0f : 0.45f + 0.45f * LT_Rand();
            st[count].tau  = 0.025f + 0.015f * LT_Rand();
            st[count].hold = 0.0f;
            st[count].rise = 0.0f;
            if (i > 0 && !ccUsed && LT_Rand() < 0.35f) {
                st[count].hold = 0.06f + 0.09f * LT_Rand();
                ccUsed         = qtrue;
                t += st[count].hold;
            }
            count++;
        }
    } else {
        // intra-cloud / sheet lightning: 1-3 broad, softer pulses
        for (i = 0; i < nStrokes && i < 3; i++) {
            if (i > 0) {
                t += 0.12f + 0.18f * LT_Rand();
            }
            st[count].t    = t;
            st[count].amp  = 0.55f + 0.35f * LT_Rand();
            st[count].tau  = 0.08f + 0.08f * LT_Rand();
            st[count].hold = 0.0f;
            st[count].rise = 0.04f;
            count++;
        }
    }

    for (i = 0; i < LT_SAMPLES; i++) {
        float tt = (float)i / LT_RATE, v = 0.0f;
        int   k;
        for (k = 0; k < count; k++) {
            v += LT_StrokeAt(&st[k], tt);
        }
        s_lt.env[i] = v;
    }
    memcpy(s_lt.envBolt, s_lt.env, sizeof(s_lt.env));
    LT_Limit(s_lt.env, LT_SAMPLES, 3, LT_RATE);
    LT_Limit(s_lt.envBolt, LT_SAMPLES, LT_BOLT_PULSES, LT_RATE);
}

// ---------------------------------------------------------------- switches
static qboolean LT_On(void)
{
    if (!cg_hzmLightning) {
        return qfalse;
    }
    if (!HZM_ResolveAutoSwitch(cg_hzmLightning->integer, HZM_LIGHTNING_AUTO)) {
        return qfalse;
    }
    if (HZM_LightRestoreMapProtected(cgs.mapname)) {
        return qfalse;
    }
    return qtrue;
}

static int LT_Mode(void)
{
    int m = cg_hzmLightningMode ? cg_hzmLightningMode->integer : 1;
    if (m < 0 || m > 2) {
        m = 1;
    }
    return m;
}

static void LT_Unpublish(void)
{
    if (s_lt.published) {
        cgi.Cvar_Set("r_hzmLtState", "0");
        s_lt.published = qfalse;
    }
}

/*
Does THIS client render the strike itself (so the quick-fix fog pulses must be hidden from it)? New cgame + a gl2 renderer
+ the rollout switch on + not Omaha. Mode 0 (Off) counts too: Off means no flash at all, so its pulses are hidden as well.
*/
static qboolean LT_RendersStrike(void)
{
    return (LT_On() && CG_HZM_RendererIsGl2()) ? qtrue : qfalse;
}

/*
Called by CG_ParseFogInfo's caller (cg_main.c CS_FOGINFO) AFTER the parse: while a hold is on, remember the new colour and
put the held one back. Returns nothing; cg.farplane_color is the only thing touched.
*/
void CG_HzmLt_FogFilter(void)
{
    if (!s_lt.holdOn) {
        return;
    }
    if (cg.time >= s_lt.holdUntil) {
        s_lt.holdOn = qfalse;
        return;
    }
    VectorCopy(cg.farplane_color, s_lt.latestColor);
    VectorCopy(s_lt.heldColor, cg.farplane_color);
}

// ---------------------------------------------------------------- lifecycle
void CG_HzmLt_Init(void)
{
    memset(&s_lt, 0, sizeof(s_lt));
    cg_hzmLightning      = cgi.Cvar_Get("cg_hzmLightning", "-1", 0);
    cg_hzmLightningMode  = cgi.Cvar_Get("cg_hzmLightningMode", "1", CVAR_ARCHIVE);
    cg_hzmLightningBolts = cgi.Cvar_Get("cg_hzmLightningBolts", "-1", 0);
    // dev tuning: "<skyScale> <worldScale> <hazeScale>" (flags 0, never read unless set)
    cg_hzmLightningTune  = cgi.Cvar_Get("cg_hzmLightningTune", "", 0);
    // CAPABILITY in the userinfo (plan section 10): this cgame plays the per-listener thunder itself, so the server must
    // not also play the legacy per-player thunder to it. Read by global/weather.scr with info_valueforkey. ROM: the
    // player cannot change what the binary can do. 12 bytes of userinfo.
    cgi.Cvar_Get("cg_hzmLt", "1", CVAR_USERINFO | CVAR_ROM);
    // the per-frame channel to the renderer is cleared here too (bug-1202 class: a stale value must not survive a
    // map change or reconnect)
    cgi.Cvar_Set("r_hzmLtState", "0");
    // T10/T14e: proves which cgame loaded (grep -a -c LTBUILD in the DLL finds the literal)
    cgi.Printf("^~^~^ LTBUILD cgame %s %s auto=%d\n", __DATE__, __TIME__, HZM_LIGHTNING_AUTO);
}

void CG_HzmLt_ConfigString(const char *str)
{
    int  seq = 0, t = 0, yaw = 0, dist = 0, n = 0, seed = 0;
    char kind = 0;
    int  mode;
    float el, h, vol, delaySec, dUnits;

    if (!str || !str[0]) {
        return;
    }
    if (sscanf(str, "v1 %d %d %c %d %d %d %d", &seq, &t, &kind, &yaw, &dist, &n, &seed) != 7) {
        return;
    }
    if (seq == s_lt.seq) {
        return;
    }
    s_lt.seq = seq;
    if (HZM_LightRestoreMapProtected(cgs.mapname)) {
        return;
    }
    // a late joiner receives the last strike in its gamestate: anything older than 2 s is history, not a flash
    if (cg.time - t > HZM_LT_STALE_MS || cg.time == 0) {
        return;
    }

    s_lt.kind  = kind;
    s_lt.yaw   = (float)yaw;
    s_lt.dist  = (float)dist;
    s_lt.seed  = seed;
    // the elevation of the lit region: the cloud base (1500 m for a ground stroke, 3000 m inside the cloud)
    h  = (kind == 'c') ? 1500.0f : 3000.0f;
    el = RAD2DEG(atan2f(h, s_lt.dist));
    if (el < 6.0f) { el = 6.0f; } else if (el > 45.0f) { el = 45.0f; }
    s_lt.dir[0] = cosf(DEG2RAD(el)) * cosf(DEG2RAD(s_lt.yaw));
    s_lt.dir[1] = cosf(DEG2RAD(el)) * sinf(DEG2RAD(s_lt.yaw));
    s_lt.dir[2] = sinf(DEG2RAD(el));

    mode            = LT_Mode();
    LT_BuildEnvelope(mode, n);
    s_lt.startTime  = t + 50;         // one server frame of lead so every client starts on the same frame
    s_lt.active     = LT_On() && mode != 0;
    // hide the server's fallback fog pulses (they start 0.1 s after the strike and end within 1.5 s): 3 s covers them
    if (LT_RendersStrike()) {
        if (!s_lt.holdOn) {
            VectorCopy(cg.farplane_color, s_lt.heldColor);
            VectorCopy(cg.farplane_color, s_lt.latestColor);
        }
        s_lt.holdOn    = qtrue;
        s_lt.holdUntil = cg.time + 3000;
    }

    // THUNDER: distance from THIS listener. The strike is dist metres away along yaw from the map (the storm is far
    // bigger than any map, so the listener's offset only matters as a projection on the strike direction).
    dUnits   = s_lt.dist / 0.0254f;
    {
        vec3_t flat;
        flat[0] = cosf(DEG2RAD(s_lt.yaw));
        flat[1] = sinf(DEG2RAD(s_lt.yaw));
        flat[2] = 0.0f;
        dUnits -= DotProduct(cg.refdef.vieworg, flat);
    }
    if (dUnits < 1500.0f / 0.0254f * 0.9f) {
        dUnits = 1500.0f / 0.0254f * 0.9f;
    }
    delaySec = (dUnits * 0.0254f) / 343.0f;
    s_lt.thunderAt = s_lt.startTime + (int)(delaySec * 1000.0f);
    VectorCopy(s_lt.dir, s_lt.thunderDir);
    // bands (user decision D4: 1.5-12 km, no near cracks)
    if (s_lt.dist < 3000.0f) {
        Q_strncpyz(s_lt.thunderAlias, "coop_lt_near", sizeof(s_lt.thunderAlias));
    } else if (s_lt.dist < 7000.0f) {
        Q_strncpyz(s_lt.thunderAlias, (seed & 1) ? "coop_lt_mid1" : "coop_lt_mid2", sizeof(s_lt.thunderAlias));
    } else {
        Q_strncpyz(s_lt.thunderAlias, (seed & 1) ? "coop_lt_far1" : "coop_lt_far2", sizeof(s_lt.thunderAlias));
    }
    vol = powf(2500.0f / s_lt.dist, 0.8f);
    if (vol > 1.0f) { vol = 1.0f; } else if (vol < 0.25f) { vol = 0.25f; }
    s_lt.thunderVol = vol;

    cgi.Printf(
        "^~^~^ LTSTRIKE cl seq=%d kind=%c yaw=%d dist=%d n=%d mode=%d on=%d thunderIn=%.2f band=%s\n",
        seq, kind, yaw, dist, n, mode, s_lt.active ? 1 : 0, (s_lt.thunderAt - cg.time) / 1000.0f, s_lt.thunderAlias
    );
}

static void LT_PlayThunder(void)
{
    AliasListNode_t *node = NULL;
    const char      *name = cgi.Alias_FindRandom(s_lt.thunderAlias, &node);
    vec3_t           org;

    if (!name || !node) {
        cgi.DPrintf("^~^~^ LTSTRIKE thunder alias %s missing\n", s_lt.thunderAlias);
        return;
    }
    VectorMA(cg.refdef.vieworg, 400.0f, s_lt.thunderDir, org);
    cgi.S_StartSound(
        org, ENTITYNUM_NONE, CHAN_AUTO, cgi.S_RegisterSound(name, node->streamed), node->volume * s_lt.thunderVol,
        node->dist, node->pitch, node->maxDist, node->streamed
    );
}

void CG_HzmLt_Frame(void)
{
    float  e, eb, eSky, eWorld, eHaze, eBolt, dist, scale, lum, ts = 1.0f, tw = 1.0f, th = 1.0f;
    int    i, mode;
    float  x;

    if (s_lt.thunderAt && cg.time >= s_lt.thunderAt) {
        s_lt.thunderAt = 0;
        LT_PlayThunder();
    }
    if (s_lt.holdOn && cg.time >= s_lt.holdUntil) {
        // the hold is over: the latest colour the server sent (normally its restored base colour) takes effect
        s_lt.holdOn = qfalse;
        VectorCopy(s_lt.latestColor, cg.farplane_color);
    }

    if (!s_lt.active || !LT_On()) {
        s_lt.active = qfalse;
        LT_Unpublish();
        return;
    }
    mode = LT_Mode();
    if (mode == 0) {
        s_lt.active = qfalse;
        LT_Unpublish();
        return;
    }

    x = (cg.time - s_lt.startTime) * (LT_RATE / 1000.0f);
    if (x < 0.0f) {
        return;
    }
    i = (int)x;
    if (i >= LT_SAMPLES - 1) {
        s_lt.active = qfalse;
        LT_Unpublish();
        return;
    }
    e  = s_lt.env[i] + (s_lt.env[i + 1] - s_lt.env[i]) * (x - i);
    eb = s_lt.envBolt[i] + (s_lt.envBolt[i + 1] - s_lt.envBolt[i]) * (x - i);

    // brightness: nearer strikes are brighter; the map's fog luminance keeps a night map from getting a day map's
    // absolute flash (plan 2.3)
    dist  = s_lt.dist;
    scale = powf(3000.0f / dist, 0.7f);
    if (scale > 1.2f) { scale = 1.2f; } else if (scale < 0.35f) { scale = 0.35f; }
    lum = 0.299f * cg.farplane_color[0] + 0.587f * cg.farplane_color[1] + 0.114f * cg.farplane_color[2];
    {
        float m = 0.35f + 0.9f * lum;
        if (m < 0.4f) { m = 0.4f; } else if (m > 1.0f) { m = 1.0f; }
        scale *= m;
    }
    if (cg_hzmLightningTune && cg_hzmLightningTune->string[0]) {
        sscanf(cg_hzmLightningTune->string, "%f %f %f", &ts, &tw, &th);
    }

    eSky   = e * scale * ts;
    eWorld = e * scale * 0.75f * tw * (s_lt.kind == 'c' ? 1.0f : 0.6f);
    eHaze  = e * scale * 0.55f * th;
    eBolt  = 0.0f;
    // THE BOLT (user decision 2026-09-27: on with realistic mode, cg_hzmLightningBolts -1 = HZM_LTBOLTS_AUTO). Ground
    // strokes only. It rides its OWN envelope eb (bug-3172): the same strokes, limited to LT_BOLT_PULSES a second,
    // because on a night sky every pulse it keeps is a full WCAG flash. Reduced flashing: every other ground stroke only (by seed,
    // the same on every client), at 40% and on the one soft pulse. Off (mode 0) never reaches here.
    if (s_lt.kind == 'c' && HZM_ResolveAutoSwitch(cg_hzmLightningBolts->integer, HZM_LTBOLTS_AUTO)) {
        // a bolt through rain fades with distance (visibility ~6 km in a storm)
        if (mode == 1) {
            eBolt = eb * expf(-dist / 6000.0f) * 1.6f;
        } else if (mode == 2 && (s_lt.seed & 2)) {
            eBolt = eb * expf(-dist / 6000.0f) * 1.6f * 0.4f;
        }
    }

    cgi.Cvar_Set(
        "r_hzmLtState",
        va("%d %d %.4f %.4f %.4f %.4f %.4f %.4f %.4f %d %.1f %.2f", HZM_LT_STATE_VERSION, cg.time, eSky, eWorld, eHaze,
           eBolt, s_lt.dir[0], s_lt.dir[1], s_lt.dir[2], s_lt.seed, s_lt.yaw,
           RAD2DEG(atan2f(1500.0f, dist)))
    );
    s_lt.published = qtrue;
}
