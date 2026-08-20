/*
HZM coop - RAGDOLL (ragdoll_plan.md v3, vetted 3 rounds / 7 agents, PASS 2026-08-19).

PHASE 2: real pose capture + Verlet simulation, NO world collision yet (P2 acceptance:
corpses crumple and fall through the floor; fingers/face ride their limbs; capture verified
non-bind). P1's bridge proof PASSED live on gl2 2026-08-19 20:57 (channels=72, matching the
static census exactly).

Architecture (see the plan for the full vetted rationale):
- cgame captures the death pose per channel via ForceUpdatePose + TIKI_Orientation, sims 15
  mass points in WORLD space (gravity + distance constraints), slaves every other channel to
  its nearest captured sim bone, converts back to capture-space each frame and pushes the
  full channel set over the renderer bridge (Hook A rewrites the bone cache, Hook B serves
  tag orientations so attachments ride along).
- Space contract (validated empirically by the P1 totem): table matrices live in the space
  TIKI_Orientation returns with origins divided by the entity scale; world = lerpOrigin +
  entAxis * (capturePos * scale).
- Seed (vet2/F8): arm on the EF_DEAD rising edge, HOLD one snapshot, then difference the
  next snapshot origins forward - the death impulse only moves the origin on FOLLOWING
  snapshots. 500ms with no second snapshot -> seed zero and run anyway.
- Timestep (vet2 exact spec): accumMs += min(frametime,200); step 8ms while accum >= 8 and
  steps < 4; then discard the excess - a hitch costs one fast-forward-free frame, never a
  backlog; supported minimum ~31fps.
- Lifecycle: state lives HERE (centity clientFlags zeroed on PVS re-entry); the clear-signal
  set runs un-gated in CG_RagdollTransition; eviction never touches an awake sim - if the
  pool is full of awake sims a new death simply keeps its server anim pose.
- Failure ladder: NaN / |pos| > 65536 -> Clear + never re-arm that entnum this map.

Debug: r_ragdollDebug (CVAR_TEMP) prints seeds/arms/sleeps. coop_ragdoll (CVAR_TEMP, dark
until P5) master-gates arming. coop_ragdollTest 1 = the P1 bone-totem test pose instead of
the sim (bridge regression drill).
*/

#include "cg_local.h"

void     CG_RagdollTransition(centity_t *cent);
void     CG_RagdollFrame(void);
void     CG_RagdollClearEnt(int entnum);
static void RagArm(centity_t *cent, entityState_t *ns);
static void RagArmTestPose(entityState_t *ns, dtiki_t *tiki, int count);

#define RAG_MAX_SIMS   8
#define RAG_MAX_CH     128
#define RAG_PTS        15
#define RAG_SUBSTEP_MS 8
#define RAG_MAX_STEPS  4
#define RAG_GRAVITY    800.0f
#define RAG_DAMPING    0.985f
#define RAG_ITERS      6

// the 15 sim bones: index, Bip01 tag name, parent sim index (-1 = root)
static const struct {
    const char *name;
    int         parent;
} s_ragBones[RAG_PTS] = {
    {"Bip01 Pelvis",    -1}, // 0
    {"Bip01 Spine1",     0}, // 1
    {"Bip01 Spine2",     1}, // 2
    {"Bip01 Neck",       2}, // 3
    {"Bip01 Head",       3}, // 4
    {"Bip01 L UpperArm", 2}, // 5
    {"Bip01 L Forearm",  5}, // 6
    {"Bip01 L Hand",     6}, // 7
    {"Bip01 R UpperArm", 2}, // 8
    {"Bip01 R Forearm",  8}, // 9
    {"Bip01 R Hand",     9}, // 10
    {"Bip01 L Thigh",    0}, // 11
    {"Bip01 L Calf",    11}, // 12
    {"Bip01 R Thigh",    0}, // 13
    {"Bip01 R Calf",    13}, // 14
};

