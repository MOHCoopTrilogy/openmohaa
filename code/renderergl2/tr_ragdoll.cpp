/*
HZM coop - RAGDOLL override table (ragdoll_plan.md v3 §2/§5, vetted PASS 2026-08-19).

The renderer-side half of the client ragdoll: cgame pushes MODEL-SPACE (skeletor-space,
unscaled) per-CHANNEL 3x4 matrices; Hook A (R_AddSkelSurfaces, tr_model.cpp) rewrites the
bone cache from these on EVERY fill, and Hook B (RE_TIKI_Orientation) serves tag lookups
from the same table so attachments ride the override.

Shape per the vetted plan: gore-style slotPlusOne index + small slot pool - never a dense
MAX_GENTITIES payload. Rows tolerate being pushed and never consumed. A row is IGNORED when
its stored tiki no longer matches the refEntity's (entity-slot reuse belt). All state
transitions live in cgame's push/clear - this file has no frame logic and no latches.
*/

#include "tr_local.h"
#include "tiki.h"
#include <vector.h>

#define RAGDOLL_MAX_SLOTS    8
#define RAGDOLL_MAX_CHANNELS 128

typedef struct ragdollSlot_s {
    qboolean active;
    int      entnum;
    dtiki_t *tiki;
    int      count;
    float    mat[RAGDOLL_MAX_CHANNELS][3][4]; // model-space, unscaled, per-channel
    float    animPose[RAGDOLL_MAX_CHANNELS][3][4]; // Hook A's copy of the vanilla frame (gore remap)
    qboolean animPoseValid;
    vec3_t   mins, maxs; // sim AABB (world), for the drift-cull work
} ragdollSlot_t;

static ragdollSlot_t s_ragSlots[RAGDOLL_MAX_SLOTS];
static byte          s_ragSlotPlusOne[MAX_GENTITIES];

ragdollSlot_t *R_RagdollSlotFor(int entityNumber, dtiki_t *tiki)
{
    int slot;
    if (entityNumber < 0 || entityNumber >= MAX_GENTITIES) {
        return NULL; // symbolic bounds check, gore precedent (plan C11)
    }
    slot = s_ragSlotPlusOne[entityNumber];
    if (!slot) {
        return NULL;
    }
    if (!s_ragSlots[slot - 1].active || (tiki && s_ragSlots[slot - 1].tiki != tiki)) {
        return NULL; // slot-reuse belt: stored tiki must match the entity being rendered
    }
    return &s_ragSlots[slot - 1];
}

void RE_SetRagdollPose(int entityNumber, dtiki_t *tiki, int count, const float *mat34, const vec3_t mins, const vec3_t maxs)
{
    ragdollSlot_t *s;
    int            i;

    if (entityNumber < 0 || entityNumber >= MAX_GENTITIES || !tiki) {
        return;
    }
    if (count <= 0 || count > RAGDOLL_MAX_CHANNELS) {
        return;
    }
    i = s_ragSlotPlusOne[entityNumber];
    if (i && s_ragSlots[i - 1].active && s_ragSlots[i - 1].entnum == entityNumber) {
        s = &s_ragSlots[i - 1];
    } else {
        s = NULL;
        for (i = 0; i < RAGDOLL_MAX_SLOTS; i++) {
            if (!s_ragSlots[i].active) {
                s = &s_ragSlots[i];
                s_ragSlotPlusOne[entityNumber] = (byte)(i + 1);
                break;
            }
        }
        if (!s) {
            return; // pool full: the death simply does not override (plan eviction lives cgame-side)
        }
    }
    s->active  = qtrue;
    s->entnum  = entityNumber;
    if (s->tiki != tiki) {
        s->animPoseValid = qfalse;
    }
    s->tiki    = tiki;
    s->count   = count;
    Com_Memcpy(s->mat, mat34, count * 12 * sizeof(float));
    VectorCopy(mins, s->mins);
    VectorCopy(maxs, s->maxs);
}

void RE_ClearRagdoll(int entityNumber)
{
    int slot;
    if (entityNumber < 0 || entityNumber >= MAX_GENTITIES) {
        return;
    }
    slot = s_ragSlotPlusOne[entityNumber];
    if (slot) {
        Com_Memset(&s_ragSlots[slot - 1], 0, sizeof(ragdollSlot_t));
        s_ragSlotPlusOne[entityNumber] = 0;
    }
}

