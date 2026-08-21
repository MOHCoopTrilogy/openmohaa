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
static ragSim_t *RagAllocSlot(int entnum);

#define RAG_MAX_SIMS   16
#define RAG_MAX_CH     128
#define RAG_PTS        17
#define RAG_SUBSTEP_MS 8
#define RAG_MAX_STEPS  4
#define RAG_DAMPING    0.98f
#define RAG_ITERS      6
#define RAG_TRACE_BUDGET 272   // per-frame MOVER trace ceiling (world traces have their own)
#define RAG_MOVER_PER_BODY 68  // ... and a per-body allowance, so the first corpses cannot eat it
#define RAG_CONTACT_RELAX 0.15f // where a point touches the world, the shape-match yields to it
#define RAG_IMPACT_RELAX  0.05f // a struck limb keeps only 5% of the pose pull at the moment of
#define RAG_IMPACT_LIMP_MS 600  // impact, easing back to full over this long. Without it the
                                // 0.25 pull reels the limb home in two frames and a hit on a
                                // corpse reads as a twitch instead of a limb actually moving.
#define RAG_MAX_PEND    16     // pending records live OUTSIDE the sim pool (32B vs ~9.4KB each)
#define RAG_PEND_CAP_MS 8000   // give-up only - never fires a capture

// the 15 sim bones: index, Bip01 tag name, parent sim index (-1 = root)
static const struct {
    const char *name;
    int         parent;
} s_ragBones[] = {
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
    // The calf point IS the knee (the engine stores only a LENGTH for the IK elbow/wrist and
    // places the bone at hip + thighLength). Without a foot the entire shin - ~24u, and about a
    // third of the body mesh - rendered as a rigid copy of the thigh.
    {"Bip01 L Foot",    12}, // 15
    {"Bip01 R Foot",    14}, // 16
};

// A bone's skinned mesh runs from its OWN origin toward its CHILD (measured across 39 humanoid
// SKDs: every bone's weight centroid sits on the child side, no exceptions), so bone i's swing
// must be measured on pt[i] -> pt[child]. The shipped code used pt[parent] -> pt[i], which
// renders every limb off by the full angle its joint bent - the mesh literally cannot close at a
// bent elbow. coop_ragdollTest 2 is structurally blind to this (S = I kills the difference).
// Pelvis (-1) uses the anatomical triad; leaves (-1) keep the incoming segment.
static const int s_ragDriveChild[] = {
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
    15, // 12 L Calf     -> L Foot   (was a leaf: the whole shin copied the thigh's rotation)
    14, // 13 R Thigh    -> R Calf
    16, // 14 R Calf     -> R Foot   (the shin finally has a direction of its own)
    -1, // 15 L Foot     - leaf
    -1, // 16 R Foot     - leaf
};

// stiffening braces beyond the 14 parent links (plan section-3). The grandparent links are
// fold limits: they cap how sharply any joint can hinge, measured at the DEATH pose, and
// they rotate with the body - so corpses topple and slump but cannot collapse into a
// bead-chain pile (the P3 live finding 2026-08-19 21:37: pure neighbor links = heap).
#define RAG_LIMITS        18
#define RAG_LIMIT_MAX_STEP DEG2RAD(12.0f) // a struck forearm can open 33deg of violation in one
                                          // substep; this is a primary limiter, not a belt
#define RAG_LIMIT_KIND_SWING 0
#define RAG_LIMIT_KIND_HINGE 1
#define RAG_LIMIT_KIND_OOP   2

#define RAG_BRACES 18
static const int s_ragBraces[][2] = {
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
    {11, 15}, // L hip - L ankle: with hip->knee and knee->ankle both fixed links, this distance
    {13, 16}, // R hip - R ankle  is a pure function of the KNEE angle - the first real one
};

// fold limits are INEQUALITY braces: they stop the joint folding TIGHTER than the factor
// of its capture distance, but never stop it straightening (an equality brace froze dead
// arms at their death-pose bend). 0 = structural equality brace at full capture length.
static const float s_ragBraceMinFactor[] = {
    0,     0,     0,     0,     0,     0,     // structural truss: equality
    0.80f,                                    // neck
    0.70f, 0.70f,                             // shoulders
    0.75f, 0.75f,                             // elbows
    0.75f, 0.75f,                             // knees
    0.75f,                                    // inert hip-socket brace
    0.60f, 0.60f,                             // hips: sitting-fold ok, flat jackknife blocked
    0.34f, 0.34f,                             // knees: DERIVED, not picked. thigh 24.10u +
                                              // shin 23.94u, straight leg 48.04u; 140deg of
                                              // flexion leaves 16.43u = 0.342. (0.75 would cap
                                              // the knee at 83deg - a kneeling death exceeds it.)
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
    // per-corpse limit derivation (stage 3)
    vec3_t   limACap[RAG_LIMITS];  // hinge: parent segment direction at capture
    vec3_t   limHCap[RAG_LIMITS];  // hinge: the flexion axis at capture
    float    limLo[RAG_LIMITS], limHi[RAG_LIMITS];   // anatomical range
    float    limLo0[RAG_LIMITS], limHi0[RAG_LIMITS]; // widened to admit the death pose
    byte     limDisabled[RAG_LIMITS];
    float    faceSign[2];
    int      limAgeMs;             // NEVER reset - lifeMs is, and a re-widen on a late shot
                                   // would admit a stale capture value exactly when it matters
    int      limCount, limSat, limOff, limBad;
    float    limMax;
    vec3_t   goal0[RAG_PTS];       // the pose he actually died in - the anchor the rewrite is bounded against
    float    gravScale;            // 0 -> 1 over 250ms (no lurch at handoff)
    int      rampMs;
    vec3_t   driveDir0[RAG_PTS];   // capture OUTGOING (bone->child) directions
    byte     driveOk[RAG_PTS];
    byte     contact[RAG_PTS];     // 2-substep memory of touching the world
    short    limpMs[RAG_PTS];      // >0 = recently struck: the shape-match yields on this point
    short    limpMax[RAG_PTS];     // the window that limpMs is counting down FROM (see the ramp)
    float    ptRadius[RAG_PTS];    // per-point collision radius, clamped to capture clearance
    float    bodyRot[3][3];        // SMOOTHED body orientation (see RagBodyRotation)
    byte     bodyRotValid;
    byte     rotLocked;            // latched once the body rests on the world - never re-fits
    // round-10 instruments: nine rounds tuned a rotation nobody ever measured (drift= is provably
    // blind to it - a rigid rotation cancels exactly, so a body standing on its head reads ~1.1)
    float    rotSample[3][3];      // raw fit at the last spin sample
    int      rotSampleMs;
    float    spinRate;             // deg/s over the most recent window
    float    spinMax;
    float    spinYawFrac;          // |world-z share| of the last window's rotation, 0..1
    int      rotLockAtMs;          // lifeMs when the latch fired (-1 = never)
    byte     ctcMax;               // PEAK simultaneous contacts (contacts= has a ~50% duty cycle)
    float    stretchMax;           // peak len/restLen over the 14 parent links
    vec3_t   capSpan;              // AABB of the captured pose
    byte     preLifted;            // points the capture pre-lift actually moved
    vec3_t   entOriginLast;        // entity placement last frame (blast-toss carry)
    byte     entOriginValid;
    // swing instrument: how far the struck bone actually ROTATED. This is the only unit that
    // measures what the user is looking at - drift/span/maxspd are all blind to it.
    int      swingBone;            // sim point whose bone is being watched (-1 = none)
    vec3_t   swingDir0;            // that bone's direction at the moment of impact
    float    swingMax;             // peak degrees since
    float    swingLast;            // last body's peak, kept for the sleep print
    short    rawBad;               // frames the anatomical triad was degenerate
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
static const float s_ragPtRadius[] = {
    7.0f, 7.0f, 7.5f, 4.0f, 5.0f, // pelvis, spine1, spine2, neck, head
    4.0f, 3.0f, 2.5f,             // L upperarm, forearm, hand
    4.0f, 3.0f, 2.5f,             // R arm
    5.0f, 4.0f,                   // L thigh, calf
    5.0f, 4.0f,                   // R thigh, calf
    3.0f, 3.0f,                   // L foot, R foot
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
static cvar_t  *rag_rotlock  = NULL;
static cvar_t  *rag_slew     = NULL;
static cvar_t  *rag_carry    = NULL;
static cvar_t  *rag_velcap   = NULL;
static cvar_t  *rag_leash    = NULL;
static cvar_t  *rag_truss    = NULL;
static cvar_t  *rag_couple   = NULL;
static cvar_t  *rag_impact   = NULL;
static cvar_t  *rag_linear   = NULL;
static cvar_t  *rag_anchor   = NULL;
static cvar_t  *rag_stick    = NULL;
static cvar_t  *rag_stickmax = NULL;
static cvar_t  *rag_buriedmax = NULL;
static cvar_t  *rag_feet     = NULL;
static cvar_t  *rag_limits   = NULL;
static cvar_t  *rag_self     = NULL;

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
        // Round 10: every fix made live during the 2026-08-20 playtest is a console line now, so
        // the next A/B costs a keypress instead of a rebuild. Defaults == committed behaviour.
        rag_rotlock = cgi.Cvar_Get("coop_ragdollRotLock", "1", CVAR_TEMP);    // latch (0 = off)
        rag_slew    = cgi.Cvar_Get("coop_ragdollSlew", "0.12", CVAR_TEMP);    // 1 = pre-fix adopt
        rag_carry   = cgi.Cvar_Get("coop_ragdollCarry", "0.85", CVAR_TEMP);   // 0 = pre-fix
        rag_velcap  = cgi.Cvar_Get("coop_ragdollVelCap", "8", CVAR_TEMP);     // 24 = pre-fix
        rag_leash   = cgi.Cvar_Get("coop_ragdollLeash", "128", CVAR_TEMP);    // 0 = off
        // THE TRUSS EXPERIMENT. 1 = today's anti-pile scaffolding at full strength; 0 = no
        // braces at all. The equality braces weld thigh-to-thigh, shoulder-to-shoulder and each
        // shoulder to the opposite hip, so the corpse is nearly rigid: a bullet anywhere slides
        // the whole body instead of moving the limb, and kicking one leg drags the other. This
        // knob tests - before we build 20 angular joint limits on the assumption - whether a
        // LOOSE body actually articulates. Expect piles at 0: that is the thing limits fix.
        rag_truss = cgi.Cvar_Get("coop_ragdollTruss", "1", CVAR_TEMP);
        // the torque couple that turns a bullet into limb ROTATION - THE knob to sweep live
        rag_couple = cgi.Cvar_Get("coop_ragdollCouple", "1.1", CVAR_TEMP);
        rag_impact = cgi.Cvar_Get("coop_ragdollImpact", "1", CVAR_TEMP); // 0 = no post-death hits
        // how much of a hit is PUSH rather than TWIST. The two used to be welded together: the
        // weights (1+c, -c) always summed to 1.0, so no amount of couple could reduce the shove
        // that slides a corpse across the floor. Now w = (lin + c, lin - c): the SUM (2*lin) is
        // the slide and the DIFFERENCE (2*c) is the limb rotation, tunable independently.
        rag_linear = cgi.Cvar_Get("coop_ragdollLinear", "0.30", CVAR_TEMP);
        // and a weak spring holding the hips near where the body actually lies, so a struck limb
        // moves without the whole corpse wandering off (0 = off)
        rag_anchor = cgi.Cvar_Get("coop_ragdollAnchor", "0.10", CVAR_TEMP);
        // 1 = a struck limb KEEPS its new position instead of being reeled back to the death
        // pose. The shape-match holds every point to the pose captured at death, so a shot limb
        // swung and then snapped home within ~600ms and the damage read as cosmetic. With this
        // on, the pose itself is rewritten for the limb that moved: the body accumulates the
        // shape you shot it into. 0 = the old snap-back.
        rag_stick = cgi.Cvar_Get("coop_ragdollStick", "1", CVAR_TEMP);
        // how far, in units, a limb's resting place may be rewritten away from the pose the man
        // actually died in. Unbounded, every hit compounds and the corpse mangles into poses no
        // body can hold; this keeps the damage visible but anatomically anchored.
        rag_stickmax = cgi.Cvar_Get("coop_ragdollStickMax", "12", CVAR_TEMP);
        // feet are the most burial-prone points on a corpse, and adding two of them tightens an
        // absolute threshold that used to see 15 points
        rag_buriedmax = cgi.Cvar_Get("coop_ragdollBuriedMax", "5", CVAR_TEMP);
        // 0 = seed the feet ON the knees, which makes driveOk fall to 0 by itself and reverts
        // the body to the byte-for-byte 15-point sim inside a 17-point array
        rag_feet = cgi.Cvar_Get("coop_ragdollFeet", "1", CVAR_TEMP);
        // the 18 angular joint limits. ATOMIC with the fold-brace gate by construction:
        // this one cvar is the single authority over both, so the pair cannot half-ship.
        rag_limits = cgi.Cvar_Get("coop_ragdollLimits", "1", CVAR_TEMP);
        // self-collision: scales the minimum separation between body parts that are not
        // already tied together. 0 = off, 1 = full anatomical radii, 0.85 leaves a little
        // slack so a corpse can still lie with its arm against its chest.
        rag_self = cgi.Cvar_Get("coop_ragdollSelf", "0.85", CVAR_TEMP);
    }
}

