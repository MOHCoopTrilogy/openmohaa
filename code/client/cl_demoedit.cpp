/*
===========================================================================
HZM coop - DEMO EDITOR (client side)

Built into demo playback, no external tool. This file owns everything that needs the client:
  - the playback clock (pause, speed, step, seek - backward seeks rewind the demo file to just
    after its gamestate and fast-forward, applying configstrings on the way)
  - the free camera (real-time WASD/mouse, never scaled by the demo speed, never touched by the
    recorded player's zoom/scope/ADS - the cgame sensitivity scale is skipped)
  - camera paths (waypoints, Hermite position + quaternion rotation, saved per demo)
  - player name aliases (demo_setname), applied inside the configstrings so every cgame screen
    (scoreboard, feed, names) shows them, saved per demo
  - the keypad controls

The cgame reads the camera from the cvar demo_cam ("x y z pitch yaw roll fov", empty = off),
the HUD mode from demo_hud (0 full, 1 names only, 2 none) and the status line from demo_status.
After a seek the client runs the cgame commands demoed_cs <index> (changed configstrings) and
demoed_reset <snapshotNum> <serverCommandSequence> (rebuild the snapshot pipeline).
===========================================================================
*/

#include "client.h"
#include "../qcommon/q_shared.h"
#include "../qcommon/hzm_filters.h" // HZM coop - visual filter names

// defined in cl_cgame.cpp with C++ linkage, not in a header
void     CL_ConfigstringModified(int index, char *s);
qboolean CL_GetServerCommand(int serverCommandNumber, qboolean differentServer);

#define DE_MAX_WP 128
#define DE_SEEK_STEP_MS 5000

typedef struct {
    int    time; // demo server time (ms) when the waypoint was set
    vec3_t org;
    vec3_t ang;
    float  fov;
} deWaypoint_t;

typedef struct {
    qboolean started; // first snapshot seen for this demo
    qboolean paused;
    float    speed;
    double   demoTime; // ms, server time we are showing
    int      lastRealMs;
    int      firstSnapTime;
    int      lastSnapTime; // newest snapshot time read so far (for the status line)
    // A demo that changes level holds one gamestate per level (a SEGMENT). The editor's clock runs on across
    // them: display ms = segBase + (demoTime - firstSnapTime). Waypoints and every seek are in display ms.
    int      segBase;
    int      segIndex;

    // rewind point (captured when the gamestate is parsed)
    qboolean haveRewind;
    int      gamestates;
    qboolean atEnd; // ran out of demo: holding the last frame
    int      rewindOffset;
    int      rewindCmdSeq;

    // free camera
    qboolean freecam;
    vec3_t   camOrg;
    vec3_t   camAng;

    // camera path
    deWaypoint_t wp[DE_MAX_WP];
    int          numWp;
    qboolean     pathPlaying;
    qboolean     pathRealtime; // waypoints share times: play them on a path clock instead
    double       pathClock;    // ms, realtime-mode clock

    int  hudMode;
    char baseName[MAX_QPATH];

    char alias[MAX_CLIENTS][MAX_NAME_LENGTH];
    char origName[MAX_CLIENTS][MAX_NAME_LENGTH];
} demoEd_t;

static demoEd_t     de;
// a seek that has to wait for a level to load (forward across a level change, or back into an earlier level,
// which restarts the demo). Outside de: a restart resets de. The camera state rides along.
static struct {
    qboolean active;
    int      displayMs;
    qboolean freecam, paused;
    vec3_t   camOrg, camAng;
    float    speed;
    char     demo[MAX_QPATH];
} s_pend;
static gameState_t  de_rewindGS;
static entityState_t de_rewindBaselines[MAX_GENTITIES];

static cvar_t *demo_editor;
static cvar_t *demo_fov;
static cvar_t *demo_camSpeed;
static cvar_t *demo_pathSegTime;
static cvar_t *demo_cam;
static cvar_t *demo_hud;
static cvar_t *demo_playing; // 1 while a demo plays: lets gl1's post-FX run with no server
static cvar_t *r_ppFilterCv;  // the visual filter (renderer reads it)
// HUD mode, 0 full / 1 player names only (demos) / 2 none. Outside de on purpose: a clean HUD set while
// playing (cleanhud, KP Minus) stays through map changes and into a demo or an F10 render.
static int s_hudMode;
static cvar_t *demo_status;

static void DE_SaveCam(void);
static void DE_LoadCam(void);
static void TL_Frame(void);
static qboolean TL_MouseKey(int key, qboolean down);
static void TL_ResetInput(void);
static void DE_SaveNames(void);
static void DE_LoadNames(void);

/*
==================
helpers
==================
*/
qboolean CL_DemoEd_Enabled(void)
{
    return (clc.demoplaying && demo_editor && demo_editor->integer && !cl_timedemo->integer) ? qtrue : qfalse;
}

static qboolean DE_Ready(void)
{
    if (!CL_DemoEd_Enabled() || clc.state != CA_ACTIVE || !de.started) {
        Com_Printf("Demo editor: only while a demo is playing.\n");
        return qfalse;
    }
    return qtrue;
}

static double DE_DisplayMs(void)
{
    return de.segBase + (de.demoTime - de.firstSnapTime);
}

static float DE_Clock(void)
{
    return (float)(DE_DisplayMs() / 1000.0);
}

static void DE_FmtTime(float sec, char *out, int size)
{
    int m;
    if (sec < 0) {
        sec = 0;
    }
    m = (int)(sec / 60.0f);
    Com_sprintf(out, size, "%d:%04.1f", m, sec - m * 60.0f);
}

static void DE_PlayerEye(vec3_t org, vec3_t ang)
{
    VectorCopy(cl.snap.ps.origin, org);
    org[2] += cl.snap.ps.viewheight;
    VectorCopy(cl.snap.ps.viewangles, ang);
}

static void DE_CurrentCamera(vec3_t org, vec3_t ang, float *fov)
{
    if (de.freecam) {
        VectorCopy(de.camOrg, org);
        VectorCopy(de.camAng, ang);
    } else {
        DE_PlayerEye(org, ang);
    }
    *fov = demo_fov->value;
}

/*
==================
quaternion maths for the path
==================
*/
static void DE_AnglesToQuat(const vec3_t ang, float q[4])
{
    vec3_t ax[3];
    float  m[3][3], tr, s;
    int    r, c;

    AnglesToAxis(ang, ax);
    for (r = 0; r < 3; r++) {
        for (c = 0; c < 3; c++) {
            m[r][c] = ax[c][r]; // columns are forward / left / up
        }
    }
    tr = m[0][0] + m[1][1] + m[2][2];
    if (tr > 0) {
        s    = sqrt(tr + 1.0f) * 2.0f;
        q[3] = 0.25f * s;
        q[0] = (m[2][1] - m[1][2]) / s;
        q[1] = (m[0][2] - m[2][0]) / s;
        q[2] = (m[1][0] - m[0][1]) / s;
    } else if (m[0][0] > m[1][1] && m[0][0] > m[2][2]) {
        s    = sqrt(1.0f + m[0][0] - m[1][1] - m[2][2]) * 2.0f;
        q[3] = (m[2][1] - m[1][2]) / s;
        q[0] = 0.25f * s;
        q[1] = (m[0][1] + m[1][0]) / s;
        q[2] = (m[0][2] + m[2][0]) / s;
    } else if (m[1][1] > m[2][2]) {
        s    = sqrt(1.0f + m[1][1] - m[0][0] - m[2][2]) * 2.0f;
        q[3] = (m[0][2] - m[2][0]) / s;
        q[0] = (m[0][1] + m[1][0]) / s;
        q[1] = 0.25f * s;
        q[2] = (m[1][2] + m[2][1]) / s;
    } else {
        s    = sqrt(1.0f + m[2][2] - m[0][0] - m[1][1]) * 2.0f;
        q[3] = (m[1][0] - m[0][1]) / s;
        q[0] = (m[0][2] + m[2][0]) / s;
        q[1] = (m[1][2] + m[2][1]) / s;
        q[2] = 0.25f * s;
    }
}

static void DE_QuatToAngles(float q[4], vec3_t ang)
{
    float  x = q[0], y = q[1], z = q[2], w = q[3];
    float  m[3][3];
    vec3_t fwd, up, r0, u0, a0;
    float  len = sqrt(x * x + y * y + z * z + w * w);

    if (len < 1e-6f) {
        VectorClear(ang);
        return;
    }
    x /= len;
    y /= len;
    z /= len;
    w /= len;
    m[0][0] = 1 - 2 * (y * y + z * z);
    m[0][1] = 2 * (x * y - z * w);
    m[0][2] = 2 * (x * z + y * w);
    m[1][0] = 2 * (x * y + z * w);
    m[1][1] = 1 - 2 * (x * x + z * z);
    m[1][2] = 2 * (y * z - x * w);
    m[2][0] = 2 * (x * z - y * w);
    m[2][1] = 2 * (y * z + x * w);
    m[2][2] = 1 - 2 * (x * x + y * y);

    fwd[0] = m[0][0];
    fwd[1] = m[1][0];
    fwd[2] = m[2][0];
    up[0]  = m[0][2];
    up[1]  = m[1][2];
    up[2]  = m[2][2];
    vectoangles(fwd, ang);
    VectorSet(a0, ang[PITCH], ang[YAW], 0);
    AngleVectors(a0, NULL, r0, u0);
    ang[ROLL]  = RAD2DEG(atan2(DotProduct(up, r0), DotProduct(up, u0)));
    ang[PITCH] = AngleNormalize180(ang[PITCH]);
    ang[YAW]   = AngleNormalize180(ang[YAW]);
}

// cubic Hermite with time-scaled tangents
static float DE_Hermite(float p0, float m0, float p1, float m1, float u, float dt)
{
    float u2 = u * u, u3 = u2 * u;
    return (2 * u3 - 3 * u2 + 1) * p0 + (u3 - 2 * u2 + u) * dt * m0 + (-2 * u3 + 3 * u2) * p1 + (u3 - u2) * dt * m1;
}

static float DE_WpTime(int i)
{
    if (de.pathRealtime) {
        float seg = demo_pathSegTime->value > 0.1f ? demo_pathSegTime->value : 0.1f;
        return i * seg * 1000.0f;
    }
    return (float)de.wp[i].time;
}

// 8 channels: org xyz, quat xyzw, fov
static void DE_WpChannels(int i, float out[8], const float *prevQuat)
{
    float q[4];
    DE_AnglesToQuat(de.wp[i].ang, q);
    if (prevQuat && (q[0] * prevQuat[0] + q[1] * prevQuat[1] + q[2] * prevQuat[2] + q[3] * prevQuat[3]) < 0) {
        q[0] = -q[0];
        q[1] = -q[1];
        q[2] = -q[2];
        q[3] = -q[3];
    }
    out[0] = de.wp[i].org[0];
    out[1] = de.wp[i].org[1];
    out[2] = de.wp[i].org[2];
    out[3] = q[0];
    out[4] = q[1];
    out[5] = q[2];
    out[6] = q[3];
    out[7] = de.wp[i].fov;
}

static qboolean DE_EvalPath(float t, vec3_t org, vec3_t ang, float *fov)
{
    static float ch[DE_MAX_WP][8];
    float        m0[8], m1[8], res[8], q[4];
    int          i, k, n = de.numWp;
    float        t0, t1, u, dt;

    if (n <= 0) {
        return qfalse;
    }
    for (i = 0; i < n; i++) {
        DE_WpChannels(i, ch[i], i ? &ch[i - 1][3] : NULL);
    }
    if (n == 1 || t <= DE_WpTime(0)) {
        i = 0;
        memcpy(res, ch[0], sizeof(res));
    } else if (t >= DE_WpTime(n - 1)) {
        memcpy(res, ch[n - 1], sizeof(res));
    } else {
        for (i = 0; i < n - 2 && t >= DE_WpTime(i + 1); i++) {
        }
        t0 = DE_WpTime(i);
        t1 = DE_WpTime(i + 1);
        dt = t1 - t0;
        if (dt < 1.0f) {
            dt = 1.0f;
        }
        u = (t - t0) / dt;
        for (k = 0; k < 8; k++) {
            // zero velocity at the two ends (ease in / ease out), Catmull-Rom style tangents inside
            m0[k] = (i > 0) ? (ch[i + 1][k] - ch[i - 1][k]) / (DE_WpTime(i + 1) - DE_WpTime(i - 1) + 0.001f) : 0.0f;
            m1[k] = (i + 2 < n) ? (ch[i + 2][k] - ch[i][k]) / (DE_WpTime(i + 2) - DE_WpTime(i) + 0.001f) : 0.0f;
            res[k] = DE_Hermite(ch[i][k], m0[k], ch[i + 1][k], m1[k], u, dt);
        }
    }
    VectorCopy(res, org);
    q[0] = res[3];
    q[1] = res[4];
    q[2] = res[5];
    q[3] = res[6];
    DE_QuatToAngles(q, ang);
    *fov = res[7];
    return qtrue;
}

