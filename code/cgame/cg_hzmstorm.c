/*
===========================================================================
HZM coop [2026-09-28] STORM DARKNESS - the client half (docs/proposals/storm_darkness_2026-09-28/plan.md section 2).

The server (coop_mod/stormlight.scr) decides every rain storm's darkness and sends it as KEYFRAMES:
    coop_storm "1 <tok> <seq> <from x1000> <to x1000> <durMs> <elapsedMs>"
re-sent every 3 s. Every client evaluates the SAME C2 curve from them every frame, so all players see the same sky at
the same time and nothing on the wire is a staircase (research 6: a 1 % replicated step is ~1.3 LSB on mid-grey, the
per-frame curve is ~0.07 LSB). A keyframe is anchored once (t0 = cg.time - elapsed); a repeat re-anchors only when it
disagrees by more than 1.5 s; a second stage (tau 0.8 s) turns any re-anchor into an ease, never a step. The first value
this cgame instance sees within 8 s of its first frame SNAPS (a mid-storm join or map load starts dark - it never
visibly darkens under the player); a vid_restart parks the live keyframe keyed on the level instance and takes it back.

The mapping (hzm_storm.h HZM_StormMap) is driven by D and by how "day" the map is, from the luminance of the RAW fog
colour. cg.farplane_color itself is never modified: the headlight dark gate and the lightning's fog scaling read the
map's real fog. Outputs: CG_HzmStorm_FogColor (the refdef copy, gl1 + gl2), r_hzmStormNow (gl2 grade / sun / sky / wet),
CG_HzmStorm_SunFade (decal shadows), CG_HzmStorm_HeadlightDay (pools + flares on day maps).
Switch cg_hzmStorm (-1 auto = HZM_STORM_AUTO, 0 = the player opts out). Omaha: never.
All state lives in one struct (renderer-reinit contract: no function statics).
===========================================================================
*/
#include "cg_local.h"
#include "../renderercommon/hzm_light_restore.h"   // HZM_ResolveAutoSwitch
#include "../renderercommon/hzm_waterwet.h"        // HZM_WaterWetOmahaMap (the five-map list)
#include "../renderercommon/hzm_storm.h"

#define STORM_TAU2_S        0.8f     // stage-2 ease
#define STORM_SNAP_MS       8000     // a value arriving this soon after the first frame snaps
#define STORM_REANCHOR_MS   1500
#define STORM_DAY_TAU_S     2.0f     // a map fog change must not step the mapping

typedef struct {
    cvar_t  *cvStorm, *cvOn, *cvKept, *cvDebug, *cvNow, *cvTune;
    int      tuneMod;
    hzmStormTune_t tune;       // HZM_STORM_TUNE_DEFAULT, or cg_hzmStormTune (look-dev only)
    int      registered;
    int      lastMod;          // coop_storm modificationCount taken
    int      valid;            // a keyframe is anchored
    int      tok, seq;
    float    from, to;
    int      dur;              // ms
    int      t0;               // cg.time the keyframe started
    float    s;                // the eased darkness (what is shown)
    int      sInit;            // s holds a value for this map
    float    day;
    int      dayInit;
    int      firstFrame;       // cg.time of this instance's first frame on this map (0 = none yet)
    int      lastTime;
    int      lastDebug;
    char     map[MAX_QPATH];
    hzmStormState_t cur;
    char     pub[192];         // last r_hzmStormNow written
    int      restoredT0;       // a vid_restart park was taken back (t0/s restored)
} hzmStorm_t;

static hzmStorm_t s_st;

static void CG_HzmStorm_Register(void)
{
    if (s_st.registered) {
        return;
    }
    s_st.registered = 1;
    s_st.cvStorm = cgi.Cvar_Get("coop_storm", "", 0);             // the server's keyframe (stufftext)
    s_st.cvOn    = cgi.Cvar_Get("cg_hzmStorm", "-1", 0);          // -1 auto, 0 off (player), 1 on
    s_st.cvKept  = cgi.Cvar_Get("cg_hzmStormKept", "", 0);        // the vid_restart park
    s_st.cvDebug = cgi.Cvar_Get("cg_hzmStormDebug", "0", 0);      // 1 = a ^~^~^ line per second
    s_st.cvNow   = cgi.Cvar_Get("r_hzmStormNow", "", 0);          // the renderer state (hzm_storm.h 3)
    // LOOK-DEV ONLY: 13 numbers in hzmStormTune_t order override the header's look ("" = HZM_STORM_TUNE_DEFAULT)
    s_st.cvTune  = cgi.Cvar_Get("cg_hzmStormTune", "", 0);
    s_st.lastMod = -1;
    s_st.tuneMod = -1;
}

