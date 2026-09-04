/*
===========================================================================
Copyright (C) 2015 the OpenMoHAA team

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

// sentient.h: Base class of entity that can carry other entities, and use weapons.
//

#pragma once

#include "g_local.h"
#include "container.h"
#include "animate.h"
#include "vehicle.h"

extern Event EV_Sentient_Attack;
extern Event EV_Sentient_Charge;
extern Event EV_Sentient_ReleaseAttack;
extern Event EV_Sentient_GiveWeapon;
extern Event EV_Sentient_GiveAmmo;
extern Event EV_Sentient_GiveArmor;
extern Event EV_Sentient_GiveItem;
extern Event EV_Sentient_GiveTargetname;
extern Event EV_Sentient_GiveInventoryItem;
extern Event EV_Sentient_GiveHealth;
extern Event EV_Sentient_SetBloodModel;
extern Event EV_Sentient_UselessCheck;
extern Event EV_Sentient_TurnOffShadow;
extern Event EV_Sentient_TurnOnShadow;
extern Event EV_Sentient_UpdateOffsetColor;
extern Event EV_Sentient_JumpXY;
extern Event EV_Sentient_MeleeAttackStart;
extern Event EV_Sentient_MeleeAttackEnd;
extern Event EV_Sentient_BlockStart;
extern Event EV_Sentient_BlockEnd;
extern Event EV_Sentient_SetMouthAngle;
extern Event EV_Sentient_SpawnBloodyGibs;
extern Event EV_Sentient_StopOnFire;
extern Event EV_Sentient_UseItem;
extern Event EV_Sentient_UseLastWeapon;
extern Event EV_Sentient_UseWeaponClass;
extern Event EV_Sentient_ToggleItemUse;
extern Event EV_Sentient_DontDropWeapons;
extern Event EV_Sentient_ForceDropWeapon;
extern Event EV_Sentient_ForceDropHealth;
extern Event EV_Sentient_GetForceDropHealth;
extern Event EV_Sentient_GetForceDropWeapon;

// Shutup compiler
class Weapon;
class Item;
class InventoryItem;
class Ammo;
class Vehicle;
class VehicleTank;
class TurretGun;

#define MAX_ACTIVE_WEAPONS     NUM_ACTIVE_WEAPONS
#define MAX_DAMAGE_MULTIPLIERS 19

#define TEAM_GERMAN            0
#define TEAM_AMERICAN          1

#define THREATBIAS_IGNOREME    0xFFFFE4C7

typedef SafePtr<Weapon> WeaponPtr;

class ActiveWeapon : public Class
{
public:
    WeaponPtr    weapon;
    weaponhand_t hand;
    ActiveWeapon();
    void Archive(Archiver& arc);
};

class Sentient : public Animate
{
protected:
    Container<int>     inventory;
    Container<Ammo *>  ammo_inventory;
    float              LMRF;
    WeaponPtr          newWeapon;
    int                poweruptype;
    int                poweruptimer;
    Vector             offset_color;
    Vector             offset_delta;
    float              charge_start_time;
    str                blood_model;
    SafePtr<Weapon>    activeWeaponList[MAX_ACTIVE_WEAPONS];
    ActiveWeapon       newActiveWeapon;
    WeaponPtr          holsteredWeapon;
    bool               weapons_holstered_by_code;
    ActiveWeapon       lastActiveWeapon;
    float              m_fDamageMultipliers[MAX_DAMAGE_MULTIPLIERS];
    SafePtr<Vehicle>   m_pVehicle;
    SafePtr<TurretGun> m_pTurret;
    SafePtr<Entity>    m_pLadder;
    str                m_sHelmetSurface1;
    str                m_sHelmetSurface2;
    str                m_sHelmetTiki;
    float              m_fHelmetSpeed;
    bool               m_bDontDropWeapons;

    virtual void EventTake(Event *ev);
    virtual void EventGiveAmmo(Event *ev);
    void         EventGetAmmo(Event *ev);	// HZM coop - script ammo getter (exact-ammo respawn)
    void         EventSetAmmo(Event *ev);	// HZM coop - script ammo setter, exact (exact-ammo respawn)
    virtual void EventGiveItem(Event *ev);
    void         EventGiveDynItem(Event *ev);
    void         SetBloodModel(Event *ev);
    void         EventUseItem(Event *ev);
    virtual void EventUseWeaponClass(Event *ev);
    virtual void EventGiveTargetname(Event *ev);

    void         EventGerman(Event *ev);
    void         EventAmerican(Event *ev);
    void         EventGetTeam(Event *ev);
    virtual void ClearEnemies();
    void         EventGetThreatBias(Event *ev);
    void         EventSetThreatBias(Event *ev);
    void         SetDamageMult(Event *ev);
    void SetupHelmet(str sHelmetTiki, float fSpeed, float fDamageMult, str sHelmetSurface1, str sHelmetSurface2);
    void EventSetupHelmet(Event *ev);
    void EventPopHelmet(Event *ev);
    bool WearingHelmet(void);
    int  CheckHitLocation(int iLocation);

    virtual void     ArmorDamage(Event *ev);
    virtual qboolean CanBlock(int meansofdeath, qboolean full_block);
    void             AddBloodSpurt(Vector direction);
    void             TryDropBloodTrail(void); // HZM coop - wounded+moving AI drip ground blood splats
    void             DropBloodPool(void);     // HZM coop - persistent blood pool under a killed sentient
    void             CoopGoreTryDripAttach(qboolean corpse); // HZM coop - gore tier 2: attach looping drip FX
    void             EventCoopGorePoolGrow(Event *ev);       // HZM coop - gore tier 2: growing corpse pool step
    void             CoopGoreUpdateSkinTier(void);           // HZM coop - gore tier 1: flip blood-skin bits
    void             EventCoopGoreReset(Event *ev);          // HZM coop - gore tier 1: script heal hook
    void             CoopGoreTryGibSkins(int meansofdeath, Entity *inflictor); // HZM coop - gore tier 1e: extreme explosion-death skins (index 3)
    void             EventCoopGoreGibMark(Event *ev);        // HZM coop - gore tier 1e: script mark for scripted blasts
    void             EventCoopGoreDecapMark(Event *ev);      // HZM coop [user 2026-09-03] - script-forced decapitation (authored set piece)
    void             EventCoopBlastShield(Event *ev);        // HZM coop - bug-1586: mission-critical blast immunity
    void             CoopGoreTryWoundProp(int location, int meansofdeath, const Vector &position); // HZM coop - gore tier 3: wound prop at the hit point (bug-735: entry-point attach)
    void             CoopHeadshotKillFx(const Vector &pos, const Vector &dir); // HZM coop - guaranteed burst+wall-splat on a confirmed headshot kill
    void             CoopGoreDisfigureHead(void);             // HZM coop [user 2026-08-17] - headshot kill: force the HEAD surface to the gib-tier face
    void             CoopGoreThrowChunks(const Vector &pos, const Vector &dir, int n, const char *pszTik, float fScale); // HZM coop [user 2026-08-19] budgeted meat-chunk spray
    void             CoopGoreHeadshotExtras(const Vector &pos, const Vector &dir); // HZM coop [user 2026-08-19] brain chunks + dangling eyeball
    void             CoopGoreDeathKinetics(const Vector &pos, const Vector &dir, float fDamage, int mod, Entity *inflictor); // HZM coop [user 2026-08-19] corpse impulse + explosion decap/chunks
    void             CoopGoreTryDecapitate(int meansofdeath, Entity *inflictor); // HZM coop [user 2026-08-17] - blast/shotgun decapitation
    void             CoopGoreCorpseDamage(Event *ev); // HZM coop [user 2026-08-20] bug-1975 - the DEAD half of ArmorDamage: gore only, no health/pain/death
    qboolean         ShouldBleed(int meansofdeath, qboolean dead);
    qboolean         ShouldGib(int meansofdeath, float damage);
    str              GetBloodSpurtName(void);
    str              GetBloodSplatName(void);
    float            GetBloodSplatSize(void);
    str              GetGibName(void);
    virtual void     TurnOffShadow(Event *ev);
    virtual void     TurnOnShadow(Event *ev);
    virtual void     WeaponKnockedFromHands(void);

    void         EventDropItems(Event *ev);
    void         EventDontDropWeapons(Event *ev);
    void         EventForceDropWeapon(Event *ev);
    void         EventForceDropHealth(Event *ev);
    void         EventGetForceDropWeapon(Event *ev);
    void         EventGetForceDropHealth(Event *ev);
    void         DetachAllActiveWeapons(void);
    void         AttachAllActiveWeapons(void);
    qboolean     WeaponsOut(void);
    qboolean     IsActiveWeapon(Weapon *weapon);
    void         ActivateWeapon(Weapon *weapon, weaponhand_t hand);
    void         ActivateLastActiveWeapon(void);
    void         EventActivateLastActiveWeapon(Event *ev);
    void         EventToggleItemUse(Event *ev);
    void         DeactivateWeapon(Weapon *weapon);
    void         DeactivateWeapon(weaponhand_t hand);
    void         CheckAnimations(Event *ev);
    void         ChargeWeapon(weaponhand_t hand, firemode_t mode);
    virtual void FireWeapon(int number, firemode_t mode);
    void         ReleaseFireWeapon(int number, firemode_t mode);
    void         Link();
    void         Unlink();

public:
    Vector            mTargetPos;
    float             mAccuracy;
    SafePtr<Sentient> m_pNextSquadMate;
    SafePtr<Sentient> m_pPrevSquadMate;
    Sentient         *m_NextSentient;
    Sentient         *m_PrevSentient;
    int               m_Team;
    int               m_iAttackerCount;
    SafePtr<Entity>   m_pLastAttacker;
    SafePtr<Sentient> m_Enemy;
    float             m_fPlayerSightLevel;
    bool              m_bIsDisguised;
    bool              m_bHasDisguise;
    bool              m_bOvercookDied;
    int               m_ShowPapersTime;
    int               m_iLastHitTime;
    int               m_iThreatBias;
    bool              m_bIsAnimal;
    Vector            gunoffset;
    Vector            eyeposition;
    int               viewheight;
    Vector            m_vViewVariation;
    int               means_of_death;
    bool              in_melee_attack;
    bool              in_block;
    bool              in_stun;
    bool              on_fire;
    float             on_fire_stop_time;
    float             next_catch_on_fire_time;
    int               on_fire_tagnums[3];
    SafePtr<Entity>   fire_owner;
    bool              attack_blocked;
    float             attack_blocked_time;
    float             max_mouth_angle;
    int               max_gibs;
    float             next_bleed_time;
    float             m_fCoopBloodSeverity;    // HZM coop [user 07-29] 0..1 wound severity, scales the blood-trail gates
    float             m_fNextBloodTrailTime;   // HZM coop - blood-trail throttle (time gate)
    Vector            m_vLastBloodTrailOrigin; // HZM coop - blood-trail throttle (distance gate)
    float             m_fCoopGoreDamage;       // HZM coop - gore tier 2: ACCUMULATED applied damage (aihandler
                                               // fakes AI health at 5000, so gore tiers key on damage taken,
                                               // never on health fraction - see CoopGoreTryDripAttach)
    SafePtr<Entity>   m_pCoopDripEmitter;      // HZM coop - gore tier 2: attached looping drip FX (NULL = none)
    int               m_iCoopGoreSkinTier;     // HZM coop - gore tier 1: current blood-skin index (0/1/2;
                                               // 3 = gore tier 1e extreme explosion-death corpse, terminal)
    qboolean          m_bCoopGoreGibMark;      // HZM coop - gore tier 1e: script flagged us inside a scripted blast
    qboolean          m_bCoopBlastShield;      // HZM coop - bug-1586: opt-in immunity to world-attributed blasts
    float             m_fCoopGoreGibMarkTime;  // HZM coop - gore tier 1e: level.time the script mark expires
    qboolean          m_bCoopGoreDecapForce;   // HZM coop [user 2026-09-03]: a script demanded THIS death take the head off
    float             m_fCoopGoreDecapForceTime; // HZM coop [user 2026-09-03]: level.time the forced-decap mark expires
    void              CoopGoreHeal(float amount); // HZM coop - gore tier 1: healed -> reduce gore, retier
    Vector            m_vCoopPoolPos;          // HZM coop - gore tier 2: floor point of the growing corpse pool
    Vector            m_vCoopPoolNormal;       // HZM coop - gore tier 2: floor normal of the growing corpse pool
    int               m_iCoopPoolGen;          // HZM coop - gore tier 2 (bug-817): this pool's start ordinal;
                                               // only the 6 newest chains keep creeping (decal-budget cap)
    SafePtr<Entity>   m_pCoopWoundProp[12];    // HZM coop - gore tier 3: attached hit-location wound props
                                               // [user 2026-08-17] 4 -> 12. Four meant an SMG burst left four
                                               // holes and then nothing; see CoopGoreTryWoundProp.
    int               m_iCoopWoundNext;        // HZM coop [user 2026-08-17] - round-robin recycle cursor
                                               // (slot count = COOP_GORE_MAX_WOUNDPROPS in sentient.cpp;
                                               // SafePtr auto-NULLs when a prop is freed with its body)
    qboolean          m_bCoopHeadGore;         // HZM coop [user 2026-08-17] - a confirmed headshot KILL has
                                               // disfigured this corpse's head surface; the body tier loop
                                               // must never walk it back down
    bool              m_bForceDropHealth;
    bool              m_bForceDropWeapon;
    bool m_bCoopSidearmSwap;   // HZM coop - already swapped to a sidearm once (hand shot)

    bool m_bFootOnGround_Right;
    bool m_bFootOnGround_Left;
    //
    // Added in OPM
    //
    int iNextLandTime;

    CLASS_PROTOTYPE(Sentient);

    Sentient();
    virtual ~Sentient();
    virtual Vector EyePosition(void);

    virtual void   SetViewAngles(Vector angles);
    virtual void   SetTargetViewAngles(Vector angles);
    virtual Vector GetViewAngles(void);
    void           AddViewVariation(const Vector& vVariation);
    void           SetMinViewVariation(const Vector& vVariation);
    void           SetHolsteredByCode(bool holstered);
    bool           CanSee(Entity *ent, float fov, float vision_distance, bool bNoEnts) override;
    bool           CanSee(const Vector& org, float fov, float vision_distance, bool bNoEnts) override;
    virtual Vector GunPosition(void);
    virtual Vector GunTarget(bool bNoCollision = false, const vec3_t position = NULL, const vec3_t forward = NULL);
    void           ReloadWeapon(Event *ev);
    void           FireWeapon(Event *ev);
    void           StopFireWeapon(Event *ev);
    void           ChargeWeapon(Event *ev);
    virtual void   EventForceLandmineMeasure(Event *ev);
    void           EventSetWeaponIdleState(Event *ev);
    void           EventPingForMines(Event *ev);
    void           ReleaseFireWeapon(Event *ev);
    void           ChangeWeapon(Weapon *weapon, weaponhand_t hand);
    Weapon        *GetActiveWeapon(weaponhand_t hand) const;
    Weapon        *WorstWeapon(Weapon *ignore = NULL, qboolean bGetItem = false, int iIgnoreClass = 0);
    Weapon        *BestWeapon(Weapon *ignore = NULL, qboolean bGetItem = false, int iIgnoreClass = 0);
    Weapon        *NextWeapon(Weapon *weapon);
    Weapon        *PreviousWeapon(Weapon *weapon);
    void           useWeapon(const char *weaponname, weaponhand_t hand = WEAPON_MAIN);
    void           useWeapon(Weapon *weapon, weaponhand_t hand = WEAPON_MAIN);
    void           EventUseWeapon(Event *ev);
    void           EventDeactivateWeapon(Event *ev);
    int            NumWeapons(void);
    int            AmmoCount(str ammo_type);
    int            MaxAmmoCount(str ammo_type);
    int            AmmoIndex(str ammo_type);
    int            UseAmmo(str ammo_type, int amount);
    void           GiveAmmo(str type, int amount, int max_amount = -1);
    Ammo          *FindAmmoByName(str name);
    Item          *giveItem(str itemname, int amount = 1);
    void           takeItem(const char *itemname);
    void           takeAmmoType(const char *ammoname);
    void           AddItem(Item *object);
    void           PruneStaleInventory(void); // HZM bug-924: heal stale/freed slots instead of skipping them
    void           RemoveItem(Item *object);
    void           RemoveWeapons(void);
    Weapon        *GetWeapon(int index);
    Item          *FindItemByClassName(const char *classname);
    Item          *FindItemByModelname(const char *modelname);
    Item          *FindItemByExternalName(const char *externalname);
    Item          *FindItem(const char *itemname);
    void           FreeInventory(void);
    void           EventFreeInventory(Event *ev);
    qboolean       HasItem(const char *itemname);
    qboolean       HasWeaponClass(int iWeaponClass);
    qboolean       HasPrimaryWeapon(void);
    qboolean       HasSecondaryWeapon(void);
    int            NumInventoryItems(void);
    Item          *NextItem(Item *item);
    Item          *PrevItem(Item *item);
    virtual void   DropInventoryItems(void);
    void           ListInventory(void);

    qboolean PowerupActive(void);

    void             setModel(const char *mdl);
    void             Archive(Archiver& arc) override;
    void             ArchivePersistantData(Archiver& arc);
    void             DoubleArmor(void);
    virtual qboolean DoGib(int meansofdeath, Entity *inflictor);
    void             JumpXY(Event *ev);
    void             MeleeAttackStart(Event *ev);
    void             MeleeAttackEnd(Event *ev);
    void             BlockStart(Event *ev);
    void             BlockEnd(Event *ev);
    void             StunStart(Event *ev);
    void             StunEnd(Event *ev);
    void             SetAttackBlocked(bool blocked);
    virtual void     ReceivedItem(Item *item);
    virtual void     RemovedItem(Item *item);
    virtual void     AmmoAmountChanged(Ammo *ammo, int inclip = 0);
    void             AmmoAmountInClipChanged(str ammo_type, int amount);

    void         SetMaxMouthAngle(Event *ev);
    void         TryLightOnFire(int meansofdeath, Entity *attacker);
    void         OnFire(Event *ev);
    void         StopOnFire(Event *ev);
    void         SpawnBloodyGibs(Event *ev);
    void         SetMaxGibs(Event *ev);
    virtual void GetStateAnims(Container<const char *> *c);
    void         SpawnEffect(str modelname, Vector pos);

    bool         IsNewActiveWeapon(void);
    Weapon      *GetNewActiveWeapon(void);
    weaponhand_t GetNewActiveWeaponHand(void);
    void         ClearNewActiveWeapon(void);
    void         SetNewActiveWeapon(Weapon *weapon, weaponhand_t hand);
    void         Holster(qboolean putaway);
    void         SafeHolster(qboolean putaway);
    void         ActivateNewWeapon(void);
    void         UpdateCoopHolsteredWeapons(void); // HZM coop - weapons-on-back persistence
    void         ActivateNewWeapon(Event *ev);
    void         UpdateWeapons(void);
    VehicleTank *GetVehicleTank(void);
    void         UpdateFootsteps(void);
    qboolean     AIDontFace() const override;
    void         PutawayWeapon(Event *ev);
    void         WeaponCommand(Event *ev);
    //
    // Squad stuff
    //
    void AssertValidSquad();
    bool IsTeamMate(Sentient *pOther);
    void JoinNearbySquads(float fJoinRadius = 1024.0f);
    void MergeWithSquad(Sentient *pFriendly);
    void DisbandSquadMate(Sentient *pExFriendly);
    bool IsSquadMate(Sentient *pFriendly);

    virtual bool IsDisabled() const; // Added in 2.30

    Vehicle *GetVehicle() const;
    void     SetVehicle(Vehicle *pVehicle);

    TurretGun *GetTurret() const;
    void       SetTurret(TurretGun *pTurret);

    Entity *GetLadder() const; // Added in OPM

    //
    // Custom openmohaa stuff
    //
    void GetActiveWeap(Event *ev);
    void CoopHasItem(Event *ev); // HZM coop bug-2027: non-perturbing inventory query
    void GetNewActiveWeaponOld(Event *ev);
    void GetNewActiveWeapon(Event *ev);
    void GetNewActiveWeaponHand(Event *ev);
    void EventClientLanding(Event *ev);

    void FootstepMain(trace_t *trace, int iRunning, int iEquipment);
    void Footstep(const char *szTagName, int iRunning, int iEquipment);
    void LandingSound(float volume, int iEquipment);

    const Container<int>&    getInventory() const;
    const Container<Ammo *>& getAmmoInventory() const;
};

typedef SafePtr<Sentient> SentientPtr;

extern Container<Sentient *> SentientList;