static void DE_DecidePathMode(void)
{
    int i;
    de.pathRealtime = qfalse;
    for (i = 1; i < de.numWp; i++) {
        if (de.wp[i].time - de.wp[i - 1].time < 50) {
            de.pathRealtime = qtrue;
            return;
        }
    }
}

/*
==================
name aliases (inside the configstrings)
==================
*/
static void DE_CleanName(const char *in, char *out, int size)
{
    int j = 0;
    for (; *in && j < size - 1; in++) {
        if (*in == '\\' || *in == '"' || *in == ';' || (unsigned char)*in < 32) {
            continue;
        }
        out[j++] = *in;
    }
    out[j] = 0;
}

// incoming configstring: remember the recorded name, return the string to store (alias applied)
const char *CL_DemoEd_FilterConfigstring(int index, const char *s)
{
    static char buf[MAX_INFO_STRING];
    int         slot;

    if (!clc.demoplaying || index < CS_PLAYERS || index >= CS_PLAYERS + MAX_CLIENTS) {
        return s;
    }
    slot = index - CS_PLAYERS;
    if (s[0]) {
        Q_strncpyz(de.origName[slot], Info_ValueForKey(s, "name"), sizeof(de.origName[slot]));
    } else {
        de.origName[slot][0] = 0;
    }
    if (!de.alias[slot][0] || !s[0] || strlen(s) >= sizeof(buf) - MAX_NAME_LENGTH) {
        return s;
    }
    Q_strncpyz(buf, s, sizeof(buf));
    Info_SetValueForKey(buf, "name", de.alias[slot]);
    return buf;
}

static const char *DE_CS(int index)
{
    return cl.gameState.stringData + cl.gameState.stringOffsets[index];
}

// re-store one player slot's configstring with its alias (or the recorded name)
static void DE_ApplyAlias(int slot, qboolean notifyCgame)
{
    char        buf[MAX_INFO_STRING];
    int         index = CS_PLAYERS + slot;
    const char *cur   = DE_CS(index);

    if (!cur[0] || strlen(cur) >= sizeof(buf) - MAX_NAME_LENGTH) {
        return;
    }
    Q_strncpyz(buf, cur, sizeof(buf));
    Info_SetValueForKey(buf, "name", de.alias[slot][0] ? de.alias[slot] : de.origName[slot]);
    if (strcmp(buf, cur)) {
        // CL_ConfigstringModified runs the filter, which would overwrite origName with the alias: keep it
        char keep[MAX_NAME_LENGTH];
        Q_strncpyz(keep, de.origName[slot], sizeof(keep));
        CL_ConfigstringModified(index, buf);
        Q_strncpyz(de.origName[slot], keep, sizeof(de.origName[slot]));
        if (notifyCgame && cls.cgameStarted) {
            Cmd_ExecuteString(va("demoed_cs %d\n", index));
        }
    }
}

// the gamestate strings are raw recorded ones: read the names, then apply aliases
static void DE_ApplyAllAliasesRaw(qboolean notifyCgame)
{
    int i;
    for (i = 0; i < MAX_CLIENTS; i++) {
        const char *cur = DE_CS(CS_PLAYERS + i);
        if (cur[0]) {
            Q_strncpyz(de.origName[i], Info_ValueForKey(cur, "name"), sizeof(de.origName[i]));
        } else {
            de.origName[i][0] = 0;
        }
        if (de.alias[i][0]) {
            DE_ApplyAlias(i, notifyCgame);
        }
    }
}

/*
==================
files: demos/cameras/<demo>.cam and .names
==================
*/
static void DE_SetBaseName(void)
{
    char *p;
    Q_strncpyz(de.baseName, clc.demoName, sizeof(de.baseName));
    p = strstr(de.baseName, ".dm_");
    if (!p) {
        p = strstr(de.baseName, ".DM_");
    }
    if (p) {
        *p = 0;
    }
    for (p = de.baseName; *p; p++) {
        if (*p == '\\') {
            *p = '/';
        }
    }
}

static void DE_SaveCam(void)
{
    fileHandle_t f;
    int          i;

    if (!de.baseName[0]) {
        return;
    }
    f = FS_FOpenFileWrite(va("demos/cameras/%s.cam", de.baseName));
    if (!f) {
        Com_Printf("Demo editor: could not write demos/cameras/%s.cam\n", de.baseName);
        return;
    }
    FS_Printf(f, "// HZM demo camera path v1 - wp <time_ms> <x> <y> <z> <pitch> <yaw> <roll> <fov>\n");
    FS_Printf(f, "fov %g\n", demo_fov->value);
    for (i = 0; i < de.numWp; i++) {
        deWaypoint_t *w = &de.wp[i];
        FS_Printf(
            f, "wp %d %.2f %.2f %.2f %.3f %.3f %.3f %.2f\n", w->time, w->org[0], w->org[1], w->org[2],
            w->ang[0], w->ang[1], w->ang[2], w->fov
        );
    }
    FS_FCloseFile(f);
}

static void DE_LoadCam(void)
{
    char *buf;
    char *p, *tok;
    int   len;

    de.numWp = 0;
    if (!de.baseName[0]) {
        return;
    }
    len = FS_ReadFile(va("demos/cameras/%s.cam", de.baseName), (void **)&buf);
    if (len <= 0 || !buf) {
        return;
    }
    p = buf;
    while (1) {
        tok = COM_ParseExt(&p, qtrue);
        if (!tok[0]) {
            break;
        }
        if (!Q_stricmp(tok, "fov")) {
            Cvar_Set("demo_fov", COM_ParseExt(&p, qfalse));
        } else if (!Q_stricmp(tok, "wp") && de.numWp < DE_MAX_WP) {
            deWaypoint_t *w = &de.wp[de.numWp];
            int           k;
            w->time         = atoi(COM_ParseExt(&p, qfalse)); // display ms (v1 files: identical for a one-level demo)
            for (k = 0; k < 3; k++) {
                w->org[k] = atof(COM_ParseExt(&p, qfalse));
            }
            for (k = 0; k < 3; k++) {
                w->ang[k] = atof(COM_ParseExt(&p, qfalse));
            }
            tok    = COM_ParseExt(&p, qfalse);
            w->fov = tok[0] ? atof(tok) : demo_fov->value;
            if (w->fov < 10 || w->fov > 160) {
                w->fov = 90;
            }
            de.numWp++;
        }
        SkipRestOfLine(&p);
    }
    FS_FreeFile(buf);
    DE_DecidePathMode();
    if (de.numWp) {
        Com_Printf("Demo editor: loaded %d waypoint(s) from demos/cameras/%s.cam\n", de.numWp, de.baseName);
    }
}

static void DE_SaveNames(void)
{
    fileHandle_t f;
    int          i, n = 0;

    if (!de.baseName[0]) {
        return;
    }
    for (i = 0; i < MAX_CLIENTS; i++) {
        n += de.alias[i][0] ? 1 : 0;
    }
    f = FS_FOpenFileWrite(va("demos/cameras/%s.names", de.baseName));
    if (!f) {
        Com_Printf("Demo editor: could not write demos/cameras/%s.names\n", de.baseName);
        return;
    }
    FS_Printf(f, "// HZM demo name aliases v1 - <slot> \"<name>\"\n");
    for (i = 0; i < MAX_CLIENTS; i++) {
        if (de.alias[i][0]) {
            FS_Printf(f, "%d \"%s\"\n", i, de.alias[i]);
        }
    }
    FS_FCloseFile(f);
    (void)n;
}

static void DE_LoadNames(void)
{
    char *buf, *p, *tok;
    int   len, slot;

    memset(de.alias, 0, sizeof(de.alias));
    if (!de.baseName[0]) {
        return;
    }
    len = FS_ReadFile(va("demos/cameras/%s.names", de.baseName), (void **)&buf);
    if (len <= 0 || !buf) {
        return;
    }
    p = buf;
    while (1) {
        tok = COM_ParseExt(&p, qtrue);
        if (!tok[0]) {
            break;
        }
        slot = atoi(tok);
        tok  = COM_ParseExt(&p, qfalse);
        if (slot >= 0 && slot < MAX_CLIENTS && tok[0]) {
            DE_CleanName(tok, de.alias[slot], sizeof(de.alias[slot]));
        }
        SkipRestOfLine(&p);
    }
    FS_FreeFile(buf);
}

/*
==================
lifecycle
==================
*/
void CL_DemoEd_Reset(void)
{
    // keep the rewind buffers allocated, drop everything per-demo
    memset(&de, 0, sizeof(de));
    de.speed = 1.0f;
    TL_ResetInput();
    if (demo_cam) {
        Cvar_Set("demo_cam", "");
        Cvar_Set("demo_status", "");
        Cvar_Set("demo_playing", "0");
        if (s_hudMode == 1) {
            s_hudMode = 0; // names only is a demo mode
            Cvar_Set("demo_hud", "0");
        }
    }
}

// called at the end of CL_ParseGamestate: the file now sits on the first message after it
void CL_DemoEd_GamestateParsed(void)
{
    if (!clc.demoplaying || !clc.demofile) {
        return;
    }
    Cvar_Set("demo_playing", "1");
    if (de.gamestates++ > 0) {
        // a level change inside the demo: a new segment. The clock carries on from the old level's last
        // snapshot; the new level's first snapshot re-anchors it (CL_DemoEd_SetTime)
        if (de.started) {
            de.segBase += (int)(de.lastSnapTime - de.firstSnapTime) + 50; // one snapshot between levels (DE_SegStartGap)
        }
        de.segIndex++;
        de.started    = qfalse;
        de.atEnd      = qfalse;
        de.pathPlaying = qfalse;
        de.freecam     = qfalse; // the old level's camera spot means nothing in the new one
        Com_Printf("Demo editor: level %d of this demo starts at %.1f s\n", de.segIndex + 1, de.segBase / 1000.0f);
    } else {
        DE_SetBaseName();
    }
    {
        // this level's rewind point
        de.rewindOffset = FS_FTell(clc.demofile);
        de.rewindCmdSeq = clc.serverCommandSequence;
        de_rewindGS     = cl.gameState; // raw recorded strings
        memcpy(de_rewindBaselines, cl.entityBaselines, sizeof(de_rewindBaselines));
        de.haveRewind = qtrue;
        if (de.gamestates == 1) {
            DE_LoadNames();
        }
    }
    DE_ApplyAllAliasesRaw(qfalse); // the cgame is not up yet: it reads them in CG_Init
}

/*
==================
seeking
==================
*/
static void DE_ApplySeekCommands(int from)
{
    int         n;
    const char *s;

    for (n = from + 1; n <= clc.serverCommandSequence; n++) {
        if (n <= clc.serverCommandSequence - MAX_RELIABLE_COMMANDS) {
            continue;
        }
        s = clc.serverCommands[n & (MAX_RELIABLE_COMMANDS - 1)];
        if (!strncmp(s, "cs ", 3) || !strncmp(s, "bcs", 3)) {
            CL_GetServerCommand(n, qfalse); // stores the configstring (bcs pieces reassembled there)
        }
        clc.lastExecutedServerCommand = n;
    }
}

static void DE_SeekTo(int target);

// the gap the clock puts between two levels (one snapshot)
static int DE_SegStartGap(void)
{
    return 50;
}

