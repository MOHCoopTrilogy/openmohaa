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

// DESCRIPTION:
// FPS view model animation

#include "cg_local.h"
#include "tiki.h"

static const char *AnimPrefixList[] = {
    "",
    "unarmed",
    "papers",
    "colt45",
    "p38",
    "histandard",
    "garand",
    "kar98",
    "kar98sniper",
    "springfield",
    "thompson",
    "mp40",
    "bar",
    "mp44",
    "fraggrenade",
    "stielhandgranate",
    "bazooka",
    "panzerschreck",
    "shotgun",
    //
    // Team Assault and Team Tactics weapons
    "mg42portable",
    "webley",
    "nagantrev",
    "beretta",
    "enfield",
    "svt",
    "mosin",
    "g43",
    "enfieldl42a1",
    "carcano",
    "delisle",
    "sten",
    "ppsh",
    "moschetto",
    "fg42",
    "vickers",
    "breda",
    "f1grenade",
    "millsgrenade",
    "nebelhandgranate",
    "m18smokegrenade",
    "rdg1smokegrenade",
    "bomba",
    "bombabreda",
    "mine",
    "minedetector",
    "minedetectoraxis",
    "detonator",
    "kar98mortar",
    "PIAT",
    // HZM coop [user 2026-08-17] appended - MUST stay index-aligned with animPrefix_e below.
    // Appended rather than inserted so no existing prefix index shifts.
    "type100",
    // [user 2026-08-17] index-aligned with animPrefix_e below; appended, never inserted.
    "johnson"
};

enum animPrefix_e {
    WPREFIX_NONE,
    WPREFIX_UNARMED,
    WPREFIX_PAPERS,
    WPREFIX_COLT45,
    WPREFIX_P38,
    WPREFIX_HISTANDARD,
    WPREFIX_GARAND,
    WPREFIX_KAR98,
    WPREFIX_KAR98SNIPER,
    WPREFIX_SPRINGFIELD,
    WPREFIX_THOMPSON,
    WPREFIX_MP40,
    WPREFIX_BAR,
    WPREFIX_MP44,
    WPREFIX_FRAGGRENADE,
    WPREFIX_STIELHANDGRANATE,
    WPREFIX_BAZOOKA,
    WPREFIX_PANZERSCHRECK,
    WPREFIX_SHOTGUN,
    //
    // Team Assault and Team Tactics weapons
    WPREFIX_MG42_PORTABLE,
    WPREFIX_WEBLEY,
    WPREFIX_NAGANTREV,
    WPREFIX_BERETTA,
    WPREFIX_ENFIELD,
    WPREFIX_SVT,
    WPREFIX_MOSIN,
    WPREFIX_G43,
    WPREFIX_ENFIELDL42A,
    WPREFIX_CARCANO,
    WPREFIX_DELISLE,
    WPREFIX_STEN,
    WPREFIX_PPSH,
    WPREFIX_MOSCHETTO,
    WPREFIX_FG42,
    WPREFIX_VICKERS,
    WPREFIX_BREDA,
    WPREFIX_F1_GRENADE,
    WPREFIX_MILLS_GRENADE,
    WPREFIX_NEBELHANDGRANATE,
    WPREFIX_M18_SMOKE_GRENADE,
    WPREFIX_RDG1_SMOKE_GRENADE,
    WPREFIX_BOMBA,
    WPREFIX_BOMBA_BREDA,
    WPREFIX_MINE,
    WPREFIX_MINE_DETECTOR,
    WPREFIX_MINE_DETECTOR_AXIS,
    WPREFIX_DETONATOR,
    WPREFIX_KAR98_MORTAR,
    WPREFIX_PIAT,
    WPREFIX_TYPE100,       // [user 2026-08-17] matches "type100" in AnimPrefixList
    WPREFIX_JOHNSON        // [user 2026-08-17] matches "johnson" in AnimPrefixList
};