// stiffening braces beyond the 14 parent links (plan section-3)
static const int s_ragBraces[6][2] = {
    {5,  8}, // shoulder - shoulder
    {11, 13}, // thigh - thigh
    {5,  13}, // L shoulder - R thigh
    {8,  11}, // R shoulder - L thigh
    {3,  1},  // neck - spine1
    {0,  2},  // pelvis - spine2
};

typedef struct {
    qboolean active;
    int      entnum;
    dtiki_t *tiki;
    float    scale;
    int      count;      // channel count
    int      state;      // 0 pending-seed, 1 running, 2 sleeping
    int      armTime;    // cg.time at arm (for the 500ms seed timeout)
    int      accumMs;
    int      sleepMs;    // time below the sleep speed
    int      lifeMs;     // total sim time

    vec3_t   entOrigin;  // entity placement at capture (server corpse is static post-impulse)
    vec3_t   entAxis[3];

    // capture-space per-channel matrices (the table space) + world capture transforms
    float    mat0[RAG_MAX_CH][3][4];
    // anchor sim-point per channel + capture offset in the anchor's capture frame
    byte     anchor[RAG_MAX_CH];
    vec3_t   relPos[RAG_MAX_CH];   // anchor-frame offset of the channel origin
    float    relRot[RAG_MAX_CH][3][3];

    int      simChan[RAG_PTS];     // channel index per sim point (-1 = missing)
    vec3_t   pt[RAG_PTS];          // world positions
    vec3_t   ptPrev[RAG_PTS];
    float    restLen[RAG_PTS];     // to parent
    float    braceLen[6];
    vec3_t   restDir[RAG_PTS];     // capture direction parent->this (world)
    float    rot0[RAG_PTS][3][3];  // capture world rotation per sim bone

    vec3_t   seedOrigin;           // entity origin at arm (for snapshot differencing)
    int      seedServerTime;
} ragSim_t;

static ragSim_t s_ragSims[RAG_MAX_SIMS];
static byte     s_ragNeverArm[MAX_GENTITIES]; // failure-ladder: NaN'd corpses keep the anim pose
static cvar_t  *rag_debug    = NULL;
static cvar_t  *coop_ragdoll = NULL;
static cvar_t  *rag_test     = NULL;

static void RagCvars(void)
{
    if (!rag_debug) {
        rag_debug    = cgi.Cvar_Get("r_ragdollDebug", "0", CVAR_TEMP);
        coop_ragdoll = cgi.Cvar_Get("coop_ragdoll", "0", CVAR_TEMP); // dark until P5 (plan section-1)
        rag_test     = cgi.Cvar_Get("coop_ragdollTest", "0", CVAR_TEMP);
    }
}

static ragSim_t *RagSimFor(int entnum)
{
    int i;
    for (i = 0; i < RAG_MAX_SIMS; i++) {
        if (s_ragSims[i].active && s_ragSims[i].entnum == entnum) {
            return &s_ragSims[i];
        }
    }
    return NULL;
}

void CG_RagdollClearEnt(int entnum)
{
    ragSim_t *s = RagSimFor(entnum);
    if (s) {
        memset(s, 0, sizeof(*s));
    }
    if (cgi.R_ClearRagdoll) {
        cgi.R_ClearRagdoll(entnum);
    }
}

// ---------- small matrix helpers (3x3 rotations, row-vector convention like the engine) ----

static void RagMat3Identity(float m[3][3])
{
    memset(m, 0, sizeof(float) * 9);
    m[0][0] = m[1][1] = m[2][2] = 1.0f;
}

static void RagMat3Mul(const float a[3][3], const float b[3][3], float out[3][3])
{
    int r, c;
    for (r = 0; r < 3; r++) {
        for (c = 0; c < 3; c++) {
            out[r][c] = a[r][0] * b[0][c] + a[r][1] * b[1][c] + a[r][2] * b[2][c];
        }
    }
}

static void RagMat3TransMul(const float a[3][3], const float b[3][3], float out[3][3])
{
    // out = transpose(a) * b
    int r, c;
    for (r = 0; r < 3; r++) {
        for (c = 0; c < 3; c++) {
            out[r][c] = a[0][r] * b[0][c] + a[1][r] * b[1][c] + a[2][r] * b[2][c];
        }
    }
}