static void DE_Pend(int displayMs, qboolean restart)
{
    s_pend.active    = qtrue;
    s_pend.displayMs = displayMs < 0 ? 0 : displayMs;
    s_pend.freecam   = qfalse; // a waiting seek always lands in another level: the old camera spot means nothing there
    s_pend.paused    = de.paused;
    s_pend.speed     = de.speed > 0 ? de.speed : 1.0f;
    VectorCopy(de.camOrg, s_pend.camOrg);
    VectorCopy(de.camAng, s_pend.camAng);
    Q_strncpyz(s_pend.demo, clc.demoName, sizeof(s_pend.demo));
    if (restart) {
        // an earlier level: play the demo again from its start and seek once that level is up
        Com_Printf("Demo editor: %.1f s is in an earlier level - reloading the demo\n", s_pend.displayMs / 1000.0f);
        // disconnect first: it frees the loaded map (CL_FlushMemory). "demo" straight over a playing demo does
        // not, and the renderer then refuses the first level's world ("redundantly load world map")
        Cbuf_ExecuteText(EXEC_APPEND, va("disconnect\ndemo %s\n", s_pend.demo));
    }
}

// every seek goes through here (display ms, across levels)
static void DE_SeekDisplay(int ms)
{
    if (ms < 0) {
        ms = 0;
    }
    if (ms < de.segBase) {
        DE_Pend(ms, qtrue);
        return;
    }
    DE_SeekTo(de.firstSnapTime + (ms - de.segBase));
}

static void DE_SeekTo(int target)
{
    static gameState_t before;
    int                applied, i;
    qboolean           rewound = qfalse;
    int                targetD;
    int                t0      = Sys_Milliseconds();

    if (target < de.firstSnapTime + 1) {
        target = de.firstSnapTime + 1;
    }
    before = cl.gameState;
    targetD = de.segBase + (target - de.firstSnapTime); // display ms, for a seek a level change interrupts

    if (target < cl.snap.serverTime - 1 || target < de.demoTime) {
        if (!de.haveRewind) {
            Com_Printf("Demo editor: this demo cannot be rewound.\n");
            return;
        }
        // back to just after the gamestate: the next message is the first, non-delta snapshot
        FS_Seek(clc.demofile, de.rewindOffset, FS_SEEK_SET);
        cl.gameState = de_rewindGS;
        memcpy(cl.entityBaselines, de_rewindBaselines, sizeof(cl.entityBaselines));
        clc.serverCommandSequence     = de.rewindCmdSeq;
        clc.lastExecutedServerCommand = de.rewindCmdSeq;
        for (i = 0; i < PACKET_BACKUP; i++) {
            cl.snapshots[i].valid = qfalse;
        }
        cl.snap.valid = qfalse;
        DE_ApplyAllAliasesRaw(qfalse);
        rewound  = qtrue;
        de.atEnd = qfalse;
    }

    applied = clc.serverCommandSequence;
    while (!cl.snap.valid || cl.snap.serverTime <= target) {
        int segWas = de.segIndex;
        if (cl.snap.valid && cl.snap.serverTime > de.lastSnapTime) {
            de.lastSnapTime = cl.snap.serverTime;
        }
        CL_ReadDemoMessage();
        if (clc.demoplaying && de.segIndex != segWas) {
            // a new level: it loads over the next frames; the rest of the seek runs once it is up
            DE_Pend(targetD, qfalse);
            return;
        }
        if (clc.state != CA_ACTIVE || !clc.demoplaying) {
            return; // ran off the end: the demo finished
        }
        DE_ApplySeekCommands(applied);
        if (de.atEnd) {
            target = cl.snap.serverTime - 1; // past the end: hold the last frame
            break;
        }
        applied = clc.serverCommandSequence;
    }
    clc.lastExecutedServerCommand = clc.serverCommandSequence;

    de.demoTime           = target;
    cl.serverTime         = target;
    cl.oldServerTime      = target;
    cl.oldFrameServerTime = cl.snap.serverTime;
    cl.newSnapshots       = qfalse;
    de.lastSnapTime       = cl.snap.serverTime;

    S_StopAllSounds(qfalse);

    // tell the cgame: changed configstrings first, then the snapshot pipeline
    for (i = 0; i < MAX_CONFIGSTRINGS; i++) {
        const char *a = before.stringData + before.stringOffsets[i];
        const char *b = cl.gameState.stringData + cl.gameState.stringOffsets[i];
        if (strcmp(a, b)) {
            Cmd_ExecuteString(va("demoed_cs %d\n", i));
        }
    }
    Cmd_ExecuteString(va("demoed_reset %d %d\n", cl.snap.messageNum, clc.serverCommandSequence));

    if (developer && developer->integer) {
        Com_Printf(
            "^~^~^ DEMOED seek t=%.1f rewound=%d snap=%d took=%dms\n", DE_Clock(), rewound, cl.snap.messageNum,
            Sys_Milliseconds() - t0
        );
    }
}

/*
==================
per frame: the clock, the camera, the published state
==================
*/
static void DE_MoveFreecam(float dtSec)
{
    vec3_t f, r;
    float  spd = demo_camSpeed->value;
    float  fm = 0, rm = 0, um = 0;

    if (Key_GetCatcher() != 0) {
        return;
    }
    if (keys['w'].down || keys['W'].down) {
        fm += 1;
    }
    if (keys['s'].down || keys['S'].down) {
        fm -= 1;
    }
    if (keys['d'].down || keys['D'].down) {
        rm += 1;
    }
    if (keys['a'].down || keys['A'].down) {
        rm -= 1;
    }
    if (keys[K_SPACE].down) {
        um += 1;
    }
    if (keys[K_CTRL].down) {
        um -= 1;
    }
    if (keys[K_SHIFT].down) {
        spd *= 3.0f;
    }
    if (keys[K_ALT].down) {
        spd *= 0.25f;
    }
    AngleVectors(de.camAng, f, r, NULL);
    VectorMA(de.camOrg, fm * spd * dtSec, f, de.camOrg);
    VectorMA(de.camOrg, rm * spd * dtSec, r, de.camOrg);
    de.camOrg[2] += um * spd * dtSec;
}

static float s_pathFov = 90.0f;

static void DE_Publish(void)
{
    char buf[256];
    char tb[32];

    if (de.freecam) {
        Com_sprintf(
            buf, sizeof(buf), "%.2f %.2f %.2f %.3f %.3f %.3f %.2f", de.camOrg[0], de.camOrg[1], de.camOrg[2],
            de.camAng[0], de.camAng[1], de.camAng[2], de.pathPlaying ? s_pathFov : demo_fov->value
        );
    } else {
        buf[0] = 0;
    }
    if (strcmp(buf, demo_cam->string)) {
        Cvar_Set("demo_cam", buf);
    }

    DE_FmtTime(DE_Clock(), tb, sizeof(tb));
    Com_sprintf(
        buf, sizeof(buf), "%s  %s%gx%s%s  wp %d%s", tb, de.atEnd ? "END  " : (de.paused ? "PAUSED  " : ""), de.speed,
        de.freecam ? "  FREECAM" : "", de.pathPlaying ? "  PATH" : "", de.numWp,
        de.pathRealtime && de.numWp > 1 ? " (timed)" : ""
    );
    if (strcmp(buf, demo_status->string)) {
        Cvar_Set("demo_status", buf);
    }
    if (demo_hud->integer != s_hudMode) {
        Cvar_Set("demo_hud", va("%d", s_hudMode));
    }
}

void CL_DemoEd_SetTime(void)
{
    int   now = Sys_Milliseconds();
    int   dt;
    float dtSec;

    if (!de.started) {
        de.started       = qtrue;
        de.demoTime      = cl.snap.serverTime;
        de.firstSnapTime = cl.snap.serverTime;
        de.lastRealMs    = now;
        de.speed         = de.speed > 0 ? de.speed : 1.0f;
        DE_LoadCam(); // waypoint times are display ms
        if (de.segIndex == 0 && !s_pend.active && Cvar_VariableIntegerValue("demo_startPaused")) {
            de.paused = qtrue; // hold on the first frame (test scripts: the load time varies)
        }
        de.lastSnapTime  = cl.snap.serverTime;
        if (s_pend.active && !Q_stricmp(s_pend.demo, clc.demoName)) {
            s_pend.active = qfalse;
            de.freecam    = s_pend.freecam;
            de.paused     = s_pend.paused;
            de.speed      = s_pend.speed;
            VectorCopy(s_pend.camOrg, de.camOrg);
            VectorCopy(s_pend.camAng, de.camAng);
            if (s_pend.displayMs > de.segBase) {
                DE_SeekDisplay(s_pend.displayMs);
            }
        }
    }
    dt = now - de.lastRealMs;
    de.lastRealMs = now;
    if (dt < 0) {
        dt = 0;
    } else if (dt > 250) {
        dt = 250;
    }
    dtSec = dt / 1000.0f;

    if (de.atEnd && !de.paused) {
        // play pressed on the last frame: start over (from the first level)
        de.paused = qfalse;
        DE_SeekDisplay(0);
    }
    if (!de.paused && !de.atEnd) {
        de.demoTime += dt * de.speed;
        if (de.pathPlaying && de.pathRealtime) {
            de.pathClock += dt * de.speed;
        }
    }

    // camera path drives the free camera
    if (de.pathPlaying) {
        float  t = de.pathRealtime ? (float)de.pathClock : (float)DE_DisplayMs();
        float  tEnd = DE_WpTime(de.numWp - 1);
        if (de.numWp <= 0) {
            de.pathPlaying = qfalse;
        } else {
            DE_EvalPath(t, de.camOrg, de.camAng, &s_pathFov);
            de.freecam = qtrue;
            if (t >= tEnd) {
                de.pathPlaying = qfalse;
                Com_Printf("Demo editor: path finished.\n");
            }
        }
    } else if (de.freecam) {
        DE_MoveFreecam(dtSec);
    }

    cl.serverTime = (int)de.demoTime;
    if (cl.serverTime < cl.oldServerTime) {
        cl.serverTime = cl.oldServerTime;
    }
    cl.serverTimeDelta = cl.serverTime - cls.realtime;
    if (cl.snap.serverTime > de.lastSnapTime) {
        de.lastSnapTime = cl.snap.serverTime;
    }

    TL_Frame(); // the timeline: its scan of the file, and a scrub in progress
    DE_Publish();
}

// the demo file ran out: with the editor on, hold the last frame (paused) instead of leaving the demo
qboolean CL_DemoEd_HoldAtEnd(void)
{
    if (!CL_DemoEd_Enabled() || !de.started || !de.haveRewind || clc.state != CA_ACTIVE || !cl.snap.valid) {
        return qfalse;
    }
    if (!de.atEnd) {
        Com_Printf("Demo editor: end of the demo - holding the last frame. Seek back, play to restart, or Esc to leave.\n");
    }
    de.atEnd      = qtrue;
    de.paused     = qtrue;
    de.pathPlaying = qfalse;
    de.demoTime   = cl.snap.serverTime - 1;
    cl.serverTime = (int)de.demoTime; // ends CL_SetCGameTime's read loop
    return qtrue;
}

/*
==================
input
==================
*/
qboolean CL_DemoEd_FreecamActive(void)
{
    return (CL_DemoEd_Enabled() && de.freecam && clc.state == CA_ACTIVE) ? qtrue : qfalse;
}

// mouse deltas already carry the sensitivity chain (NOT the cgame zoom scale)
qboolean CL_DemoEd_MouseLook(float mx, float my)
{
    if (!CL_DemoEd_FreecamActive()) {
        return qfalse;
    }
    if (de.pathPlaying) {
        return qtrue; // the path owns the camera
    }
    de.camAng[YAW] -= m_yaw->value * mx;
    de.camAng[YAW] = AngleNormalize180(de.camAng[YAW]);
    de.camAng[PITCH] += m_pitch->value * my;
    if (de.camAng[PITCH] > 89.0f) {
        de.camAng[PITCH] = 89.0f;
    } else if (de.camAng[PITCH] < -89.0f) {
        de.camAng[PITCH] = -89.0f;
    }
    de.camAng[ROLL] = 0;
    return qtrue;
}

