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

#pragma once

#include "q_shared.h"

#include "../tiki/tiki_shared.h"

#ifdef __cplusplus
class Archiver;
#    include "../qcommon/mem_blockalloc.h"
#    include "../qcommon/con_set.h"
#    include "../qcommon/str.h"
#endif

#define MAX_TIKI_LOAD_ANIMS                 8192 // HZM: raised from 4095; new_generic_human.tik exceeds 4095 after force-load pack additions (Issue #12)
#define MAX_TIKI_LOAD_SKEL_INDEX            12
#define MAX_TIKI_LOAD_SERVER_INIT_COMMANDS  160
#define MAX_TIKI_LOAD_CLIENT_INIT_COMMANDS  180 // 2.30: Increased from 160 to 180
#define MAX_TIKI_LOAD_HEADMODELS_LENGTH     4096
#define MAX_TIKI_LOAD_HEADSKINS_LENGTH      4096
#define MAX_TIKI_LOAD_MODEL_BUFFER          8192

#define MAX_TIKI_LOAD_FRAME_SERVER_COMMANDS 32
// [user 08-08] bug-1596 - raised 128 -> 256. models/emitters/mortar_snownodamage.tik (the snow
// mortar effect on t2l3) defines ~133 client commands in one frame block; the parser dropped the
// tail SILENTLY apart from a log line, so the effect ran without its last steps (avelocity,
// scalemin/scalemax, fadedelay).
//
// NOTE THE PAIRING: this is the LOAD-time cap on how many commands one frame block may DEFINE.
// TIKI_MAX_COMMANDS (tiki/tiki_shared.h) is the separate RUNTIME cap on how many may FIRE in a
// single frame, and these commands all sit in one parenthesised group, so they fire together.
// Raising only this one would have moved the overflow from parse time to run time rather than
// fixing it - they are raised together, and TIKI_MAX_COMMANDS carries the reciprocal note.
#define MAX_TIKI_LOAD_FRAME_CLIENT_COMMANDS 256

// HZM coop [user 2026-08-23, bug-2082] MUST TRACK MAX_TIKI_SHADER. There are TWO shader-array
// constants and raising only the runtime one (tiki/tiki_shared.h) corrupts memory: the parser
// guard at tiki_parse.cpp is written against MAX_TIKI_SHADER, so with that at 8 and this at 4
// it strncpy's a 64-byte shader name into shader[4..7] of a 4-wide array - straight over
// numskins, flags and damage_multiplier in the same struct. The corrupted numskins is then the
// loop bound in TIKI_SetupIndividualSurface, and TIKI_Error only Com_Printf's rather than
// aborting, so the game hangs printing the same line ~2 billion times. That is exactly what
// happened loading Omaha with the glove content in: 800+ identical lines and a freeze.
#define MAX_TIKI_LOAD_SHADERS               8

// HZM: Max distinct surfaces parsed from a model's setup/$case blocks into the
// stack array loadsurfaces[] in TIKI_LoadTikiModel. Was a bare hardcoded 24 with
// NO bounds check in the SETUP_SURFACE parse case (tiki_parse.cpp::TIKI_LoadSetupCase),
// so a model whose matched setup defines >24 surfaces overran the stack array ->
// /GS stack-canary trip (0xc0000409). Which $case branches match (and thus how many
// surfaces accumulate) depends on the entity's spawn key/values, which is why the
// overrun only manifested on certain registrations (e.g. heavy m1l3c models on an
// in-game transition, where the registering key|value| string differs from a fresh
// load). Raised headroom + added an explicit guard. See tiki_parse.cpp.
#define MAX_TIKI_LOAD_SURFACES              48

typedef struct AliasList_s     AliasList_t;
typedef struct AliasListNode_s AliasListNode_t;
typedef struct msg_s           msg_t;

typedef struct {
    int indexes[3];
} tikiTriangle_t;

typedef struct {
    vec2_t st;
} tikiSt_t;

typedef struct {
    short unsigned int xyz[3];
    short int          normal;
} tikiXyzNormal_t;

typedef struct {
    vec3_t origin;
    vec3_t axis[3];
} tikiTagData_t;

typedef struct {
    char name[64];
} tikiTag_t;

typedef struct {
    qboolean valid;
    int      surface;
    vec3_t   position;
    vec3_t   normal;
    float    damage_multiplier;
} tikimdl_intersection_t;

typedef struct {
    int indexes[3];
} skelTriangle_t;

typedef struct dtikicmd_s {
    int    frame_num;
    int    num_args;
    char **args;
} dtikicmd_t;

typedef struct {
    int    frame_num;
    int    num_args;
    char **args;
    char   location[MAX_QPATH];
} dloadframecmd_t;

typedef struct {
    int    num_args;
    char **args;
} dloadinitcmd_t;

typedef struct {
    char  name[MAX_NAME_LENGTH];
    char  shader[MAX_TIKI_LOAD_SHADERS][MAX_RES_NAME];
    int   numskins;
    int   flags;
    float damage_multiplier;
} dloadsurface_t;

typedef struct {
    char            *alias;
    char             name[MAX_QPATH];
    char             location[MAX_QPATH];
    float            weight;
    float            blendtime;
    int              flags;
    int              num_client_cmds;
    int              num_server_cmds;
    dloadframecmd_t *loadservercmds[MAX_TIKI_LOAD_FRAME_SERVER_COMMANDS];
    dloadframecmd_t *loadclientcmds[MAX_TIKI_LOAD_FRAME_CLIENT_COMMANDS];
} dloadanim_t;

typedef struct dloaddef_s dloaddef_t;

#include "tiki_script.h"

#ifdef __cplusplus

typedef struct dloaddef_s {
    const char      *path;
    class TikiScript tikiFile;

    dloadanim_t    *loadanims[MAX_TIKI_LOAD_ANIMS];
    dloadinitcmd_t *loadserverinitcmds[MAX_TIKI_LOAD_SERVER_INIT_COMMANDS];
    dloadinitcmd_t *loadclientinitcmds[MAX_TIKI_LOAD_CLIENT_INIT_COMMANDS];

    int skelIndex_ld[MAX_TIKI_LOAD_SKEL_INDEX];
    int numanims;
    int numserverinitcmds;
    int numclientinitcmds;

    char     headmodels[MAX_TIKI_LOAD_HEADMODELS_LENGTH];
    char     headskins[MAX_TIKI_LOAD_HEADSKINS_LENGTH];
    qboolean bIsCharacter;

    struct msg_s *modelBuf;
    unsigned char modelData[MAX_TIKI_LOAD_MODEL_BUFFER];

    qboolean bInIncludesSection;

    // Added in 2.0
    //====
    char     idleSkel[MAX_QPATH];
    int      numskels;
    qboolean hasSkel;
    //====
} dloaddef_t;

#endif

#include "../skeletor/skeletor.h"

#include "../qcommon/tiki_main.h"
#include "../tiki/tiki_anim.h"
#include "../tiki/tiki_cache.h"
#include "../tiki/tiki_commands.h"
#include "../tiki/tiki_files.h"
#include "../tiki/tiki_imports.h"
#include "../tiki/tiki_parse.h"
#include "../tiki/tiki_skel.h"
#include "../tiki/tiki_tag.h"
#include "../tiki/tiki_utility.h"
#include "../tiki/tiki_frame.h"
#include "../tiki/tiki_surface.h"
#include "../tiki/tiki_mesh.h"
