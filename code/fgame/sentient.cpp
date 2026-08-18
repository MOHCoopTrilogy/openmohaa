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

// sentient.cpp: Base class of entity that can carry other entities, and use weapons.
//

#include "g_local.h"
#include "g_phys.h"
#include "entity.h"
#include "sentient.h"
#include "weapon.h"
#include "weaputils.h"
#include "scriptmaster.h"
#include "scriptexception.h"
#include "ammo.h"
#include "armor.h"
#include "misc.h"
#include "inventoryitem.h"
#include "player.h"
#include "actor.h"
#include "decals.h"
#include "g_spawn.h"
#include "object.h"
#include "../qcommon/tiki.h"
#include "weapturret.h"

Event EV_Sentient_ReloadWeapon
(
    "reloadweapon",
    EV_DEFAULT,
    "s",
    "hand",
    "Reloads the weapon in the specified hand",
    EV_NORMAL
);
Event EV_Sentient_Attack
(
    "fire",
    EV_DEFAULT,
    "SS",
    "hand mode",
    "Fires the weapon in the specified hand.",
    EV_NORMAL
);
Event EV_Sentient_StopFire
(
    "stopfire",
    EV_DEFAULT,
    "s",
    "hand",
    "Stops the firing of the weapon in the specified hand.",
    EV_NORMAL
);
Event EV_Sentient_Charge
(
    "charge",
    EV_DEFAULT,
    "s",
    "hand",
    "Starts the charging of the weapon in the specified hand",
    EV_NORMAL
);
Event EV_Sentient_ReleaseAttack
(
    "releasefire",
    EV_DEFAULT,
    "f",
    "fireholdtime",
    "Releases the attack in the time specified.",
    EV_NORMAL
);
Event EV_Sentient_GiveWeapon
(
    "weapon",
    EV_DEFAULT,
    "s",
    "weapon_modelname",
    "Gives the sentient the weapon specified.",
    EV_NORMAL
);
Event EV_Sentient_SetWeaponIdleState
(
    "setweaponidlestate",
    EV_DEFAULT,
    "i",
    "state",
    "set the idle state of the given weapon.",
    EV_NORMAL
);
Event EV_Sentient_PingForMines
(
    "pingformines",
    EV_DEFAULT,
    NULL,
    NULL,
    "actively uncover mines nearby.",
    EV_NORMAL
);
Event EV_Sentient_ForceLandmineMeasure
(
    "forcelandminemeasure",
    EV_DEFAULT,
    NULL,
    NULL,
    "Force a remeasurement to all landmines",
    EV_NORMAL
);
Event EV_Sentient_Take
(
    "take",
    EV_DEFAULT,
    "s",
    "item_name",
    "Takes away the specified item from the sentient.",
    EV_NORMAL
);
Event EV_Sentient_TakeAll
(
    "takeall",
    EV_DEFAULT,
    NULL,
    NULL,
    "Clears out the sentient's entire inventory.",
    EV_NORMAL
);
Event EV_Sentient_GiveAmmo
(
    "ammo",
    EV_DEFAULT,
    "siI",
    "type amount max_amount",
    "Gives the sentient some ammo.",
    EV_NORMAL
);
Event EV_Sentient_GetAmmo
(
    "getammo",
    EV_DEFAULT,
    "s",
    "type",
    "Returns the current reserve ammo count of the named type (HZM coop - for exact-ammo respawn).",
    EV_RETURN
);
Event EV_Sentient_SetAmmo
(
    "setammo",
    EV_DEFAULT,
    "si",
    "type amount",
    "Sets the reserve ammo of the named type to an exact amount (HZM coop - for exact-ammo respawn).",
    EV_NORMAL
);
Event EV_Sentient_GiveArmor
(
    "armor",
    EV_DEFAULT,
    "si",
    "type amount",
    "Gives the sentient some armor.",
    EV_NORMAL
);
Event EV_Sentient_GiveItem
(
    "item",
    EV_DEFAULT,
    "si",
    "type amount",
    "Gives the sentient the specified amount of the specified item.",
    EV_NORMAL
);
Event EV_Sentient_GiveDynItem
(
    "givedynitem",
    EV_DEFAULT,
    "ss",
    "model bonename",
    "Pass the args to the item.",
    EV_NORMAL
);
Event EV_Sentient_GiveTargetname
(
    "give",
    EV_DEFAULT,
    "s",
    "name",
    "Gives the sentient the targeted item.",
    EV_NORMAL
);
Event EV_Sentient_UseItem
(
    "use",
    EV_CONSOLE,
    "si",
    "name weapon_hand",
    "Use the specified weapon or item in the hand chosen (optional).",
    EV_NORMAL
);
Event EV_Sentient_SetBloodModel
(
    "bloodmodel",
    EV_DEFAULT,
    "s",
    "bloodModel",
    "set the model to be used when showing blood",
    EV_NORMAL
);
Event EV_Sentient_TurnOffShadow
(
    "noshadow",
    EV_DEFAULT,
    NULL,
    NULL,
    "Turns off the shadow for this sentient.",
    EV_NORMAL
);
Event EV_Sentient_TurnOnShadow
(
    "shadow",
    EV_DEFAULT,
    NULL,
    NULL,
    "Turns on the shadow for this sentient.",
    EV_NORMAL
);
Event EV_Sentient_JumpXY
(
    "jumpxy",
    EV_DEFAULT,
    "fff",
    "forwardmove sidemove speed",
    "Makes the sentient jump.",
    EV_NORMAL
);
Event EV_Sentient_MeleeAttackStart
(
    "meleeattackstart",
    EV_DEFAULT,
    NULL,
    NULL,
    "Is the start of the sentient's melee attack.",
    EV_NORMAL
);
Event EV_Sentient_MeleeAttackEnd
(
    "meleeattackend",
    EV_DEFAULT,
    NULL,
    NULL,
    "Is the end of the sentient's melee attack.",
    EV_NORMAL
);
Event EV_Sentient_BlockStart
(
    "blockstart",
    EV_DEFAULT,
    NULL,
    NULL,
    "Is the start of the sentient's block.",
    EV_NORMAL
);
Event EV_Sentient_BlockEnd
(
    "blockend",
    EV_DEFAULT,
    NULL,
    NULL,
    "Is the end of the sentient's block.",
    EV_NORMAL
);
Event EV_Sentient_StunStart
(
    "stunstart",
    EV_DEFAULT,
    NULL,
    NULL,
    "Is the start of the sentient's stun.",
    EV_NORMAL
);
Event EV_Sentient_StunEnd
(
    "stunend",
    EV_DEFAULT,
    NULL,
    NULL,
    "Is the end of the sentient's stun.",
    EV_NORMAL
);
Event EV_Sentient_SetMouthAngle
(
    "mouthangle",
    EV_DEFAULT,
    "f",
    "mouth_angle",
    "Sets the mouth angle of the sentient.",
    EV_NORMAL
);
Event EV_Sentient_SetMaxMouthAngle
(
    "maxmouthangle",
    EV_DEFAULT,
    "f",
    "max_mouth_angle",
    "Sets the max mouth angle.",
    EV_NORMAL
);
Event EV_Sentient_OnFire
(
    "onfire",
    EV_DEFAULT,
    NULL,
    NULL,
    "Called every frame when the sentient is on fire.",
    EV_NORMAL
);
Event EV_Sentient_StopOnFire
(
    "stoponfire",
    EV_DEFAULT,
    NULL,
    NULL,
    "Stops the sentient from being on fire.",
    EV_NORMAL
);
Event EV_Sentient_SpawnBloodyGibs
(
    "spawnbloodygibs",
    EV_DEFAULT,
    "IF",
    "number_of_gibs scale",
    "Spawns some bloody generic gibs.",
    EV_NORMAL
);
Event EV_Sentient_SetMaxGibs
(
    "maxgibs",
    EV_DEFAULT,
    "i",
    "max_number_of_gibs",
    "Sets the maximum amount of generic gibs this sentient will spawn when hit.",
    EV_NORMAL
);
Event EV_Sentient_CheckAnimations
(
    "checkanims",
    EV_DEFAULT,
    NULL,
    NULL,
    "Check the animations in the .tik file versus the statefile",
    EV_NORMAL
);
Event EV_Sentient_DeactivateWeapon
(
    "deactivateweapon",
    EV_DEFAULT,
    "s",
    "side",
    "Deactivate the weapon in the specified hand.",
    EV_NORMAL
);
Event EV_Sentient_ActivateNewWeapon
(
    "activatenewweapon",
    EV_DEFAULT,
    NULL,
    NULL,
    "Activate the new weapon specified by useWeapon. handsurf allows specifying which hand to use for the player",
    EV_NORMAL
);
Event EV_Sentient_PutawayWeapon
(
    "putawayweapon",
    EV_DEFAULT,
    "s",
    "whichHand",
    "Put away or deactivate the current weapon, whichHand can be left or right.",
    EV_NORMAL
);
Event EV_Sentient_Weapon
(
    "weaponcommand",
    EV_DEFAULT,
    "sSSSSSSS",
    "hand arg1 arg2 arg3 arg4 arg5 arg6 arg7",
    "Pass the args to the active weapon in the specified hand",
    EV_NORMAL
);
Event EV_Sentient_UseWeaponClass
(
    "useweaponclass",
    EV_CONSOLE,
    "sI",
    "name weapon_hand",
    "Use the weapon of the specified class in the hand chosen (optional).",
    EV_NORMAL
);
Event EV_Sentient_German
(
    "german",
    EV_DEFAULT,
    NULL,
    NULL,
    "Makes the sentient a German.",
    EV_NORMAL
);
Event EV_Sentient_American
(
    "american",
    EV_DEFAULT,
    NULL,
    NULL,
    "Makes the sentient an American.",
    EV_NORMAL
);
Event EV_Sentient_GetTeam
(
    "team",
    EV_DEFAULT,
    NULL,
    NULL,
    "returns 'german' or 'american'",
    EV_GETTER
);
Event EV_Sentient_SetDamageMult
(
    "damagemult",
    EV_DEFAULT,
    "if",
    "location multiplier",
    "Sets the damage multiplier for a particular body location",
    EV_NORMAL
);
Event EV_Sentient_UseLastWeapon
(
    "uselast",
    EV_DEFAULT,
    NULL,
    NULL,
    "Activates the last active weapon",
    EV_NORMAL
);
Event EV_Sentient_ToggleItemUse
(
    "toggleitem",
    EV_CONSOLE,
    NULL,
    NULL,
    "Toggles the use of the player's item (first item if he has multiple)",
    EV_NORMAL
);
Event EV_Sentient_GetThreatBias
(
    "threatbias",
    EV_DEFAULT,
    NULL,
    NULL,
    "Gets the threat bias for this player / AI",
    EV_GETTER
);
Event EV_Sentient_SetThreatBias
(
    "threatbias",
    EV_DEFAULT,
    "i",
    "bias",
    "Sets the threat bias for this player / AI",
    EV_SETTER
);
Event EV_Sentient_SetThreatBias2
(
    "threatbias",
    EV_DEFAULT,
    "i",
    "bias",
    "Sets the threat bias for this player / AI",
    EV_NORMAL
);
Event EV_Sentient_SetupHelmet
(
    "sethelmet",
    EV_DEFAULT,
    "sffss",
    "tikifile popspeed dmgmult surfacename [optional_additional_surface_name]",
    "Gives the sentient a helmet and sets the needed info for it",
    EV_NORMAL
);
Event EV_Sentient_PopHelmet
(
    "pophelmet",
    EV_DEFAULT,
    NULL,
    NULL,
    "Pops a sentient's helmet off if he's got one",
    EV_NORMAL
);
Event EV_Sentient_DropItems
(
    "dropitems",
    EV_DEFAULT,
    NULL,
    NULL,
    "drops inventory items",
    EV_NORMAL
);
Event EV_Sentient_DontDropWeapons
(
    "dontdropweapons",
    EV_DEFAULT,
    "B",
    "dont_drop",
    "Make the sentient not drop weapons",
    EV_NORMAL
);
Event EV_Sentient_ForceDropWeapon
(
    "forcedropweapon",
    EV_DEFAULT,
    NULL,
    NULL,
    "Force the sentient to drop weapons no matter what level.nodropweapon is.",
    EV_NORMAL
);
Event EV_Sentient_ForceDropWeapon2
(
    "forcedropweapon",
    EV_DEFAULT,
    NULL,
    NULL,
    "Force the sentient to drop weapons no matter what level.nodropweapon is.",
    EV_SETTER
);
Event EV_Sentient_ForceDropHealth
(
    "forcedrophealth",
    EV_DEFAULT,
    NULL,
    NULL,
    "Force the sentient to drop health no matter what level.nodrophealth is.",
    EV_NORMAL
);
Event EV_Sentient_ForceDropHealth2
(
    "forcedrophealth",
    EV_DEFAULT,
    NULL,
    NULL,
    "Force the sentient to drop health no matter what level.nodrophealth is.",
    EV_SETTER
);
Event EV_Sentient_GetForceDropHealth
(
    "forcedrophealth",
    EV_DEFAULT,
    NULL,
    NULL,
    "Get if the sentient is forced to drop health no matter what level.nodrophealth is.",
    EV_GETTER
);
Event EV_Sentient_GetForceDropWeapon
(
    "forcedropweapon",
    EV_DEFAULT,
    NULL,
    NULL,
    "Get if the sentient is forced to drop health no matter what level.nodrophealth is.",
    EV_GETTER
);

//
// Added in OPM
//
Event EV_Sentient_GetNewActiveWeap
(
    "getnewactiveweap",
    EV_DEFAULT,
    NULL,
    NULL,
    "gets new active weapon",
    EV_RETURN
);
Event EV_Sentient_GetNewActiveWeapon
(
    "newActiveWeapon",
    EV_DEFAULT,
    NULL,
    NULL,
    "gets new active weapon",
    EV_GETTER
);
Event EV_Sentient_GetNewActiveWeaponHand
(
    "newActiveWeaponHand",
    EV_DEFAULT,
    NULL,
    NULL,
    "gets the hand of the new active weapon",
    EV_GETTER
);
Event EV_Sentient_GetActiveWeap
(
    "getactiveweap",
    EV_DEFAULT,
    "i",
    "weaponhand",
    "gets currently active weapon in a given hand",
    EV_RETURN
);
Event EV_Sentient_Client_Landing
(
    "_client_landing",
    EV_DEFAULT,
    "FI",
    "fVolume iEquipment",
    "Play a landing sound that is appropriate to the surface we are landing on\n"
);
// HZM coop - gore tier 2: one step of the GROWING corpse blood pool. Self-chained PostEvent started by
// DropBloodPool; each step layers one more (larger) non-fading coop_bloodpool decal on the same floor point.
Event EV_Sentient_CoopGorePoolGrow
(
    "_coop_gore_pool_grow",
    EV_DEFAULT,
    "i",
    "step",
    "HZM coop - internal: drop one ring of the growing blood pool under a corpse"
);
// HZM coop - gore tier 1: script hook for heals the engine cannot see (officer canteen, DBNO revive,
// aihandler script-side HP). No arg = full reset (clean uniform + counters + drip removed); with an
// amount = subtract that much healed damage from the gore counter and re-tier (partial heals).
Event EV_Sentient_CoopGoreReset
(
    "gore_reset",
    EV_DEFAULT,
    "F",
    "healed_amount",
    "HZM coop - gore tier 1: clear blood skins/counters (no arg) or credit a partial heal (amount)"
);
// HZM coop - gore tier 1e (extreme explosion-death skins): script hook for SCRIPTED blasts whose applied
// damage does not carry an explosive MOD (e.g. t1l1 truck passengers killed by bare `hurt` when the truck
// blows up). Call `<victim> gore_gibmark` just before/with the scripted blast damage; if the victim dies
// while the mark is fresh (default 2s window) the corpse gets the tier-3 gib skins exactly as if the
// engine had seen MOD_EXPLOSION. Marks on survivors expire harmlessly.
Event EV_Sentient_CoopBlastShield
(
    "blastshield",
    EV_DEFAULT,
    "i",
    "on",
    "HZM coop - bug-1586: 1 = this actor ignores WORLD-attributed explosion damage (mission-critical NPCs only)"
);

Event EV_Sentient_CoopGoreGibMark
(
    "gore_gibmark",
    EV_DEFAULT,
    "F",
    "window_seconds",
    "HZM coop - gore tier 1e: mark this sentient as dying to a scripted explosion (optional window, default 2s)"
);

CLASS_DECLARATION(Animate, Sentient, NULL) {
    {&EV_Sentient_ReloadWeapon,           &Sentient::ReloadWeapon                 },
    {&EV_Sentient_Attack,                 &Sentient::FireWeapon                   },
    {&EV_Sentient_StopFire,               &Sentient::StopFireWeapon               },
    {&EV_Sentient_Charge,                 &Sentient::ChargeWeapon                 },
    {&EV_Sentient_ReleaseAttack,          &Sentient::ReleaseFireWeapon            },
    {&EV_Sentient_GiveAmmo,               &Sentient::EventGiveAmmo                },
    {&EV_Sentient_GetAmmo,                &Sentient::EventGetAmmo                 },
    {&EV_Sentient_SetAmmo,                &Sentient::EventSetAmmo                 },
    {&EV_Sentient_GiveWeapon,             &Sentient::EventGiveItem                },
    {&EV_Sentient_GiveArmor,              &Sentient::EventGiveItem                },
    {&EV_Sentient_GiveItem,               &Sentient::EventGiveItem                },
    {&EV_Sentient_GiveDynItem,            &Sentient::EventGiveDynItem             },
    {&EV_Sentient_UseItem,                &Sentient::EventUseItem                 },
    {&EV_Sentient_Take,                   &Sentient::EventTake                    },
    {&EV_Sentient_TakeAll,                &Sentient::EventFreeInventory           },
    {&EV_Sentient_SetBloodModel,          &Sentient::SetBloodModel                },
    {&EV_Sentient_GiveTargetname,         &Sentient::EventGiveTargetname          },
    {&EV_Sentient_SetWeaponIdleState,     &Sentient::EventSetWeaponIdleState      },
    {&EV_Sentient_PingForMines,           &Sentient::EventPingForMines            },
    {&EV_Sentient_ForceLandmineMeasure,   &Sentient::EventForceLandmineMeasure    },
    {&EV_Damage,                          &Sentient::ArmorDamage                  },
    {&EV_Sentient_TurnOffShadow,          &Sentient::TurnOffShadow                },
    {&EV_Sentient_TurnOnShadow,           &Sentient::TurnOnShadow                 },
    {&EV_Sentient_JumpXY,                 &Sentient::JumpXY                       },
    {&EV_Sentient_MeleeAttackStart,       &Sentient::MeleeAttackStart             },
    {&EV_Sentient_MeleeAttackEnd,         &Sentient::MeleeAttackEnd               },
    {&EV_Sentient_BlockStart,             &Sentient::BlockStart                   },
    {&EV_Sentient_BlockEnd,               &Sentient::BlockEnd                     },
    {&EV_Sentient_StunStart,              &Sentient::StunStart                    },
    {&EV_Sentient_StunEnd,                &Sentient::StunEnd                      },
    {&EV_Sentient_SetMaxMouthAngle,       &Sentient::SetMaxMouthAngle             },
    {&EV_Sentient_OnFire,                 &Sentient::OnFire                       },
    {&EV_Sentient_StopOnFire,             &Sentient::StopOnFire                   },
    {&EV_Sentient_SpawnBloodyGibs,        &Sentient::SpawnBloodyGibs              },
    {&EV_Sentient_SetMaxGibs,             &Sentient::SetMaxGibs                   },
    {&EV_Sentient_CheckAnimations,        &Sentient::CheckAnimations              },
    {&EV_Sentient_German,                 &Sentient::EventGerman                  },
    {&EV_Sentient_American,               &Sentient::EventAmerican                },
    {&EV_Sentient_GetTeam,                &Sentient::EventGetTeam                 },
    {&EV_Sentient_SetDamageMult,          &Sentient::SetDamageMult                },
    {&EV_Sentient_SetupHelmet,            &Sentient::EventSetupHelmet             },
    {&EV_Sentient_PopHelmet,              &Sentient::EventPopHelmet               },
    {&EV_Sentient_GetThreatBias,          &Sentient::EventGetThreatBias           },
    {&EV_Sentient_SetThreatBias,          &Sentient::EventSetThreatBias           },
    {&EV_Sentient_SetThreatBias2,         &Sentient::EventSetThreatBias           },
    {&EV_Sentient_DeactivateWeapon,       &Sentient::EventDeactivateWeapon        },
    {&EV_Sentient_ActivateNewWeapon,      &Sentient::ActivateNewWeapon            },
    {&EV_Sentient_PutawayWeapon,          &Sentient::PutawayWeapon                },
    {&EV_Sentient_Weapon,                 &Sentient::WeaponCommand                },
    {&EV_Sentient_UseWeaponClass,         &Sentient::EventUseWeaponClass          },
    {&EV_Sentient_UseLastWeapon,          &Sentient::EventActivateLastActiveWeapon},
    {&EV_Sentient_ToggleItemUse,          &Sentient::EventToggleItemUse           },
    {&EV_Sentient_DropItems,              &Sentient::EventDropItems               },
    {&EV_Sentient_DontDropWeapons,        &Sentient::EventDontDropWeapons         },
    {&EV_Sentient_ForceDropHealth,        &Sentient::EventForceDropHealth         },
    {&EV_Sentient_ForceDropHealth2,       &Sentient::EventForceDropHealth         },
    {&EV_Sentient_GetForceDropHealth,     &Sentient::EventGetForceDropHealth      },
    {&EV_Sentient_ForceDropWeapon,        &Sentient::EventForceDropWeapon         },
    {&EV_Sentient_ForceDropWeapon2,       &Sentient::EventForceDropWeapon         },
    {&EV_Sentient_GetForceDropWeapon,     &Sentient::EventGetForceDropWeapon      },

    {&EV_Sentient_GetActiveWeap,          &Sentient::GetActiveWeap                },
    {&EV_Sentient_GetNewActiveWeap,       &Sentient::GetNewActiveWeaponOld        },
    {&EV_Sentient_GetNewActiveWeapon,     &Sentient::GetNewActiveWeapon           },
    {&EV_Sentient_GetNewActiveWeaponHand, &Sentient::GetNewActiveWeaponHand       },
    {&EV_Sentient_Client_Landing,         &Sentient::EventClientLanding           },
    {&EV_Sentient_CoopGorePoolGrow,       &Sentient::EventCoopGorePoolGrow        }, // HZM coop - gore tier 2
    {&EV_Sentient_CoopGoreReset,          &Sentient::EventCoopGoreReset           }, // HZM coop - gore tier 1
    {&EV_Sentient_CoopGoreGibMark,        &Sentient::EventCoopGoreGibMark         }, // HZM coop - gore tier 1e
    {&EV_Sentient_CoopBlastShield,        &Sentient::EventCoopBlastShield         }, // HZM coop - bug-1586
    {NULL,                                NULL                                    }
};

Container<Sentient *> SentientList;

void Sentient::EventGiveDynItem(Event *ev)
{
    str      tikiname;
    int      tagnum;
    Vector   offset;
    DynItem *item;

    item                = new DynItem();
    tikiname            = ev->GetString(1);
    item->m_attachPrime = ev->GetString(2);

    item->setModel(tikiname);
    tagnum = gi.Tag_NumForName(edict->tiki, item->m_attachPrime.c_str());
    if (tagnum >= 0 && !item->attach(entnum, tagnum, qtrue, offset)) {
        // invalid tagnum
        delete item;
        return;
    }

    item->setSolidType(SOLID_BBOX);
    item->setMoveType(MOVETYPE_BOUNCE);
    item->takedamage = DAMAGE_YES;
    item->ProcessPendingEvents();
}