// A table that forgot a row now SHRINKS, so these catch it at compile time. SIZED arrays would
// not: C zero-fills the missing rows and sizeof() still equals RAG_PTS, so the guard would pass
// in exactly the case it exists to catch. The three invisible zero-fills this prevents: a 0
// radius makes the pre-lift a POINT trace (bug-1962's pin), a 0 drive-child aims a foot at the
// pelvis, a 0 min-factor turns a fold limit into an EQUALITY brace welding ankle to hip - and a
// NULL bone name is an access violation inside stricmp on the first kill.
typedef char rag_chk_bones[(sizeof(s_ragBones) / sizeof(s_ragBones[0]) == RAG_PTS) ? 1 : -1];
typedef char rag_chk_drive[(sizeof(s_ragDriveChild) / sizeof(s_ragDriveChild[0]) == RAG_PTS) ? 1 : -1];
typedef char rag_chk_radius[(sizeof(s_ragPtRadius) / sizeof(s_ragPtRadius[0]) == RAG_PTS) ? 1 : -1];
typedef char rag_chk_brace[(sizeof(s_ragBraces) / sizeof(s_ragBraces[0]) == RAG_BRACES) ? 1 : -1];
typedef char rag_chk_minf[(sizeof(s_ragBraceMinFactor) / sizeof(s_ragBraceMinFactor[0]) == RAG_BRACES) ? 1 : -1];

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



// ---------- self-collision (stage 4) --------------------------------------------------------
// Joint limits constrain ANGLES; they say nothing about two body parts occupying the same space.
// Without this a hand passes through the chest, the knees swap sides, and a corpse shot enough
// times slowly merges into itself. These are push-apart-ONLY minimum separations between pairs
// that are not already tied by a link or an equality brace - a pair that IS tied would fight it.
// Distances come from the anatomical radius table, not the per-corpse clamped radii, because the
// clamped ones shrink to the floor clearance and would let parts inter-penetrate on the ground.
#define RAG_SELF_PAIRS 16
static const byte s_ragSelfPairs[RAG_SELF_PAIRS][2] = {
    {7, 10},  // hand - hand
    {7, 2},   // L hand - chest
    {10, 2},  // R hand - chest
    {7, 0},   // L hand - pelvis
    {10, 0},  // R hand - pelvis
    {7, 4},   // L hand - head
    {10, 4},  // R hand - head
    {6, 2},   // L forearm - chest
    {9, 2},   // R forearm - chest
    {6, 9},   // forearm - forearm
    {12, 14}, // knee - knee
    {15, 16}, // foot - foot
    {15, 14}, // L foot - R knee
    {16, 12}, // R foot - L knee
    {4, 0},   // head - pelvis
    {4, 2},   // head - chest
};

// Push-apart-only, and BOTH pt and ptPrev move by the same delta so the separation is perfectly
// inelastic: it cannot inject the velocity that blew bodies across the map twice in this project.
static void RagSelfCollide(ragSim_t *s, float scale)
{
    int i;
    for (i = 0; i < RAG_SELF_PAIRS; i++) {
        int    a = s_ragSelfPairs[i][0], b = s_ragSelfPairs[i][1];
        vec3_t d;
        float  len, minSep, corr;
        minSep = (s_ragPtRadius[a] + s_ragPtRadius[b]) * scale;
        VectorSubtract(s->pt[a], s->pt[b], d);
        len = VectorLength(d);
        if (len >= minSep) {
            continue;
        }
        if (len < 0.001f) {
            VectorSet(d, 0, 0, 1); // exactly coincident: pick an axis rather than divide by zero
            len = 0.001f;
        }
        corr = (minSep - len) * 0.5f / len;
        VectorMA(s->pt[a], corr, d, s->pt[a]);
        VectorMA(s->ptPrev[a], corr, d, s->ptPrev[a]);
        VectorMA(s->pt[b], -corr, d, s->pt[b]);
        VectorMA(s->ptPrev[b], -corr, d, s->ptPrev[b]);
    }
}

// ============================ ANGULAR JOINT LIMITS (stage 3) ================================
// What stops a corpse folding into poses no body can hold. Distance constraints keep bones the
// right LENGTH but say nothing about direction: a knee can bend backwards without any distance
// changing. These 18 limits are derived per corpse from its own skeleton at capture, so they
// work across every model without hand authoring.

typedef struct {
    byte  kind;
    byte  frame;   // 0 = pelvis triad, 1 = chest triad
    byte  pivot, grand, child;
    byte  axisRow; // swing: which triad row is the axis
    float neutSign; // swing: neutral is neutSign * T[2]
    float hSign;    // hinge: +1 knee (hinges back), -1 elbow (hinges forward)
    float lo, hi;   // radians
    unsigned mask;  // the child subtree that rotates
} ragLimitDef_t;

