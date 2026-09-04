/*
===========================================================================
Copyright (C) 2024 the OpenMoHAA team

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

// tiki_cache.cpp : TIKI's fast implementation

#include "q_shared.h"
#include "qcommon.h"
#include "../skeletor/skeletor.h"
#include <mem_blockalloc.h>
#include <con_set.h>
#include "tiki_files.h"
#include "tiki_skel.h"

struct pchar {
    const char *m_Value;

    pchar() { m_Value = NULL; }

    pchar(const char *value) { m_Value = value; }

    friend bool operator==(const pchar& l, const pchar& r) { return !strcmp(l.m_Value, r.m_Value); }
};

con_map<pchar, dtikianim_t *> *tikianimcache;
con_map<pchar, dtiki_t *>     *tikicache;
static skeletor_c             *skel_entity_cache[TIKI_MAX_ENTITY_CACHE];

template<>
int HashCode<pchar>(const pchar& key)
{
    return HashCode<const char *>(key.m_Value);
}

/*
===============
TIKI_FindTikiAnim
===============
*/
dtikianim_t *TIKI_FindTikiAnim(const char *path)
{
    char filename[1024];

    if (tikianimcache) {
        dtikianim_t **t;

        t = tikianimcache->find(filename);
        if (t) {
            return *t;
        }
    }

    return NULL;
}

/*
===============
TIKI_FindTiki
===============
*/
dtiki_t *TIKI_FindTiki(const char *path)
{
    char filename[1024];

    if (tikicache) {
        dtiki_t **t;

        Q_strncpyz(filename, path, sizeof(filename));
        FS_CanonicalFilename(filename);

        t = tikicache->find(filename);
        if (t) {
            return *t;
        }
    }

    return NULL;
}

/*
===============
TIKI_RegisterTikiAnimFlags
===============
*/
dtikianim_t *TIKI_RegisterTikiAnimFlags(const char *path, qboolean use)
{
    dtikianim_t *tiki;
    char         filename[1024];

    Q_strncpyz(filename, path, sizeof(filename));
    FS_CanonicalFilename(filename);

    if (tikianimcache) {
        dtikianim_t **t;

        t = tikianimcache->find(filename);
        if (t) {
            return *t;
        }
    } else {
        tikianimcache = new con_map<pchar, dtikianim_t *>;
    }

    tiki = TIKI_LoadTikiAnim(filename);
    if (tiki) {
        if (use) {
            Com_DPrintf("^~^~^ Add the following line to the *_precache.scr map script:\n");
            Com_DPrintf("cache %s\n", filename);
        }

        (*tikianimcache)[tiki->name] = tiki;
    }

    return tiki;
}

/*
===============
TIKI_RegisterTikiAnim
===============
*/
dtikianim_t *TIKI_RegisterTikiAnim(const char *path)
{
    return TIKI_RegisterTikiAnimFlags(path, qfalse);
}

/*
===============
TIKI_RegisterTikiFlags
===============
*/
dtiki_t *TIKI_RegisterTikiFlags(const char *path, qboolean use)
{
    dtiki_t          *tiki     = NULL;
    dtikianim_t      *tikianim = NULL;
    con_map<str, str> keyValues;
    const char       *next_path;
    str               key;
    str               value;
    const char       *name;
    char              filename[1024];
    char              full_filename[1024];

    full_filename[0] = 0;

    name = path;
    for (next_path = strstr(name, "|"); next_path; next_path = strstr(name, "|")) {
        key                   = name;
        key[next_path - name] = 0;

        name      = next_path + 1;
        next_path = strstr(name, "|");
        if (!next_path) {
            break;
        }

        value                   = name;
        value[next_path - name] = 0;
        name                    = next_path + 1;

        // add it to the entry
        keyValues[key] = value;

        strcat(full_filename, key.c_str());
        strcat(full_filename, "|");
        strcat(full_filename, value.c_str());
        strcat(full_filename, "|");
    }

    Q_strncpyz(filename, name, sizeof(filename));
    FS_CanonicalFilename(filename);
    Q_strcat(full_filename, sizeof(full_filename), filename);

    if (!tikicache) {
        tikicache = new con_map<pchar, dtiki_t *>;
    } else {
        dtiki_t **t;

        t = tikicache->find(full_filename);
        if (t) {
            return *t;
        }
    }

    tikianim = TIKI_RegisterTikiAnimFlags(filename, use);
    if (tikianim) {
        tiki = TIKI_LoadTikiModel(tikianim, full_filename, &keyValues);
        if (tiki) {
            // cache the tiki
            (*tikicache)[tiki->name] = tiki;
        }
    }

    return tiki;
}