int CG_GetVMAnimPrefixIndex()
{
    const char *szWeaponName;
    int         iWeaponClass;

    iWeaponClass = cg.snap->ps.stats[STAT_EQUIPPED_WEAPON];
    szWeaponName = CG_ConfigString(CS_WEAPONS + cg.snap->ps.activeItems[1]);

    if (iWeaponClass & WEAPON_CLASS_ANY_ITEM) {
        if (!Q_stricmp(szWeaponName, "Papers")) {
            return WPREFIX_PAPERS;
        }
        if (!Q_stricmp(szWeaponName, "Packed MG42 Turret")) {
            return WPREFIX_MG42_PORTABLE;
        }
    } else if (iWeaponClass & WEAPON_CLASS_PISTOL) {
        if (!Q_stricmp(szWeaponName, "Colt 45")) {
            return WPREFIX_COLT45;
        }
        if (!Q_stricmp(szWeaponName, "Walther P38")) {
            return WPREFIX_P38;
        }
        if (!Q_stricmp(szWeaponName, "Hi-Standard Silenced")) {
            return WPREFIX_HISTANDARD;
        }

        //
        // Team Assault and Team Tactics
        //
        if (!Q_stricmp(szWeaponName, "Webley Revolver")) {
            return WPREFIX_WEBLEY;
        }
        if (!Q_stricmp(szWeaponName, "Nagant Revolver")) {
            return WPREFIX_NAGANTREV;
        }
        if (!Q_stricmp(szWeaponName, "Beretta")) {
            return WPREFIX_BERETTA;
        }
        //
        // HZM coop [user 2026-08-17]: East's Mauser C96 is rigged to the P38 skeleton - that is
        // what let his mod ship as a P38 replacement and inherit its animations. Without this the
        // pistol-class fallback below would hand it Colt 45 hands, which is the wrong rig.
        //
        if (!Q_stricmp(szWeaponName, "Mauser C96")) {
            return WPREFIX_P38;
        }

        return WPREFIX_COLT45;
    } else if (iWeaponClass & WEAPON_CLASS_RIFLE) {
        if (!Q_stricmp(szWeaponName, "M1 Garand")) {
            return WPREFIX_GARAND;
        }
        if (!Q_stricmp(szWeaponName, "Mauser KAR 98K")) {
            return WPREFIX_KAR98;
        }
        if (!Q_stricmp(szWeaponName, "KAR98 - Sniper")) {
            return WPREFIX_KAR98SNIPER;
        }
        if (!Q_stricmp(szWeaponName, "Springfield '03 Sniper")) {
            return WPREFIX_SPRINGFIELD;
        }

        //
        // Team Assault and Team Tactics
        //
        if (!Q_stricmp(szWeaponName, "Lee-Enfield")) {
            return WPREFIX_ENFIELD;
        }
        if (!Q_stricmp(szWeaponName, "SVT 40")) {
            return WPREFIX_SVT;
        }
        if (!Q_stricmp(szWeaponName, "Mosin Nagant Rifle")) {
            return WPREFIX_MOSIN;
        }
        if (!Q_stricmp(szWeaponName, "G 43")) {
            return WPREFIX_G43;
        }
        if (!Q_stricmp(szWeaponName, "Enfield L42A1")) {
            return WPREFIX_ENFIELDL42A;
        }
        if (!Q_stricmp(szWeaponName, "Carcano")) {
            return WPREFIX_CARCANO;
        }
        if (!Q_stricmp(szWeaponName, "DeLisle")) {
            return WPREFIX_DELISLE;
        }

        //
        // HZM coop [user 07-12]: imported (xw) rifles share their vanilla base gun's 1P anim set
        // (reload/bolt/charge/fire viewmodels + their client sounds) instead of the garand fallback.
        //
        if (!Q_stricmp(szWeaponName, "G43 Sniper")) {
            return WPREFIX_G43;
        }
        if (!Q_stricmp(szWeaponName, "StG44 Scoped")) {
            return WPREFIX_MP44;
        }
        if (!Q_stricmp(szWeaponName, "Arisaka Type 99") || !Q_stricmp(szWeaponName, "Arisaka Sniper")) {
            return WPREFIX_KAR98;
        }
        if (!Q_stricmp(szWeaponName, "Silenced Kar98 Sniper")) {
            return WPREFIX_KAR98SNIPER;
        }
        if (!Q_stricmp(szWeaponName, "Mosin-Nagant Sniper") || !Q_stricmp(szWeaponName, "Silenced Mosin Sniper")) {
            return WPREFIX_MOSIN;
        }
        if (!Q_stricmp(szWeaponName, "Carcano Sniper")) {
            return WPREFIX_CARCANO;
        }
        if (!Q_stricmp(szWeaponName, "Lee-Enfield Sniper")) {
            return WPREFIX_ENFIELD;
        }
        if (!Q_stricmp(szWeaponName, "Springfield M1903")) {
            return WPREFIX_SPRINGFIELD;
        }
        //
        // HZM coop [user 2026-08-17]: the Johnson gets its OWN prefix rather than borrowing the
        // Garand's, because East shipped a real 91-frame first-person reload with it. Mapping it
        // to WPREFIX_GARAND would have thrown that away - the same defect as the FG42 wearing StG
        // 44 hands and the Type 100 wearing the Sten's.
        //
        if (!Q_stricmp(szWeaponName, "Johnson M1941")) {
            return WPREFIX_JOHNSON;
        }

        return WPREFIX_GARAND;
    } else if (iWeaponClass & WEAPON_CLASS_SMG) {
        if (!Q_stricmp(szWeaponName, "Thompson")) {
            return WPREFIX_THOMPSON;
        }
        if (!Q_stricmp(szWeaponName, "MP40")) {
            return WPREFIX_MP40;
        }
        //
        // Team Assault and Team Tactics
        //
        if (!Q_stricmp(szWeaponName, "Sten Mark II")) {
            return WPREFIX_STEN;
        }
        if (!Q_stricmp(szWeaponName, "PPSH SMG")) {
            return WPREFIX_PPSH;
        }
        if (!Q_stricmp(szWeaponName, "Moschetto")) {
            return WPREFIX_MOSCHETTO;
        }

        //
        // HZM coop [user 07-12]: imported (xw) SMGs share their vanilla base gun's 1P anim set:
        // Type 100 -> Sten (user spec), Grease Guns / Silenced MP40 / Silenced PPS-43 -> MP40
        // (user spec), Beretta M38 (moschetto) -> Moschetto.
        //
        if (!Q_stricmp(szWeaponName, "Type 100 SMG")) {
            // HZM coop [user 2026-08-17] - was WPREFIX_STEN, per the 07-12 spec above. That spec was
            // made on the assumption the xw pack shipped no first-person animations for this gun.
            // It does: models/human/animation/viewmodel/type100/ carries five DISTINCT anims
            // (verified by hash, not by size - four of them share a byte count by coincidence),
            // including a 76-frame reload. Borrowing the Sten's threw all of that away. Same defect
            // as the FG42's WPREFIX_MP44 mapping, found by auditing the pack for shipped-but-
            // unreferenced content. Revert this one line to go back to Sten hands.
            return WPREFIX_TYPE100;
        }
        if (!Q_stricmp(szWeaponName, "M3 Grease Gun") || !Q_stricmp(szWeaponName, "Silenced Grease Gun")
            || !Q_stricmp(szWeaponName, "Silenced MP40") || !Q_stricmp(szWeaponName, "Silenced PPS-43")) {
            return WPREFIX_MP40;
        }
        if (!Q_stricmp(szWeaponName, "Beretta M38")) {
            return WPREFIX_MOSCHETTO;
        }

        return WPREFIX_THOMPSON;
    } else if (iWeaponClass & WEAPON_CLASS_MG) {
        if (!Q_stricmp(szWeaponName, "BAR")) {
            return WPREFIX_BAR;
        }
        if (!Q_stricmp(szWeaponName, "StG 44")) {
            return WPREFIX_MP44;
        }
        //
        // Team Assault and Team Tactics
        //
        if (!Q_stricmp(szWeaponName, "FG 42")) {
            // HZM coop [user 2026-08-17] - was WPREFIX_MP44, which is why the FG42 kept using StG 44
            // hands: the engine asked for mp44_* aliases, so repointing the fg42_* ones in
            // fps_anims_mg.txt could never take effect. WPREFIX_FG42 was declared in the enum and the
            // "fg42" string sits at the matching index in AnimPrefixList, but it was NEVER RETURNED
            // anywhere - a prefix wired up on both sides and left unreachable in the middle.
            // Now that DaRKaNGeL's real fg42_stand_idle/fire/reload animations exist, this is the
            // line that lets them be selected. cgame.dll-only change.
            return WPREFIX_FG42;
        }
        if (!Q_stricmp(szWeaponName, "Vickers-Berthier")) {
            return WPREFIX_VICKERS;
        }
        if (!Q_stricmp(szWeaponName, "Breda")) {
            return WPREFIX_BREDA;
        }

        return WPREFIX_BAR;
    } else if (iWeaponClass & WEAPON_CLASS_GRENADE) {
        if (!Q_stricmp(szWeaponName, "Frag Grenade")) {
            return WPREFIX_FRAGGRENADE;
        }
        if (!Q_stricmp(szWeaponName, "Stielhandgranate")) {
            return WPREFIX_STIELHANDGRANATE;
        }
        //
        // Team Assault and Team Tactics
        //
        if (!Q_stricmp(szWeaponName, "F1 Grenade")) {
            return WPREFIX_F1_GRENADE;
        }
        if (!Q_stricmp(szWeaponName, "Mills Grenade")) {
            return WPREFIX_MILLS_GRENADE;
        }
        if (!Q_stricmp(szWeaponName, "Nebelhandgranate")) {
            return WPREFIX_NEBELHANDGRANATE;
        }
        if (!Q_stricmp(szWeaponName, "M18 Smoke Grenade")) {
            return WPREFIX_M18_SMOKE_GRENADE;
        }
        if (!Q_stricmp(szWeaponName, "RDG-1 Smoke Grenade")) {
            return WPREFIX_RDG1_SMOKE_GRENADE;
        }
        if (!Q_stricmp(szWeaponName, "Bomba A Mano")) {
            return WPREFIX_BOMBA;
        }
        if (!Q_stricmp(szWeaponName, "Bomba A Mano Breda")) {
            return WPREFIX_BOMBA_BREDA;
        }
        if (!Q_stricmp(szWeaponName, "LandmineAllies")) {
            return WPREFIX_MINE;
        }
        if (!Q_stricmp(szWeaponName, "LandmineAxis")) {
            return WPREFIX_MINE;
        }
        if (!Q_stricmp(szWeaponName, "Minensuchgerat")) {
            return WPREFIX_MINE_DETECTOR_AXIS;
        }
        if (!Q_stricmp(szWeaponName, "Minedetector")) {
            return WPREFIX_MINE_DETECTOR;
        }

        return WPREFIX_FRAGGRENADE;
    } else if (iWeaponClass & WEAPON_CLASS_HEAVY) {
        if (!Q_stricmp(szWeaponName, "Bazooka")) {
            return WPREFIX_BAZOOKA;
        }
        if (!Q_stricmp(szWeaponName, "Panzerschreck")) {
            return WPREFIX_PANZERSCHRECK;
        }
        if (!Q_stricmp(szWeaponName, "Gewehrgranate")) {
            return WPREFIX_KAR98_MORTAR;
        }
        if (!Q_stricmp(szWeaponName, "Shotgun")) {
            return WPREFIX_SHOTGUN;
        }
        //
        // Team Assault and Team Tactics
        //
        if (!Q_stricmp(szWeaponName, "PIAT")) {
            return WPREFIX_PIAT;
        }

        return WPREFIX_BAZOOKA;
    }

    return WPREFIX_UNARMED;
}