Sentient::Sentient()
    : mAccuracy(0.2f)
    , m_bIsAnimal(false)
{
    SentientList.AddObject((Sentient *)this);
    entflags |= ECF_SENTIENT;

    m_bOvercookDied = false;

    if (LoadingSavegame) {
        return;
    }

    viewheight              = 0;
    means_of_death          = MOD_NONE;
    LMRF                    = 0;
    in_melee_attack         = false;
    in_block                = false;
    in_stun                 = false;
    on_fire                 = 0;
    on_fire_stop_time       = 0;
    next_catch_on_fire_time = 0;
    on_fire_tagnums[0]      = -1;
    on_fire_tagnums[1]      = -1;
    on_fire_tagnums[2]      = -1;
    attack_blocked_time     = 0;
    m_fHelmetSpeed          = 0;
    m_fNextBloodTrailTime   = 0;            // HZM coop - blood trail
    m_fCoopBloodSeverity    = 0;            // HZM coop [user 07-29] - blood-trail severity scale
    m_vLastBloodTrailOrigin = vec_zero;     // HZM coop - blood trail
    m_fCoopGoreDamage       = 0;            // HZM coop - gore tier 2 (drips + growing pool)
    m_iCoopGoreSkinTier     = 0;            // HZM coop - gore tier 1 (damage-tier blood skins)
    m_bCoopGoreGibMark      = qfalse;
    m_bCoopHeadGore         = qfalse;        // HZM coop [user 2026-08-17] - headshot face disfigurement
    m_iCoopWoundNext        = 0;            // HZM coop [user 2026-08-17] - wound-prop recycle cursor
    m_bCoopBlastShield      = qfalse;       // HZM coop - bug-1586: opt-in, NOT team-wide (see TakeDamage)
    m_fCoopGoreGibMarkTime  = 0;            // HZM coop - gore tier 1e
    m_vCoopPoolPos          = vec_zero;     // HZM coop - gore tier 2
    m_vCoopPoolNormal       = vec_zero;     // HZM coop - gore tier 2
    m_iCoopPoolGen          = 0;            // HZM coop - gore tier 2 (bug-817: continuous pool growth)

    inventory.ClearObjectList();

    m_pNextSquadMate = this;
    m_pPrevSquadMate = this;

    m_Enemy.Clear();

    m_fPlayerSightLevel = 0;
    newWeapon           = NULL;

    eyeposition       = Vector(0, 0, 64);
    charge_start_time = 0;
    poweruptype       = 0;
    poweruptimer      = 0;
    // do better lighting on all sentients
    edict->s.renderfx |= RF_EXTRALIGHT;
    edict->s.renderfx |= RF_SHADOW;
    // sentients have precise shadows
    edict->s.renderfx |= RF_SHADOW_PRECISE;

    m_vViewVariation = Vector(0, 0, 0);
    for (int i = 0; i < MAX_ACTIVE_WEAPONS; i++) {
        activeWeaponList[i] = NULL;
    }

    in_melee_attack = false;
    in_block        = false;
    in_stun         = false;
    attack_blocked  = qfalse;
    max_mouth_angle = 10;

    // touch triggers by default
    flags |= FL_TOUCH_TRIGGERS;

    on_fire         = false;
    max_gibs        = 0;
    next_bleed_time = 0;

    ClearNewActiveWeapon();
    newActiveWeapon.weapon    = NULL;
    holsteredWeapon           = NULL;
    weapons_holstered_by_code = false;
    lastActiveWeapon.weapon   = NULL;
    edict->s.eFlags |= EF_UNARMED;

    m_pVehicle.Clear();
    m_pTurret.Clear();
    m_pLadder.Clear();
    m_iAttackerCount = 0;
    m_pLastAttacker.Clear();

    m_bIsDisguised        = false;
    m_bHasDisguise        = false;
    m_ShowPapersTime      = 0;
    m_iLastHitTime        = 0;
    m_Team                = TEAM_AMERICAN;
    m_iThreatBias         = 0;
    m_bFootOnGround_Right = true;
    m_bFootOnGround_Left  = true;
    iNextLandTime         = 0;
    m_bDontDropWeapons    = false;

    if (g_realismmode->integer) {
        m_fDamageMultipliers[HITLOC_HEAD]        = 5.0f;
        m_fDamageMultipliers[HITLOC_HELMET]      = 5.0f;
        m_fDamageMultipliers[HITLOC_NECK]        = 5.0f;
        m_fDamageMultipliers[HITLOC_TORSO_UPPER] = 1.0f;
        m_fDamageMultipliers[HITLOC_TORSO_MID]   = 0.95f;
        m_fDamageMultipliers[HITLOC_TORSO_LOWER] = 0.9f;
        m_fDamageMultipliers[HITLOC_PELVIS]      = 0.85f;
    } else {
        m_fDamageMultipliers[HITLOC_HEAD]        = 4.0f;
        m_fDamageMultipliers[HITLOC_HELMET]      = 4.0f;
        m_fDamageMultipliers[HITLOC_NECK]        = 4.0f;
        m_fDamageMultipliers[HITLOC_TORSO_UPPER] = 1.0f;
        m_fDamageMultipliers[HITLOC_TORSO_MID]   = 1.0f;
        m_fDamageMultipliers[HITLOC_TORSO_LOWER] = 1.0f;
        m_fDamageMultipliers[HITLOC_PELVIS]      = 0.9f;
    }

    m_fDamageMultipliers[HITLOC_R_ARM_UPPER] = 0.8f;
    m_fDamageMultipliers[HITLOC_L_ARM_UPPER] = 0.8f;
    m_fDamageMultipliers[HITLOC_R_LEG_UPPER] = 0.8f;
    m_fDamageMultipliers[HITLOC_L_LEG_UPPER] = 0.8f;
    m_fDamageMultipliers[HITLOC_R_ARM_LOWER] = 0.6f;
    m_fDamageMultipliers[HITLOC_L_ARM_LOWER] = 0.6f;
    m_fDamageMultipliers[HITLOC_R_LEG_LOWER] = 0.6f;
    m_fDamageMultipliers[HITLOC_L_LEG_LOWER] = 0.6f;
    m_fDamageMultipliers[HITLOC_R_HAND]      = 0.5f;
    m_fDamageMultipliers[HITLOC_L_HAND]      = 0.5f;
    m_fDamageMultipliers[HITLOC_R_FOOT]      = 0.5f;
    m_fDamageMultipliers[HITLOC_L_FOOT]      = 0.5f;

    m_PrevSentient = m_NextSentient = NULL;
    m_bForceDropHealth              = false;
    m_bForceDropWeapon              = false;

    Link();
}

Sentient::~Sentient()
{
    Unlink();
    DisbandSquadMate(this);

    SentientList.RemoveObject((Sentient *)this);
    FreeInventory();

    entflags &= ~ECF_SENTIENT;
}

void Sentient::Link()
{
    m_PrevSentient = NULL;
    m_NextSentient = level.m_HeadSentient[m_Team];
    if (m_NextSentient) {
        m_NextSentient->m_PrevSentient = this;
    }
    level.m_HeadSentient[m_Team] = this;
}

void Sentient::Unlink()
{
    if (m_NextSentient) {
        m_NextSentient->m_PrevSentient = m_PrevSentient;
    }
    if (m_PrevSentient) {
        m_PrevSentient->m_NextSentient = m_NextSentient;
    } else {
        level.m_HeadSentient[this->m_Team] = m_NextSentient;
    }

    m_NextSentient = m_PrevSentient = NULL;
}

Vector Sentient::EyePosition(void)
{
    return origin + eyeposition;
}

void Sentient::SetBloodModel(Event *ev)
{
    // HZM coop [user 2026-08-17] - decap assets are registered HERE, on the spawn path, so the
    // first decapitation of a map does not pay a registration spike mid-firefight (bug-856).
    CacheResource("models/fx/coop_stump_neck.tik");
    str name;
    str cache_name;
    str models_dir = "models/";

    if (ev->NumArgs() < 1) {
        return;
    }

    blood_model = ev->GetString(1);
    cache_name  = models_dir + blood_model;
    CacheResource(cache_name.c_str());

    name = GetBloodSpurtName();
    if (name.length()) {
        cache_name = models_dir + name;
        CacheResource(cache_name.c_str());
    }

    name = GetBloodSplatName();
    if (name.length()) {
        CacheResource(name.c_str());
    }

    name = GetGibName();
    if (name.length()) {
        cache_name = models_dir + name;
        CacheResource(cache_name.c_str());
    }
}

void Sentient::AddItem(Item *object)
{
    // HZM 07-19 (bug-920): refuse duplicate entnums - RemoveItem/~Item remove only ONE
    // occurrence, so a double-add leaves a permanently stale entry that dangles once the
    // item entity is freed (the producer shape behind the bug-915/917/919 crash family).
    if (inventory.IndexOfObject(object->entnum)) {
        // HZM bug-924: membership-safe refusal (container holds raw entnums, so the listed int
        // already resolves to this new entity) - but print loudly to timestamp producer activity.
        gi.DPrintf("^~^~^ INVDUP add refused ent=%d model=%s owner=%d\n", object->entnum, object->model.c_str(), entnum);
        return;
    }
    inventory.AddObject(object->entnum);
}

void Sentient::RemoveItem(Item *object)
{
    if (!inventory.IndexOfObject(object->entnum)) {
        return;
    }

    inventory.RemoveObject(object->entnum);

    if (object->IsSubclassOfWeapon()) {
        DeactivateWeapon((Weapon *)object);
    }

    //
    // let the sent know about it
    //
    RemovedItem(object);
}

void Sentient::RemoveWeapons(void)
{
    for (int i = inventory.NumObjects(); i > 0; i--) {
        int     entnum = inventory.ObjectAt(i);
        Weapon *item   = (Weapon *)G_GetEntity(entnum);

        // HZM 07-19 (bug-915): a stale inventory entnum (item freed without owner cleanup) made
        // these unguarded derefs crash - live dump: AV read at Sentient::FindItem+0x95 while using
        // the mine detector. Release builds compile the asserts out, so skip dead slots instead.
        if (item && item->IsSubclassOfWeapon()) {
            item->Delete();
        }
    }
}

Weapon *Sentient::GetWeapon(int index)
{
    for (int i = inventory.NumObjects(); i > 0; i--) {
        int     entnum = inventory.ObjectAt(i);
        Weapon *item   = (Weapon *)G_GetEntity(entnum);

        if (item && item->IsSubclassOfWeapon()) {
            if (!index) {
                return item;
            }

            index--;
        }
    }

    return NULL;
}

Item *Sentient::FindItemByExternalName(const char *itemname)
{
    int   num;
    int   i;
    Item *item;

    num = inventory.NumObjects();
    for (i = 1; i <= num; i++) {
        item = (Item *)G_GetEntity(inventory.ObjectAt(i));
        assert(item);
        if (!item || !item->isSubclassOf(Item)) {
            continue; // HZM 07-19 (bug-915/919): stale OR RECYCLED slot (entnum reused by a non-Item under blast churn - live dump: wild read at FindItem+0xad, addr -1)
        }
        if (!Q_stricmp(item->getName(), itemname)) {
            return item;
        }
    }

    return NULL;
}

Item *Sentient::FindItemByModelname(const char *mdl)
{
    int   num;
    int   i;
    Item *item;
    str   tmpmdl;

    if (Q_stricmpn("models/", mdl, 7)) {
        tmpmdl = "models/";
    }
    tmpmdl += mdl;

    num = inventory.NumObjects();
    for (i = 1; i <= num; i++) {
        item = (Item *)G_GetEntity(inventory.ObjectAt(i));
        assert(item);
        if (!item || !item->isSubclassOf(Item)) {
            continue; // HZM 07-19 (bug-915/919): stale or recycled slot
        }
        if (!Q_stricmp(item->model, tmpmdl)) {
            return item;
        }
    }

    return NULL;
}

Item *Sentient::FindItemByClassName(const char *classname)
{
    int   num;
    int   i;
    Item *item;

    num = inventory.NumObjects();
    for (i = 1; i <= num; i++) {
        item = (Item *)G_GetEntity(inventory.ObjectAt(i));
        assert(item);
        if (!item || !item->isSubclassOf(Item)) {
            continue; // HZM 07-19 (bug-915/919): stale or recycled slot
        }
        if (!Q_stricmp(item->edict->entname, classname)) {
            return item;
        }
    }

    return NULL;
}

// HZM 07-20 (bug-924): stale slots (entity freed while listed; producer at large -
// bug-915/917/919/920/925 family) were only SKIPPED by the guards, so they accumulate:
// walks hide them ("missing" weapons in cycling), FindItem misses so later re-gives spawn
// duplicates, and any unswept walk crashes. Heal: remove every slot that no longer
// resolves to an Item, loudly, so the next incident timestamps the producer.
void Sentient::PruneStaleInventory(void)
{
    for (int i = inventory.NumObjects(); i > 0; i--) {
        Entity *e = G_GetEntity(inventory.ObjectAt(i));
        if (!e || !e->isSubclassOf(Item)) {
            gi.DPrintf("^~^~^ INVSTALE pruned ent=%d slot=%d owner=%d\n", inventory.ObjectAt(i), i, entnum);
            inventory.RemoveObjectAt(i);
        }
    }
}

Item *Sentient::FindItem(const char *itemname)
{
    Item *item;

    PruneStaleInventory(); // HZM bug-924: heal before searching

    item = FindItemByExternalName(itemname);
    if (!item) {
        item = FindItemByModelname(itemname);
        if (!item) {
            item = FindItemByClassName(itemname);
        }
    }
    return item;
}

void Sentient::FreeInventory(void)
{
    int   num;
    int   i;

    PruneStaleInventory(); // HZM bug-924
    Item *item;
    Ammo *ammo;

    // Detach all Weapons
    DetachAllActiveWeapons();

    // Delete all inventory items ( this includes weapons )
    num = inventory.NumObjects();
    for (i = num; i > 0; i--) {
        item = (Item *)G_GetEntity(inventory.ObjectAt(i));
        // HZM 07-19 (bug-919): same stale/recycled-slot guard as FindItem
        if (item && item->isSubclassOf(Item)) {
            item->Delete();
        }
    }
    inventory.ClearObjectList();

    // Remove all ammo
    num = ammo_inventory.NumObjects();
    for (i = num; i > 0; i--) {
        ammo = (Ammo *)ammo_inventory.ObjectAt(i);
        delete ammo;
    }
    ammo_inventory.ClearObjectList();

    if (IsSubclassOfPlayer()) {
        ((Player *)this)->InitMaxAmmo();
    }
}

void Sentient::EventFreeInventory(Event *ev)
{
    FreeInventory();
}

qboolean Sentient::HasItem(const char *itemname)
{
    return (FindItem(itemname) != NULL);
}

qboolean Sentient::HasWeaponClass(int iWeaponClass)
{
    int     i;
    Weapon *weapon;

    // look up for a weapon class
    for (i = 1; i <= inventory.NumObjects(); i++) {
        weapon = (Weapon *)G_GetEntity(inventory.ObjectAt(i));

        if (weapon && weapon->IsSubclassOfWeapon()) {
            if (weapon->GetWeaponClass() & iWeaponClass) {
                // weapon class found
                return qtrue;
            }
        }
    }

    return qfalse;
}

qboolean Sentient::HasPrimaryWeapon(void)
{
    int     i;
    Weapon *weapon;

    // look up for a primary weapon
    for (i = 1; i <= inventory.NumObjects(); i++) {
        weapon = (Weapon *)G_GetEntity(inventory.ObjectAt(i));

        if (weapon && weapon->IsSubclassOfWeapon()) {
            if (!(weapon->GetWeaponClass() & WEAPON_CLASS_MISC) && !weapon->IsSecondaryWeapon()) {
                // Sentient has a primary weapon
                return qtrue;
            }
        }
    }

    return qfalse;
}

qboolean Sentient::HasSecondaryWeapon(void)
{
    int     i;
    Weapon *weapon;

    // look up for a secondary weapon
    for (i = 1; i <= inventory.NumObjects(); i++) {
        weapon = (Weapon *)G_GetEntity(inventory.ObjectAt(i));

        if (weapon && weapon->IsSubclassOfWeapon()) {
            if (weapon->IsSecondaryWeapon()) {
                // Sentient has a secondary weapon
                return qtrue;
            }
        }
    }

    return qfalse;
}

void Sentient::EventGiveTargetname(Event *ev)
{
    int            i;
    str            name;
    qboolean       found;
    ScriptVariable var;
    SimpleEntity  *ent;

    var = ev->GetValue(1);
    var.CastConstArrayValue();

    for (i = var.arraysize(); i > 0; i--) {
        const ScriptVariable *variable = var[i];
        ent                            = variable->simpleEntityValue();
        if (ent && ent->IsSubclassOfItem()) {
            Item *item;

            item = (Item *)ent;
            item->SetOwner(this);
            item->ProcessPendingEvents();
            AddItem(item);
            found = qtrue;
        }
    }

    if (!found) {
        ScriptError("Could not give item with targetname %s to this sentient.\n", name.c_str());
    }
}

Item *Sentient::giveItem(str itemname, int amount)
{
    ClassDef *cls;
    Item     *item;

    item = FindItem(itemname);
    if (item) {
        item->Add(amount);
        return item;
    } else {
        qboolean set_the_model = qfalse;

        // we don't have it, so lets try to resolve the item name
        // first lets see if it is a registered class name
        cls = getClass(itemname);
        if (!cls) {
            SpawnArgs args;

            // if that didn't work lets try to resolve it as a model
            args.setArg("model", itemname);

            cls = args.getClassDef();
            if (!cls) {
                gi.DPrintf("No item called '%s'\n", itemname.c_str());
                return NULL;
            }
            set_the_model = qtrue;
        }
        assert(cls);
        item = (Item *)cls->newInstance();

        if (!item) {
            gi.DPrintf("Could not spawn an item called '%s'\n", itemname.c_str());
            return NULL;
        }

        if (!item->isSubclassOf(Item)) {
            gi.DPrintf("Could not spawn an item called '%s'\n", itemname.c_str());
            delete item;
            return NULL;
        }

        if (set_the_model) {
            // Set the model
            item->setModel(itemname);
        }

        item->SetOwner(this);
        item->ProcessPendingEvents();
        item->setAmount(amount);

        AddItem(item);

        if (item->IsSubclassOfWeapon()) {
            // Post an event to give the ammo to the sentient
            Event *ev1;

            ev1 = new Event(EV_Weapon_GiveStartingAmmo);
            ev1->AddEntity(this);
            item->PostEvent(ev1, level.frametime);
        }

        return item;
    }
    return NULL;
}

void Sentient::takeItem(const char *name)
{
    Item *item;

    item = FindItem(name);
    if (item) {
        gi.DPrintf("Taking item %s away from player\n", item->getName().c_str());

        item->PostEvent(EV_Remove, 0);
        return;
    }

    Ammo *ammo;
    ammo = FindAmmoByName(name);
    if (ammo) {
        gi.DPrintf("Taking ammo %s away from player\n", name);

        ammo->setAmount(0);
    }
}

void Sentient::takeAmmoType(const char *name)
{
    Ammo *ammo;

    ammo = FindAmmoByName(name);
    if (ammo) {
        gi.DPrintf("Taking ammo %s away from player\n", name);

        ammo->setAmount(0);
    }
}

void Sentient::EventUseItem(Event *ev)
{
    str          name;
    weaponhand_t hand = WEAPON_MAIN;

    if (deadflag) {
        return;
    }

    name = ev->GetString(1);

    if (ev->NumArgs() > 1) {
        hand = WeaponHandNameToNum(ev->GetString(2));
    }

    useWeapon(name, hand);
}

void Sentient::EventTake(Event *ev)
{
    takeItem(ev->GetString(1));
}

void Sentient::EventGiveItem(Event *ev)
{
    str   type;
    float amount;

    type = ev->GetString(1);
    if (ev->NumArgs() > 1) {
        amount = ev->GetInteger(2);
    } else {
        amount = 1;
    }

    giveItem(type, amount);
}

qboolean Sentient::DoGib(int meansofdeath, Entity *inflictor)
{
    if (!com_blood->integer) {
        return false;
    }

    if ((meansofdeath == MOD_TELEFRAG) || (meansofdeath == MOD_LAVA)) {
        return true;
    }

    if (health > -75) {
        return false;
    }

    // Impact and Crush < -75 health
    if ((meansofdeath == MOD_IMPACT) || (meansofdeath == MOD_CRUSH)) {
        return true;
    }

    return false;
}

void Sentient::SpawnEffect(str modelname, Vector pos)
{
    Animate *block;

    block = new Animate;
    block->setModel(modelname);
    block->setOrigin(pos);
    block->setSolidType(SOLID_NOT);
    block->setMoveType(MOVETYPE_NONE);
    block->NewAnim("idle");
    block->PostEvent(EV_Remove, 1);
}

int Sentient::CheckHitLocation(int iLocation)
{
    if (iLocation == 1) {
        if (WearingHelmet()) {
            return iLocation;
        } else {
            return HITLOC_HEAD;
        }
    }

    return iLocation;
}

#define WATER_CONVERSION_FACTOR 1.0f

