/*
===========================================================================
HZM coop - DEMO EDITOR (cgame side)

The client (cl_demoedit.cpp) owns the playback clock, the free camera, the paths and the names.
This file only renders what it publishes:
  demo_cam    "x y z pitch yaw roll fov" while the free camera is on, empty otherwise
  demo_hud    0 full HUD, 1 player names only, 2 no HUD - also in live play (cleanhud / KP Minus)
  demo_status the editor's status line (full HUD only)

While the free camera is on:
  - the view is the camera, nothing else: the recorded player's zoom, scope, ADS, shoulder camera,
    turret camera, view shake and reload feel never reach it (CG_CalcViewValues returns early)
  - every player, the recorded one included, is drawn as the full third-person character
    (cg.renderingThirdPerson, and CG_ModelAnim's own bThirdPerson lockstep)
  - the world is drawn from every area (the recorded player's areamask would cull rooms the camera
    is in) and the PiP scope / lens post-fx are held off
After a seek the client runs demoed_cs <index> for each changed configstring and demoed_reset.
===========================================================================
*/

#include "cg_local.h"
#include "../qcommon/hzm_filters.h" // HZM coop - visual filter names

void CG_ProcessConfigString(int num, qboolean modelOnly);
void CG_DemoEdResetSnapshots(int snapNum, int cmdSeq);

static cvar_t *s_demoCam;
static cvar_t *s_demoHud;
static cvar_t *s_demoStatus;

static qboolean s_camOn;
static int      s_camMod = -1;
static vec3_t   s_camOrg;
static vec3_t   s_camAng;
static float    s_camFov = 90.0f;

static void CG_DemoEdCvars(void)
{
    if (!s_demoCam) {
        s_demoCam    = cgi.Cvar_Get("demo_cam", "", CVAR_ROM);
        s_demoHud    = cgi.Cvar_Get("demo_hud", "0", CVAR_ROM);
        s_demoStatus = cgi.Cvar_Get("demo_status", "", CVAR_ROM);
    }
}

qboolean CG_DemoCamActive(void)
{
    if (!cg.demoPlayback) {
        return qfalse;
    }
    CG_DemoEdCvars();
    if (s_demoCam->modificationCount != s_camMod) {
        s_camMod = s_demoCam->modificationCount;
        s_camOn  = qfalse;
        if (s_demoCam->string[0]
            && sscanf(
                   s_demoCam->string, "%f %f %f %f %f %f %f", &s_camOrg[0], &s_camOrg[1], &s_camOrg[2], &s_camAng[0],
                   &s_camAng[1], &s_camAng[2], &s_camFov
               ) == 7) {
            s_camOn = qtrue;
            if (s_camFov < 10.0f || s_camFov > 160.0f) {
                s_camFov = 90.0f;
            }
        }
    }
    return s_camOn;
}

int CG_DemoHudMode(void)
{
    CG_DemoEdCvars();
    return s_demoHud->integer;
}

/*
==================
CG_DemoCamView - the whole view for this frame; returns CG_CalcFov's "inwater"
==================
*/
int CG_DemoCamView(void)
{
    static qboolean s_neutral = qfalse;
    float           fovX, fovY, ratio, x;
    int             contents;

    // the recorded player's lens effects belong to his eyes, not to this camera: neutral once on entry
    if (!s_neutral || (cg.time & 1023) < 20) {
        cgi.Cvar_Set("r_ppBlood", "0");
        cgi.Cvar_Set("r_ppHit", "0");
        cgi.Cvar_Set("r_ppSuppress", "0");
        cgi.Cvar_Set("r_ppHeat", "0");
        cgi.Cvar_Set("r_ppMuzzleAs", "0");
        cgi.Cvar_Set("r_ppMuzzleHeat", "0");
        cgi.Cvar_Set("r_ppHealthFrac", "1");
        cgi.Cvar_Set("r_ppRainWet", "0");
        cgi.Cvar_Set("r_ppUnderwaterAir", "0");
        cgi.Cvar_Set("r_weaponfovx", "0");
        s_neutral = qtrue;
    }

    VectorCopy(s_camOrg, cg.refdef.vieworg);
    VectorCopy(s_camAng, cg.refdefViewAngles);
    AnglesToAxis(cg.refdefViewAngles, cg.refdef.viewaxis);

    // hear the scene from the camera
    VectorCopy(cg.refdef.vieworg, cg.SoundOrg);
    AnglesToAxis(cg.refdefViewAngles, cg.SoundAxis);
    VectorCopy(cg.refdef.vieworg, cg.currentViewPos);
    VectorCopy(cg.refdefViewAngles, cg.currentViewAngles);

    // demo_fov is a 4:3 horizontal fov, widened for the screen exactly as cg_fov is
    ratio = (float)cg.refdef.width / (float)cg.refdef.height * (3.0f / 4.0f);
    fovX  = RAD2DEG(atan(tan(DEG2RAD(s_camFov / 2.0)) * ratio)) * 2.0f;
    x     = cg.refdef.width / tan(fovX / 360.0f * M_PI);
    fovY  = atan2(cg.refdef.height, x) * 360.0f / M_PI;

    cg.refdef.fov_x    = fovX;
    cg.refdef.fov_y    = fovY;
    cg.fRefFovXCos     = cos(fovX / 114.0f);
    cg.fRefFovXSin     = sin(fovX / 114.0f);
    cg.fRefFovYCos     = cos(fovY / 114.0f);
    cg.fRefFovYSin     = sin(fovY / 114.0f);
    cg.zoomSensitivity = 1.0f;

    contents = CG_PointContents(cg.refdef.vieworg, -1);
    return (contents & (CONTENTS_WATER | CONTENTS_SLIME | CONTENTS_LAVA)) ? 1 : 0;
}