// Ranges are ANATOMICAL and their signs are derived, not copied: RagSignedAngle returns theta
// for n2 = R(n,theta)*n1, so positive phi moves b toward (n x n1). For a hip, axis = T[1] = L
// and neutral = -T[2] = -U, so n x n1 = L x -U = -F: POSITIVE IS BACKWARD, i.e. EXTENSION.
// An earlier design table had the hips as [-25,+110], which grants 110 degrees of BACKWARD
// swing - and no instrument can see a range inversion, the body just settles wrong. Do not
// "tidy" these signs without re-deriving them.
static const ragLimitDef_t s_ragLimits[RAG_LIMITS] = {
    // kind                  frame pivot grand child axisRow neutSign hSign   lo        hi      mask
    {RAG_LIMIT_KIND_SWING, 1,  3, 0,  4, 1, +1.0f, 0, DEG2RAD(-50), DEG2RAD(55),  (1u<<4)},
    {RAG_LIMIT_KIND_SWING, 1,  3, 0,  4, 0, +1.0f, 0, DEG2RAD(-40), DEG2RAD(40),  (1u<<4)},
    {RAG_LIMIT_KIND_SWING, 1,  5, 0,  6, 1, -1.0f, 0, DEG2RAD(-170),DEG2RAD(60),  (1u<<6)|(1u<<7)},
    {RAG_LIMIT_KIND_SWING, 1,  5, 0,  6, 0, -1.0f, 0, DEG2RAD(-20), DEG2RAD(150), (1u<<6)|(1u<<7)},
    {RAG_LIMIT_KIND_SWING, 1,  8, 0,  9, 1, -1.0f, 0, DEG2RAD(-170),DEG2RAD(60),  (1u<<9)|(1u<<10)},
    {RAG_LIMIT_KIND_SWING, 1,  8, 0,  9, 0, -1.0f, 0, DEG2RAD(-150),DEG2RAD(20),  (1u<<9)|(1u<<10)},
    {RAG_LIMIT_KIND_HINGE, 1,  6, 5,  7, 0,  0.0f, -1.0f, DEG2RAD(2), DEG2RAD(150), (1u<<7)},
    {RAG_LIMIT_KIND_OOP,   1,  6, 5,  7, 0,  0.0f, -1.0f, DEG2RAD(-12), DEG2RAD(12), (1u<<7)},
    {RAG_LIMIT_KIND_HINGE, 1,  9, 8, 10, 0,  0.0f, -1.0f, DEG2RAD(2), DEG2RAD(150), (1u<<10)},
    {RAG_LIMIT_KIND_OOP,   1,  9, 8, 10, 0,  0.0f, -1.0f, DEG2RAD(-12), DEG2RAD(12), (1u<<10)},
    {RAG_LIMIT_KIND_SWING, 0, 11, 0, 12, 1, -1.0f, 0, DEG2RAD(-115),DEG2RAD(25),  (1u<<12)|(1u<<15)},
    {RAG_LIMIT_KIND_SWING, 0, 11, 0, 12, 0, -1.0f, 0, DEG2RAD(-15), DEG2RAD(55),  (1u<<12)|(1u<<15)},
    {RAG_LIMIT_KIND_SWING, 0, 13, 0, 14, 1, -1.0f, 0, DEG2RAD(-115),DEG2RAD(25),  (1u<<14)|(1u<<16)},
    {RAG_LIMIT_KIND_SWING, 0, 13, 0, 14, 0, -1.0f, 0, DEG2RAD(-55), DEG2RAD(15),  (1u<<14)|(1u<<16)},
    {RAG_LIMIT_KIND_HINGE, 0, 12, 11, 15, 0, 0.0f, +1.0f, DEG2RAD(2), DEG2RAD(150), (1u<<15)},
    {RAG_LIMIT_KIND_OOP,   0, 12, 11, 15, 0, 0.0f, +1.0f, DEG2RAD(-8), DEG2RAD(8),  (1u<<15)},
    {RAG_LIMIT_KIND_HINGE, 0, 14, 13, 16, 0, 0.0f, +1.0f, DEG2RAD(2), DEG2RAD(150), (1u<<16)},
    {RAG_LIMIT_KIND_OOP,   0, 14, 13, 16, 0, 0.0f, +1.0f, DEG2RAD(-8), DEG2RAD(8),  (1u<<16)},
};

// T rows = [Forward, Left, Up]. MOHAA convention: at yaw 0, axis[0]=(1,0,0) fwd, axis[1]=(0,1,0)
// LEFT, axis[2]=(0,0,1) up. Built from points rigidly attached to the same bone in ANY pose -
// the hip sockets are the thigh origins, the shoulder sockets the upper-arm origins - so this
// needs no bind pose.
static qboolean RagBodyTriad(const vec3_t tip, const vec3_t base, const vec3_t sockR,
                             const vec3_t sockL, float faceSign, float T[3][3])
{
    vec3_t U, L, F;
    VectorSubtract(tip, base, U);
    if (VectorNormalize(U) < 0.001f) {
        return qfalse;
    }
    VectorSubtract(sockL, sockR, L);
    VectorMA(L, -DotProduct(L, U), U, L); // Gram-Schmidt against U
    if (VectorNormalize(L) < 0.001f) {
        return qfalse;
    }
    VectorScale(L, faceSign, L);
    CrossProduct(L, U, F);
    VectorCopy(F, T[0]);
    VectorCopy(L, T[1]);
    VectorCopy(U, T[2]);
    return qtrue;
}

static float RagSignedAngle(const vec3_t n1, const vec3_t n2, const vec3_t n)
{
    vec3_t x;
    CrossProduct(n1, n2, x);
    return (float)atan2(DotProduct(x, n), DotProduct(n1, n2));
}

static void RagMat3FromAxisAngle(const vec3_t axis, float ang, float out[3][3])
{
    float c = (float)cos(ang), sn = (float)sin(ang), t = 1.0f - c;
    float x = axis[0], y = axis[1], z = axis[2];
    out[0][0] = t * x * x + c;      out[0][1] = t * x * y + sn * z; out[0][2] = t * x * z - sn * y;
    out[1][0] = t * x * y - sn * z; out[1][1] = t * y * y + c;      out[1][2] = t * y * z + sn * x;
    out[2][0] = t * x * z + sn * y; out[2][1] = t * y * z - sn * x; out[2][2] = t * z * z + c;
}

// Rotating the whole child SUBTREE, not one point, is what makes this stable: a rigid rotation
// preserves every distance inside the set, so a limit can never fight a link it passes through,
// and the link crossing the pivot survives because the pivot is ON the axis.
static void RagRotateSet(ragSim_t *s, unsigned mask, const vec3_t pivot, const float R[3][3])
{
    int i;
    for (i = 0; i < RAG_PTS; i++) {
        vec3_t r, o;
        if (!(mask & (1u << i))) {
            continue;
        }
        // NON-NEGOTIABLE: pt AND ptPrev by the SAME rotation about the SAME pivot. In Verlet the
        // gap between them IS the velocity, so rotating both leaves |v| exactly unchanged and
        // tangent to the stop - the limb slides ALONG the limit instead of buzzing against it.
        // Moving pt alone is the pattern that produced the flying-body blowup twice already.
        VectorSubtract(s->pt[i], pivot, r);
        RagMat3RotateVec(R, r, o);
        VectorAdd(pivot, o, s->pt[i]);
        VectorSubtract(s->ptPrev[i], pivot, r);
        RagMat3RotateVec(R, r, o);
        VectorAdd(pivot, o, s->ptPrev[i]);
    }
}

static void RagLimitFrames(ragSim_t *s, float T0[3][3], float T2[3][3], qboolean ok[2]);

