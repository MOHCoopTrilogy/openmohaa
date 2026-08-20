/*
HZM coop - RAGDOLL, Phase 0 (ragdoll_plan.md v3, vetted 3 rounds / 7 agents, PASS 2026-08-19).

This file is the system's permanent home. Phase 0 ships ONLY the P0-b instrumentation:
the §4 arm-guard shape detecting death edges, plus seed-quality logging - the velocity a
future sim would seed from snapshot origin differencing, logged for the first TWO post-edge
snapshots (the death impulse lands the same server frame and moves the origin only on
FOLLOWING snapshots - vet2 integration F8: differencing AT the edge seeds ~zero on
stationary kills, which is exactly what this probe must expose or clear).

P0 open question this probe answers: what s.clientNum do ACTORS carry vs player body-queue
Bodies? The plan's body-queue reject ("valid s.clientNum") needs the real discriminator -
so Phase 0 LOGS clientNum on every edge instead of guessing the reject predicate.

Everything is gated behind r_ragdollDebug (CVAR_TEMP - never archived, per plan §5 cvar
discipline / the T7 trap).
*/

#include "cg_local.h"

#define RAG_PROBE_MAX 16

typedef struct {
    qboolean active;
    int      entnum;
    int      snapsLogged;
    vec3_t   lastOrigin;
    int      lastServerTime;
} ragProbe_t;

static ragProbe_t s_ragProbe[RAG_PROBE_MAX];
static cvar_t    *rag_debug = NULL;

static ragProbe_t *RagProbeFor(int entnum, qboolean allocate)
{
    int i;
    for (i = 0; i < RAG_PROBE_MAX; i++) {
        if (s_ragProbe[i].active && s_ragProbe[i].entnum == entnum) {
            return &s_ragProbe[i];
        }
    }
    if (!allocate) {
        return NULL;
    }
    for (i = 0; i < RAG_PROBE_MAX; i++) {
        if (!s_ragProbe[i].active) {
            return &s_ragProbe[i];
        }
    }
    return NULL;
}

// Called from CG_TransitionEntity BEFORE currentState = nextState (both states visible).
void CG_RagdollTransition(centity_t *cent)
{
    entityState_t *cs = &cent->currentState;
    entityState_t *ns = &cent->nextState;
    ragProbe_t    *p;

    if (!rag_debug) {
        rag_debug = cgi.Cvar_Get("r_ragdollDebug", "0", CVAR_TEMP);
    }
    if (!rag_debug->integer) {
        return;
    }

    // already-tracked ent: log the post-edge snapshot deltas (the future velocity seed)
    p = RagProbeFor(ns->number, qfalse);
    if (p) {
        int   dt = cg.nextSnap ? (cg.nextSnap->serverTime - p->lastServerTime) : 0;
        float dts = (dt > 0) ? (dt * 0.001f) : 0.05f;
        vec3_t d;
        VectorSubtract(ns->origin, p->lastOrigin, d);
        cgi.Printf("^~^~^ RAGSEED ent=%d snap+%d dt=%dms delta=(%.1f %.1f %.1f) vel=(%.0f %.0f %.0f) speed=%.0f\n",
                   ns->number, p->snapsLogged + 1, dt, d[0], d[1], d[2],
                   d[0] / dts, d[1] / dts, d[2] / dts, VectorLength(d) / dts);
        VectorCopy(ns->origin, p->lastOrigin);
        p->lastServerTime = cg.nextSnap ? cg.nextSnap->serverTime : p->lastServerTime;
        p->snapsLogged++;
        if (p->snapsLogged >= 3) {
            p->active = qfalse;
        }
        return;
    }

    // plan §4 arm-guard shape (Phase 0: LOG eligibility, arm nothing)
    if (cs->eType != ET_MODELANIM || ns->eType != ET_MODELANIM) {
        return; // BOTH states - an eType change does not clear interpolate (vet3/F6)
    }
    if ((cs->eFlags & EF_DEAD) || !(ns->eFlags & EF_DEAD)) {
        return; // rising edge only; first-seen-dead never arms (plan C10)
    }
    if (!cent->interpolate) {
        return; // continuity test (vet2/F6)
    }
    if (ns->number < cgs.maxclients) {
        return; // never a live player slot
    }

    p = RagProbeFor(ns->number, qtrue);
    if (!p) {
        return;
    }
    p->active         = qtrue;
    p->entnum         = ns->number;
    p->snapsLogged    = 0;
    VectorCopy(ns->origin, p->lastOrigin);
    p->lastServerTime = cg.nextSnap ? cg.nextSnap->serverTime : cg.time;
    // clientNum logged raw: Phase 0 data decides the body-queue reject predicate
    cgi.Printf("^~^~^ RAGSEED edge ent=%d clientNum=%d modelindex=%d origin=(%.0f %.0f %.0f)\n",
               ns->number, ns->clientNum, ns->modelindex, ns->origin[0], ns->origin[1], ns->origin[2]);
}