void CG_ViewModelAnimation(refEntity_t *pModel)
{
    int         i;
    int         iAnimFlags;
    float       fCrossblendTime, fCrossblendFrac, fCrossblendAmount;
    float       fAnimLength;
    int         iAnimPrefixIndex;
    const char *pszAnimSuffix;
    char        szAnimName[MAX_QPATH];
    dtiki_t    *pTiki;
    qboolean    bAnimChanged;
    qboolean    bWeaponChanged;

    fCrossblendFrac = 0.0;
    bAnimChanged    = qfalse;
    bWeaponChanged  = qfalse; // Added in OPM
    pTiki           = pModel->tiki;

    if (cgi.anim->g_iLastEquippedWeaponStat == cg.snap->ps.stats[STAT_EQUIPPED_WEAPON]
        && !strcmp(cgi.anim->g_szLastActiveItem, CG_ConfigString(CS_WEAPONS + cg.snap->ps.activeItems[1]))) {
        iAnimPrefixIndex = cgi.anim->g_iLastAnimPrefixIndex;
    } else {
        iAnimPrefixIndex                    = CG_GetVMAnimPrefixIndex();
        cgi.anim->g_iLastEquippedWeaponStat = cg.snap->ps.stats[STAT_EQUIPPED_WEAPON];
        Q_strncpyz(cgi.anim->g_szLastActiveItem, CG_ConfigString(CS_WEAPONS + cg.snap->ps.activeItems[1]), sizeof(cgi.anim->g_szLastActiveItem));
        cgi.anim->g_iLastAnimPrefixIndex = iAnimPrefixIndex;

        bAnimChanged   = qtrue;
        bWeaponChanged = qtrue;
    }

    if (cgi.anim->g_iLastVMAnim == -1) {
        Com_sprintf(szAnimName, sizeof(szAnimName), "%s_idle", AnimPrefixList[iAnimPrefixIndex]);
        cgi.anim->g_VMFrameInfo[cgi.anim->g_iCurrentVMAnimSlot].index = cgi.Anim_NumForName(pTiki, szAnimName);

        if (cgi.anim->g_VMFrameInfo[cgi.anim->g_iCurrentVMAnimSlot].index == -1) {
            cgi.anim->g_VMFrameInfo[cgi.anim->g_iCurrentVMAnimSlot].index = cgi.Anim_NumForName(pTiki, "idle");
            cgi.DPrintf("Warning: Couldn't find view model animation %s\n", szAnimName);
        }

        cgi.anim->g_VMFrameInfo[cgi.anim->g_iCurrentVMAnimSlot].time   = 0.0;
        cgi.anim->g_VMFrameInfo[cgi.anim->g_iCurrentVMAnimSlot].weight = 1.0;
        cgi.anim->g_iLastVMAnim                                        = 0;
    }

    if (cg.snap->ps.iViewModelAnimChanged != cgi.anim->g_iLastVMAnimChanged) {
        bAnimChanged                   = qtrue;
        cgi.anim->g_iLastVMAnim        = cg.snap->ps.iViewModelAnim;
        cgi.anim->g_iLastVMAnimChanged = cg.snap->ps.iViewModelAnimChanged;
    }

    if (bAnimChanged) {
        switch (cgi.anim->g_iLastVMAnim) {
        case VM_ANIM_CHARGE:
            pszAnimSuffix = "charge";
            break;
        case VM_ANIM_FIRE:
            pszAnimSuffix = "fire";
            break;
        case VM_ANIM_FIRE_SECONDARY:
            pszAnimSuffix = "fire_secondary";
            break;
        case VM_ANIM_RECHAMBER:
            pszAnimSuffix = "rechamber";
            break;
        case VM_ANIM_RELOAD:
            pszAnimSuffix = "reload";
            break;
        case VM_ANIM_RELOAD_SINGLE:
            pszAnimSuffix = "reload_single";
            break;
        case VM_ANIM_RELOAD_END:
            pszAnimSuffix = "reload_end";
            break;
        case VM_ANIM_PULLOUT:
            pszAnimSuffix = "pullout";
            break;
        case VM_ANIM_PUTAWAY:
            pszAnimSuffix = "putaway";
            break;
        case VM_ANIM_LADDERSTEP:
            pszAnimSuffix = "ladderstep";
            break;
        case VM_ANIM_IDLE_0:
            pszAnimSuffix = "idle0";
            break;
        case VM_ANIM_IDLE_1:
            pszAnimSuffix = "idle1";
            break;
        case VM_ANIM_IDLE_2:
            pszAnimSuffix = "idle2";
            break;
        case VM_ANIM_DISABLED:
            pszAnimSuffix = "disabled";
            break;
        default:
        case VM_ANIM_IDLE:
            pszAnimSuffix = "idle";
            break;
        }

        Com_sprintf(szAnimName, sizeof(szAnimName), "%s_%s", AnimPrefixList[iAnimPrefixIndex], pszAnimSuffix);
        if (!bWeaponChanged) {
            fCrossblendTime =
                cgi.Anim_CrossblendTime(pTiki, cgi.anim->g_VMFrameInfo[cgi.anim->g_iCurrentVMAnimSlot].index);
            fCrossblendAmount = cgi.anim->g_iCurrentVMDuration / 1000.0;

            if (fCrossblendAmount < fCrossblendTime && fCrossblendAmount > 0.0) {
                fCrossblendFrac = fCrossblendAmount / fCrossblendTime;
                for (i = 0; i < MAX_FRAMEINFOS; ++i) {
                    if (cgi.anim->g_VMFrameInfo[i].weight) {
                        if (i == cgi.anim->g_iCurrentVMAnimSlot) {
                            cgi.anim->g_VMFrameInfo[i].weight = fCrossblendFrac;
                        } else {
                            cgi.anim->g_VMFrameInfo[i].weight *= (1.0 - fCrossblendFrac);
                        }
                    }
                }
            }
        } else {
            fCrossblendTime   = 0;
            fCrossblendAmount = 0;
            fCrossblendFrac   = 0;
        }

        cgi.anim->g_iCurrentVMAnimSlot = (cgi.anim->g_iCurrentVMAnimSlot + 1) % MAX_FRAMEINFOS;
        cgi.anim->g_VMFrameInfo[cgi.anim->g_iCurrentVMAnimSlot].index = cgi.Anim_NumForName(pTiki, szAnimName);

        if (cgi.anim->g_VMFrameInfo[cgi.anim->g_iCurrentVMAnimSlot].index == -1) {
            cgi.anim->g_VMFrameInfo[cgi.anim->g_iCurrentVMAnimSlot].index = cgi.Anim_NumForName(pTiki, "idle");
            cgi.DPrintf("Warning: Couldn't find view model animation %s\n", szAnimName);
        }

        cgi.anim->g_VMFrameInfo[cgi.anim->g_iCurrentVMAnimSlot].time   = 0.0;
        cgi.anim->g_VMFrameInfo[cgi.anim->g_iCurrentVMAnimSlot].weight = 1.0;
        cgi.anim->g_iCurrentVMDuration                                 = 0;

        if (!bWeaponChanged) {
            fCrossblendTime =
                cgi.Anim_CrossblendTime(pTiki, cgi.anim->g_VMFrameInfo[cgi.anim->g_iCurrentVMAnimSlot].index);
            if (!fCrossblendTime) {
                for (i = 0; i < MAX_FRAMEINFOS; ++i) {
                    if (i != cgi.anim->g_iCurrentVMAnimSlot) {
                        cgi.anim->g_VMFrameInfo[i].weight = 0.0;
                    }
                }

                cgi.anim->g_bCrossblending = qfalse;
            } else {
                cgi.anim->g_bCrossblending = qtrue;
            }
        } else {
            // Added in OPM
            //  If there is a new weapon, don't do any crossblend
            cgi.anim->g_bCrossblending = qfalse;

            // clear crossblend values
            for (i = 0; i < MAX_FRAMEINFOS; ++i) {
                if (i != cgi.anim->g_iCurrentVMAnimSlot) {
                    cgi.anim->g_VMFrameInfo[i].weight = 0.0;
                }
            }
        }
    }

    cgi.anim->g_iCurrentVMDuration += cg.frametime;
    if (cgi.anim->g_bCrossblending) {
        fCrossblendTime = cgi.Anim_CrossblendTime(pTiki, cgi.anim->g_VMFrameInfo[cgi.anim->g_iCurrentVMAnimSlot].index);
        fCrossblendAmount = cgi.anim->g_iCurrentVMDuration / 1000.0;
        if (fCrossblendAmount >= fCrossblendTime || fCrossblendAmount <= 0.0) {
            // clear crossblend values
            for (i = 0; i < MAX_FRAMEINFOS; ++i) {
                if (i != cgi.anim->g_iCurrentVMAnimSlot) {
                    cgi.anim->g_VMFrameInfo[i].weight = 0.0;
                }
            }

            cgi.anim->g_bCrossblending = qfalse;
        } else {
            fCrossblendFrac = fCrossblendAmount / fCrossblendTime;
        }
    }

    for (i = 0; i < MAX_FRAMEINFOS; ++i) {
        if (!cgi.anim->g_VMFrameInfo[i].weight) {
            // clear the weight values of the ref entity
            pModel->frameInfo[i].index  = 0;
            pModel->frameInfo[i].time   = 0.0;
            pModel->frameInfo[i].weight = 0.0;
        } else {
            fAnimLength = cgi.Anim_Time(pTiki, cgi.anim->g_VMFrameInfo[i].index);
            cgi.anim->g_VMFrameInfo[i].time += cg.frametime / 1000.0;

            if (cgi.anim->g_VMFrameInfo[i].time > fAnimLength) {
                if (pTiki) {
                    if (cgi.Anim_Flags(pTiki, cgi.anim->g_VMFrameInfo[i].index) & TAF_DELTADRIVEN) {
                        cgi.anim->g_VMFrameInfo[i].time -= fAnimLength;
                    } else {
                        cgi.anim->g_VMFrameInfo[i].time = fAnimLength;
                    }
                } else {
                    cgi.anim->g_VMFrameInfo[i].time = 0.f;
                }
            }

            pModel->frameInfo[i].index = cgi.anim->g_VMFrameInfo[i].index;
            pModel->frameInfo[i].time  = cgi.anim->g_VMFrameInfo[i].time;

            if (cgi.anim->g_bCrossblending) {
                if (i == cgi.anim->g_iCurrentVMAnimSlot) {
                    pModel->frameInfo[i].weight = fCrossblendFrac;
                } else {
                    pModel->frameInfo[i].weight = cgi.anim->g_VMFrameInfo[i].weight * (1.0 - fCrossblendFrac);
                }
            } else {
                pModel->frameInfo[i].weight = 1.0;
            }
        }
    }

    pModel->actionWeight = 1.0;
}

