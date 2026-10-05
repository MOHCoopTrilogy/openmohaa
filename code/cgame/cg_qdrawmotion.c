/*
===========================================================================
HZM coop [user 2026-10-05] QUICK-DRAW MOTION - the primary is pushed down and aside, the pistol comes up, and back.

"on quick draw ... it still looks really funky when the primary gets put aside ... make it work the way you would
 actually move a primary to pull a handgun out."  + "And ensure it works fluidly."

WHAT TODAY LOOKED LIKE (run qd1, docs/proposals/quickdraw_motion_2026-10-05): ~80 ms after the press the rifle AND both
hands vanished in one frame, the view stayed empty for ~145 ms, then the pistol rose on its pullout clip; on release the
pistol vanished in one frame and the view stayed empty ~240 ms before the rifle's raise clip. The view-space park
(CG_CoopQDrawParkInView) did not run at all.

THE MOTION. Every movement here is ONE kind of thing: a rotation of the first-person rig ABOUT THE EYE (plus a small
offset), driven by a closed-form front-loaded ease of time (QDM_Snap). Rotating about the eye means no vertex can come closer to the
camera than the authored clip already puts it - hand clearance is preserved by construction, not by tuning - and a
closed-form curve is a pure function of time: C1 (velocity never jumps), identical at 60 and 125+ fps; it is front-loaded
(quick start, long gentle settle), because a quick draw must read FASTER than a manual switch, never slower.
  ASIDE    key down (LOCAL, before the server answers): the long gun and both hands start down and to the support side
           at once - so the ~80 ms the server takes is already motion, not a hold. When the swap arrives the parked
           primary carries on along the SAME function of the SAME clock from the same pose (no pop for the gun), and
           leaves the view down-left. Its final pose is out of the line of sight.
  RAISE    the pistol in its one-handed idle (server coop_qdrawDrawAnim "idle", not the slow pullout clip) snapping up
           from low on the strong side in 0.18 s, overlapping the aside: in the sight picture as it becomes
           fire-ready (coop_qdrawDelay 0.18 s after the server's enter) - never SEEN ready before it can fire.
  HOLSTER  key up (local): the pistol drops away on the strong side ahead of the server's swap, so it leaves moving
           (the vanish is at the edge of the frame instead of mid-screen), then the primary comes back.
  REGRIP   the primary (server coop_qdrawReturnAnim "idle") comes up from the support side, where it went, in 0.23 s.
Every channel retargets from where it IS (press again mid-holster: the pistol comes back up; a refused draw: the gun
comes back up), so interruptions never snap.

Gameplay is untouched: the server's coop_qdrawDelay / coop_qdrawReturn timing is exactly as before. coop_qdrawMotion 0 =
the shipped look. coop_qdrawMotionDbg 1 = per-frame probe lines (QDM ...), 2 = the arm clearance line every frame.
===========================================================================
*/

#include "cg_local.h"

qboolean CG_QDMotion_Park(refEntity_t *ent, int iEntNum);
void     CG_QDMotion_SampleHeld(refEntity_t *ent, int iEntNum);
void     CG_QDMotion_ViewModel(refEntity_t *arms);
void     CG_QDMotion_Note(int iEntNum, const char *szTag, int iParked);
void     CG_QDMotion_KeyDown(void);
void     CG_QDMotion_KeyUp(void);
void     CG_QDMotion_StuffFastPath(const char *cmd);
qboolean CG_QDMotion_OneHanded(void);

// ------------------------------------------------------------------------------------------------ math
// THE SNAP CURVE [user 2026-10-05: "the whole point is to make it faster than switching weapons manually"].
// f(u) = 1 - (1-u)^4 (1+4u): f(0)=0, f(1)=1, f'(u) = 20u(1-u)^3 - zero velocity at BOTH ends (no velocity jump, the
// fluidity gate) but the peak speed is at u = 0.25, so 69% of the travel is done by u = 0.4 - a quick start and a
// long gentle settle (ease-out), not the slow ease-in of a spring from rest. A pure function of time: identical at
// 60 and 125+ fps.
static float QDM_Snap(float t, float T)
{
    float u, v;

    if (T <= 0.001f) {
        return 1.0f;   // a channel never targeted (zero-initialised): settled - never a 0/0
    }
    u = t / T;
    if (u <= 0.0f) {
        return 0.0f;
    }
    if (u >= 1.0f) {
        return 1.0f;
    }
    v = 1.0f - u;
    return 1.0f - v * v * v * v * (1.0f + 4.0f * u);
}