void RE_ClearAllRagdolls(void)
{
    Com_Memset(s_ragSlots, 0, sizeof(s_ragSlots));
    Com_Memset(s_ragSlotPlusOne, 0, sizeof(s_ragSlotPlusOne));
}

// Hook A worker: rewrite the freshly-filled bone cache region from the override, and stash
// the vanilla frame as the anim-pose block. Channel layout: mat[ch] rows 0..2 = axis rows,
// translation in [0][3]/[1][3]/[2][3] (standard 3x4). Never writes past either count.
void R_RagdollApplyToCache(struct ragdollSlot_s *slot, skelBoneCache_t *cache, int num_tags, struct skelAnimFrame_s *newFrame)
{
    int i, n;

    if (!slot || !cache) {
        return;
    }
    // anim-pose stash first (one producer): vanilla GetFrame output, same 3x4 layout
    n = num_tags < RAGDOLL_MAX_CHANNELS ? num_tags : RAGDOLL_MAX_CHANNELS;
    if (newFrame) {
        for (i = 0; i < n; i++) {
            slot->animPose[i][0][0] = newFrame->bones[i][0][0];
            slot->animPose[i][0][1] = newFrame->bones[i][0][1];
            slot->animPose[i][0][2] = newFrame->bones[i][0][2];
            slot->animPose[i][0][3] = newFrame->bones[i][3][0];
            slot->animPose[i][1][0] = newFrame->bones[i][1][0];
            slot->animPose[i][1][1] = newFrame->bones[i][1][1];
            slot->animPose[i][1][2] = newFrame->bones[i][1][2];
            slot->animPose[i][1][3] = newFrame->bones[i][3][1];
            slot->animPose[i][2][0] = newFrame->bones[i][2][0];
            slot->animPose[i][2][1] = newFrame->bones[i][2][1];
            slot->animPose[i][2][2] = newFrame->bones[i][2][2];
            slot->animPose[i][2][3] = newFrame->bones[i][3][2];
        }
        slot->animPoseValid = qtrue;
    }
    n = n < slot->count ? n : slot->count;
    for (i = 0; i < n; i++) {
        cache[i].offset[0]    = slot->mat[i][0][3];
        cache[i].offset[1]    = slot->mat[i][1][3];
        cache[i].offset[2]    = slot->mat[i][2][3];
        cache[i].matrix[0][0] = slot->mat[i][0][0];
        cache[i].matrix[0][1] = slot->mat[i][0][1];
        cache[i].matrix[0][2] = slot->mat[i][0][2];
        cache[i].matrix[0][3] = 0;
        cache[i].matrix[1][0] = slot->mat[i][1][0];
        cache[i].matrix[1][1] = slot->mat[i][1][1];
        cache[i].matrix[1][2] = slot->mat[i][1][2];
        cache[i].matrix[1][3] = 0;
        cache[i].matrix[2][0] = slot->mat[i][2][0];
        cache[i].matrix[2][1] = slot->mat[i][2][1];
        cache[i].matrix[2][2] = slot->mat[i][2][2];
        cache[i].matrix[2][3] = 0;
    }
}

// Hook B worker: serve a tag orientation from the override (model-space bone * entity scale,
// matching TIKI_OrientationInternal's output contract for attachment composition).
qboolean R_RagdollGetOrientation(int entityNumber, dtiki_t *tiki, int tagnum, float scale, orientation_t *out)
{
    ragdollSlot_t *slot = R_RagdollSlotFor(entityNumber, tiki);
    int            r, c;

    if (!slot || tagnum < 0 || tagnum >= slot->count) {
        return qfalse;
    }
    if (scale <= 0.0f) {
        scale = 1.0f;
    }
    out->origin[0] = slot->mat[tagnum][0][3] * scale;
    out->origin[1] = slot->mat[tagnum][1][3] * scale;
    out->origin[2] = slot->mat[tagnum][2][3] * scale;
    for (r = 0; r < 3; r++) {
        for (c = 0; c < 3; c++) {
            out->axis[r][c] = slot->mat[tagnum][r][c];
        }
    }
    return qtrue;
}