void Sentient::ArmorDamage(Event *ev)
{
    Entity   *inflictor;
    Sentient *attacker;
    float     damage;
    Vector    momentum;
    Vector    position;
    Vector    normal;
    Vector    direction;
    Event     event;
    int       dflags;
    int       meansofdeath;
    int       knockback;
    int       location;

    //qboolean	blocked;
    float damage_red;
    float damage_green;
    float damage_time;
    //qboolean	set_means_of_death;

    static bool    tmp          = false;
    static cvar_t *AIDamageMult = NULL;

    if (!tmp) {
        tmp          = true;
        AIDamageMult = gi.Cvar_Get("g_aiDamageMult", "1.0", 0);
    }

    if (IsDead()) {
        return;
    }

    attacker     = (Sentient *)ev->GetEntity(1);
    damage       = ev->GetFloat(2);
    inflictor    = ev->GetEntity(3);
    position     = ev->GetVector(4);
    direction    = ev->GetVector(5);
    normal       = ev->GetVector(6);
    knockback    = ev->GetInteger(7);
    dflags       = ev->GetInteger(8);
    meansofdeath = ev->GetInteger(9);
    location     = CheckHitLocation(ev->GetInteger(10));

    if (location == HITLOC_MISS) {
        return;
    }

    if ((takedamage == DAMAGE_NO) || (movetype == MOVETYPE_NOCLIP)) {
        return;
    }

    // HZM coop - SELECTIVE ally protection. The coop OFFICER and all its reinforcements/bodyguards carry the
    // RF_COOP_BOSS render flag. Drop any damage a GERMAN "boss" actor deals to an ALLIED AI actor (an Actor --
    // not a player -- on the american team), so the mission's scripted squad survives the coop boss waves.
    // Normal (untagged) enemies still hurt allies, so a map's "too many casualties" fail can still fire, and
    // players still take boss damage (a Player is not an Actor).
    // FIXED 2026-07-03: the check used to drop damage from ANY boss-flagged attacker to ANY cross-team actor --
    // but allied PARADROP troopers also carry RF_COOP_BOSS (it drives their overhead star icon), so every
    // paratrooper bullet into a german Actor was silently zeroed (ALLYFIRE log: hits=1 dmg=0 -> "paratroopers
    // shooting the shit out of the enemy AI and they aren't dying"). Protection is now DIRECTIONAL: only a
    // german boss attacking an american AI is blocked.
    if (attacker && attacker != this && (attacker->edict->s.renderfx & RF_COOP_BOSS)
        && attacker->m_Team == TEAM_GERMAN && IsSubclassOfActor() && m_Team == TEAM_AMERICAN) {
        return;
    }

    // HZM coop - script blast shield for allied mission NPCs. The player-callable bombing run delivers its
    // damage via the script radiusdamage command, which attributes the explosion to WORLD (see
    // ScriptThread::EventRadiusDamage: RadiusDamage(origin, world, world, ..., MOD_EXPLOSION, ...)). Allied
    // escort actors have ~100hp, so one 600-damage bomb instantly killed mission-critical NPCs -- on t1l3 the
    // captain's death fires missionfailed and the private/colonel deaths break the ride/balcony gags ("bombing
    // run killed the colonel"). Drop world-attributed explosion damage to AMERICAN AI actors in coop only;
    // players and german AI still take full blast damage, and weapon/grenade explosions (attributed to their
    // owner) still hurt allies.
    // [user 08-08] bug-1586 - NARROWED from "every allied actor" to "actors that opted in". The old
    // blanket test made the whole allied squad immune to every world-attributed blast, which is why
    // mortars and artillery could not wound, gib or even scratch them - the damage never arrived, so
    // no gore path ever ran. That was far broader than the defect it was written for. Mission-critical
    // NPCs now set `blastshield 1` and keep exactly the old protection; ordinary allied AI take the
    // blast, bleed and gib, and via coop_mod/allysquad.scr go DOWN rather than die outright - so
    // losing them is recoverable instead of instant.
    if (g_gametype->integer != GT_SINGLE_PLAYER && meansofdeath == MOD_EXPLOSION && IsSubclassOfActor()
        && m_Team == TEAM_AMERICAN && (!attacker || (Entity *)attacker == (Entity *)world)) {
        return;
    }

    if ((!isClient() || g_gametype->integer != GT_SINGLE_PLAYER)
        && (location > HITLOC_GENERAL && location < NUMBODYLOCATIONS)) {
        damage *= m_fDamageMultipliers[location];
    } else if (isClient() && attacker && attacker->IsSubclassOfActor() && g_gametype->integer == GT_SINGLE_PLAYER) {
        damage *= AIDamageMult->value;
    }

    // See if sentient is immune to this type of damage
    if (Immune(meansofdeath)) {
        /*
        means_of_death = meansofdeath;

        // Send pain event
        event = new Event( EV_Pain );
        event->AddEntity( attacker );
        event->AddFloat( 0 );
        event->AddVector( position );
        event->AddVector( direction );
        event->AddVector( normal );
        event->AddInteger( knockback );
        event->AddInteger( dflags );
        event->AddInteger( meansofdeath );
        event->AddInteger( location );

        ProcessEvent( event );
*/
        return;
    }

    // See if the damage is melee and high enough on actor

    /*
    if( deadflag )
    {
        // Spawn a blood spurt if this model has one
        if( ShouldBleed( meansofdeath, true ) )
        {
            AddBloodSpurt( direction );

            if( ShouldGib( meansofdeath, damage ) )
                ProcessEvent( EV_Sentient_SpawnBloodyGibs );
        }

        means_of_death = meansofdeath;

        if( meansofdeath == MOD_FIRE )
            TryLightOnFire( meansofdeath, attacker );

        // Send pain event
        event = new Event( EV_Pain );
        event->AddEntity( attacker );
        event->AddFloat( damage );
        event->AddVector( position );
        event->AddVector( direction );
        event->AddVector( normal );
        event->AddInteger( knockback );
        event->AddInteger( dflags );
        event->AddInteger( meansofdeath );
        event->AddInteger( location );

        ProcessEvent( event );

        return;
    }
*/

    // Do the kick
    if (!(dflags & DAMAGE_NO_KNOCKBACK)) {
        if ((knockback) && (movetype != MOVETYPE_NONE) && (movetype != MOVETYPE_STATIONARY)
            && (movetype != MOVETYPE_BOUNCE) && (movetype != MOVETYPE_PUSH) && (movetype != MOVETYPE_STOP)) {
            float  m;
            Event *immunity_event;

            if (mass < 20) {
                m = 20;
            } else {
                m = mass;
            }

            direction.normalize();
            if (isClient() && (attacker == this) && deathmatch->integer) {
                momentum = direction * (1700.0f * (float)knockback / m); // the rocket jump hack...
            } else {
                momentum = direction * (500.0f * (float)knockback / m);
            }

            if (dflags & DAMAGE_BULLET) {
                // Clip the z velocity for bullet weapons
                if (momentum.z > 75) {
                    momentum.z = 75;
                }
            }
            velocity += momentum;

            // Make this sentient vulnerable to falling damage now

            if (Immune(MOD_FALLING)) {
                immunity_event = new Event(EV_Entity_RemoveImmunity);
                immunity_event->AddString("falling");
                ProcessEvent(immunity_event);
            }
        }
    }

    if (g_debugdamage->integer) {
        G_DebugDamage(damage, this, attacker, inflictor);
    }

    // COOP: same-team damage is filtered in ALL gametypes (was SP-only) so the officer's
    // reinforcements/bodyguards can't kill each other or the officer, and coop teammates don't friendly-fire.
    float fCoopPrevHealth = health; // HZM coop - headshot-kill confirm reads the alive->dead edge below
    if (!(flags & FL_GODMODE)
        && (!(attacker) || (attacker) == this
            || !(attacker->IsSubclassOfSentient()) || (attacker->m_Team != m_Team))) {
        health -= damage;
        // HZM coop - gore tier 2: accumulate APPLIED damage only. Gore tiers key on this, never on health
        // fraction, because aihandler.scr fakes rank-and-file AI health at 5000 (real HP lives script-side).
        m_fCoopGoreDamage += damage;
        CoopGoreUpdateSkinTier(); // HZM coop - gore tier 1: bloody the uniform as damage accumulates
        CoopGoreTryWoundProp(location, meansofdeath, position); // HZM coop - gore tier 3: wound prop at the hit point
    }

    // Set means of death
    means_of_death = meansofdeath;

    /*
    // Spawn a blood spurt if this model has one
    if( ShouldBleed( meansofdeath, false ) && !blocked )
    {
        AddBloodSpurt( direction );

        if( ( this->isSubclassOf( Actor ) || damage > 10 ) && ShouldGib( meansofdeath, damage ) )
            ProcessEvent( EV_Sentient_SpawnBloodyGibs );
    }
*/

    if (health <= 0) {
        // See if we can kill this actor or not

        if (this->IsSubclassOfActor()) {
            Actor *act = (Actor *)this;

            if (act->IsImmortal()) {
                health = 1;
            }
        }
    }

    // HZM coop - CONFIRMED HEADSHOT KILL (cue + guaranteed visible feedback). Lives HERE, not in
    // BulletAttack, because rank-and-file AI carry the aihandler 5000-health buffer: the player's
    // bullet only WOUNDS them engine-side and the real killing blow is the pain handler's scripted
    // overkill (aihandler.scr::handlePain), which preserves attacker/position/direction/MOD/location
    // and arrives through this same event - the old BulletAttack hook could never see those kills.
    // Engine-side kills (buffer-less sentients, dogs) pass through here too, so this is the single
    // choke point; IsDead() at the top makes any later script overkill on the same corpse a no-op,
    // and the same-team damage filter above means an allied victim never reaches health <= 0.
    if (fCoopPrevHealth > 0 && health <= 0 && attacker && attacker->IsSubclassOfPlayer()
        && !IsSubclassOfPlayer()
        && (meansofdeath == MOD_BULLET || meansofdeath == MOD_FAST_BULLET || meansofdeath == MOD_SHOTGUN)
        && (location == HITLOC_HEAD || location == HITLOC_HELMET || location == HITLOC_NECK)) {
        attacker->Sound("coop_headshot", CHAN_LOCAL);
        CoopHeadshotKillFx(position, direction);
        CoopGoreDisfigureHead(); // HZM coop [user 2026-08-17] - and leave the face unrecognisable
    }

    if (meansofdeath == MOD_SLIME) {
        damage_green = damage / 50;
        if (damage_green > 1.0f) {
            damage_green = 1.0f;
        }
        if ((damage_green < 0.2) && (damage_green > 0)) {
            damage_green = 0.2f;
        }
        damage_red = 0;
    } else {
        damage_red = damage / 50;
        if (damage_red > 1.0f) {
            damage_red = 1.0f;
        }
        if ((damage_red < 0.2) && (damage_red > 0)) {
            damage_red = 0.2f;
        }
        damage_green = 0;
    }

    damage_time = damage / 50;

    if (damage_time > 2) {
        damage_time = 2;
    }

    //SetOffsetColor(damage_red, damage_green, 0, damage_time);

    if (health < 0.1) {
        // Make sure health is now 0

        health = 0;

        // HZM coop - gore tier 1 (bug-735): the KILLING BLOW always leaves a heavy-tier corpse. Without
        // this, enemies that died before crossing the accumulated-damage gates (fast TTK is the norm)
        // kept a clean uniform. Saturating the counter and re-tiering routes through every existing gate
        // (com_blood / coop_goreSkins / bleedable check) inside CoopGoreUpdateSkinTier.
        m_fCoopGoreDamage = 999999.0f;
        CoopGoreUpdateSkinTier();

        // HZM coop - gore tier 1e: an EXPLOSION kill (native explosive MOD, direct projectile impact,
        // or a fresh script gore_gibmark) upgrades the corpse from the heavy tier to the extreme
        // gib-splatter skins (index 3), with a per-corpse random coverage pattern.
        CoopGoreTryGibSkins(meansofdeath, inflictor);

        // HZM coop [user 2026-08-17] - and it may take the head clean off (chance-gated,
        // per-frame budgeted). Runs AFTER the gib skins so the severed head inherits them.
        CoopGoreTryDecapitate(meansofdeath, inflictor);

        DropBloodPool(); // HZM coop - leave a persistent blood pool under the body where it dies

        CoopGoreTryDripAttach(qtrue); // HZM coop - gore tier 2: short full-rate bleed-out drip on the corpse

        if (attacker) {
            const EntityPtr attackerPtr = attacker;

            // Added in OPM
            event = Event(EV_GotKill);
            event.AddEntity(this);
            event.AddInteger(damage);
            event.AddEntity(inflictor);
            event.AddInteger(meansofdeath);
            event.AddInteger(0);

            attackerPtr->ProcessEvent(event);
            if (attackerPtr) {
                attackerPtr->delegate_gotKill.Execute(event);
            }
        }

        event = Event(EV_Killed, 10);
        event.AddEntity(attacker);
        event.AddFloat(damage);
        event.AddEntity(inflictor);
        event.AddVector(position);
        event.AddVector(direction);
        event.AddVector(normal);
        event.AddInteger(knockback);
        event.AddInteger(dflags);
        event.AddInteger(meansofdeath);
        event.AddInteger(location);

        ProcessEvent(event);
        delegate_killed.Execute(event);
    }

    if (health > 0) {
        // Send pain event
        event = Event(EV_Pain, 10);
        event.AddEntity(attacker);
        event.AddFloat(damage);
        event.AddEntity(inflictor);
        event.AddVector(position);
        event.AddVector(direction);
        event.AddVector(normal);
        event.AddInteger(knockback);
        event.AddInteger(dflags);
        event.AddInteger(meansofdeath);
        event.AddInteger(location);

        ProcessEvent(event);

        CoopGoreTryDripAttach(qfalse); // HZM coop - gore tier 2: looping slow drip once wounded enough
    }

    delegate_damage.Execute(*ev);
}

qboolean Sentient::CanBlock(int meansofdeath, qboolean full_block)
{
    // Check to see what a full block can't even block

    switch (meansofdeath) {
    case MOD_TELEFRAG:
    case MOD_SLIME:
    case MOD_LAVA:
    case MOD_FALLING:
    case MOD_IMPALE:
    case MOD_ON_FIRE:
    case MOD_ELECTRICWATER:
        return false;
    }

    // Full blocks block everything else

    if (full_block) {
        return true;
    }

    // Check to see what a small block can't block

    switch (meansofdeath) {
    case MOD_FIRE:
    case MOD_CRUSH_EVERY_FRAME:
        return false;
    }

    // Everything else is blocked

    return true;
}

void Sentient::AddBloodSpurt(Vector direction)
{
    Entity *blood;
    Vector  dir;
    Event  *event;
    str     blood_splat_name;
    float   blood_splat_size;
    float   length;
    trace_t trace;
    float   scale;

    if (!com_blood->integer) {
        return;
    }

    next_bleed_time = level.time + .5;

    // Calculate a good scale for the blood

    if (mass < 50) {
        scale = .5;
    } else if (mass > 300) {
        scale = 1.5;
    } else if (mass >= 200) {
        scale = mass / 200.0;
    } else {
        scale = .5 + (mass - 50) / 300;
    }

    // Add blood spurt

    blood = new Animate;
    blood->setModel(blood_model);

    dir[0]        = -direction[0];
    dir[1]        = -direction[1];
    dir[2]        = -direction[2];
    blood->angles = dir.toAngles();
    blood->setAngles(blood->angles);

    blood->setOrigin(centroid);
    blood->origin.copyTo(blood->edict->s.origin2);
    blood->setSolidType(SOLID_NOT);
    blood->setScale(scale);

    event = new Event(EV_Remove);
    blood->PostEvent(event, 1);

    // Add blood splats near feet

    blood_splat_name = GetBloodSplatName();
    blood_splat_size = GetBloodSplatSize();

    if (blood_splat_name.length() && G_Random() < 0.5) {
        dir = origin - centroid;
        dir.z -= 50;
        dir.x += G_CRandom(20);
        dir.y += G_CRandom(20);

        length = dir.length();

        dir.normalize();

        dir = dir * (length + 10);

        trace = G_Trace(centroid, vec_zero, vec_zero, centroid + dir, NULL, MASK_DEADSOLID, false, "AddBloodSpurt");

        if (trace.fraction < 1) {
            Decal *decal = new Decal;
            decal->setShader("coop_bloodsplat"); // our depth-biased (polygonOffset) red mark - no z-fight flicker
            // tint the ground splat (else it renders white - same fix as the blood trail).
            if (blood_splat_name == "greensplat.spr") {
                decal->setColor(0.15f, 0.45f, 0.12f);
            } else if (blood_splat_name == "bluesplat.spr") {
                decal->setColor(0.12f, 0.20f, 0.55f);
            } else {
                decal->setColor(0.50f, 0.03f, 0.03f);
            }
            decal->setOrigin(Vector(trace.endpos) + (Vector(trace.plane.normal) * 0.2f));
            decal->setDirection(trace.plane.normal);
            decal->setOrientation("random");
            decal->setRadius(blood_splat_size + G_Random(blood_splat_size));
        }
    }
}

// HZM coop - GUARANTEED HEADSHOT-KILL FEEDBACK (the "confirmed headshot but no visible gore" fix).
// Each gore channel can individually miss on a headshot kill: the UV wound stamp needs the client
// pose to match the server segment (movers miss the skin snap) and a stamp on a WORN HELMET pops
// off with the helmet; the helmet pop needs headgear and hitloc 0/1 (neck kills never pop); the
// blood pool grows slowly at the feet. So the confirmed kill itself - the same alive->dead edge
// that plays the coop_headshot cue - spawns one unmissable server-authoritative burst: the
// flesh-hit blood tik at the wound (the HRRTM blood addon ships a rich streaks+splat override of
// bh_human_uniform_hard) plus a persistent coop_bloodsplat mark on whatever surface sits behind
// the head along the bullet path (the classic wall splat). Entities are short-lived (1s Animate;
// a Decal is a 1-frame self-removing edict) and per-frame budgeted (bug-866 decap lesson: cap
// per-death spawns even when they look player-paced).
void Sentient::CoopHeadshotKillFx(const Vector &pos, const Vector &dir)
{
    static cvar_t *pOn = NULL, *pDist = NULL, *pSize = NULL, *pDbg = NULL;
    static float   fFrameTime = -1.0f;
    static int     iFrameCount;
    Animate       *burst;
    Vector         vPos;
    Vector         vDir;
    int            iSplat = 0;

    if (!pOn) {
        pOn   = gi.Cvar_Get("coop_headshotFx", "1", CVAR_ARCHIVE);
        pDist = gi.Cvar_Get("coop_headshotFxSplatDist", "140", CVAR_ARCHIVE);
        pSize = gi.Cvar_Get("coop_headshotFxSplatSize", "16", CVAR_ARCHIVE);
        pDbg  = gi.Cvar_Get("coop_goreDebug", "0", 0);
    }
    if (!com_blood->integer || !pOn->integer) {
        return;
    }

    if (fFrameTime != level.time) {
        fFrameTime  = level.time;
        iFrameCount = 0;
    }
    if (iFrameCount >= 4) {
        return; // per-frame budget - headshot kills are player-paced, this is belt-and-braces
    }
    iFrameCount++;

    // scripted damage may carry a zero position/direction - fall back to head-height centroid / down
    vPos = pos;
    if (vPos == vec_zero) {
        vPos = centroid + Vector(0, 0, maxs.z * 0.3f);
    }
    vDir = dir;
    if (vDir.length() < 0.1f) {
        vDir = Vector(0, 0, -1);
    }
    vDir.normalize();

    burst = new Animate;
    burst->setModel("models/fx/bh_human_uniform_hard.tik");
    burst->setSolidType(SOLID_NOT);
    burst->setOrigin(vPos);
    {
        Vector vBack(-vDir.x, -vDir.y, -vDir.z);
        burst->setAngles(vBack.toAngles()); // effect sprays back toward the shooter (AddBloodSpurt convention)
    }
    burst->PostEvent(EV_Remove, 1);

    if (pDist->value > 1.0f) {
        trace_t splat = G_Trace(
            vPos + vDir * 4.0f, vec_zero, vec_zero, vPos + vDir * pDist->value, this, MASK_DEADSOLID, false,
            "CoopHeadshotKillFx"
        );
        if (splat.fraction < 1.0f && !splat.startsolid) {
            Decal *decal = new Decal;
            decal->setShader("coop_bloodsplat");
            decal->setColor(0.50f, 0.03f, 0.03f); // the mod's fresh-blood tint (see AddBloodSpurt)
            decal->setOrigin(Vector(splat.endpos) + Vector(splat.plane.normal) * 0.2f);
            decal->setDirection(splat.plane.normal);
            decal->setOrientation("random");
            decal->setRadius(pSize->value + G_Random(pSize->value * 0.5f));
            iSplat = 1;
        }
    }

    if (pDbg->integer) {
        gi.Printf(
            "^~^~^ HSFX ent=%d pos=(%.0f %.0f %.0f) splat=%d\n", entnum, vPos.x, vPos.y, vPos.z, iSplat
        );
    }
}

// HZM coop - PERSISTENT BLOOD POOL under a body where it dies. Unlike the impact splats (which the client
// fades out in ~10s), this uses the "coop_bloodpool" decal shader, which CG_Decal renders WITHOUT the fade
// (it lasts until the mark pool recycles it), so blood is actually there when you walk up to a corpse. Big +
// dark crimson. coop_bloodPool = radius (0 = off). Traces to the floor under the body's centroid.
// HZM coop - gore tier 2 (bug-817: continuous, non-popping pool growth). The pool no longer
// stamps 4 big rings that visibly POP - it starts small and layers ~9 finely-spaced rings with
// small OVERLAPPING radius deltas every 0.6-0.9s so the edge just creeps outward. s_coopPoolGen
// is a monotonic ordinal assigned to each new pool; the grow chain stops once 6 NEWER pools
// exist (COOP_GORE_MAX_POOLS), so at most the 6 most-recent kills are actively growing at once
// (decal-budget cap; the rings already placed persist regardless as world marks).
#define COOP_GORE_MAX_POOLS  6
#define COOP_GORE_POOL_STEPS 7   // bug-828: 9 -> 7 (smaller pool, fewer decals; brightness fixed by tint)
static int s_coopPoolGen = 0;

void Sentient::DropBloodPool(void)
{
    static cvar_t *pBP = NULL, *pDbg = NULL;
    float          rad;
    trace_t        trace;
    Vector         end;

    if (!pBP) { pBP = gi.Cvar_Get("coop_bloodPool", "32", CVAR_ARCHIVE); }
    if (!pDbg) { pDbg = gi.Cvar_Get("coop_goreDebug", "0", 0); }

    // HZM coop - no body gore on players / no player pooling (bug-792 final spec): pools mark
    // dead ACTORS only - allied AND axis AI, incl. officer/wave/reinforcement actors (verified
    // 07-18 log: GOREPOOL reached on officer + ranger + rank-and-file deaths). Players never
    // pool, alive or dead. This also keeps the grow chain off players (only DropBloodPool
    // starts it).
    if (IsSubclassOfPlayer()) {
        if (pDbg->integer) { gi.Printf("^~^~^ GOREPOOL ent=%d BLOCKED player (no pooling for players)\n", entnum); }
        return;
    }

    rad = pBP ? pBP->value : 44.0f;
    if (rad <= 1.0f) {
        if (pDbg->integer) { gi.Printf("^~^~^ GOREPOOL ent=%d BLOCKED coop_bloodPool=%.1f\n", entnum, rad); }
        return;
    }

    // green/blue bleeders (rare) keep their tint; everyone else is a dark red pool
    str splat = GetBloodSplatName();
    if (!splat.length()) {
        if (pDbg->integer) { gi.Printf("^~^~^ GOREPOOL ent=%d BLOCKED no blood_model (model %s)\n", entnum, model.c_str()); }
        return; // this thing doesn't bleed
    }

    end = centroid - Vector(0, 0, 256);
    trace = G_Trace(centroid, vec_zero, vec_zero, end, this, MASK_DEADSOLID, false, "DropBloodPool");
    if (trace.fraction >= 1.0f) {
        if (pDbg->integer) { gi.Printf("^~^~^ GOREPOOL ent=%d BLOCKED no floor under (%.0f %.0f %.0f)\n", entnum, centroid[0], centroid[1], centroid[2]); }
        return; // no floor under the body
    }

    // HZM coop - gore tier 2 (GROWING pool): with coop_gorePool 1 (default) the pool doesn't stamp at full
    // size - the base layer starts small and a self-chained PostEvent (EV_Sentient_CoopGorePoolGrow) LAYERS
    // progressively larger coop_bloodpool decals on the same floor point over ~6-7s (bug-817: many fine
    // overlapping rings so the edge creeps instead of popping). The shader renders non-fading, so the
    // overlap reads as one spreading pool. coop_gorePool 0 = exactly the old single full-size decal (no-op).
    {
        static cvar_t *pGrow = NULL;
        if (!pGrow) { pGrow = gi.Cvar_Get("coop_gorePool", "1", CVAR_ARCHIVE); }
        if (pGrow->integer) {
            m_vCoopPoolPos    = Vector(trace.endpos) + (Vector(trace.plane.normal) * 0.25f);
            m_vCoopPoolNormal = trace.plane.normal;
            // bug-817 (user "grows more seamlessly"): the base layer starts SMALL and the grow chain
            // creeps it out in fine overlapping rings (EventCoopGorePoolGrow). bug-828 (user round 2
            // "pool too big"): base start 0.45 -> 0.40 (plus coop_bloodPool 44->32 and POOL_END
            // 1.15->0.72 in the grow fn) so the finished pool is clearly smaller than the body.
            rad *= 0.40f;

            m_iCoopPoolGen = ++s_coopPoolGen; // decal-budget cap: only the 6 newest chains creep

            Event *growEv = new Event(EV_Sentient_CoopGorePoolGrow);
            growEv->AddInteger(1);
            PostEvent(growEv, 0.6f + G_Random(0.3f));
        }
    }

    Decal *decal = new Decal;
    decal->setShader("coop_bloodpool"); // distinct shader -> cgame renders it non-fading (CG_Decal)
    if (splat == "greensplat.spr") {
        decal->setColor(0.12f, 0.40f, 0.10f);
    } else if (splat == "bluesplat.spr") {
        decal->setColor(0.10f, 0.16f, 0.45f);
    } else {
        // bug-828 ROOT-CAUSE fix (user "pool too bright red; original splatter is darker"): the
        // decal's vertex COLOUR drives the rendered RGB (the mark poly path modulates by it; the dark
        // pool TEXTURE only supplies the blob ALPHA). At 0.50 red a SOLID blob reads full crimson, and
        // the overlapping grow rings converge toward that colour = bright centre. Dropped to the mod's
        // #150200 authority (21,2,0 -> 0.082,0.008): the solid pool now renders near-black maroon like
        // the original bloodsplat, and any N overlapping rings converge to #150200 (no bright centre).
        decal->setColor(0.082f, 0.008f, 0.0f);
    }
    decal->setOrigin(Vector(trace.endpos) + (Vector(trace.plane.normal) * 0.25f));
    decal->setDirection(trace.plane.normal);
    decal->setOrientation("random");
    decal->setRadius(rad + G_Random(rad * 0.3f));

    if (pDbg->integer) {
        gi.Printf("^~^~^ GOREPOOL ent=%d base r=%.0f at (%.0f %.0f %.0f)\n",
                  entnum, rad, trace.endpos[0], trace.endpos[1], trace.endpos[2]);
    }
}

