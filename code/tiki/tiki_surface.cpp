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

// tiki_surface.cpp : TIKI surface

#include "q_shared.h"
#include "qcommon.h"
#include <tiki.h>

/*
===============
TIKI_Surface_NameToNum
===============
*/
int TIKI_Surface_NameToNum(dtiki_t *pmdl, const char *name)
{
    int             i;
    dtikisurface_t *psurface;

    for (i = 0; i < pmdl->num_surfaces; i++) {
        psurface = &pmdl->surfaces[i];

        if (!stricmp(psurface->name, name)) {
            return i;
        }
    }

    return -1;
}

/*
===============
TIKI_Surface_NumToName
===============
*/
// HZM coop [2026-08-21] how many shader variants this surface actually has. The gore system
// writes a skin-offset tier across every surface, and a surface with fewer variants than the tier
// selects a shader that does not exist and renders as nothing. Returns 0 on a bad index so a caller
// clamping against it degrades to "no tier" rather than to garbage.
int TIKI_Surface_NumSkins(dtiki_t *pmdl, int num)
{
    if (!pmdl || num < 0 || num >= pmdl->num_surfaces) {
        return 0;
    }
    return pmdl->surfaces[num].numskins;
}

const char *TIKI_Surface_NumToName(dtiki_t *pmdl, int num)
{
    if (num < 0 || num >= pmdl->num_surfaces) {
        TIKI_Error("TIKI_Surface_NumToName: Surface %d out of range for %s.\n", num, pmdl->a->name);
        return NULL;
    }

    assert(pmdl->surfaces[num].name[0]);
    return pmdl->surfaces[num].name;
}
