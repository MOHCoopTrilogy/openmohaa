/*
===========================================================================
Copyright (C) 2023 the OpenMoHAA team

This file is part of OpenMoHAA source code.

OpenMoHAA source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

OpenMoHAA source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with OpenMoHAA source code; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/

// tiki_tag.cpp : TIKI Tag

#include "q_shared.h"
#include "qcommon.h"
#include "../skeletor/skeletor.h"
#include "tiki_cache.h"
#include "tiki_tag.h"

/*
===============
TIKI_Tag_NameToNum
===============
*/
int TIKI_Tag_NameToNum(dtiki_t *pmdl, const char *name)
{
    return pmdl->GetBoneNumFromName(name);
}

/*
===============
TIKI_Tag_NumToName
===============
*/
const char *TIKI_Tag_NumToName(dtiki_t *pmdl, int iTagNum)
{
    return pmdl->GetBoneNameFromNum(iTagNum);
}

/*
===============
TIKI_TransformInternal
===============
*/
SkelMat4 *TIKI_TransformInternal(dtiki_t *tiki, int entnum, int tagnum)
{
    skeletor_c *skeletor;

    if (tagnum < 0 || !tiki || tagnum >= tiki->m_boneList.NumChannels()) {
        return NULL;
    }

    skeletor = (skeletor_c *)TIKI_GetSkeletor(tiki, entnum);
    if (!skeletor) {
        return NULL;
    }

    return &skeletor->GetBoneFrame(tagnum);
}

/*
===============
TIKI_IsOnGroundInternal
===============
*/
qboolean TIKI_IsOnGroundInternal(dtiki_t *tiki, int entnum, int tagnum, float threshold)
{
    skeletor_c *skeletor;

    if (tagnum < 0 || !tiki || tagnum >= tiki->m_boneList.NumChannels()) {
        return qfalse;
    }

    skeletor = (skeletor_c *)TIKI_GetSkeletor(tiki, entnum);
    return skeletor->IsBoneOnGround(tagnum, threshold);
}

/*
===============
TIKI_OrientationInternal
===============
*/
orientation_t TIKI_OrientationInternal(dtiki_t *tiki, int entnum, int tagnum, float scale)
{
    if (tagnum < 0 || !tiki || tagnum >= tiki->m_boneList.NumChannels()) {
        return orientation_t {};
    }

    const skeletor_c *skeletor   = (skeletor_c *)TIKI_GetSkeletor(tiki, entnum);
    if (!skeletor) {
        return orientation_t {};
    }

    const SkelMat4  & pTransform = skeletor->GetBoneFrame(tagnum);

    orientation_t orient;
    orient.origin[0] = (pTransform.val[3][0] + tiki->load_origin[0]) * (scale * tiki->load_scale);
    orient.origin[1] = (pTransform.val[3][1] + tiki->load_origin[1]) * (scale * tiki->load_scale);
    orient.origin[2] = (pTransform.val[3][2] + tiki->load_origin[2]) * (scale * tiki->load_scale);
    memcpy(orient.axis, pTransform.val, sizeof(orient.axis));

    return orient;
}

/*
===============
TIKI_SetPoseInternal
===============
*/
void TIKI_SetPoseInternal(
    void *skeletor, const frameInfo_t *frameInfo, const int *bone_tag, const vec4_t *bone_quat, float actionWeight,
    int numControllers
)
{
    skeletor_c *skel = (skeletor_c *)skeletor;
    skel->SetPose(frameInfo, bone_tag, bone_quat, actionWeight, numControllers);
}

/*
===============
TIKI_GetRadiusInternal
===============
*/
float TIKI_GetRadiusInternal(dtiki_t *tiki, int entnum, float scale)
{
    skeletor_c *skeletor = (skeletor_c *)TIKI_GetSkeletor(tiki, entnum);
    return skeletor->GetRadius() * tiki->load_scale * scale;
}

/*
===============
TIKI_GetCentroidRadiusInternal
===============
*/
float TIKI_GetCentroidRadiusInternal(dtiki_t *tiki, int entnum, float scale, float *centroid)
{
    skeletor_c *skeletor = (skeletor_c *)TIKI_GetSkeletor(tiki, entnum);
    return skeletor->GetCentroidRadius(centroid) * tiki->load_scale * scale;
}