/*
==================
2D
==================
*/
static fontheader_t *CG_DemoEdFont(void)
{
    static fontheader_t *f = NULL;
    if (!f) {
        f = cgi.R_LoadFont("verdana-14");
    }
    return f;
}

static void CG_DemoEdText(const char *t, float px, float py, float scale, const float *rgba, qboolean centre)
{
    fontheader_t *font = CG_DemoEdFont();
    vec2_t        sc;
    float         tw;
    vec4_t        shadow = {0, 0, 0, rgba[3] * 0.8f};

    if (!font) {
        return;
    }
    tw    = (float)cgi.UI_FontStringWidth(font, t, -1) * scale;
    sc[0] = sc[1] = scale;
    if (centre) {
        px -= tw * 0.5f;
    }
    cgi.R_SetColor(shadow);
    cgi.R_DrawString(font, t, (px + 1.0f) / scale, (py + 1.0f) / scale, -1, sc);
    cgi.R_SetColor(rgba);
    cgi.R_DrawString(font, t, px / scale, py / scale, -1, sc);
    cgi.R_SetColor(NULL);
}

static void CG_DemoEdDrawNames(void)
{
    int    i;
    float  W = (float)cgs.glconfig.vidWidth, H = (float)cgs.glconfig.vidHeight;
    float  tx = tan(DEG2RAD(cg.refdef.fov_x * 0.5f)), ty = tan(DEG2RAD(cg.refdef.fov_y * 0.5f));
    float  scale = H / 720.0f;
    vec3_t head, d;
    trace_t tr;

    if (scale < 0.75f) {
        scale = 0.75f;
    }
    for (i = 0; i < MAX_CLIENTS; i++) {
        centity_t *cent = &cg_entities[i];
        float      fx, fy, fz, dist, a;
        vec4_t     col;

        if (!cent->currentValid || cent->currentState.eType != ET_PLAYER) {
            continue;
        }
        if (i == cg.snap->ps.clientNum && !CG_DemoCamActive()) {
            continue; // we are looking through his eyes
        }
        if (cent->currentState.renderfx & RF_DONTDRAW) {
            continue;
        }
        VectorCopy(cent->lerpOrigin, head);
        head[2] += 82.0f;
        VectorSubtract(head, cg.refdef.vieworg, d);
        fx = DotProduct(d, cg.refdef.viewaxis[0]);
        if (fx < 8.0f) {
            continue;
        }
        fy   = -DotProduct(d, cg.refdef.viewaxis[1]) / fx / tx;
        fz   = -DotProduct(d, cg.refdef.viewaxis[2]) / fx / ty;
        dist = VectorLength(d);
        if (fy < -1.05f || fy > 1.05f || fz < -1.05f || fz > 1.05f || dist > 6000.0f) {
            continue;
        }
        a = 1.0f - (dist - 1500.0f) / 4500.0f;
        if (a > 1.0f) {
            a = 1.0f;
        }
        cgi.CM_BoxTrace(&tr, cg.refdef.vieworg, head, vec3_origin, vec3_origin, 0, MASK_SOLID, qfalse);
        if (tr.fraction < 0.98f) {
            a *= 0.35f; // behind a wall: still findable, clearly not in view
        }
        if (cent->currentState.eFlags & EF_AXIS) {
            VectorSet(col, 1.0f, 0.62f, 0.55f);
        } else if (cent->currentState.eFlags & EF_ALLIES) {
            VectorSet(col, 0.62f, 0.80f, 1.0f);
        } else {
            VectorSet(col, 0.95f, 0.93f, 0.86f);
        }
        col[3] = a;
        CG_DemoEdText(
            cg.clientinfo[i].name[0] ? cg.clientinfo[i].name : va("player %d", i), W * 0.5f * (1.0f + fy),
            H * 0.5f * (1.0f + fz) - 18.0f * scale, scale * (dist < 600.0f ? 1.0f : 0.85f), col, qtrue
        );
    }
}

