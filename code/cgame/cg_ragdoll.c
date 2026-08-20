/*
HZM coop - RAGDOLL (ragdoll_plan.md v3, vetted 3 rounds / 7 agents, PASS 2026-08-19).

PHASE 3: world + brush-entity collision on top of the P2 sim. P2 PASSED live 2026-08-19
21:15 (armed/seeded/slept clean, corpses crumpled as one body and fell through the floor -
the expected P2 state). P1 bridge proof PASSED 20:57 (channels=72 = static census).

Collision (plan section-3): per awake body per frame, sweep a +/-2u box from each point's
frame-start position to its post-substep position - world first (cgi.CM_BoxTrace model 0,
MASK_DEADSOLID: the engine's own dead-body clip), then one CG_GetBrushEntitiesInBounds
query and CM_TransformedBoxTrace per nearby bmodel (doors/elevators, EF_LINKANGLES
honored). Hits resolve with restitution 0.1 and tangential friction (0.45 on floors, 0.75
on walls), re-encoded into the Verlet ptPrev. Global ceiling 240 traces/frame - past it,
points glide this frame (plan's stated degradation). Slept bodies re-wake when a bmodel
in their bounds moves (origin-sum hash) so an elevator carries corpses instead of leaving
them in the air. Sleep is measured AFTER collision resolve, else the rest-contact jitter
(gravity vs floor) reads as ~6u/s and bodies never sleep.

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
typedef struct ragSim_s ragSim_t;
typedef struct ragPend_s ragPend_t;
static void RagPendingThink(ragPend_t *p);

#define RAG_MAX_SIMS   8
#define RAG_MAX_CH     128
#define RAG_PTS        15
#define RAG_SUBSTEP_MS 8
#define RAG_MAX_STEPS  4
#define RAG_DAMPING    0.98f
#define RAG_ITERS      6
#define RAG_TRACE_BUDGET 240   // per-frame MOVER trace ceiling (world traces have their own)
#define RAG_MOVER_PER_BODY 60  // ... and a per-body allowance, so the first corpses cannot eat it
#define RAG_CONTACT_RELAX 0.15f // where a point touches the world, the shape-match yields to it
#define RAG_MAX_PEND    16     // pending records live OUTSIDE the sim pool (32B vs ~9.4KB each)
#define RAG_PEND_CAP_MS 8000   // give-up only - never fires a capture

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

// A bone's skinned mesh runs from its OWN origin toward its CHILD (measured across 39 humanoid
// SKDs: every bone's weight centroid sits on the child side, no exceptions), so bone i's swing
// must be measured on pt[i] -> pt[child]. The shipped code used pt[parent] -> pt[i], which
// renders every limb off by the full angle its joint bent - the mesh literally cannot close at a
// bent elbow. coop_ragdollTest 2 is structurally blind to this (S = I kills the difference).
// Pelvis (-1) uses the anatomical triad; leaves (-1) keep the incoming segment.
static const int s_ragDriveChild[RAG_PTS] = {
    -1, //  0 Pelvis     - anatomical triad, never segment-driven
     2, //  1 Spine1     -> Spine2
     3, //  2 Spine2     -> Neck   (of 3 sim children: the spine's own flesh runs here)
     4, //  3 Neck       -> Head
    -1, //  4 Head       - leaf
     6, //  5 L UpperArm -> L Forearm
     7, //  6 L Forearm  -> L Hand
    -1, //  7 L Hand     - leaf
     9, //  8 R UpperArm -> R Forearm
    10, //  9 R Forearm  -> R Hand
    -1, // 10 R Hand     - leaf
    12, // 11 L Thigh    -> L Calf
    -1, // 12 L Calf     - leaf TODAY; becomes L Foot in round 9 (the unsimulated knee)
    14, // 13 R Thigh    -> R Calf
    -1, // 14 R Calf     - leaf
};

// stiffening braces beyond the 14 parent links (plan section-3). The grandparent links are
// fold limits: they cap how sharply any joint can hinge, measured at the DEATH pose, and
// they rotate with the body - so corpses topple and slump but cannot collapse into a
// bead-chain pile (the P3 live finding 2026-08-19 21:37: pure neighbor links = heap).
#define RAG_BRACES 16
static const int s_ragBraces[RAG_BRACES][2] = {
    {5,  8},  // shoulder - shoulder
    {11, 13}, // thigh - thigh
    {5,  13}, // L shoulder - R thigh (torso cross)
    {8,  11}, // R shoulder - L thigh (torso cross)
    {3,  1},  // neck - spine1
    {0,  2},  // pelvis - spine2
    {2,  4},  // spine2 - head       (neck fold limit)
    {2,  6},  // spine2 - L forearm  (shoulder fold limit)
    {2,  9},  // spine2 - R forearm
    {5,  7},  // L upperarm - L hand (elbow fold limit)
    {8,  10}, // R upperarm - R hand
    {0,  12}, // pelvis - L calf     (knee fold limit)
    {0,  14}, // pelvis - R calf
    {1,  11}, // spine1 - L thigh (geometrically inert - pt 11 IS the hip socket; kept, harmless)
    {2,  12}, // spine2 - L calf  (HIP fold limit - facts-vet FIX 9: without these a full
    {2,  14}, // spine2 - R calf   jackknife is constraint-legal; the live data proved it)
};

// fold limits are INEQUALITY braces: they stop the joint folding TIGHTER than the factor
// of its capture distance, but never stop it straightening (an equality brace froze dead
// arms at their death-pose bend). 0 = structural equality brace at full capture length.
static const float s_ragBraceMinFactor[RAG_BRACES] = {
    0,     0,     0,     0,     0,     0,     // structural truss: equality
    0.80f,                                    // neck
    0.70f, 0.70f,                             // shoulders
    0.75f, 0.75f,                             // elbows
    0.75f, 0.75f,                             // knees
    0.75f,                                    // inert hip-socket brace
    0.60f, 0.60f,                             // hips: sitting-fold ok, flat jackknife blocked
};

struct ragSim_s {
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

    int      simChan[RAG_PTS];     // channel index per sim point (-1 = missing)
    vec3_t   pt[RAG_PTS];          // world positions
    vec3_t   ptPrev[RAG_PTS];
    float    restLen[RAG_PTS];     // to parent
    float    braceLen[RAG_BRACES];
    vec3_t   restDir[RAG_PTS];     // capture direction parent->this (world)
    vec3_t   hipDir0;              // capture hip line L->R thigh (world), pelvis roll triad
    float    rot0[RAG_PTS][3][3];  // capture MODEL-SPACE rotation per sim bone (TIKI axis rows)

    vec3_t   seedOrigin;           // entity origin at arm (for snapshot differencing)
    int      seedServerTime;

    float    moverHash;            // bmodel origin-sum at sleep time (mover-wake detector)
    byte     touched;              // first-world-contact debug latch

    // SETTLE branch (round 8): the authored death animation owns the fall; physics owns
    // only the landing. goal[] is the captured (authored, already-landed) pose - the sim
    // is shape-matched back toward it every substep so the corpse keeps the animator's
    // silhouette while draping over whatever it actually landed on.
    byte     branch;               // 1 = settle, 0 = legacy free-fall (mode 3)
    vec3_t   goal[RAG_PTS];
    float    gravScale;            // 0 -> 1 over 250ms (no lurch at handoff)
    int      rampMs;
    vec3_t   driveDir0[RAG_PTS];   // capture OUTGOING (bone->child) directions
    byte     driveOk[RAG_PTS];
    byte     contact[RAG_PTS];     // 2-substep memory of touching the world
    byte     buried;               // points still in solid after the capture pre-lift
    float    maxSpeed;             // peak mean point speed (acceptance evidence)

    byte     freezePose;           // coop_ragdollTest 2: push the capture verbatim, no sim -
                                   // a pure space/round-trip test (any warp = render defect)
};

// A pending record: a corpse whose authored death animation is still playing. Kept OUT of the sim
// pool - the pending phase can legitimately run 5s (death_fire is 4.7s) and RagAllocSlot can only
// evict sleeping sims, so pendings in sim slots would starve the pool after 8 quick deaths.
struct ragPend_s {
    qboolean active;
    int      entnum;
    int      armTime;
    int      lastPrint;
    int      pendStatic;
    vec3_t   pendOrigin;
};

static ragPend_t s_ragPend[RAG_MAX_PEND];
static ragSim_t s_ragSims[RAG_MAX_SIMS];
static byte     s_ragNeverArm[MAX_GENTITIES]; // failure-ladder: NaN'd corpses keep the anim pose
static int      s_ragTraceCount;              // MOVER traces (the RAG_TRACE_BUDGET is theirs)
static int      s_ragWorldTraces;             // world traces: bounded by construction, reported

// per-bone collision radius (facts-vet FIX 4): a uniform box seated every point at the
// same height - the whole skeleton rested in one plane (z-span 0-2 in live data) and thick
// parts clipped the floor. Torso/head hold higher, extremities lower = natural drape.
static const float s_ragPtRadius[RAG_PTS] = {
    7.0f, 7.0f, 7.5f, 4.0f, 5.0f, // pelvis, spine1, spine2, neck, head
    4.0f, 3.0f, 2.5f,             // L upperarm, forearm, hand
    4.0f, 3.0f, 2.5f,             // R arm
    5.0f, 4.0f,                   // L thigh, calf
    5.0f, 4.0f,                   // R thigh, calf
};

// hierarchy anchor table (facts-vet FIX 2, table variant with its corrections applied:
// tag_weapon_left belongs to the LEFT hand; "Bip01 Spine" parents to Pelvis). Prefix match,
// first hit wins; sim channels self-anchor BEFORE this table runs (so "Bip01 Spine1/2"
// never fall into the "Bip01 Spine" entry); unknown gear/helper bones fall back to nearest,
// which is correct for them because they sit ON joints.
static const struct {
    const char *prefix;
    byte        sim;
} s_ragAnchorTable[] = {
    {"Bip01 L Finger",   7 },
    {"Bip01 R Finger",   10},
    {"Bip01 L Hand",     7 }, // child nubs
    {"Bip01 R Hand",     10},
    {"Bip01 L Foot",     12},
    {"Bip01 L Toe",      12},
    {"Bip01 R Foot",     14},
    {"Bip01 R Toe",      14},
    {"Bip01 L Clavicle", 2 },
    {"Bip01 R Clavicle", 2 },
    {"tag_weapon_left",  7 },
    {"tag_weapon",       10},
    {"tag_eyes",         4 },
    {"tag_head",         4 },
    {"helmet",           4 },
    {"eye",              4 },
    {"JAW",              4 },
    {"Bip01 Head",       4 }, // child nubs
    {"Bip01 Neck",       3 },
    {"Bip01 Spine",      0 },
    {NULL,               0 },
};
static cvar_t  *rag_debug    = NULL;
static cvar_t  *coop_ragdoll = NULL;
static cvar_t  *rag_test     = NULL;
static cvar_t  *rag_mode     = NULL;
static cvar_t  *rag_stiff    = NULL;
static cvar_t  *rag_drive    = NULL;

static void RagCvars(void)
{
    if (!rag_debug) {
        rag_debug    = cgi.Cvar_Get("r_ragdollDebug", "0", CVAR_TEMP);
        // ARCHIVE, not TEMP: a player's "off" has to survive a relaunch (CVAR_TEMP is
        // explicitly non-archived). Still defaults 0 - dark until the look is signed off.
        coop_ragdoll = cgi.Cvar_Get("coop_ragdoll", "0", CVAR_ARCHIVE);
        rag_test     = cgi.Cvar_Get("coop_ragdollTest", "0", CVAR_TEMP);
        // 0 = OFF, 1 = SETTLE (authored death anim plays out, then physics drapes the corpse
        // onto the geometry), 3 = the old arm-at-EF_DEAD behaviour, kept as the live A/B.
        rag_mode  = cgi.Cvar_Get("coop_ragdollMode", "1", CVAR_TEMP);
        rag_stiff = cgi.Cvar_Get("coop_ragdollStiff", "0.25", CVAR_TEMP);
        rag_drive = cgi.Cvar_Get("coop_ragdollDrive", "1", CVAR_TEMP);
    }
}

// the world's own gravity, not a hand-picked 800 (= 1.56 g against sv_gravity 512): a corpse must
// fall at the same rate as the player looking at it, and the lower value drops the constraint
// jitter floor the sleep gate has to clear.
static float RagGravity(void)
{
    float g = (cg.snap && cg.snap->ps.gravity > 0) ? (float)cg.snap->ps.gravity : 512.0f;
    if (g < 1.0f) {
        g = 1.0f;
    }
    if (g > 4000.0f) {
        g = 4000.0f;
    }
    return g;
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
    int       i;
    if (s) {
        memset(s, 0, sizeof(*s));
    }
    for (i = 0; i < RAG_MAX_PEND; i++) { // clear signals must reach pending records too
        if (s_ragPend[i].active && s_ragPend[i].entnum == entnum) {
            if (rag_debug && rag_debug->integer) {
                cgi.Printf("^~^~^ RAGDOLL pending cleared ent=%d (clear-signal)\n", entnum);
            }
            memset(&s_ragPend[i], 0, sizeof(s_ragPend[i]));
        }
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

static void RagMat3MulTrans(const float a[3][3], const float b[3][3], float out[3][3])
{
    // out = a * transpose(b)
    int r, c;
    for (r = 0; r < 3; r++) {
        for (c = 0; c < 3; c++) {
            out[r][c] = a[r][0] * b[c][0] + a[r][1] * b[c][1] + a[r][2] * b[c][2];
        }
    }
}

// orthonormal row-triad from two world directions (primary kept exact, secondary
// Gram-Schmidt'd); qfalse when degenerate (parallel/zero)
static qboolean RagTriad(const vec3_t primary, const vec3_t secondary, float T[3][3])
{
    vec3_t x, y, z;
    float  d;

    VectorCopy(primary, x);
    if (VectorNormalize(x) < 0.001f) {
        return qfalse;
    }
    d    = DotProduct(secondary, x);
    y[0] = secondary[0] - d * x[0];
    y[1] = secondary[1] - d * x[1];
    y[2] = secondary[2] - d * x[2];
    if (VectorNormalize(y) < 0.001f) {
        return qfalse;
    }
    CrossProduct(x, y, z);
    VectorCopy(x, T[0]);
    VectorCopy(y, T[1]);
    VectorCopy(z, T[2]);
    return qtrue;
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
            // exactly antiparallel: rotate pi about ANY axis perpendicular to a.
            // (the old diag(-1,1,-1) fixed +y, so it silently failed for rest directions
            // along world y - math-vet confirmed defect). R = 2*u*u^T - I, u perp a.
            vec3_t u;
            int    k = 0;
            if (fabs(a[1]) < fabs(a[k])) {
                k = 1;
            }
            if (fabs(a[2]) < fabs(a[k])) {
                k = 2;
            }
            VectorClear(u);
            u[k] = 1.0f;
            CrossProduct(a, u, u);
            VectorNormalize(u);
            out[0][0] = 2.0f * u[0] * u[0] - 1.0f;
            out[0][1] = 2.0f * u[0] * u[1];
            out[0][2] = 2.0f * u[0] * u[2];
            out[1][0] = 2.0f * u[1] * u[0];
            out[1][1] = 2.0f * u[1] * u[1] - 1.0f;
            out[1][2] = 2.0f * u[1] * u[2];
            out[2][0] = 2.0f * u[2] * u[0];
            out[2][1] = 2.0f * u[2] * u[1];
            out[2][2] = 2.0f * u[2] * u[2] - 1.0f;
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

    // pre-lift: death poses routinely bury calves/feet slightly in the floor. A buried
    // point starts every trace in-solid -> permanently frozen -> it PINS the body and the
    // rest drapes onto it (the 21:47 pile). Lift buried points to the surface above them
    // before any rest geometry is measured, so the sim starts penetration-free.
    for (i = 0; i < RAG_PTS; i++) {
        trace_t tr;
        vec3_t  above, pm, px;
        if (!(cgi.CM_PointContents(s->pt[i], 0) & MASK_DEADSOLID)) {
            continue;
        }
        VectorCopy(s->pt[i], above);
        above[2] += 40; // 24 was too short on stepped geometry: both ends in solid = startsolid,
                        // the point stays buried, freezes, and pins the body (bug-1962's pile)
        VectorSet(pm, -s_ragPtRadius[i], -s_ragPtRadius[i], -s_ragPtRadius[i]);
        VectorSet(px, s_ragPtRadius[i], s_ragPtRadius[i], s_ragPtRadius[i]);
        cgi.CM_BoxTrace(&tr, above, s->pt[i], pm, px, 0, MASK_DEADSOLID, qfalse);
        if (!tr.startsolid && tr.fraction < 1.0f) {
            VectorCopy(tr.endpos, s->pt[i]);
            s->pt[i][2] += 0.25f;
            VectorCopy(s->pt[i], s->ptPrev[i]);
        }
    }

    // a buried point starts every trace in-solid, freezes, and PINS the body. If the pre-lift
    // could not free the TORSO, or four points anywhere, refuse to arm outright: keeping the
    // authored pose is vanilla, therefore invisible, and strictly better than a pinned pile.
    {
        int nTorso = 0;
        s->buried  = 0;
        for (i = 0; i < RAG_PTS; i++) {
            if (cgi.CM_PointContents(s->pt[i], 0) & MASK_DEADSOLID) {
                s->buried++;
                if (i <= 4) {
                    nTorso++;
                }
            }
        }
        if (nTorso > 0 || s->buried >= 4) {
            if (rag_debug->integer) {
                cgi.Printf("^~^~^ RAGDOLL capture BURIED ent=%d torso=%d total=%d - not arming\n",
                           ns->number, nTorso, (int)s->buried);
            }
            return qfalse;
        }
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
    for (i = 0; i < RAG_BRACES; i++) {
        vec3_t d;
        VectorSubtract(s->pt[s_ragBraces[i][0]], s->pt[s_ragBraces[i][1]], d);
        s->braceLen[i] = VectorLength(d);
        if (s_ragBraceMinFactor[i] > 0) {
            s->braceLen[i] *= s_ragBraceMinFactor[i]; // inequality limits store the MINIMUM
        }
    }
    VectorSubtract(s->pt[13], s->pt[11], s->hipDir0); // L thigh -> R thigh (world)
    VectorNormalize(s->hipDir0);

    // capture the OUTGOING (bone -> child) directions the push drives each bone with
    for (i = 0; i < RAG_PTS; i++) {
        int    dch = s_ragDriveChild[i];
        vec3_t dv;
        s->driveOk[i] = 0;
        if (dch < 0) {
            continue;
        }
        VectorSubtract(s->pt[dch], s->pt[i], dv);
        if (VectorNormalize(dv) < 0.01f) {
            continue;
        }
        VectorCopy(dv, s->driveDir0[i]);
        s->driveOk[i] = 1;
    }

    // slave every channel to its nearest sim point at capture (fingers -> hands, face ->
    // head, gear -> nearest segment). rel = inv(anchor world) * channel world.
    for (ch = 0; ch < s->count; ch++) {
        vec3_t      capPos, worldPos, rel;
        int         best = -1, t;
        const char *chName;

        capPos[0] = s->mat0[ch][0][3];
        capPos[1] = s->mat0[ch][1][3];
        capPos[2] = s->mat0[ch][2][3];
        RagCaptureToWorld(s, capPos, worldPos);

        // 1) sim channels anchor to themselves
        for (i = 0; i < RAG_PTS; i++) {
            if (s->simChan[i] == ch) {
                best = i;
                break;
            }
        }
        // 2) fixed Bip01 hierarchy table (nearest mis-binds clavicles to upper arms and
        //    feet to the wrong calf in stride poses - facts-vet measured)
        if (best < 0) {
            chName = cgi.Tag_NameForNum(s->tiki, ch);
            if (chName) {
                for (t = 0; s_ragAnchorTable[t].prefix; t++) {
                    if (!Q_stricmpn(chName, s_ragAnchorTable[t].prefix, strlen(s_ragAnchorTable[t].prefix))) {
                        best = s_ragAnchorTable[t].sim;
                        break;
                    }
                }
            }
        }
        // 3) nearest: the fallback for gear/helper bones, which sit ON joints
        if (best < 0) {
            float bestD = 999999.0f;
            best = 0;
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
        }
        s->anchor[ch] = (byte)best;
        VectorSubtract(worldPos, s->pt[best], rel);
        RagMat3TransRotateVec(s->rot0[best], rel, s->relPos[ch]);
        // NOTE (math-vet 2026-08-19): no relRot is stored any more. The old
        // rot0^T*chRot0 "relative rotation" mixed model-space and world-space frames
        // ("the entity axis cancels" was FALSE) and mangled every twisted channel.
        // The push now composes chRot0 * (E*S*E^T) directly from mat0.
    }

    return qtrue;
}

// ---------- per-frame sim ------------------------------------------------------------------

// The body's rigid rotation from its capture pose to its current one: an anatomical triad
// (spine direction + hip line) at both ends. ONE producer - RagPush's pelvis orientation
// and the settle's shape-match goal must never disagree.
static qboolean RagBodyRotation(const ragSim_t *s, float S[3][3])
{
    float  T0[3][3], T1[3][3];
    vec3_t spineNow, hipNow;

    VectorSubtract(s->pt[1], s->pt[0], spineNow);
    VectorSubtract(s->pt[13], s->pt[11], hipNow);
    if (RagTriad(s->restDir[1], s->hipDir0, T0) && RagTriad(spineNow, hipNow, T1)) {
        RagMat3TransMul(T0, T1, S); // capture basis -> current basis (row-vector: v' = v*S)
        return qtrue;
    }
    VectorNormalize(spineNow);
    RagMat3FromTo(s->restDir[1], spineNow, S); // degenerate: 1-axis fallback
    return qfalse;
}

// pull the point cloud back toward the authored pose, rigidly re-fitted to wherever the
// body currently is (pelvis anchor + body rotation). alpha 1 = perfectly rigid corpse,
// 0 = today's free Verlet. Runs after the constraints, before collision, so the ground
// always gets the last word - that is what produces the drape.
static void RagShapeMatch(ragSim_t *s, float alpha)
{
    float S[3][3];
    int   i;

    if (alpha <= 0.001f) {
        return;
    }
    RagBodyRotation(s, S);
    for (i = 1; i < RAG_PTS; i++) {
        vec3_t rel, want, d;
        float  a = alpha;
        VectorSubtract(s->goal[i], s->goal[0], rel);
        RagMat3RotateVec(S, rel, want);
        VectorAdd(s->pt[0], want, want);
        VectorSubtract(want, s->pt[i], d);
        if (s->contact[i]) {
            a *= RAG_CONTACT_RELAX; // where the body TOUCHES, the ground gets the last word - the
        }                           // limb stays draped where it landed instead of being reeled in
        VectorMA(s->pt[i], a, d, s->pt[i]);
    }
}

static void RagStep(ragSim_t *s, float dt)
{
    int   i, it;
    float g = RagGravity() * dt * dt * (s->branch ? s->gravScale : 1.0f);

    for (i = 0; i < RAG_PTS; i++) {
        vec3_t vel, next;
        float  vlen;
        VectorSubtract(s->pt[i], s->ptPrev[i], vel);
        VectorScale(vel, RAG_DAMPING, vel);
        vlen = VectorLength(vel);
        if (vlen > 24.0f) {
            VectorScale(vel, 24.0f / vlen, vel); // blowup insurance: 24u/substep = 3000u/s cap
        }
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
        for (i = 0; i < RAG_BRACES; i++) {
            int    a = s_ragBraces[i][0], b = s_ragBraces[i][1];
            vec3_t d;
            float  len, corr;
            VectorSubtract(s->pt[a], s->pt[b], d);
            len = VectorLength(d);
            if (len < 0.001f) {
                continue;
            }
            if (s_ragBraceMinFactor[i] > 0 && len >= s->braceLen[i]) {
                continue; // inequality fold limit: only ever pushes APART
            }
            corr = (len - s->braceLen[i]) * 0.5f / len; // firm: these are the anti-pile truss
            VectorMA(s->pt[a], -corr, d, s->pt[a]);
            VectorMA(s->pt[b], corr, d, s->pt[b]);
        }
    }
    if (s->branch) {
        float target = rag_stiff->value;
        float alpha;
        if (target < 0) {
            target = 0;
        } else if (target > 1) {
            target = 1;
        }
        // rigid at handoff, relaxing to the target over 300ms: the corpse never twitches
        // as physics takes the wheel
        alpha = (s->rampMs >= 300) ? target : target + (1.0f - target) * (1.0f - s->rampMs / 300.0f);
        RagShapeMatch(s, alpha);
    }
}

// ---------- collision (PHASE 3) -------------------------------------------------------------

static void RagResolveHit(ragSim_t *s, int i, const trace_t *tr)
{
    vec3_t v, vn, vt, pos;
    float  d;

    VectorMA(tr->endpos, 0.25f, tr->plane.normal, pos);
    // implicit per-substep velocity, split on the contact plane
    VectorSubtract(s->pt[i], s->ptPrev[i], v);
    d = DotProduct(v, tr->plane.normal);
    VectorScale(tr->plane.normal, d, vn);
    VectorSubtract(v, vn, vt);
    // resting contact: a slow point on a floor stops DEAD - this is what lets bodies
    // speed-sleep instead of micro-skidding their whole 6s life (and off ledges).
    // 0.35/substep = ~44u/s; the old 1.2 gate was 150u/s and froze the landing slide,
    // folding bodies vertically over their first contact (audit defect 3)
    if (tr->plane.normal[2] > 0.7f && VectorLength(v) < 0.35f) {
        VectorCopy(pos, s->pt[i]);
        VectorCopy(pos, s->ptPrev[i]);
        s->contact[i] = 2; // resting on the world: the shape-match yields here
        return;
    }
    VectorScale(vn, -0.1f, vn); // restitution
    VectorScale(vt, (tr->plane.normal[2] > 0.7f) ? 0.45f : 0.75f, vt);
    VectorAdd(vn, vt, v);
    VectorCopy(pos, s->pt[i]);
    VectorSubtract(pos, v, s->ptPrev[i]);
    if (!s->touched) {
        s->touched = 1;
        if (rag_debug->integer) {
            cgi.Printf("^~^~^ RAGDOLL contact ent=%d pt=%d n=(%.2f %.2f %.2f)\n",
                       s->entnum, i, tr->plane.normal[0], tr->plane.normal[1], tr->plane.normal[2]);
        }
    }
}

// origin-sum over bmodels in the body's bounds; a slept body wakes when this changes
static float RagMoverHash(const ragSim_t *s)
{
    centity_t *movers[4];
    vec3_t     bmins, bmaxs;
    int        n, m, i;
    float      h = 0;

    ClearBounds(bmins, bmaxs);
    for (i = 0; i < RAG_PTS; i++) {
        AddPointToBounds(s->pt[i], bmins, bmaxs);
    }
    for (i = 0; i < 3; i++) {
        bmins[i] -= 8;
        bmaxs[i] += 8;
    }
    n = CG_GetBrushEntitiesInBounds(4, movers, bmins, bmaxs);
    for (m = 0; m < n; m++) {
        h += (float)movers[m]->currentState.number * 3.0f;
        h += movers[m]->lerpOrigin[0] + movers[m]->lerpOrigin[1] + movers[m]->lerpOrigin[2];
        h += movers[m]->lerpAngles[1];
    }
    return h;
}

// world pass, run INSIDE every substep AFTER the constraint iterations. The 21:47 live
// finding: per-frame collision let the solver end frames with points already below the
// floor - the next sweep then started underground and never saw the surface (bodies sank
// under the map), and the floor-frozen points pinned corpses into piles. Per-substep
// ordering (integrate -> constraints -> collide) ends every substep penetration-clean.
// World traces are budget-EXEMPT (hard-bounded at 15 x 4 x pool; short sweeps are cheap).
static void RagCollideWorld(ragSim_t *s, vec3_t subStart[RAG_PTS])
{
    trace_t tr;
    int     i;

    for (i = 0; i < RAG_PTS; i++) {
        if (s->contact[i]) {
            s->contact[i]--; // 2-substep memory: the shape-match runs BEFORE collide, so it
        }                    // reads the previous substep's contact set
    }
    for (i = 0; i < RAG_PTS; i++) {
        vec3_t d, pm, px;
        VectorSubtract(s->pt[i], subStart[i], d);
        if (VectorLengthSquared(d) < 0.0001f) {
            continue;
        }
        VectorSet(pm, -s_ragPtRadius[i], -s_ragPtRadius[i], -s_ragPtRadius[i]);
        VectorSet(px, s_ragPtRadius[i], s_ragPtRadius[i], s_ragPtRadius[i]);
        s_ragWorldTraces++; // world traces have their OWN counter - sharing the mover budget
                            // silently disabled mover collision from the 4th body onward
        cgi.CM_BoxTrace(&tr, subStart[i], s->pt[i], pm, px, 0, MASK_DEADSOLID, qfalse);
        if (tr.startsolid) {
            // A point already inside geometry. On the FREE branch, hold it and kill its velocity
            // (it has nowhere legal to go and holding stops tunnelling). On the SETTLE branch,
            // RELEASE it instead: the authored pose is an attractor that will pull the limb back
            // out on its own, whereas freezing anchors it inside the wall and the rest of the
            // body tears around it - live 2026-08-20, a corpse clipped into a wall "got kinda
            // mangled" at low stiffness, which is exactly that anchor.
            if (!s->branch) {
                VectorCopy(subStart[i], s->pt[i]);
                VectorCopy(s->pt[i], s->ptPrev[i]);
            } else {
                s->contact[i] = 0; // full-strength pull home, not the contact-relaxed one
            }
            continue;
        }
        if (tr.fraction < 1.0f) {
            RagResolveHit(s, i, &tr);
        }
    }
}

static void RagCollideMovers(ragSim_t *s, vec3_t frameStart[RAG_PTS])
{
    trace_t    tr;
    centity_t *movers[4];
    vec3_t     bmins, bmaxs, angles;
    int        i, m, nMovers;
    int        allow = s_ragTraceCount + RAG_MOVER_PER_BODY; // per-body slice of the budget

    // mover pass: one bounds query per body per frame (plan section-3)
    ClearBounds(bmins, bmaxs);
    for (i = 0; i < RAG_PTS; i++) {
        AddPointToBounds(s->pt[i], bmins, bmaxs);
        AddPointToBounds(frameStart[i], bmins, bmaxs);
    }
    for (i = 0; i < 3; i++) {
        bmins[i] -= 8;
        bmaxs[i] += 8;
    }
    nMovers = CG_GetBrushEntitiesInBounds(4, movers, bmins, bmaxs);
    for (m = 0; m < nMovers; m++) {
        clipHandle_t cmodel = cgi.CM_InlineModel(movers[m]->currentState.modelindex);
        if (!cmodel) {
            continue;
        }
        if (movers[m]->currentState.eFlags & EF_LINKANGLES) {
            VectorCopy(movers[m]->lerpAngles, angles);
        } else {
            VectorClear(angles);
        }
        for (i = 0; i < RAG_PTS; i++) {
            vec3_t pm, px;
            if (s_ragTraceCount >= RAG_TRACE_BUDGET || s_ragTraceCount >= allow) {
                return;
            }
            VectorSet(pm, -s_ragPtRadius[i], -s_ragPtRadius[i], -s_ragPtRadius[i]);
            VectorSet(px, s_ragPtRadius[i], s_ragPtRadius[i], s_ragPtRadius[i]);
            s_ragTraceCount++;
            cgi.CM_TransformedBoxTrace(&tr, frameStart[i], s->pt[i], pm, px, cmodel,
                                       MASK_DEADSOLID, movers[m]->lerpOrigin, angles, qfalse);
            if (tr.startsolid) {
                s->pt[i][2] += 2.5f; // mover rose into the body: shove up, re-settle next frame
                VectorCopy(s->pt[i], s->ptPrev[i]);
                continue;
            }
            if (tr.fraction < 1.0f) {
                RagResolveHit(s, i, &tr);
            }
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
    float        rotNow[RAG_PTS][3][3]; // rot0 * S: POSITION path only (sandwich cancels exactly)
    float        conj[RAG_PTS][3][3];   // Ecap * S * Enow^T: world swing in the renderer's frame
    float        E[3][3], Enow[3][3];
    vec3_t       mins, maxs, curOrigin;
    vec3_t       axisNow[3];
    centity_t   *cent = &cg_entities[s->entnum];
    float        invScale;
    int          i, ch, r, c;

    // CURRENT-frame placement (plan C13; live-proven 23:05: death anims keep moving the
    // server corpse origin after capture - converting with the CAPTURE placement rendered
    // the mesh offset from the simulated skeleton by every unit of post-death drift:
    // bodies slid forward, clipped into rocks, and sank through floors while the sim
    // points rested correctly on top).
    VectorCopy(cent->lerpOrigin, curOrigin);
    AnglesToAxis(cent->lerpAngles, axisNow);
    invScale = 1.0f / s->scale;

    // MATH CONTRACT (math-vet confirmed 2026-08-19): the bone cache is MODEL space - the
    // renderer post-multiplies the entity axis E. mat0/rot0 are model space; the swing S is
    // WORLD space (built from world point directions). The correct channel rotation is
    // chRot0 * (E*S*E^T). The old form (rot0*S)*(rot0^T*chRot0) carried a frame mismatch
    // (120 deg bone error at yaw 90 + swing 90) AND put the swing on the wrong side for
    // twisted channels (a hand rendered perpendicular to its own forearm at yaw 0).
    for (r = 0; r < 3; r++) {
        for (c = 0; c < 3; c++) {
            E[r][c]    = s->entAxis[r][c]; // capture-frame axis (the frame S was built against)
            Enow[r][c] = axisNow[r][c];    // render-frame axis (what the renderer multiplies by)
        }
    }

    for (i = 0; i < RAG_PTS; i++) {
        int    p = s_ragBones[i].parent;
        float  S[3][3], tmp[3][3];
        vec3_t dNow;

        if (s->freezePose) {
            RagMat3Identity(S); // verbatim-capture drill: with S=I the push must reproduce
                                // the death pose exactly; any warp isolates the render side
        } else if (p < 0) {
            RagBodyRotation(s, S); // pelvis: shared anatomical triad (see the helper)
        } else {
            const float *ref;
            int          dch = rag_drive->integer ? s_ragDriveChild[i] : -1;
            if (dch >= 0 && s->driveOk[i]) {
                VectorSubtract(s->pt[dch], s->pt[i], dNow); // OUTGOING: the mesh's own run
                ref = s->driveDir0[i];
            } else {
                VectorSubtract(s->pt[i], s->pt[p], dNow); // leaf / drive off: incoming segment
                ref = s->restDir[i];
            }
            if (VectorLength(dNow) < 0.01f) {
                RagMat3Identity(S);
            } else {
                VectorNormalize(dNow);
                RagMat3FromTo(ref, dNow, S);
            }
        }
        RagMat3Mul(s->rot0[i], S, rotNow[i]);
        RagMat3Mul(E, S, tmp);
        RagMat3MulTrans(tmp, Enow, conj[i]); // conj = Ecap * S * Enow^T (== E*S*E^T while the
                                             // corpse's angles stay at their capture values)
    }

    ClearBounds(mins, maxs);
    for (ch = 0; ch < s->count && ch < RAG_MAX_CH; ch++) {
        int    a = s->anchor[ch];
        vec3_t off, world, cap;

        RagMat3RotateVec(rotNow[a], s->relPos[ch], off);
        VectorAdd(s->pt[a], off, world);
        AddPointToBounds(world, mins, maxs);
        // world -> model against the CURRENT placement (curOrigin/axisNow), never the
        // capture placement: the renderer recomposes with the current placement each frame
        {
            vec3_t relw;
            VectorSubtract(world, curOrigin, relw);
            cap[0] = DotProduct(relw, axisNow[0]) * invScale;
            cap[1] = DotProduct(relw, axisNow[1]) * invScale;
            cap[2] = DotProduct(relw, axisNow[2]) * invScale;
        }
        mat[ch][0][3] = cap[0];
        mat[ch][1][3] = cap[1];
        mat[ch][2][3] = cap[2];
        // rotation: chRot0 (rows of mat0) * conj[anchor]
        for (r = 0; r < 3; r++) {
            for (c = 0; c < 3; c++) {
                mat[ch][r][c] = s->mat0[ch][r][0] * conj[a][0][c] + s->mat0[ch][r][1] * conj[a][1][c]
                              + s->mat0[ch][r][2] * conj[a][2][c];
            }
        }
    }
    for (i = 0; i < 3; i++) {
        mins[i] -= 16;
        maxs[i] += 16;
    }
    cgi.R_SetRagdollPose(s->entnum, s->tiki, s->count, &mat[0][0][0], mins, maxs);
}

// r_ragdollDebug 2: draw the 15 sim points as world sprites - the ground-truth view that
// separates "the SKELETON is wrong" (dots piled/warped) from "the MESH is wrong" (dots form
// a clean body shape but the model looks mangled). Pelvis renders red and bigger.
static void RagDrawSkeleton(ragSim_t *s)
{
    static qhandle_t hDot = 0;
    refEntity_t      ent;
    vec3_t           zero = {0, 0, 0};
    int              i;

    if (!hDot) {
        hDot = cgi.R_RegisterModel("textures/hud/coop_ally_icon.spr");
        if (!hDot) {
            return;
        }
    }
    for (i = 0; i < RAG_PTS; i++) {
        memset(&ent, 0, sizeof(ent));
        ent.hModel = hDot;
        AnglesToAxis(zero, ent.axis);
        ent.scale              = (i == 0) ? 0.30f : 0.16f;
        ent.renderfx           = RF_DEPTHHACK; // dots sit at bone centers INSIDE the mesh -
                                               // without this the body hides its own skeleton
        ent.reType             = RT_SPRITE;
        ent.frameInfo[0].index = 0;
        ent.shaderRGBA[0]      = -1;
        ent.shaderRGBA[1]      = (i == 0) ? 0 : -1;
        ent.shaderRGBA[2]      = (i == 0) ? 0 : -1;
        VectorCopy(s->pt[i], ent.origin);
        cgi.R_AddRefEntityToScene(&ent, ENTITYNUM_NONE);
    }
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
    s_ragTraceCount  = 0;
    s_ragWorldTraces = 0;
    for (i = 0; i < RAG_MAX_PEND; i++) {
        if (s_ragPend[i].active) {
            RagPendingThink(&s_ragPend[i]); // corpses whose authored death is still playing
        }
    }
    for (i = 0; i < RAG_MAX_SIMS; i++) {
        ragSim_t *s = &s_ragSims[i];
        vec3_t    frameStart[RAG_PTS];
        int       steps, ms, j;
        if (!s->active) {
            continue;
        }
        if (rag_debug->integer >= 2) {
            RagDrawSkeleton(s); // ground-truth dots, drawn for awake AND slept bodies
        }
        if (s->freezePose) {
            RagPush(s); // verbatim capture every frame; no sim, no seed, no sleep
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
            // sleeping: last pushed pose stands - unless a bmodel under/over it moved
            if (RagMoverHash(s) != s->moverHash) {
                s->state   = 1;
                s->sleepMs = 0;
                s->lifeMs  = 0; // ride the mover as long as it moves
                s->accumMs = 0;
                if (rag_debug->integer) {
                    cgi.Printf("^~^~^ RAGDOLL mover-wake ent=%d\n", s->entnum);
                }
            } else {
                continue;
            }
        }
        ms = cg.frametime;
        if (ms > 200) {
            ms = 200;
        }
        s->accumMs += ms;
        s->lifeMs += ms;
        if (s->branch) {
            s->rampMs += ms;
            s->gravScale = (s->rampMs >= 250) ? 1.0f : s->rampMs / 250.0f;
        }
        for (j = 0; j < RAG_PTS; j++) {
            VectorCopy(s->pt[j], frameStart[j]);
        }
        steps = 0;
        while (s->accumMs >= RAG_SUBSTEP_MS && steps < RAG_MAX_STEPS) {
            vec3_t subStart[RAG_PTS];
            for (j = 0; j < RAG_PTS; j++) {
                VectorCopy(s->pt[j], subStart[j]);
            }
            RagStep(s, RAG_SUBSTEP_MS * 0.001f);
            RagCollideWorld(s, subStart); // every substep ends penetration-clean
            s->accumMs -= RAG_SUBSTEP_MS;
            steps++;
        }
        if (s->accumMs > RAG_SUBSTEP_MS * RAG_MAX_STEPS) {
            s->accumMs = RAG_SUBSTEP_MS * RAG_MAX_STEPS; // discard the hitch backlog
        }
        if (steps) {
            RagCollideMovers(s, frameStart); // movers are slow: whole-frame sweep suffices
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
            if (speed > s->maxSpeed) {
                s->maxSpeed = speed; // acceptance evidence: >300 on settle = the pose wasn't landed
            }
            // 10 not 4: truss-supported points that never floor-contact carry ~6u/s of
            // gravity-vs-constraint jitter (sub-pixel, invisible) - at 4 nothing ever
            // speed-slept, every body rode to the 6s life cap (live 21:46, 3/3 kills)
            if (speed < 10.0f) {
                s->sleepMs += ms;
            } else {
                s->sleepMs = 0;
            }
            if (s->sleepMs > 1000 || s->lifeMs > 6000) {
                s->state     = 2;
                s->moverHash = RagMoverHash(s); // baseline for the mover-wake detector
                if (rag_debug->integer) {
                    // span discriminates pile-vs-mangle: a sprawled body has one lateral
                    // axis 55-75u and z 8-20u; a point-pile is <35u everywhere; sane
                    // points + mangled mesh means the PUSH rotation math is the defect
                    vec3_t bmn, bmx;
                    float  drift = 0;
                    ClearBounds(bmn, bmx);
                    for (j = 0; j < RAG_PTS; j++) {
                        AddPointToBounds(s->pt[j], bmn, bmx);
                    }
                    if (s->branch) {
                        // how far the settle moved the body off the animator's pose:
                        // ~0 on flat ground (vanilla-identical), 5-20u draped on geometry
                        float  S[3][3];
                        vec3_t rel, want, dd;
                        RagBodyRotation(s, S);
                        for (j = 1; j < RAG_PTS; j++) {
                            VectorSubtract(s->goal[j], s->goal[0], rel);
                            RagMat3RotateVec(S, rel, want);
                            VectorAdd(s->pt[0], want, want);
                            VectorSubtract(want, s->pt[j], dd);
                            drift += VectorLength(dd);
                        }
                        drift /= (RAG_PTS - 1);
                    }
                    {
                        int nContacts = 0;
                        for (j = 0; j < RAG_PTS; j++) {
                            if (s->contact[j]) {
                                nContacts++;
                            }
                        }
                        // life= is NOT an acceptance metric on the settle branch: the body starts
                        // at rest so sleepMs accrues from frame 1. Judge drift/span/maxspd.
                        cgi.Printf("^~^~^ RAGDOLL sleep ent=%d life=%dms span=(%.0f %.0f %.0f) branch=%s "
                                   "drift=%.1f maxspd=%.0f contacts=%d alpha=%.2f drive=%d worldtr=%d\n",
                                   s->entnum, s->lifeMs, bmx[0] - bmn[0], bmx[1] - bmn[1], bmx[2] - bmn[2],
                                   s->branch ? "settle" : "free", drift, s->maxSpeed, nContacts,
                                   rag_stiff->value, rag_drive->integer, s_ragWorldTraces);
                    }
                }
            }
        }
        RagPush(s);
    }
}

// ---------- lifecycle ----------------------------------------------------------------------

static ragSim_t *RagAllocSlot(int entnum)
{
    ragSim_t *s = NULL;
    int       i;

    if (RagSimFor(entnum) || s_ragNeverArm[entnum]) {
        return NULL;
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
            cgi.Printf("^~^~^ RAGDOLL arm refused ent=%d (pool awake-full)\n", entnum);
        }
        return NULL;
    }
    memset(s, 0, sizeof(*s));
    return s;
}

static void RagArm(centity_t *cent, entityState_t *ns)
{
    ragSim_t *s = RagAllocSlot(ns->number);

    if (!s) {
        return;
    }
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

// SETTLE branch, state -1: hold while the authored death animation plays. The engine does
// not even REQUEST the death anim until a Think after the EF_DEAD edge, and then blends it
// in over 0.5s (actor.cpp ChangeAnim m_fCrossblendTime) - so the pose at the edge is the
// LIVING one. Arming there photographed a standing soldier and dropped him cold, which is
// the whole "bodies don't fall like that" verdict. We wait for the animator's fall to
// finish and for the server to ground the corpse, THEN hand the landed pose to physics.
// THE SERVER'S OWN HANDOFF SIGNAL. Actor::FinishedAnimation_Killed is the only route to
// Actor::BecomeCorpse, which swaps the box from the living {-15,-15,0}..{15,15,94} to the corpse
// slab {-32,-32,0}..{32,32,16} and re-links, so SV_LinkEntity re-packs entityState.solid and the
// 32-bit netfield delivers it to us. Seeing that box PROVES the two things eight rounds spent
// guessing at: the authored death animation ran to completion, AND CheckGround()/droptofloor(64)
// has already parked the body. DeathEmbalm also re-links every 0.5s during the anim but only ever
// lowers maxs.z - x/y stay 15 the whole way down - so maxs[0] is a monotone one-shot
// discriminator that cannot fire early.
static qboolean RagServerParked(const entityState_t *es)
{
    vec3_t bmin, bmax;

    if (!es->solid) {
        return qfalse; // SOLID_NOT (coop_corpseShootable 0) parks silently: those ride the cap
    }
    IntegerToBoundingBox(es->solid, bmin, bmax);
    return (bmax[0] >= 24.0f && bmax[2] <= 32.0f) ? qtrue : qfalse;
}

// SETTLE branch: hold while the authored death animation plays, then capture the LANDED pose at
// the exact frame the server parks the corpse. Arming at the EF_DEAD edge photographed a STANDING
// soldier - the engine does not request the death anim until a Think after the edge and then
// crossblends it in - which is the whole "bodies don't fall like that" verdict of rounds 1-8.
static void RagPendingThink(ragPend_t *p)
{
    centity_t     *cent = &cg_entities[p->entnum];
    entityState_t *cs   = &cent->currentState;
    vec3_t         d;
    int            age = cg.time - p->armTime;

    // lifecycle first: revived, recycled, dropped from the snapshot, or PVS-exited
    if (!cent->currentValid || !cent->interpolate || cs->modelindex <= 0 || cs->eType != ET_MODELANIM
        || !(cs->eFlags & EF_DEAD)) {
        if (rag_debug->integer) {
            // WHICH condition dropped it - live round 8 showed 10 of 14 pendings vanishing with
            // neither an arm nor a give-up, so the drop reason is the whole question
            cgi.Printf("^~^~^ RAGDOLL pending dropped ent=%d age=%dms valid=%d interp=%d midx=%d "
                       "etype=%d dead=%d\n",
                       p->entnum, age, (int)cent->currentValid, (int)cent->interpolate, cs->modelindex,
                       cs->eType, (cs->eFlags & EF_DEAD) ? 1 : 0);
        }
        memset(p, 0, sizeof(*p));
        return;
    }

    VectorSubtract(cent->lerpOrigin, p->pendOrigin, d);
    VectorCopy(cent->lerpOrigin, p->pendOrigin);
    p->pendStatic = (VectorLength(d) < 0.5f) ? p->pendStatic + 1 : 0;

    if (rag_debug->integer >= 2 && cg.time - p->lastPrint >= 250) {
        vec3_t bmn, bmx;
        p->lastPrint = cg.time;
        IntegerToBoundingBox(cs->solid, bmn, bmx);
        cgi.Printf("^~^~^ RAGDOLL pending ent=%d box=(%.0f %.0f) static=%d age=%d\n", p->entnum, bmx[0],
                   bmx[2], p->pendStatic, age);
    }

    if (!RagServerParked(cs)) {
        // GIVE UP, never fire on a guess. A corpse the server never parks keeps its authored
        // pose - which IS vanilla, and therefore invisible. Arming a guessed pose is the exact
        // failure mode this branch exists to end.
        if (age > RAG_PEND_CAP_MS) {
            if (rag_debug->integer) {
                cgi.Printf("^~^~^ RAGDOLL pending gave-up ent=%d age=%dms (server never parked)\n",
                           p->entnum, age);
            }
            memset(p, 0, sizeof(*p));
        }
        return;
    }
    if (p->pendStatic < 2) {
        return; // the park itself moves the origin (droptofloor 64u): let it land first
    }

    {
        int         entnum  = p->entnum;
        int         armTime = p->armTime;
        const char *nm      = NULL;
        ragSim_t   *s;
        int         i;

        memset(p, 0, sizeof(*p)); // the pending record is spent either way
        s = RagAllocSlot(entnum);
        if (!s) {
            return; // pool full of awake sims: keep the authored pose
        }
        if (!RagCapture(cent, cs, s)) {
            memset(s, 0, sizeof(*s));
            return;
        }
        s->active    = qtrue;
        s->entnum    = entnum;
        s->state     = 1; // no seed: the authored fall already happened
        s->branch    = 1;
        s->armTime   = armTime;
        s->gravScale = 0.0f;
        for (i = 0; i < RAG_PTS; i++) {
            VectorCopy(s->pt[i], s->goal[i]); // the authored pose is the target silhouette
        }
        RagPush(s);
        if (rag_debug->integer) {
            int   dom = -1, k;
            float bw = 0;
            for (k = 0; k < MAX_FRAMEINFOS; k++) {
                if (cs->frameInfo[k].weight > bw) {
                    bw  = cs->frameInfo[k].weight;
                    dom = k;
                }
            }
            if (dom >= 0) {
                nm = cgi.Anim_NameForNum(s->tiki, cs->frameInfo[dom].index);
            }
            // the anim name is DIAGNOSTIC ONLY now - it gates nothing
            cgi.Printf("^~^~^ RAGDOLL settle-armed ent=%d channels=%d after=%dms via=solid anim=%s buried=%d\n",
                       entnum, s->count, cg.time - armTime, nm ? nm : "?", (int)s->buried);
        }
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
                // small per-point jitter so the body tumbles rather than translating
                // rigidly. 0.08u on ptPrev = ~10u/s; the old 0.4 was a 50u/s kick that
                // crumpled bodies the instant they seeded (audit defect 7)
                s->ptPrev[i][0] += crandom() * 0.08f;
                s->ptPrev[i][1] += crandom() * 0.08f;
                s->ptPrev[i][2] += crandom() * 0.06f;
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

    if (rag_test->integer == 1) {
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

    if (rag_mode->integer == 1 && !rag_test->integer) {
        // SETTLE: record a pending arm and let the animators own the fall. The capture happens
        // when the SERVER parks the body, not when we guess the anim is over.
        int        k;
        ragPend_t *p = NULL;
        if (RagSimFor(ns->number) || s_ragNeverArm[ns->number]) {
            return;
        }
        for (k = 0; k < RAG_MAX_PEND; k++) {
            if (s_ragPend[k].active && s_ragPend[k].entnum == ns->number) {
                return; // already pending
            }
        }
        for (k = 0; k < RAG_MAX_PEND; k++) {
            if (!s_ragPend[k].active) {
                p = &s_ragPend[k];
                break;
            }
        }
        if (!p) {
            for (k = 0; k < RAG_MAX_PEND; k++) { // oldest-first eviction
                if (!p || s_ragPend[k].armTime < p->armTime) {
                    p = &s_ragPend[k];
                }
            }
            if (p && rag_debug->integer) { // the last silent path: an evicted pending never
                                           // arms and never prints, so it looked like a drop
                cgi.Printf("^~^~^ RAGDOLL pending EVICTED ent=%d age=%dms (pool of %d full) for ent=%d\n",
                           p->entnum, cg.time - p->armTime, RAG_MAX_PEND, ns->number);
            }
        }
        if (p) {
            memset(p, 0, sizeof(*p));
            p->active  = qtrue;
            p->entnum  = ns->number;
            p->armTime = cg.time;
            VectorCopy(cent->lerpOrigin, p->pendOrigin);
            if (rag_debug->integer) {
                cgi.Printf("^~^~^ RAGDOLL pending-arm ent=%d (waiting on server park)\n", ns->number);
            }
        }
        return;
    }
    if (rag_mode->integer != 3) {
        return; // mode 0 = OFF (it used to fall through into settle - a silent trap)
    }

    RagArm(cent, ns);
    if (rag_test->integer == 2) {
        ragSim_t *ss = RagSimFor(ns->number);
        if (ss) {
            ss->freezePose = 1; // verbatim-capture drill (any warp = render defect)
            if (rag_debug->integer) {
                cgi.Printf("^~^~^ RAGDOLL freeze-pose armed ent=%d\n", ns->number);
            }
        }
    }
}