// view frame: [0] forward, [1] left, [2] up
static void QDM_ViewAxis(vec3_t v[3])
{
    AnglesToAxis(cg.refdefViewAngles, v);
}

// A DOF set: [0] pitch DOWN, [1] yaw LEFT, [2] roll, [3..5] offset (forward, left, up) - all in the VIEW frame,
// rotations about the eye: roll about forward first, then pitch about left, then yaw about up.
static const vec3_t kX = {1, 0, 0}, kY = {0, 1, 0}, kZ = {0, 0, 1};

static void QDM_Rot(const float *d, const vec3_t in, vec3_t out)
{
    vec3_t a, b;

    RotatePointAroundVector(a, kX, in, d[2]);
    RotatePointAroundVector(b, kY, a, d[0]);
    RotatePointAroundVector(out, kZ, b, d[1]);
}

static void QDM_RotInv(const float *d, const vec3_t in, vec3_t out)
{
    vec3_t a, b;

    RotatePointAroundVector(a, kZ, in, -d[1]);
    RotatePointAroundVector(b, kY, a, -d[0]);
    RotatePointAroundVector(out, kX, b, -d[2]);
}

static void QDM_Xform(const float *d, vec3_t o, vec3_t a[3])
{
    vec3_t t;
    int    i;

    QDM_Rot(d, o, t);
    VectorAdd(t, d + 3, o);
    for (i = 0; i < 3; i++) {
        QDM_Rot(d, a[i], t);
        VectorCopy(t, a[i]);
    }
}

static void QDM_XformInv(const float *d, vec3_t o, vec3_t a[3])
{
    vec3_t t;
    int    i;

    VectorSubtract(o, d + 3, t);
    QDM_RotInv(d, t, o);
    for (i = 0; i < 3; i++) {
        QDM_RotInv(d, a[i], t);
        VectorCopy(t, a[i]);
    }
}

static void QDM_ToView(const refEntity_t *e, vec3_t o, vec3_t a[3])
{
    vec3_t v[3], d;
    int    i, j;

    QDM_ViewAxis(v);
    VectorSubtract(e->origin, cg.refdef.vieworg, d);
    for (j = 0; j < 3; j++) {
        o[j] = DotProduct(d, v[j]);
    }
    for (i = 0; i < 3; i++) {
        for (j = 0; j < 3; j++) {
            a[i][j] = DotProduct(e->axis[i], v[j]);
        }
    }
}

static void QDM_FromView(refEntity_t *e, const vec3_t o, vec3_t a[3])
{
    vec3_t v[3];
    int    i;

    QDM_ViewAxis(v);
    VectorCopy(cg.refdef.vieworg, e->origin);
    for (i = 0; i < 3; i++) {
        VectorMA(e->origin, o[i], v[i], e->origin);
    }
    MatrixMultiply(a, v, e->axis);
    VectorCopy(e->origin, e->oldorigin);
    VectorCopy(e->origin, e->lightingOrigin);
}

static void QDM_Apply(refEntity_t *e, const float *d)
{
    vec3_t o, a[3];

    QDM_ToView(e, o, a);
    QDM_Xform(d, o, a);
    QDM_FromView(e, o, a);
}

static qboolean QDM_Zero(const float *d)
{
    int i;
    for (i = 0; i < 6; i++) {
        if (fabs(d[i]) > 0.001f) {
            return qfalse;
        }
    }
    return qtrue;
}

// ------------------------------------------------------------------------------------------------ channels
// value(t) = from + (to - from) * snap(t - t0). Retargeting starts from the CURRENT value, so nothing snaps.
typedef struct {
    float from[6], to[6];
    int   t0;
    float T;   // seconds
} qdmChan_t;

