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

#ifdef __cplusplus
extern "C" {
#endif

// HZM (bug-1156): raised 20000 -> 131072 as the prerequisite for renderergl2 cascade sun shadows.
// TIKI_Reset_Caches() runs ONCE PER SCENE (tr_scene.c RE_BeginScene), not per view, while
// R_AddSkelSurfaces consumes from this pool for EVERY view that adds entities. Cascade shadow
// mapping adds 4 more views on top of the main one, so bone consumption goes up ~5x; overflow
// makes R_AddSkelSurfaces bail with "too many skeleton models visible" and the model SILENTLY
// VANISHES for that frame.
// A per-view reset is NOT a valid alternative: the backend resolves bones later through
// ent->e.bonestart, which indexes this pool, so recycling it between views would corrupt the
// views already queued. Raising the ceiling is the correct fix.
// Cost is trivial - skelBoneCache_t is 64 bytes (4+12 floats), so this is 1.28 MB -> 8.4 MB.
// Shared by both renderers; the exe and both renderer DLLs must be rebuilt together.
#define MAX_SKELBONES 131072

// HZM (bug-1189): raised 12800 -> 131072. skeletorMorphCache is the SIBLING of TIKI_Skel_Bones and
// is consumed by exactly the same producer (R_AddSkelSurfaces, once per entity per VIEW, reset once
// per SCENE by TIKI_Reset_Caches) - but when MAX_SKELBONES went 20000 -> 131072 for the gl2 cascade
// views this ceiling was left behind, and unlike the bone pool it has NO bounds check in either
// renderer:
//     if (num_tags + TIKI_Skel_Bones_Index > MAX_SKELBONES) { ...bail... }   <-- bone pool: guarded
//     added = SKEL_GetMorphWeightFrame(..., &skeletorMorphCache[skeletorMorphCacheIndex]);
//     ent->e.morphstart = skeletorMorphCacheIndex;                            <-- morph pool: NOT
// SKEL_GetMorphWeightFrame memsets numTargets ints at that index unconditionally and returns
// numTargets, so every character carrying a head skelmodel burns 40 ints PER VIEW (heads declare 40
// morph targets; body-only models like german_afrika_private declare 0 and never touch the pool).
// At the old ceiling the morph pool exhausted ~5.7x EARLIER than the bone pool
// (131072/72 bone-channels = ~1820 character-views, x40 morph ints = 72800 >> 12800), and the
// overrun is silent: the write runs off the end of the array, and every entity added afterwards
// gets a morphstart past the end, so RB_SkelMesh reads whatever follows in BSS as morph weights and
// applies them via VectorMA(out, weight, morph->offset, out) - i.e. head/face vertices flung to
// arbitrary positions with no log line. Sizing it to MAX_SKELBONES makes exhaustion impossible
// before the guarded bone pool bails (512 KB of BSS, same cost basis as the bone pool raise).
// See also the belt-and-braces bounds check added in renderergl1/tr_model.cpp R_AddSkelSurfaces;
// the identical guard is still owed to renderergl2/tr_model.cpp (~line 1252).
#define MAX_SKELMORPH 131072

    extern int             TIKI_Skel_Bones_Index;
    extern int             skeletorMorphCacheIndex;
    extern skelBoneCache_t TIKI_Skel_Bones[];
    extern int             skeletorMorphCache[];

    void TIKI_Reset_Caches();

#ifdef __cplusplus
}
#endif