// ============================================================================
// ^~^~^ POSECHK - one-shot per-model skeletal pose audit.
//
// WHY THIS EXISTS: when a character renders CONTORTED (limbs stretched into spikes, faces
// flattened) there are exactly two possible halves of the pipeline at fault, and no way to tell
// them apart by looking at the screen:
//
//   (a) the POSE is already wrong  - skeletor produced garbage bone matrices (bad anim data, bad
//       bone merge, bad bone controller, stale/shared skeletor cache), or
//   (b) the POSE is fine and the SKINNING is wrong - the renderer maps vertices onto the wrong
//       bone index, reads the wrong slice of the bone/morph pools, or collapses LOD badly.
//
// TIKI_GetFrameInternal is the single funnel every skeletal model's final pose passes through, in
// BOTH renderers, and it lives in the exe - so this audit sees case (a) exactly and never sees
// case (b). One playtest with it armed therefore decides which half to search, and if it is (a) it
// also names the offending bones.
//
// Arm with:  tiki_posecheck 1   (CVAR_TEMP, not CVAR_CHEAT - a listen server runs sv_cheats 0 and
// would clamp a cheat cvar straight back to 0; see bug-1148.) Default 0 = zero cost.
//
// Output, at most one line per model handle per arming, capped at TIKI_POSECHK_MAXLINES:
//   ^~^~^ POSECHK model=<tik> ent=<n> bones=<n> bad=<n> [worst=<idx> '<bone>' t=[..] rowlen=[..]]
// bad=0 on a model the user can SEE contorted  => the pose is correct, the defect is in skinning.
// bad>0                                        => the pose is corrupt; the named bones are the lead.
//
// KNOWN BENIGN HIT: models/human/2nd-ranger_private.tik carries a bone called "grenade" that comes
// from models/gear/ranger_2grenades.skd (an HRRTM mesh). That skd declares TWO bones whose names
// collide case-insensitively ("origin" at index 0, parent worldbone, and "ORIGIN" at index 4,
// parent Bip01 Footsteps) - it is the only skd out of 1427 in the whole game data that does this.
// The engine dedupes bone names with stricmp, so the second definition is discarded and "grenade"
// ends up parented to the BODY's ORIGIN rather than its own root. Nothing is skinned to "grenade",
// so it cannot deform anything, but it may sit far from the body and trip the translation test.
// A report naming ONLY "grenade" therefore means the pose is CLEAN.
// ============================================================================
#define TIKI_POSECHK_MAXLINES 96

static cvar_t *tiki_posecheck = NULL;

static void TIKI_PoseCheck(dtiki_t *tiki, int entnum, skelAnimFrame_t *newFrame)
{
    static const dtiki_t *seen[TIKI_POSECHK_MAXLINES];
    static int            numSeen = 0;
    static int            armedAt = -1;

    int   numBones;
    int   i, r;
    int   bad     = 0;
    int   worst   = -1;
    float worstMag = 0.0f;
    float rowlen[3];

    if (!tiki_posecheck) {
        tiki_posecheck = Cvar_Get("tiki_posecheck", "0", CVAR_TEMP);
    }
    if (!tiki_posecheck->integer || !tiki || !tiki->a || !newFrame) {
        return;
    }

    // re-arm (clear the one-shot table) whenever the cvar value changes
    if (tiki_posecheck->integer != armedAt) {
        armedAt = tiki_posecheck->integer;
        numSeen = 0;
    }

    for (i = 0; i < numSeen; i++) {
        if (seen[i] == tiki) {
            return;
        }
    }
    if (numSeen >= TIKI_POSECHK_MAXLINES) {
        return;
    }
    seen[numSeen++] = tiki;

    numBones = tiki->m_boneList.NumChannels();

    for (i = 0; i < numBones; i++) {
        const SkelMat4& m = newFrame->bones[i];
        float           t[3];
        float           mag;
        int             thisBad = 0;

        t[0] = m[3][0];
        t[1] = m[3][1];
        t[2] = m[3][2];

        for (r = 0; r < 3; r++) {
            rowlen[r] = (float)sqrt(m[r][0] * m[r][0] + m[r][1] * m[r][1] + m[r][2] * m[r][2]);
            // NaN-safe: a comparison against NaN is false both ways, so test the negation.
            if (!(rowlen[r] > 0.02f && rowlen[r] < 50.0f)) {
                thisBad = 1;
            }
        }

        mag = (float)sqrt(t[0] * t[0] + t[1] * t[1] + t[2] * t[2]);
        if (!(mag < 4096.0f)) {
            thisBad = 1;
        }

        if (thisBad) {
            bad++;
            if (worst < 0 || mag > worstMag) {
                worst    = i;
                worstMag = mag;
            }
        }
    }

    if (bad && worst >= 0) {
        const SkelMat4& m    = newFrame->bones[worst];
        const char     *name = tiki->GetBoneNameFromNum(worst);

        for (r = 0; r < 3; r++) {
            rowlen[r] = (float)sqrt(m[r][0] * m[r][0] + m[r][1] * m[r][1] + m[r][2] * m[r][2]);
        }

        Com_Printf(
            "^~^~^ POSECHK model=%s ent=%d bones=%d bad=%d worst=%d '%s' t=[%.1f %.1f %.1f] "
            "rowlen=[%.3f %.3f %.3f]\n",
            tiki->a->name,
            entnum,
            numBones,
            bad,
            worst,
            name ? name : "?",
            m[3][0],
            m[3][1],
            m[3][2],
            rowlen[0],
            rowlen[1],
            rowlen[2]
        );
    } else {
        Com_Printf("^~^~^ POSECHK model=%s ent=%d bones=%d bad=0\n", tiki->a->name, entnum, numBones);
    }
}

/*
===============
TIKI_GetFrameInternal
===============
*/
void TIKI_GetFrameInternal(dtiki_t *tiki, int entnum, skelAnimFrame_t *newFrame)
{
    skeletor_c *skeletor = (skeletor_c *)TIKI_GetSkeletor(tiki, entnum);
    skeletor->GetFrame(newFrame);
    TIKI_PoseCheck(tiki, entnum, newFrame);
}

/*
===============
TIKI_SetEyeTargetPos
===============
*/
void TIKI_SetEyeTargetPos(dtiki_t *tiki, int entnum, vec3_t pos)
{
    skeletor_c *skeletor = (skeletor_c *)TIKI_GetSkeletor(tiki, entnum);
    skeletor->SetEyeTargetPos(pos);
}