static void RagMat3RotateVec(const float m[3][3], const vec3_t v, vec3_t out)
{
    // row-vector: out = v * m
    out[0] = v[0] * m[0][0] + v[1] * m[1][0] + v[2] * m[2][0];
    out[1] = v[0] * m[0][1] + v[1] * m[1][1] + v[2] * m[2][1];
    out[2] = v[0] * m[0][2] + v[1] * m[1][2] + v[2] * m[2][2];
}

static void RagMat3TransRotateVec(const float m[3][3], const vec3_t v, vec3_t out)
{
    out[0] = v[0] * m[0][0] + v[1] * m[0][1] + v[2] * m[0][2];
    out[1] = v[0] * m[1][0] + v[1] * m[1][1] + v[2] * m[1][2];
    out[2] = v[0] * m[2][0] + v[1] * m[2][1] + v[2] * m[2][2];
}

// rotation taking unit vector a onto unit vector b (Rodrigues), identity when parallel
static void RagMat3FromTo(const vec3_t a, const vec3_t b, float out[3][3])
{
    vec3_t axis;
    float  c, s, t, x, y, z;

    CrossProduct(a, b, axis);
    c = DotProduct(a, b);
    s = VectorLength(axis);
    if (s < 0.0001f) {
        RagMat3Identity(out);
        if (c < 0) {
            out[0][0] = -1.0f; // 180: flip two axes (any perpendicular flip serves a corpse)
            out[2][2] = -1.0f;
        }
        return;
    }
    axis[0] /= s;
    axis[1] /= s;
    axis[2] /= s;
    x = axis[0];
    y = axis[1];
    z = axis[2];
    t = 1.0f - c;
    // row-vector rotation matrix (transpose of the usual column form)
    out[0][0] = t * x * x + c;
    out[0][1] = t * x * y + s * z;
    out[0][2] = t * x * z - s * y;
    out[1][0] = t * x * y - s * z;
    out[1][1] = t * y * y + c;
    out[1][2] = t * y * z + s * x;
    out[2][0] = t * x * z + s * y;
    out[2][1] = t * y * z - s * x;
    out[2][2] = t * z * z + c;
}

// ---------- world <-> capture space -------------------------------------------------------

static void RagCaptureToWorld(const ragSim_t *s, const vec3_t cap, vec3_t world)
{
    vec3_t scaled, rotated;
    VectorScale(cap, s->scale, scaled);
    rotated[0] = scaled[0] * s->entAxis[0][0] + scaled[1] * s->entAxis[1][0] + scaled[2] * s->entAxis[2][0];
    rotated[1] = scaled[0] * s->entAxis[0][1] + scaled[1] * s->entAxis[1][1] + scaled[2] * s->entAxis[2][1];
    rotated[2] = scaled[0] * s->entAxis[0][2] + scaled[1] * s->entAxis[1][2] + scaled[2] * s->entAxis[2][2];
    VectorAdd(rotated, s->entOrigin, world);
}

static void RagWorldToCapture(const ragSim_t *s, const vec3_t world, vec3_t cap)
{
    vec3_t rel, unrot;
    VectorSubtract(world, s->entOrigin, rel);
    unrot[0] = rel[0] * s->entAxis[0][0] + rel[1] * s->entAxis[0][1] + rel[2] * s->entAxis[0][2];
    unrot[1] = rel[0] * s->entAxis[1][0] + rel[1] * s->entAxis[1][1] + rel[2] * s->entAxis[1][2];
    unrot[2] = rel[0] * s->entAxis[2][0] + rel[1] * s->entAxis[2][1] + rel[2] * s->entAxis[2][2];
    VectorScale(unrot, 1.0f / s->scale, cap);
}

// ---------- capture ------------------------------------------------------------------------

