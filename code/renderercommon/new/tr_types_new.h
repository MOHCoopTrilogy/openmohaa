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

#include "../../tiki/tiki_shared.h"

// HZM NOTE (bug-1172): this only affects renderergl1 (the shipping renderer - it's the only
// consumer of MAX_ENTITIES, see tr_scene.c/tr_model.cpp/tr_local.h). The "can't be increased"
// comment is stale (gl1's own sort-key packing already budgets 12 bits/4095 - tr_main.c
// QSORT_ENTITYNUM_SHIFT=8, entity field masked "& 4095" - the array was just never sized to
// match), and a busy multi-player coop scene could plausibly exceed 1023 simultaneous
// refEntities. Left at the stock 1023 deliberately: gl1 is the real, shipping build and this
// session's engine experimentation is gl2-sandbox-only per explicit instruction - revisit this
// raise only as its own deliberate, separately-approved gl1 change, not a side effect of gl2 work.
#define	MAX_ENTITIES	1023
#define MAX_POINTS     32
#define MAX_SPRITES    2048

#define MAX_MARK_FRAGMENTS   128
#define MAX_MARK_POLYVERTS   384

#define RDF_FULLBRIGHT		2		// fullbright intensity

typedef struct
{
    float pos[2];
    float size[2];
} letterloc_t;

typedef struct fontheader_sgl_s
{
    int               indirection[256];
    letterloc_t       locations[256];
    char              name[64];
    float             height;
    float             aspectRatio;
    void              *shader;
    int               trhandle; // the last renderer handle this font used
} fontheader_sgl_t;

typedef struct {
    unsigned short cp;
    unsigned char index;
    unsigned char loc;
} fontchartable_t;

typedef struct fontheader_s {
    int numPages;
    int charTableLength;
    fontchartable_t* charTable;
    short codePage;
    fontheader_sgl_t* sgl[32];
    char name[64];
} fontheader_t;

typedef enum {
    lensflare = (1 << 0),
    viewlensflare = (1 << 1),
    additive = (1 << 2)
} dlighttype_t;

#if !defined _WIN32

#define _3DFX_DRIVER_NAME	"libMesaVoodooGL.so.3.1"
#define OPENGL_DRIVER_NAME	"libGL.so"

#else

#define _3DFX_DRIVER_NAME	"3dfxvgl"
#define OPENGL_DRIVER_NAME	"opengl32"

#endif	// !defined _WIN32