// HZM coop - GORE TIER 2: one ring of the GROWING corpse blood pool. Chained from DropBloodPool: each step
// drops one more non-fading coop_bloodpool decal at the stored floor point with a larger radius (plus a hair
// of XY jitter so the edge creeps unevenly), then posts the next step. The chain lives on the corpse entity,
// so if the corpse is removed early the remaining steps simply don't fire - the decals already down persist
// on their own (they are world marks, not children of the body).
void Sentient::EventCoopGorePoolGrow(Event *ev)
{
    // bug-817 (user "grows more seamlessly"): a LINEAR radius ramp across COOP_GORE_POOL_STEPS
    // fine rings (ring 1 = 0.53x coop_bloodPool -> final ring = 1.15x), i.e. ~8% radius per step.
    // The small overlapping deltas mean each new ring only extends the pool by a thin crescent, so
    // the edge creeps instead of jumping (the old 0.82/0.94/1.05/1.18 ramp popped 4 big rings).
    // bug-828 (user "pool too big"): final ring 1.15x -> 0.72x of coop_bloodPool (and the cvar
    // default itself 44 -> 32), so even a user with coop_bloodPool archived at 44 gets ~32u radius
    // (was ~50u) - clearly smaller than the body.
    static const float POOL_START = 0.50f;
    static const float POOL_END   = 0.72f;
    static cvar_t     *pBP        = NULL, *pDbg = NULL;
    int                step       = ev->GetInteger(1);
    float              rad, frac;
    str                splat;

    if (!pBP) { pBP = gi.Cvar_Get("coop_bloodPool", "32", CVAR_ARCHIVE); }
    if (!pDbg) { pDbg = gi.Cvar_Get("coop_goreDebug", "0", 0); }
    rad = pBP->value;
    if (rad <= 1.0f || step < 1 || step > COOP_GORE_POOL_STEPS) {
        return;
    }

    // decal-budget cap: once 6 NEWER pools have started, this older chain stops creeping (its
    // already-placed rings persist as world marks). Keeps at most the 6 newest kills growing.
    if (s_coopPoolGen - m_iCoopPoolGen >= COOP_GORE_MAX_POOLS) {
        if (pDbg->integer) {
            gi.Printf("^~^~^ GOREPOOL ent=%d capped at ring %d (gen %d, newest %d)\n",
                      entnum, step, m_iCoopPoolGen, s_coopPoolGen);
        }
        return;
    }

    frac = POOL_START + (POOL_END - POOL_START) * (float)(step - 1) / (float)(COOP_GORE_POOL_STEPS - 1);

    splat = GetBloodSplatName();

    Decal *decal = new Decal;
    decal->setShader("coop_bloodpool"); // same non-fading shader + tints as the DropBloodPool base layer
    if (splat == "greensplat.spr") {
        decal->setColor(0.12f, 0.40f, 0.10f);
    } else if (splat == "bluesplat.spr") {
        decal->setColor(0.10f, 0.16f, 0.45f);
    } else {
        // bug-828: match the DropBloodPool base tint - #150200 vertex colour so the overlapping grow
        // rings converge to near-black maroon instead of accumulating toward bright crimson.
        decal->setColor(0.082f, 0.008f, 0.0f);
    }
    // tighter XY jitter than the old 4-ring version so the overlapping rings stay concentric (a
    // creeping edge, not a wandering blob)
    decal->setOrigin(m_vCoopPoolPos + Vector(G_CRandom(2), G_CRandom(2), 0));
    decal->setDirection(m_vCoopPoolNormal);
    decal->setOrientation("random");
    decal->setRadius(rad * frac + G_Random(rad * 0.05f));

    if (pDbg->integer) {
        gi.Printf("^~^~^ GOREPOOL ent=%d ring %d/%d r=%.0f\n", entnum, step, COOP_GORE_POOL_STEPS, rad * frac);
    }

    if (step < COOP_GORE_POOL_STEPS) {
        Event *growEv = new Event(EV_Sentient_CoopGorePoolGrow);
        growEv->AddInteger(step + 1);
        PostEvent(growEv, 0.6f + G_Random(0.3f)); // bug-817: continuous creep, full pool in ~6-7s
    }
}

// HZM coop - GORE TIER 2 (blood drip): attach a small looping drip-FX entity (models/fx/coop_blooddrip*.tik,
// pure client-side emitters whose falling streaks leave the mod's coop_bloodsplat mark where they land via
// bouncedecal - zero protocol traffic) to a badly wounded human or a fresh corpse. Applies to ALL bleedable
// humans - enemy AI, allied AI (escorts, paradropped reinforcements) AND players; GetBloodSplatName() empty
// means "doesn't bleed" (and vehicles/turrets aren't Sentients at all), so non-flesh is excluded naturally.
// WOUNDED GATE - the aihandler trap: rank-and-file AI run with FAKED engine health 5000 (real HP lives in
// script flags), so a health-fraction test would never fire for them. Those tier on ACCUMULATED applied
// damage (m_fCoopGoreDamage) instead; real-health sentients (players, vanilla-health AI) use a health
// fraction. Concurrency is capped by a small SafePtr slot table (each emitter costs ~2-3 client tempmodels
// per second); slots auto-NULL when their emitter entity is freed.
#define COOP_GORE_MAX_DRIPS  8
#define COOP_GORE_FAKEHP_MIN 2000.0f // engine health at/above this = aihandler-faked (real maxes are <= ~1000)

static SafePtr<Entity> s_coopDripSlots[COOP_GORE_MAX_DRIPS];

void Sentient::CoopGoreTryDripAttach(qboolean corpse)
{
    static cvar_t *pDrip = NULL, *pDmg = NULL, *pFrac = NULL, *pWoundT = NULL, *pCorpseT = NULL;
    int            i, slot, tagnum;
    float          thresholdDmg, frac, lifetime;
    Animate       *drip;

    if (!pDrip) {
        pDrip    = gi.Cvar_Get("coop_goreDrip", "1", CVAR_ARCHIVE);
        pDmg     = gi.Cvar_Get("coop_goreDripDamage", "70", CVAR_ARCHIVE); // bug-735: 120 was above typical lethal accum

        pFrac    = gi.Cvar_Get("coop_goreDripHealthFrac", "0.35", CVAR_ARCHIVE);
        pWoundT  = gi.Cvar_Get("coop_goreDripWoundTime", "20", CVAR_ARCHIVE);
        pCorpseT = gi.Cvar_Get("coop_goreDripCorpseTime", "12", CVAR_ARCHIVE);
    }

    if (!com_blood->integer || !pDrip->integer) {
        return;
    }
    // HZM coop - no body gore on players (bug-792): the drip emitter is ATTACHED to the
    // body - wounded (living) AND corpse - so players skip it entirely, in both branches.
    // This is also the "pooling while merely injured" the user reported: a stationary
    // wounded player's drip bouncedecals accumulated under their feet and read as a
    // premature blood pool. Players keep ground pools ON DEATH + blood trails (world
    // decals); AI on both sides keep the full drip behavior.
    if (IsSubclassOfPlayer()) {
        return;
    }
    if (!GetBloodSplatName().length()) {
        return; // this thing doesn't bleed (non-flesh)
    }
    if (m_pCoopDripEmitter) {
        if (!corpse) {
            return; // already dripping
        }
        // death while the slow wounded drip is up: retire it, the corpse gets the full-rate one
        m_pCoopDripEmitter->PostEvent(EV_Remove, 0);
        m_pCoopDripEmitter = NULL;
    }

    if (!corpse) {
        // wounded-enough gate (see the fake-5000 note above)
        if (max_health >= COOP_GORE_FAKEHP_MIN) {
            thresholdDmg = pDmg->value > 1.0f ? pDmg->value : 70.0f;
            if (m_fCoopGoreDamage < thresholdDmg) {
                return;
            }
            // require another half threshold of FRESH damage before a re-attach after this drip expires -
            // approximates the script-side heals (officer medkits etc.) the engine can't see
            m_fCoopGoreDamage = thresholdDmg * 0.5f;
        } else {
            frac = pFrac->value;
            if (frac <= 0.0f || frac > 1.0f) { frac = 0.35f; }
            if (max_health <= 0 || health > max_health * frac) {
                return;
            }
        }
    }

    // concurrency cap: find a free slot (SafePtr auto-NULLs when its emitter entity is freed)
    slot = -1;
    for (i = 0; i < COOP_GORE_MAX_DRIPS; i++) {
        if (!s_coopDripSlots[i]) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        return; // cap reached - skip quietly, this is pure cosmetics
    }

    tagnum = gi.Tag_NumForName(edict->tiki, "Bip01 Spine1");
    if (tagnum < 0) {
        return; // not a biped rig
    }

    drip = new Animate;
    drip->setModel(corpse ? "models/fx/coop_blooddrip.tik" : "models/fx/coop_blooddrip_slow.tik");
    drip->setSolidType(SOLID_NOT);
    if (!drip->attach(entnum, tagnum, qfalse, vec_zero)) {
        delete drip;
        return;
    }

    lifetime = corpse ? pCorpseT->value : pWoundT->value;
    if (lifetime < 1.0f) { lifetime = 1.0f; }
    drip->PostEvent(EV_Remove, lifetime);

    s_coopDripSlots[slot] = drip;
    m_pCoopDripEmitter    = drip;
}

// HZM coop - GORE TIER 1 (damage-tier blood skins). Every roster player/AI TIK carries up to 3 shaders
// per uniform surface (clean / _blood1 / _blood2 - see scripts/coop_gore.shader + gen_gore_skins.py);
// this writes the skin index straight into entityState.surfaces (replicated netfields, late joiners get
// full state) so the renderer picks the bloodied diffuse. Applies to ALL bleedable humans - enemy AI,
// allied AI AND players (GetBloodSplatName() empty = non-flesh = excluded). Surfaces without authored
// blood skins are a harmless no-op (renderer clamps out-of-range skin indices back to 0).
// TIER GATE - same aihandler trap as the drip: faked-5000-HP AI tier on ABSOLUTE accumulated damage
// (coop_goreTier1/2Dmg), real-health sentients on a FRACTION of max health (coop_goreTier1/2Frac,
// defaults 35% / 70%). Respawn is self-cleaning (InitModel wipes surface bits; Player::InitHealth
// resets the counters); script-side heals call the gore_reset event.
void Sentient::CoopGoreUpdateSkinTier(void)
{
    static cvar_t *pSkins = NULL, *pFrac1 = NULL, *pFrac2 = NULL, *pDmg1 = NULL, *pDmg2 = NULL;
    static cvar_t *pDbg = NULL; // bug-754: live verification print (coop_goreDebug 1)
    static cvar_t *pPerm = NULL; // [user 2026-08-03] bug-1320: coop_gorePermanent - blood never wiped
    float          t1, t2;
    int            tier, i, numsurfaces;

    // HZM coop - no body gore on players (bug-792, user 07-18: "blood appearing on my skin
    // from being shot"): the tier-1 blood-skin overlays painted the PLAYER model too (3P/
    // freecam self-view + what teammates see). Nothing is ever painted/attached ON a player
    // body - this extends the bug-785 no-holes rule. AI on BOTH sides keep every tier;
    // ground pools + blood trails are world decals, not body paint, and stay for players.
    if (IsSubclassOfPlayer()) {
        return;
    }

    // HZM coop - gore tier 1e: a gibbed corpse is terminal - post-death damage events must never
    // re-tier it back down to heavy (tier computation below would yield 2). gore_reset writes
    // m_iCoopGoreSkinTier = 0 directly, so revive/heal paths still clean up correctly.
    if (m_iCoopGoreSkinTier >= 3) {
        return;
    }

    if (!pDbg) { pDbg = gi.Cvar_Get("coop_goreDebug", "0", 0); }
    if (!pSkins) {
        // RETUNED 2026-07-18 (bug-735): the old 60/140 absolute + 0.35/0.70 fraction gates sat ABOVE what
        // rank-and-file AI actually absorb before dying (SMG/pistol hits are 25-37 dmg, rifles 60-120, and
        // script HP is ~100) - most enemies died clean. New gates: one solid hit = light, ~two = heavy.
        pSkins = gi.Cvar_Get("coop_goreSkins", "1", CVAR_ARCHIVE);
        pFrac1 = gi.Cvar_Get("coop_goreTier1Frac", "0.22", CVAR_ARCHIVE);
        pFrac2 = gi.Cvar_Get("coop_goreTier2Frac", "0.50", CVAR_ARCHIVE);
        pDmg1  = gi.Cvar_Get("coop_goreTier1Dmg", "35", CVAR_ARCHIVE);
        pDmg2  = gi.Cvar_Get("coop_goreTier2Dmg", "90", CVAR_ARCHIVE);
    }

    if (!com_blood->integer || !pSkins->integer || !edict->tiki) {
        if (pDbg->integer) {
            gi.Printf("^~^~^ GORESKIN ent=%d BLOCKED com_blood=%d coop_goreSkins=%d tiki=%d\n",
                      entnum, com_blood->integer, pSkins->integer, edict->tiki ? 1 : 0);
        }
        return;
    }
    if (!GetBloodSplatName().length() && !IsSubclassOfPlayer()) {
        // non-flesh doesn't bleed. Players are ALWAYS flesh but only get blood_model assigned
        // lazily by the blood-trail path (Player::Postthink), so they pass explicitly here.
        if (pDbg->integer) {
            gi.Printf("^~^~^ GORESKIN ent=%d BLOCKED no blood_model (model %s)\n",
                      entnum, model.c_str());
        }
        return;
    }

    if (max_health >= COOP_GORE_FAKEHP_MIN) {
        t1 = pDmg1->value;
        t2 = pDmg2->value;
    } else {
        t1 = max_health * pFrac1->value;
        t2 = max_health * pFrac2->value;
    }
    if (t1 <= 0.0f || t2 <= t1) {
        t1 = 35.0f; // guard nonsense cvar values
        t2 = 90.0f;
    }

    if (m_fCoopGoreDamage >= t2) {
        tier = 2;
    } else if (m_fCoopGoreDamage >= t1) {
        tier = 1;
    } else {
        tier = 0;
    }
    // [user 2026-08-03] bug-1320 - STANDING RULE: "I don't ever want blood wiped from any model."
    // The tier is now MONOTONIC within a life - it can rise with damage but never fall. Before this, a
    // heal credited the damage counter back (Health::PickupHealth -> CoopGoreHeal, or a script
    // gore_reset carrying an amount) and the recomputed tier below came out LOWER, so a bloodied
    // soldier visibly wiped clean. Healing still credits m_fCoopGoreDamage, so later damage maths is
    // unchanged; only the visual downgrade is gone. coop_gorePermanent 0 restores the old behaviour.
    if (!pPerm) { pPerm = gi.Cvar_Get("coop_gorePermanent", "1", CVAR_ARCHIVE); }
    if (pPerm->integer && tier < m_iCoopGoreSkinTier) {
        return;
    }
    if (tier == m_iCoopGoreSkinTier) {
        return;
    }
    m_iCoopGoreSkinTier = tier;

    // bug-754: machine-parseable tier-flip evidence. With coop_goreDebug 1 every flip logs the
    // entity, tier, accumulated damage and model, so "skins aren't showing" can be split into
    // driver-never-fired vs renderer-didn't-show in one play session.
    if (pDbg->integer) {
        gi.Printf("^~^~^ GORESKIN ent=%d tier=%d dmg=%.0f max_health=%.0f model=%s\n",
                  entnum, tier, m_fCoopGoreDamage, max_health, model.c_str());
    }

    numsurfaces = gi.TIKI_NumSurfaces(edict->tiki);
    if (numsurfaces > MAX_MODEL_SURFACES) {
        numsurfaces = MAX_MODEL_SURFACES;
    }
    // tier IS the skin index: 0 clean, 1 = SKINOFFSET_BIT0 (light), 2 = SKINOFFSET_BIT1 (heavy).
    // Written exactly (never additive) so tier transitions and downgrades are both correct, and the
    // nodraw/crossfade bits other systems own (helmet!) are preserved.
    // HZM coop [user 2026-08-17] - a headshot-disfigured head is EXEMPT. This loop writes one tier
    // across every surface, so without the exemption the next damage event on the corpse would reset
    // the face straight back to the body's (lower) tier.
    {
        int headSurf = m_bCoopHeadGore ? gi.Surface_NameToNum(edict->tiki, "head") : -1;
        for (i = 0; i < numsurfaces; i++) {
            if (i == headSurf) {
                continue;
            }
            edict->s.surfaces[i] =
                (edict->s.surfaces[i] & ~(MDL_SURFACE_SKINOFFSET_BIT0 | MDL_SURFACE_SKINOFFSET_BIT1)) | tier;
        }
    }
}

/*
=================
Sentient::CoopGoreDisfigureHead   (HZM coop [user 2026-08-17])

"headshots really mutilate the face / becomes unrecognizable", after death.

NO NEW ART. The disfigured face already shipped: the head TIKs declare a 4th skin
(`surface head shader <skin>_blood3`) and 28 of those textures are in the tex pak. What
they ALSO do - deliberately - is pad skins 1 and 2 with the CLEAN face, so ordinary
accumulated damage never paints someone's face. That design is kept; this only opens the
gib-tier face to a second cause, a confirmed headshot kill, instead of explosions alone.

Only the HEAD surface is touched. CoopGoreUpdateSkinTier writes one tier across every
surface in a single loop, so without the m_bCoopHeadGore latch the next damage event on
the corpse would immediately reset the head back to the body's tier.

Index 3 on a head is not new ground - CoopGoreTryGibSkins has been setting exactly that on
every surface, heads included, since gore tier 1e shipped.
=================
*/
void Sentient::CoopGoreDisfigureHead(void)
{
    static cvar_t *pOn = NULL;
    int            surf;

    // HZM coop - nothing is ever painted on a player body (bug-792 standing rule)
    if (IsSubclassOfPlayer()) {
        return;
    }
    if (!edict->tiki || !com_blood->integer) {
        return;
    }
    if (!pOn) {
        pOn = gi.Cvar_Get("coop_goreHeadshotFace", "1", CVAR_ARCHIVE);
    }
    if (!pOn->integer) {
        return;
    }
    if (!GetBloodSplatName().length()) {
        return; // non-flesh doesn't bleed
    }

    surf = gi.Surface_NameToNum(edict->tiki, "head");
    if (surf < 0 || surf >= MAX_MODEL_SURFACES) {
        return; // model has no separately-skinned head (helmeted/wrapped variants) - silent skip
    }

    edict->s.surfaces[surf] =
        (edict->s.surfaces[surf] & ~(MDL_SURFACE_SKINOFFSET_BIT0 | MDL_SURFACE_SKINOFFSET_BIT1)) | 3;
    m_bCoopHeadGore = qtrue;
}

// HZM coop - GORE TIER 3 (hit-location wound props): on a qualifying BULLET hit, attach a tiny wound-patch
// model (models/fx/coop_wound1.tik - the retail crossed-quad xbeam mesh wearing our coop_wound1 shader,
// hole + blood art baked at the mod's exact blood hue #150200) at the bone the deep LBD trace actually
// reported for this hit. The HITLOC -> bone map is the engine's own szLocArray table (cm_trace_lbd.cpp),
// read here through gi.CM_GetHitLocationInfo so the prop lands on the correct limb segment with the same
// bone-local sphere-center offset the hit test used. Attached entities replicate via parent/tag_num, so
// all clients + late joiners see them; the body's destructor EV_Removes its children, so props can never
// outlive the corpse. Applies to bleedable AI humans - enemy AND allied (players are skipped: HZM coop -
// no holes on players); non-flesh (vehicles/turrets) is excluded naturally. Crash-safe by construction:
// any missing tiki/tag/table entry = silent skip.
#define COOP_GORE_MAX_WOUNDPROPS 12 // per body; MUST match m_pCoopWoundProp[] in sentient.h