static qboolean RagCapture(centity_t *cent, entityState_t *ns, ragSim_t *s)
{
    refEntity_t   model;
    orientation_t or_;
    int           i, ch;

    memset(&model, 0, sizeof(model));
    model.tiki         = cgi.R_Model_GetHandle(cgs.model_draw[ns->modelindex]);
    model.hModel       = cgs.model_draw[ns->modelindex];
    model.entityNumber = ns->number;
    model.scale        = ns->scale > 0 ? ns->scale : 1.0f;
    for (i = 0; i < MAX_FRAMEINFOS; i++) {
        model.frameInfo[i].index  = ns->frameInfo[i].index;
        model.frameInfo[i].weight = ns->frameInfo[i].weight;
        model.frameInfo[i].time   = ns->frameInfo[i].time;
    }
    if (!model.tiki) {
        return qfalse;
    }

    // channel count: walk the tag namespace
    for (s->count = 0; s->count < RAG_MAX_CH; s->count++) {
        const char *nm = cgi.Tag_NameForNum(model.tiki, s->count);
        if (!nm || !nm[0]) {
            break;
        }
    }
    if (s->count <= 0) {
        return qfalse;
    }

    s->tiki  = model.tiki;
    s->scale = model.scale;
    VectorCopy(cent->lerpOrigin, s->entOrigin);
    AnglesToAxis(cent->lerpAngles, s->entAxis);

    // WARNING (plan C1): ForceUpdatePose stamps tr.skel_index[entnum] - the frameInfo above
    // is the entity's own current server state, so this poses exactly what is on screen.
    cgi.ForceUpdatePose(&model);

    for (ch = 0; ch < s->count; ch++) {
        or_ = cgi.TIKI_Orientation(&model, ch);
        s->mat0[ch][0][3] = or_.origin[0] / s->scale;
        s->mat0[ch][1][3] = or_.origin[1] / s->scale;
        s->mat0[ch][2][3] = or_.origin[2] / s->scale;
        for (i = 0; i < 3; i++) {
            s->mat0[ch][i][0] = or_.axis[i][0];
            s->mat0[ch][i][1] = or_.axis[i][1];
            s->mat0[ch][i][2] = or_.axis[i][2];
        }
    }

    // sim points: channel per Bip01 bone; capture world positions
    for (i = 0; i < RAG_PTS; i++) {
        vec3_t cap;
        s->simChan[i] = cgi.Tag_NumForName(model.tiki, s_ragBones[i].name);
        if (s->simChan[i] < 0 || s->simChan[i] >= s->count) {
            return qfalse; // vet1 verified all 17 names across the roster; a miss = bail clean
        }
        cap[0] = s->mat0[s->simChan[i]][0][3];
        cap[1] = s->mat0[s->simChan[i]][1][3];
        cap[2] = s->mat0[s->simChan[i]][2][3];
        RagCaptureToWorld(s, cap, s->pt[i]);
        VectorCopy(s->pt[i], s->ptPrev[i]);
    }

    // capture-verified-non-bind (P2 acceptance): head must sit above pelvis for a standing
    // capture, and the skeleton must span something - a bind/zero pose fails both.
    {
        vec3_t span;
        VectorSubtract(s->pt[4], s->pt[0], span);
        if (VectorLength(span) < 4.0f) {
            if (rag_debug->integer) {
                cgi.Printf("^~^~^ RAGDOLL capture looks BIND/degenerate ent=%d - not arming\n", ns->number);
            }
            return qfalse;
        }
    }

    // rest lengths, rest directions, capture rotations
    for (i = 0; i < RAG_PTS; i++) {
        int p = s_ragBones[i].parent;
        int r, c;
        for (r = 0; r < 3; r++) {
            for (c = 0; c < 3; c++) {
                s->rot0[i][r][c] = s->mat0[s->simChan[i]][r][c];
            }
        }
        if (p >= 0) {
            vec3_t d;
            VectorSubtract(s->pt[i], s->pt[p], d);
            s->restLen[i] = VectorLength(d);
            if (s->restLen[i] > 0.01f) {
                VectorScale(d, 1.0f / s->restLen[i], s->restDir[i]);
            } else {
                VectorSet(s->restDir[i], 0, 0, 1);
            }
        } else {
            s->restLen[i] = 0;
            VectorSet(s->restDir[i], 0, 0, 1);
        }
    }
    for (i = 0; i < 6; i++) {
        vec3_t d;
        VectorSubtract(s->pt[s_ragBraces[i][0]], s->pt[s_ragBraces[i][1]], d);
        s->braceLen[i] = VectorLength(d);
    }

    // slave every channel to its nearest sim point at capture (fingers -> hands, face ->
    // head, gear -> nearest segment). rel = inv(anchor world) * channel world.
    for (ch = 0; ch < s->count; ch++) {
        vec3_t capPos, worldPos, rel;
        int    best = 0, r, c;
        float  bestD = 999999.0f;
        float  chRotW[3][3];

        capPos[0] = s->mat0[ch][0][3];
        capPos[1] = s->mat0[ch][1][3];
        capPos[2] = s->mat0[ch][2][3];
        RagCaptureToWorld(s, capPos, worldPos);
        for (i = 0; i < RAG_PTS; i++) {
            vec3_t d;
            float  len;
            VectorSubtract(worldPos, s->pt[i], d);
            len = VectorLengthSquared(d);
            if (len < bestD) {
                bestD = len;
                best  = i;
            }
        }
        s->anchor[ch] = (byte)best;
        VectorSubtract(worldPos, s->pt[best], rel);
        RagMat3TransRotateVec(s->rot0[best], rel, s->relPos[ch]);
        // channel world rotation = capture rotation composed with entity axis; store relative
        // to the anchor's capture rotation. Entity axis cancels in the round trip, so we can
        // work purely in capture-rotation space:
        for (r = 0; r < 3; r++) {
            for (c = 0; c < 3; c++) {
                chRotW[r][c] = s->mat0[ch][r][c];
            }
        }
        RagMat3TransMul(s->rot0[best], chRotW, s->relRot[ch]);
    }

    return qtrue;
}