/*
===============
TIKI_RegisterTiki
===============
*/
dtiki_t *TIKI_RegisterTiki(const char *path)
{
    return TIKI_RegisterTikiFlags(path, qfalse);
}

/*
===============
TIKI_FreeAll
===============
*/
void TIKI_FreeAll()
{
    dtiki_t     **entry;
    dtikianim_t **entryanim;
    dtiki_t      *tiki;
    dtikianim_t  *tikianim;
    int           i;

    if (tikicache) {
        con_map_enum<pchar, dtiki_t *> en = *tikicache;

        for (entry = en.NextValue(); entry != NULL; entry = en.NextValue()) {
            skeletor_c *skeletor;

            tiki     = *entry;
            skeletor = (skeletor_c *)tiki->skeletor;
            if (skeletor) {
                delete skeletor;
            }

            tiki->m_boneList.CleanUpChannels();
            // Fixed in OPM
            //  It's better to free aliases when actually clearing the anim cache
            /*
            if (tiki->a->m_aliases) {
                TIKI_Free(tiki->a->m_aliases);
                tiki->a->m_aliases = NULL;
                tiki->a->num_anims = 0;
            }
            */

            TIKI_Free(tiki);
        }

        tikicache->clear();
    }

    if (tikianimcache) {
        con_map_enum<pchar, dtikianim_t *> en = *tikianimcache;

        for (entryanim = en.NextValue(); entryanim != NULL; entryanim = en.NextValue()) {
            tikianim = *entryanim;

            TIKI_RemoveTiki(tikianim);

            // Fixed in OPM
            //  Each tikianim should free their aliases
            if (tikianim->m_aliases) {
                TIKI_Free(tikianim->m_aliases);
                tikianim->m_aliases = NULL;
                tikianim->num_anims = 0;
            }

            TIKI_Free(tikianim);
        }

        tikianimcache->clear();
    }

    tiki_loading = true;

    for (i = 0; i < cache_maxskel; i++) {
        if (skelcache[i].skel) {
            TIKI_FreeSkel(i);
        }
    }
}

/*
===============
TIKI_GetSkeletor
===============
*/
static qboolean tiki_started;

void *TIKI_GetSkeletor(dtiki_t *tiki, int entnum)
{
    skeletor_c *skel;
    int         i;
    int         index;

    if (entnum == ENTITYNUM_NONE) {
        if (!tiki->skeletor) {
            tiki->skeletor = new skeletor_c(tiki);
        }
        skel = (skeletor_c *)tiki->skeletor;
    } else {
        // Added in 2.30
        //  Multiple caches per entity
        for (i = 0; i < TIKI_MAX_ENTITY_CACHE_PER_ENT; i++) {
            index = ((entnum % TIKI_MAX_ENTITIES) * TIKI_MAX_ENTITY_CACHE_PER_ENT) + i;

            skel = skel_entity_cache[index];
            if (!skel) {
                break;
            }

            if (skel->m_Tiki == tiki) {
                return skel;
            }
        }

        if (i == TIKI_MAX_ENTITY_CACHE_PER_ENT) {
            i = 0;
        }

        index = ((entnum % TIKI_MAX_ENTITIES) * TIKI_MAX_ENTITY_CACHE_PER_ENT) + i;
        skel  = skel_entity_cache[index];

        if (skel) {
            delete skel;
        }

        skel                     = new skeletor_c(tiki);
        skel_entity_cache[index] = skel;
    }

    return skel;
}