void Sentient::CoopGoreTryWoundProp(int location, int meansofdeath, const Vector &position)
{
    static cvar_t *pWounds = NULL;
    const char    *tagname;
    float          locRadius;
    vec3_t         locOffset;
    int            i, slot, tagnum, maxProps;
    static cvar_t *pWoundMax = NULL; // HZM coop [user 2026-08-17] - per-body hole ceiling
    Animate       *prop;
    Vector         attachOfs;

    if (!pWounds) {
        pWounds = gi.Cvar_Get("coop_goreWounds", "1", CVAR_ARCHIVE);
    }

    if (!com_blood->integer || !pWounds->integer || !edict->tiki) {
        return;
    }
    // HZM coop - no holes on players: tier-3 wound props are visually bullet holes,
    // and players must never show holes (matches the renderer-side UV-stamp gate in
    // tr_gore.c).  AI / allied AI keep their wound props; players keep blood drips
    // and the gore skin tiers.
    if (IsSubclassOfPlayer()) {
        return;
    }
    if (meansofdeath != MOD_BULLET && meansofdeath != MOD_FAST_BULLET && meansofdeath != MOD_SHOTGUN) {
        return; // bullet wounds only - blast/melee/fire don't leave a neat entry hole
    }
    if (location < HITLOC_HEAD || location >= NUMBODYLOCATIONS) {
        return; // MISS/GENERAL or garbage
    }
    switch (location) {
    case HITLOC_HELMET: // still HELMET after CheckHitLocation = actually wearing one; prop would float on it
    case HITLOC_R_HAND: // hands/feet: segments too small, prop reads as a growth
    case HITLOC_L_HAND:
    case HITLOC_R_FOOT:
    case HITLOC_L_FOOT:
        return;
    default:
        break;
    }
    if (!GetBloodSplatName().length() && !IsSubclassOfPlayer()) {
        return; // non-flesh doesn't bleed (players get blood_model lazily, so they pass explicitly)
    }

    // [user 2026-08-17] "when I unload an SMG on someone I should see bullet holes all over their
    // body where I hit them" - they did not. The cap was FOUR per body, and a full body SKIPPED the
    // hit outright, so from the 5th round on nothing appeared however long you kept firing - and the
    // four holes you did get sat wherever the first four rounds happened to land, not where you were
    // aiming now. That is exactly the reported "only sometimes".
    //
    // Two changes: the ceiling is a cvar over a 12-slot array, and a FULL body now RECYCLES its
    // oldest prop instead of skipping. The recycle is what actually fixes the complaint - holes
    // follow your current burst - and it keeps entity cost per body strictly bounded, which matters
    // because coop count-scales enemies up to 80 (TRAPS T4: per-hit entity spawns are a server-load
    // multiplier on this project's hordes).
    if (!pWoundMax) {
        pWoundMax = gi.Cvar_Get("coop_goreWoundMax", "8", CVAR_ARCHIVE);
    }
    maxProps = pWoundMax->integer;
    if (maxProps < 1) {
        maxProps = 1;
    } else if (maxProps > COOP_GORE_MAX_WOUNDPROPS) {
        maxProps = COOP_GORE_MAX_WOUNDPROPS;
    }

    slot = -1;
    for (i = 0; i < maxProps; i++) {
        if (!m_pCoopWoundProp[i]) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        // full: reuse the OLDEST slot rather than dropping the hit on the floor
        slot = m_iCoopWoundNext % maxProps;
        if (m_pCoopWoundProp[slot]) {
            m_pCoopWoundProp[slot]->PostEvent(EV_Remove, 0);
            m_pCoopWoundProp[slot] = NULL;
        }
    }
    m_iCoopWoundNext = (slot + 1) % maxProps;

    tagname = gi.CM_GetHitLocationInfo(location, &locRadius, locOffset);
    if (!tagname || !*tagname) {
        return;
    }
    tagnum = gi.Tag_NumForName(edict->tiki, tagname);
    if (tagnum < 0) {
        return; // not a biped rig (or bone missing) - skip silently
    }

    // bug-735: attach at the BULLET ENTRY POINT, not the bone-sphere center. The LBD sphere center sits
    // INSIDE the mesh (radius 4-9u) while the prop spans only ~4u, so center-attached props were swallowed
    // by the body and effectively never visible. Convert the damage position into the tag's local frame,
    // then clamp it onto the hit sphere's shell (+1u proud) so the crossed quads poke out of the cloth.
    attachOfs = Vector(locOffset); // fallback: old sphere-center behavior (position missing/degenerate)
    if (position != vec_zero) {
        orientation_t tagOr;
        Vector        local, fromCenter;
        float         len;

        GetTagPositionAndOrientation(tagnum, &tagOr);
        Vector delta = position - Vector(tagOr.origin);
        local.x    = DotProduct(delta, tagOr.axis[0]);
        local.y    = DotProduct(delta, tagOr.axis[1]);
        local.z    = DotProduct(delta, tagOr.axis[2]);
        fromCenter = local - Vector(locOffset);
        len        = fromCenter.length();
        if (len > 0.25f && len < locRadius * 6.0f) {
            fromCenter *= (locRadius + 1.0f) / len;
            attachOfs = Vector(locOffset) + fromCenter;
        }
    }

    prop = new Animate;
    prop->setModel("models/fx/coop_wound1.tik");
    prop->setSolidType(SOLID_NOT);
    prop->setScale(0.8f + G_Random(0.5f)); // slight size variety so stacked hits don't read as copies
    // small jitter so repeat hits to one segment don't z-fight on the exact same spot
    if (!prop->attach(entnum, tagnum, qfalse, attachOfs + Vector(G_CRandom(0.75f), G_CRandom(0.75f), G_CRandom(0.75f)))) {
        delete prop;
        return; // parent's child table full (helmet + drip + props) - skip
    }

    m_pCoopWoundProp[slot] = prop;
}

// HZM coop - gore tier 1: an engine-visible heal credits the gore counter back and re-tiers
// (tiers can go DOWN - a patched-up soldier looks cleaner). Called from Health::PickupHealth.
void Sentient::CoopGoreHeal(float amount)
{
    if (amount <= 0.0f) {
        return;
    }
    m_fCoopGoreDamage -= amount;
    if (m_fCoopGoreDamage < 0.0f) {
        m_fCoopGoreDamage = 0.0f;
    }
    CoopGoreUpdateSkinTier();
}

// HZM coop - gore tier 1: the script-side heal hook ("gore_reset"). Script heals live outside the
// engine's view (officer canteen/health post, aihandler coop_actorActualHealth, DBNO revive), so
// those paths call this. No arg = full reset; with an amount = partial-heal credit.
void Sentient::EventCoopGoreReset(Event *ev)
{
    int i, numsurfaces;

    if (ev->NumArgs() > 0) {
        CoopGoreHeal(ev->GetFloat(1));
        return;
    }

    m_fCoopGoreDamage   = 0;
    m_bCoopGoreGibMark  = qfalse; // HZM coop - gore tier 1e: a heal/revive also clears a pending script mark

    // [user 2026-08-03] bug-1320 - same standing rule as the monotonic guard in
    // CoopGoreUpdateSkinTier. The no-arg script "gore_reset" is called by revive / canteen-heal /
    // aihandler paths, and it used to hard-clear the skin bits - the one remaining way blood came off a
    // model. Under coop_gorePermanent it credits the damage counter (harmless: the monotonic guard
    // stops the zeroed counter from re-tiering anything down) but leaves the painted blood alone.
    // Player respawn is unaffected: Player::Respawn zeroes m_iCoopGoreSkinTier directly, not via this
    // event, so a new life still starts clean.
    {
        static cvar_t *pPerm = NULL;
        if (!pPerm) { pPerm = gi.Cvar_Get("coop_gorePermanent", "1", CVAR_ARCHIVE); }
        if (pPerm->integer) {
            return;
        }
    }

    m_iCoopGoreSkinTier = 0;
    m_bCoopHeadGore     = qfalse; // HZM coop [user 2026-08-17] - a revive clears the disfigured face too
    if (edict->tiki) { // force-clear the skin bits even if the cvars were toggled off mid-life
        numsurfaces = gi.TIKI_NumSurfaces(edict->tiki);
        if (numsurfaces > MAX_MODEL_SURFACES) {
            numsurfaces = MAX_MODEL_SURFACES;
        }
        for (i = 0; i < numsurfaces; i++) {
            edict->s.surfaces[i] &= ~(MDL_SURFACE_SKINOFFSET_BIT0 | MDL_SURFACE_SKINOFFSET_BIT1);
        }
    }
    if (m_pCoopDripEmitter) { // a healed sentient stops dripping too
        m_pCoopDripEmitter->PostEvent(EV_Remove, 0);
        m_pCoopDripEmitter = NULL;
    }
    for (i = 0; i < COOP_GORE_MAX_WOUNDPROPS; i++) { // HZM coop - gore tier 3: patched up = wound props come off
        if (m_pCoopWoundProp[i]) {
            m_pCoopWoundProp[i]->PostEvent(EV_Remove, 0);
            m_pCoopWoundProp[i] = NULL;
        }
    }
}

// HZM coop - gore tier 1e: is this surface exposed SKIN (face/hands/neck)? Exact-name matches only,
// mirroring the generator's FACE_SURF_RE/HAND_SURF_RE ('headwrap'/'headgear'/'helmet' must NOT qualify).
// Skin surfaces always take the full gib splatter - an "extremely bloody" corpse never rolls a clean face.
static qboolean CoopGoreSurfIsSkin(const char *name)
{
    static const char *skinNames[] = {"head", "face", "neck", "hand", "hands"};
    char               withSuffix[64];
    size_t             i;

    for (i = 0; i < sizeof(skinNames) / sizeof(skinNames[0]); i++) {
        if (!Q_stricmp(name, skinNames[i])) {
            return qtrue;
        }
        Com_sprintf(withSuffix, sizeof(withSuffix), "%s_c", skinNames[i]);
        if (!Q_stricmp(name, withSuffix)) {
            return qtrue;
        }
    }
    return qfalse;
}

// HZM coop - gore tier 1e: the explosive means-of-death set. Native engine blasts (grenades, rockets,
// generic explosions/exploders, AA/tank guns, landmines) plus direct projectile impacts (a bazooka/tank
// shell that kills on body contact reports MOD_IMPACT before its radius blast - still an explosion kill).
static qboolean CoopGoreModIsExplosive(int meansofdeath, Entity *inflictor)
{
    switch (meansofdeath) {
    case MOD_EXPLOSION:
    case MOD_EXPLODEWALL:
    case MOD_GRENADE:
    case MOD_ROCKET:
    case MOD_AAGUN:
    case MOD_LANDMINE:
        return qtrue;
    case MOD_IMPACT:
        return (inflictor && inflictor->IsSubclassOfProjectile()) ? qtrue : qfalse;
    default:
        return qfalse;
    }
}

// HZM coop - GORE TIER 1e (extreme explosion-death "gib" skins). Called once from the killing-blow path,
// right after CoopGoreUpdateSkinTier() has written the heavy tier: if the kill was an explosion (native
// explosive MOD, projectile direct impact, or a fresh script gore_gibmark), upgrade the corpse's surfaces
// from skin index 2 to skin index 3 - the *_blood3 gib-splatter shaders authored in scripts/coop_gore3.shader
// (uniform AND face/hand skin, see gen_gore3_skins.py). RANDOMNESS: a per-corpse coverage pattern decides
// which CLOTH surfaces take the extreme skin vs. keep the ordinary heavy tier (skin surfaces always flip),
// and the art itself is one of 3 authored splatter styles per texture - so repeated deaths differ.
// SAFETY: index 3 is only ever written to surfaces whose TIKI actually carries 4 shaders - the renderer
// clamps out-of-range skin indices to 0 (CLEAN), so a blind write would UN-bloody unauthored surfaces
// (tr_model.cpp iShaderNum >= numskins -> 0). Surfaces without a 4th skin simply stay heavy.
// Replication is free: skin bits live in entityState.surfaces (netfields, late joiners get full state).
void Sentient::CoopGoreTryGibSkins(int meansofdeath, Entity *inflictor)
{
    static cvar_t        *pGib = NULL, *pSkins = NULL, *pDbg = NULL;
    const dtikisurface_t *dsurf;
    int                   i, numsurfaces, pattern, nExtreme;
    float                 keepHeavyChance;

    // bug-792 rule: nothing is ever painted on a PLAYER body (3P/freecam self-view + teammates).
    if (IsSubclassOfPlayer()) {
        return;
    }

    if (!pGib) {
        pGib   = gi.Cvar_Get("coop_goreGibSkins", "1", CVAR_ARCHIVE);
        pSkins = gi.Cvar_Get("coop_goreSkins", "1", CVAR_ARCHIVE);
        pDbg   = gi.Cvar_Get("coop_goreDebug", "0", 0);
    }

    // same master gates as the tier system it extends (com_blood kills all gore; the gib tier also
    // requires the base skin tiers to be on, plus its own coop_goreGibSkins switch).
    if (!com_blood->integer || !pSkins->integer || !pGib->integer || !edict->tiki) {
        return;
    }
    if (!GetBloodSplatName().length()) {
        return; // non-flesh doesn't bleed
    }

    if (!CoopGoreModIsExplosive(meansofdeath, inflictor)
        && !(m_bCoopGoreGibMark && level.time <= m_fCoopGoreGibMarkTime)) {
        return; // not an explosion death
    }

    numsurfaces = gi.TIKI_NumSurfaces(edict->tiki);
    if (numsurfaces > MAX_MODEL_SURFACES) {
        numsurfaces = MAX_MODEL_SURFACES;
    }

    // per-corpse coverage pattern: 0 = full drench (every authored surface), 1 = patchy (cloth keeps
    // the ordinary heavy tier 25% of the time), 2 = contrasty (45%). Skin (face/hands) always flips.
    pattern = (int)G_Random(3.0f);
    if (pattern > 2) {
        pattern = 2; // G_Random can return exactly its bound
    }
    keepHeavyChance = (pattern == 0) ? 0.0f : ((pattern == 1) ? 0.25f : 0.45f);

    nExtreme = 0;
    for (i = 0; i < numsurfaces; i++) {
        dsurf = &edict->tiki->surfaces[i];
        if (dsurf->numskins < 4) {
            continue; // no authored gib skin - writing index 3 would render CLEAN (renderer clamp)
        }
        if (!CoopGoreSurfIsSkin(dsurf->name) && keepHeavyChance > 0.0f && G_Random(1.0f) < keepHeavyChance) {
            continue; // this cloth surface keeps the heavy tier for per-corpse variation
        }
        // both skin bits set = skin index 3 (nodraw/crossfade bits other systems own are preserved)
        edict->s.surfaces[i] |= (MDL_SURFACE_SKINOFFSET_BIT0 | MDL_SURFACE_SKINOFFSET_BIT1);
        nExtreme++;
    }

    if (nExtreme) {
        // terminal tier: locks CoopGoreUpdateSkinTier out of re-tiering the corpse down to 2 when
        // post-death damage events (shooting the body) run the accumulate path again.
        m_iCoopGoreSkinTier = 3;
    }

    if (pDbg->integer) {
        // machine-parseable evidence (same convention as GORESKIN): split "gibs aren't showing" into
        // driver-never-fired vs renderer-didn't-show in one session.
        gi.Printf("^~^~^ GOREGIB ent=%d mod=%d mark=%d pattern=%d gib_surfs=%d/%d model=%s\n",
                  entnum, meansofdeath, m_bCoopGoreGibMark ? 1 : 0, pattern, nExtreme, numsurfaces,
                  model.c_str());
    }
}

// HZM coop [user 2026-08-17] - LIVE SEVERED HEADS, bounded by COUNT not by a timer.
//
// Heads now persist like corpses (coop_decapLife 0), so something has to stop a long map from
// filling the entity pool one head at a time - that is the leak the original 4-second lifetime was
// really guarding against, and a stopwatch was a blunt way to do it. This is a ring of the live
// heads: when the (coop_decapMax + 1)th is created, the OLDEST fades out. The player keeps a
// battlefield littered with heads, and the entity cost has a hard ceiling either way.
//
// Same shape as the wound-prop recycle: SafePtr entries self-NULL if a head is freed some other way
// (map change, fade completing), so a stale slot can never be mistaken for a live one.
#define COOP_DECAP_MAX_HEADS 32
static SafePtr<Entity> s_coopHeads[COOP_DECAP_MAX_HEADS];
static int             s_coopHeadNext = 0;

static void CoopDecapRegisterHead(Entity *head)
{
    static cvar_t *pMax = NULL;
    int            cap, i, live;

    if (!head) {
        return;
    }
    if (!pMax) {
        pMax = gi.Cvar_Get("coop_decapMax", "32", CVAR_ARCHIVE); // [user 2026-08-18] 16 -> 32: "the heads disappear still". 32 is the ring array size (COOP_DECAP_MAX_HEADS), so this uses the existing ceiling rather than raising it
    }
    cap = pMax->integer;
    if (cap < 1) {
        cap = 1;
    } else if (cap > COOP_DECAP_MAX_HEADS) {
        cap = COOP_DECAP_MAX_HEADS;
    }

    // count what is actually still alive (SafePtr has already NULLed anything freed elsewhere)
    live = 0;
    for (i = 0; i < COOP_DECAP_MAX_HEADS; i++) {
        if (s_coopHeads[i]) {
            live++;
        }
    }

    if (live >= cap) {
        // retire the oldest rather than refusing to spawn the newest - the head you just took off is
        // the one the player is looking at
        for (i = 0; i < COOP_DECAP_MAX_HEADS; i++) {
            int idx = (s_coopHeadNext + i) % COOP_DECAP_MAX_HEADS;
            if (s_coopHeads[idx]) {
                Event *fade = new Event(EV_Fade);
                fade->AddFloat(1.0f);
                s_coopHeads[idx]->PostEvent(fade, 0.0f);
                s_coopHeads[idx] = NULL;
                break;
            }
        }
    }

    s_coopHeads[s_coopHeadNext] = head;
    s_coopHeadNext              = (s_coopHeadNext + 1) % COOP_DECAP_MAX_HEADS;
}