static void DE_ToggleFreecam(void)
{
    if (!de.freecam) {
        DE_PlayerEye(de.camOrg, de.camAng);
        de.camAng[ROLL] = 0;
        de.freecam      = qtrue;
    } else {
        de.freecam     = qfalse;
        de.pathPlaying = qfalse;
    }
    Com_Printf("Demo editor: free camera %s\n", de.freecam ? "ON" : "OFF");
}

static void DE_SetSpeed(float s)
{
    if (s < 0.05f) {
        s = 0.05f;
    } else if (s > 16.0f) {
        s = 16.0f;
    }
    de.speed = s;
    Com_Printf("Demo editor: speed %gx\n", s);
}

static void DE_Step(int n)
{
    de.paused = qtrue;
    while (n-- > 0) {
        // the snapshot ahead of the shown time becomes the shown one
        if (cl.snap.valid && cl.snap.serverTime > de.demoTime) {
            de.demoTime = cl.snap.serverTime;
        } else {
            de.demoTime += 50;
        }
        if (n > 0) {
            CL_ReadDemoMessage();
            if (clc.state != CA_ACTIVE) {
                return;
            }
        }
    }
}

static void DE_AddWaypoint(void)
{
    deWaypoint_t w;
    int          i;

    if (de.numWp >= DE_MAX_WP) {
        Com_Printf("Demo editor: waypoint limit (%d) reached\n", DE_MAX_WP);
        return;
    }
    DE_CurrentCamera(w.org, w.ang, &w.fov);
    w.time = (int)DE_DisplayMs();
    // keep the list in time order; a waypoint at the same time goes after the ones already there
    for (i = de.numWp; i > 0 && de.wp[i - 1].time > w.time; i--) {
        de.wp[i] = de.wp[i - 1];
    }
    de.wp[i] = w;
    de.numWp++;
    DE_DecidePathMode();
    DE_SaveCam();
    Com_Printf("Demo editor: waypoint %d of %d added at %.1f s\n", i + 1, de.numWp, DE_Clock());
}

static void DE_PathPlay(void)
{
    if (de.numWp <= 0) {
        Com_Printf("Demo editor: no waypoints - press KP Enter to add some.\n");
        return;
    }
    DE_DecidePathMode();
    de.freecam = qtrue;
    if (!de.pathRealtime && de.numWp > 1) {
        DE_SeekDisplay(de.wp[0].time);
    }
    de.pathClock   = 0;
    de.pathPlaying = qtrue;
    de.paused      = qfalse;
    Com_Printf("Demo editor: playing path (%d waypoints, %s)\n", de.numWp, de.pathRealtime ? "timed" : "demo time");
}

static void DE_PathStop(void)
{
    if (de.pathPlaying) {
        de.pathPlaying = qfalse;
        Com_Printf("Demo editor: path stopped\n");
    }
}

static void DE_SetHud(int m)
{
    static const char *names[] = {"full HUD", "player names only", "no HUD"};
    if (m < 0 || m > 2) {
        m = 0;
    }
    s_hudMode = m;
    Cvar_Set("demo_hud", va("%d", s_hudMode));
    Com_Printf("HUD: %s\n", names[s_hudMode]);
}

// demos cycle full -> names only -> none; live play toggles full <-> none
static void DE_CycleHud(void)
{
    if (CL_DemoEd_Enabled()) {
        DE_SetHud((s_hudMode + 1) % 3);
    } else {
        DE_SetHud(s_hudMode ? 0 : 2);
    }
}

static void DE_SetFilter(int n)
{
    n = ((n % (HZM_NUM_FILTERS + 1)) + HZM_NUM_FILTERS + 1) % (HZM_NUM_FILTERS + 1);
    Cvar_Set("r_ppFilter", va("%d", n));
    Com_Printf("Filter: %s (%d/%d)\n", hzmFilterNames[n], n, HZM_NUM_FILTERS);
}

// keypad controls; returns qtrue when the key was used (so no bind fires)
qboolean CL_DemoEd_KeyEvent(int key, qboolean down)
{
    // HZM coop - live play too (also while recording with F9): KP Minus = clean HUD, KP* / KP/ = next /
    // previous filter. Only when the key is unbound, so a player's own keypad binds win.
    if ((key == K_KP_MINUS || key == K_KP_STAR || key == K_KP_SLASH) && clc.state == CA_ACTIVE
        && Key_GetCatcher() == 0 && !CL_DemoEd_Enabled()) {
        const char *b = Key_GetBinding(key);
        if (b && b[0]) {
            return qfalse;
        }
        if (down) {
            if (key == K_KP_MINUS) {
                DE_CycleHud();
            } else {
                DE_SetFilter(r_ppFilterCv->integer + (key == K_KP_STAR ? 1 : -1));
            }
        }
        return qtrue;
    }
    if (key == K_MOUSE2 && !down) {
        TL_MouseKey(key, down); // a release always clears the hold, whatever is up now
    }
    if (!CL_DemoEd_Enabled() || clc.state != CA_ACTIVE || !de.started || Key_GetCatcher() != 0) {
        return qfalse;
    }
    if (key == K_MOUSE1 || key == K_MOUSE2 || key == K_MWHEELUP || key == K_MWHEELDOWN) {
        return TL_MouseKey(key, down); // the timeline
    }
    switch (key) {
    case K_KP_INS:
    case K_KP_LEFTARROW:
    case K_KP_RIGHTARROW:
    case K_KP_5:
    case K_DEL:
    case K_KP_DEL:
    case K_KP_ENTER:
    case K_KP_PLUS:
    case K_KP_MINUS:
    case K_KP_END:
    case K_KP_DOWNARROW:
    case K_KP_PGDN:
    case K_KP_HOME:
    case K_KP_PGUP:
    case K_KP_STAR:
    case K_KP_SLASH:
        break;
    default:
        return qfalse;
    }
    if (!down) {
        return qtrue;
    }
    switch (key) {
    case K_KP_INS:
        DE_ToggleFreecam();
        break;
    case K_KP_LEFTARROW:
        DE_SeekDisplay((int)DE_DisplayMs() - DE_SEEK_STEP_MS);
        break;
    case K_KP_RIGHTARROW:
        DE_SeekDisplay((int)DE_DisplayMs() + DE_SEEK_STEP_MS);
        break;
    case K_KP_5:
        DE_Step(1);
        break;
    case K_DEL:
    case K_KP_DEL:
        de.paused = !de.paused;
        Com_Printf("Demo editor: %s\n", de.paused ? "paused" : "playing");
        break;
    case K_KP_ENTER:
        DE_AddWaypoint();
        break;
    case K_KP_PLUS:
        if (de.pathPlaying) {
            DE_PathStop();
        } else {
            DE_PathPlay();
        }
        break;
    case K_KP_MINUS:
        DE_CycleHud();
        break;
    case K_KP_END:
        DE_SetSpeed(0.5f);
        break;
    case K_KP_DOWNARROW:
        DE_SetSpeed(1.0f);
        break;
    case K_KP_PGDN:
        DE_SetSpeed(2.0f);
        break;
    case K_KP_HOME:
        DE_SetSpeed(4.0f);
        break;
    case K_KP_PGUP:
        DE_SetSpeed(5.0f);
        break;
    case K_KP_STAR:
        DE_SetFilter(r_ppFilterCv->integer + 1);
        break;
    case K_KP_SLASH:
        DE_SetFilter(r_ppFilterCv->integer - 1);
        break;
    }
    return qtrue;
}

int CL_DemoEd_HudMode(void)
{
    return (clc.state == CA_ACTIVE) ? s_hudMode : 0;
}

/*
==================
console commands
==================
*/
static float DE_ParseTime(const char *s)
{
    const char *c = strchr(s, ':');
    if (c) {
        return atoi(s) * 60.0f + (float)atof(c + 1);
    }
    return (float)atof(s);
}

static void DE_Seek_f(void)
{
    if (!DE_Ready()) {
        return;
    }
    if (Cmd_Argc() < 2) {
        Com_Printf("demo_seek <seconds | m:ss>  - jump to a time from the start of the demo\n");
        return;
    }
    DE_SeekDisplay((int)(DE_ParseTime(Cmd_Argv(1)) * 1000.0f));
}

static void DE_SeekBy_f(void)
{
    if (!DE_Ready()) {
        return;
    }
    if (Cmd_Argc() < 2) {
        Com_Printf("demo_seekby <+/-seconds>\n");
        return;
    }
    DE_SeekDisplay((int)(DE_DisplayMs() + atof(Cmd_Argv(1)) * 1000.0));
}

static void DE_Step_f(void)
{
    if (!DE_Ready()) {
        return;
    }
    DE_Step(Cmd_Argc() > 1 ? atoi(Cmd_Argv(1)) : 1);
}

static void DE_Pause_f(void)
{
    if (!DE_Ready()) {
        return;
    }
    de.paused = (Cmd_Argc() > 1) ? (atoi(Cmd_Argv(1)) ? qtrue : qfalse) : !de.paused;
    Com_Printf("Demo editor: %s\n", de.paused ? "paused" : "playing");
}

static void DE_Speed_f(void)
{
    if (!DE_Ready()) {
        return;
    }
    if (Cmd_Argc() < 2) {
        Com_Printf("demo_speed <x>  (now %gx)\n", de.speed);
        return;
    }
    DE_SetSpeed(atof(Cmd_Argv(1)));
}

static void DE_Camera_f(void)
{
    const char *a = Cmd_Argc() > 1 ? Cmd_Argv(1) : "toggle";

    if (!DE_Ready()) {
        return;
    }
    if (!Q_stricmp(a, "toggle")) {
        DE_ToggleFreecam();
    } else if (!Q_stricmp(a, "on") || !strcmp(a, "1")) {
        if (!de.freecam) {
            DE_ToggleFreecam();
        }
    } else if (!Q_stricmp(a, "off") || !strcmp(a, "0")) {
        if (de.freecam) {
            DE_ToggleFreecam();
        }
    } else if (!Q_stricmp(a, "fov") && Cmd_Argc() > 2) {
        Cvar_Set("demo_fov", Cmd_Argv(2));
    } else if (!Q_stricmp(a, "speed") && Cmd_Argc() > 2) {
        Cvar_Set("demo_camSpeed", Cmd_Argv(2));
    } else if (!Q_stricmp(a, "save")) {
        DE_SaveCam();
        Com_Printf("Demo editor: saved demos/cameras/%s.cam\n", de.baseName);
    } else if (!Q_stricmp(a, "load")) {
        DE_LoadCam();
    } else if (!Q_stricmp(a, "set") && Cmd_Argc() >= 8) {
        int k;
        for (k = 0; k < 3; k++) {
            de.camOrg[k] = atof(Cmd_Argv(2 + k));
            de.camAng[k] = atof(Cmd_Argv(5 + k));
        }
        de.freecam = qtrue;
    } else if (!Q_stricmp(a, "chase")) {
        // put the camera around a player, looking at him: chase [dist] [yaw offset] [height] [slot]
        // (no slot = the recorded player; any other slot is found among this snapshot's entities)
        float  dist = Cmd_Argc() > 2 ? atof(Cmd_Argv(2)) : 160.0f;
        float  yo   = Cmd_Argc() > 3 ? atof(Cmd_Argv(3)) : 0.0f;
        float  hgt  = Cmd_Argc() > 4 ? atof(Cmd_Argv(4)) : 60.0f;
        vec3_t fwd, tgt, d, ya;

        float  baseYaw = cl.snap.ps.viewangles[YAW];
        int    slot    = Cmd_Argc() > 5 ? atoi(Cmd_Argv(5)) : cl.snap.ps.clientNum;

        VectorCopy(cl.snap.ps.origin, tgt);
        if (slot != cl.snap.ps.clientNum) {
            int k, found = 0;
            for (k = 0; k < cl.snap.numEntities; k++) {
                entityState_t *es = &cl.parseEntities[(cl.snap.parseEntitiesNum + k) & (MAX_PARSE_ENTITIES - 1)];
                if (es->number == slot) {
                    VectorCopy(es->origin, tgt);
                    baseYaw = es->angles[YAW];
                    found   = 1;
                    break;
                }
            }
            if (!found) {
                Com_Printf("Demo editor: player %d is not in view of this snapshot\n", slot);
                return;
            }
        }
        VectorSet(ya, 0, baseYaw + yo, 0);
        AngleVectors(ya, fwd, NULL, NULL);
        tgt[2] += 44.0f;
        VectorMA(tgt, -dist, fwd, de.camOrg);
        de.camOrg[2] += hgt - 44.0f;
        VectorSubtract(tgt, de.camOrg, d);
        vectoangles(d, de.camAng);
        de.camAng[PITCH] = AngleNormalize180(de.camAng[PITCH]);
        de.camAng[ROLL]  = 0;
        de.freecam       = qtrue;
    } else if (!Q_stricmp(a, "where")) {
        Com_Printf(
            "camera %.1f %.1f %.1f  angles %.1f %.1f %.1f  fov %g\n", de.camOrg[0], de.camOrg[1], de.camOrg[2],
            de.camAng[0], de.camAng[1], de.camAng[2], demo_fov->value
        );
    } else {
        Com_Printf("demo_camera [toggle|on|off|fov <deg>|speed <u/s>|save|load|where|chase [dist] [yaw] [height] [slot]|set x y z pitch yaw roll]\n");
    }
}