static void QDM_ChanValue(const qdmChan_t *c, float *out)
{
    float x = QDM_Snap((cg.time - c->t0) * 0.001f, c->T);
    int   i;

    for (i = 0; i < 6; i++) {
        out[i] = c->from[i] + (c->to[i] - c->from[i]) * x;
    }
}

static void QDM_ChanTarget(qdmChan_t *c, const float *to, float fMs)
{
    float cur[6];
    int   i;

    QDM_ChanValue(c, cur);
    for (i = 0; i < 6; i++) {
        c->from[i] = cur[i];
        c->to[i]   = to ? to[i] : 0.0f;
    }
    c->t0 = cg.time;
    c->T  = fMs * 0.001f;
}

static void QDM_ChanSet(qdmChan_t *c, const float *v)
{
    int i;
    for (i = 0; i < 6; i++) {
        c->from[i] = c->to[i] = v ? v[i] : 0.0f;
    }
    c->t0 = cg.time;
    c->T  = 0.0f;
}

static qboolean QDM_ChanTargetZero(const qdmChan_t *c)
{
    return QDM_Zero(c->to);
}

// ------------------------------------------------------------------------------------------------ cvars
static cvar_t *qdm_on, *qdm_dbg;
static cvar_t *qdm_aside, *qdm_asideMs, *qdm_raise, *qdm_raiseMs, *qdm_holster, *qdm_holsterMs;
static cvar_t *qdm_regrip, *qdm_regripMs, *qdm_refuseMs;

static void QDM_Cvars(void)
{
    if (qdm_on) {
        return;
    }
    qdm_on       = cgi.Cvar_Get("coop_qdrawMotion", "1", 0);
    qdm_dbg      = cgi.Cvar_Get("coop_qdrawMotionDbg", "0", 0);
    // "pitchDown yawLeft roll fwd left up" - where the primary goes (and comes back from): down and to the support side
    qdm_aside    = cgi.Cvar_Get("coop_qdrawAside", "50 48 18 0 6 -4", 0);
    // DURATIONS (ms) of the snap curve. Timed against the server's own clock so the look never delays the gun: the
    // pistol is fire-ready coop_qdrawDelay (0.18 s) after the server's enter, which is also the frame its swap is
    // sent, so the raise lasts that long from the swap: it settles as it becomes able to fire. Likewise the primary
    // (coop_qdrawReturn 0.25 s after the exit) - its regrip runs that long from the swap.
    qdm_asideMs  = cgi.Cvar_Get("coop_qdrawAsideMs", "240", 0);
    // the pistol's arc in from low on the strong side (on top of its pullout clip)
    qdm_raise    = cgi.Cvar_Get("coop_qdrawRaise", "17 -10 -6 0 -1 -1", 0);
    qdm_raiseMs  = cgi.Cvar_Get("coop_qdrawRaiseMs", "180", 0);
    // the pistol leaving on release, ahead of the server's swap
    qdm_holster  = cgi.Cvar_Get("coop_qdrawHolster", "46 -16 0 0 0 -2", 0);
    qdm_holsterMs = cgi.Cvar_Get("coop_qdrawHolsterMs", "100", 0);
    // the primary coming back up from the support side
    qdm_regrip   = cgi.Cvar_Get("coop_qdrawRegrip", "30 30 10 0 3 -2", 0);
    qdm_regripMs = cgi.Cvar_Get("coop_qdrawRegripMs", "250", 0);
    // a press the server refused (no loaded pistol, mid-reload past clip_fill, ...): bring the gun back after this
    qdm_refuseMs = cgi.Cvar_Get("coop_qdrawRefuseMs", "220", 0);   // + the current ping
}

static void QDM_Dof(cvar_t *c, float *d, const float *def)
{
    int i;
    if (!c || sscanf(c->string, "%f %f %f %f %f %f", &d[0], &d[1], &d[2], &d[3], &d[4], &d[5]) != 6) {
        for (i = 0; i < 6; i++) {
            d[i] = def[i];
        }
    }
}

static void QDM_DofAside(float *d)
{
    static const float def[6] = {50, 48, 18, 0, 6, -4};
    QDM_Dof(qdm_aside, d, def);
}