/*
=================
Sentient::CoopGoreTryDecapitate   (HZM coop [user 2026-08-17])

Blast/shotgun decapitation. This shipped once as bug-866 and was reverted by bug-892 as a
precaution during the MAX_MODELS rebuild; the AI-twitch that motivated the revert later resolved to
the entity-pool stomp (bugs 914-927), which the 2048 pool fixed. This is a re-add that keeps every
mitigation from the adversarially-reviewed safe pattern, because the FIRST attempt (bug-856) made
all AI stutter and stop shooting - and that was server-sim LOAD, not a bad hook:

  * HARD PER-FRAME BUDGET. One grenade into a count-scaled horde used to fire dozens of decaps in a
    single server frame. coop_decapBudget (default 3) caps it; the rest of that frame is skipped.
    This is THE key fix - without it nothing else matters.
  * ONE short-lived rigid prop, never animated, SOLID_NOT + MASK_VIEWSOLID so it can never block a
    player or an AI path, self-removing at coop_decapLife (4s, not the original 30).
  * The neck cap takes a FREE m_pCoopWoundProp slot ONLY. If none is free it is skipped - an
    untracked attachment leaves a dangling s.parent that the per-frame parent-chain walk then
    follows, which is one of the things that made bug-856 so expensive.
  * NO new art. The severed head is the victim's own model with every surface except "head" set to
    NODRAW and no animation ever issued, so it renders in bind pose.
  * Dead-gated twice, and chance-gated (coop_decapChance, default 30) - the user asked that it not
    be guaranteed.
=================
*/
void Sentient::CoopGoreTryDecapitate(int meansofdeath, Entity *inflictor)
{
    static cvar_t *pOn = NULL, *pChance = NULL, *pBudget = NULL, *pDbg = NULL;
    static float   sBudgetTime  = -1.0f;
    static int     sBudgetCount = 0;
    HeadGibObject *gib;
    Animate       *cap;
    orientation_t  tagOr;
    int            headSurf, headTag, neckTag, i, slot;
    static Vector  sCoopLastHeadOfs = vec_zero; // reported below - if this is 0,0,0 the measurement failed
    int            numsurfaces = 0, nHeadSurfs = 0;
    qboolean       allowed;

    // --- dead-gate #2 (the call site already tests health <= 0) ---
    if (health > 0) {
        return;
    }
    if (IsSubclassOfPlayer()) {
        return; // never a player - extends the bug-785/792 no-gore-on-players rule
    }
    if (!edict->tiki || !com_blood->integer) {
        return;
    }
    if (!pOn) {
        pOn     = gi.Cvar_Get("coop_decap", "1", CVAR_ARCHIVE);
        pChance = gi.Cvar_Get("coop_decapChance", "30", CVAR_ARCHIVE);
        pBudget = gi.Cvar_Get("coop_decapBudget", "3", CVAR_ARCHIVE);
        pDbg    = gi.Cvar_Get("coop_goreDebug", "0", 0);
    }
    if (!pOn->integer) {
        return;
    }
    if (!GetBloodSplatName().length()) {
        return; // non-flesh doesn't come apart
    }

    // [user 2026-08-17] "certain weapons should decapitate... rockets, grenades, shotguns,
    // artillery". CoopGoreModIsExplosive already classifies the blast set and is shared with the
    // gib skins, so it is reused rather than duplicated; shotgun is added here only.
    allowed = CoopGoreModIsExplosive(meansofdeath, inflictor);
    if (!allowed && meansofdeath == MOD_SHOTGUN) {
        allowed = qtrue;
    }
    if (!allowed) {
        return;
    }

    if (G_Random(100.0f) >= (float)pChance->integer) {
        return; // deliberately not guaranteed
    }

    // --- the per-frame budget ---
    if (sBudgetTime != level.time) {
        sBudgetTime  = level.time;
        sBudgetCount = 0;
    }
    if (sBudgetCount >= (pBudget->integer > 0 ? pBudget->integer : 3)) {
        if (pDbg->integer) {
            gi.Printf("^~^~^ DECAPFRAME budget hit at t=%.2f, skipping ent=%d\n", level.time, entnum);
        }
        return;
    }

    headSurf = gi.Surface_NameToNum(edict->tiki, "head");
    headTag  = gi.Tag_NumForName(edict->tiki, "Bip01 Head");
    if (headSurf < 0 || headSurf >= MAX_MODEL_SURFACES || headTag < 0) {
        return; // helmeted/wrapped variants and non-bipeds simply do not decapitate
    }
    if (edict->s.surfaces[headSurf] & MDL_SURFACE_NODRAW) {
        return; // already headless
    }

    sBudgetCount++;

    // [user 2026-08-17] DECAPSURF probe. Reported symptom: "only really notice a random set of hands
    // appearing that kinda float, not a head. Their head is still attached." Both halves of that point
    // at the surface INDEX meaning something different than intended - but Surface_NameToNum is an
    // exact stricmp, so it cannot be partial-matching "hand" for "head". Rather than guess a third
    // time, dump the corpse's real surface table and the gib's, and let one run settle it.
    if (pDbg->integer) {
        int dn = gi.TIKI_NumSurfaces(edict->tiki);
        gi.Printf("^~^~^ DECAPSURF corpse tiki=%s surfaces=%d headSurf=%d headTag=%d\n",
                  gi.TIKI_NameForNum(edict->tiki), dn, headSurf, headTag);
        for (i = 0; i < dn && i < MAX_MODEL_SURFACES; i++) {
            gi.Printf("^~^~^ DECAPSURF   corpse[%d] = %s%s\n", i,
                      gi.Surface_NumToName(edict->tiki, i), (i == headSurf) ? "   <== treated as HEAD" : "");
        }
    }

    GetTagPositionAndOrientation(headTag, &tagOr);

    // --- take the head off the body ---
    // [user 2026-08-17] Hide EVERY surface named "head", not just the first. This model carries two
    // (corpse[3] and corpse[4] in the surface dump), so nodraw-ing only headSurf left the second one
    // drawn - the body kept a visible head while the gib flew off, which is precisely the original
    // "their head is still attached" report. Same duplicate-surface trap as the gib side below.
    {
        int ci, cn = gi.TIKI_NumSurfaces(edict->tiki);
        if (cn > MAX_MODEL_SURFACES) { cn = MAX_MODEL_SURFACES; }
        for (ci = 0; ci < cn; ci++) {
            const char *cname = gi.Surface_NumToName(edict->tiki, ci);
            if (cname && !Q_stricmp(cname, "head")) {
                edict->s.surfaces[ci] |= MDL_SURFACE_NODRAW;
            }
        }
    }
    m_bCoopHeadGore = qtrue; // the face logic must not try to re-skin a head that is gone

    // --- the severed head: our own model, everything but the head hidden, never animated ---
    gib = new HeadGibObject;

    // [user 2026-08-17] The gib needs the COMPOSITE model string, not the bare path. Measured with
    // the DECAPSURF probe: the corpse's tiki has 11 surfaces including two heads, while a plain
    // setModel(model) produced only 4 - tunic, tunic_cull, pants, hand - with NO head at all, so
    // every decap correctly aborted and nothing happened.
    //
    // The reason is that the head is not part of the base .tik: it arrives through
    // `$include models/human/heads/*.tik` inside a `case headskin <name>` block, so the head
    // skelmodel is only compiled into the tiki when a headskin is actually selected. Actor::setModel
    // (actor.cpp:11302) is where the real model string gets built - "headmodel|X|headskin|Y|<path>" -
    // and a fresh entity has none of that, so its case never fires.
    //
    // Rebuild the head half of that string here. The weapon|<loadout>| part is deliberately omitted:
    // every non-head surface is nodraw'd below anyway, so pulling in the gear would only cost tiki
    // surfaces for nothing.
    {
        str gibName = model;

        if (IsSubclassOfActor()) {
            Actor *act = (Actor *)this;
            str    pre;

            if (act->m_csHeadModel != STRING_EMPTY) {
                pre += "headmodel|" + Director.GetString(act->m_csHeadModel) + "|";
            }
            if (act->m_csHeadSkin != STRING_EMPTY) {
                pre += "headskin|" + Director.GetString(act->m_csHeadSkin) + "|";
            }
            if (pre.length()) {
                gibName = pre + model;
            }
        }
        // [user 2026-08-17] Set the model the way Actor::setModel does - gi.setmodel DIRECTLY - and
        // NOT through Entity::setModel(str). That overload runs the name through CanonicalTikiName
        // (g_utils.cpp:1523), which PREPENDS "models/" to anything not already starting with it. Our
        // composite starts with "headmodel|", so it became
        // "models/headmodel|head4|headskin|X|models/human/....tik" and could never resolve - which is
        // exactly why the gib kept loading the bare 4-surface model with no head surface, and why
        // Actor::setModel bypasses that path as well.
        //
        // Skipping Entity::setModel also skips its idle-anim start and ProcessInitCommands, which is
        // what we want here: the head must never be animated or it leaves the bind pose the whole
        // no-new-art trick depends on. The bbox is set explicitly in the HeadGibObject constructor.
        gib->model = gibName;
        level.skel_index[gib->edict->s.number] = -1;
        gi.setmodel(gib->edict, gibName);
        if (pDbg->integer) {
            gi.Printf("^~^~^ DECAPSURF gib requested model=%s\n", gibName.c_str());
        }
    }
    gib->setOrigin(Vector(tagOr.origin));
    gib->setAngles(angles);
    gib->setScale(edict->s.scale);
    if (pDbg->integer) {
        gi.Printf("^~^~^ DECAPSURF gib model=%s tiki=%s surfaces=%d\n",
                  model.c_str(),
                  gib->edict->tiki ? gi.TIKI_NameForNum(gib->edict->tiki) : "(none)",
                  gib->edict->tiki ? gi.TIKI_NumSurfaces(gib->edict->tiki) : -1);
        if (gib->edict->tiki) {
            int gn = gi.TIKI_NumSurfaces(gib->edict->tiki);
            for (i = 0; i < gn && i < MAX_MODEL_SURFACES; i++) {
                gi.Printf("^~^~^ DECAPSURF   gib[%d] = %s%s\n", i,
                          gi.Surface_NumToName(gib->edict->tiki, i),
                          (i == headSurf) ? "   <== KEPT VISIBLE" : "");
            }
        }
    }

    if (gib->edict->tiki) {
        // [user 2026-08-17] Resolve the head on the GIB'S OWN tiki, by NAME. Reusing headSurf - which
        // was resolved against the CORPSE's tiki - was a real bug: the two are different composites
        // (the corpse carries its weapon case's gear surfaces, a fresh setModel does not), so the same
        // index is a different surface on each. That is why the user saw "a random set of hands
        // appearing that kinda float, not a head": index N was head on the body and hand on the gib.
        int gibHead = gi.Surface_NameToNum(gib->edict->tiki, "head");

        numsurfaces = gi.TIKI_NumSurfaces(gib->edict->tiki);
        if (numsurfaces > MAX_MODEL_SURFACES) {
            numsurfaces = MAX_MODEL_SURFACES;
        }
        if (gibHead < 0 || gibHead >= numsurfaces) {
            // no head surface on this composite - a headless gib would just be an invisible prop
            // tumbling around, which is worse than no decap at all. Undo and bail.
            gib->PostEvent(EV_Remove, 0);
            {
                int ci, cn = gi.TIKI_NumSurfaces(edict->tiki);
                if (cn > MAX_MODEL_SURFACES) { cn = MAX_MODEL_SURFACES; }
                for (ci = 0; ci < cn; ci++) {
                    const char *cname = gi.Surface_NumToName(edict->tiki, ci);
                    if (cname && !Q_stricmp(cname, "head")) {
                        edict->s.surfaces[ci] &= ~MDL_SURFACE_NODRAW;
                    }
                }
            }
            m_bCoopHeadGore = qfalse;
            // [user 2026-08-17] NOT gated on coop_goreDebug any more. With the probe off, this abort
            // was silent - the user set coop_decapChance 100, took eight shotgun kills, saw no heads
            // come off, and there was nothing in the log to say why. A decap that refuses to fire is
            // a defect worth reporting. Deduped per model so it cannot become spam.
            {
                static str sLastWarned;
                if (sLastWarned != model) {
                    sLastWarned = model;
                    gi.Printf("^~^~^ DECAP unavailable for %s - the gib composite has no head surface\n",
                              model.c_str());
                }
            }
            return;
        }
        // [user 2026-08-17] Keep EVERY surface named "head", not just the first index. The corpse
        // surface dump showed this model carries TWO of them - corpse[3] = head and corpse[4] = head
        // (the same shape as tunic / tunic_cull). Surface_NameToNum returns the FIRST match, so
        // nodraw-ing "everything except gibHead" hid the second one - and if the visible geometry is
        // that second surface, the severed head renders as nothing at all. Which is exactly the
        // reported "no decap" with no error: it fired, and produced an invisible head.
        nHeadSurfs = 0;
        for (i = 0; i < numsurfaces; i++) {
            const char *sname = gi.Surface_NumToName(gib->edict->tiki, i);
            if (sname && !Q_stricmp(sname, "head")) {
                nHeadSurfs++;
                // carry the disfigured face onto every head surface
                gib->edict->s.surfaces[i] =
                    (gib->edict->s.surfaces[i] & ~(MDL_SURFACE_SKINOFFSET_BIT0 | MDL_SURFACE_SKINOFFSET_BIT1)) | 3;
            } else {
                gib->edict->s.surfaces[i] |= MDL_SURFACE_NODRAW;
            }
        }

    }
    // [user 2026-08-17] PUT THE COLLISION BOX ON THE HEAD, not on the entity origin.
    //
    // Symptoms were "it clips thru the ground" and "other times it floats in the air" - opposite
    // complaints with ONE cause. The gib draws the whole body in bind pose with only the head
    // visible, so the head MESH sits roughly 60 units above the entity ORIGIN. The collision box was
    // at the origin, so physics settled the origin on the floor and the visible head hung in the air
    // above it - or on a slope/ledge the reverse, with the head buried. Making the box bigger could
    // never fix that, because the box and the thing you can see were in different places.
    //
    // mins/maxs are relative to the origin, so the box can simply be MOVED onto the head: measure
    // where the head bone actually renders (that offset IS the bind-pose displacement), wrap the box
    // around it, and shift the spawn origin back by the same amount so the visible head starts
    // exactly where the real one was. Physics then moves the origin while the box tracks the head,
    // and the head lands on the ground like an object instead of a puppet on an invisible string.
    {
        int gibHeadTag = gib->edict->tiki ? gi.Tag_NumForName(gib->edict->tiki, "Bip01 Head") : -1;
        if (gibHeadTag >= 0) {
            orientation_t gibOr;
            Vector        headOfs;
            float         r;

            gib->GetTagPositionAndOrientation(gibHeadTag, &gibOr);
            headOfs = Vector(gibOr.origin) - gib->origin; // bind-pose head offset

            r = 6.0f;
            gib->setSize(headOfs + Vector(-r, -r, -r), headOfs + Vector(r, r, r));

            // start the VISIBLE head where the corpse's head actually was
            gib->setOrigin(Vector(tagOr.origin) - headOfs);

            // hand the offset to the settle think, which lands the head without trusting the box
            gib->m_vCoopHeadOfs = headOfs;
            sCoopLastHeadOfs    = headOfs;
        }
    }

    // [user 2026-08-17] DECAP HELMET. "the helmet stays over where the enemies head used to be, so
    // that should maybe come off too separately." It does now - and via the engine's OWN helmet pop
    // (EV_Sentient_PopHelmet), which already hides the helmet surfaces and throws a real HelmetObject
    // with its own physics and landing clatter. Reusing it means the helmet behaves exactly as it
    // does when shot off, instead of a second bespoke prop that could drift out of step with it.
    if (WearingHelmet()) {
        ProcessEvent(EV_Sentient_PopHelmet);
    } else {
        //
        // [user 2026-08-17] "helmets didnt come off". EV_Sentient_PopHelmet only does anything for
        // a model that actually REGISTERED a helmet via sethelmet - WearingHelmet() tests
        // m_sHelmetSurface1, which is empty otherwise. Plenty of models carry their helmet as
        // ordinary geometry (an "outside"/"hat"/"camocover" surface) and never register it, so the
        // pop was a no-op and the helmet stayed hanging where the head had been.
        //
        // For those, hide the headgear surfaces directly. Names are the ones the retail sethelmet
        // calls use across the trilogy - us_helmet, outside/inside (German steel), hat (caps),
        // creasecap, camocover - matched exactly, so nothing else can be caught by accident.
        //
        static const char *headgear[] = {"outside", "inside",  "us_helmet", "hat",
                                         "creasecap", "camocover", "helmet"};
        int                ns = gi.TIKI_NumSurfaces(edict->tiki);
        int                si, gi_;

        if (ns > MAX_MODEL_SURFACES) {
            ns = MAX_MODEL_SURFACES;
        }
        for (si = 0; si < ns; si++) {
            const char *sn = gi.Surface_NumToName(edict->tiki, si);
            if (!sn || !*sn) {
                continue;
            }
            for (gi_ = 0; gi_ < (int)(sizeof(headgear) / sizeof(headgear[0])); gi_++) {
                if (!Q_stricmp(sn, headgear[gi_])) {
                    edict->s.surfaces[si] |= MDL_SURFACE_NODRAW;
                    break;
                }
            }
        }
    }

    // [user 2026-08-17] blood from the severed neck END of the flying head - the same drip FX the
    // corpse bleed-out uses, so a decapitated head trails the mod's blood rather than inventing a
    // second look. Attached to the GIB at its own neck bone (the gib carries the full bind-pose
    // skeleton, so the tag exists), and children are removed with their parent, so it can never
    // outlive the head.
    {
        int gibNeck = gib->edict->tiki ? gi.Tag_NumForName(gib->edict->tiki, "Bip01 Neck") : -1;
        if (gibNeck >= 0) {
            Animate *hdrip = new Animate;
            hdrip->setModel("models/fx/coop_blooddrip.tik");
            hdrip->setSolidType(SOLID_NOT);
            if (!hdrip->attach(gib->entnum, gibNeck, qfalse, Vector(0, 0, 0))) {
                delete hdrip; // parent's child table full - never leave it untracked
            }
        }
    }

    gib->velocity   = velocity + Vector(G_CRandom(90.0f), G_CRandom(90.0f), 130.0f + G_Random(90.0f));
    // [user 2026-08-17] Heavier tumble (option 3). A head that turns slowly presents a clean,
    // recognisable profile for whole seconds at a time; spinning it hard means the silhouette is
    // never still long enough to read as an intact head.
    gib->avelocity  = Vector(G_CRandom(1400.0f), G_CRandom(1400.0f), G_CRandom(1400.0f));
    // [user 2026-08-17] DECAP GORE PROPS (option 1) - break the SILHOUETTE.
    //
    // The engine cannot deform a mesh from the game module: entityState scale is a single float
    // (q_shared.h:2198) so there is no per-axis squash, no morph API is exposed in g_public.h at
    // all, and bone controllers only ROTATE a bone. Real disfigurement would need re-authored
    // geometry. What IS available is the wound-prop trick already used on bodies: hang a few
    // crossed-quad gore meshes off the head's own bones so the outline is ragged and lumpy instead
    // of a clean sphere. Combined with the gore skin and the spin, it reads as a ruined lump.
    //
    // Cost is bounded and small: at most 3 props, they are children of the head so they are freed
    // with it, and a failed attach is deleted rather than left dangling.
    {
        static cvar_t *pProps = NULL;
        const char    *bones[3] = {"Bip01 Head", "Bip01 Neck", "Bip01 Head"};
        int            want, k;

        if (!pProps) {
            pProps = gi.Cvar_Get("coop_decapGoreProps", "3", CVAR_ARCHIVE);
        }
        want = pProps->integer;
        if (want < 0) { want = 0; } else if (want > 3) { want = 3; }

        for (k = 0; k < want; k++) {
            int btag = gib->edict->tiki ? gi.Tag_NumForName(gib->edict->tiki, bones[k]) : -1;
            if (btag < 0) {
                continue;
            }
            Animate *chunk = new Animate;
            chunk->setModel("models/fx/coop_wound1.tik");
            chunk->setSolidType(SOLID_NOT);
            chunk->setScale(1.6f + G_Random(1.1f)); // varied so the three do not read as copies
            if (!chunk->attach(gib->entnum, btag, qfalse,
                               Vector(G_CRandom(3.5f), G_CRandom(3.5f), G_CRandom(3.5f)))) {
                delete chunk; // child table full - never leave it untracked
            }
        }
    }

    CoopDecapRegisterHead(gib); // bounded by count - see the ring above

    // --- neck cap: a FREE tracked slot or nothing at all ---
    neckTag = gi.Tag_NumForName(edict->tiki, "Bip01 Neck");
    if (neckTag >= 0) {
        slot = -1;
        for (i = 0; i < COOP_GORE_MAX_WOUNDPROPS; i++) {
            if (!m_pCoopWoundProp[i]) {
                slot = i;
                break;
            }
        }
        if (slot >= 0) {
            cap = new Animate;
            cap->setModel("models/fx/coop_stump_neck.tik");
            cap->setSolidType(SOLID_NOT);
            cap->setScale(1.0f);
            if (!cap->attach(entnum, neckTag, qfalse, Vector(0, 0, 0))) {
                delete cap; // child table full - never leave it untracked
            } else {
                m_pCoopWoundProp[slot] = cap;
            }
        }
    }

    // [user 2026-08-17] UNGATED, rate-limited. The success line used to sit behind coop_goreDebug,
    // so a run with the probe off could not distinguish "fired and looked wrong" from "bailed
    // silently" - which is exactly the hole the user fell into twice. 12 lines a session is nothing.
    {
        static int sTold = 0;
        if (sTold < 12) {
            sTold++;
            gi.Printf("^~^~^ DECAPTRY fired ent=%d mod=%d headsurfs=%d gibsurfs=%d headofs=(%.1f %.1f %.1f) model=%s\n",
                      entnum, meansofdeath, nHeadSurfs, numsurfaces,
                      sCoopLastHeadOfs[0], sCoopLastHeadOfs[1], sCoopLastHeadOfs[2], model.c_str());
        }
    }
    if (pDbg->integer) {
        gi.Printf("^~^~^ DECAP ent=%d mod=%d frame=%d/%d model=%s\n",
                  entnum, meansofdeath, sBudgetCount,
                  pBudget->integer > 0 ? pBudget->integer : 3, model.c_str());
    }
}

// HZM coop - gore tier 1e: the script-side mark ("gore_gibmark"). Scripted blasts that apply damage
// without an explosive MOD (bare `hurt`, MOD_CRUSH) call this on their victims just before the damage;
// dying inside the window (default 2s) counts as an explosion death. Survivors' marks expire harmlessly.
void Sentient::EventCoopBlastShield(Event *ev)
{
    m_bCoopBlastShield = (ev->NumArgs() > 0) ? (ev->GetInteger(1) != 0) : qtrue;
}

void Sentient::EventCoopGoreGibMark(Event *ev)
{
    float window = 2.0f;

    if (ev->NumArgs() > 0) {
        window = ev->GetFloat(1);
        if (window <= 0.0f) {
            window = 2.0f;
        }
    }
    m_bCoopGoreGibMark     = qtrue;
    m_fCoopGoreGibMarkTime = level.time + window;
}

// HZM coop - BLOOD TRAIL. A wounded (health below a fraction of max) AI that is MOVING drips ground
// blood splats behind it, reusing the engine's own feet-splat from AddBloodSpurt (floor trace + a 1-frame
// Decal carrying the AI's GetBloodSplatName() shader; persistence is the client's auto-recycled mark pool,
// so this is cheap and needs no cgame change). Called per-frame from Actor::Think (AI only - players are
// not Actors). Self-throttled by time + distance so droplets are spaced along the path, not a smear.
void Sentient::TryDropBloodTrail(void)
{
    cvar_t *pVar;
    str     splat;
    float   frac, interval, mindist, chance, sz, length;
    trace_t trace;
    Vector  dir, start;

    if (!com_blood->integer) {
        return;
    }

    pVar = gi.Cvar_Get("coop_bloodTrail", "1", CVAR_ARCHIVE);
    if (!pVar->integer) {
        return;
    }

    // alive only
    if (health <= 0 || max_health <= 0) {
        return;
    }

    // wounded only
    pVar = gi.Cvar_Get("coop_bloodTrailHealthFrac", "0.5", CVAR_ARCHIVE);
    frac = pVar->value;
    if (frac <= 0.0f || frac > 1.0f) { frac = 0.5f; }
    if (health > max_health * frac) {
        return;
    }

    // HZM coop [user 07-29] SEVERITY SCALING. The gates below were flat, so a man at 95% of the wound
    // threshold bled exactly as hard as one seconds from death - and a DBNO player, whom the script pins
    // at `healthonly 100` against a 750 max (13% health), dripped at the same sparse rate as a scratch.
    // Worse for DBNO specifically: the distance gate is 56 units and a downed crawl covers that slowly,
    // so the trail read as essentially absent exactly when the player is most obviously bleeding out.
    //
    // Deliberately NOT written as a DBNO special case. The engine has no per-player DBNO flag at all
    // (nothing in fgame knows about it - it is script state), so a special case would have meant new
    // engine/script plumbing and four exit paths to keep in sync, each one a chance to leave a player
    // stuck bleeding. Scaling by how hurt you are needs none of that, covers DBNO because DBNO IS the
    // low-health case, and makes ordinary wounds escalate as they worsen - which is the effect the user
    // actually described wanting.
    //
    // sev: 0 at the wound threshold, 1 at death's door. coop_bloodTrailScale 0 restores the flat gates.
    {
        float hfrac, sev, k;

        pVar = gi.Cvar_Get("coop_bloodTrailScale", "1", CVAR_ARCHIVE);
        k    = pVar->value;
        if (k < 0.0f) { k = 0.0f; }
        if (k > 1.0f) { k = 1.0f; }

        hfrac = (max_health > 0.0f) ? (health / max_health) : 1.0f;
        sev   = (frac > 0.0f) ? ((frac - hfrac) / frac) : 0.0f;
        if (sev < 0.0f) { sev = 0.0f; }
        if (sev > 1.0f) { sev = 1.0f; }
        sev *= k;

        // time gate - down to 30% of the configured interval at full severity
        pVar     = gi.Cvar_Get("coop_bloodTrailInterval", "0.45", CVAR_ARCHIVE);
        interval = pVar->value * (1.0f - 0.70f * sev);
        if (interval < 0.1f) { interval = 0.1f; }
        if (level.time < m_fNextBloodTrailTime) {
            return;
        }

        // distance gate - down to 25% at full severity, which is what makes a slow DBNO crawl leave a
        // continuous trail instead of an occasional isolated splat
        pVar    = gi.Cvar_Get("coop_bloodTrailDist", "56", CVAR_ARCHIVE);
        mindist = pVar->value * (1.0f - 0.75f * sev);
        if (mindist < 8.0f) { mindist = 8.0f; }

        m_fCoopBloodSeverity = sev; // handed to the chance roll below
    }
    if ((origin - m_vLastBloodTrailOrigin).lengthSquared() < mindist * mindist) {
        return;
    }

    // advance the gates now so a failed chance roll / missing splat doesn't retry every frame
    m_fNextBloodTrailTime   = level.time + interval;
    m_vLastBloodTrailOrigin = origin;

    splat = GetBloodSplatName();
    if (!splat.length()) {
        return; // AI with no blood splat shader simply leaves no trail
    }

    pVar   = gi.Cvar_Get("coop_bloodTrailChance", "0.8", CVAR_ARCHIVE);
    chance = pVar->value + (1.0f - pVar->value) * m_fCoopBloodSeverity; // -> 1.0 at death's door
    if (G_Random() > chance) {
        return;
    }

    // trace to the floor beneath the AI (same feet-splat geometry as AddBloodSpurt)
    start  = centroid;
    dir    = origin - centroid;
    dir.z -= 50;
    dir.x += G_CRandom(12);
    dir.y += G_CRandom(12);
    length = dir.length();
    dir.normalize();
    dir = dir * (length + 16);

    trace = G_Trace(start, vec_zero, vec_zero, start + dir, NULL, MASK_DEADSOLID, false, "BloodTrail");

    // HZM coop - blood-trail DIAGNOSTIC (coop_bloodDebug 1). The PLAYER's splat rendered as untextured white
    // wedges while the AI's IDENTICAL decal renders red, so log the inputs for BOTH to diff player vs AI:
    // who dropped it, the floor-trace result, the splat shader + its image index (0/invalid would mean an
    // unregistered shader -> white), and the geometry. Lines are ^~^~^-prefixed for qconsole.log parsing.
    {
        cvar_t *pDbg = gi.Cvar_Get("coop_bloodDebug", "0", CVAR_ARCHIVE);
        if (pDbg && pDbg->integer) {
            int imgidx = gi.imageindex(splat.c_str());
            gi.Printf(
                "^~^~^ BLOODTRAIL %s ent=%d splat='%s' imgidx=%d frac=%.2f norm=(%.2f %.2f %.2f) "
                "end=(%.0f %.0f %.0f) cen=(%.0f %.0f %.0f) org=(%.0f %.0f %.0f)\n",
                IsSubclassOfPlayer() ? "PLAYER" : "AI", entnum, splat.c_str(), imgidx, trace.fraction,
                trace.plane.normal[0], trace.plane.normal[1], trace.plane.normal[2],
                trace.endpos[0], trace.endpos[1], trace.endpos[2],
                centroid[0], centroid[1], centroid[2],
                origin[0], origin[1], origin[2]);
        }
    }

    if (trace.fraction < 1) {
        sz = GetBloodSplatSize();
        Decal *decal = new Decal;
        decal->setShader("coop_bloodsplat"); // our depth-biased (polygonOffset) red blood mark - no z-fight flicker
        // tint the mark - without a color the grayscale splat renders WHITE (the "white squares" bug). Match
        // the blood type: red for normal, green/blue for the special-fluid variants.
        if (splat == "greensplat.spr") {
            decal->setColor(0.15f, 0.45f, 0.12f);
        } else if (splat == "bluesplat.spr") {
            decal->setColor(0.12f, 0.20f, 0.55f);
        } else {
            decal->setColor(0.50f, 0.03f, 0.03f); // bloodsplat.spr -> dark blood red
        }
        decal->setOrigin(Vector(trace.endpos) + (Vector(trace.plane.normal) * 0.2f));
        decal->setDirection(trace.plane.normal);
        decal->setOrientation("random");
        decal->setRadius((sz * 0.6f) + G_Random(sz * 0.5f)); // a touch smaller than a hit-splat
    }
}