static int DE_NearestWp(void)
{
    int    i, best = -1;
    float  bd = 1e30f;
    vec3_t o, a, d;
    float  f;

    DE_CurrentCamera(o, a, &f);
    for (i = 0; i < de.numWp; i++) {
        VectorSubtract(de.wp[i].org, o, d);
        if (VectorLength(d) < bd) {
            bd   = VectorLength(d);
            best = i;
        }
    }
    return best;
}

static void DE_Waypoint_f(void)
{
    const char *a = Cmd_Argc() > 1 ? Cmd_Argv(1) : "add";
    int         i, n;
    char        tb[32];

    if (!DE_Ready()) {
        return;
    }
    n = (Cmd_Argc() > 2) ? atoi(Cmd_Argv(2)) - 1 : -1;
    if (!Q_stricmp(a, "add")) {
        DE_AddWaypoint();
    } else if (!Q_stricmp(a, "update")) {
        if (n < 0) {
            n = DE_NearestWp();
        }
        if (n < 0 || n >= de.numWp) {
            Com_Printf("Demo editor: no such waypoint\n");
            return;
        }
        DE_CurrentCamera(de.wp[n].org, de.wp[n].ang, &de.wp[n].fov);
        if (Cmd_Argc() > 3 && !Q_stricmp(Cmd_Argv(3), "retime")) {
            de.wp[n].time = (int)DE_DisplayMs();
        }
        // re-sort after a time change
        {
            deWaypoint_t w = de.wp[n];
            for (i = n; i < de.numWp - 1; i++) {
                de.wp[i] = de.wp[i + 1];
            }
            de.numWp--;
            for (i = de.numWp; i > 0 && de.wp[i - 1].time > w.time; i--) {
                de.wp[i] = de.wp[i - 1];
            }
            de.wp[i] = w;
            de.numWp++;
            Com_Printf("Demo editor: waypoint updated (now %d of %d)\n", i + 1, de.numWp);
        }
        DE_DecidePathMode();
        DE_SaveCam();
    } else if (!Q_stricmp(a, "delete") || !Q_stricmp(a, "del")) {
        if (n < 0) {
            n = de.numWp - 1;
        }
        if (n < 0 || n >= de.numWp) {
            Com_Printf("Demo editor: no such waypoint\n");
            return;
        }
        for (i = n; i < de.numWp - 1; i++) {
            de.wp[i] = de.wp[i + 1];
        }
        de.numWp--;
        DE_DecidePathMode();
        DE_SaveCam();
        Com_Printf("Demo editor: waypoint %d deleted, %d left\n", n + 1, de.numWp);
    } else if (!Q_stricmp(a, "goto")) {
        if (n < 0 || n >= de.numWp) {
            Com_Printf("demo_waypoint goto <n>\n");
            return;
        }
        DE_SeekDisplay(de.wp[n].time);
        VectorCopy(de.wp[n].org, de.camOrg);
        VectorCopy(de.wp[n].ang, de.camAng);
        de.freecam = qtrue;
        de.paused  = qtrue;
    } else if (!Q_stricmp(a, "list")) {
        for (i = 0; i < de.numWp; i++) {
            DE_FmtTime(de.wp[i].time / 1000.0f, tb, sizeof(tb));
            Com_Printf(
                "%2d  %s  %.0f %.0f %.0f  (%.0f %.0f %.0f)  fov %g\n", i + 1, tb, de.wp[i].org[0], de.wp[i].org[1],
                de.wp[i].org[2], de.wp[i].ang[0], de.wp[i].ang[1], de.wp[i].ang[2], de.wp[i].fov
            );
        }
        Com_Printf("%d waypoint(s)%s\n", de.numWp, de.pathRealtime && de.numWp > 1 ? ", timed path" : "");
    } else {
        Com_Printf("demo_waypoint [add|update [n] [retime]|delete [n]|goto <n>|list]\n");
    }
}

static void DE_Path_f(void)
{
    const char *a = Cmd_Argc() > 1 ? Cmd_Argv(1) : "toggle";

    if (!DE_Ready()) {
        return;
    }
    if (!Q_stricmp(a, "play")) {
        DE_PathPlay();
    } else if (!Q_stricmp(a, "stop")) {
        DE_PathStop();
    } else if (!Q_stricmp(a, "toggle")) {
        if (de.pathPlaying) {
            DE_PathStop();
        } else {
            DE_PathPlay();
        }
    } else {
        Com_Printf("demo_path [play|stop|toggle]\n");
    }
}

static void DE_CameraClear_f(void)
{
    if (!DE_Ready()) {
        return;
    }
    de.numWp       = 0;
    de.pathPlaying = qfalse;
    DE_SaveCam();
    Com_Printf("Demo editor: all waypoints cleared\n");
}

static void DE_HideHud_f(void)
{
    if (Cmd_Argc() > 1) {
        DE_SetHud(atoi(Cmd_Argv(1)));
    } else {
        DE_CycleHud();
    }
}

// cleanhud [0|1] - every HUD element off / back, in play or in a demo (KP Minus)
static void DE_CleanHud_f(void)
{
    if (Cmd_Argc() > 1) {
        DE_SetHud(atoi(Cmd_Argv(1)) ? 2 : 0);
    } else {
        DE_SetHud(s_hudMode ? 0 : 2);
    }
}

// filter [next | prev | off | list | <number> | <name>]   (KP* / KP/)
static void DE_Filter_f(void)
{
    const char *a;
    int         i, cur = r_ppFilterCv->integer;

    if (Cmd_Argc() < 2) {
        DE_SetFilter(cur + 1);
        return;
    }
    a = Cmd_ArgsFrom(1);
    if (!Q_stricmp(a, "next")) {
        DE_SetFilter(cur + 1);
    } else if (!Q_stricmp(a, "prev")) {
        DE_SetFilter(cur - 1);
    } else if (!Q_stricmp(a, "off")) {
        DE_SetFilter(0);
    } else if (!Q_stricmp(a, "list")) {
        for (i = 0; i <= HZM_NUM_FILTERS; i++) {
            Com_Printf("  %2d  %s%s\n", i, hzmFilterNames[i], i == cur ? "  <" : "");
        }
        Com_Printf("filter <number | name | next | prev | off>   r_ppFilterStrength 0..1 fades it\n");
    } else if (a[0] >= '0' && a[0] <= '9') {
        DE_SetFilter(atoi(a));
    } else {
        for (i = 0; i <= HZM_NUM_FILTERS; i++) {
            if (!Q_stricmpn(a, hzmFilterNames[i], strlen(a))) {
                DE_SetFilter(i);
                return;
            }
        }
        Com_Printf("No filter called \"%s\" (filter list)\n", a);
    }
}

static void DE_Names_f(void)
{
    int i, n = 0;

    if (!DE_Ready()) {
        return;
    }
    for (i = 0; i < MAX_CLIENTS; i++) {
        if (!DE_CS(CS_PLAYERS + i)[0]) {
            continue;
        }
        if (de.alias[i][0]) {
            Com_Printf("  slot %2d: %s  ^7->  %s\n", i, de.origName[i], de.alias[i]);
        } else {
            Com_Printf("  slot %2d: %s\n", i, de.origName[i]);
        }
        n++;
    }
    Com_Printf("%d player(s). demo_setname <slot> \"New name\"  ( - restores the recorded name)\n", n);
}

static void DE_SetName_f(void)
{
    int  slot;
    char clean[MAX_NAME_LENGTH];

    if (!DE_Ready()) {
        return;
    }
    if (Cmd_Argc() < 3) {
        Com_Printf("demo_setname <slot> \"New name\"   (use - to restore the recorded name)\n");
        return;
    }
    slot = atoi(Cmd_Argv(1));
    if (slot < 0 || slot >= MAX_CLIENTS) {
        Com_Printf("Demo editor: slot out of range (0-%d)\n", MAX_CLIENTS - 1);
        return;
    }
    if (!strcmp(Cmd_Argv(2), "-")) {
        de.alias[slot][0] = 0;
    } else {
        DE_CleanName(Cmd_ArgsFrom(2), clean, sizeof(clean));
        if (!clean[0]) {
            Com_Printf("Demo editor: empty name\n");
            return;
        }
        Q_strncpyz(de.alias[slot], clean, sizeof(de.alias[slot]));
    }
    DE_ApplyAlias(slot, qtrue);
    DE_SaveNames();
    Com_Printf(
        "Demo editor: slot %d is now \"%s\"\n", slot, de.alias[slot][0] ? de.alias[slot] : de.origName[slot]
    );
}

/*
==================
TIMELINE - a YouTube-style bar along the bottom while a demo plays with the full HUD

The length, the level boundaries and the kill / death / objective markers come from a second read of the
demo file (TL_ScanStep, a few ms a frame, so a long demo never stalls a frame). It reads each message only
up to its snapshot's server time: server commands come before svc_snapshot in every message, and a
gamestate starts a new level. Its display clock is the same one the editor runs (segBase): a level starts
one snapshot (DE_SegStartGap) after the previous level's last snapshot.

The mouse drives a cursor of its own (TL_CursorActive): always while you watch through the recorded
player's eyes, and while the right button is held with the free camera on (the mouse turns the camera
otherwise). Click the bar to jump, drag to scrub (the seeks are throttled, and stay inside the level
that is loaded; letting go lands anywhere, loading another level if it must). Never drawn in the clean
HUD modes or into an F10 render.
==================
*/
#define TL_MAX_SEG  32
#define TL_MAX_MARK 1024

enum { TLM_KILL, TLM_DEATH, TLM_OBJ, TLM_WP };

typedef struct {
    int  ms;     // display ms, -1 until the message's snapshot is read
    int  kind;
    char label[96];
    char victim[MAX_NAME_LENGTH];
} tlMark_t;

static struct {
    char         file[MAX_OSPATH]; // the demo file the scan belongs to
    fileHandle_t f;
    qboolean     scanning, done;
    int          bytes, msgs, t0;
    int          numSeg;
    int          segStart[TL_MAX_SEG]; // display ms
    int          segFirst[TL_MAX_SEG], segLast[TL_MAX_SEG];
    qboolean     segHasSnap[TL_MAX_SEG];
    int          cmdSeq;
    int          pendFrom;
    tlMark_t     mark[TL_MAX_MARK];
    int          numMark;
    int          objFlags[MAX_OBJECTIVES];
    char         objText[MAX_OBJECTIVES][MAX_STRING_CHARS / 4];
} tl;

// the cursor, the drag, the hover (per demo play, not per file)
static struct {
    float    x, y;
    qboolean placed;
    int      movedAt;
    qboolean rmb;
    qboolean drag;
    qboolean wasPaused;
    int      dragMs, lastSeekMs, nextSeekAt;
    float    pressX; // where the drag began: no scrub seek until the mouse moves (a marker press keeps its moment)
    qboolean hoverBar, hoverPanel, hoverPlay;
    int      hoverMs;
    int      hoverMark; // index into the merged list below, -1 none
} tlc;