// ------------------------------------------------------------------------------------------------ state
#define QDM_HELD_SLOTS 8
static struct {
    // the weapons in the hands, view-relative and WITHOUT the aside applied - keyed by entity number, because reload
    // magazine props ride tag_weapon_right too and are drawn in the same frame
    int    heldEnt[QDM_HELD_SLOTS], heldTime[QDM_HELD_SLOTS];
    vec3_t heldO[QDM_HELD_SLOTS], heldA[QDM_HELD_SLOTS][3];
    // the aside actually applied to the arms this frame (and when) - the held sample is un-done through it
    float  rigAside[6];
    int    rigAsideTime;
    // the park
    int    parkEnt, parkLast;
    vec3_t baseO, baseA[3];
    // channels
    qdmChan_t aside, pistol, regrip;
    // draw state as the client sees it
    int    inDraw, drawItem, tEnter, tExit;
    int    tPress, released, tRelease, holsterOn;
} qdm;

static qboolean QDM_Fresh(int t, int ms)
{
    return (t > 0 && cg.time - t >= 0 && cg.time - t <= ms) ? qtrue : qfalse;
}

static qboolean QDM_LongGunInHand(void)
{
    int     cls;
    cvar_t *pMask;

    if (!cg.snap) {
        return qfalse;
    }
    cls   = cg.snap->ps.stats[STAT_EQUIPPED_WEAPON];
    pMask = cgi.Cvar_Get("coop_qdrawClasses", "14", 0);
    return ((cls & (pMask->integer ? pMask->integer : 14)) && !(cls & WEAPON_CLASS_ANY_ITEM)) ? qtrue : qfalse;
}

// ------------------------------------------------------------------------------------------------ the draw state
// The server publishes coop_qdrawOn as `stufftext "set coop_qdrawOn N"`, and a stuffed line is APPENDED to the command
// buffer - so whatever was already queued there (a bind or cfg with waits; the capture harness) held it back for its
// whole length: in run qd2 it stayed 0 through every draw, and the park, the ADS guard and the foley never ran. Called
// from the stufftext branch AFTER the statement filter has passed it; the exact form only, the value an integer.
void CG_QDMotion_StuffFastPath(const char *cmd)
{
    static const char kPre[] = "set coop_qdrawOn ";
    const char       *v;

    if (!cmd || Q_stricmpn(cmd, kPre, sizeof(kPre) - 1)) {
        return;
    }
    v = cmd + sizeof(kPre) - 1;
    while (*v == ' ' || *v == '"') {
        v++;
    }
    if ((*v < '0' || *v > '9') && *v != '-') {
        return;
    }
    cgi.Cvar_Set("coop_qdrawOn", va("%d", atoi(v)));
}

// [user 2026-10-05] "Quick drawing would also only be one handed": the support hand is on the slung primary, so the
// ADS two-hand pistol layer (the ADS agent's) must stay off for a quick-drawn pistol - aimed or not. True from the
// swap until the primary is back in the hands.
qboolean CG_QDMotion_OneHanded(void)
{
    return qdm.inDraw ? qtrue : qfalse;
}

// ------------------------------------------------------------------------------------------------ the keys
// Local, from the +coopsidearm / -coopsidearm relays in cg_consolecmds.c: the motion starts on the press itself.
void CG_QDMotion_KeyDown(void)
{
    float d[6];

    QDM_Cvars();
    if (!qdm_on->integer || !cg.snap) {
        return;
    }
    if (qdm.inDraw) {
        // pressed again while the pistol was going away: the server keeps the draw (held again before its exit
        // ran), so bring the pistol back up from wherever it is
        if (qdm.holsterOn) {
            QDM_DofAside(d);
            QDM_ChanTarget(&qdm.pistol, NULL, qdm_raiseMs->value);
            QDM_ChanTarget(&qdm.aside, d, qdm_asideMs->value);
        }
        qdm.released  = 0;
        qdm.holsterOn = 0;
        return;
    }
    if (!QDM_LongGunInHand() || !QDM_ChanTargetZero(&qdm.aside)) {
        return;
    }
    // the refusals the client can see for itself (CoopQDrawEnter refuses all of these): no dip at all
    if ((cg.snap->ps.pm_flags & (PMF_TURRET | PMF_SPECTATING | PMF_FROZEN | PMF_INTERMISSION | PMF_CAMERA_VIEW | PMF_NO_MOVE))
        || cg.snap->ps.stats[STAT_HEALTH] <= 0 || cg.snap->ps.stats[STAT_VEHICLE_MAX_HEALTH] > 0
        || cg.snap->ps.stats[STAT_CINEMATIC]) {
        return;
    }
    QDM_DofAside(d);
    QDM_ChanTarget(&qdm.aside, d, qdm_asideMs->value);
    qdm.tPress    = cg.time;
    qdm.released  = 0;
    qdm.holsterOn = 0;
}