void CG_CalcViewModelMovement(float fViewBobPhase, float fViewBobAmp, vec_t *vVelocity, vec_t *vMovement)
{
    int    i;
    float  fPhase, fDelta;
    vec3_t vTargOfs;
    vec3_t vNorm;

    fPhase       = sin(fViewBobPhase + M_PI / 10) * fViewBobAmp * vm_sway_side->value;
    vMovement[0] = fPhase * vm_sway_front->value;
    vMovement[1] = fPhase;

    fPhase       = sin(fViewBobPhase - 0.94 + fViewBobPhase - 0.94 + M_PI);
    vMovement[2] = (sin((fViewBobPhase - 0.94) * 4.0 + M_PI / 2) * 0.125 + fPhase) * fViewBobAmp * vm_sway_up->value;

    if (cg.predicted_player_state.walking) {
        if (cg.predicted_player_state.viewheight == CROUCH_EYE_HEIGHT) {
            if (cgi.anim->g_iLastAnimPrefixIndex == WPREFIX_BAZOOKA
                || cgi.anim->g_iLastAnimPrefixIndex == WPREFIX_PANZERSCHRECK) {
                vTargOfs[0] = vm_offset_rocketcrouch_front->value;
                vTargOfs[1] = vm_offset_rocketcrouch_side->value;
                vTargOfs[2] = vm_offset_rocketcrouch_up->value;
            } else if (cgi.anim->g_iLastAnimPrefixIndex == WPREFIX_SHOTGUN) {
                vTargOfs[0] = vm_offset_shotguncrouch_front->value;
                vTargOfs[1] = vm_offset_shotguncrouch_side->value;
                vTargOfs[2] = vm_offset_shotguncrouch_up->value;
            } else {
                vTargOfs[0] = vm_offset_crouch_front->value;
                vTargOfs[1] = vm_offset_crouch_side->value;
                vTargOfs[2] = vm_offset_crouch_up->value;
            }
        } else {
            memset(vTargOfs, 0, sizeof(vTargOfs));
        }
    } else {
        vTargOfs[0] = vm_offset_air_front->value;
        vTargOfs[1] = vm_offset_air_side->value;
        vTargOfs[2] = vm_offset_air_up->value;
    }

    if (cg.predicted_player_state.walking) {
        fDelta = VectorLength(vVelocity) - vm_offset_vel_base->value;
        if (fDelta > 0.0) {
            if (fDelta > 250.0 - vm_offset_vel_base->value) {
                fDelta = 250.0 - vm_offset_vel_base->value;
            }

            fPhase = fDelta / (250.0 - vm_offset_vel_base->value);
            vTargOfs[0] += fPhase * vm_offset_vel_front->value;
            vTargOfs[1] += fPhase * vm_offset_vel_side->value;
            vTargOfs[2] += fPhase * vm_offset_vel_up->value;
        }
    } else if (vVelocity[2]) {
        vTargOfs[2] -= vVelocity[2] * vm_offset_upvel->value;
    }

    for (i = 0; i < 3; i++) {
        fDelta = vTargOfs[i] - cgi.anim->g_vCurrentVMPosOffset[i];
        cgi.anim->g_vCurrentVMPosOffset[i] += cg.frametime / 1000.0 * fDelta * vm_offset_speed->value;

        if (fDelta > 0.0) {
            if (cgi.anim->g_vCurrentVMPosOffset[i] > vTargOfs[i]) {
                cgi.anim->g_vCurrentVMPosOffset[i] = vTargOfs[i];
            }
        } else if (fDelta < 0.0) {
            if (cgi.anim->g_vCurrentVMPosOffset[i] < vTargOfs[i]) {
                cgi.anim->g_vCurrentVMPosOffset[i] = vTargOfs[i];
            }
        }
    }

    VectorAdd(vMovement, cgi.anim->g_vCurrentVMPosOffset, vMovement);
    if (cg.predicted_player_state.fLeanAngle) {
        vMovement[2] -= fabs(cg.predicted_player_state.fLeanAngle) * vm_lean_lower->value;
    }

    if (VectorNormalize2(vMovement, vNorm) > vm_offset_max->value) {
        VectorScale(vNorm, vm_offset_max->value, vMovement);
    }
}