qboolean Sentient::ShouldBleed(int meansofdeath, qboolean dead)
{
    // Make sure we have a blood model

    if (!blood_model.length()) {
        return false;
    }

    // See if we can bleed now based on means of death

    switch (meansofdeath) {
        // Sometimes bleed (based on time)

    case MOD_BULLET:
    case MOD_CRUSH_EVERY_FRAME:
    case MOD_ELECTRICWATER:

        if (next_bleed_time > level.time) {
            return false;
        }

        break;

        // Sometimes bleed (based on chance)

    case MOD_SHOTGUN:

        if (G_Random() > 0.1) {
            return false;
        }

        break;

        // Never bleed

    case MOD_SLIME:
    case MOD_LAVA:
    case MOD_FIRE:
    case MOD_FLASHBANG:
    case MOD_ON_FIRE:
    case MOD_FALLING:
        return false;
    }

    // Always bleed by default

    return true;
}

// ShouldGib assumes that ShouldBleed has already been called

qboolean Sentient::ShouldGib(int meansofdeath, float damage)
{
    // See if we can gib based on means of death

    switch (meansofdeath) {
        // Always gib

    case MOD_CRUSH_EVERY_FRAME:

        return true;

        break;

        // Sometimes gib

    case MOD_BULLET:

        if (G_Random(100) < damage * 10) {
            return true;
        }

        break;

    case MOD_BEAM:

        if (G_Random(100) < damage * 5) {
            return true;
        }

        break;

        // Never gib

    case MOD_SLIME:
    case MOD_LAVA:
    case MOD_FIRE:
    case MOD_FLASHBANG:
    case MOD_ON_FIRE:
    case MOD_FALLING:
    case MOD_ELECTRICWATER:
        return false;
    }

    // Default is random based on how much damage done

    if (G_Random(100) < damage * 2) {
        return true;
    }

    return false;
}

str Sentient::GetBloodSpurtName(void)
{
    str blood_spurt_name;

    if (blood_model == "fx_bspurt.tik") {
        blood_spurt_name = "fx_bspurt2.tik";
    } else if (blood_model == "fx_gspurt.tik") {
        blood_spurt_name = "fx_gspurt2.tik";
    } else if (blood_model == "fx_bspurt_blue.tik") {
        blood_spurt_name = "fx_bspurt2_blue.tik";
    }

    return blood_spurt_name;
}

str Sentient::GetBloodSplatName(void)
{
    str blood_splat_name;

    if (blood_model == "fx_bspurt.tik") {
        blood_splat_name = "bloodsplat.spr";
    } else if (blood_model == "fx_gspurt.tik") {
        blood_splat_name = "greensplat.spr";
    } else if (blood_model == "fx_bspurt_blue.tik") {
        blood_splat_name = "bluesplat.spr";
    }

    return blood_splat_name;
}

float Sentient::GetBloodSplatSize(void)
{
    float m;

    m = mass;

    if (m < 50) {
        m = 50;
    } else if (m > 250) {
        m = 250;
    }

    // HZM coop - bigger splats so blood reads at a DISTANCE (vanilla 10-16 units was a coin-sized dot,
    // invisible past short range). Now ~20-32 units. Tunable via coop_bloodSplatScale (default 1.0).
    {
        static cvar_t *pBS = NULL;
        float          s;
        if (!pBS) { pBS = gi.Cvar_Get("coop_bloodSplatScale", "1.0", CVAR_ARCHIVE); }
        s = (pBS && pBS->value > 0.05f) ? pBS->value : 1.0f;
        return (20.0f + (m - 50) / 200.0f * 12.0f) * s;
    }
}

str Sentient::GetGibName(void)
{
    str gib_name;

    if (blood_model == "fx_bspurt.tik") {
        gib_name = "fx_rgib";
    } else if (blood_model == "fx_gspurt.tik") {
        gib_name = "fx_ggib";
    }

    return gib_name;
}

int Sentient::NumInventoryItems(void)
{
    return inventory.NumObjects();
}

Item *Sentient::NextItem(Item *item)
{
    Item    *next_item;
    int      i;
    int      n;
    qboolean item_found = false;

    if (!item) {
        item_found = true;
    } else if (!inventory.ObjectInList(item->entnum)) {
        error("NextItem", "Item not in list");
    }

    n = inventory.NumObjects();

    for (i = 1; i <= n; i++) {
        next_item = (Item *)G_GetEntity(inventory.ObjectAt(i));
        assert(next_item);

        if (next_item && next_item->isSubclassOf(InventoryItem) && item_found) {
            return next_item;
        }

        if (next_item == item) {
            item_found = true;
        }
    }

    return NULL;
}

Item *Sentient::PrevItem(Item *item)
{
    Item    *prev_item;
    int      i;
    int      n;
    qboolean item_found = false;

    if (!item) {
        item_found = true;
    } else if (!inventory.ObjectInList(item->entnum)) {
        error("NextItem", "Item not in list");
    }

    n = inventory.NumObjects();

    for (i = n; i >= 1; i--) {
        prev_item = (Item *)G_GetEntity(inventory.ObjectAt(i));
        assert(prev_item);

        if (prev_item && prev_item->isSubclassOf(InventoryItem) && item_found) {
            return prev_item;
        }

        if (prev_item == item) {
            item_found = true;
        }
    }

    return NULL;
}

void Sentient::DropInventoryItems(void)
{
    int   num;
    int   i;
    Item *item;

    if (m_bForceDropHealth) {
        giveItem("ITEMS/item_25_healthbox.tik", 25);
    } else if (skill->integer != 2 && !level.mbNoDropHealth) {
        static cvar_t *ai_health_kar  = gi.Cvar_Get("ai_health_kar", "6", CVAR_CHEAT);
        static cvar_t *ai_health_mp40 = gi.Cvar_Get("ai_health_mp40points", "2", CVAR_CHEAT);

        Weapon *weapon = GetActiveWeapon(WEAPON_MAIN);
        if (weapon) {
            if (!Q_stricmp("rifle", Director.GetString(weapon->GetWeaponGroup()))) {
                level.mHealthPopCount++;
            } else {
                level.mHealthPopCount += ai_health_mp40->integer;
            }

            if (level.mHealthPopCount >= ai_health_kar->integer) {
                giveItem("ITEMS/item_25_healthbox.tik", 25);
                level.mHealthPopCount -= ai_health_kar->integer;
            }
        }
    }

    // Drop any inventory items
    num = inventory.NumObjects();
    for (i = num; i >= 1; i--) {
        item = (Item *)G_GetEntity(inventory.ObjectAt(i));
        if (!item) { continue; } // HZM 07-19 (bug-920): stale slot guard
        // Added in 2.30
        //  Force drop the item when specified
        if (m_bForceDropWeapon && item->IsSubclassOfWeapon()) {
            item->Drop();
            continue;
        }

        if (!m_bDontDropWeapons && !level.mbNoDropWeapons) {
            item->Drop();
            continue;
        }

        if (!item->IsSubclassOfWeapon()) {
            item->Drop();
            continue;
        }

        item->Delete();
    }
}

qboolean Sentient::PowerupActive(void)
{
    if (poweruptype && this->client) {
        gi.SendServerCommand(edict - g_entities, "print \"You are already using a powerup\n\"");
    }

    return poweruptype;
}

void Sentient::setModel(const char *mdl)
{
    // Rebind all active weapons

    DetachAllActiveWeapons();
    Entity::setModel(mdl);
    AttachAllActiveWeapons();
}

void Sentient::TurnOffShadow(Event *ev)
{
    edict->s.renderfx &= ~RF_SHADOW;
}

void Sentient::TurnOnShadow(Event *ev)
{
    edict->s.renderfx |= RF_SHADOW;
}

void Sentient::Archive(Archiver& arc)
{
    int i;
    int num;

    Animate::Archive(arc);

    arc.ArchiveSafePointer(&m_pNextSquadMate);
    arc.ArchiveSafePointer(&m_pPrevSquadMate);

    inventory.Archive(arc);
    if (arc.Saving()) {
        num = ammo_inventory.NumObjects();
    } else {
        ammo_inventory.ClearObjectList();
    }
    arc.ArchiveInteger(&num);
    for (i = 1; i <= num; i++) {
        Ammo *ptr;

        if (arc.Loading()) {
            ptr = new Ammo;
            ammo_inventory.AddObject(ptr);
        } else {
            ptr = ammo_inventory.ObjectAt(i);
        }
        arc.ArchiveObject(ptr);
    }

    arc.ArchiveFloat(&LMRF);

    arc.ArchiveInteger(&poweruptype);
    arc.ArchiveInteger(&poweruptimer);

    arc.ArchiveVector(&offset_color);
    arc.ArchiveVector(&offset_delta);
    arc.ArchiveFloat(&charge_start_time);
    arc.ArchiveString(&blood_model);

    for (i = 0; i < MAX_ACTIVE_WEAPONS; i++) {
        arc.ArchiveSafePointer(&activeWeaponList[i]);
    }

    newActiveWeapon.Archive(arc);
    arc.ArchiveSafePointer(&holsteredWeapon);
    arc.ArchiveBool(&weapons_holstered_by_code);
    lastActiveWeapon.Archive(arc);

    for (int i = 0; i < MAX_DAMAGE_MULTIPLIERS; i++) {
        arc.ArchiveFloat(&m_fDamageMultipliers[i]);
    }

    arc.ArchiveSafePointer(&m_pVehicle);
    arc.ArchiveSafePointer(&m_pTurret);
    arc.ArchiveSafePointer(&m_pLadder);

    arc.ArchiveString(&m_sHelmetSurface1);
    arc.ArchiveString(&m_sHelmetSurface2);
    arc.ArchiveString(&m_sHelmetTiki);

    arc.ArchiveFloat(&m_fHelmetSpeed);

    arc.ArchiveVector(&gunoffset);
    arc.ArchiveVector(&eyeposition);
    arc.ArchiveInteger(&viewheight);
    arc.ArchiveVector(&m_vViewVariation);
    arc.ArchiveInteger(&means_of_death);

    arc.ArchiveBool(&in_melee_attack);
    arc.ArchiveBool(&in_block);
    arc.ArchiveBool(&in_stun);
    arc.ArchiveBool(&on_fire);
    arc.ArchiveFloat(&on_fire_stop_time);
    arc.ArchiveFloat(&next_catch_on_fire_time);
    arc.ArchiveInteger(&on_fire_tagnums[0]);
    arc.ArchiveInteger(&on_fire_tagnums[1]);
    arc.ArchiveInteger(&on_fire_tagnums[2]);
    arc.ArchiveSafePointer(&fire_owner);

    arc.ArchiveBool(&attack_blocked);
    arc.ArchiveFloat(&attack_blocked_time);

    arc.ArchiveFloat(&max_mouth_angle);
    arc.ArchiveInteger(&max_gibs);

    arc.ArchiveFloat(&next_bleed_time);

    arc.ArchiveBool(&m_bFootOnGround_Right);
    arc.ArchiveBool(&m_bFootOnGround_Left);

    arc.ArchiveObjectPointer((Class **)&m_NextSentient);
    arc.ArchiveObjectPointer((Class **)&m_PrevSentient);

    arc.ArchiveVector(&mTargetPos);
    arc.ArchiveFloat(&mAccuracy);

    arc.ArchiveInteger(&m_Team);
    arc.ArchiveInteger(&m_iAttackerCount);

    arc.ArchiveSafePointer(&m_pLastAttacker);
    arc.ArchiveSafePointer(&m_Enemy);

    arc.ArchiveFloat(&m_fPlayerSightLevel);

    arc.ArchiveBool(&m_bIsDisguised);
    arc.ArchiveBool(&m_bHasDisguise);

    arc.ArchiveInteger(&m_ShowPapersTime);
    arc.ArchiveInteger(&m_iLastHitTime);
    arc.ArchiveInteger(&m_iThreatBias);

    arc.ArchiveBool(&m_bDontDropWeapons);
    arc.ArchiveBool(&m_bIsAnimal);
    arc.ArchiveBool(&m_bForceDropHealth);
    arc.ArchiveBool(&m_bForceDropWeapon);

    if (arc.Loading()) {
        if (WeaponsOut()) {
            Holster(true);
        }
    }

    //
    // Added in OPM
    //
    arc.ArchiveInteger(&iNextLandTime);
}

static bool IsItemName(const char *name)
{
    if (!str::icmp(name, "models/items/camera.tik")) {
        return true;
    } else if (!str::icmp(name, "models/items/binoculars.tik")) {
        return true;
    } else if (!str::icmp(name, "models/items/papers.tik")) {
        return true;
    } else if (!str::icmp(name, "models/items/papers2.tik")) {
        return true;
    }

    return false;
}

void Sentient::ArchivePersistantData(Archiver& arc)
{
    int     i;
    int     num;
    str     name;
    int     amount;
    Item   *item;
    Entity *ent;

    // archive the inventory
    if (arc.Saving()) {
        // remove all special items before persistence
        for (i = inventory.NumObjects(); i > 0; i--) {
            int index;

            index = inventory.ObjectAt(i);
            ent   = G_GetEntity(index);
            if (!ent) { continue; } // HZM 07-19 (bug-920): stale slot guard
            name  = ent->model;

            if (IsItemName(name)) {
                ent->Delete();
            }
        }
        // count up the total number
        num = inventory.NumObjects();
    } else {
        inventory.ClearObjectList();
    }
    // archive the number
    arc.ArchiveInteger(&num);
    // archive each item
    for (i = 1; i <= num; i++) {
        if (arc.Saving()) {
            Entity *ent;

            ent = G_GetEntity(inventory.ObjectAt(i));
            if (ent && ent->isSubclassOf(Item)) {
                item   = (Item *)ent;
                name   = item->model;
                amount = item->getAmount();
            } else {
                error("ArchivePersistantData", "Non Item in inventory\n");
            }
        }

        arc.ArchiveString(&name);
        arc.ArchiveInteger(&amount);

        if (arc.Loading()) {
            item = giveItem(name, amount);
        }

        if (item && item->IsSubclassOfWeapon()) {
            Weapon *pWeap = static_cast<Weapon *>(item);

            item->CancelEventsOfType(EV_Weapon_GiveStartingAmmo);
            if (arc.Saving()) {
                amount = pWeap->ClipAmmo(FIRE_PRIMARY);
            }
            arc.ArchiveInteger(&amount);

            if (arc.Loading()) {
                pWeap->SetAmmoAmount(amount, FIRE_PRIMARY);
            }
        }
    }

    // archive the ammo inventory
    if (arc.Saving()) {
        // count up the total number
        num = ammo_inventory.NumObjects();
    } else {
        ammo_inventory.ClearObjectList();
    }
    // archive the number
    arc.ArchiveInteger(&num);
    // archive each item
    for (i = 1; i <= num; i++) {
        str   name;
        int   amount;
        int   maxamount;
        Ammo *ptr;

        if (arc.Saving()) {
            ptr       = ammo_inventory.ObjectAt(i);
            name      = ptr->getName();
            amount    = ptr->getAmount();
            maxamount = ptr->getMaxAmount();
        }

        arc.ArchiveString(&name);
        arc.ArchiveInteger(&amount);
        arc.ArchiveInteger(&maxamount);

        if (arc.Loading()) {
            GiveAmmo(name, amount, maxamount);
        }
    }

    for (i = 0; i < MAX_ACTIVE_WEAPONS; i++) {
        if (arc.Saving()) {
            if (activeWeaponList[i]) {
                name = activeWeaponList[i]->getName();
            } else {
                name = "none";
            }
        }

        arc.ArchiveString(&name);

        if (arc.Loading() && name != "none") {
            Weapon *weapon;

            weapon = (Weapon *)FindItem(name);
            if (weapon) {
                ChangeWeapon(weapon, (weaponhand_t)i);
            }
        }
    }

    if (GetActiveWeapon(WEAPON_MAIN)) {
        edict->s.eFlags &= ~EF_UNARMED;
    } else {
        edict->s.eFlags |= EF_UNARMED;
    }

    arc.ArchiveFloat(&health);
    arc.ArchiveFloat(&max_health);
}

void Sentient::DoubleArmor(void)
{
    int i, n;

    n = inventory.NumObjects();

    for (i = 1; i <= n; i++) {
        Item *item;
        item = (Item *)G_GetEntity(inventory.ObjectAt(i));

        if (!item) {
            continue; // HZM 07-20 (bug-925): stale inventory slot guard
        }
        if (item->isSubclassOf(Armor)) {
            item->setAmount(item->getAmount() * 2);
        }
    }
}

void Sentient::JumpXY(Event *ev)
{
    float  forwardmove;
    float  sidemove;
    float  distance;
    float  time;
    float  speed;
    Vector yaw_forward;
    Vector yaw_left;

    forwardmove = ev->GetFloat(1);
    sidemove    = ev->GetFloat(2);
    speed       = ev->GetFloat(3);

    Vector(0, angles.y, 0).AngleVectors(&yaw_forward, &yaw_left);

    velocity = yaw_forward * forwardmove - yaw_left * sidemove;
    distance = velocity.length();
    velocity *= speed / distance;
    time        = distance / speed;
    velocity[2] = sv_gravity->integer * time * 0.5f;
}

void Sentient::BlockStart(Event *ev)
{
    in_block = true;
}

void Sentient::BlockEnd(Event *ev)
{
    in_block = false;
}

void Sentient::StunStart(Event *ev)
{
    in_stun = true;
}

void Sentient::StunEnd(Event *ev)
{
    in_stun = false;
}

void Sentient::ListInventory(void)
{
    int i, count;

    // Display normal inventory
    count = inventory.NumObjects();

    gi.Printf("'Name' : 'Amount'\n");

    for (i = 1; i <= count; i++) {
        int   entnum = inventory.ObjectAt(i);
        Item *item   = (Item *)G_GetEntity(entnum);
        gi.Printf("'%s' : '%d'\n", item->getName().c_str(), item->getAmount());
    }

    // Display ammo inventory
    count = ammo_inventory.NumObjects();

    for (i = 1; i <= count; i++) {
        Ammo *ammo = ammo_inventory.ObjectAt(i);
        gi.Printf("'%s' : '%d'\n", ammo->getName().c_str(), ammo->getAmount());
    }
}

void Sentient::SetAttackBlocked(bool blocked)
{
    attack_blocked      = blocked;
    attack_blocked_time = level.time;
}

void Sentient::SetMaxMouthAngle(Event *ev)
{
    max_mouth_angle = ev->GetFloat(1);
}

void Sentient::TryLightOnFire(int meansofdeath, Entity *attacker)
{
    gi.Printf("Sentient::TryLightOnFire not implemented. Needs fixed");
}

void Sentient::OnFire(Event *ev)
{
    gi.Printf("Sentient::OnFire not implemented. Needs fixed");
}

void Sentient::StopOnFire(Event *ev)
{
    gi.Printf("Sentient::StopOnFire not implemented. Needs fixed");
}

void Sentient::SpawnBloodyGibs(Event *ev)
{
    str      gib_name;
    int      number_of_gibs;
    float    scale;
    Animate *ent;
    str      real_gib_name;

    if (!com_blood->integer) {
        return;
    }

    //if ( GetActorFlag( ACTOR_FLAG_FADING_OUT ) )
    //	return;

    gib_name = GetGibName();

    if (!gib_name.length()) {
        return;
    }

    // Determine the number of gibs to spawn

    if (ev->NumArgs() > 0) {
        number_of_gibs = ev->GetInteger(1);
    } else {
        if (max_gibs == 0) {
            return;
        }

        if (deadflag) {
            number_of_gibs = G_Random(max_gibs / 2) + 1;
        } else {
            number_of_gibs = G_Random(max_gibs) + 1;
        }
    }

    // Make sure we don't have too few or too many gibs

    if (number_of_gibs <= 0 || number_of_gibs > 9) {
        return;
    }

    if (ev->NumArgs() > 1) {
        scale = ev->GetFloat(2);
    } else {
        // Calculate a good scale value

        if (mass <= 50) {
            scale = 1.0f;
        } else if (mass <= 100) {
            scale = 1.1f;
        } else if (mass <= 250) {
            scale = 1.2f;
        } else {
            scale = 1.3f;
        }
    }

    // Spawn the gibs

    real_gib_name = gib_name;
    real_gib_name += number_of_gibs;
    real_gib_name += ".tik";

    ent = new Animate;
    ent->setModel(real_gib_name.c_str());
    ent->setScale(scale);
    ent->setOrigin(centroid);
    ent->NewAnim("idle");
    ent->PostEvent(EV_Remove, 1);

    Sound("snd_decap", CHAN_BODY, 1, 300);
}

void Sentient::SetMaxGibs(Event *ev)
{
    max_gibs = ev->GetInteger(1);
}

void Sentient::GetStateAnims(Container<const char *> *c) {}

void Sentient::CheckAnimations(Event *ev)
{
    int                     i, j;
    Container<const char *> co;
    const char             *cs;

    GetStateAnims(&co);

    gi.DPrintf("Unused Animations in TIKI\n");
    gi.DPrintf("-------------------------\n");
    for (i = 0; i < NumAnims(); i++) {
        const char *c;

        c = gi.Anim_NameForNum(edict->tiki, i);

        for (j = 1; j <= co.NumObjects(); j++) {
            cs = co.ObjectAt(j);

            if (!Q_stricmp(c, cs)) {
                goto out;
            } else if (!Q_stricmpn(c, cs, strlen(cs))) // partial match
            {
                size_t state_len = strlen(cs);

                // Animation in tik file is longer than the state machine's anim
                if (strlen(c) > state_len) {
                    if (c[state_len] != '_') // If next character is an '_' then no match
                    {
                        goto out;
                    }
                } else {
                    goto out;
                }
            }
        }
        // No match made
        gi.DPrintf("%s used in TIK file but not statefile\n", c);
    out:;
    }

    gi.DPrintf("Unknown Animations in Statefile\n");
    gi.DPrintf("-------------------------------\n");
    for (j = 1; j <= co.NumObjects(); j++) {
        if (!HasAnim(co.ObjectAt(j))) {
            gi.DPrintf("%s in statefile is not in TIKI\n", co.ObjectAt(j));
        }
    }
}

void Sentient::EventGerman(Event *ev)
{
    bool bRejoinSquads = false;

    if (ev->IsFromScript()) {
        if (m_Team) {
            bRejoinSquads = true;
        }
    }

    if (bRejoinSquads) {
        ClearEnemies();
        DisbandSquadMate(this);
    }

    Unlink();
    m_Team = TEAM_GERMAN;
    Link();

    if (bRejoinSquads) {
        JoinNearbySquads(1024.0f);
    }

    // Added in 2.0
    //  Tell clients about sentient team
    edict->s.eFlags &= ~EF_ALLIES;
    edict->s.eFlags |= EF_AXIS;
}