void CG_QDMotion_KeyUp(void)
{
    QDM_Cvars();
    qdm.released = 1;
    qdm.tRelease = cg.time;
}

// ------------------------------------------------------------------------------------------------ the gun
// Every frame, for the LOCAL player's first-person weapon on tag_weapon_right, after every pose block has run.
void CG_QDMotion_SampleHeld(refEntity_t *ent, int iEntNum)
{
    vec3_t o, a[3];
    int    i, k = 0;

    QDM_Cvars();
    if (!qdm_on->integer) {
        return;
    }
    QDM_ToView(ent, o, a);
    if (qdm.rigAsideTime == cg.time) {
        QDM_XformInv(qdm.rigAside, o, a);   // the gun WITHOUT this frame's aside: the park re-applies it on the clock
    }
    for (i = 0; i < QDM_HELD_SLOTS; i++) {
        if (qdm.heldEnt[i] == iEntNum) {
            k = i;
            break;
        }
        if (qdm.heldTime[i] < qdm.heldTime[k]) {
            k = i;
        }
    }
    qdm.heldEnt[k]  = iEntNum;
    qdm.heldTime[k] = cg.time;
    VectorCopy(o, qdm.heldO[k]);
    AxisCopy(a, qdm.heldA[k]);
}

static int QDM_HeldSlot(int iEntNum)
{
    int i;
    for (i = 0; i < QDM_HELD_SLOTS; i++) {
        if (qdm.heldEnt[i] == iEntNum && QDM_Fresh(qdm.heldTime[i], 200)) {
            return i;
        }
    }
    return -1;
}

// distance from the eye to the gun's bore line, butt (-18) to muzzle (+34) - the clearance probe
static float QDM_EyeClear(const vec3_t o, vec3_t a[3])
{
    float best = 9999.0f, k;
    for (k = -18.0f; k <= 34.0f; k += 2.0f) {
        vec3_t p;
        float  d;
        VectorMA(o, k, a[0], p);
        d = VectorLength(p);
        if (d < best) {
            best = d;
        }
    }
    return best;
}

// The parked primary (tag_weapon_left, identified by entity number). Returns qfalse when the motion is off so the
// caller keeps the shipped placement.
qboolean CG_QDMotion_Park(refEntity_t *ent, int iEntNum)
{
    float  d[6];
    vec3_t o, a[3];

    QDM_Cvars();
    if (!qdm_on->integer) {
        return qfalse;
    }
    if (iEntNum != qdm.parkEnt || !QDM_Fresh(qdm.parkLast, 250)) {
        int k = QDM_HeldSlot(iEntNum);
        qdm.parkEnt = iEntNum;
        if (k >= 0) {
            // exactly where it was in the hands (minus the aside it was already riding)
            VectorCopy(qdm.heldO[k], qdm.baseO);
            AxisCopy(qdm.heldA[k], qdm.baseA);
        } else {
            // never seen in the hands (joined mid-draw, a third-person toggle): low and centred, sights up
            static const vec3_t fa = {18.0f, 3.0f, 0.0f};
            vec3_t              p[3];
            VectorSet(qdm.baseO, 14.0f, 2.0f, -13.0f);
            AnglesToAxis(fa, p);
            VectorCopy(p[0], qdm.baseA[0]);
            VectorNegate(p[1], qdm.baseA[1]);
            VectorNegate(p[2], qdm.baseA[2]);
        }
        if (QDM_ChanTargetZero(&qdm.aside)) {
            float t[6];
            QDM_DofAside(t);
            QDM_ChanTarget(&qdm.aside, t, qdm_asideMs->value);   // no local press seen
        }
    }
    qdm.parkLast = cg.time;

    QDM_ChanValue(&qdm.aside, d);
    VectorCopy(qdm.baseO, o);
    AxisCopy(qdm.baseA, a);
    QDM_Xform(d, o, a);
    QDM_FromView(ent, o, a);

    if (qdm_dbg->integer) {
        cgi.Printf("^~^~^ QDM PARK t=%d ent=%d d=(%.2f %.2f %.2f %.2f %.2f %.2f) o=(%.2f %.2f %.2f) clear=%.1f\n", cg.time,
                   iEntNum, d[0], d[1], d[2], d[3], d[4], d[5], o[0], o[1], o[2], QDM_EyeClear(o, a));
    }
    return qtrue;
}