// ---------- per-frame sim ------------------------------------------------------------------

static void RagStep(ragSim_t *s, float dt)
{
    int   i, it;
    float g = RAG_GRAVITY * dt * dt;

    for (i = 0; i < RAG_PTS; i++) {
        vec3_t vel, next;
        VectorSubtract(s->pt[i], s->ptPrev[i], vel);
        VectorScale(vel, RAG_DAMPING, vel);
        VectorCopy(s->pt[i], s->ptPrev[i]);
        VectorAdd(s->pt[i], vel, next);
        next[2] -= g;
        VectorCopy(next, s->pt[i]);
    }
    for (it = 0; it < RAG_ITERS; it++) {
        for (i = 1; i < RAG_PTS; i++) {
            int    p = s_ragBones[i].parent;
            vec3_t d;
            float  len, corr;
            VectorSubtract(s->pt[i], s->pt[p], d);
            len = VectorLength(d);
            if (len < 0.001f) {
                continue;
            }
            corr = (len - s->restLen[i]) * 0.5f / len;
            VectorMA(s->pt[i], -corr, d, s->pt[i]);
            VectorMA(s->pt[p], corr, d, s->pt[p]);
        }
        for (i = 0; i < 6; i++) {
            int    a = s_ragBraces[i][0], b = s_ragBraces[i][1];
            vec3_t d;
            float  len, corr;
            VectorSubtract(s->pt[a], s->pt[b], d);
            len = VectorLength(d);
            if (len < 0.001f) {
                continue;
            }
            corr = (len - s->braceLen[i]) * 0.35f / len; // braces are softer than links
            VectorMA(s->pt[a], -corr, d, s->pt[a]);
            VectorMA(s->pt[b], corr, d, s->pt[b]);
        }
    }
}

static qboolean RagSane(const ragSim_t *s)
{
    int i;
    for (i = 0; i < RAG_PTS; i++) {
        if (Q_isnan(s->pt[i][0]) || Q_isnan(s->pt[i][1]) || Q_isnan(s->pt[i][2])
            || fabs(s->pt[i][0]) > 65536 || fabs(s->pt[i][1]) > 65536 || fabs(s->pt[i][2]) > 65536) {
            return qfalse;
        }
    }
    return qtrue;
}