static void CG_DemoEdDrawStatus(void)
{
    static const char *hint =
        "KP0 free cam    KP4 / KP6  -5 s / +5 s    KP5 step    Del pause    KP1 2 3 7 9 speed    KP Enter waypoint    KP+ path    KP- HUD    KP* / KP/ filter";
    fontheader_t *font = CG_DemoEdFont();
    float         W = (float)cgs.glconfig.vidWidth, H = (float)cgs.glconfig.vidHeight;
    float         s1 = H / 720.0f, s2, w1, w2, pw, ph, px, py;
    const char   *line;
    vec4_t        bg   = {0.04f, 0.04f, 0.05f, 0.62f};
    vec4_t        edge = {1.0f, 0.86f, 0.52f, 0.55f};
    vec4_t        gold = {1.0f, 0.86f, 0.52f, 1.0f};
    vec4_t        dim  = {0.86f, 0.84f, 0.79f, 0.85f};

    if (!s_demoStatus->string[0] || !font) {
        return;
    }
    if (s1 < 0.8f) {
        s1 = 0.8f;
    }
    s2 = s1 * 0.72f;
    line = va("DEMO   %s", s_demoStatus->string);
    w1   = (float)cgi.UI_FontStringWidth(font, line, -1) * s1;
    w2   = (float)cgi.UI_FontStringWidth(font, hint, -1) * s2;
    if (w2 > W - 40.0f) {
        s2 *= (W - 40.0f) / w2;
        w2 = W - 40.0f;
    }
    pw = (w1 > w2 ? w1 : w2) + 36.0f * s1;
    ph = 46.0f * s1;
    px = (W - pw) * 0.5f;
    py = H - ph - 14.0f * s1;
    cgi.R_SetColor(bg);
    cgi.R_DrawBox(px, py, pw, ph);
    cgi.R_SetColor(edge);
    cgi.R_DrawBox(px, py, pw, 2.0f);
    cgi.R_SetColor(NULL);
    CG_DemoEdText(line, W * 0.5f, py + 5.0f * s1, s1, gold, qtrue);
    CG_DemoEdText(hint, W * 0.5f, py + 27.0f * s1, s2, dim, qtrue);
}

/*
==================
CG_DemoEdNeutralLens - the recorded player's own lens / injury effects (blood on the lens, hit flash,
low-health wash and dizzy double vision, suppression, heat, muzzle haze, rain on the lens, DoF, DBNO
view) must never sit on the free camera. The cgame re-publishes these during its 2D pass, after the
view is set up, and the renderer reads them when the frame is executed - so this runs LAST in the
cgame frame (end of CG_Draw2D, every HUD mode). coop_dbnoView comes from the server only on change,
so it is saved on the way in and given back when the free camera goes off.
==================
*/
static void CG_DemoEdSetIf(const char *name, const char *val)
{
    cvar_t *c = cgi.Cvar_Get(name, val, 0);
    if (c && strcmp(c->string, val)) {
        cgi.Cvar_Set(name, val);
    }
}

void CG_DemoEdNeutralLens(void)
{
    static qboolean s_was = qfalse;
    static char     s_dbno[32];
    cvar_t         *d;

    if (!CG_DemoCamActive()) {
        if (s_was) {
            s_was = qfalse;
            cgi.Cvar_Set("coop_dbnoView", s_dbno);
        }
        return;
    }
    if (!s_was) {
        s_was = qtrue;
        d     = cgi.Cvar_Get("coop_dbnoView", "0", 0);
        Q_strncpyz(s_dbno, d ? d->string : "0", sizeof(s_dbno));
    }
    CG_DemoEdSetIf("r_ppHealthFrac", "1");
    CG_DemoEdSetIf("r_ppHit", "0");
    CG_DemoEdSetIf("r_ppBlood", "0");
    CG_DemoEdSetIf("r_ppSuppress", "0");
    CG_DemoEdSetIf("r_ppHeat", "0");
    CG_DemoEdSetIf("r_ppMuzzleAs", "0");
    CG_DemoEdSetIf("r_ppMuzzleHeat", "0");
    CG_DemoEdSetIf("r_ppRainWet", "0");
    CG_DemoEdSetIf("r_ppUnderwaterAir", "0");
    CG_DemoEdSetIf("r_dofBlur", "0");
    CG_DemoEdSetIf("r_ppScopeState", "0");
    CG_DemoEdSetIf("coop_dbnoView", "0");
}