static cvar_t *demo_timeline;
static cvar_t *demo_tlDump;

static void TL_ResetInput(void)
{
    tlc.drag = qfalse;
    tlc.rmb  = qfalse;
}
static char    s_demoFile[MAX_OSPATH];
static byte    s_tlMsg[MAX_MSGLEN];
int            CL_FontStringWidth(fontheader_t *pFont, const char *pszString, int iMaxLen); // cl_ui.cpp

void CL_DemoEd_SetFile(const char *name)
{
    Q_strncpyz(s_demoFile, name, sizeof(s_demoFile));
}

static int TL_TotalMs(void)
{
    int k = tl.numSeg - 1, t;
    if (k < 0) {
        return 0;
    }
    t = tl.segStart[k] + (tl.segHasSnap[k] ? tl.segLast[k] - tl.segFirst[k] : 0);
    if (!tl.done && DE_DisplayMs() > t) {
        t = (int)DE_DisplayMs();
    }
    return t > 1 ? t : 1;
}

// end of the level that is loaded, display ms
static int TL_SegEndMs(int seg)
{
    if (seg < 0 || seg >= tl.numSeg) {
        return TL_TotalMs();
    }
    if (seg + 1 < tl.numSeg) {
        return tl.segStart[seg + 1] - DE_SegStartGap();
    }
    return TL_TotalMs();
}

static const char *TL_Quoted(const char *s, char *out, int size)
{
    int n = 0;
    while (*s && *s != '"') {
        s++;
    }
    if (*s != '"') {
        out[0] = 0;
        return s;
    }
    s++;
    while (*s && *s != '"') {
        if (n < size - 1) {
            out[n++] = *s;
        }
        s++;
    }
    out[n] = 0;
    return *s ? s + 1 : s;
}

static tlMark_t *TL_NewMark(int kind)
{
    tlMark_t *m;
    if (tl.numMark >= TL_MAX_MARK) {
        return NULL;
    }
    m = &tl.mark[tl.numMark++];
    memset(m, 0, sizeof(*m));
    m->ms   = -1;
    m->kind = kind;
    return m;
}

static void TL_Command(const char *s)
{
    if (demo_tlDump && demo_tlDump->integer) {
        Com_Printf("^~^~^ TLCMD seg=%d %.150s\n", tl.numSeg, s);
    }
    if (!strncmp(s, "printdeathmsg ", 14)) {
        char      s1[64], s2[64], att[MAX_NAME_LENGTH], vic[MAX_NAME_LENGTH], a[MAX_NAME_LENGTH], v[MAX_NAME_LENGTH];
        const char *p = s + 14;
        tlMark_t  *m;
        p = TL_Quoted(p, s1, sizeof(s1));
        p = TL_Quoted(p, s2, sizeof(s2));
        p = TL_Quoted(p, att, sizeof(att));
        p = TL_Quoted(p, vic, sizeof(vic));
        while (*p == ' ') {
            p++;
        }
        DE_CleanName(att, a, sizeof(a));
        DE_CleanName(vic, v, sizeof(v));
        if (!v[0] || !(m = TL_NewMark(TLM_KILL))) {
            return;
        }
        Q_strncpyz(m->victim, v, sizeof(m->victim));
        // the cgame's own obituary wording (CG_ServerCommand printdeathmsg): "x" is an empty part;
        // type p = killed by a player, s = suicide, w = the world (and every AI kill in coop)
        if (s1[0] == 'x' && !s1[1]) {
            s1[0] = 0;
        }
        if (s2[0] == 'x' && !s2[1]) {
            s2[0] = 0;
        }
        if (tolower(*p) == 'p' && a[0]) {
            Com_sprintf(m->label, sizeof(m->label), "%s %s %s%s", v, s1[0] ? s1 : "killed by", a, s2);
        } else {
            Com_sprintf(m->label, sizeof(m->label), "%s %s", v, s1[0] ? s1 : "died");
        }
        return;
    }
    if (!strncmp(s, "cs ", 3)) {
        int         idx = atoi(s + 3), o, flags;
        const char *p   = s + 3;
        char        val[MAX_STRING_CHARS], text[MAX_STRING_CHARS / 4];
        tlMark_t   *m;

        if (idx < CS_OBJECTIVES || idx >= CS_OBJECTIVES + MAX_OBJECTIVES) {
            return;
        }
        o = idx - CS_OBJECTIVES;
        while (*p && *p != ' ') {
            p++;
        }
        while (*p == ' ') {
            p++;
        }
        if (*p == '"') {
            TL_Quoted(p, val, sizeof(val));
        } else {
            Q_strncpyz(val, p, sizeof(val));
        }
        flags = atoi(Info_ValueForKey(val, "flags"));
        Q_strncpyz(text, Info_ValueForKey(val, "text"), sizeof(text));
        if (flags == tl.objFlags[o] && !strcmp(text, tl.objText[o])) {
            return; // the same objective pushed again
        }
        tl.objFlags[o] = flags;
        Q_strncpyz(tl.objText[o], text, sizeof(tl.objText[o]));
        if (flags == OBJ_FLAG_NONE || (flags & OBJ_FLAG_HIDDEN) || !text[0] || !(m = TL_NewMark(TLM_OBJ))) {
            return;
        }
        Com_sprintf(
            m->label, sizeof(m->label), "%s: %s", (flags & OBJ_FLAG_COMPLETED) ? "Objective complete" : "Objective", text
        );
    }
}

static void TL_FinishPending(int ms)
{
    int i;
    for (i = tl.pendFrom; i < tl.numMark; i++) {
        if (tl.mark[i].ms < 0) {
            tl.mark[i].ms = ms;
        }
    }
    tl.pendFrom = tl.numMark;
}

static void TL_NewSegment(void)
{
    int k = tl.numSeg;
    if (k > 0) {
        TL_FinishPending(tl.segStart[k - 1] + (tl.segHasSnap[k - 1] ? tl.segLast[k - 1] - tl.segFirst[k - 1] : 0));
    }
    if (k >= TL_MAX_SEG) {
        return;
    }
    if (k == 0) {
        tl.segStart[0] = 0;
    } else {
        // the same sum CL_DemoEd_GamestateParsed makes
        tl.segStart[k] = tl.segStart[k - 1]
                       + (tl.segHasSnap[k - 1] ? tl.segLast[k - 1] - tl.segFirst[k - 1] + DE_SegStartGap() : 0);
    }
    tl.segHasSnap[k] = qfalse;
    tl.numSeg++;
    memset(tl.objFlags, 0, sizeof(tl.objFlags));
    memset(tl.objText, 0, sizeof(tl.objText));
}

static void TL_ParseMsg(int len)
{
    msg_t m;
    int   cmd;

    MSG_Init(&m, s_tlMsg, sizeof(s_tlMsg));
    m.cursize = len;
    MSG_Bitstream(&m);
    MSG_ReadLong(&m); // reliable acknowledge
    tl.pendFrom = tl.numMark;
    while (m.readcount <= m.cursize) {
        cmd = MSG_ReadByte(&m);
        if (cmd == svc_nop) {
            continue;
        }
        if (cmd == svc_serverCommand) {
            int         seq = MSG_ReadLong(&m);
            const char *s   = MSG_ReadScrambledString(&m);
            if (seq > tl.cmdSeq) {
                tl.cmdSeq = seq;
                TL_Command(s);
            }
            continue;
        }
        if (cmd == svc_gamestate) {
            tl.cmdSeq = MSG_ReadLong(&m);
            TL_NewSegment();
            return;
        }
        if (cmd == svc_snapshot) {
            int t = MSG_ReadLong(&m), k = tl.numSeg - 1;
            if (k >= 0 && k < TL_MAX_SEG) {
                if (!tl.segHasSnap[k]) {
                    tl.segHasSnap[k] = qtrue;
                    tl.segFirst[k]   = t;
                    tl.segLast[k]    = t;
                }
                if (t > tl.segLast[k]) {
                    tl.segLast[k] = t;
                }
                TL_FinishPending(tl.segStart[k] + tl.segLast[k] - tl.segFirst[k]);
            }
            return;
        }
        return; // centerprints and cgame messages only follow the snapshot
    }
}

static void TL_ScanFinish(void)
{
    int k = tl.numSeg - 1;
    if (k >= 0) {
        TL_FinishPending(tl.segStart[k] + (tl.segHasSnap[k] ? tl.segLast[k] - tl.segFirst[k] : 0));
    }
    if (tl.f) {
        FS_FCloseFile(tl.f);
        tl.f = 0;
    }
    tl.scanning = qfalse;
    tl.done     = qtrue;
    if (developer && developer->integer) {
        Com_Printf(
            "^~^~^ DEMOED timeline scan %s: %d levels, %.1f s, %d markers, %d msgs, %d KB, %d ms\n", tl.file, tl.numSeg,
            TL_TotalMs() / 1000.0f, tl.numMark, tl.msgs, tl.bytes / 1024, Sys_Milliseconds() - tl.t0
        );
        for (k = 0; k < tl.numSeg; k++) {
            Com_Printf("^~^~^ DEMOED timeline level %d at %.2f s\n", k + 1, tl.segStart[k] / 1000.0f);
        }
        for (k = 0; k < tl.numMark; k++) {
            Com_Printf("^~^~^ DEMOED timeline mark %.2f s %s\n", tl.mark[k].ms / 1000.0f, tl.mark[k].label);
        }
    }
}

// a few ms of the scan per frame
static void TL_ScanStep(void)
{
    int t0, n = 0;

    if (!s_demoFile[0]) {
        return;
    }
    if (Q_stricmp(tl.file, s_demoFile)) {
        // a new demo (a restart of the same one keeps what was found)
        if (tl.f) {
            FS_FCloseFile(tl.f);
        }
        memset(&tl, 0, sizeof(tl));
        Q_strncpyz(tl.file, s_demoFile, sizeof(tl.file));
        FS_FOpenFileRead(tl.file, &tl.f, qtrue, qtrue);
        if (!tl.f) {
            tl.done = qtrue;
            return;
        }
        tl.scanning = qtrue;
        tl.t0       = Sys_Milliseconds();
    }
    if (!tl.scanning) {
        return;
    }
    t0 = Sys_Milliseconds();
    while (Sys_Milliseconds() - t0 < 3) {
        int seq, len;
        if (FS_Read(&seq, 4, tl.f) != 4 || FS_Read(&len, 4, tl.f) != 4) {
            TL_ScanFinish();
            return;
        }
        len = LittleLong(len);
        if (len < 0 || len > (int)sizeof(s_tlMsg) || FS_Read(s_tlMsg, len, tl.f) != len) {
            TL_ScanFinish();
            return;
        }
        tl.bytes += len + 8;
        tl.msgs++;
        TL_ParseMsg(len);
        if (++n > 4000) {
            break;
        }
    }
}

/*
---- layout, shared by the drawing and the mouse
*/
typedef struct {
    float s, W, H;
    float px, py, pw, ph;       // panel
    float bx, bw;               // play / pause button
    float tx0, tx1, ty;         // track ends, track centre line
    float speedX, timeX;
} tlLayout_t;

static fontheader_t *TL_Font(void)
{
    return re.LoadFont("verdana-14");
}

static float TL_TextW(const char *t, float scale)
{
    fontheader_t *f = TL_Font();
    return f ? (float)CL_FontStringWidth(f, t, -1) * scale : 0.0f;
}

static void TL_Text(const char *t, float x, float y, float scale, const float *rgba)
{
    fontheader_t *font = TL_Font();
    vec2_t        sc;
    vec4_t        shadow = {0, 0, 0, rgba[3] * 0.8f};
    if (!font) {
        return;
    }
    sc[0] = sc[1] = scale;
    re.SetColor(shadow);
    re.DrawString(font, t, (x + 1.0f) / scale, (y + 1.0f) / scale, -1, sc);
    re.SetColor(rgba);
    re.DrawString(font, t, x / scale, y / scale, -1, sc);
    re.SetColor(NULL);
}