// the level instance (the bug-3048 grade-keep key): "<sv_serverid>.<level start time>@<serverinfo mapname>"
static void CG_HzmStorm_Key(char *out, int size)
{
    char sid[64];
    char map[MAX_QPATH];

    Q_strncpyz(sid, Info_ValueForKey(CG_ConfigString(CS_SYSTEMINFO), "sv_serverid"), sizeof(sid));
    Q_strncpyz(map, Info_ValueForKey(CG_ConfigString(CS_SERVERINFO), "mapname"), sizeof(map));
    out[0] = 0;
    if (sid[0] && map[0]) {
        Com_sprintf(out, size, "%s.%s@%s", sid, CG_ConfigString(CS_LEVEL_START_TIME), map);
    }
}

static int CG_HzmStorm_Parse(const char *str, int *tok, int *seq, float *from, float *to, int *dur, int *elapsed)
{
    int ver, f, t;

    if (!str || !str[0]) {
        return 0;
    }
    if (sscanf(str, "%d %d %d %d %d %d %d", &ver, tok, seq, &f, &t, dur, elapsed) != 7 || ver != HZM_STORM_VERSION) {
        return 0;
    }
    *from = HZM_StormSat01(f * 0.001f);
    *to   = HZM_StormSat01(t * 0.001f);
    if (*dur < 50) {
        *dur = 50;
    } else if (*dur > 600000) {
        *dur = 600000;
    }
    if (*elapsed < 0) {
        *elapsed = 0;
    } else if (*elapsed > *dur) {
        *elapsed = *dur;
    }
    return 1;
}

static float CG_HzmStorm_Curve(void)
{
    float t;

    if (!s_st.valid) {
        return 0.0f;
    }
    t = (float)(cg.time - s_st.t0) / (float)s_st.dur;
    return s_st.from + (s_st.to - s_st.from) * HZM_StormSmootherstep(t);
}

void CG_HzmStorm_Init(void)
{
    char key[MAX_QPATH + 96];
    char kept[MAX_STRING_CHARS];
    char *p1, *p2, *p3;
    int  keyLen;

    CG_HzmStorm_Register();
    memset(s_st.pub, 0, sizeof(s_st.pub));
    cgi.Cvar_Set("r_hzmStormNow", "");      // the renderer starts at identity; the first frame republishes
    cgi.Printf("^~^~^ STORMBUILD cgame %s %s auto=%d\n", __DATE__, __TIME__, HZM_STORM_AUTO);

    // a vid_restart that kept the level: take the parked keyframe back ("<key>|<payload>|<t0>|<s>"), one-shot
    Q_strncpyz(kept, s_st.cvKept->string, sizeof(kept));
    cgi.Cvar_Set("cg_hzmStormKept", "");
    if (!kept[0]) {
        return;
    }
    CG_HzmStorm_Key(key, sizeof(key));
    keyLen = (int)strlen(key);
    if (!keyLen || strncmp(kept, key, keyLen) || kept[keyLen] != '|') {
        cgi.Printf("^~^~^ STORMLIGHT keep: parked keyframe dropped (level changed)\n");
        return;
    }
    p1 = kept + keyLen + 1;              // payload
    p2 = strchr(p1, '|');
    if (!p2) {
        return;
    }
    *p2++ = 0;
    p3 = strchr(p2, '|');
    if (!p3) {
        return;
    }
    *p3++ = 0;
    {
        int   tok, seq, dur, el;
        float from, to;
        if (!CG_HzmStorm_Parse(p1, &tok, &seq, &from, &to, &dur, &el)) {
            return;
        }
        cgi.Cvar_Set("coop_storm", p1);
        s_st.lastMod = s_st.cvStorm->modificationCount;   // taken: do not re-anchor from its (older) elapsed
        s_st.valid   = 1;
        s_st.tok     = tok;
        s_st.seq     = seq;
        s_st.from    = from;
        s_st.to      = to;
        s_st.dur     = dur;
        s_st.t0      = atoi(p2);
        s_st.s       = (float)atof(p3);
        s_st.sInit   = 1;
        s_st.restoredT0 = 1;
        Q_strncpyz(s_st.map, cgs.mapname, sizeof(s_st.map));
        cgi.Printf("^~^~^ STORMLIGHT keep: restored after cgame reload seq=%d s=%.3f\n", seq, s_st.s);
    }
}