void Sentient::EventAmerican(Event *ev)
{
    bool bRejoinSquads = false;

    if (ev->IsFromScript()) {
        if (m_Team != TEAM_AMERICAN) {
            bRejoinSquads = true;
        }
    }

    if (bRejoinSquads) {
        ClearEnemies();
        DisbandSquadMate(this);
    }

    Unlink();
    m_Team = TEAM_AMERICAN;
    Link();

    if (bRejoinSquads) {
        JoinNearbySquads(1024);
    }

    if (IsSubclassOfActor()) {
        Actor *pActor        = static_cast<Actor *>(this);
        pActor->m_csMood     = STRING_NERVOUS;
        pActor->m_csIdleMood = STRING_NERVOUS;
    }

    // Added in 2.0
    //  Tell clients about sentient team
    edict->s.eFlags &= ~EF_AXIS;
    edict->s.eFlags |= EF_ALLIES;
}

void Sentient::EventGetTeam(Event *ev)
{
    if (m_Team == TEAM_AMERICAN) {
        ev->AddConstString(STRING_AMERICAN);
    } else if (m_Team == TEAM_GERMAN) {
        ev->AddConstString(STRING_GERMAN);
    } else {
        ev->AddConstString(STRING_EMPTY);
    }
}

void Sentient::ClearEnemies() {}

void Sentient::EventGetThreatBias(Event *ev)
{
    ev->AddInteger(m_iThreatBias);
}

void Sentient::EventSetThreatBias(Event *ev)
{
    str sBias;

    if (ev->IsStringAt(1)) {
        sBias = ev->GetString(1);

        if (!Q_stricmp(sBias, "ignoreme")) {
            m_iThreatBias = THREATBIAS_IGNOREME;
            return;
        }
    }

    m_iThreatBias = ev->GetInteger(1);
}

void Sentient::SetDamageMult(Event *ev)
{
    int index = ev->GetInteger(1);
    if (index < 0 || index >= MAX_DAMAGE_MULTIPLIERS) {
        ScriptError("Index must be between 0-" STRING(MAX_DAMAGE_MULTIPLIERS - 1) ".");
    }

    m_fDamageMultipliers[index] = ev->GetFloat(2);
}

void Sentient::SetupHelmet(str sHelmetTiki, float fSpeed, float fDamageMult, str sHelmetSurface1, str sHelmetSurface2)
{
    m_sHelmetTiki     = sHelmetTiki;
    m_sHelmetSurface1 = sHelmetSurface1;
    m_sHelmetSurface2 = sHelmetSurface2;

    m_fHelmetSpeed          = fSpeed;
    m_fDamageMultipliers[1] = fDamageMult;
}

void Sentient::EventSetupHelmet(Event *ev)
{
    str sHelmetTiki;
    str sHelmetSurface;

    sHelmetTiki    = ev->GetString(1);
    sHelmetSurface = ev->GetString(4);

    if (ev->NumArgs() == 4) {
        SetupHelmet(sHelmetTiki, ev->GetFloat(2), ev->GetFloat(3), sHelmetSurface, sHelmetSurface);
    } else {
        SetupHelmet(sHelmetTiki, ev->GetFloat(2), ev->GetFloat(3), sHelmetSurface, ev->GetString(5));
    }
}

bool Sentient::WearingHelmet(void)
{
    if (!m_sHelmetSurface1.length()) {
        return false;
    }

    int iSurf = gi.Surface_NameToNum(edict->tiki, m_sHelmetSurface1);
    if (iSurf >= 0) {
        return (~edict->s.surfaces[iSurf] & MDL_SURFACE_NODRAW) != 0;
    } else {
        return false;
    }
}

void Sentient::EventPopHelmet(Event *ev)
{
    int           iSurf;
    vec3_t        vWorldAngles;
    vec3_t        vXAxis, vYAxis, vZAxis;
    orientation_t oLocalTag, oWorldTag;
    int           iHeadTag;
    float         fRandom;
    float         fPitchVelocity, fYawVelocity;
    HelmetObject *obj;

    if (!WearingHelmet()) {
        return;
    }

    iSurf = gi.Surface_NameToNum(edict->tiki, m_sHelmetSurface1.c_str());
    // Hide the helmet
    edict->s.surfaces[iSurf] |= MDL_SURFACE_NODRAW;

    if (m_sHelmetSurface2.length()) {
        iSurf = gi.Surface_NameToNum(edict->tiki, m_sHelmetSurface2.c_str());
        if (iSurf >= 0) {
            // Hide the second helmet
            edict->s.surfaces[iSurf] |= MDL_SURFACE_NODRAW;
        } else {
            Com_Printf(
                "Warning: Surface %s found, but %s not found in setting up helmet for %s.\n",
                m_sHelmetSurface1.c_str(),
                m_sHelmetSurface2.c_str(),
                edict->tiki->name
            );
        }
    }

    if (!m_sHelmetTiki.length()) {
        return;
    }

    iHeadTag  = gi.Tag_NumForName(edict->tiki, "Bip01 Head");
    oLocalTag = G_TIKI_Orientation(edict, iHeadTag);

    for (int i = 0; i < 3; i++) {
        vXAxis[i] = oLocalTag.axis[0][i];
        vYAxis[i] = oLocalTag.axis[1][i];
        vZAxis[i] = oLocalTag.axis[2][i];
    }

    for (int i = 0; i < 3; i++) {
        oLocalTag.axis[0][i] = -vYAxis[i];
        oLocalTag.axis[1][i] = -vZAxis[i];
        oLocalTag.axis[2][i] = vXAxis[i];
    }

    VectorCopy(origin, oWorldTag.origin);

    for (int i = 0; i < 3; i++) {
        VectorMA(oWorldTag.origin, oLocalTag.origin[i], orientation[i], oWorldTag.origin);
    }

    MatrixMultiply(oLocalTag.axis, orientation, oWorldTag.axis);
    MatrixToEulerAngles(oWorldTag.axis, vWorldAngles);

    obj = new HelmetObject();
    obj->setOrigin(oWorldTag.origin);
    obj->setAngles(vWorldAngles);
    obj->setModel(m_sHelmetTiki);

    fRandom = crandom() * 30;
    // HZM coop: was VectorScale(obj->velocity, fRandom, oWorldTag.axis[0]) - scaled the zero
    // velocity INTO the axis, losing this lateral component entirely. Mirror the axis[1] line.
    VectorMA(obj->velocity, fRandom, oWorldTag.axis[0], obj->velocity);

    fRandom = crandom() * 30;
    VectorMA(obj->velocity, fRandom, oWorldTag.axis[1], obj->velocity);

    fRandom = (crandom() * 0.3f + 1.0f) * m_fHelmetSpeed;
    VectorMA(obj->velocity, fRandom, oWorldTag.axis[2], obj->velocity);

    fPitchVelocity = crandom() * 300;
    fYawVelocity   = crandom() * 400;

    obj->avelocity.x = fPitchVelocity;
    obj->avelocity.y = fYawVelocity;
    obj->avelocity.z = crandom() * 300.0;
}

void Sentient::ReceivedItem(Item *item)
{
    // HZM coop - weapons-on-back: a weapon given but never drawn should still show holstered
    if (item && item->IsSubclassOfWeapon()) {
        UpdateCoopHolsteredWeapons();
    }
}

void Sentient::RemovedItem(Item *item)
{
    // HZM coop - weapons-on-back: refill the spot a dropped/taken weapon vacated
    if (item && item->IsSubclassOfWeapon()) {
        UpdateCoopHolsteredWeapons();
    }
}

void Sentient::AssertValidSquad()
{
    for (Sentient *pSquadMate = this; pSquadMate != this; pSquadMate = pSquadMate->m_pNextSquadMate) {
        assert(pSquadMate->m_pNextSquadMate);
        assert(pSquadMate->m_pPrevSquadMate);
    }
}

bool Sentient::IsTeamMate(Sentient *pOther)
{
    // HZM coop: NULL guard. Actor::MoveOnPathWithSquad walks the squad ring and can hand a
    // NULL/dangling mate here when an actor was removed without a ring unlink (this crashed
    // EVERY machine at client connect on m1l2a/m1l2b/m1l3c once patrol AI activated - the
    // "map load crash" trio; bug-242 root cause, corrected from the earlier OOM misdiagnosis).
    // A missing mate is simply not a team mate.
    if (!pOther) {
        return false;
    }
    return (pOther->m_bIsDisguised || pOther->m_Team == m_Team);
}

void Sentient::JoinNearbySquads(float fJoinRadius)
{
    float fJoinRadiusSquared = Square(fJoinRadius);

    for (Sentient *pFriendly = level.m_HeadSentient[m_Team]; pFriendly != NULL; pFriendly = pFriendly->m_NextSentient) {
        if (pFriendly->IsDead() || IsSquadMate(pFriendly) || pFriendly->m_Team != m_Team) {
            continue;
        }

        if (fJoinRadius >= Vector::DistanceSquared(pFriendly->origin, origin)) {
            MergeWithSquad(pFriendly);
        }
    }
}

void Sentient::MergeWithSquad(Sentient *pFriendly)
{
    Sentient *pFriendNext;
    Sentient *pSelfPrev;

    if (!pFriendly || IsDead() || pFriendly->IsDead()) {
        return;
    }

    pFriendNext = pFriendly->m_pNextSquadMate;
    pSelfPrev   = m_pPrevSquadMate;

    pFriendly->m_pNextSquadMate = this;
    m_pPrevSquadMate            = pFriendly;

    pFriendNext->m_pPrevSquadMate = pSelfPrev;
    pSelfPrev->m_pNextSquadMate   = pFriendNext;
}

void Sentient::DisbandSquadMate(Sentient *pExFriendly)
{
    Sentient *pPrev;
    Sentient *pNext;

    AssertValidSquad();

    pPrev = pExFriendly->m_pPrevSquadMate;
    pNext = pExFriendly->m_pNextSquadMate;

    pPrev->m_pNextSquadMate = pNext;
    pNext->m_pPrevSquadMate = pPrev;

    pExFriendly->m_pPrevSquadMate = pExFriendly;
    pExFriendly->m_pNextSquadMate = pExFriendly;

    AssertValidSquad();
    pNext->AssertValidSquad();
}

bool Sentient::IsSquadMate(Sentient *pFriendly)
{
    Sentient *pSquadMate = this;

    while (1) {
        if (pSquadMate == pFriendly) {
            return true;
        }

        pSquadMate = pSquadMate->m_pNextSquadMate;
        if (pSquadMate == this) {
            return false;
        }
    }
}

bool Sentient::IsDisabled() const
{
    return false;
}

VehicleTank *Sentient::GetVehicleTank(void)
{
    if (m_pVehicle && m_pVehicle->IsSubclassOfVehicleTank()) {
        return (VehicleTank *)m_pVehicle.Pointer();
    } else {
        return NULL;
    }
}

void Sentient::UpdateFootsteps(void)
{
    int iAnimNum;
    int iAnimFlags;
    int iTagNum;

    iAnimFlags = 0;

    for (iAnimNum = 0; iAnimNum < MAX_FRAMEINFOS; iAnimNum++) {
        if (edict->s.frameInfo[iAnimNum].weight != 0 && CurrentAnim(iAnimNum) >= 0) {
            iAnimFlags |= gi.Anim_Flags(edict->tiki, CurrentAnim(iAnimNum));
        }
    }

    if (!(iAnimFlags & TAF_AUTOSTEPS_RUNNING) || !(iAnimFlags & TAF_AUTOSTEPS)) {
        // if walking, or if the animation doesn't step
        m_bFootOnGround_Right = true;
        m_bFootOnGround_Left  = true;
        return;
    }

    if (m_bFootOnGround_Right) {
        iTagNum = gi.Tag_NumForName(edict->tiki, "Bip01 R Foot");
        if (iTagNum >= 0) {
            m_bFootOnGround_Right = G_TIKI_IsOnGround(edict, iTagNum, 13.653847f);
        } else {
            m_bFootOnGround_Right = true;
        }
    } else {
        iTagNum = gi.Tag_NumForName(edict->tiki, "Bip01 R Foot");
        if (iTagNum >= 0) {
            if (G_TIKI_IsOnGround(edict, iTagNum, 13.461539f)) {
                BroadcastAIEvent(AI_EVENT_FOOTSTEP, G_AIEventRadius(AI_EVENT_FOOTSTEP));
                // simulate footstep sounds
                Footstep("Bip01 L Foot", (iAnimFlags & TAF_AUTOSTEPS_RUNNING), (iAnimFlags & TAF_AUTOSTEPS_EQUIPMENT));
                m_bFootOnGround_Right = true;
            }
        } else {
            m_bFootOnGround_Right = true;
        }
    }

    if (m_bFootOnGround_Left) {
        iTagNum = gi.Tag_NumForName(edict->tiki, "Bip01 L Foot");
        if (iTagNum >= 0) {
            m_bFootOnGround_Left = G_TIKI_IsOnGround(edict, iTagNum, 13.653847f);
        } else {
            m_bFootOnGround_Left = true;
        }
    } else {
        iTagNum = gi.Tag_NumForName(edict->tiki, "Bip01 L Foot");
        if (iTagNum >= 0) {
            if (G_TIKI_IsOnGround(edict, iTagNum, 13.461539f)) {
                BroadcastAIEvent(AI_EVENT_FOOTSTEP, G_AIEventRadius(AI_EVENT_FOOTSTEP));
                // simulate footstep sounds
                Footstep("Bip01 R Foot", (iAnimFlags & TAF_AUTOSTEPS_RUNNING), (iAnimFlags & TAF_AUTOSTEPS_EQUIPMENT));
                m_bFootOnGround_Left = true;
            }
        } else {
            m_bFootOnGround_Left = true;
        }
    }
}

qboolean Sentient::AIDontFace() const
{
    return qfalse;
}

void Sentient::EventDropItems(Event *ev)
{
    DropInventoryItems();
}

void Sentient::EventDontDropWeapons(Event *ev)
{
    if (ev->NumArgs() > 0) {
        m_bDontDropWeapons = ev->GetBoolean(1);
    } else {
        m_bDontDropWeapons = true;
    }
}

void Sentient::EventForceDropWeapon(Event *ev)
{
    if (ev->NumArgs() > 0) {
        m_bForceDropWeapon = ev->GetBoolean(1);
    } else {
        m_bForceDropWeapon = true;
    }
}

void Sentient::EventForceDropHealth(Event *ev)
{
    if (ev->NumArgs() > 0) {
        m_bForceDropHealth = ev->GetBoolean(1);
    } else {
        m_bForceDropHealth = true;
    }
}

void Sentient::EventGetForceDropWeapon(Event *ev)
{
    ev->AddInteger(m_bForceDropWeapon);
}

void Sentient::EventGetForceDropHealth(Event *ev)
{
    ev->AddInteger(m_bForceDropHealth);
}

void Sentient::SetViewAngles(Vector angles) {}

void Sentient::SetTargetViewAngles(Vector angles) {}

Vector Sentient::GetViewAngles(void)
{
    return angles;
}

void Sentient::AddViewVariation(const Vector& vVariation)
{
    m_vViewVariation += vVariation;
}

void Sentient::SetMinViewVariation(const Vector& vVariation)
{
    m_vViewVariation.x = Q_min(m_vViewVariation.x, vVariation.x);
    m_vViewVariation.y = Q_min(m_vViewVariation.y, vVariation.y);
    m_vViewVariation.z = Q_min(m_vViewVariation.z, vVariation.z);
}

void Sentient::SetHolsteredByCode(bool holstered)
{
    weapons_holstered_by_code = holstered;
}

Vehicle *Sentient::GetVehicle() const
{
    return m_pVehicle;
}

void Sentient::SetVehicle(Vehicle *pVehicle)
{
    m_pVehicle = NULL;
}

TurretGun *Sentient::GetTurret() const
{
    return m_pTurret;
}

void Sentient::SetTurret(TurretGun *pTurret)
{
    m_pTurret = pTurret;
}

Entity *Sentient::GetLadder() const
{
    return m_pLadder;
}

#define GROUND_DISTANCE        8
#define WATER_NO_SPLASH_HEIGHT 16

void Sentient::EventClientLanding(Event *ev)
{
    float fVolume    = ev->NumArgs() >= 1 ? ev->GetFloat(1) : 1;
    int   iEquipment = ev->NumArgs() >= 2 ? ev->GetInteger(2) : 1;

    LandingSound(fVolume, iEquipment);
}

void Sentient::FootstepMain(trace_t *trace, int iRunning, int iEquipment)
{
    int    contents;
    int    surftype;
    float  fVolume;
    vec3_t vPos;
    vec3_t midlegs;
    str    sSoundName;

    VectorCopy(trace->endpos, vPos);
    sSoundName = "snd_step_";

    contents = gi.pointcontents(trace->endpos, -1);
    if (contents & MASK_WATER) {
        // take our ground position and trace upwards
        VectorCopy(trace->endpos, midlegs);
        midlegs[2] += WATER_NO_SPLASH_HEIGHT;
        contents = gi.pointcontents(midlegs, -1);
        if (contents & MASK_WATER) {
            sSoundName += "wade";
        } else {
            sSoundName += "puddle";
        }
    } else {
        surftype = trace->surfaceFlags & MASK_SURF_TYPE;
        switch (surftype) {
        case SURF_FOLIAGE:
            sSoundName += "foliage";
            break;
        case SURF_SNOW:
            sSoundName += "snow";
            break;
        case SURF_CARPET:
            sSoundName += "carpet";
            break;
        case SURF_SAND:
            sSoundName += "sand";
            break;
        case SURF_PUDDLE:
            sSoundName += "puddle";
            break;
        case SURF_GLASS:
            sSoundName += "glass";
            break;
        case SURF_GRAVEL:
            sSoundName += "gravel";
            break;
        case SURF_MUD:
            sSoundName += "mud";
            break;
        case SURF_DIRT:
            sSoundName += "dirt";
            break;
        case SURF_GRILL:
            sSoundName += "grill";
            break;
        case SURF_GRASS:
            sSoundName += "grass";
            break;
        case SURF_ROCK:
            sSoundName += "stone";
            break;
        case SURF_PAPER:
            sSoundName += "paper";
            break;
        case SURF_WOOD:
            sSoundName += "wood";
            break;
        case SURF_METAL:
            sSoundName += "metal";
            break;
        default:
            sSoundName += "stone";
            break;
        }
    }

    if (iRunning) {
        if (iRunning == -1) {
            fVolume = 0.5;
        } else {
            fVolume = 1.0;
        }
    } else {
        fVolume = 0.25;
    }

    if (!iRunning && g_gametype->integer == GT_SINGLE_PLAYER) {
        return;
    }

    PlayNonPvsSound(sSoundName, fVolume);

    if (iEquipment && random() < 0.3) {
        // also play equipment sound
        PlayNonPvsSound("snd_step_equipment", fVolume);
    }
}

void Sentient::Footstep(const char *szTagName, int iRunning, int iEquipment)
{
    int           i;
    int           iTagNum;
    vec3_t        vStart, vEnd;
    vec3_t        midlegs;
    vec3_t        vMins, vMaxs;
    str           sSoundName;
    trace_t       trace;
    orientation_t oTag;

    // send a trace down from the player to the ground
    VectorCopy(this->origin, vStart);
    vStart[2] += GROUND_DISTANCE;

    if (szTagName) {
        iTagNum = gi.Tag_NumForName(this->edict->tiki, szTagName);
        if (iTagNum != -1) {
            oTag = G_TIKI_Orientation(this->edict, iTagNum);

            for (i = 0; i < 2; i++) {
                VectorMA(vStart, oTag.origin[i], this->orientation[i], vStart);
            }
        }
    }

    if (iRunning == -1) {
        AngleVectors(this->angles, midlegs, NULL, NULL);
        VectorMA(vStart, -16, midlegs, vStart);
        VectorMA(vStart, 64, midlegs, vEnd);

        VectorSet(vMins, -2, -2, -8);
        VectorSet(vMaxs, 2, 2, 8);
    } else {
        VectorSet(vMins, -4, -4, 0);
        VectorSet(vMaxs, 4, 4, 2);

        // add 16 units above feets
        vStart[2] += 16.0;
        VectorCopy(vStart, vEnd);
        vEnd[2] -= 64.0;
    }

    if (IsSubclassOfPlayer()) {
        trace = G_Trace(vStart, vMins, vMaxs, vEnd, edict, MASK_PLAYERSOLID, qtrue, "Player Footsteps");
    } else {
        trace = G_Trace(vStart, vMins, vMaxs, vEnd, edict, MASK_MONSTERSOLID, qfalse, "Monster Footsteps");
    }

    if (trace.fraction == 1.0f) {
        return;
    }

    FootstepMain(&trace, iRunning, iEquipment);
}

void Sentient::LandingSound(float volume, int iEquipment)
{
    int           contents;
    int           surftype;
    vec3_t        vStart, vEnd;
    vec3_t        midlegs;
    str           sSoundName;
    trace_t       trace;
    static vec3_t g_vFootstepMins = {-4, -4, 0};
    static vec3_t g_vFootstepMaxs = {4, 4, 2};

    if (this->iNextLandTime > level.inttime) {
        this->iNextLandTime = level.inttime + 200;
        return;
    }

    this->iNextLandTime = level.time + 200;
    VectorCopy(this->origin, vStart);
    vStart[2] += GROUND_DISTANCE;

    VectorCopy(vStart, vEnd);
    vEnd[2] -= 64.0;

    if (IsSubclassOfPlayer()) {
        trace =
            G_Trace(vStart, g_vFootstepMins, g_vFootstepMaxs, vEnd, edict, MASK_PLAYERSOLID, qtrue, "Player Footsteps");
    } else {
        trace = G_Trace(
            vStart, g_vFootstepMins, g_vFootstepMaxs, vEnd, edict, MASK_MONSTERSOLID, qfalse, "Monster Footsteps"
        );
    }

    if (trace.fraction == 1.0) {
        return;
    }

    sSoundName += "snd_landing_";

    contents = gi.pointcontents(trace.endpos, -1);
    if (contents & MASK_WATER) {
        // take our ground position and trace upwards
        VectorCopy(trace.endpos, midlegs);
        midlegs[2] += WATER_NO_SPLASH_HEIGHT;
        contents = gi.pointcontents(midlegs, -1);
        if (contents & MASK_WATER) {
            sSoundName += "wade";
        } else {
            sSoundName += "puddle";
        }
    } else {
        surftype = trace.surfaceFlags & MASK_SURF_TYPE;
        switch (surftype) {
        case SURF_FOLIAGE:
            sSoundName += "foliage";
            break;
        case SURF_SNOW:
            sSoundName += "snow";
            break;
        case SURF_CARPET:
            sSoundName += "carpet";
            break;
        case SURF_SAND:
            sSoundName += "sand";
            break;
        case SURF_PUDDLE:
            sSoundName += "puddle";
            break;
        case SURF_GLASS:
            sSoundName += "glass";
            break;
        case SURF_GRAVEL:
            sSoundName += "gravel";
            break;
        case SURF_MUD:
            sSoundName += "mud";
            break;
        case SURF_DIRT:
            sSoundName += "dirt";
            break;
        case SURF_GRILL:
            sSoundName += "grill";
            break;
        case SURF_GRASS:
            sSoundName += "grass";
            break;
        case SURF_ROCK:
            sSoundName += "stone";
            break;
        case SURF_PAPER:
            sSoundName += "paper";
            break;
        case SURF_WOOD:
            sSoundName += "wood";
            break;
        case SURF_METAL:
            sSoundName += "metal";
            break;
        default:
            sSoundName += "stone";
            break;
        }
    }

    PlayNonPvsSound(sSoundName, volume);

    if (iEquipment && random() < 0.5) {
        PlayNonPvsSound("snd_step_equipment", volume);
    }
}