// The probe (coop_qdrawMotionDbg 1): every local first-person attachment on a weapon tag, printed when its
// (entity, tag, parked) triple changes.
void CG_QDMotion_Note(int iEntNum, const char *szTag, int iParked)
{
    static int  s_ent[8] = {-1, -1, -1, -1, -1, -1, -1, -1}, s_par[8], s_time[8];
    static char s_tag[8][24];
    int         i, k = 0;

    QDM_Cvars();
    if (!qdm_dbg->integer) {
        return;
    }
    for (i = 0; i < 8; i++) {
        if (s_ent[i] == iEntNum) {
            k = i;
            break;
        }
        if (s_time[i] < s_time[k]) {
            k = i;
        }
    }
    if (s_ent[k] != iEntNum || s_par[k] != iParked || Q_stricmp(s_tag[k], szTag ? szTag : "")) {
        cgi.Printf("^~^~^ QDM NOTE t=%d ent=%d tag=%s parked=%d on=%s item=%d\n", cg.time, iEntNum, szTag ? szTag : "-",
                   iParked, cgi.Cvar_Get("coop_qdrawOn", "0", 0)->string, cg.snap ? cg.snap->ps.activeItems[1] : -9);
    }
    s_ent[k]  = iEntNum;
    s_par[k]  = iParked;
    s_time[k] = cg.time;
    Q_strncpyz(s_tag[k], szTag ? szTag : "", sizeof(s_tag[k]));
}

// HAND CLEARANCE PROBE: the nearest point IN FRONT OF THE EYE on either arm's forearm->hand->fingertip chain, from the
// posed bones of the arms as drawn this frame. Bones run down the middle of the limb, so mesh clearance is about this
// minus 2 (forearm/hand half-thickness).
static void QDM_ArmClear(refEntity_t *arms, const char *why)
{
    static const char *kB[2][3] = {{"Bip01 L Forearm", "Bip01 L Hand", "Bip01 L Finger12"},
                                   {"Bip01 R Forearm", "Bip01 R Hand", "Bip01 R Finger12"}};
    static void       *s_tiki = NULL;
    static int         s_tag[2][3];
    vec3_t             v[3], P[3], d, rh = {0, 0, 0};
    float              best = 9999.0f;
    int                side, j, i, n;

    if (!arms || !arms->tiki) {
        return;
    }
    if (s_tiki != (void *)arms->tiki) {
        for (side = 0; side < 2; side++) {
            for (j = 0; j < 3; j++) {
                s_tag[side][j] = cgi.Tag_NumForName(arms->tiki, kB[side][j]);
            }
        }
        s_tiki = (void *)arms->tiki;
    }
    QDM_ViewAxis(v);
    for (side = 0; side < 2; side++) {
        for (j = 0; j < 3; j++) {
            orientation_t or;
            VectorCopy(arms->origin, P[j]);
            if (s_tag[side][j] >= 0) {
                or = cgi.TIKI_Orientation(arms, s_tag[side][j]);
                for (i = 0; i < 3; i++) {
                    VectorMA(P[j], or.origin[i], arms->axis[i], P[j]);
                }
            }
        }
        if (side == 1) {   // the right (trigger) hand in the view frame: time-to-sight-picture is measured on it
            VectorSubtract(P[1], cg.refdef.vieworg, d);
            rh[0] = DotProduct(d, v[0]);
            rh[1] = DotProduct(d, v[1]);
            rh[2] = DotProduct(d, v[2]);
        }
        for (j = 0; j < 2; j++) {
            for (n = 0; n <= 8; n++) {
                vec3_t q;
                float  f = n / 8.0f, fwd, dist;
                for (i = 0; i < 3; i++) {
                    q[i] = P[j][i] + (P[j + 1][i] - P[j][i]) * f;
                }
                VectorSubtract(q, cg.refdef.vieworg, d);
                fwd  = DotProduct(d, v[0]);
                dist = VectorLength(d);
                if (fwd > 0.0f && dist < best) {
                    best = dist;
                }
            }
        }
    }
    cgi.Printf("^~^~^ QDM ARMS t=%d %s clear=%.2f anim=%d item=%d rh=(%.2f %.2f %.2f)\n", cg.time, why, best,
               cg.snap ? cg.snap->ps.iViewModelAnim : -1, cg.snap ? cg.snap->ps.activeItems[1] : -1, rh[0], rh[1], rh[2]);
}