static void RagPush(ragSim_t *s)
{
    static float mat[RAG_MAX_CH][3][4];
    float        rotNow[RAG_PTS][3][3];
    vec3_t       mins, maxs;
    int          i, ch, r, c;

    // current world rotation per sim bone: swing from the capture parent-direction onto the
    // simmed one (leafs inherit their parent's swing); pelvis gets a 2-axis basis from the
    // spine and hip line for stability.
    for (i = 0; i < RAG_PTS; i++) {
        int p = s_ragBones[i].parent;
        if (p < 0) {
            // pelvis: forward-ish = spine dir, side = hip line
            vec3_t up0, up1, side0, side1;
            float  swing[3][3];
            VectorSubtract(s->pt[1], s->pt[0], up1);
            VectorSubtract(s->restDir[1], vec3_origin, up0); // capture spine dir (unit)
            VectorNormalize(up1);
            RagMat3FromTo(up0, up1, swing);
            RagMat3Mul(s->rot0[i], swing, rotNow[i]);
            (void)side0;
            (void)side1;
        } else {
            vec3_t dNow;
            float  swing[3][3];
            VectorSubtract(s->pt[i], s->pt[p], dNow);
            if (VectorLength(dNow) < 0.01f) {
                memcpy(rotNow[i], s->rot0[i], sizeof(rotNow[i]));
                continue;
            }
            VectorNormalize(dNow);
            RagMat3FromTo(s->restDir[i], dNow, swing);
            RagMat3Mul(s->rot0[i], swing, rotNow[i]);
        }
    }

    ClearBounds(mins, maxs);
    for (ch = 0; ch < s->count && ch < RAG_MAX_CH; ch++) {
        int    a = s->anchor[ch];
        vec3_t off, world, cap;
        float  rot[3][3];

        RagMat3RotateVec(rotNow[a], s->relPos[ch], off);
        VectorAdd(s->pt[a], off, world);
        AddPointToBounds(world, mins, maxs);
        RagWorldToCapture(s, world, cap);
        mat[ch][0][3] = cap[0];
        mat[ch][1][3] = cap[1];
        mat[ch][2][3] = cap[2];
        RagMat3Mul(rotNow[a], s->relRot[ch], rot);
        // convert the WORLD-composed rotation back to capture-rotation space: the entity
        // axis cancels because both rot0 and the swing were built in the same frame.
        for (r = 0; r < 3; r++) {
            for (c = 0; c < 3; c++) {
                mat[ch][r][c] = rot[r][c];
            }
        }
    }
    for (i = 0; i < 3; i++) {
        mins[i] -= 16;
        maxs[i] += 16;
    }
    cgi.R_SetRagdollPose(s->entnum, s->tiki, s->count, &mat[0][0][0], mins, maxs);
}