/*
==================
CG_DemoEdDraw2D - names-only / no-HUD modes: qtrue when it drew the whole 2D frame
==================
*/
qboolean CG_DemoEdDraw2D(void)
{
    int mode;

    if (!cg.snap) {
        return qfalse;
    }
    mode = CG_DemoHudMode();
    if (mode == 1 && cg.demoPlayback) {
        CG_DemoEdDrawNames();
    }
    return mode != 0 ? qtrue : qfalse;
}

/*
==================
CG_DemoEdDrawFilterLabel - the filter's name for a moment after KP* / KP/ (full HUD only, so it never
lands in a clean recording). Real time, so it also fades while a demo is paused.
==================
*/
static void CG_DemoEdDrawFilterLabel(void)
{
    static cvar_t *pFilter = NULL;
    static int     s_mod = -1, s_until = 0;
    fontheader_t  *font = CG_DemoEdFont();
    float          W = (float)cgs.glconfig.vidWidth, H = (float)cgs.glconfig.vidHeight;
    float          s = H / 720.0f, w, pw, ph, px, py, a;
    int            n, now = cgi.Milliseconds();
    const char    *line;
    vec4_t         bg = {0.04f, 0.04f, 0.05f, 0.62f}, edge = {1.0f, 0.86f, 0.52f, 0.55f}, gold = {1.0f, 0.86f, 0.52f, 1.0f};

    if (!pFilter) {
        pFilter = cgi.Cvar_Get("r_ppFilter", "0", 0);
    }
    if (pFilter->modificationCount != s_mod) {
        if (s_mod != -1) {
            s_until = now + 1800;
        }
        s_mod = pFilter->modificationCount;
    }
    if (now >= s_until || !font) {
        return;
    }
    a = (s_until - now) < 450 ? (s_until - now) / 450.0f : 1.0f;
    n = pFilter->integer;
    if (n < 0 || n > HZM_NUM_FILTERS) {
        n = 0;
    }
    if (s < 0.8f) {
        s = 0.8f;
    }
    line = n ? va("FILTER   %s   %d / %d", hzmFilterNames[n], n, HZM_NUM_FILTERS) : "FILTER   OFF";
    w  = (float)cgi.UI_FontStringWidth(font, line, -1) * s;
    pw = w + 40.0f * s;
    ph = 26.0f * s;
    px = (W - pw) * 0.5f;
    py = H * 0.14f;
    bg[3] *= a;
    edge[3] *= a;
    gold[3] *= a;
    cgi.R_SetColor(bg);
    cgi.R_DrawBox(px, py, pw, ph);
    cgi.R_SetColor(edge);
    cgi.R_DrawBox(px, py + ph - 2.0f, pw, 2.0f);
    cgi.R_SetColor(NULL);
    CG_DemoEdText(line, W * 0.5f, py + 5.0f * s, s, gold, qtrue);
}

// end of the full-HUD CG_Draw2D: name tags while the free camera is up, and the status line
void CG_DemoEdDraw2DTail(void)
{
    if (cg.snap && CG_DemoHudMode() == 0) {
        CG_DemoEdDrawFilterLabel();
    }
    if (!cg.demoPlayback || !cg.snap || CG_DemoHudMode() != 0) {
        return;
    }
    if (CG_DemoCamActive()) {
        CG_DemoEdDrawNames();
    }
    CG_DemoEdDrawStatus();
}

/*
==================
commands from the client after a seek
==================
*/
void CG_DemoEdCs_f(void)
{
    int num;
    if (cgi.Argc() < 2) {
        return;
    }
    num = atoi(cgi.Argv(1));
    if (num < 0 || num >= MAX_CONFIGSTRINGS) {
        return;
    }
    cgi.GetGameState(&cgs.gameState);
    CG_ProcessConfigString(num, qfalse);
}

void CG_DemoEdReset_f(void)
{
    if (!cg.demoPlayback || cgi.Argc() < 3) {
        return;
    }
    cgi.GetGameState(&cgs.gameState);
    // transient effects belong to the time we left
    CG_RestartCommandManager();
    CG_InitMarks();
    CG_InitFootprints();
    CG_ClearSwipes();
    CG_ResetTempModels();
    CG_ResetVSSSources();
    CG_DemoEdResetSnapshots(atoi(cgi.Argv(1)), atoi(cgi.Argv(2)));
    cg.thisFrameTeleport = qtrue;
}
