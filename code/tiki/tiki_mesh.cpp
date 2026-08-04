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

// tiki_mesh.cpp : Mesh handling

#include "tiki.h"

int             TIKI_Skel_Bones_Index;
int             skeletorMorphCacheIndex;
skelBoneCache_t TIKI_Skel_Bones[MAX_SKELBONES];
int             skeletorMorphCache[MAX_SKELMORPH];

// ^~^~^ SKELPOOL - per-scene high-water report for the two skeletal frontend pools.
//
// Both pools are filled by R_AddSkelSurfaces once per entity PER VIEW and reset here, once per
// SCENE (RE_BeginScene). gl2 adds sun-cascade + depth pre-pass views on top of the main view, so
// consumption scales with view count - that is why MAX_SKELBONES had to go 20000 -> 131072.
// This prints ONLY when a new peak is reached, so a session emits a handful of lines at most and
// then goes silent. No cvar, because the whole point is that it must be present in the log of a
// session the user did not know they would need to diagnose.
//
// Reading it: bones/BONECAP and morph/MORPHCAP are the peak fill of each pool for one scene.
// If morph ever approaches MORPHCAP, characters added late in the frame are skinning from morph
// weights read past the end of the array (silent OOB) - which deforms exactly the meshes that carry
// morph targets, i.e. HEADS. If both stay far below their caps, pool exhaustion is ruled out and
// the skeletal-corruption search must move elsewhere.
void TIKI_Reset_Caches()
{
    static int peakBones = 0;
    static int peakMorph = 0;

    if (TIKI_Skel_Bones_Index > peakBones || skeletorMorphCacheIndex > peakMorph) {
        if (TIKI_Skel_Bones_Index > peakBones) {
            peakBones = TIKI_Skel_Bones_Index;
        }
        if (skeletorMorphCacheIndex > peakMorph) {
            peakMorph = skeletorMorphCacheIndex;
        }

        Com_Printf(
            "^~^~^ SKELPOOL peak bones=%d/%d (%d%%)  morph=%d/%d (%d%%)\n",
            peakBones,
            MAX_SKELBONES,
            (int)(((float)peakBones / (float)MAX_SKELBONES) * 100.0f),
            peakMorph,
            MAX_SKELMORPH,
            (int)(((float)peakMorph / (float)MAX_SKELMORPH) * 100.0f)
        );
    }

    TIKI_Skel_Bones_Index   = 0;
    skeletorMorphCacheIndex = 0;
}