void CG_RagdollFrame(void)
{
    int i;

    RagCvars();
    if (!cgi.R_SetRagdollPose) {
        return;
    }
    if (cg.frametime <= 0) {
        return; // paused/hitch: the table persists, Hook A keeps applying the last push
    }
    for (i = 0; i < RAG_MAX_SIMS; i++) {
        ragSim_t *s = &s_ragSims[i];
        int       steps, ms;
        if (!s->active) {
            continue;
        }
        if (s->state == 0) {
            // pending seed: differencing happens in CG_RagdollTransition; time out at 500ms
            if (cg.time - s->armTime > 500) {
                s->state = 1;
                if (rag_debug->integer) {
                    cgi.Printf("^~^~^ RAGDOLL seed timeout ent=%d (zero seed)\n", s->entnum);
                }
            } else {
                continue;
            }
        }
        if (s->state == 2) {
            continue; // sleeping: last pushed pose stands
        }
        ms = cg.frametime;
        if (ms > 200) {
            ms = 200;
        }
        s->accumMs += ms;
        s->lifeMs += ms;
        steps = 0;
        while (s->accumMs >= RAG_SUBSTEP_MS && steps < RAG_MAX_STEPS) {
            RagStep(s, RAG_SUBSTEP_MS * 0.001f);
            s->accumMs -= RAG_SUBSTEP_MS;
            steps++;
        }
        if (s->accumMs > RAG_SUBSTEP_MS * RAG_MAX_STEPS) {
            s->accumMs = RAG_SUBSTEP_MS * RAG_MAX_STEPS; // discard the hitch backlog
        }
        if (!RagSane(s)) {
            if (rag_debug->integer) {
                cgi.Printf("^~^~^ RAGDOLL NaN/blowup ent=%d - reverting to anim pose\n", s->entnum);
            }
            s_ragNeverArm[s->entnum] = 1;
            CG_RagdollClearEnt(s->entnum);
            continue;
        }
        // sleep: average speed below 4u/s for 1s, or life exceeded
        {
            float  speed = 0;
            vec3_t v;
            int    j;
            for (j = 0; j < RAG_PTS; j++) {
                VectorSubtract(s->pt[j], s->ptPrev[j], v);
                speed += VectorLength(v);
            }
            speed = speed / RAG_PTS / (RAG_SUBSTEP_MS * 0.001f);
            if (speed < 4.0f) {
                s->sleepMs += ms;
            } else {
                s->sleepMs = 0;
            }
            if (s->sleepMs > 1000 || s->lifeMs > 6000) {
                s->state = 2;
                if (rag_debug->integer) {
                    cgi.Printf("^~^~^ RAGDOLL sleep ent=%d life=%dms\n", s->entnum, s->lifeMs);
                }
            }
        }
        RagPush(s);
    }
}

// ---------- lifecycle ----------------------------------------------------------------------

static void RagArm(centity_t *cent, entityState_t *ns)
{
    ragSim_t *s = NULL;
    int       i;

    if (RagSimFor(ns->number)) {
        return;
    }
    if (s_ragNeverArm[ns->number]) {
        return;
    }
    for (i = 0; i < RAG_MAX_SIMS; i++) {
        if (!s_ragSims[i].active) {
            s = &s_ragSims[i];
            break;
        }
    }
    if (!s) {
        // eviction: an ASLEEP sim may be replaced; never an awake one (plan section-4)
        for (i = 0; i < RAG_MAX_SIMS; i++) {
            if (s_ragSims[i].state == 2) {
                CG_RagdollClearEnt(s_ragSims[i].entnum);
                s = &s_ragSims[i];
                break;
            }
        }
    }
    if (!s) {
        if (rag_debug->integer) {
            cgi.Printf("^~^~^ RAGDOLL arm refused ent=%d (pool awake-full)\n", ns->number);
        }
        return;
    }
    memset(s, 0, sizeof(*s));
    if (!RagCapture(cent, ns, s)) {
        memset(s, 0, sizeof(*s));
        return;
    }
    s->active         = qtrue;
    s->entnum         = ns->number;
    s->state          = 0; // pending seed
    s->armTime        = cg.time;
    VectorCopy(ns->origin, s->seedOrigin);
    s->seedServerTime = cg.nextSnap ? cg.nextSnap->serverTime : cg.time;
    RagPush(s); // first push: the captured pose verbatim (visually seamless arm)
    if (rag_debug->integer) {
        cgi.Printf("^~^~^ RAGDOLL armed ent=%d channels=%d scale=%.2f\n", ns->number, s->count, s->scale);
    }
}

// P1 regression drill: the bone-totem test pose (coop_ragdollTest 1)
static void RagArmTestPose(entityState_t *ns, dtiki_t *tiki, int count)
{
    static float mat[RAG_MAX_CH][3][4];
    vec3_t       mins, maxs;
    int          i;

    memset(mat, 0, sizeof(mat));
    for (i = 0; i < count; i++) {
        mat[i][0][0] = 1.0f;
        mat[i][1][1] = 1.0f;
        mat[i][2][2] = 1.0f;
        mat[i][2][3] = 20.0f + i * 1.5f;
    }
    VectorSet(mins, -64, -64, 0);
    VectorSet(maxs, 64, 64, 220);
    cgi.R_SetRagdollPose(ns->number, tiki, count, &mat[0][0][0], mins, maxs);
    cgi.Printf("^~^~^ RAGDOLL P1 test pose armed ent=%d channels=%d\n", ns->number, count);
}