static void TL_Box(float x, float y, float w, float h, const float *rgba)
{
    re.SetColor(rgba);
    re.DrawBox(x, y, w, h);
}

static void TL_TimeText(char *out, int size)
{
    char a[32], b[32];
    DE_FmtTime(DE_Clock(), a, sizeof(a));
    DE_FmtTime(TL_TotalMs() / 1000.0f, b, sizeof(b));
    Com_sprintf(out, size, "%s / %s%s", a, b, tl.done ? "" : "+");
}

static void TL_Layout(tlLayout_t *L)
{
    float sw;

    L->W = (float)cls.glconfig.vidWidth;
    L->H = (float)cls.glconfig.vidHeight;
    L->s = L->H / 720.0f;
    if (L->s < 0.8f) {
        L->s = 0.8f;
    }
    // just above the editor's status panel (CG_DemoEdDrawStatus: 46 high, 14 off the bottom)
    L->ph = 30.0f * L->s;
    L->px = 24.0f * L->s;
    L->pw = L->W - 48.0f * L->s;
    L->py = L->H - (14.0f + 46.0f + 8.0f) * L->s - L->ph;
    L->bx = L->px + 10.0f * L->s;
    L->bw = 18.0f * L->s;
    L->speedX = L->bx + L->bw + 10.0f * L->s;
    sw     = TL_TextW("0.25x", L->s * 0.8f);
    L->tx0   = L->speedX + sw + 14.0f * L->s;
    L->timeX = L->px + L->pw - 12.0f * L->s - TL_TextW("00:00.0 / 00:00.0+", L->s * 0.8f);
    L->tx1   = L->timeX - 14.0f * L->s;
    L->ty    = L->py + L->ph * 0.5f;
}

static float TL_MsToX(const tlLayout_t *L, int ms)
{
    float f = (float)ms / (float)TL_TotalMs();
    if (f < 0) {
        f = 0;
    } else if (f > 1) {
        f = 1;
    }
    return L->tx0 + f * (L->tx1 - L->tx0);
}

static int TL_XToMs(const tlLayout_t *L, float x)
{
    float f = (x - L->tx0) / (L->tx1 - L->tx0);
    if (f < 0) {
        f = 0;
    } else if (f > 1) {
        f = 1;
    }
    return (int)(f * TL_TotalMs());
}

// every marker: the scanned ones, then the camera waypoints (index >= tl.numMark)
static int TL_NumMarks(void)
{
    return tl.numMark + de.numWp;
}

static void TL_GetMark(int i, int *ms, int *kind, char *label, int size)
{
    if (i < tl.numMark) {
        const tlMark_t *m = &tl.mark[i];
        char            me[MAX_NAME_LENGTH];
        *ms   = m->ms;
        *kind = m->kind;
        Q_strncpyz(label, m->label, size);
        if (m->kind == TLM_KILL) {
            // the recorded player's own death stands out
            DE_CleanName(Info_ValueForKey(DE_CS(CS_PLAYERS + clc.clientNum), "name"), me, sizeof(me));
            if ((me[0] && !Q_stricmp(me, m->victim))
                || (de.origName[clc.clientNum][0] && !Q_stricmp(de.origName[clc.clientNum], m->victim))) {
                *kind = TLM_DEATH;
            }
        }
        return;
    }
    i -= tl.numMark;
    *ms   = (int)DE_WpTime(i);
    *kind = TLM_WP;
    Com_sprintf(label, size, "Camera waypoint %d", i + 1);
}

static qboolean TL_Visible(void)
{
    return (CL_DemoEd_Enabled() && clc.state == CA_ACTIVE && de.started && s_hudMode == 0 && demo_timeline
            && demo_timeline->integer && !CL_VideoRecording() && !(Key_GetCatcher() & ~KEYCATCH_CONSOLE))
             ? qtrue
             : qfalse;
}

static qboolean TL_CursorActive(void)
{
    if (!TL_Visible() || Key_GetCatcher() != 0) {
        return qfalse;
    }
    return (!de.freecam || de.pathPlaying || tlc.rmb || tlc.drag) ? qtrue : qfalse;
}

static void TL_UpdateHover(void)
{
    tlLayout_t L;
    float      best = 1e9f, tol;
    int        i;

    tlc.hoverBar = tlc.hoverPanel = tlc.hoverPlay = qfalse;
    tlc.hoverMark = -1;
    if (!TL_CursorActive()) {
        return;
    }
    TL_Layout(&L);
    tlc.hoverPanel = (tlc.x >= L.px && tlc.x <= L.px + L.pw && tlc.y >= L.py - 16.0f * L.s && tlc.y <= L.py + L.ph)
                       ? qtrue
                       : qfalse;
    if (!tlc.hoverPanel && !tlc.drag) {
        return;
    }
    tlc.hoverPlay = (tlc.x >= L.bx - 4.0f * L.s && tlc.x <= L.bx + L.bw + 4.0f * L.s) ? qtrue : qfalse;
    tlc.hoverBar  = (tlc.drag || (tlc.x >= L.tx0 - 6.0f * L.s && tlc.x <= L.tx1 + 6.0f * L.s)) ? qtrue : qfalse;
    tlc.hoverMs   = TL_XToMs(&L, tlc.x);
    if (!tlc.hoverBar || tlc.drag) {
        return;
    }
    tol = 6.0f * L.s;
    for (i = 0; i < TL_NumMarks(); i++) {
        int  ms, kind;
        char lb[8];
        float d;
        TL_GetMark(i, &ms, &kind, lb, sizeof(lb));
        if (ms < 0) {
            continue;
        }
        d = fabs(TL_MsToX(&L, ms) - tlc.x);
        if (d <= tol && d < best) {
            best          = d;
            tlc.hoverMark = i;
        }
    }
}

// mouse motion: qtrue when the cursor took it (the camera and the view never see it)
qboolean CL_DemoEd_CursorMouse(int dx, int dy)
{
    float k;
    if (!TL_CursorActive()) {
        return qfalse;
    }
    if (!tlc.placed) {
        tlc.x      = cls.glconfig.vidWidth * 0.5f;
        tlc.y      = cls.glconfig.vidHeight * 0.5f;
        tlc.placed = qtrue;
    }
    k = cls.glconfig.vidHeight / 900.0f;
    if (k < 0.5f) {
        k = 0.5f;
    }
    tlc.x += dx * k;
    tlc.y += dy * k;
    tlc.x = tlc.x < 0 ? 0 : (tlc.x > cls.glconfig.vidWidth - 1 ? cls.glconfig.vidWidth - 1 : tlc.x);
    tlc.y = tlc.y < 0 ? 0 : (tlc.y > cls.glconfig.vidHeight - 1 ? cls.glconfig.vidHeight - 1 : tlc.y);
    tlc.movedAt = cls.realtime;
    return qtrue;
}

static void TL_Seek(int ms, const char *why)
{
    int t0 = Sys_Milliseconds();
    DE_SeekDisplay(ms);
    tlc.lastSeekMs = ms;
    // scrub throttle: never spend more than about a third of the time seeking
    t0            = Sys_Milliseconds() - t0;
    tlc.nextSeekAt = Sys_Milliseconds() + (t0 * 2 > 30 ? t0 * 2 : 30);
    if (developer && developer->integer) {
        Com_Printf("^~^~^ DEMOED timeline %s %.2f s (took %d ms)\n", why, ms / 1000.0f, t0);
    }
}

static void TL_EndDrag(void)
{
    int segEnd;
    if (!tlc.drag) {
        return;
    }
    tlc.drag = qfalse;
    segEnd   = TL_SegEndMs(de.segIndex) - 2 * DE_SegStartGap();
    de.paused = tlc.wasPaused;
    if (tlc.dragMs != tlc.lastSeekMs || tlc.dragMs < de.segBase || tlc.dragMs > segEnd) {
        TL_Seek(tlc.dragMs, "release"); // the drag may have left the loaded level: land there now
    }
}

// mouse buttons and wheel: qtrue when used
static qboolean TL_MouseKey(int key, qboolean down)
{
    if (key == K_MOUSE2) {
        tlc.rmb = down && TL_Visible() && de.freecam ? qtrue : qfalse;
        if (!down) {
            TL_EndDrag();
        }
        return TL_Visible();
    }
    if (key == K_MOUSE1 && !down) {
        if (tlc.drag) {
            TL_EndDrag();
            return qtrue;
        }
        return TL_CursorActive();
    }
    if (!TL_CursorActive()) {
        return qfalse;
    }
    TL_UpdateHover();
    if (key == K_MWHEELUP || key == K_MWHEELDOWN) {
        if (!tlc.hoverPanel) {
            return qfalse;
        }
        TL_Seek((int)DE_DisplayMs() + (key == K_MWHEELUP ? DE_SEEK_STEP_MS : -DE_SEEK_STEP_MS), "wheel");
        return qtrue;
    }
    if (key != K_MOUSE1) {
        return qfalse;
    }
    tlc.movedAt = cls.realtime;
    if (tlc.hoverPlay) {
        de.paused = !de.paused;
        Com_Printf("Demo editor: %s\n", de.paused ? "paused" : "playing");
        return qtrue;
    }
    if (tlc.hoverBar) {
        int ms = tlc.hoverMs, kind, segEnd = TL_SegEndMs(de.segIndex) - 2 * DE_SegStartGap();
        char lb[96];
        if (tlc.hoverMark >= 0) {
            TL_GetMark(tlc.hoverMark, &ms, &kind, lb, sizeof(lb)); // a marker: its exact moment
        }
        tlc.wasPaused = de.paused;
        TL_Seek(ms, tlc.hoverMark >= 0 ? "marker" : "click");
        if (de.started && ms >= de.segBase && ms <= segEnd) {
            // inside the loaded level: hold the picture still and scrub with the mouse
            tlc.drag   = qtrue;
            tlc.dragMs = ms;
            tlc.pressX = tlc.x;
            de.paused  = qtrue;
        }
        return qtrue;
    }
    if (!tlc.hoverPanel) {
        // a click in the picture: pause / play, as in a video player
        de.paused = !de.paused;
        Com_Printf("Demo editor: %s\n", de.paused ? "paused" : "playing");
    }
    return qtrue;
}

// per frame (from CL_DemoEd_SetTime): the scan, the scrub
static void TL_Frame(void)
{
    TL_ScanStep();
    if (!tlc.drag) {
        return;
    }
    if (!TL_CursorActive() || !de.started) {
        tlc.drag  = qfalse;
        de.paused = tlc.wasPaused;
        return;
    }
    {
        tlLayout_t L;
        // a seek reads until the snapshot AFTER its target: stop two snapshots short of the level's last
        // one, or the read runs into the next level's gamestate and loads it with the button still down
        int        ms, lo = de.segBase, hi = TL_SegEndMs(de.segIndex) - 2 * DE_SegStartGap();
        TL_Layout(&L);
        if (tlc.pressX >= 0 && fabs(tlc.x - tlc.pressX) < 1.0f) {
            return; // not moved yet
        }
        tlc.pressX = -1;
        ms = TL_XToMs(&L, tlc.x);
        tlc.dragMs = ms;
        if (ms < lo) {
            ms = lo; // no level loads while the button is down
        } else if (ms > hi) {
            ms = hi;
        }
        if (ms != tlc.lastSeekMs && Sys_Milliseconds() >= tlc.nextSeekAt) {
            TL_Seek(ms, "scrub");
        }
    }
}

static void TL_DrawCursor(float x, float y, float s)
{
    vec4_t dark = {0, 0, 0, 0.85f}, lite = {1.0f, 0.95f, 0.82f, 1.0f};
    int    i;
    float  u = s < 1.0f ? 1.0f : s;
    for (i = 0; i < 14; i++) {
        float w = (i < 10 ? i : 19 - i) + 1.0f;
        TL_Box(x - u, y + (i - 1) * u, (w + 2.0f) * u, 3.0f * u, dark);
    }
    for (i = 0; i < 13; i++) {
        float w = (i < 10 ? i : 19 - i);
        if (w > 0) {
            TL_Box(x, y + i * u, w * u, u, lite);
        }
    }
    re.SetColor(NULL);
}