void CG_HzmStorm_Shutdown(void)
{
    char key[MAX_QPATH + 96];

    CG_HzmStorm_Register();
    CG_HzmStorm_Key(key, sizeof(key));
    if (key[0] && s_st.valid && s_st.cvStorm->string[0]) {
        cgi.Cvar_Set("cg_hzmStormKept", va("%s|%s|%d|%g", key, s_st.cvStorm->string, s_st.t0, s_st.s));
    } else {
        cgi.Cvar_Set("cg_hzmStormKept", "");
    }
    // nothing outlives the level: the next map (or an MP server, or the menu) starts clear until a server republishes
    cgi.Cvar_Set("coop_storm", "");
    cgi.Cvar_Set("r_hzmStormNow", "");
    memset(&s_st, 0, sizeof(s_st));
}

void CG_HzmStorm_Frame(void)
{
    float    dt, D, lum, day, tau;
    qboolean on, gl2;
    char     buf[192];

    CG_HzmStorm_Register();

    // a new map in the same instance (a map change reloads cgame, but be safe): start over
    if (Q_stricmp(s_st.map, cgs.mapname)) {
        Q_strncpyz(s_st.map, cgs.mapname, sizeof(s_st.map));
        s_st.valid = s_st.sInit = s_st.dayInit = 0;
        s_st.firstFrame = 0;
        s_st.lastTime = 0;
        s_st.s = 0.0f;
    }
    if (!s_st.firstFrame) {
        s_st.firstFrame = cg.time ? cg.time : 1;
    }

    // dt from elapsed time, clamped (TRAPS: integrate elapsed time; a hitch must not dump a whole ease)
    if (!s_st.lastTime || cg.time < s_st.lastTime) {
        dt = 0.0f;
    } else {
        dt = (cg.time - s_st.lastTime) * 0.001f;
        if (dt > 0.25f) {
            dt = 0.25f;
        }
    }
    s_st.lastTime = cg.time;

    // the keyframe
    if (s_st.cvStorm->modificationCount != s_st.lastMod) {
        int   tok, seq, dur, el;
        float from, to;

        s_st.lastMod = s_st.cvStorm->modificationCount;
        if (CG_HzmStorm_Parse(s_st.cvStorm->string, &tok, &seq, &from, &to, &dur, &el)) {
            int t0 = cg.time - el;
            if (!s_st.valid || tok != s_st.tok || seq != s_st.seq) {
                s_st.restoredT0 = 0;   // a new keyframe: missed-start re-anchoring is live again (vet F20)
                s_st.valid = 1;
                s_st.tok   = tok;
                s_st.seq   = seq;
                s_st.from  = from;
                s_st.to    = to;
                s_st.dur   = dur;
                s_st.t0    = t0;
                cgi.DPrintf("^~^~^ STORMLIGHT cl seq=%d from=%.3f to=%.3f dur=%d el=%d\n", seq, from, to, dur, el);
            } else if (abs(t0 - s_st.t0) > STORM_REANCHOR_MS && !s_st.restoredT0) {
                s_st.t0 = t0;     // a missed start: re-anchor (stage 2 below eases the difference)
            }
        }
    }

    on  = HZM_ResolveAutoSwitch(s_st.cvOn->integer, HZM_STORM_AUTO)
       && !HZM_WaterWetOmahaMap(cgs.mapname) && cg.snap;
    gl2 = CG_HZM_RendererIsGl2();

    D = on ? CG_HzmStorm_Curve() : 0.0f;

    if (!s_st.sInit) {
        if (s_st.valid && on) {
            // the first value: snap if it came with the load / join, else ease in from clear
            s_st.s = (cg.time - s_st.firstFrame < STORM_SNAP_MS) ? D : 0.0f;
            s_st.sInit = 1;
        } else {
            s_st.s = 0.0f;
        }
    }
    if (s_st.sInit && dt > 0.0f) {
        s_st.s += (D - s_st.s) * (1.0f - expf(-dt / STORM_TAU2_S));
    }
    if (!on && HZM_WaterWetOmahaMap(cgs.mapname)) {
        s_st.s = 0.0f;   // Omaha: nothing, not even an ease
    }
    if (s_st.s < 0.0005f && D <= 0.0f) {
        s_st.s = 0.0f;
    }

    // how "day" the map is, from its RAW fog colour (eased: a fog zone must not step the mapping)
    lum = 0.299f * cg.farplane_color[0] + 0.587f * cg.farplane_color[1] + 0.114f * cg.farplane_color[2];
    day = HZM_StormDayness(lum);
    if (!s_st.dayInit || dt <= 0.0f) {
        if (!s_st.dayInit) {
            s_st.day = day;
            s_st.dayInit = 1;
        }
    } else {
        tau = 1.0f - expf(-dt / STORM_DAY_TAU_S);
        s_st.day += (day - s_st.day) * tau;
    }

    if (s_st.cvTune->modificationCount != s_st.tuneMod) {
        static const hzmStormTune_t def = HZM_STORM_TUNE_DEFAULT;
        hzmStormTune_t t = def;
        s_st.tuneMod = s_st.cvTune->modificationCount;
        if (s_st.cvTune->string[0]
            && sscanf(s_st.cvTune->string, "%f %f %f %f %f %f %f %f %f %f %f %f %f", &t.expoDay, &t.expoNight, &t.satDay,
                      &t.satNight, &t.tempDay, &t.tempNight, &t.fadeDay, &t.fadeNight, &t.fogDay, &t.fogNight,
                      &t.gainDay, &t.gainNight, &t.fogPortal) != 13) {
            t = def;
        }
        s_st.tune = t;
    }
    HZM_StormMap(s_st.s, s_st.day, gl2 ? 1 : 0, cg.sky_portal ? 1 : 0, &s_st.tune, &s_st.cur);

    // gl2: publish only when it moves (and "" at rest, so the renderer is byte-identical when there is no storm)
    if (gl2) {
        if (s_st.cur.active) {
            Com_sprintf(buf, sizeof(buf), "%.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f", s_st.cur.expo, s_st.cur.cont,
                        s_st.cur.sat, s_st.cur.temp, s_st.cur.sunFade, s_st.cur.fog[0], s_st.cur.fog[1],
                        s_st.cur.fog[2], s_st.cur.gain);
        } else {
            buf[0] = 0;
        }
        if (strcmp(buf, s_st.pub) || strcmp(s_st.cvNow->string, buf)) {
            Q_strncpyz(s_st.pub, buf, sizeof(s_st.pub));
            cgi.Cvar_Set("r_hzmStormNow", buf);
        }
    }

    if (s_st.cvDebug->integer && cg.time - s_st.lastDebug >= 1000) {
        s_st.lastDebug = cg.time;
        cgi.Printf("^~^~^ STORMLIGHT t=%d D=%.4f s=%.4f day=%.3f on=%d gl2=%d expo=%.4f fog=%.4f sun=%.3f seq=%d\n",
                   cg.time, D, s_st.s, s_st.day, on ? 1 : 0, gl2 ? 1 : 0, s_st.cur.expo, s_st.cur.fog[1],
                   s_st.cur.sunFade, s_st.seq);
    }
}

void CG_HzmStorm_FogColor(float *rgb)
{
    if (!s_st.cur.active || !rgb) {
        return;
    }
    // farplaneColorOverride uses -1 as "unset": never scale a negative sentinel
    if (rgb[0] < 0.0f || rgb[1] < 0.0f || rgb[2] < 0.0f) {
        return;
    }
    rgb[0] *= s_st.cur.fog[0];
    rgb[1] *= s_st.cur.fog[1];
    rgb[2] *= s_st.cur.fog[2];
}

float CG_HzmStorm_SunFade(void)
{
    return s_st.cur.active ? s_st.cur.sunFade : 0.0f;
}

float CG_HzmStorm_HeadlightDay(void)
{
    return s_st.cur.active ? HZM_StormHeadlightDay(s_st.s, s_st.day) : 0.0f;
}