/*
===============
TIKI_DeleteSkeletor
===============
*/
static void TIKI_DeleteSkeletor(int entnum)
{
    skeletor_c *skel;
    int         i;
    int         index;

    if (entnum == ENTITYNUM_NONE) {
        return;
    }

    // HZM coop [user 2026-09-01, bug-2292] *** THIS IS THE GENTITYNUM_BITS 12 MAP-LOAD CRASH. ***
    //
    // TIKI_GetSkeletor wraps its index with `entnum % TIKI_MAX_ENTITIES` when it WRITES the cache.
    // This function, which frees it, did not - it indexed straight off entnum. The two agreed only
    // because TIKI_MAX_ENTITIES was hardcoded 2048 and MAX_GENTITIES was also 2048, so TIKI_End()'s
    // `for (i = 0; i < MAX_GENTITIES; i++)` topped out at index 2047*2+1 == 4095, the last slot of a
    // 4096-pointer array. Exactly in bounds, by coincidence, with zero margin.
    //
    // Raise GENTITYNUM_BITS to 12 and MAX_GENTITIES becomes 4096 while the cache stayed 2048 wide, so
    // the same loop ran to index 8191 - reading 4096 pointers of whatever follows the array and calling
    // `delete` on each one. Hence a 0xC0000005 inside skeletor_c::~skeletor_c during map load, with no
    // ERR_DROP and nothing in the console: bug-2283, which bug-2285 reverted with the cause unknown.
    //
    // FOUND BY THE FAULT ADDRESS, after two rounds of plausible-looking fixes to other things missed it.
    // WER logs the faulting RVA for every crash (Get-WinEvent -ProviderName 'Application Error'), the
    // linker already emits a .map, and the two together name the function in about a minute - no
    // debugger needed. That is the tool to reach for first on any silent AV, not inspection.
    //
    // Wrapped to match the writer, and the slot is cleared - it was left dangling before, which is a
    // double-free waiting for any second caller. TIKI_End is the only caller today and TIKI_Begin
    // zeroes the table afterwards, so that half was latent rather than live.
    for (i = 0; i < TIKI_MAX_ENTITY_CACHE_PER_ENT; i++) {
        index = ((entnum % TIKI_MAX_ENTITIES) * TIKI_MAX_ENTITY_CACHE_PER_ENT) + i;
        skel  = skel_entity_cache[index];
        if (skel) {
            delete skel;
            skel_entity_cache[index] = NULL;
        }
    }
}

/*
===============
TIKI_Begin
===============
*/
void TIKI_Begin(void)
{
    int i;

    for (i = 0; i < TIKI_MAX_ENTITY_CACHE; i++) {
        skel_entity_cache[i] = 0;
    }

    tiki_started = true;
}

/*
===============
TIKI_End
===============
*/
void TIKI_End(void)
{
    int i;

    // [bug-2292] the CACHE's width, not the entity pool's - see TIKI_DeleteSkeletor. These are
    // now the same number again, but they are different quantities and must not be conflated.
    for (i = 0; i < TIKI_MAX_ENTITIES; i++) {
        TIKI_DeleteSkeletor(i);
    }

    tiki_started = false;
}

/*
===============
TIKI_FinishLoad
===============
*/
void TIKI_FinishLoad(void)
{
    con_map_enum<pchar, dtikianim_t *> en;
    dtikianim_t                      **entry;

    if (!tiki_loading) {
        return;
    }

    tiki_loading = false;
    SkeletorCacheCleanCache();

    if (!low_anim_memory || !low_anim_memory->integer) {
        return;
    }

    if (tikianimcache) {
        en = *tikianimcache;
        for (entry = en.NextValue(); entry != NULL; entry = en.NextValue()) {
            TIKI_LoadAnim(*entry);
        }
    }
}

/*
===============
TIKI_FreeImages
===============
*/
void TIKI_FreeImages(void)
{
    dtikisurface_t                *dsurf;
    dtiki_t                       *tiki;
    int                            j, k;
    dtiki_t                      **entry;
    con_map_enum<pchar, dtiki_t *> en;

    if (!tikicache) {
        return;
    }

    en = *tikicache;
    for (entry = en.NextValue(); entry != NULL; entry = en.NextValue()) {
        tiki = *entry;
        for (k = 0; k < tiki->num_surfaces; k++) {
            dsurf = &tiki->surfaces[k];

            for (j = 0; j < dsurf->numskins; j++) {
                dsurf->hShader[j] = 0;
            }
        }
    }
}

/*
===============
TIKI_TikiAnimList_f
===============
*/
void TIKI_TikiAnimList_f(void)
{
    con_map_enum<pchar, dtikianim_t *> en;
    dtikianim_t                      **entry;

    Com_Printf("\ntikianimlist:\n");
    if (tikicache) {
        en = *tikianimcache;
        for (entry = en.NextValue(); entry != NULL; entry = en.NextValue()) {
            Com_Printf("%s\n", (*entry)->name);
        }
    }
}

/*
===============
TIKI_TikiList_f
===============
*/
void TIKI_TikiList_f(void)
{
    con_map_enum<pchar, dtiki_t *> en;
    dtiki_t                      **entry;

    Com_Printf("\ntikilist:\n");
    if (tikicache) {
        en = *tikicache;
        for (entry = en.NextValue(); entry != NULL; entry = en.NextValue()) {
            Com_Printf("%s\n", (*entry)->name);
        }
    }
}