// ---------- torso twist limit ---------------------------------------------------------------
// A spine can rotate maybe 35 degrees relative to the hips. Nothing in a distance-constraint
// model forbids more: the shoulder line can wind around the spine axis indefinitely and every
// bone keeps its length, so a corpse shot repeatedly can end up with its chest facing backwards.
// Measured between the two anatomical triads and corrected by rotating the whole upper body
// about the spine axis, which is a rigid rotation and therefore preserves every distance inside
// that set. The two torso-cross braces resist the same twist, so they agree with this rather
// than fighting it - but they only slow it, they cannot bound it, which is why this exists.



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
        if (rag_debug->integer) {
            cgi.Printf("^~^~^ RAGDOLL capture FAILED ent=%d reason=no-tiki\n", ns->number);
        }
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
        if (rag_debug->integer) {
            cgi.Printf("^~^~^ RAGDOLL capture FAILED ent=%d reason=no-channels\n", ns->number);
        }
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
        if (i >= 15 && !rag_feet->integer) {
            s->simChan[i] = -1; // rollback path: behave exactly like a footless model
        }
        if (s->simChan[i] < 0 || s->simChan[i] >= s->count) {
            if (i >= 15) {
                // FOOTLESS MODEL, or coop_ragdollFeet 0. Never bail for a foot: a bail costs
                // that corpse its ragdoll permanently, and coverage is the number this project
                // is not handing back. Seeding the foot ON the knee keeps all 17 array slots
                // valid, makes the zero-length link a no-op, and drops driveOk[12] to 0 by
                // itself - which reverts the calf to today's leaf behaviour with no special
                // case anywhere in RagPush.
                s->simChan[i] = -1;
                VectorCopy(s->pt[(i == 15) ? 12 : 14], s->pt[i]);
                VectorCopy(s->pt[i], s->ptPrev[i]);
                continue;
            }
            if (rag_debug->integer) {
                cgi.Printf("^~^~^ RAGDOLL capture FAILED ent=%d reason=missing-tag '%s'\n",
                           ns->number, s_ragBones[i].name);
            }
            return qfalse; // vet1 verified all 17 names across the roster; a miss = bail clean
        }
        if (s->simChan[i] < 0) {
            continue; // seeded foot: pt[] already set above, it has no channel of its own
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
        // s_ragPtRadius (the static table), NOT s->ptRadius: the per-sim radii are computed ~100
        // lines BELOW this loop and the slot is memset before every capture, so this read was
        // always 0.0f - which the collision model turns into a POINT trace. A capture-buried
        // point was then seated flush, the clearance probe below startsolid'd, its radius kept
        // the full anatomical value, and that point was startsolid on every sweep for the rest of
        // its life: permanently released on settle, permanently frozen on free - bug-1962's pin.
        VectorSet(pm, -s_ragPtRadius[i], -s_ragPtRadius[i], -s_ragPtRadius[i]);
        VectorSet(px, s_ragPtRadius[i], s_ragPtRadius[i], s_ragPtRadius[i]);
        cgi.CM_BoxTrace(&tr, above, s->pt[i], pm, px, 0, MASK_DEADSOLID, qfalse);
        if (!tr.startsolid && tr.fraction < 1.0f) {
            VectorCopy(tr.endpos, s->pt[i]);
            s->pt[i][2] += 0.25f;
            VectorCopy(s->pt[i], s->ptPrev[i]);
            s->preLifted++;
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
        if (nTorso > 0 || s->buried >= rag_buriedmax->integer) {
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
        if (s->simChan[i] < 0) {
            // seeded foot on a footless model (241 of 1626 human tiks): it has no channel, and
            // mat0[-1] reads 12 floats of entOrigin/entAxis - benign only by accident today, and
            // if that accident ever changes the rot0 sandwich stops cancelling and the offset
            // scales by ~1e6 with RagSane none the wiser, since it only checks pt[].
            RagMat3Identity(s->rot0[i]);
            s->restLen[i] = 0;
            VectorSet(s->restDir[i], 0, 0, 1);
            continue;
        }
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
    // Per-point collision radius, CLAMPED TO THE CLEARANCE THE AUTHORED POSE ALREADY HAS.
    // The animator's pose is by definition a correct-looking resting pose, so a point sitting
    // 1u off the floor must keep a 1u box - inflating it to the anatomical radius shoves the
    // body up off its own pose and it hovers (live 2026-08-20: "ragdolls float around a bit
    // with their legs angled upwards" - the pelvis alone was lifting 7u).
    for (i = 0; i < RAG_PTS; i++) {
        trace_t tr;
        vec3_t  down, tiny = {-1, -1, -1}, tinyx = {1, 1, 1};
        float   clear = s_ragPtRadius[i];
        VectorCopy(s->pt[i], down);
        down[2] -= s_ragPtRadius[i] + 2.0f;
        cgi.CM_BoxTrace(&tr, s->pt[i], down, tiny, tinyx, 0, MASK_DEADSOLID, qfalse);
        if (!tr.startsolid && tr.fraction < 1.0f) {
            clear = (s_ragPtRadius[i] + 2.0f) * tr.fraction;
        }
        if (clear > s_ragPtRadius[i]) {
            clear = s_ragPtRadius[i];
        }
        if (clear < 1.0f) {
            clear = 1.0f; // never zero: a zero-size box tunnels through everything
        }
        s->ptRadius[i] = clear;
    }

    VectorSubtract(s->pt[13], s->pt[11], s->hipDir0); // L thigh -> R thigh (world)
    VectorNormalize(s->hipDir0);

    // seed the orientation filter here: RagRawFit is EXACTLY identity at capture (restDir[1] and
    // hipDir0 are built from these very points), so this reproduces what the first RagPush used
    // to seed - but RagPush is a pure reader now and may not seed anything.
    RagMat3Identity(s->bodyRot);
    s->bodyRotValid = 1;
    s->rotLockAtMs  = -1; // memset leaves 0, which would read as "latched on frame 0"
    {
        vec3_t cmn, cmx;
        int    k;
        ClearBounds(cmn, cmx);
        for (k = 0; k < RAG_PTS; k++) {
            AddPointToBounds(s->pt[k], cmn, cmx);
        }
        VectorSubtract(cmx, cmn, s->capSpan); // was the corpse captured upright or flat?
    }


    // ---- derive this corpse's joint limits from its own captured pose (stage 3) ----
    {
        float    T[2][3][3];
        qboolean tok[2];
        int      k;
        // faceSign only has to be right to within 90deg: it orients the LEFT axis against the
        // corpse's facing at death
        s->faceSign[0] = s->faceSign[1] = 1.0f;
        RagBodyTriad(s->pt[1], s->pt[0], s->pt[13], s->pt[11], 1.0f, T[0]);
        s->faceSign[0] = (DotProduct(T[0][0], s->entAxis[0]) >= 0.0f) ? 1.0f : -1.0f;
        RagBodyTriad(s->pt[3], s->pt[2], s->pt[8], s->pt[5], 1.0f, T[1]);
        s->faceSign[1] = (DotProduct(T[1][0], s->entAxis[0]) >= 0.0f) ? 1.0f : -1.0f;
        RagLimitFrames(s, T[0], T[1], tok);

        for (k = 0; k < RAG_LIMITS; k++) {
            const ragLimitDef_t *J = &s_ragLimits[k];
            vec3_t               a, b, h, neut, axis, x;
            float                capPhi = 0;
            s->limDisabled[k] = 0;
            s->limLo[k]       = J->lo;
            s->limHi[k]       = J->hi;
            if (!tok[J->frame]) {
                s->limDisabled[k] = 1;
                s->limOff++;
                continue;
            }
            VectorSubtract(s->pt[J->child], s->pt[J->pivot], b);
            if (VectorNormalize(b) < 0.001f) {
                s->limDisabled[k] = 1;
                s->limOff++;
                continue;
            }
            if (J->kind == RAG_LIMIT_KIND_SWING) {
                VectorCopy(T[J->frame][J->axisRow], axis);
                VectorScale(T[J->frame][2], J->neutSign, neut);
                capPhi = RagSignedAngle(neut, b, axis);
            } else {
                VectorSubtract(s->pt[J->pivot], s->pt[J->grand], a);
                if (VectorNormalize(a) < 0.001f) {
                    s->limDisabled[k] = 1;
                    s->limOff++;
                    continue;
                }
                // SIGN IS ANATOMY, NOT MEASUREMENT: a knee flexes so the ankle goes BACKWARD, an
                // elbow so the hand goes FORWARD. The opposite hSign is correct, not a bug.
                VectorScale(T[J->frame][1], J->hSign, h);
                VectorMA(h, -DotProduct(h, a), a, h); // Gram-Schmidt: h perpendicular to a
                if (VectorNormalize(h) < 0.001f) {
                    s->limDisabled[k] = 1;
                    s->limOff++;
                    continue;
                }
                // VALIDATOR, never a source: if the limb is genuinely bent at capture, the real
                // flexion plane must agree with the anatomical rule. If it disagrees this
                // skeleton is not the one we reasoned about, so disable this hinge for this
                // corpse rather than guessing or snapping it straight.
                CrossProduct(a, b, x);
                if (VectorNormalize(x) > 0.342f && DotProduct(x, h) < 0.0f) {
                    s->limDisabled[k] = 1;
                    s->limOff++;
                    continue;
                }
                VectorCopy(a, s->limACap[k]);
                VectorCopy(h, s->limHCap[k]);
                capPhi = (J->kind == RAG_LIMIT_KIND_HINGE) ? RagSignedAngle(a, b, h)
                                                           : (float)asin(DotProduct(b, h));
            }
            // the death pose may already sit outside an anatomical range; snapping it in on
            // frame 1 is a visible pop, so start wide enough to admit it and close over 300ms
            s->limLo0[k] = (capPhi - DEG2RAD(5.0f) < J->lo) ? capPhi - DEG2RAD(5.0f) : J->lo;
            s->limHi0[k] = (capPhi + DEG2RAD(5.0f) > J->hi) ? capPhi + DEG2RAD(5.0f) : J->hi;
            // failsafe: a capture far outside the range means the derivation is suspect
            if (capPhi < J->lo - DEG2RAD(30.0f) || capPhi > J->hi + DEG2RAD(30.0f)) {
                s->limDisabled[k] = 1;
                s->limOff++;
            }
        }
    }

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
// The raw anatomical fit, capture basis -> current basis. PURE: touches no state, so the debug
// instrument and the filter can never perturb each other (before round 10, RagBodyRotation was an
// impure mutator called from three sites - including the sleep DEBUG PRINT, which therefore
// changed the simulation). Exactly identity at capture, because restDir[1] and hipDir0 are both
// built from the capture pt[]: T1 == T0 and raw = T0^T*T0 = I. That is what makes the round-10
// rot= instrument a true total-rotation-since-capture.
static qboolean RagRawFit(const ragSim_t *s, float raw[3][3])
{
    float  T0[3][3], T1[3][3];
    vec3_t spineNow, hipNow;

    VectorSubtract(s->pt[1], s->pt[0], spineNow);
    VectorSubtract(s->pt[13], s->pt[11], hipNow);
    if (RagTriad(s->restDir[1], s->hipDir0, T0) && RagTriad(spineNow, hipNow, T1)) {
        RagMat3TransMul(T0, T1, raw); // row-vector: v' = v*S
        return qtrue;
    }
    VectorNormalize(spineNow);
    RagMat3FromTo(s->restDir[1], spineNow, raw); // degenerate: 1-axis fallback, roll unconstrained
    return qfalse;
}

// Pure READER - every consumer of the body orientation goes through this and none of them can
// advance the filter.
static void RagBodyRotation(const ragSim_t *s, float S[3][3])
{
    memcpy(S, s->bodyRot, sizeof(s->bodyRot));
}

// The ONE advance site (called from RagStep, once per substep, after the constraints and before
// the shape-match - exactly where the old in-line advance happened on the settle branch).
// Two brakes on the orientation: slew toward the new fit rather than adopting it, and once the
// body rests on the world, LATCH it - a corpse lying on the ground does not re-orient itself.
static void RagBodyRotationAdvance(ragSim_t *s)
{
    float raw[3][3], mixed[3][3], T[3][3];
    int   i, r, c, nContact = 0;
    float a;

    if (!RagRawFit(s, raw)) {
        s->rawBad++; // degenerate triad: KEEP the previous orientation. Blending in the roll-free
        return;      // fallback would inject an unconstrained roll - itself a spin source.
    }
    if (!s->bodyRotValid) {
        memcpy(s->bodyRot, raw, sizeof(raw));
        s->bodyRotValid = 1;
        return;
    }
    if (s->rotLocked) {
        return;
    }
    for (i = 0; i < RAG_PTS; i++) {
        if (s->contact[i]) {
            nContact++;
        }
    }
    if (nContact >= 3 && rag_rotlock->integer) {
        s->rotLocked   = 1; // LATCHED, not momentary: contact counts flicker as points settle
        s->rotLockAtMs = s->lifeMs;
    }
    a = s->rotLocked ? 0.0f : rag_slew->value;
    if (a <= 0.0f) {
        return;
    }
    if (a > 1.0f) {
        a = 1.0f;
    }
    for (r = 0; r < 3; r++) {
        for (c = 0; c < 3; c++) {
            mixed[r][c] = s->bodyRot[r][c] * (1.0f - a) + raw[r][c] * a;
        }
    }
    if (RagTriad(mixed[0], mixed[1], T)) { // re-orthonormalize the blend
        memcpy(s->bodyRot, T, sizeof(T));
    }
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
        if (s->limpMs[i] > 0 && rag_stick->integer && i > 4) {
            // i > 4: pelvis, spines and neck are EXCLUDED. Letting damage rewrite the
            // torso's own resting shape is what allows a chest to accumulate twist and
            // deformation over repeated hits until the body reads as melted. Limbs and the
            // head keep what the bullet did to them; the trunk keeps its authored shape.
            // THE LIMB KEEPS WHAT THE BULLET DID TO IT. Rewrite this point's goal to where it
            // actually is now, expressed in the body's own frame, so when the limp window closes
            // the pose being held IS the new one. Bone lengths are still enforced by the distance
            // links and the truss still forbids a collapse - only the limb's resting angle moves.
            vec3_t rel, inv, want, dev;
            float  devLen, cap = rag_stickmax->value;
            VectorSubtract(s->pt[i], s->pt[0], rel);
            RagMat3TransRotateVec(S, rel, inv); // inverse of the body rotation (row-vector)
            VectorAdd(s->goal[0], inv, want);
            // ... but never further than cap units from the pose he died in. Without this every
            // hit compounds on the last and the body walks into positions no anatomy allows.
            VectorSubtract(want, s->goal0[i], dev);
            devLen = VectorLength(dev);
            if (cap > 0.0f && devLen > cap) {
                VectorScale(dev, cap / devLen, dev);
                VectorAdd(s->goal0[i], dev, want);
            }
            VectorCopy(want, s->goal[i]);
        }
        if (s->limpMs[i] > 0) {
            // struck limb: essentially free at the instant of impact, easing back to the full
            // pose pull as the window expires (a hard restore would snap it home at the deadline).
            // Ramp against the window THIS point was actually given, not a fixed constant: call
            // sites pass 600-1410ms while the constant is 600, so anything longer drove k negative
            // and VectorMA then pushed the point AWAY from its pose, compounding every substep -
            // an anti-shape-match running ~568ms on every grenade near a corpse.
            float span = (s->limpMax[i] > 0) ? (float)s->limpMax[i] : (float)RAG_IMPACT_LIMP_MS;
            float k    = 1.0f - (float)s->limpMs[i] / span;
            if (k < 0.0f) {
                k = 0.0f;
            } else if (k > 1.0f) {
                k = 1.0f;
            }
            a *= RAG_IMPACT_RELAX + (1.0f - RAG_IMPACT_RELAX) * k;
        }
        VectorMA(s->pt[i], a, d, s->pt[i]);
        // ... and carry ptPrev most of the way with it. In Verlet the gap between pt and ptPrev
        // IS the velocity, so a pull that moves pt alone injects (a*|d|)/dt of speed every
        // substep - which compounds into the "spassing and flying across the world" blowup seen
        // live 2026-08-20. Carrying 85% leaves a little genuine settling motion and throws the
        // rest away instead of banking it.
        VectorMA(s->ptPrev[i], a * rag_carry->value, d, s->ptPrev[i]);
    }
}


// Rebuild both body frames from the CURRENT points. On a degenerate spine or a hip line
// parallel to it, every limit in that frame is skipped for this iteration.
static void RagLimitFrames(ragSim_t *s, float T0[3][3], float T2[3][3], qboolean ok[2])
{
    ok[0] = RagBodyTriad(s->pt[1], s->pt[0], s->pt[13], s->pt[11], s->faceSign[0], T0);
    ok[1] = RagBodyTriad(s->pt[3], s->pt[2], s->pt[8], s->pt[5], s->faceSign[1], T2);
}

static void RagLimitApply(ragSim_t *s, int idx, const vec3_t axis, float phi, float lo, float hi,
                          float k)
{
    const ragLimitDef_t *J = &s_ragLimits[idx];
    float                target = (phi < lo) ? lo : ((phi > hi) ? hi : phi);
    float                d, R[3][3];

    if (target == phi) {
        return; // inequality: project only when actually violated
    }
    // CHATTER RAMP. A limit that switches on and off between iterations makes the limb buzz at
    // the boundary - live 2026-08-20, "sometimes the head kinda stutters after being shot up",
    // the head being a leaf with a single link and two neck limits acting on it. Fade the
    // correction in over the first 4 degrees of violation so a hair's-breadth overshoot gets a
    // hair's-breadth correction instead of a full-strength one.
    {
        float viol = (float)fabs(target - phi) / DEG2RAD(4.0f);
        if (viol > 1.0f) {
            viol = 1.0f;
        }
        k *= viol * viol * (3.0f - 2.0f * viol); // smoothstep
    }
    d = (target - phi) * k;
    if (d > RAG_LIMIT_MAX_STEP) {
        d = RAG_LIMIT_MAX_STEP;
        s->limSat++;
    } else if (d < -RAG_LIMIT_MAX_STEP) {
        d = -RAG_LIMIT_MAX_STEP;
        s->limSat++;
    }
    RagMat3FromAxisAngle(axis, d, R);
    RagRotateSet(s, J->mask, s->pt[J->pivot], R);
    s->limCount++;
    if (fabs(d) > s->limMax) {
        s->limMax = (float)fabs(d);
    }
}

// One sweep of all 18 limits. Order is REVERSED on odd iterations so Gauss-Seidel bias does not
// make one side of the body stiffer than the other.
static void RagLimitSweep(ragSim_t *s, int iter)
{
    float    T[2][3][3], k;
    qboolean ok[2];
    float    t, span;
    int      n, i;

    if (!rag_limits->integer) {
        return;
    }
    RagLimitFrames(s, T[0], T[1], ok);
    if (!ok[0] && !ok[1]) {
        s->limBad++;
        return;
    }
    // iteration-count-independent stiffness, so tuning survives an RAG_ITERS change
    k = 1.0f - (float)pow(1.0f - 0.98f, 1.0f / (float)RAG_ITERS);
    // ranges start wide enough to admit the pose he died in, and close to anatomical over 300ms
    t = (s->limAgeMs >= 300) ? 1.0f : (float)s->limAgeMs / 300.0f;

    for (n = 0; n < RAG_LIMITS; n++) {
        const ragLimitDef_t *J;
        vec3_t               a, b, h, axis, neut;
        float                phi, lo, hi;
        i = (iter & 1) ? (RAG_LIMITS - 1 - n) : n;
        J = &s_ragLimits[i];
        if (s->limDisabled[i] || !ok[J->frame]) {
            continue;
        }
        lo = s->limLo0[i] + (s->limLo[i] - s->limLo0[i]) * t;
        hi = s->limHi0[i] + (s->limHi[i] - s->limHi0[i]) * t;

        VectorSubtract(s->pt[J->child], s->pt[J->pivot], b);
        if (VectorNormalize(b) < 0.001f) {
            continue;
        }
        if (J->kind == RAG_LIMIT_KIND_SWING) {
            VectorCopy(T[J->frame][J->axisRow], axis);
            VectorScale(T[J->frame][2], J->neutSign, neut);
            phi = RagSignedAngle(neut, b, axis);
        } else {
            // hinge: the axis rides the PARENT segment's own swing since capture, so it can
            // never be flipped by animation noise the way a live cross product can
            float Rp[3][3];
            VectorSubtract(s->pt[J->pivot], s->pt[J->grand], a);
            if (VectorNormalize(a) < 0.001f) {
                continue;
            }
            RagMat3FromTo(s->limACap[i], a, Rp);
            RagMat3RotateVec(Rp, s->limHCap[i], h);
            if (J->kind == RAG_LIMIT_KIND_HINGE) {
                VectorCopy(h, axis);
                phi = RagSignedAngle(a, b, h);
            } else {
                // out-of-plane: how far the child has left the hinge plane
                vec3_t oop;
                CrossProduct(b, h, oop);
                if (VectorNormalize(oop) < 0.001f) {
                    continue;
                }
                VectorCopy(oop, axis);
                phi = (float)asin(DotProduct(b, h) > 1.0f ? 1.0f
                                  : (DotProduct(b, h) < -1.0f ? -1.0f : DotProduct(b, h)));
            }
        }
        span = hi - lo;
        if (span < DEG2RAD(1.0f)) {
            continue;
        }
        RagLimitApply(s, i, axis, phi, lo, hi, k);
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
        {
            float vcap = rag_velcap->value;
            if (vcap < 1.0f) { vcap = 1.0f; }
            if (vcap > 64.0f) { vcap = 64.0f; }
            if (vlen > vcap) {
                VectorScale(vel, vcap / vlen, vel);
            }
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
            float  len, corr, tstiff = rag_truss->value;
            if (tstiff <= 0.0f) {
                break; // truss off: the body is free to articulate (and free to pile - the
                       // experiment that tells us whether angular limits are worth building)
            }
            if (tstiff > 1.0f) {
                tstiff = 1.0f;
            }
            VectorSubtract(s->pt[a], s->pt[b], d);
            len = VectorLength(d);
            if (len < 0.001f) {
                continue;
            }
            if (s_ragBraceMinFactor[i] > 0 && i != 13 && rag_limits->integer) {
                // ATOMICITY, ENFORCED AT RUNTIME. These fold rows cap the very joints the
                // angular limits now cap properly - with a direction instead of a distance -
                // and a min-distance brace shoving a hand away from spine2 while a shoulder
                // limit pulls the arm across is a guaranteed limit cycle. Row 13 is the one
                // exception: neither endpoint is in any limit's moving set and its distance
                // is not fixed, so it stays a live bound on a lumbar DOF no limit covers.
                continue;
            }
            if (s_ragBraceMinFactor[i] > 0 && len >= s->braceLen[i]) {
                continue; // inequality fold limit: only ever pushes APART
            }
            corr = (len - s->braceLen[i]) * 0.5f * tstiff / len; // firm: the anti-pile truss
            VectorMA(s->pt[a], -corr, d, s->pt[a]);
            VectorMA(s->pt[b], corr, d, s->pt[b]);
        }
        RagLimitSweep(s, it); // the 18 angular limits, inside the iteration loop
        if (rag_self->value > 0.0f) {
            RagSelfCollide(s, rag_self->value); // ... then stop parts sharing space
        }
    }
    for (i = 0; i < RAG_PTS; i++) { // impact-limp windows tick down with the sim, not the frame
        if (s->limpMs[i] > 0) {
            s->limpMs[i] -= RAG_SUBSTEP_MS;
            if (s->limpMs[i] < 0) {
                s->limpMs[i] = 0;
            }
        }
    }
    // PELVIS ANCHOR. Nothing in the system holds the hips: the shape-match ties all 14 other
    // points TO the pelvis and the pelvis itself is free, so any net push slides the whole
    // corpse across the floor ("bodies still slide a bit after you shoot them"). A weak spring
    // toward where the body actually lies kills the wander without touching limb rotation.
    // pt and ptPrev move together (no energy injection - that mistake has cost this project
    // twice), then the pelvis velocity is bled so it settles instead of oscillating.
    if (s->branch && rag_anchor->value > 0.0f) {
        vec3_t d;
        float  dist;
        VectorSubtract(s->goal[0], s->pt[0], d);
        dist = VectorLength(d);
        if (dist > 1.0f) {
            float a = rag_anchor->value;
            vec3_t v;
            if (a > 0.5f) {
                a = 0.5f;
            }
            VectorMA(s->pt[0], a, d, s->pt[0]);
            VectorMA(s->ptPrev[0], a, d, s->ptPrev[0]);
            VectorSubtract(s->pt[0], s->ptPrev[0], v);
            VectorScale(v, 0.90f, v);
            VectorSubtract(s->pt[0], v, s->ptPrev[0]);
        }
    }
    s->limAgeMs += RAG_SUBSTEP_MS; // NEVER reset: lifeMs is, and a re-widen on a late shot
                                  // would admit a stale capture value exactly when the
                                  // limits are most needed
    RagBodyRotationAdvance(s); // the ONE filter advance, after the constraints
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
        if (rag_self->value > 0.0f) {
            RagSelfCollide(s, rag_self->value);
        }
        RagLimitSweep(s, 0); // the shape-match moves points with PER-POINT alphas, so it is
                             // not rigid and can re-violate a limit it was just corrected
                             // out of. Collision still gets the last word after RagStep.
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
        VectorSet(pm, -s->ptRadius[i], -s->ptRadius[i], -s->ptRadius[i]);
        VectorSet(px, s->ptRadius[i], s->ptRadius[i], s->ptRadius[i]);
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
                s->contact[i] = 0;                  // full-strength pull home, not the relaxed one
                VectorCopy(s->pt[i], s->ptPrev[i]); // ... but it must not KEEP the speed it had, or
            }                                       // it integrates freely inside solid and emerges
                                                    // anywhere. Killed, it creeps out along the pull.
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
            VectorSet(pm, -s->ptRadius[i], -s->ptRadius[i], -s->ptRadius[i]);
            VectorSet(px, s->ptRadius[i], s->ptRadius[i], s->ptRadius[i]);
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

static qboolean RagSane(const ragSim_t *s, const char **why)
{
    vec3_t mn, mx;
    int    i;

    ClearBounds(mn, mx);
    for (i = 0; i < RAG_PTS; i++) {
        if (Q_isnan(s->pt[i][0]) || Q_isnan(s->pt[i][1]) || Q_isnan(s->pt[i][2])) {
            *why = "nan";
            return qfalse;
        }
        if (fabs(s->pt[i][0]) > 65536 || fabs(s->pt[i][1]) > 65536 || fabs(s->pt[i][2]) > 65536) {
            *why = "offworld";
            return qfalse;
        }
        AddPointToBounds(s->pt[i], mn, mx);
    }
    // A human skeleton spans ~76u fully extended. Anything past 200u on an axis is a solver
    // blowup in progress, and catching it HERE - long before the 65536 test could - turns
    // "a body spassed and flew across the world" into a silent revert to the authored pose.
    for (i = 0; i < 3; i++) {
        if (mx[i] - mn[i] > 200.0f) {
            *why = "span";
            return qfalse;
        }
    }
    // PELVIS LEASH - the net that actually targets the reported symptom. Measured over the 41
    // bodies of the 2026-08-20 session: the seven fastest hit 1087-1611 u/s while spanning only
    // 38-58u. They were TRANSLATING coherently, not stretching, because the shape-match ties all
    // 14 points TO the pelvis and nothing anchors the pelvis. A span gate is blind to that.
    if (rag_leash->value > 0) {
        vec3_t dd;
        VectorSubtract(s->pt[0], cg_entities[s->entnum].lerpOrigin, dd);
        if (VectorLengthSquared(dd) > rag_leash->value * rag_leash->value) {
            *why = "leash";
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

// ---------- post-death impacts: shoot a corpse and its limbs move ----------------------------
//
// Called from the flesh-impact and explosion paths in cg_parsemsg.cpp. The server already sends
// a bone-accurate impact position and an inward normal for every flesh hit, to EVERYONE in PVS,
// so this needs no new networking and reacts to other players' shots as well as the local one.
//
// Impulse goes on ptPrev, never pt: in Verlet the gap between them IS the velocity, so moving
// ptPrev gives the point speed without teleporting it (moving pt would do the opposite - the
// exact bug that flung bodies across the map).
void CG_RagdollImpulse(const vec3_t pos, const vec3_t dir, float force, float radius, int limpMs)
{
    int   i, j;
    float subDt = RAG_SUBSTEP_MS * 0.001f;

    RagCvars();
    if (!cgi.R_SetRagdollPose || force <= 0 || radius <= 1.0f || !rag_impact->integer) {
        return;
    }
    // RE-ARM ON BEING SHOT. The sim pool is finite and RagAllocSlot evicts SLEEPING bodies to
    // make room for fresh kills, so after enough deaths an older corpse silently stops being
    // simulated - it still looks identical, but bullets have nothing to act on. Live
    // 2026-08-20: "eventually it does nothing when you shoot the limbs". A corpse near this
    // impact that is dead, drawn, and not currently simulated gets captured and armed right
    // here, so any body reacts no matter how long ago it died.
    if (cg.snap) { // NULL before the first rendered frame - a real deref, caught by the r13 audit
        int e;
        for (e = 0; e < cg.snap->numEntities; e++) {
            entityState_t *es = &cg.snap->entities[e];
            centity_t     *ce;
            ragSim_t      *ns;
            vec3_t         dd;
            if (es->eType != ET_MODELANIM || !(es->eFlags & EF_DEAD) || es->modelindex <= 0) {
                continue;
            }
            if (es->number < cgs.maxclients || RagSimFor(es->number) || s_ragNeverArm[es->number]) {
                continue;
            }
            ce = &cg_entities[es->number];
            VectorSubtract(ce->lerpOrigin, pos, dd);
            if (VectorLengthSquared(dd) > 96.0f * 96.0f) {
                continue;
            }
            ns = RagAllocSlot(es->number);
            if (!ns) {
                continue;
            }
            if (!RagCapture(ce, es, ns)) {
                memset(ns, 0, sizeof(*ns));
                continue;
            }
            ns->active    = qtrue;
            ns->entnum    = es->number;
            ns->state     = 1;
            ns->branch    = 1;
            ns->armTime   = cg.time;
            ns->gravScale = 1.0f; // already resting: no ramp needed
            ns->rampMs    = 400;
            ns->swingBone = -1;
            for (i = 0; i < RAG_PTS; i++) {
                VectorCopy(ns->pt[i], ns->goal[i]);
                VectorCopy(ns->pt[i], ns->goal0[i]);
            }
            if (rag_debug->integer) {
                cgi.Printf("^~^~^ RAGDOLL re-armed ent=%d (shot after eviction)\n", es->number);
            }
        }
    }
    for (i = 0; i < RAG_MAX_SIMS; i++) {
        ragSim_t *s = &s_ragSims[i];
        qboolean  hit = qfalse;
        if (!s->active || s->state < 1) {
            continue; // pendings and empty slots have no points to push
        }
        // BULLETS: find the BONE SEGMENT the round passed through, not the nearest joint. A shot
        // through the middle of a forearm is ~12u from both the elbow and the wrist, so a
        // point-distance falloff scored it 0.04 and the arm took 4% of the force and never went
        // limp - live 2026-08-20, "arms arent moving at all when shot". Hitting the segment and
        // splitting the force between its two ends is both what a bullet does and what makes the
        // limb rotate about its joint.
        if (dir && (dir[0] || dir[1] || dir[2])) {
            int   bestJ = -1, bestP = -1;
            float bestD = 99999.0f, bestT = 0;
            for (j = 1; j < RAG_PTS; j++) {
                vec3_t ab, ap, closest, diff;
                float  t, len2, dseg;
                int    p = s_ragBones[j].parent;
                VectorSubtract(s->pt[j], s->pt[p], ab);
                VectorSubtract(pos, s->pt[p], ap);
                len2 = DotProduct(ab, ab);
                t    = (len2 > 0.001f) ? DotProduct(ap, ab) / len2 : 0.0f;
                if (t < 0) {
                    t = 0;
                } else if (t > 1) {
                    t = 1;
                }
                VectorMA(s->pt[p], t, ab, closest);
                VectorSubtract(pos, closest, diff);
                dseg = VectorLength(diff);
                if (dseg < bestD) {
                    bestD = dseg;
                    bestJ = j;
                    bestP = p;
                    bestT = t;
                }
            }
            if (bestJ < 0 || bestD >= radius) {
                continue; // the round did not pass near any bone of this corpse
            }
            {
                float k2 = 1.0f - (bestD / radius); // linear: we are already ON the right bone
                int   ends[2];
                float w[2];
                int   e;
                // ASYMMETRIC, and this is the whole trick. Splitting the force between both ends
                // by where the round hit makes the bone TRANSLATE, and translation is invisible -
                // live 2026-08-20, limbs "dont really move at all" even with the truss switched
                // off entirely. A limb reads as moving only when it ROTATES about its joint, so
                // drive the DISTAL end (pt[bestJ] is always the child, i.e. further from the
                // pelvis) and leave the proximal end nearly planted to act as the pivot.
                // TORQUE COUPLE. Two POSITIVE pushes are still a net translation (0.975 of the
                // force), and the 50/50 parent link drags the proximal end after the distal one
                // inside a single solver iteration - so the bone slid and the renderer, which
                // aims a bone by the DIRECTION between its two points, saw almost no rotation.
                // Push the distal end and PULL the proximal end: a couple, which is pure torque.
                // Measured swing on a forearm: 10.1deg with two pushes, 21.1deg with the couple.
                float c = rag_couple->value;
                if (c < 0.0f) {
                    c = 0.0f;
                } else if (c > 1.2f) {
                    c = 1.2f;
                }
                {
                    float lin = rag_linear->value;
                    if (lin < 0.0f) {
                        lin = 0.0f;
                    } else if (lin > 1.0f) {
                        lin = 1.0f;
                    }
                    ends[0] = bestJ;
                    w[0]    = lin + c;
                    ends[1] = bestP;
                    w[1]    = lin - c;
                }
                for (e = 0; e < 2; e++) {
                    int    q = ends[e];
                    vec3_t vv;
                    float  vlen, vmax;
                    if (fabs(w[e]) < 0.05f) {
                        continue; // fabs: the proximal weight is NEGATIVE now (the couple)
                    }
                    VectorMA(s->ptPrev[q], -(force * k2 * w[e] * subDt), dir, s->ptPrev[q]);
                    // the ceiling must scale with the couple, or past c ~ 0.8 it rescales the
                    // TOTAL point velocity and sign-flips the counter-driven proximal end - a
                    // non-monotone cliff that reads live as "I turned it up and it stopped working"
                    vmax = force * 1.6f * (1.0f + c);
                    VectorSubtract(s->pt[q], s->ptPrev[q], vv);
                    vlen = VectorLength(vv) / subDt;
                    if (vlen > vmax && vlen > 0.001f) {
                        VectorScale(vv, (vmax / vlen) * subDt, vv);
                        VectorSubtract(s->pt[q], vv, s->ptPrev[q]);
                    }
                    s->limpMs[q]  = (short)limpMs;
                    s->limpMax[q] = (short)limpMs;
                }
                // ... and slacken everything BELOW the hit. If the forearm is struck but the hand
                // is still being reeled toward the authored pose at full strength, the hand
                // anchors the forearm and the swing dies. The limb has to go limp as a chain.
                for (j = 1; j < RAG_PTS; j++) {
                    int walk = j, depth;
                    for (depth = 0; depth < 6 && walk > 0; depth++) {
                        walk = s_ragBones[walk].parent;
                        if (walk == bestJ) {
                            s->limpMs[j]  = (short)limpMs;
                            s->limpMax[j] = (short)limpMs;
                            break;
                        }
                    }
                }
                // arm the swing instrument on the bone we just struck
                {
                    vec3_t d0;
                    int    dch = s_ragDriveChild[bestJ];
                    s->swingBone = bestJ;
                    if (dch >= 0) {
                        VectorSubtract(s->pt[dch], s->pt[bestJ], d0);
                    } else {
                        VectorSubtract(s->pt[bestJ], s->pt[bestP], d0);
                    }
                    if (VectorNormalize(d0) > 0.001f) {
                        VectorCopy(d0, s->swingDir0);
                        s->swingMax = 0;
                    } else {
                        s->swingBone = -1;
                    }
                }
                hit = qtrue;
            }
            if (hit) {
                goto ragImpulseWake;
            }
            continue;
        }
        for (j = 0; j < RAG_PTS; j++) {
            vec3_t d, n;
            float  dist, k;
            VectorSubtract(s->pt[j], pos, d);
            dist = VectorLength(d);
            if (dist >= radius) {
                continue;
            }
            // QUADRATIC falloff, not linear (live 2026-08-20: "when I shoot one leg they both
            // move... the whole body just kinda moves with it"). A soldier's hips are ~18u apart
            // and his shoulders ~24u, so a linear 30u sphere hands nearly full force to the far
            // limb and the pelvis, and the body translates instead of the limb swinging. Squaring
            // it makes the struck point dominate while neighbours still take enough to keep the
            // links near rest length - which is what turns the kick into rotation about a joint.
            k = 1.0f - (dist / radius);
            k = k * k;
            if (dir && (dir[0] || dir[1] || dir[2])) {
                VectorCopy(dir, n); // bullets: the inward normal the server already computed
            } else {
                VectorCopy(d, n); // explosions: outward from the blast, per point
                n[2] += radius * 0.35f;
                if (VectorNormalize(n) < 0.001f) {
                    VectorSet(n, 0, 0, 1);
                }
                // per-point variation, or every point gets nearly the same push (the body is
                // ~40u across inside a 180u blast) and the corpse translates as a rigid slab
                // instead of flailing - live 2026-08-20, "still kind of as a whole body"
                k *= 0.55f + 0.9f * ((j * 37) % 17) / 17.0f;
                n[0] += crandom() * 0.25f;
                n[1] += crandom() * 0.25f;
                n[2] += crandom() * 0.15f;
                VectorNormalize(n);
            }
            VectorMA(s->ptPrev[j], -(force * k * subDt), n, s->ptPrev[j]);
            // ACCUMULATION CEILING. Impulses add to whatever speed the point already had, so a
            // magazine emptied into a corpse accelerates it like a puck - live 2026-08-20, seven
            // rifle hits walked a body 128u from its own entity and tripped the leash. Clamp the
            // RESULT, not the impulse: one hit still lands at full strength, ten do not stack.
            {
                vec3_t vv;
                float  vlen, vmax = force * 1.6f;
                VectorSubtract(s->pt[j], s->ptPrev[j], vv);
                vlen = VectorLength(vv) / subDt; // u/s
                if (vlen > vmax && vlen > 0.001f) {
                    VectorScale(vv, (vmax / vlen) * subDt, vv);
                    VectorSubtract(s->pt[j], vv, s->ptPrev[j]);
                }
            }
            if (k > 0.15f) {
                s->limpMs[j]  = (short)limpMs; // only the limb that was actually hit goes limp:
                s->limpMax[j] = (short)limpMs; // limping the whole body makes it move as one lump
            }
            hit = qtrue;
        }
        if (!hit) {
            continue;
        }
    ragImpulseWake:
        if (s->state == 2) { // wake: same recipe as the mover-wake, including the fresh life
            s->state   = 1;  // budget - the 6s cap is per-wake, not a retirement, so a corpse
            s->sleepMs = 0;  // shot 30 seconds after death still reacts
            s->lifeMs  = 0;
            s->accumMs = 0;
        }
        s->rotLocked   = 0; // a struck body may re-orient; it re-latches once it rests again
        s->rotLockAtMs = -1;
        if (rag_debug->integer) {
            cgi.Printf("^~^~^ RAGDOLL impulse ent=%d force=%.0f radius=%.0f limp=%d\n", s->entnum,
                       force, radius, limpMs);
        }
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
                s->rotLocked = 0; // an elevator may carry a corpse AND re-orient it: without this
                                  // the latch outlives the wake and the body rides perfectly rigid
                s->rotLockAtMs = -1;
                memset(s->contact, 0, sizeof(s->contact));
                if (rag_debug->integer) {
                    cgi.Printf("^~^~^ RAGDOLL mover-wake ent=%d\n", s->entnum);
                }
            } else {
                continue;
            }
        }
        // RIDE THE SERVER'S CORPSE TOSS. A blast gives the corpse ENTITY real velocity (measured
        // 121-441u of travel), but our points are world-anchored and RagPush converts world->model
        // against the CURRENT placement while the renderer recomposes with that same placement -
        // the two cancel exactly, so the mesh renders where the SIM is, not where the entity went.
        // The body stayed behind, and the pelvis leash then measured the gap and permanently
        // retired the corpse. Carry the whole sim - including goal[], or the shape-match instantly
        // drags the body back to where the entity used to be.
        {
            centity_t *ce = &cg_entities[s->entnum];
            vec3_t     ed;
            if (!s->entOriginValid) {
                VectorCopy(ce->lerpOrigin, s->entOriginLast);
                s->entOriginValid = 1;
            }
            VectorSubtract(ce->lerpOrigin, s->entOriginLast, ed);
            if (VectorLengthSquared(ed) > 0.0001f) {
                if (VectorLengthSquared(ed) < 32.0f * 32.0f) { // a teleport is not a toss
                    int q;
                    for (q = 0; q < RAG_PTS; q++) {
                        VectorAdd(s->pt[q], ed, s->pt[q]);
                        VectorAdd(s->ptPrev[q], ed, s->ptPrev[q]);
                        VectorAdd(s->goal[q], ed, s->goal[q]);
                        VectorAdd(s->goal0[q], ed, s->goal0[q]);
                    }
                    if (s->state == 2) { // being thrown wakes it, same as a mover
                        s->state     = 1;
                        s->sleepMs   = 0;
                        s->lifeMs    = 0;
                        s->accumMs   = 0;
                        s->rotLocked = 0;
                    }
                }
                VectorCopy(ce->lerpOrigin, s->entOriginLast);
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
        // SPIN INSTRUMENT: nine rounds tuned a rotation nobody measured. drift= is provably blind
        // to it (goal[]==pt[] at capture, so a rigid rotation cancels exactly and a body standing
        // on its head still reads ~1). Measure the POINT CLOUD, and never via s->bodyRot - the
        // latch freezes that, so a metric reading it would print spin=0 for every latched body
        // and score its own fix a success.
        if (rag_debug->integer) {
            float rawNow[3][3];
            if (!RagRawFit(s, rawNow)) {
                s->rawBad++;
            } else if (s->lifeMs - s->rotSampleMs >= 500) {
                if (s->rotSampleMs > 0) {
                    float dR[3][3], tr3, ang, sn;
                    float dts = (s->lifeMs - s->rotSampleMs) * 0.001f;
                    RagMat3TransMul(s->rotSample, rawNow, dR); // sample -> now
                    tr3 = dR[0][0] + dR[1][1] + dR[2][2];
                    if (tr3 > 3.0f) {
                        tr3 = 3.0f;
                    }
                    if (tr3 < -1.0f) {
                        tr3 = -1.0f;
                    }
                    ang = (float)acos((tr3 - 1.0f) * 0.5f);
                    sn  = (float)sin(ang);
                    // yaw share, sign-free: a pure spin about world z reads 1.0, a pure topple 0.0
                    s->spinYawFrac = (sn > 0.0087f) ? (float)fabs(dR[0][1] - dR[1][0]) / (2.0f * sn) : 0.0f;
                    s->spinRate    = (dts > 0) ? ang * 180.0f / (float)M_PI / dts : 0.0f;
                    if (s->spinRate > s->spinMax) {
                        s->spinMax = s->spinRate;
                    }
                }
                memcpy(s->rotSample, rawNow, sizeof(rawNow));
                s->rotSampleMs = s->lifeMs;
            }
            if (s->swingBone >= 0) { // how far the struck bone has rotated since it was hit
                vec3_t dn;
                int    b = s->swingBone, dch = s_ragDriveChild[s->swingBone];
                if (dch >= 0) {
                    VectorSubtract(s->pt[dch], s->pt[b], dn);
                } else {
                    VectorSubtract(s->pt[b], s->pt[s_ragBones[b].parent], dn);
                }
                if (VectorNormalize(dn) > 0.001f) {
                    float dot = DotProduct(dn, s->swingDir0), ang;
                    if (dot > 1.0f) {
                        dot = 1.0f;
                    } else if (dot < -1.0f) {
                        dot = -1.0f;
                    }
                    ang = (float)acos(dot) * 180.0f / (float)M_PI;
                    if (ang > s->swingMax) {
                        s->swingMax  = ang;
                        s->swingLast = ang;
                    }
                }
            }
            { // stretch: RagShapeMatch runs LAST in RagStep and nothing re-enforces distance after
              // it, while per-point alphas differ across a link and pt[0] is never pulled at all
                int   k;
                float st;
                for (k = 1; k < RAG_PTS; k++) {
                    vec3_t dl;
                    if (s->restLen[k] < 0.01f) {
                        continue;
                    }
                    VectorSubtract(s->pt[k], s->pt[s_ragBones[k].parent], dl);
                    st = VectorLength(dl) / s->restLen[k];
                    if (st > s->stretchMax) {
                        s->stretchMax = st;
                    }
                }
            }
            {
                int k, nc = 0;
                for (k = 0; k < RAG_PTS; k++) {
                    if (s->contact[k]) {
                        nc++;
                    }
                }
                if (nc > s->ctcMax) {
                    s->ctcMax = (byte)nc; // contacts= has a ~50% duty cycle; the PEAK is the truth
                }
            }
        }
        {
            const char *why = "?";
            if (!RagSane(s, &why)) {
                if (rag_debug->integer) {
                    // WHICH test fired matters: live 2026-08-20 both blowups landed on the same
                    // death anim (unarmed_pain_kneestodeath), so the failing condition names the
                    // pose that breaks the solver
                    cgi.Printf("^~^~^ RAGDOLL blowup ent=%d reason=%s life=%dms stretch=%.2f "
                               "spinmax=%.1f - reverting to anim pose\n",
                               s->entnum, why, s->lifeMs, s->stretchMax, s->spinMax);
                }
                s_ragNeverArm[s->entnum] = 1;
                CG_RagdollClearEnt(s->entnum);
                continue;
            }
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
                        float rawNow[3][3], tr3, rotDeg = 0;
                        cgi.Printf("^~^~^ RAGDOLL sleep ent=%d life=%dms span=(%.0f %.0f %.0f) branch=%s "
                                   "drift=%.1f maxspd=%.0f contacts=%d alpha=%.2f drive=%d worldtr=%d\n",
                                   s->entnum, s->lifeMs, bmx[0] - bmn[0], bmx[1] - bmn[1], bmx[2] - bmn[2],
                                   s->branch ? "settle" : "free", drift, s->maxSpeed, nContacts,
                                   rag_stiff->value, rag_drive->integer, s_ragWorldTraces);
                        // total rotation since capture: valid because RagRawFit is exactly I there
                        if (RagRawFit(s, rawNow)) {
                            tr3 = rawNow[0][0] + rawNow[1][1] + rawNow[2][2];
                            if (tr3 > 3.0f) {
                                tr3 = 3.0f;
                            }
                            if (tr3 < -1.0f) {
                                tr3 = -1.0f;
                            }
                            rotDeg = (float)acos((tr3 - 1.0f) * 0.5f) * 180.0f / (float)M_PI;
                        }
                        cgi.Printf("^~^~^ RAGDOLL sleep-rot ent=%d rot=%.0fdeg spin=%.1f spinmax=%.1f "
                                   "yawf=%.2f rotlockAt=%d ctcmax=%d stretch=%.2f rawbad=%d "
                                   "swing=%.1fdeg couple=%.2f lim=%d limmax=%.0fdeg limsat=%d limoff=%d limbad=%d "
                                   "lock=%d slew=%.2f carry=%.2f vcap=%.0f\n",
                                   s->entnum, rotDeg, s->spinRate, s->spinMax, s->spinYawFrac,
                                   s->rotLockAtMs, (int)s->ctcMax, s->stretchMax, (int)s->rawBad,
                                   s->swingLast, rag_couple->value, s->limCount,
                                   s->limMax * 180.0f / (float)M_PI, s->limSat, s->limOff,
                                   s->limBad, rag_rotlock->integer,
                                   rag_slew->value, rag_carry->value, rag_velcap->value);
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

    // lifecycle first: revived, recycled, dropped from the snapshot, or PVS-exited.
    // NOT cent->interpolate: that guard exists for the FREE branch, which differences two
    // snapshots to seed velocity. The settle branch seeds nothing - it captures a body the
    // server has already parked - so requiring interpolation only discarded corpses. Live
    // 2026-08-20: EVERY dropped pending read interp=0, i.e. this one guard was rejecting
    // ~70% of all kills.
    if (!cent->currentValid || cs->modelindex <= 0 || cs->eType != ET_MODELANIM
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
        s->gravScale  = 0.0f;
        s->swingBone  = -1; // memset gives 0, which is a REAL bone index (the pelvis)
        s->freezePose = (rag_test->integer == 2); // the drill must be reachable on the branch
                                                  // that actually ships (it used to require mode 3)
        for (i = 0; i < RAG_PTS; i++) {
            VectorCopy(s->pt[i], s->goal[i]);  // the authored pose is the target silhouette
            VectorCopy(s->pt[i], s->goal0[i]); // ... and the anchor the stick rewrite is bounded to
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
            cgi.Printf("^~^~^ RAGDOLL settle-armed ent=%d channels=%d after=%dms via=solid anim=%s "
                       "buried=%d prelift=%d capspan=(%.0f %.0f %.0f)\n",
                       entnum, s->count, cg.time - armTime, nm ? nm : "?", (int)s->buried,
                       (int)s->preLifted, s->capSpan[0], s->capSpan[1], s->capSpan[2]);
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

    if (rag_mode->integer == 1 && rag_test->integer != 1) {
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