void CL_DemoEd_DrawTimeline(void)
{
    tlLayout_t L;
    char       buf[160];
    float      s, x, h, phx;
    int        i, n, cur;
    qboolean   showCursor;
    vec4_t bg     = {0.04f, 0.04f, 0.05f, 0.62f};
    vec4_t edge   = {1.0f, 0.86f, 0.52f, 0.55f};
    vec4_t gold   = {1.0f, 0.86f, 0.52f, 1.0f};
    vec4_t dim    = {0.86f, 0.84f, 0.79f, 0.85f};
    vec4_t trackC = {0.86f, 0.84f, 0.79f, 0.22f};
    vec4_t ahead  = {0.86f, 0.84f, 0.79f, 0.10f};
    vec4_t kill   = {1.0f, 0.55f, 0.35f, 0.95f};
    vec4_t death  = {1.0f, 0.18f, 0.15f, 1.0f};
    vec4_t obj    = {1.0f, 0.86f, 0.52f, 1.0f};
    vec4_t wpC    = {0.55f, 0.85f, 1.0f, 1.0f};
    vec4_t sep    = {0.95f, 0.95f, 0.95f, 0.85f};
    vec4_t black  = {0, 0, 0, 0.9f};
    vec4_t hoverC = {1.0f, 1.0f, 1.0f, 0.45f};

    if (!TL_Visible()) {
        return;
    }
    TL_UpdateHover();
    TL_Layout(&L);
    s = L.s;

    TL_Box(L.px, L.py, L.pw, L.ph, bg);
    TL_Box(L.px, L.py, L.pw, 2.0f, edge);

    // play / pause
    {
        vec4_t *c = tlc.hoverPlay ? &gold : &dim;
        float   by = L.ty - 7.0f * s;
        if (de.paused || de.atEnd) {
            // play triangle, in steps
            for (i = 0; i < 7; i++) {
                float hh = (14.0f - 2.0f * i) * s;
                TL_Box(L.bx + 3.0f * s + i * 2.0f * s, L.ty - hh * 0.5f, 2.0f * s, hh, *c);
            }
        } else {
            TL_Box(L.bx + 3.0f * s, by, 4.0f * s, 14.0f * s, *c);
            TL_Box(L.bx + 11.0f * s, by, 4.0f * s, 14.0f * s, *c);
        }
    }
    // speed
    Com_sprintf(buf, sizeof(buf), "%gx", de.speed);
    TL_Text(buf, L.speedX, L.ty - 8.0f * s, s * 0.8f, de.speed != 1.0f ? gold : dim);
    // time
    TL_TimeText(buf, sizeof(buf));
    TL_Text(buf, L.timeX, L.ty - 8.0f * s, s * 0.8f, dim);

    // the track
    h   = (tlc.hoverBar ? 7.0f : 4.0f) * s;
    phx = TL_MsToX(&L, (int)DE_DisplayMs());
    TL_Box(L.tx0, L.ty - h * 0.5f, L.tx1 - L.tx0, h, trackC);
    if (!tl.done) {
        // not scanned yet: the rest of the bar is a guess
        float sx = TL_MsToX(&L, TL_TotalMs());
        TL_Box(sx, L.ty - h * 0.5f, L.tx1 - sx, h, ahead);
    }
    TL_Box(L.tx0, L.ty - h * 0.5f, phx - L.tx0, h, gold);

    // the loaded level, faintly framed
    if (tl.numSeg > 1) {
        float a = TL_MsToX(&L, de.segBase), b = TL_MsToX(&L, TL_SegEndMs(de.segIndex));
        vec4_t lv = {1.0f, 0.86f, 0.52f, 0.12f};
        TL_Box(a, L.ty - 9.0f * s, b - a, 18.0f * s, lv);
    }
    // level boundaries
    for (i = 1; i < tl.numSeg; i++) {
        x = TL_MsToX(&L, tl.segStart[i]);
        TL_Box(x - 1.5f * s, L.ty - 9.0f * s, 3.0f * s, 18.0f * s, black);
        TL_Box(x - 0.5f * s, L.ty - 9.0f * s, 1.0f * s, 18.0f * s, sep);
        Com_sprintf(buf, sizeof(buf), "L%d", i + 1);
        TL_Text(buf, x + 3.0f * s, L.py + 1.0f * s, s * 0.55f, dim);
    }
    if (tl.numSeg > 1) {
        TL_Text("L1", L.tx0 + 3.0f * s, L.py + 1.0f * s, s * 0.55f, dim);
    }

    // markers
    n = TL_NumMarks();
    for (i = 0; i < n; i++) {
        int  ms, kind;
        char lb[8];
        float big = (i == tlc.hoverMark) ? 1.5f : 1.0f;
        TL_GetMark(i, &ms, &kind, lb, sizeof(lb));
        if (ms < 0) {
            continue;
        }
        x = TL_MsToX(&L, ms);
        switch (kind) {
        case TLM_KILL:
            TL_Box(x - 1.0f * s * big, L.ty - 12.0f * s * big, 2.0f * s * big, 8.0f * s * big, kill);
            break;
        case TLM_DEATH:
            TL_Box(x - 2.0f * s * big, L.ty - 14.0f * s * big, 4.0f * s * big, 10.0f * s * big, death);
            TL_Box(x - 4.0f * s * big, L.ty - 14.0f * s * big, 8.0f * s * big, 2.0f * s * big, death);
            break;
        case TLM_OBJ:
            TL_Box(x - 3.5f * s * big, L.ty - 3.5f * s * big, 7.0f * s * big, 7.0f * s * big, black);
            TL_Box(x - 2.5f * s * big, L.ty - 2.5f * s * big, 5.0f * s * big, 5.0f * s * big, obj);
            break;
        default:
            TL_Box(x - 1.5f * s * big, L.ty + 4.0f * s, 3.0f * s * big, 7.0f * s * big, wpC);
            break;
        }
    }

    // playhead
    TL_Box(phx - 3.0f * s, L.ty - 8.0f * s, 6.0f * s, 16.0f * s, black);
    TL_Box(phx - 2.0f * s, L.ty - 7.0f * s, 4.0f * s, 14.0f * s, gold);

    // hover: a line and the time (and the marker) above the panel
    if (tlc.hoverBar) {
        char tb[32], lb[96];
        int  ms = tlc.drag ? tlc.dragMs : tlc.hoverMs, kind;
        float tw, bw, bx, by, lh = 18.0f * s;
        lb[0] = 0;
        if (tlc.hoverMark >= 0) {
            TL_GetMark(tlc.hoverMark, &ms, &kind, lb, sizeof(lb));
        }
        x = TL_MsToX(&L, ms);
        TL_Box(x - 0.5f * s, L.ty - 9.0f * s, 1.0f * s, 18.0f * s, hoverC);
        DE_FmtTime(ms / 1000.0f, tb, sizeof(tb));
        cur = 0;
        for (i = 0; i < tl.numSeg; i++) {
            if (ms >= tl.segStart[i]) {
                cur = i;
            }
        }
        if (tl.numSeg > 1) {
            Q_strcat(tb, sizeof(tb), va("   L%d", cur + 1));
        }
        tw = TL_TextW(tb, s * 0.8f);
        bw = tw;
        if (lb[0] && TL_TextW(lb, s * 0.72f) > bw) {
            bw = TL_TextW(lb, s * 0.72f);
        }
        bw += 20.0f * s;
        bx = x - bw * 0.5f;
        if (bx < L.px) {
            bx = L.px;
        } else if (bx + bw > L.px + L.pw) {
            bx = L.px + L.pw - bw;
        }
        by = L.py - (lb[0] ? 2.0f : 1.0f) * lh - 12.0f * s;
        TL_Box(bx, by, bw, (lb[0] ? 2.0f : 1.0f) * lh + 6.0f * s, bg);
        TL_Box(bx, by + (lb[0] ? 2.0f : 1.0f) * lh + 4.0f * s, bw, 2.0f, edge);
        TL_Text(tb, bx + (bw - tw) * 0.5f, by + 3.0f * s, s * 0.8f, gold);
        if (lb[0]) {
            TL_Text(lb, bx + (bw - TL_TextW(lb, s * 0.72f)) * 0.5f, by + 3.0f * s + lh, s * 0.72f, dim);
        }
    }

    showCursor = TL_CursorActive() && (tlc.drag || tlc.hoverPanel || cls.realtime - tlc.movedAt < 2500) && tlc.placed;
    if (showCursor) {
        TL_DrawCursor(tlc.x, tlc.y, s);
    }
    re.SetColor(NULL);
}

// test aid (the harness has no OS cursor to aim with): demo_tlcursor <seconds> puts the timeline cursor on
// the bar at that moment, demo_tlcursor xy <x> <y> anywhere. The press itself is a real mouse button.
static void TL_Cursor_f(void)
{
    tlLayout_t L;
    if (!DE_Ready()) {
        return;
    }
    TL_Layout(&L);
    if (Cmd_Argc() >= 4 && !Q_stricmp(Cmd_Argv(1), "xy")) {
        tlc.x = (float)atof(Cmd_Argv(2));
        tlc.y = (float)atof(Cmd_Argv(3));
    } else if (Cmd_Argc() >= 2) {
        tlc.x = TL_MsToX(&L, (int)(atof(Cmd_Argv(1)) * 1000.0f));
        tlc.y = L.ty;
    }
    tlc.placed  = qtrue;
    tlc.movedAt = cls.realtime;
    Com_Printf(
        "^~^~^ DEMOED timeline cursor %.1f %.1f (track %.1f..%.1f y %.1f, total %.2f s, %d px/s)\n", tlc.x, tlc.y, L.tx0,
        L.tx1, L.ty, TL_TotalMs() / 1000.0f, (int)((L.tx1 - L.tx0) * 1000.0f / TL_TotalMs())
    );
}

void CL_DemoEd_Init(void)
{
    demo_editor      = Cvar_Get("demo_editor", "1", CVAR_ARCHIVE);
    demo_fov         = Cvar_Get("demo_fov", "90", CVAR_ARCHIVE);
    demo_camSpeed    = Cvar_Get("demo_camSpeed", "320", CVAR_ARCHIVE);
    demo_pathSegTime = Cvar_Get("demo_pathSegTime", "2", CVAR_ARCHIVE);
    demo_cam         = Cvar_Get("demo_cam", "", CVAR_ROM);
    demo_hud         = Cvar_Get("demo_hud", "0", CVAR_ROM);
    demo_playing     = Cvar_Get("demo_playing", "0", CVAR_ROM);
    r_ppFilterCv     = Cvar_Get("r_ppFilter", "0", 0);
    demo_status      = Cvar_Get("demo_status", "", CVAR_ROM);
    demo_timeline    = Cvar_Get("demo_timeline", "1", CVAR_ARCHIVE); // the bar along the bottom (full HUD only)
    demo_tlDump      = Cvar_Get("demo_tlDump", "0", 0);
    Cvar_Get("demo_startPaused", "0", 0); // 1: a demo starts paused on its first frame              // print every server command the scan reads

    Cmd_AddCommand("demo_seek", DE_Seek_f);
    Cmd_AddCommand("demo_seekby", DE_SeekBy_f);
    Cmd_AddCommand("demo_step", DE_Step_f);
    Cmd_AddCommand("demo_pause", DE_Pause_f);
    Cmd_AddCommand("demo_speed", DE_Speed_f);
    Cmd_AddCommand("demo_camera", DE_Camera_f);
    Cmd_AddCommand("demo_waypoint", DE_Waypoint_f);
    Cmd_AddCommand("demo_path", DE_Path_f);
    Cmd_AddCommand("demo_camera_clear", DE_CameraClear_f);
    Cmd_AddCommand("demo_hidehud", DE_HideHud_f);
    Cmd_AddCommand("demo_names", DE_Names_f);
    Cmd_AddCommand("demo_setname", DE_SetName_f);
    Cmd_AddCommand("cleanhud", DE_CleanHud_f);
    Cmd_AddCommand("filter", DE_Filter_f);
    Cmd_AddCommand("demo_tlcursor", TL_Cursor_f);

    CL_DemoEd_Reset();
}