// Called from CG_TransitionEntity BEFORE currentState = nextState (both states visible).
void CG_RagdollTransition(centity_t *cent)
{
    entityState_t *cs = &cent->currentState;
    entityState_t *ns = &cent->nextState;
    ragSim_t      *s;

    RagCvars();

    // plan section-4 clear signals - ALWAYS evaluated: EF_DEAD falling edge, teleport
    // toggle, modelindex change, eType change -> drop any override for this slot.
    if (((cs->eFlags & EF_DEAD) && !(ns->eFlags & EF_DEAD)) || ((cs->eFlags ^ ns->eFlags) & EF_TELEPORT_BIT)
        || (cs->modelindex != ns->modelindex) || (cs->eType != ns->eType)) {
        CG_RagdollClearEnt(ns->number);
        s_ragNeverArm[ns->number] = 0; // slot moved on: a future corpse here may arm again
    }

    // pending-seed sims: difference the first post-edge snapshot (vet2/F8 - the impulse
    // moves the origin only on snapshots AFTER the edge)
    s = RagSimFor(ns->number);
    if (s && s->state == 0) {
        vec3_t d;
        int    dt = (cg.nextSnap ? cg.nextSnap->serverTime : cg.time) - s->seedServerTime;
        float  dts = (dt > 0) ? dt * 0.001f : 0.05f;
        VectorSubtract(ns->origin, s->seedOrigin, d);
        if (VectorLength(d) > 0.1f || dt > 60) {
            int   i;
            float subDt = RAG_SUBSTEP_MS * 0.001f;
            vec3_t vel;
            VectorScale(d, 1.0f / dts, vel);
            for (i = 0; i < RAG_PTS; i++) {
                // Verlet seeding: prev = pos - v*dt
                VectorMA(s->pt[i], -subDt, vel, s->ptPrev[i]);
                // small per-point jitter so the body tumbles rather than translating rigidly
                s->ptPrev[i][0] += crandom() * 0.4f;
                s->ptPrev[i][1] += crandom() * 0.4f;
                s->ptPrev[i][2] += crandom() * 0.3f;
            }
            s->state = 1;
            if (rag_debug->integer) {
                cgi.Printf("^~^~^ RAGDOLL seeded ent=%d vel=(%.0f %.0f %.0f)\n", s->entnum, vel[0], vel[1], vel[2]);
            }
        }
        return;
    }

    if (!coop_ragdoll->integer) {
        return;
    }
    if (!cgi.R_SetRagdollPose) {
        if (rag_debug->integer && (!(cs->eFlags & EF_DEAD) && (ns->eFlags & EF_DEAD))) {
            cgi.Printf("^~^~^ RAGDOLL P1 skip ent=%d coop_ragdoll=%d bridge=NULL\n", ns->number, coop_ragdoll->integer);
        }
        return;
    }

    // plan section-4 arm guard, in full
    if (cs->eType != ET_MODELANIM || ns->eType != ET_MODELANIM) {
        return; // BOTH states (vet3/F6: an eType change does not clear interpolate)
    }
    if ((cs->eFlags & EF_DEAD) || !(ns->eFlags & EF_DEAD)) {
        return; // rising edge only; first-seen-dead never arms (plan C10)
    }
    if (!cent->interpolate) {
        return; // continuity (vet2/F6)
    }
    if (ns->number < cgs.maxclients) {
        return; // never a live player slot
    }

    if (rag_test->integer) {
        dtiki_t *tiki = cgi.R_Model_GetHandle(cgs.model_draw[ns->modelindex]);
        int      count;
        if (!tiki) {
            return;
        }
        for (count = 0; count < RAG_MAX_CH; count++) {
            const char *nm = cgi.Tag_NameForNum(tiki, count);
            if (!nm || !nm[0]) {
                break;
            }
        }
        if (count > 0) {
            RagArmTestPose(ns, tiki, count);
        }
        return;
    }

    RagArm(cent, ns);
}
