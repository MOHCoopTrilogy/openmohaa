/*
===========================================================================
HZM coop 2026-06-30 - skx_format.h

Self-contained legacy SKD/SKC on-disk struct definitions for su44's md5_2_skX.

su44's tool (2012) was written against an old OpenMoHAA qcommon/tiki_local.h that
named the on-disk model/anim structs skdHeader_t / skcHeader_t / etc. This fork
(OpenMoHAA 2023+ rewrite) renamed and split them into:

    code/skeletor/skeletor_model_file_format.h      (skelBaseHeader_t, boneFileData_t)
    code/skeletor/skeletor_animation_file_format.h  (skelAnimDataFileHeader_t, skelAnimFileFrame_t)
    code/tiki/tiki_shared.h                          (skelSurface_t, skeletorVertex_t, skelWeight_t, skeletorMorph_t)

The ON-DISK byte layout is UNCHANGED - the fork still loads these "OLD" formats:
    SKD version 5  (TIKI_SKD_HEADER_OLD_VERSION) - tiki_skel.cpp:893 accepts it
    SKC version 13 (TIKI_SKC_HEADER_OLD_VERSION) - tiki_files.cpp:559 accepts it

So rather than port the tool to the new API (which would change what it writes),
we re-declare su44's structs here with the SAME field order/sizes as the fork's
on-disk headers, keeping su44's original field NAMES so the tool's code is untouched.
Each struct below is annotated with the fork struct it mirrors. Sizes are asserted.

Depends on q_shared.h (vec2_t/vec3_t/byte) being included first (md5_2_skX.h does this).
===========================================================================
*/

#ifndef __SKX_FORMAT_H__
#define __SKX_FORMAT_H__

// -------- identifiers / versions (mirror tiki_shared.h) --------
#define SKD_IDENT             (*(int *)"SKMD")  // TIKI_SKD_HEADER_IDENT
#define SKD_VERSION           5                 // TIKI_SKD_HEADER_OLD_VERSION (no 'scale' field => v5 layout)
#define SKC_IDENT             (*(int *)"SKAN")  // TIKI_SKC_HEADER_IDENT
#define SKC_VERSION           13                // TIKI_SKC_HEADER_OLD_VERSION (unprocessed/old anim format)

// Per-surface ident. The engine copies it through without validating it
// (tiki_skel.cpp:313), so the exact value does not affect loading. "SKSF" is the
// conventional MoHAA surface magic.
#define SKD_SURFACE_IDENT     (*(int *)"SKSF")

// Channel-name stride. Fork: skeletor/skeletor_name_lists.h MAX_CHANNEL_NAME.
#define SKC_MAX_CHANNEL_CHARS 32

// Joint type written for every bone. The fork's loader requires jointType == 1
// (loadtiki.c readSKD also asserts this); 1 == SKELBONE_POSROT in the fork's
// boneType_e (skeletor_model_file_format.h). su44 emits a pos+rot channel pair.
#define JT_POSROT_SKC         1

// ============================ SKD (mesh) ============================

// mirrors skelBaseHeader_t (skeletor_model_file_format.h) - SKD v5 header, no 'scale'
typedef struct {
	int  ident;
	int  version;
	char name[64];
	int  numSurfaces;
	int  numBones;
	int  ofsBones;
	int  ofsSurfaces;
	int  ofsEnd;
	int  lodIndex[10];
	int  numBoxes;
	int  ofsBoxes;
	int  numMorphTargets;
	int  ofsMorphTargets;
} skdHeader_t;

// mirrors boneFileData_t (skeletor_model_file_format.h)
//   su44 field    -> fork field
//   jointType     -> boneType
//   ofsValues     -> ofsBaseData
//   ofsChannels   -> ofsChannelNames
//   ofsRefs       -> ofsBoneNames
typedef struct {
	char name[32];
	char parent[32];
	int  jointType;
	int  ofsValues;
	int  ofsChannels;
	int  ofsRefs;
	int  ofsEnd;
} skdBone_t;

// mirrors skelSurface_t (tiki_shared.h)
typedef struct {
	int  ident;
	char name[64];
	int  numTriangles;
	int  numVerts;
	int  staticSurfProcessed;
	int  ofsTriangles;
	int  ofsVerts;
	int  ofsCollapse;
	int  ofsEnd;
	int  ofsCollapseIndex;
} skdSurface_t;

// mirrors skeletorVertex_t (tiki_shared.h): normal, texCoords, numWeights, numMorphs
typedef struct {
	vec3_t normal;
	vec2_t texCoords;
	int    numWeights;
	int    numMorphs;
} skdVertex_t;

// mirrors skelWeight_t (tiki_shared.h): boneIndex, boneWeight, offset
typedef struct {
	int    boneIndex;
	float  boneWeight;
	vec3_t offset;
} skdWeight_t;

// mirrors skeletorMorph_t (tiki_shared.h): morphIndex, offset
typedef struct {
	int    morphIndex;
	vec3_t offset;
} skdMorph_t;

// on-disk SKD triangle = 3 ints (matches tool's tTri_t; loadtiki.c asserts equal size)
typedef struct {
	unsigned int indexes[3];
} skdTriangle_t;

// ============================ SKC (animation) ============================

// mirrors skelAnimDataFileHeader_t (skeletor_animation_file_format.h) WITHOUT the
// trailing flexible frame[1] member, so sizeof(skcHeader_t) == offsetof(..,frame) == 48.
// su44 writes the header then the frames immediately after it (loadtiki reads frames
// at h + sizeof(*h)), exactly matching the engine's frame[] base offset.
//   su44 field   -> fork field
//   ofsEnd       -> nBytesUsed
//   ofsChannels  -> ofsChannelNames
typedef struct {
	int    ident;
	int    version;
	int    flags;
	int    ofsEnd;
	float  frameTime;
	vec3_t totalDelta;
	float  totalAngleDelta;
	int    numChannels;
	int    ofsChannels;
	int    numFrames;
} skcHeader_t;

// mirrors skelAnimFileFrame_t (skeletor_animation_file_format.h)
//   su44 field  -> fork field
//   unknown     -> angleDelta
//   ofsValues   -> iOfsChannels
typedef struct {
	vec3_t bounds[2];
	float  radius;
	vec3_t delta;
	float  unknown;
	int    ofsValues;
} skcFrame_t;

// ---- compile-time layout guards (catch any accidental padding/reorder) ----
typedef char skx_assert_skdHeader [(sizeof(skdHeader_t)  == 148) ? 1 : -1];
typedef char skx_assert_skdBone   [(sizeof(skdBone_t)    ==  84) ? 1 : -1];
typedef char skx_assert_skdSurface[(sizeof(skdSurface_t) == 100) ? 1 : -1];
typedef char skx_assert_skdVertex [(sizeof(skdVertex_t)  ==  28) ? 1 : -1];
typedef char skx_assert_skdWeight [(sizeof(skdWeight_t)  ==  20) ? 1 : -1];
typedef char skx_assert_skdMorph  [(sizeof(skdMorph_t)   ==  16) ? 1 : -1];
typedef char skx_assert_skdTri    [(sizeof(skdTriangle_t)==  12) ? 1 : -1];
typedef char skx_assert_skcHeader [(sizeof(skcHeader_t)  ==  48) ? 1 : -1];
typedef char skx_assert_skcFrame  [(sizeof(skcFrame_t)   ==  48) ? 1 : -1];

#endif // __SKX_FORMAT_H__