// ------------------------------------------------------------------------------------------------ the arms
// After CG_OffsetFirstPersonView, so it rides on top of every other offset (bob, ADS, recoil). The enter and the exit
// are detected HERE, on the same snapshot that swaps the view model, so an offset is on from the very first frame of
// the new clip - never one frame late.
//   enter: coop_qdrawOn becomes > 0 (published on the same server frame as the swap)
//   exit:  the main-hand item changes away from the pistol the draw started with, or coop_qdrawOn goes back to 0
//          (the 0 is published one server frame AFTER the swap, so the item change is the earlier, exact edge)
void CG_QDMotion_ViewModel(refEntity_t *arms)
{
    static cvar_t *pOn = NULL, *pMinHold = NULL, *pSticky = NULL;
    int            iItem, iOn, k;
    qboolean       bProbe;
    float          d[6], z[6] = {0, 0, 0, 0, 0, 0};

    QDM_Cvars();
    if (!pOn) {
        pOn      = cgi.Cvar_Get("coop_qdrawOn", "0", 0);
        pMinHold = cgi.Cvar_Get("coop_qdrawMinHold", "0.30", 0);
        pSticky  = cgi.Cvar_Get("coop_qdrawSticky", "0", 0);
    }
    if (!cg.snap) {
        return;
    }
    iOn   = pOn->integer;
    iItem = cg.snap->ps.activeItems[1];

    if (iOn > 0 && !qdm.inDraw) {
        // ENTER: the pistol's clip starts now; it arcs in from low on the strong side
        qdm.inDraw   = 1;
        qdm.drawItem = iItem;
        qdm.tEnter   = cg.time;
        QDM_Dof(qdm_raise, d, z);
        QDM_ChanSet(&qdm.pistol, d);
        QDM_ChanTarget(&qdm.pistol, NULL, qdm_raiseMs->value);
        qdm.holsterOn = 0;
        if (QDM_ChanTargetZero(&qdm.aside)) {   // no local press (the draw started some other way)
            QDM_DofAside(d);
            QDM_ChanTarget(&qdm.aside, d, qdm_asideMs->value);
        }
    } else if (qdm.inDraw && (iOn <= 0 || iItem != qdm.drawItem)) {
        // EXIT: the primary is back in the hands - it comes up from where it went
        qdm.inDraw    = 0;
        qdm.tExit     = cg.time;
        qdm.holsterOn = 0;
        qdm.released  = 0;
        qdm.tPress    = 0;
        // the regrip starts FROM where the parked primary is on this frame (the aside channel, which the release has
        // already been swinging back up - see HOLSTER below): same rotation about the eye, same in-hand base pose,
        // so the primary carries on rising instead of restarting from below. A draw that ended without a release
        // (magazine spent, a script, death) still has the aside at its full value: that is simply further away.
        QDM_ChanValue(&qdm.aside, d);
        QDM_ChanSet(&qdm.regrip, d);
        QDM_ChanTarget(&qdm.regrip, NULL, qdm_regripMs->value);
        QDM_ChanSet(&qdm.aside, NULL);
        QDM_ChanSet(&qdm.pistol, NULL);
    }

    if (!qdm.inDraw && !QDM_ChanTargetZero(&qdm.aside) && qdm.tPress > 0
        && cg.time - qdm.tPress > (int)qdm_refuseMs->value + (cg.snap->ping > 0 ? (cg.snap->ping < 400 ? cg.snap->ping : 400) : 0)) {
        // the server never swapped: the draw was refused - bring the gun back up from where it is
        QDM_ChanTarget(&qdm.aside, NULL, qdm_regripMs->value);
        qdm.tPress = 0;
    }
    if (qdm.inDraw && qdm.released && !qdm.holsterOn && !pSticky->integer) {
        // the server exits at max(release, its enter + coop_qdrawMinHold); start a beat early - the pistol only waits
        // out of view if the swap is late, it never sits mid-screen
        int tAt = qdm.tEnter + (int)(pMinHold->value * 1000.0f) - 100;
        if (cg.time >= tAt && cg.time >= qdm.tRelease) {
            QDM_Dof(qdm_holster, d, z);
            QDM_ChanTarget(&qdm.pistol, d, qdm_holsterMs->value);
            // ...and at the same instant the parked primary starts back up from the support side toward the regrip
            // pose, so the two cross: the pistol leaves at the lower right while the primary rises at the lower left
            // (toward HALF the regrip offset: the primary is entering the frame edge by the time the swap lands)
            QDM_Dof(qdm_regrip, d, z);
            for (k = 0; k < 6; k++) {
                d[k] *= 0.45f;
            }
            QDM_ChanTarget(&qdm.aside, d, qdm_holsterMs->value * 1.6f);
            qdm.holsterOn = 1;
        }
    }
    if (qdm.inDraw && qdm.holsterOn && cg.time - qdm.pistol.t0 > 900) {
        // released, dropped, and the server kept the draw anyway (a lost edge): come back up rather than hide forever
        QDM_DofAside(d);
        QDM_ChanTarget(&qdm.pistol, NULL, qdm_raiseMs->value);
        QDM_ChanTarget(&qdm.aside, d, qdm_asideMs->value);
        qdm.holsterOn = 0;
        qdm.released  = 0;
    }

    bProbe = (qdm_dbg->integer >= 2 || (qdm_dbg->integer && (qdm.inDraw || QDM_Fresh(qdm.tExit, 1500) || QDM_Fresh(qdm.tPress, 1500))))
               ? qtrue
               : qfalse;
    if (!qdm_on->integer) {
        if (bProbe) {
            QDM_ArmClear(arms, "today");
        }
        return;
    }

    if (!qdm.inDraw) {
        QDM_ChanValue(&qdm.aside, d);
        if (!QDM_Zero(d)) {
            QDM_Apply(arms, d);   // the long gun AND both hands go down and aside together, before the swap
            memcpy(qdm.rigAside, d, sizeof(d));
            qdm.rigAsideTime = cg.time;
        }
        QDM_ChanValue(&qdm.regrip, d);
        if (!QDM_Zero(d)) {
            QDM_Apply(arms, d);
        }
    } else {
        QDM_ChanValue(&qdm.pistol, d);
        if (!QDM_Zero(d)) {
            QDM_Apply(arms, d);
        }
    }
    if (bProbe) {
        float a[6], p[6], r[6];
        QDM_ChanValue(&qdm.aside, a);
        QDM_ChanValue(&qdm.pistol, p);
        QDM_ChanValue(&qdm.regrip, r);
        cgi.Printf("^~^~^ QDM VM t=%d on=%d item=%d draw=%d rel=%d hol=%d aside=(%.1f %.1f %.1f) pistol=(%.1f %.1f %.1f) regrip=(%.1f %.1f %.1f)\n",
                   cg.time, iOn, iItem, qdm.inDraw, qdm.released, qdm.holsterOn, a[0], a[1], a[2], p[0], p[1], p[2], r[0],
                   r[1], r[2]);
        QDM_ArmClear(arms, "new");
    }
}
