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
// player.h: Class definition of the player.

#include "g_local.h"
#include "bg_local.h"
#include "g_spawn.h"
#include "g_phys.h"
#include "entity.h"
#include "consoleevent.h"
#include "player.h"
#include "huddraw.h" // HZM coop - grenade-kick hud icon
#include "worldspawn.h"
#include "weapon.h"
#include "trigger.h"
#include "scriptmaster.h"
#include "scriptexception.h"
#include "navigate.h"
#include "misc.h"
#include "earthquake.h"
#include "gravpath.h"
#include "armor.h"
#include "inventoryitem.h"
#include "gibs.h"
#include "actor.h"
#include "object.h"
#include "characterstate.h"
#include "weaputils.h"
#include "dm_manager.h"
#include "parm.h"
#include "body.h"
#include "playerstart.h"
#include "camera.h"
#include "weapturret.h"
#include "vehicleturret.h"
#include "portableturret.h"
#include "fixedturret.h"

const Vector power_color(0.0, 1.0, 0.0);
const Vector acolor(1.0, 1.0, 1.0);
const Vector bcolor(1.0, 0.0, 0.0);

ScriptDelegate Player::scriptDelegate_connected("player_connected", "Sent once when the player connected");
ScriptDelegate Player::scriptDelegate_disconnecting("player_disconnecting", "The player is disconnecting");
ScriptDelegate Player::scriptDelegate_spawned("player_spawned", "The player has spawned");
ScriptDelegate Player::scriptDelegate_damage("player_damaged", "The player got hit");
ScriptDelegate Player::scriptDelegate_kill("player_killed", "The player got killed");
ScriptDelegate Player::scriptDelegate_textMessage("player_textMessage", "The player just sent a text message");

//
// mohaas 2.0 and above
//
const char *pInstantMsgEng[6][9] = {
    {"Good job team!",
     "Alright!", "We've done it!",
     "Wooohoo!", "Objective achieved.",
     "We've completed an objective.", "We've lost an objective!",
     "The enemy has overrun our objective!", NULL},
    {"Squad, move in!",
     "Squad, fall back!", "Squad, attack right flank!",
     "Squad, attack left flank!", "Squad, hold this position!",
     "Squad, covering fire!", "Squad, regroup!",
     "Squad, split up!", NULL},
    {"Cover me!",
     "I'll cover you!", "Follow me!",
     "You take point.", "Taking Fire!  Need some help!",
     "Get ready to move in on my signal.", "Attack!",
     "Open fire!", NULL},
    {"Yes sir!",
     "No sir!", "Enemy Spotted.",
     "Sniper!", "Grenade! Take Cover!",
     "Area Clear.", "Thanks.",
     "I owe you one.", NULL},
    {"Who wants more?!",
     "Never send boys to do a man's job.", "This is too easy!",
     "You mess with the best, you die like the rest.", "Watch that friendly fire!",
     "Hey!  I'm on your team!", "Come on out you cowards!",
     "Where are you hiding?", NULL},
    // Added in 2.30
    {"Guard our jail!",
     "Capture the enemy jail!", "I'm defending our jail!",
     "I'm attacking the enemy jail!", "Rescue the Prisoners!",
     "The enemy is attacking our jail!"}
};

//
// for mohaa version 1.11 and below
//
const char *pInstantMsgEng_ver6[5][9] = {
    {"Squad, move in!",
     "Squad, fall back!", "Squad, attack right flank!",
     "Squad, attack left flank!", "Squad, hold this position!",
     "Squad, covering fire!", "Squad, regroup!",
     "", ""},
    {
     "Cover me!", "I'll cover you!",
     "Follow me!", "You take point.",
     "You take the lead.", "Taking Fire!  Need some help!",
     "Charge!", "Attack!",
     "Open fire!", },
    {
     "Yes sir!", "No sir!",
     "Enemy Spotted.", "Sniper!",
     "Grenade! Take Cover!", "Area Clear.",
     "Great Shot!", "Thanks.",
     "I owe you one.", },
    {
     "Is that all you've got?", "I think they are all out of real men!",
     "Go on and run, you yellow-bellies!", "They're a bunch of cowards!",
     "Come back when you've had some target practice!", "Come prepared next time!",
     "Try again!", "I've seen French school girls shoot better!",
     "That made a mess.", },
    {"He's going to get us killed!",
     "A lot of good men are going to die because of his poor leadership", "Good riddance!",
     "That guy is going to get us all killed!", "Hey buddy, get down!",
     "Stay out of my foxhole, pal!", "Find your own hiding place!",
     "Get out of my way!", ""}
};

Event EV_Player_DumpState
(
    "state",
    EV_CHEAT,
    NULL,
    NULL,
    "Dumps the player's state to the console.",
    EV_NORMAL
);
Event EV_Player_ForceTorsoState
(
    "forcetorsostate",
    EV_DEFAULT,
    "s",
    "torsostate",
    "Force the player's torso to a certain state",
    EV_NORMAL
);
Event EV_Player_ForceLegsState
(
    "forcelegsstate",
    EV_DEFAULT,
    "s",
    "legsstate",
    "Force the player's legs to a certain state",
    EV_NORMAL
);
Event EV_Player_GiveAllCheat
(
    "wuss",
    EV_CONSOLE | EV_CHEAT,
    NULL,
    NULL,
    "Gives player all weapons.",
    EV_NORMAL
);
Event EV_Player_GiveNewWeaponsCheat
(
    "giveweapon",
    EV_CONSOLE | EV_CHEAT,
    "s",
    "weapon_name",
    "Gives player all weapons.",
    EV_NORMAL
);
Event EV_Player_EndLevel
(
    "endlevel",
    EV_DEFAULT,
    NULL,
    NULL,
    "Called when the player gets to the end of the level.",
    EV_NORMAL
);
Event EV_Player_DevGodCheat
(
    "dog",
    EV_CHEAT | EV_CONSOLE,
    "I",
    "god_mode",
    "Sets the god mode cheat or toggles it.",
    EV_NORMAL
);
Event EV_Player_FullHeal
(
    "fullheal",
    EV_CHEAT | EV_CONSOLE,
    NULL,
    NULL,
    "Heals player.",
    EV_NORMAL
);
Event EV_Player_DevNoTargetCheat
(
    "notarget",
    EV_CHEAT,
    "B",
    "bValue",
    "Toggles the notarget cheat. With an argument, SETS it instead (parity with EV_NoTarget).",
    EV_NORMAL
);
Event EV_Player_DevNoClipCheat
(
    "noclip",
    EV_CHEAT | EV_CONSOLE,
    NULL,
    NULL,
    "Toggles the noclip cheat.",
    EV_NORMAL
);
Event EV_Player_PrevItem
(
    "invprev",
    EV_CONSOLE,
    NULL,
    NULL,
    "Cycle to player's previous item.",
    EV_NORMAL
);
Event EV_Player_NextItem
(
    "invnext",
    EV_CONSOLE,
    NULL,
    NULL,
    "Cycle to player's next item.",
    EV_NORMAL
);
Event EV_Player_PrevWeapon
(
    "weapprev",
    EV_CONSOLE,
    NULL,
    NULL,
    "Cycle to player's previous weapon.",
    EV_NORMAL
);
Event EV_Player_NextWeapon
(
    "weapnext",
    EV_CONSOLE,
    NULL,
    NULL,
    "Cycle to player's next weapon.",
    EV_NORMAL
);
Event EV_Player_DropWeapon
(
    "weapdrop",
    EV_CONSOLE,
    NULL,
    NULL,
    "Drops the player's current weapon.",
    EV_NORMAL
);
Event EV_Player_Reload
(
    "reload",
    EV_CONSOLE,
    NULL,
    NULL,
    "Reloads the player's weapon",
    EV_NORMAL
);
Event EV_Player_CorrectWeaponAttachments
(
    "correctweaponattachments",
    EV_CONSOLE,
    NULL,
    NULL,
    "makes sure the weapons is properly attached when interrupting a reload",
    EV_NORMAL
);
Event EV_Player_GiveCheat
(
    "give",
    EV_CONSOLE | EV_CHEAT,
    "sI",
    "name amount",
    "Gives the player the specified thing (weapon, ammo, item, etc.) and optionally the amount.",
    EV_NORMAL
);
Event EV_Player_GiveWeaponCheat
(
    "giveweapon",
    EV_CONSOLE | EV_CHEAT,
    "s",
    "weapon_name",
    "Gives the player the specified weapon.",
    EV_NORMAL
);
/* HZM [user 2026-08-10] CLIENT -> SERVER PROFILE CHANNEL.
   The hybrid progression design (server authoritative, client carries a mirror, server imports the
   mirror when it has no record for that id) needs the client to be able to hand its saved profile
   BACK to the server. Server->client was already solved - chal_ui_export stufftexts `seta`s, and
   helmet.scr uses the `vstr` trick - but nothing went the other way: client console commands are
   dispatched through a C++ table (gamecmds.cpp:189) and the mod registered none of its own.

   No new plumbing was needed in the end. G_ProcessClientCommand already falls through to
   `ent->entity->ProcessEvent(ev)` (gamecmds.cpp:231-233) for any command naming a registered event
   that passes CheckEventFlags - and EV_CONSOLE is exactly the flag that passes it
   (entity.cpp:5258-5263). So a console-flagged Player event IS the channel.

   The handler hands the payload to script the same way coop_guid already reaches it:
   Vars()->SetVariable (precedent at g_client.cpp:818), which makes it readable in script as
   `local.player.coop_profdata` - the identical mechanism the profile identity already uses.

   Chunked on purpose: a full profile will not fit one command, so the client sends
   `coopprof <index> <payload>` repeatedly and script reassembles. Index 0 resets the buffer. */
Event EV_Player_CoopProf
(
    "coopprof",
    EV_CONSOLE,
    "is",
    "index data",
    "HZM coop: receive one chunk of this client's profile mirror.",
    EV_NORMAL
);

Event EV_Player_GameVersion
(
    "gameversion",
    EV_CONSOLE,
    NULL,
    NULL,
    "Prints the game version.",
    EV_NORMAL
);
Event EV_Player_Fov
(
    "fov",
    EV_CONSOLE,
    "F",
    "fov",
    "Sets the fov.",
    EV_NORMAL
);
Event EV_Player_Dead
(
    "dead",
    EV_DEFAULT,
    NULL,
    NULL,
    "Called when the player is dead.",
    EV_NORMAL
);
Event EV_Player_SpawnEntity
(
    "spawn",
    EV_CHEAT,
    "sSSSSSSSS",
    "entityname keyname1 value1 keyname2 value2 keyname3 value3 keyname4 value4",
    "Spawns an entity.",
    EV_NORMAL
);
Event EV_Player_SpawnActor
(
    "actor",
    EV_CHEAT,
    "sSSSSSSSS",
    "modelname keyname1 value1 keyname2 value2 keyname3 value3 keyname4 value4",
    "Spawns an actor.",
    EV_NORMAL
);
Event EV_Player_Respawn
(
    "respawn",
    EV_DEFAULT,
    NULL,
    NULL,
    "Respawns the player.",
    EV_NORMAL
);
Event EV_Player_TestThread
(
    "testthread",
    EV_CHEAT,
    "sS",
    "scriptfile label",
    "Starts the named thread at label if provided.",
    EV_NORMAL
);
Event EV_Player_PowerupTimer
(
    "poweruptimer",
    EV_DEFAULT,
    "ii",
    "poweruptimer poweruptype",
    "Sets the powerup timer and powerup type.",
    EV_NORMAL
);
Event EV_Player_UpdatePowerupTimer
(
    "updatepoweruptime",
    EV_DEFAULT,
    NULL,
    NULL,
    "Called once a second to decrement powerup time.",
    EV_NORMAL
);
Event EV_Player_ResetState
(
    "resetstate",
    EV_CHEAT,
    NULL,
    NULL,
    "Reset the player's state table.",
    EV_NORMAL
);
Event EV_Player_WhatIs
(
    "whatis",
    EV_CHEAT,
    "i",
    "entity_number",
    "Prints info on the specified entity.",
    EV_NORMAL
);
Event EV_Player_ActorInfo
(
    "actorinfo",
    EV_CHEAT,
    "i",
    "actor_number",
    "Prints info on the specified actor.",
    EV_NORMAL
);
Event EV_Player_KillEnt
(
    "killent",
    EV_CHEAT,
    "i",
    "entity_number",
    "Kills the specified entity.",
    EV_NORMAL
);
Event EV_Player_KillClass
(
    "killclass",
    EV_CHEAT,
    "sI",
    "classname except_entity_number",
    "Kills all of the entities in the specified class.",
    EV_NORMAL
);
Event EV_Player_RemoveEnt
(
    "removeent",
    EV_CHEAT,
    "i",
    "entity_number",
    "Removes the specified entity.",
    EV_NORMAL
);
Event EV_Player_RemoveClass
(
    "removeclass",
    EV_CHEAT,
    "sI",
    "classname except_entity_number",
    "Removes all of the entities in the specified class.",
    EV_NORMAL
);
Event EV_Player_Jump
(
    "jump",
    EV_DEFAULT,
    "f",
    "height",
    "Makes the player jump.",
    EV_NORMAL
);
Event EV_Player_AnimLoop_Torso
(
    "animloop_torso",
    EV_DEFAULT,
    NULL,
    NULL,
    "Called when the torso animation has finished.",
    EV_NORMAL
);
Event EV_Player_AnimLoop_Legs
(
    "animloop_legs",
    EV_DEFAULT,
    NULL,
    NULL,
    "Called when the legs animation has finished.",
    EV_NORMAL
);
Event EV_Player_AnimLoop_Pain // Added in 2.0
(
    "animloop_pain",
    EV_DEFAULT,
    NULL,
    NULL,
    "Called when the pain animation has finished.",
    EV_NORMAL
);
Event EV_Player_DoUse
(
    "usestuff",
    EV_DEFAULT,
    NULL,
    NULL,
    "Makes the player try to use whatever is in front of her.",
    EV_NORMAL
);
Event EV_Player_ListInventory
(
    "listinventory",
    EV_CONSOLE,
    NULL,
    NULL,
    "List of the player's inventory.",
    EV_NORMAL
);
Event EV_Player_ActivateShield
(
    "activateshield",
    EV_DEFAULT,
    NULL,
    NULL,
    "Activates the player's shield",
    EV_NORMAL
);
Event EV_Player_DeactivateShield
(
    "deactivateshield",
    EV_DEFAULT,
    NULL,
    NULL,
    "Deactivates the player's shield",
    EV_NORMAL
);
Event EV_Player_Turn
(
    "turn",
    EV_DEFAULT,
    "f",
    "yawangle",
    "Causes player to turn the specified amount.",
    EV_NORMAL
);
Event EV_Player_TurnUpdate
(
    "turnupdate",
    EV_DEFAULT,
    "ff",
    "yaw timeleft",
    "Causes player to turn the specified amount.",
    EV_NORMAL
);
Event EV_Player_TurnLegs
(
    "turnlegs",
    EV_DEFAULT,
    "f",
    "yawangle",
    "Turns the players legs instantly by the specified amount.",
    EV_NORMAL
);
Event EV_Player_NextPainTime(
    "nextpaintime",
    EV_DEFAULT,
    "f",
    "seconds",
    "Set the next time the player experiences pain (Current time + seconds specified).",
    EV_NORMAL
);

Event EV_Player_FinishUseAnim
(
    "finishuseanim",
    EV_DEFAULT,
    NULL,
    NULL,
    "Fires off all targets associated with a particular useanim.",
    EV_NORMAL
);
Event EV_Player_Holster
(
    "holster",
    EV_CONSOLE,
    NULL,
    NULL,
    "Holsters all wielded weapons, or unholsters previously put away weapons",
    EV_NORMAL
);
Event EV_Player_SafeHolster
(
    "safeholster",
    EV_CONSOLE,
    "b",
    "putaway",
    "Holsters all wielded weapons, or unholsters previously put away weapons\n"
    "preserves state, so it will not holster or unholster unless necessary",
    EV_NORMAL
);
Event EV_Player_CoopLobbyPose
(
    "coop_lobbypose",
    EV_DEFAULT,
    NULL,
    NULL,
    "HZM coop lobby: freezes the player as a parade-rest mannequin - forces the EMOTE_ATEASE legs +\n"
    "STAND torso states, slings the main weapon onto the back tag, then sets FL_IMMOBILE so the anim\n"
    "state machine stops evaluating (the NEW_WEAPON/HAS_WEAPON edges can no longer twitch the pose or\n"
    "redraw the rifle). Call once the spawn loadout has settled.",
    EV_NORMAL
);
Event EV_Player_CoopLobbyUnpose
(
    "coop_lobbyunpose",
    EV_DEFAULT,
    NULL,
    NULL,
    "HZM coop lobby: releases the mannequin freeze set by coop_lobbypose (clears FL_IMMOBILE and\n"
    "restores the slung weapon to the hands).",
    EV_NORMAL
);
Event EV_Player_CoopLobbyRepose
(
    "coop_lobbyrepose",
    EV_DEFAULT,
    "s",
    "legsstate",
    "HZM coop lobby: atomically swaps a frozen mannequin's idle pose to the given legs state. Lifts\n"
    "FL_IMMOBILE, forces STAND torso + that legs state, then re-sets FL_IMMOBILE - all in one call so\n"
    "the per-frame statemap never ticks in between (no twitch) and the slung weapon is left alone.",
    EV_NORMAL
);
Event EV_Player_CoopNavRec
(
    "coop_navrec",
    EV_CONSOLE,
    NULL,
    NULL,
    "HZM coop: toggle NAV NODE RECORDING. Walk the ground you want AI to be able to use; a node is"
    " dropped every coop_navRecSpacing units. Toggling off rebuilds the path graph so it is usable"
    " immediately, and prints every node as ^~^~^ NAVNODE x y z for baking into the map script.",
    EV_NORMAL
);
Event EV_Player_CoopNavNode
(
    "coop_navnode",
    EV_CONSOLE,
    "fff",
    "x y z",
    "HZM coop: create one AI path node at the given point (used by baked map scripts).",
    EV_NORMAL
);
Event EV_Player_CoopNavBuild
(
    "coop_navbuild",
    EV_CONSOLE,
    NULL,
    NULL,
    "HZM coop: rebuild the AI path graph after creating nodes.",
    EV_NORMAL
);
Event EV_Player_CoopLimpTest
(
    "coop_limptest",
    EV_CONSOLE,
    "F",
    "frac",
    "HZM coop DEV: set the player's health to <frac> of max (default 0.25) WITHOUT killing them, so "
    "the low-health limp can be tested without dying first. Clamped to at least 1hp.",
    EV_NORMAL
);
Event EV_Player_CoopLobbyCycleAnim
(
    "coop_lobbycycleanim",
    EV_CONSOLE,
    "i",
    "dir",
    "HZM coop lobby DEV tool: step through the candidate standing-idle anims (dir >0 next, <0 prev),\n"
    "holding each one, and print its name - used to visually identify the hands-on-hips pose. Bind to keys.",
    EV_NORMAL
);
Event EV_Player_CoopLobbyHoldPose
(
    "coop_lobbyholdpose",
    EV_DEFAULT,
    NULL,
    NULL,
    "HZM coop lobby: re-assert the hands-on-hips pose (STAND torso + coop_pose_g100 legs + FL_IMMOBILE).\n"
    "Called every frame by lobby.scr::lobbyLockWatch so the spawn/weapon settle churn can't clobber it -\n"
    "it is the exact call the pose-cycler uses, and is a no-op once the pose is already correct.",
    EV_NORMAL
);
Event EV_Player_CoopLobbyInput
(
    "coop_lobbyinput",
    EV_DEFAULT,
    "i",
    "onoff",
    "HZM coop lobby: while enabled (1), read this player's A/D strafe (rightmove) + F (BUTTON_USE) from the\n"
    "usercmd every frame and publish self.coop_lobbyInput (31=next uniform, 32=prev, 33=ready) so the lobby\n"
    "script reacts WITHOUT any client key binds - works for every client, host and remote. 0 disables.",
    EV_NORMAL
);
Event EV_Player_CoopLobbyCursor
(
    "coop_lobbycursor",
    EV_DEFAULT,
    "i",
    "onoff",
    "HZM coop lobby: while enabled (1), derive a MOUSE CURSOR from this player's usercmd view-angle deltas\n"
    "and publish self.coop_lobbyCurX / self.coop_lobbyCurY (virtual 640x480) + self.coop_lobbyClick (1 on a\n"
    "left-mouse press edge) so the lobby script can draw a cursor and hit-test clickable buttons. 0 disables.\n"
    "Foundation for the clickable lobby UI (challenges now, loadout / skill trees later). coop_lobbyCursorSens tunes speed.",
    EV_NORMAL
);
Event EV_Player_SafeZoom
(
    "safezoom",
    EV_DEFAULT,
    "b",
    "zoomin",
    "0 turns off zoom,"
    "and 1 returns zoom to previous setting"
);
Event EV_Player_ZoomOff
(
    "zoomoff",
    EV_DEFAULT,
    NULL,
    NULL,
    "makes sure that zoom is off",
    EV_NORMAL
);
Event EV_Player_StartUseObject
(
    "startuseobject",
    EV_DEFAULT,
    NULL,
    NULL,
    "starts up the useobject's animations.",
    EV_NORMAL
);
Event EV_Player_FinishUseObject
(
    "finishuseobject",
    EV_DEFAULT,
    NULL,
    NULL,
    "Fires off all targets associated with a particular useobject.",
    EV_NORMAL
);
Event EV_Player_WatchActor
(
    "watchactor",
    EV_DEFAULT,
    "e",
    "actor_to_watch",
    "Makes the player's camera watch the specified actor.",
    EV_NORMAL
);
Event EV_Player_StopWatchingActor
(
    "stopwatchingactor",
    EV_DEFAULT,
    "e",
    "actor_to_stop_watching",
    "Makes the player's camera stop watching the specified actor.",
    EV_NORMAL
);
Event EV_Player_SetDamageMultiplier
(
    "damage_multiplier",
    EV_DEFAULT,
    "f",
    "damage_multiplier",
    "Sets the current damage multiplier",
    EV_NORMAL
);
Event EV_Player_WaitForState
(
    "waitForState",
    EV_DEFAULT,
    "s",
    "stateToWaitFor",
    "When set, the player will clear waitforplayer when this state is hit\n"
    "in the legs or torso.",
    EV_NORMAL
);
Event EV_Player_LogStats
(
    "logstats",
    EV_CHEAT,
    "b",
    "state",
    "Turn on/off the debugging playlog",
    EV_NORMAL
);
Event EV_Player_TakePain
(
    "takepain",
    EV_DEFAULT,
    "b",
    "bool",
    "Set whether or not to take pain",
    EV_NORMAL
);
Event EV_Player_SkipCinematic
(
    "skipcinematic",
    EV_CONSOLE,
    NULL,
    NULL,
    "Skip the current cinematic",
    EV_NORMAL
);
Event EV_Player_ResetHaveItem
(
    "resethaveitem",
    EV_CONSOLE,
    "s",
    "weapon_name",
    "Resets the game var that keeps track that we have gotten this weapon",
    EV_NORMAL
);
Event EV_Player_ModifyHeight
(
    "modheight",
    EV_DEFAULT,
    "s",
    "height",
    "change the maximum height of the player\ncan specify 'stand', 'duck' or 'prone'.",
    EV_NORMAL
);
Event EV_Player_ModifyHeightFloat // Added in 2.40
(
    "modheightfloat",
     EV_DEFAULT,
     "ff",
     "height max_z",
     "Specify the view height of the player and the height of his bounding box.",
     EV_NORMAL
);
Event EV_Player_SetMovePosFlags
(
    "moveposflags",
    EV_DEFAULT,
    "sS",
    "position movement",
    "used by the state files to tell the game dll what the player is doing",
    EV_NORMAL
);
Event EV_Player_GetPosition
(
    "getposition",
    EV_DEFAULT,
    NULL,
    NULL,
    "returns the player current position",
    EV_RETURN
);
Event EV_Player_GetMovement
(
    "getmovement",
    EV_DEFAULT,
    NULL,
    NULL,
    "returns the player current movement",
    EV_RETURN
);
Event EV_Player_Score
(
    "score",
    EV_CONSOLE,
    NULL,
    NULL,
    "Show the score for the current deathmatch game",
    EV_NORMAL
);
Event EV_Player_JoinDMTeam
(
    "join_team",
    EV_CONSOLE,
    "s",
    "team",
    "Join the specified team (allies or axis)",
    EV_NORMAL
);
Event EV_Player_AutoJoinDMTeam
(
    "auto_join_team",
    EV_CONSOLE,
    NULL,
    NULL,
    "Join the team with fewer players",
    EV_NORMAL
);
Event EV_Player_PickWeapon
(
    "pickweapon",
    EV_CONSOLE,
    NULL,
    NULL,
    "Pick your weapon.",
    EV_NORMAL
);
Event EV_Player_SetInJail // Added in 2.30
(
    "injail",
    EV_DEFAULT,
    "i",
    "boolean",
    "set to 1 to indicate when player is in jail,"
    "0 when they are free",
    EV_SETTER
);
Event EV_Player_GetInJail // Added in 2.30
(
    "injail",
    EV_DEFAULT,
    NULL,
    NULL,
    "returns 1 if player is in jail,"
    "0 if out",
    EV_GETTER
);
Event EV_Player_GetNationalityPrefix // Added in 2.30
(
    "nationalityprefix",
    EV_DEFAULT,
    NULL,
    NULL,
    "get the three or five letter prefix that denotes the player's nationality",
    EV_GETTER
);
Event EV_Player_IsSpectator // Added in 2.30
(
    "isSpectator",
    EV_DEFAULT,
    NULL,
    NULL,
    "Check to see if player is a spectator (non-zero return value)",
    EV_GETTER
);
Event EV_Player_Spectator
(
    "spectator",
    EV_CONSOLE,
    NULL,
    NULL,
    "Become a spectator",
    EV_NORMAL
);
Event EV_Player_JoinArena
(
    "join_arena",
    EV_CONSOLE,
    "i",
    "arena_id_num",
    "Join the specified arena",
    EV_NORMAL
);
Event EV_Player_LeaveArena
(
    "leave_arena",
    EV_CONSOLE,
    NULL,
    NULL,
    "Leave the current arena",
    EV_NORMAL
);
Event EV_Player_CreateTeam
(
    "create_team",
    EV_CONSOLE,
    NULL,
    NULL,
    "Create a team in the current arena",
    EV_NORMAL
);
Event EV_Player_LeaveTeam
(
    "leave_team",
    EV_CONSOLE,
    NULL,
    NULL,
    "Leave the current team",
    EV_NORMAL
);
Event EV_Player_RefreshArenaUI
(
    "arena_ui",
    EV_CONSOLE,
    NULL,
    NULL,
    "Refresh the arena UI",
    EV_NORMAL
);
Event EV_Player_CallVote
(
    "callvote",
    EV_CONSOLE,
    "ss",
    "arg1 arg2",
    "Player calls a vote",
    EV_NORMAL
);
Event EV_Player_Vote
(
    "vote",
    EV_CONSOLE,
    "s",
    "arg1",
    "Player votes either yes or no",
    EV_NORMAL
);
Event EV_Player_RetrieveVoteOptions // Added in 2.0
(
    "gvo",
    EV_CONSOLE,
    NULL,
    NULL,
    "Retrieves the server's vote options file",
    EV_NORMAL
);
Event EV_Player_PrimaryDMWeapon
(
    "primarydmweapon",
    EV_CONSOLE,
    "s",
    "weaptype",
    "Sets the player's primary DM weapon",
    EV_NORMAL
);
Event EV_Player_DeadBody
(
    "deadbody",
    EV_DEFAULT,
    NULL,
    NULL,
    "Spawn a dead body",
    EV_NORMAL
);
Event EV_Player_Physics_On
(
    "physics_on",
    EV_DEFAULT,
    NULL,
    NULL,
    "turn player physics on.",
    EV_NORMAL
);
Event EV_Player_Physics_Off
(
    "physics_off",
    EV_DEFAULT,
    NULL,
    NULL,
    "turn player physics off.",
    EV_NORMAL
);
Event EV_Player_ArmWithWeapons // Added in 2.30
(
    "armwithweapons",
    EV_DEFAULT,
    NULL,
    NULL,
    "give player their primary and secondary weapons.",
    EV_NORMAL
);
Event EV_Player_GetCurrentDMWeaponType // Added in 2.30
(
    "getcurrentdmweapontype",
    EV_DEFAULT,
    NULL,
    NULL,
    "get the player's current DM weapon type.",
    EV_GETTER
);
Event EV_Player_AttachToLadder
(
    "attachtoladder",
    EV_DEFAULT,
    NULL,
    NULL,
    "Attaches the sentient to a ladder",
    EV_NORMAL
);
Event EV_Player_UnattachFromLadder
(
    "unattachfromladder",
    EV_DEFAULT,
    NULL,
    NULL,
    "Unattaches the sentient from a ladder",
    EV_NORMAL
);
Event EV_Player_TweakLadderPos
(
    "tweakladderpos",
    EV_DEFAULT,
    NULL,
    NULL,
    "Tweaks the player's position on a ladder to be proper",
    EV_NORMAL
);
Event EV_Player_EnsureOverLadder
(
    "ensureoverladder",
    EV_DEFAULT,
    NULL,
    NULL,
    "Ensures that the player is at the proper height when getting off the top of a ladder",
    EV_NORMAL
);
Event EV_Player_EnsureForwardOffLadder
(
    "ensureforwardoffladder",
    EV_DEFAULT,
    NULL,
    NULL,
    "Ensures that the player went forward off the ladder.",
    EV_NORMAL
);
Event EV_Player_JailIsEscaping // Added in 2.30
(
    "isEscaping",
    EV_DEFAULT,
    NULL,
    NULL,
    "Return non-zero if escaping or assisting escape",
    EV_GETTER
);
Event EV_Player_JailEscape // Added in 2.30
(
    "jailescape",
    EV_DEFAULT,
    NULL,
    NULL,
    "Start the escape from jail animation",
    EV_NORMAL
);
Event EV_Player_JailAssistEscape // Added in 2.30
(
    "jailassistescape",
    EV_DEFAULT,
    NULL,
    NULL,
    "Start the assist jail escape animation",
    EV_NORMAL
);
Event EV_Player_JailEscapeStop // Added in 2.30
(
    "jailescapestop",
    EV_DEFAULT,
    NULL,
    NULL,
    "Stop either the escape from jail or assist animation",
    EV_NORMAL
);
Event EV_Player_GetIsDisguised
(
    "is_disguised",
    EV_DEFAULT,
    NULL,
    NULL,
    "zero = not disguised"
    "non-zero = disguised",
    EV_GETTER
);
Event EV_Player_GetHasDisguise
(
    "has_disguise",
    EV_DEFAULT,
    NULL,
    NULL,
    "zero = does not have a disguise,"
    "non - zero = has a disguise",
    EV_GETTER
);
Event EV_Player_SetHasDisguise
(
    "has_disguise",
    EV_DEFAULT,
    "i",
    "is_disguised",
    "zero = does not have a disguise,"
    "non - zero = has a disguise",
    EV_SETTER
);
Event EV_Player_ObjectiveCount
(
    "objective",
    EV_DEFAULT,
    "ii",
    "num_completed out_of",
    "Sets the number of objectives completed and the total number of objectives",
    EV_NORMAL
);
Event EV_Player_Stats
(
    "stats",
    EV_CONSOLE,
    NULL,
    NULL,
    "Display the MissionLog.",
    EV_NORMAL
);
Event EV_Player_Teleport
(
    "tele",
    EV_CHEAT | EV_CONSOLE,
    "v",
    "location",
    "Teleport to location",
    EV_NORMAL
);
Event EV_Player_Face
(
    "face",
    EV_CHEAT | EV_CONSOLE,
    "v",
    "angles",
    "Force angles to specified vector",
    EV_NORMAL
);
Event EV_Player_Coord
(
    "coord",
    EV_CONSOLE,
    NULL,
    NULL,
    "Prints out current location and angles",
    EV_NORMAL
);
Event EV_Player_TestAnim
(
    "testplayeranim",
    EV_CHEAT,
    "fS",
    "weight anim",
    "Plays a test animation on the player",
    EV_NORMAL
);
Event EV_Player_StuffText
(
    "stufftext",
    EV_DEFAULT,
    "s",
    "stuffstrings",
    "Stuffs text to the player's console",
    EV_NORMAL
);
Event EV_Player_DMMessage
(
    "dmmessage",
    EV_CONSOLE,
    "is",
    "mode stuffstrings",
    "sends a DM message to the appropriate players",
    EV_NORMAL
);
Event EV_Player_IPrint
(
    "iprint",
    EV_CONSOLE,
    "sI",
    "string bold",
    "prints a string to the player,"
    "optionally in bold",
    EV_NORMAL
);
Event EV_SetViewangles
(
    "viewangles",
    EV_DEFAULT,
    "v",
    "newAngles",
    "set the view angles of the entity to newAngles.",
    EV_SETTER
);
Event EV_GetViewangles
(
    "viewangles",
    EV_DEFAULT,
    NULL,
    NULL,
    "get the angles of the entity.",
    EV_GETTER
);
Event EV_GetUseHeld
(
    "useheld",
    EV_DEFAULT,
    NULL,
    NULL,
    "returns 1 if this player is holding use,"
    "or 0 if he is not",
    EV_GETTER
);
Event EV_GetFireHeld
(
    "fireheld",
    EV_DEFAULT,
    NULL,
    NULL,
    "returns 1 if this player is holding fire,"
    "or 0 if he is not",
    EV_GETTER
);
Event EV_GetPrimaryFireHeld // Added in 2.30
(
    "primaryfireheld",
     EV_DEFAULT,
     NULL,
     NULL,
     "returns 1 if this player is holding the primary fire, or 0 if not",
     EV_GETTER);
Event EV_GetSecondaryFireHeld // Added in 2.30
(
    "secondaryfireheld",
     EV_DEFAULT,
     NULL,
     NULL,
     "returns 1 if this player is holding the secondary fire, or 0 if not",
     EV_GETTER
);
Event EV_GetCoopAdsHeld // HZM coop
(
    "coopadsheld",
     EV_DEFAULT,
     NULL,
     NULL,
     "returns 1 if this player is holding the aim-down-sights button, or 0 if not",
     EV_GETTER
);
Event EV_Player_CoopKillWall // HZM coop - wall probe v5 (bug-953)
(
    "killwall",
    EV_CONSOLE,
    NULL,
    NULL,
    "HZM coop - kill the invisible wall brush the player is aiming at (live + persisted)"
);
Event EV_Player_CoopMarkWall // HZM coop - wall probe v5 (bug-953)
(
    "markwall",
    EV_CONSOLE,
    NULL,
    NULL,
    "HZM coop - forensic report of whatever the player is aiming at (brush id, shader, species)"
);
Event EV_Player_CoopSetDbno // HZM coop - DBNO state [user 08-02]
(
    "coop_setdbno",
     EV_DEFAULT,
     "i",
     "active",
     "HZM coop - publish DBNO (down-but-not-out) state to the engine so turret mount can refuse it"
);
Event EV_Player_CoopSetCover // HZM coop - TAKE COVER [214]
(
    "coop_setcover",
     EV_DEFAULT,
     "i",
     "active",
     "HZM coop - request (1) or release (0) the take-cover pose; the engine validates it per frame"
);
Event EV_Player_GetCoopCover // HZM coop - TAKE COVER [214]
(
    "coop_incover",
     EV_DEFAULT,
     NULL,
     NULL,
     "HZM coop - cover state: 0 none, 1 requested (no valid pose), 2 wall pose, 3 low pose",
     EV_GETTER
);
Event EV_Player_GetReady
(
    "ready",
    EV_DEFAULT,
    NULL,
    NULL,
    "returns 1 if this player is ready,"
    "0 otherwise",
    EV_GETTER
);
Event EV_Player_SetReady
(
    "ready",
    EV_CONSOLE,
    NULL,
    NULL,
    "makes this player ready for the round to start",
    EV_NORMAL
);
Event EV_Player_SetNotReady
(
    "notready",
    EV_CONSOLE,
    NULL,
    NULL,
    "makes this player not ready for the round to start",
    EV_NORMAL
);
Event EV_Player_GetName
(
    "netname",
    EV_DEFAULT,
    NULL,
    NULL,
    "returns player's name",
    EV_GETTER
);
Event EV_Player_GetDMTeam
(
    "dmteam",
    EV_DEFAULT,
    NULL,
    NULL,
    "returns 'allies',"
    "'axis',"
    "'spectator',"
    "or 'freeforall'",
    EV_GETTER
);
Event EV_Player_SetViewModelAnim
(
    "viewmodelanim",
    EV_DEFAULT,
    "sI",
    "name force_restart",
    "Sets the player's view model animation.",
    EV_NORMAL
);
Event EV_Player_DMDeathDrop
(
    "dmdeathdrop",
    EV_DEFAULT,
    NULL,
    NULL,
    "Drops the player inventory in DM after's he's been killed",
    EV_NORMAL
);
Event EV_Player_Stopwatch
(
    "stopwatch",
    EV_DEFAULT,
    "i",
    "duration",
    "Starts a stopwatch for a given duration... use 0 to clear the stopwatch",
    EV_NORMAL
);
Event EV_Player_EnterIntermission
(
    "_enterintermission",
    EV_CODEONLY,
    NULL,
    NULL,
    "CODE USE ONLY",
    EV_NORMAL
);
Event EV_Player_SetPerferredWeapon
(
    "perferredweapon",
    EV_DEFAULT,
    "s",
    "weapon_name",
    "Overrides your preferred weapon that is displayed in the stats screen.",
    EV_NORMAL
);
Event EV_Player_SetVoiceType
(
    "voicetype",
    EV_DEFAULT,
    "s",
    "voice_name",
    "Sets the voice type to use the player.",
    EV_NORMAL
);

Event EV_Player_AddKills // Added in 2.0
(
    "addkills",
    EV_DEFAULT,
    "i",
    "kills",
    "Give or take kills from the player",
    EV_NORMAL
);

Event EV_Player_KillAxis // Added in 2.30
(
    "killaxis",
    EV_CHEAT,
    "f",
    "radius",
    "Kills all of the axis that are in the passed radius, or all of them if radius is 0.",
    EV_NORMAL
);
Event EV_Player_GetTurret // Added in 2.30
(
    "turret",
    EV_DEFAULT,
    NULL,
    NULL,
    "Returns the turret the player is using. NULL if player isn't using a turret.",
    EV_GETTER
);
Event EV_Player_GetVehicle // Added in 2.30
(
    "vehicle",
    EV_DEFAULT,
    NULL,
    NULL,
    "Returns the vehicle the player is using. NULL if player isn't using a vehicle.",
    EV_GETTER
);

////////////////////////////
//
// Added in OPM
//
////////////////////////////
Event EV_Player_AddDeaths
(
    "adddeaths",
    EV_DEFAULT,
    "i",
    "deaths",
    "adds deaths number to player",
    EV_NORMAL
);
Event EV_Player_AdminRights
(
    "adminrights",
    EV_DEFAULT,
    NULL,
    NULL,
    "returns client admin rights",
    EV_GETTER
);
Event EV_Player_IsAdmin
(
    "isadmin",
    EV_DEFAULT,
    NULL,
    NULL,
    "checks if player is logged as admin",
    EV_RETURN
);
Event EV_Player_BindWeap
(
    "bindweap",
    EV_DEFAULT,
    "ei",
    "weapon handnum",
    "binds weapon to player and sets him as weapon owner",
    EV_NORMAL
);
Event EV_Player_Dive
(
    "dive",
    EV_DEFAULT,
    "fF",
    "height airborne_duration",
    "Makes the player dive into prone position.",
    EV_NORMAL
);
Event EV_Player_FreezeControls
(
    "freezecontrols",
    EV_DEFAULT,
    "b",
    "freeze_state",
    "Blocks or unblocks control input from this player.",
    EV_NORMAL
);
Event EV_Player_GetConnState
(
    "getconnstate",
    EV_DEFAULT,
    NULL,
    NULL,
    "gets connection state. [DEPRECATED]",
    EV_RETURN
);
Event EV_Player_GetDamageMultiplier
(
    "damage_multiplier",
    EV_DEFAULT,
    "Gets the current damage multiplier",
    NULL,
    NULL,
    EV_GETTER
);
Event EV_Player_GetKillHandler
(
    "killhandler",
    EV_DEFAULT,
    "s",
    "label",
    "Gets the player's current killed event handler. Returns NIL if no custom killhandler was set.",
    EV_GETTER
);
Event EV_Player_GetKills
(
    "getkills",
    EV_DEFAULT,
    NULL,
    NULL,
    "gets kills number of player",
    EV_RETURN
);
Event EV_Player_GetDeaths
(
    "getdeaths",
    EV_DEFAULT,
    NULL,
    NULL,
    "gets deaths number of player",
    EV_RETURN
);
Event EV_Player_GetLegsState
(
    "getlegsstate",
    EV_DEFAULT,
    NULL,
    NULL,
    "Gets the player's current legs state name",
    EV_RETURN
);
Event EV_Player_GetStateFile
(
    "statefile",
    EV_DEFAULT,
    NULL,
    NULL,
    "Gets the player's current state file.",
    EV_GETTER
);
Event EV_Player_GetTorsoState
(
    "gettorsostate",
    EV_DEFAULT,
    NULL,
    NULL,
    "Gets the player's current torso state name",
    EV_RETURN
);
Event EV_Player_GetUserInfo
(
    "userinfo",
    EV_DEFAULT,
    NULL,
    NULL,
    "Retrieves the player's user info. Use info_valueforkey to retrieve the value for the specified key."
);
Event EV_Player_Inventory
(
    "inventory",
    EV_DEFAULT,
    NULL,
    NULL,
    "returns player's inventory",
    EV_GETTER
);
Event EV_Player_InventorySet
(
    "inventory",
    EV_DEFAULT,
    "e",
    "array",
    "Set up the player's inventory",
    EV_SETTER
);
Event EV_Player_LeanLeftHeld
(
    "leanleftheld",
    EV_DEFAULT,
    NULL,
    NULL,
    "Returns 1 if this player is holding lean left key, or 0 if he is not",
    EV_GETTER
);
Event EV_Player_LeanRightHeld
(
    "leanrightheld",
    EV_DEFAULT,
    NULL,
    NULL,
    "returns EV_RETURN if this player is holding lean right key, or 0 if he is not",
    EV_GETTER
);
Event EV_Player_MoveSpeedScale
(
    "moveSpeedScale",
    EV_DEFAULT,
    "f",
    "speed",
    "Sets the player's speed multiplier (default 1.0).",
    EV_SETTER
);
Event EV_Player_MoveSpeedScaleGet
(
    "moveSpeedScale",
    EV_DEFAULT,
    NULL,
    NULL,
    "Gets the player's speed multiplier.",
    EV_GETTER
);
Event EV_Player_PlayLocalSound
(
    "playlocalsound",
    EV_DEFAULT,
    "sBF",
    "soundName loop time",
    "Plays a local sound to the player. The sound must be aliased globally. Requires sv_reborn to be set for stereo "
    "sounds.",
    EV_NORMAL
);
Event EV_Player_RunHeld
(
    "runheld",
    EV_DEFAULT,
    NULL,
    NULL,
    "returns 1 if this player is holding run key,"
    "or 0 if he is not",
    EV_GETTER
);
Event EV_Player_SecFireHeld
(
    "secfireheld",
    EV_DEFAULT,
    NULL,
    NULL,
    "returns 1 if this player is holding secondary fire, or 0 if he is not",
    EV_GETTER
);
Event EV_Player_SetAnimSpeed
(
    "setanimspeed",
    EV_DEFAULT,
    "f",
    "speed",
    "set the player's animation speed multiplier (default 1.0).",
    EV_NORMAL
);
Event EV_Player_SetFov
(
    "setfov",
    EV_DEFAULT,
    "f",
    "fov",
    "set the player's fov (default 80).",
    EV_NORMAL
);
Event EV_Player_SetKillHandler
(
    "killhandler",
    EV_DEFAULT,
    "s",
    "label",
    "Replace the player's killed event by a new scripted handler. None or an empty string will revert to the default "
    "killed event handler.",
    EV_SETTER
);
Event EV_Player_SetSpeed
(
    "setspeed",
    EV_DEFAULT,
    "fI",
    "speed index",
    "Sets the player's speed multiplier (default 1.0). Index specify which array value will be used (maximum 4).",
    EV_NORMAL
);
Event EV_Player_SetStateFile
(
    "statefile",
    EV_DEFAULT,
    "S",
    "statefile",
    "Sets the player's current state file (setting NIL, NULL or an empty string will revert to the global statefile).",
    EV_SETTER
);
Event EV_Player_SetTeam
(
    "setteam",
    EV_DEFAULT,
    "s",
    "team_name",
    "sets the player's team without respawning.\n"
    "Available team names are 'none', 'spectator', 'freeforall', 'axis' and 'allies'.",
    EV_NORMAL
);
Event EV_Player_HideEnt
(
    "hideent",
    EV_DEFAULT,
    "e",
    "entity",
    "Hides the specified entity to the player.",
    EV_NORMAL
);
Event EV_Player_ShowEnt
(
    "showent",
    EV_DEFAULT,
    "e",
    "entity",
    "Shows the specified entity to the player.",
    EV_NORMAL
);
Event EV_Player_StopLocalSound
(
    "stoplocalsound",
    EV_DEFAULT,
    "sF",
    "soundName time",
    "Stops the specified sound.",
    EV_NORMAL
);
Event EV_Player_Userinfo
(
    "userinfo",
    EV_DEFAULT,
    NULL,
    NULL,
    "returns userinfo string",
    EV_GETTER
);

Event EV_Player_ViewModelGetAnim
(
    "viewmodelgetanim",
    EV_DEFAULT,
    "B",
    "fullanim",
    "Gets the player's current view model animation.",
    EV_RETURN
);
Event EV_Player_ViewModelAnimFinished
(
    "viewmodelanimfinished",
    EV_DEFAULT,
    NULL,
    NULL,
    "True if the player's current view model finished its animation.",
    EV_RETURN
);
Event EV_Player_ViewModelAnimValid
(
    "viewmodelanimvalid",
    EV_DEFAULT,
    "sB",
    "anim fullanim",
    "True if the view model animation is valid.",
    EV_RETURN
);

#ifdef OPM_FEATURES
Event EV_Player_Earthquake
(
    "earthquake2",
    EV_DEFAULT,
    "ffbbVF",
    "duration magnitude no_rampup no_rampdown location radius",
    "Create a smooth realistic earthquake for a player. Requires sv_reborn to be set.",
    EV_NORMAL
);
Event EV_Player_Replicate
(
    "replicate",
    EV_DEFAULT,
    "s",
    "variable",
    "Replicate a variable to the client (needs patch 1.12).",
    EV_NORMAL
);
Event EV_Player_SetClientFlag
(
    "setclientflag",
    EV_DEFAULT,
    "s",
    "name",
    "Calls a flag to the script client.",
    EV_NORMAL
);
Event EV_Player_SetEntityShader
(
    "setentshader",
    EV_DEFAULT,
    "es",
    "entity shadername",
    "Sets an entity shader for this player. An empty string will revert to the normal entity shader.",
    EV_NORMAL
);
Event EV_Player_SetLocalSoundRate
(
    "setlocalsoundrate",
    EV_DEFAULT,
    "sfF",
    "name rate time",
    "Sets the local sound rate.",
    EV_NORMAL
);
Event EV_Player_SetViewModelAnimSpeed
(
    "setvmaspeed",
    EV_DEFAULT,
    "sf",
    "name speed",
    "Sets the player's animation speed when playing it.",
    EV_NORMAL
);
Event EV_Player_VisionSetBlur
(
    "visionsetblur",
    EV_DEFAULT,
    "fF",
    "level transition_time",
    "Sets the player's blur level. Level is a fraction from 0-1",
    EV_NORMAL
);
Event EV_Player_VisionGetNaked
(
    "visiongetnaked",
    EV_DEFAULT,
    NULL,
    NULL,
    "Gets the player's current naked-eye vision.",
    EV_RETURN
);
Event EV_Player_VisionSetNaked
(
    "visionsetnaked",
    EV_DEFAULT,
    "sFF",
    "vision_name transition_time phase",
    "Sets the player's naked-eye vision. Optionally give a transition time from the current vision. If vision_name is "
    "an empty string, it will revert to the current global vision.",
    EV_NORMAL
);
#endif

qboolean TryPush(int entnum, vec3_t move_origin, vec3_t move_end);

/*
==============================================================================

PLAYER

==============================================================================
*/

CLASS_DECLARATION(Sentient, Player, "player") {
    {&EV_Vehicle_Enter,                   &Player::EnterVehicle                 },
    {&EV_Vehicle_Exit,                    &Player::ExitVehicle                  },
    {&EV_Turret_Enter,                    &Player::EnterTurret                  },
    {&EV_Turret_Exit,                     &Player::ExitTurret                   },
    {&EV_Player_EndLevel,                 &Player::EndLevel                     },
    {&EV_Player_PrevItem,                 &Player::SelectPreviousItem           },
    {&EV_Player_NextItem,                 &Player::SelectNextItem               },
    {&EV_Player_PrevWeapon,               &Player::SelectPreviousWeapon         },
    {&EV_Player_NextWeapon,               &Player::SelectNextWeapon             },
    {&EV_Player_DropWeapon,               &Player::DropCurrentWeapon            },
    {&EV_Player_Reload,                   &Player::PlayerReload                 },
    {&EV_Player_CorrectWeaponAttachments, &Player::EventCorrectWeaponAttachments},
    {&EV_Player_GiveCheat,                &Player::GiveCheat                    },
    {&EV_Player_GiveWeaponCheat,          &Player::GiveWeaponCheat              },
    {&EV_Player_GiveAllCheat,             &Player::GiveAllCheat                 },
    {&EV_Player_GiveNewWeaponsCheat,      &Player::GiveNewWeaponsCheat          },
    {&EV_Player_DevGodCheat,              &Player::GodCheat                     },
    {&EV_Player_FullHeal,                 &Player::FullHeal                     },
    {&EV_Player_DevNoTargetCheat,         &Player::NoTargetCheat                },
    {&EV_Player_DevNoClipCheat,           &Player::NoclipCheat                  },
    {&EV_Player_GameVersion,              &Player::GameVersion                  },
    {&EV_Player_CoopProf,                 &Player::EventCoopProf                },
    {&EV_Player_DumpState,                &Player::DumpState                    },
    {&EV_Player_ForceTorsoState,          &Player::ForceTorsoState              },
    {&EV_Player_ForceLegsState,           &Player::ForceLegsState               },
    {&EV_Player_Fov,                      &Player::EventSetSelectedFov          },
    {&EV_Kill,                            &Player::Kill                         },
    {&EV_Player_Dead,                     &Player::Dead                         },
    {&EV_Player_SpawnEntity,              &Player::SpawnEntity                  },
    {&EV_Player_SpawnActor,               &Player::SpawnActor                   },
    {&EV_Player_Respawn,                  &Player::Respawn                      },
    {&EV_Player_DoUse,                    &Player::DoUse                        },
    {&EV_Pain,                            &Player::Pain                         },
    {&EV_Killed,                          &Player::Killed                       },
    {&EV_GotKill,                         &Player::GotKill                      },
    {&EV_Player_TestThread,               &Player::TestThread                   },
    {&EV_Player_PowerupTimer,             &Player::SetPowerupTimer              },
    {&EV_Player_UpdatePowerupTimer,       &Player::UpdatePowerupTimer           },
    {&EV_Player_ResetState,               &Player::ResetState                   },
    {&EV_Player_WhatIs,                   &Player::WhatIs                       },
    {&EV_Player_ActorInfo,                &Player::ActorInfo                    },
    {&EV_Player_KillEnt,                  &Player::KillEnt                      },
    {&EV_Player_RemoveEnt,                &Player::RemoveEnt                    },
    {&EV_Player_KillClass,                &Player::KillClass                    },
    {&EV_Player_RemoveClass,              &Player::RemoveClass                  },
    {&EV_Player_AnimLoop_Legs,            &Player::EndAnim_Legs                 },
    {&EV_Player_AnimLoop_Torso,           &Player::EndAnim_Torso                },
    {&EV_Player_AnimLoop_Pain,            &Player::EndAnim_Pain                 },
    {&EV_Player_Jump,                     &Player::Jump                         },
    {&EV_Sentient_JumpXY,                 &Player::JumpXY                       },
    {&EV_Player_ListInventory,            &Player::ListInventoryEvent           },
    {&EV_Player_NextPainTime,             &Player::NextPainTime                 },
    {&EV_Player_Turn,                     &Player::Turn                         },
    {&EV_Player_TurnUpdate,               &Player::TurnUpdate                   },
    {&EV_Player_TurnLegs,                 &Player::TurnLegs                     },
    {&EV_Player_FinishUseAnim,            &Player::FinishUseAnim                },
    {&EV_Player_Holster,                  &Player::HolsterToggle                },
    {&EV_Player_SafeHolster,              &Player::Holster                      },
    {&EV_Player_CoopLobbyPose,           &Player::CoopLobbyPose                },
    {&EV_Player_CoopLobbyUnpose,         &Player::CoopLobbyUnpose              },
    {&EV_Player_CoopLobbyRepose,         &Player::CoopLobbyRepose              },
    {&EV_Player_CoopLobbyCycleAnim,      &Player::CoopLobbyCycleAnim           },
    {&EV_Player_CoopLimpTest,            &Player::EventCoopLimpTest            },
    {&EV_Player_CoopNavRec,              &Player::EventCoopNavRec              },
    {&EV_Player_CoopNavNode,             &Player::EventCoopNavNode             },
    {&EV_Player_CoopNavBuild,            &Player::EventCoopNavBuild            },
    {&EV_Player_CoopLobbyHoldPose,       &Player::CoopLobbyHoldPose            },
    {&EV_Player_CoopLobbyInput,          &Player::CoopLobbyInput               },
    {&EV_Player_CoopLobbyCursor,         &Player::CoopLobbyCursor              },
    {&EV_Player_SafeZoom,                 &Player::SafeZoomed                   },
    {&EV_Player_ZoomOff,                  &Player::ZoomOffEvent                 },
    {&EV_Player_StartUseObject,           &Player::StartUseObject               },
    {&EV_Player_FinishUseObject,          &Player::FinishUseObject              },
    {&EV_Player_WatchActor,               &Player::WatchActor                   },
    {&EV_Player_StopWatchingActor,        &Player::StopWatchingActor            },
    {&EV_Player_SetDamageMultiplier,      &Player::SetDamageMultiplier          },
    {&EV_Player_WaitForState,             &Player::WaitForState                 },
    {&EV_Player_LogStats,                 &Player::LogStats                     },
    {&EV_Player_TakePain,                 &Player::SetTakePain                  },
    {&EV_Player_SkipCinematic,            &Player::SkipCinematic                },
    {&EV_Player_ResetHaveItem,            &Player::ResetHaveItem                },
    {&EV_Show,                            &Player::PlayerShowModel              },
    {&EV_Player_ModifyHeight,             &Player::ModifyHeight                 },
    {&EV_Player_ModifyHeightFloat,        &Player::ModifyHeightFloat            },
    {&EV_Player_SetMovePosFlags,          &Player::SetMovePosFlags              },
    {&EV_Player_GetPosition,              &Player::GetPositionForScript         },
    {&EV_Player_GetMovement,              &Player::GetMovementForScript         },
    {&EV_Player_Teleport,                 &Player::EventTeleport                },
    {&EV_Player_Face,                     &Player::EventFace                    },
    {&EV_Player_Coord,                    &Player::EventCoord                   },
    {&EV_Player_TestAnim,                 &Player::EventTestAnim                },
    {&EV_Player_JailIsEscaping,           &Player::EventGetIsEscaping           },
    {&EV_Player_JailEscapeStop,           &Player::EventJailEscapeStop          },
    {&EV_Player_JailAssistEscape,         &Player::EventJailAssistEscape        },
    {&EV_Player_JailEscape,               &Player::EventJailEscape              },
    {&EV_Player_Score,                    &Player::Score                        },
    {&EV_Player_JoinDMTeam,               &Player::Join_DM_Team                 },
    {&EV_Player_AutoJoinDMTeam,           &Player::Auto_Join_DM_Team            },
    {&EV_Player_LeaveTeam,                &Player::Leave_DM_Team                },
    {&EV_Player_IsSpectator,              &Player::GetIsSpectator               },
    {&EV_Player_GetNationalityPrefix,     &Player::GetNationalityPrefix         },
    {&EV_Player_SetInJail,                &Player::EventSetInJail               },
    {&EV_Player_GetInJail,                &Player::EventGetInJail               },
    {&EV_Player_Spectator,                &Player::Spectator                    },
    {&EV_Player_PickWeapon,               &Player::PickWeaponEvent              },
    {&EV_Player_CallVote,                 &Player::CallVote                     },
    {&EV_Player_Vote,                     &Player::Vote                         },
    {&EV_Player_RetrieveVoteOptions,      &Player::RetrieveVoteOptions          },
    {&EV_Player_PrimaryDMWeapon,          &Player::EventPrimaryDMWeapon         },
    {&EV_Player_DeadBody,                 &Player::DeadBody                     },
    {&EV_Player_ArmWithWeapons,           &Player::ArmWithWeapons               },
    {&EV_Player_GetCurrentDMWeaponType,   &Player::EventGetCurrentDMWeaponType  },
    {&EV_Player_Physics_On,               &Player::PhysicsOn                    },
    {&EV_Player_Physics_Off,              &Player::PhysicsOff                   },
    {&EV_Player_AttachToLadder,           &Player::AttachToLadder               },
    {&EV_Player_UnattachFromLadder,       &Player::UnattachFromLadder           },
    {&EV_Player_TweakLadderPos,           &Player::TweakLadderPos               },
    {&EV_Player_EnsureOverLadder,         &Player::EnsureOverLadder             },
    {&EV_Player_EnsureForwardOffLadder,   &Player::EnsureForwardOffLadder       },
    {&EV_Damage,                          &Player::ArmorDamage                  },
    {&EV_Player_GetIsDisguised,           &Player::GetIsDisguised               },
    {&EV_Player_GetHasDisguise,           &Player::GetHasDisguise               },
    {&EV_Player_SetHasDisguise,           &Player::SetHasDisguise               },
    {&EV_Player_ObjectiveCount,           &Player::SetObjectiveCount            },
    {&EV_Player_Stats,                    &Player::Stats                        },
    {&EV_Player_StuffText,                &Player::EventStuffText               },
    {&EV_Player_DMMessage,                &Player::EventDMMessage               },
    {&EV_Player_IPrint,                   &Player::EventIPrint                  },
    {&EV_SetViewangles,                   &Player::SetViewangles                },
    {&EV_GetViewangles,                   &Player::GetViewangles                },
    {&EV_GetUseHeld,                      &Player::EventGetUseHeld              },
    {&EV_GetFireHeld,                     &Player::EventGetFireHeld             },
    {&EV_GetPrimaryFireHeld,              &Player::EventGetPrimaryFireHeld      },
    {&EV_GetSecondaryFireHeld,            &Player::EventGetSecondaryFireHeld    },
    {&EV_GetCoopAdsHeld,                  &Player::EventGetCoopAdsHeld          },
    {&EV_Player_CoopKillWall,             &Player::EventCoopKillWall            }, // HZM coop - wall probe v5
    {&EV_Player_CoopMarkWall,             &Player::EventCoopMarkWall            }, // HZM coop - wall probe v5
    {&EV_Player_CoopSetDbno,              &Player::EventCoopSetDbno             }, // HZM coop - DBNO state [user 08-02]
    {&EV_Player_CoopSetCover,             &Player::EventCoopSetCover            }, // HZM coop - take cover [214]
    {&EV_Player_GetCoopCover,             &Player::EventGetCoopCover            }, // HZM coop - take cover [214]
    {&EV_Player_GetReady,                 &Player::EventGetReady                },
    {&EV_Player_SetReady,                 &Player::EventSetReady                },
    {&EV_Player_SetNotReady,              &Player::EventSetNotReady             },
    {&EV_Player_GetDMTeam,                &Player::EventGetDMTeam               },
    {&EV_Player_GetName,                  &Player::EventGetNetName              },
    {&EV_Player_SetViewModelAnim,         &Player::EventSetViewModelAnim        },
    {&EV_Player_DMDeathDrop,              &Player::EventDMDeathDrop             },
    {&EV_Player_Stopwatch,                &Player::EventStopwatch               },
    {&EV_Player_EnterIntermission,        &Player::EventEnterIntermission       },
    {&EV_Player_SetPerferredWeapon,       &Player::EventSetPerferredWeapon      },
    {&EV_Player_SetVoiceType,             &Player::EventSetVoiceType            },
    {&EV_Player_AddKills,                 &Player::EventAddKills                },
    {&EV_Player_KillAxis,                 &Player::EventKillAxis                },
    {&EV_Player_GetTurret,                &Player::EventGetTurret               },
    {&EV_Player_GetVehicle,               &Player::EventGetVehicle              },

    {&EV_Player_AddDeaths,                &Player::AddDeaths                    },
    {&EV_Player_AdminRights,              &Player::AdminRights                  },
    {&EV_Player_BindWeap,                 &Player::BindWeap                     },
    {&EV_Player_Dive,                     &Player::Dive                         },
    {&EV_Player_FreezeControls,           &Player::FreezeControls               },
    {&EV_Player_SetTeam,                  &Player::EventSetTeam                 },
    {&EV_Player_GetConnState,             &Player::GetConnState                 },
    {&EV_Player_GetDamageMultiplier,      &Player::GetDamageMultiplier          },
    {&EV_Player_GetDeaths,                &Player::GetDeaths                    },
    {&EV_Player_GetKillHandler,           &Player::GetKillHandler               },
    {&EV_Player_GetKills,                 &Player::GetKills                     },
    {&EV_Player_GetLegsState,             &Player::GetLegsState                 },
    {&EV_Player_GetStateFile,             &Player::GetStateFile                 },
    {&EV_Player_GetTorsoState,            &Player::GetTorsoState                },
    {&EV_Player_GetUserInfo,              &Player::GetUserInfo                  },
    {&EV_Player_HideEnt,                  &Player::HideEntity                   },
    {&EV_Player_Inventory,                &Player::Inventory                    },
    {&EV_Player_InventorySet,             &Player::InventorySet                 },
    {&EV_Player_IsSpectator,              &Player::GetIsSpectator               },
    {&EV_Player_IsAdmin,                  &Player::IsAdmin                      },
    {&EV_Player_LeanLeftHeld,             &Player::LeanLeftHeld                 },
    {&EV_Player_LeanRightHeld,            &Player::LeanRightHeld                },
    {&EV_Player_MoveSpeedScale,           &Player::SetSpeed                     },
    {&EV_Player_MoveSpeedScaleGet,        &Player::GetMoveSpeedScale            },
    {&EV_Player_PlayLocalSound,           &Player::PlayLocalSound               },
    {&EV_Player_RunHeld,                  &Player::RunHeld                      },
    {&EV_Player_SecFireHeld,              &Player::SecFireHeld                  },
    {&EV_Player_SetAnimSpeed,             &Player::SetAnimSpeed                 },
    {&EV_Player_SetKillHandler,           &Player::SetKillHandler               },
    {&EV_Player_SetSpeed,                 &Player::SetSpeed                     },
    {&EV_Player_SetStateFile,             &Player::SetStateFile                 },
    {&EV_Player_ShowEnt,                  &Player::ShowEntity                   },
    {&EV_Player_Spectator,                &Player::Spectator                    },
    {&EV_Player_StopLocalSound,           &Player::StopLocalSound               },
    {&EV_Player_Userinfo,                 &Player::Userinfo                     },
    {&EV_Player_ViewModelAnimFinished,    &Player::EventGetViewModelAnimFinished},
    {&EV_Player_ViewModelGetAnim,         &Player::EventGetViewModelAnim        },
    {&EV_Player_ViewModelAnimValid,       &Player::EventGetViewModelAnimValid   },
#ifdef OPM_FEATURES
    {&EV_Player_Earthquake,               &Player::EventEarthquake              },
    {&EV_Player_SetClientFlag,            &Player::SetClientFlag                },
    {&EV_Player_SetEntityShader,          &Player::SetEntityShader              },
    {&EV_Player_SetLocalSoundRate,        &Player::SetLocalSoundRate            },
    {&EV_Player_SetViewModelAnimSpeed,    &Player::SetVMASpeed                  },
    {&EV_Player_VisionGetNaked,           &Player::VisionGetNaked               },
    {&EV_Player_VisionSetBlur,            &Player::VisionSetBlur                },
    {&EV_Player_VisionSetNaked,           &Player::VisionSetNaked               },
#endif
    {NULL,                                NULL                                  }
};

movecontrolfunc_t Player::MoveStartFuncs[] = {
    NULL, // MOVECONTROL_USER,				// Quake style
    NULL, // MOVECONTROL_LEGS,				// Quake style, legs state system active
    NULL, // MOVECONTROL_USER_MOVEANIM,		// Quake style, legs state system active
    NULL, // MOVECONTROL_ANIM,				// move based on animation, with full collision testing
    NULL, // MOVECONTROL_ABSOLUTE,			// move based on animation, with full collision testing but no turning
    NULL, // MOVECONTROL_HANGING,				// move based on animation, with full collision testing, hanging
    NULL, // MOVECONTROL_ROPE_GRAB
    NULL, // MOVECONTROL_ROPE_RELEASE
    NULL, // MOVECONTROL_ROPE_MOVE
    NULL, // MOVECONTROL_PICKUPENEMY
    &Player::StartPush,        // MOVECONTROL_PUSH
    NULL,                      // MOVECONTROL_CLIMBWALL
    &Player::StartUseAnim,     // MOVECONTROL_USEANIM
    NULL,                      // MOVECONTROL_CROUCH
    &Player::StartLoopUseAnim, // MOVECONTROL_LOOPUSEANIM
    &Player::SetupUseObject,   // MOVECONTROL_USEOBJECT
    NULL,                      // MOVECONTROL_COOLOBJECT
};

Player::Player()
{
    //
    // set the entity type
    //
    entflags |= ECF_PLAYER;

    mCurTrailOrigin   = 0;
    mLastTrailTime    = 0;
    m_pLastSpawnpoint = NULL;

    voted                      = false;
    m_fInvulnerableTimeElapsed = 0;
    m_voiceType                = PVT_NONE_SET;
    m_fTalkTime                = 0;
    num_deaths                 = 0;
    num_kills                  = 0;
    num_won_matches            = 0;
    num_lost_matches           = 0;
    num_team_kills             = 0;
    m_iLastNumTeamKills        = 0;
    m_bTempSpectator           = false;
    m_bSpectator               = false;
    m_bSpectatorSwitching      = false;
    m_bAllowFighting           = false;
    m_bReady                   = false;
    m_iPlayerSpectating        = 0;
    dm_team                    = TEAM_NONE;
    m_fTeamSelectTime          = -30;
    votecount                  = 0;
    m_fNextVoteOptionTime      = 0;
    m_fWeapSelectTime          = 0;

    fAttackerDispTime   = 0;
    m_iInfoClient       = 0;
    m_iInfoClientHealth = 0;
    m_fInfoClientTime   = 0;

    m_bDeathSpectator            = false;
    m_fSpawnTimeLeft             = 0;
    m_bWaitingForRespawn         = 0;
    m_bShouldRespawn             = false;
    last_camera_type             = -1;
    m_fLastInvulnerableTime      = 0;
    m_iInvulnerableTimeRemaining = 0;
    m_fLastVoteTime              = 0;

    //
    // Added in OPM
    //====
    m_bConnected = false;

    m_iInstantMessageTime = 0;
    m_iTextChatTime       = 0;
    //====

    // [vet 2026-08-28] SEEDED ABOVE THE LoadingSavegame RETURN. Player memory comes from gi.Malloc
    // with no memset, and Player::Archive persists NONE of these coop members - so every one of them
    // held indeterminate heap bytes after a savegame load. All 118 sat below the early return. The
    // worst of them had no repair path: m_fCoopHitMarkNext is only rewritten on a NON-kill hit, so a
    // garbage-high value killed hit markers for the rest of the session. weapon.cpp:1027 already
    // established this exact pattern on 2026-08-21; player.cpp never got the same treatment.
    m_szCoopFireLast[0]     = 0;
    m_fCoopStamina    = 9999.0f;
    m_fCoopStress     = 0.0f;
    m_iCoopSuppHits   = 0;
    m_bCoopSprinting  = false;
    m_fCoopSlideNext  = 0;
    m_bCoopSliding    = false;
    m_bCoopNadeHeld   = false;
    m_fCoopNadeT0     = 0;
    m_fCoopNadeThrow  = 0;
    m_fCoopHeadPitch  = 0;
    m_fCoopTorsoLag   = 0;
    m_fCoopPrevViewYaw = 0;
    m_fCoopProneRollEnd = 0;
    m_iCoopProneRollDir = 0;
    m_iCoopProneLeanPrev = 0;
    m_bCoopSupine = false;
    m_fCoopSupineFlip = 0;
    m_iCoopSupineFlipDir = 0;
    m_bCoopDiedSupine = false;
    m_fCoopProneExitAt = 0;
    m_fCoopProneYawTarget = 0;
    m_iCoopProneTurnDir = 0;
    m_bCoopCrawlNoFire = false;
    m_bCoopProneWant  = false;
    m_fCoopCrouchHeld = 0;
    m_bCoopProneKeyBlock = false;
    m_fCoopCrouchUp   = 0;
    m_fCoopProneEnter = 0;
    m_fCoopReadyUpAt      = 0.0f;
    m_iCoopGunHeftSent    = -1;
    m_fCoopRecoilLast     = 0.0f;
    m_fCoopStressLast     = 0.0f;
    m_fCoopBraceLast      = 0.0f;
    m_fCoopRecoilMinDecay = 12.0f;
    m_fCoopRecoilMaxDecay = 25.0f;
    m_fCoopStaminaHold    = 0.0f;
    m_bCoopJumpPrev       = false;
    m_fCoopRecoilRecenter = 0.0f;
    m_fCoopBraceRest     = 0.0f;
    m_iCoopBraceRestSent = -1;
    m_fCoopSupineRefYaw = 0.0f;
    m_fCoopBrace          = 0.0f;
    m_fCoopBraceDwell     = 0.0f;
    m_fCoopBraceHold      = 0.0f;
    m_fCoopBraceYaw       = 0.0f;
    m_bCoopBraceStill     = false;
    m_bCoopBraceAvail     = false;
    m_bCoopBraceMounted   = false;
    m_bCoopBraceUsePrev   = false;
    m_fCoopHitMarkNext    = 0.0f;
    m_iCoopHsCueSent      = 0;
    m_iCoopBraceAvailSent = -1;
    m_iCoopBraceYawSent   = -9999;
    m_bCoopBfShotDone = false;
    m_bCoopNavRec     = false;
    m_bCoopNavFull    = false;
    m_iCoopNavCount   = 0;
    m_fCoopCoverEdge      = 0.0f;
    m_iCoopCoverSideWant  = 0;
    m_fCoopCoverSideDwell = 0.0f;
    m_fCoopCoverLastYaw   = 0.0f;
    m_iCoopVarCoverLast = -1;
    m_iCoopLobbyRightPrev = 0;
    m_bCoopLobbyUsePrev   = false;
    m_bCoopLobbyCurInit   = false;
    m_iCoopLobbyYawPrev   = 0;
    m_iCoopLobbyPitchPrev = 0;
    m_fCoopLobbyCurX      = 320.0f;
    m_fCoopLobbyCurY      = 240.0f;
    m_bCoopLobbyAtkPrev   = false;
    m_fCoopSprintDur  = 0.0f;
    m_iCoopBreathRemainMs   = -1;
    m_iCoopBreathCooldownMs = 0;
    m_iCoopBreathLastMs     = 0;
    m_bCoopBreathSteady     = qfalse;
    m_bCoopCoverRequested = false;
    m_fCoopCoverAutoRetry = 0.0f;
    m_bCoopCoverWall      = false;
    m_bCoopCoverLow       = false;
    m_bCoopBlindfire      = false;
    m_fCoopCoverBadTime   = 0.0f;

    m_fCoopStressSupp = 0.0f;   // HZM coop [user 2026-08-25] - server stress
    m_bCoopLimping    = false;   // HZM coop - low-health limp (bug-1291)
    m_fCoopSlideEnd   = 0;       // HZM coop [user 2026-08-24] - sprint-to-slide
    m_iCoopNadeState  = 0;       // HZM coop [user 2026-08-24] - quick grenade
    m_fCoopHeadYaw    = 0;       // HZM coop [user 2026-08-24] - head look / torso lag
    m_bCoopProne      = false;   // HZM coop [user 2026-08-24] - prone
    m_fCoopProneBodyYaw = 0;   // P1 fluidity
    m_bCoopWounded    = false;   // HZM coop - bug-1324
    m_iCoopLimpSent   = -1;      // force the first coop_limpView stuff, whatever its value
    m_iCoopVaultSent  = 0;       // HZM coop - vault pulse counter (see player.h)
    m_iCoopCoverSent  = -1;      // HZM coop - same for coop_coverView
    m_bCoopSupineArmsOn = false;      // [v3] never read from uninitialised heap - see bug-2133
    m_iCoopBraceSent      = -1;    // impossible: forces one send on first evaluation
    m_iCoopBraceMountedSent = -1;  // same, for the binary mount flag
    m_iCoopDaylightSent   = -1;    // same, for the time-of-day scalar
    m_iCoopBfButtons  = 0;       // HZM coop - semi-auto blindfire edge tracking
    m_bCoopShoulderAim = false; // HZM coop - 3P shoulder-aim stage (userinfo mirror)
    m_bCoopView3p      = false; // HZM coop - client view mode (u_view3p userinfo mirror)
    m_vCoopCoverNormal = vec_zero; // HZM coop - anchored cover OUT normal [215]
    m_iCoopCoverSide   = 0;        // HZM coop - 0 = NONE. Never assume a side (bug-2028)
    m_iCoopCoverSideSent  = -99;   // impossible value: forces one send on first evaluation
    m_bCoopCoverPeek   = false;    // HZM coop - RMB peek-aim from cover [215]
    m_fCoopVehTurretTime = -10.0f; // HZM coop - vehicle-turret manning stamp [219]
    m_fCoopProbeTime   = -10.0f;   // HZM coop - GUNNERPROBE throttle [221]
    m_pCoopBotTarget   = NULL;     // HZM coop - bot combat drive (dev/test, coop_botInput) target cache
    m_iCoopBotRetarget = 0;        // HZM coop - bot combat drive next-rescan stamp
    m_iCoopCoverTypeLast = -1;   // [bug-2090] force the first coop_coverType push      // HZM coop - force the first coop_incover var push [235]
    m_bCoopLobbyInputOn   = false; // HZM coop - lobby usercmd input bridge stays off until the lobby enables it
    m_bCoopLobbyCursorOn  = false; // HZM coop - lobby mouse-cursor bridge (clickable lobby UI) stays off until enabled
    m_vCoopCoverBaseOrg = vec_zero; // HZM coop - cover pose anchor position [216]
    m_fCoopPeekFrac    = 0.0f;     // HZM coop - eased peek step-out fraction [216]
    m_bCoopGearLoop  = false; // HZM coop - gear rattle off
    m_bCoopDbno           = false;   // HZM coop [user 08-02]
    m_fCoopCoverAutoDwell = 0.0f; // HZM coop [user 2026-08-09] auto-cover dwell/backoff
    m_vCoopRecoilOwed     = Vector(0, 0, 0);
    m_iCoopSpeedBase      = 0;   // [vet] was never assigned anywhere - the SPEEDPROBE read it raw
    if (LoadingSavegame) {
        return;
    }

    actor_camera_right          = false;
    starting_actor_camera_right = false;
    useanim_numloops            = 1;
    move_right_vel              = 0;

    m_iInfoClient       = -1;
    m_iInfoClientHealth = 0;
    m_fInfoClientTime   = 0.0;

    music_current_volume   = -1;
    music_saved_volume     = -1;
    music_volume_fade_time = -1;

    feetfalling        = false;
    edict->s.eType     = ET_PLAYER;
    buttons            = 0;
    new_buttons        = 0;
    server_new_buttons = 0;
    m_bFireLockUntilRelease = false;
    respawn_time       = -1.0;

    //
    // State
    //
    statemap_Legs      = NULL;
    statemap_Torso     = NULL;
    m_fPartBlends[0]   = 0;
    m_fPartBlends[1]   = 0;
    m_iPartSlot[legs]  = 0;
    m_iPartSlot[torso] = 2;
    partBlendMult[0]   = 0;
    partBlendMult[1]   = 0;
    m_fPainBlend       = 0;
    animdone_Pain      = false;

    m_fLastDeltaTime = level.time;

    camera         = NULL;
    atobject       = NULL;
    atobject_dist  = 0;
    toucheduseanim = NULL;
    useitem_in_use = NULL;

    damage_blood = 0;
    damage_count = 0;
    damage_from  = vec_zero;
    damage_alpha = 0;
    damage_yaw   = 0;

    fAttackerDispTime    = 0;
    pAttackerDistPointer = NULL;
    last_attack_button   = 0;
    attack_blocked       = false;
    canfall              = false;

    move_left_vel     = 0;
    move_right_vel    = 0;
    move_backward_vel = 0;
    move_forward_vel  = 0;
    move_up_vel       = 0;
    move_down_vel     = 0;

    moveresult                = 0;
    animspeed                 = 0;
    airspeed                  = 200.0f;
    weapons_holstered_by_code = false;
    actor_camera              = NULL;

    damage_multiplier = 1.0f;
    take_pain         = true;
    m_bIsInJail       = false;
    dm_team           = TEAM_NONE;
    current_team      = NULL;

    m_bTempSpectator      = false;
    m_bSpectator          = false;
    m_bSpectatorSwitching = false;
    m_bAllowFighting      = false;
    m_bReady              = true;
    m_fTeamSelectTime     = -30;
    m_fTalkTime           = 0;

    num_deaths            = 0;
    num_kills             = 0;
    num_team_kills        = 0;
    m_iLastNumTeamKills   = 0;
    num_won_matches       = 0;
    num_lost_matches      = 0;
    client->ps.voted      = false;
    votecount             = 0;
    m_fLastVoteTime       = 0;
    m_fNextVoteOptionTime = 0;
    m_fWeapSelectTime     = 0;
    m_jailstate           = JAILSTATE_NONE;

    SetSelectedFov(atof(Info_ValueForKey(client->pers.userinfo, "fov")));
    SetFov(selectedfov);

    m_iInZoomMode       = 0;
    m_iNumShotsFired    = 0;
    m_iNumHits          = 0;
    m_iNumGroinShots    = 0;
    m_iNumHeadShots     = 0;
    m_iNumLeftArmShots  = 0;
    m_iNumLeftLegShots  = 0;
    m_iNumRightArmShots = 0;
    m_iNumRightLegShots = 0;
    m_iNumTorsoShots    = 0;

    m_sPerferredWeaponOverride = "";

    SetTargetName("player");

    Init();

    for (int i = 0; i < MAX_TRAILS; i++) {
        mvTrail[i] = Vector(0, 0, 0);
    }

    for (int i = 0; i < MAX_TRAILS; i++) {
        mvTrailEyes[i] = Vector(0, 0, 0);
    }

    client->ps.pm_flags &= ~PMF_NO_HUD;

    m_fLastSprintTime = 0;
    // start with a full stamina pool on (re)spawn; TickSprint clamps this down to the cvar max each frame
    // HZM coop [user 2026-08-27, bug-2133] BRACE STATE - seeded here for two separate reasons.
    // (1) Player is allocated through gi.Malloc, which does NOT zero: every one of these was read
    // from indeterminate heap memory, and m_fCoopBrace is consumed by TickCoopStress ~60 lines
    // BEFORE TickCoopBrace first writes it. (2) The publish is change-only, so without an
    // impossible sentinel a fresh Player whose counter happened to hold 100 would never send
    // 'set coop_braceView 0' - and a client that changed map while mounted would keep a clamped
    // aim cone and forced ADS for the rest of the session with no way out. Same sentinel trick
    // the limp and cover channels above already use, and for exactly the same reason.
    // HZM coop [user 2026-08-17] - breath budget; -1 means "uninitialised", filled on first tick
    // HZM coop - TAKE COVER [214]: start clear (no request, no valid pose)
    m_bHasJumped      = false;

    m_fLastInvulnerableTime      = 0;
    m_iInvulnerableTimeRemaining = -1;
    m_fSpawnTimeLeft             = 0;
    m_bWaitingForRespawn         = false;
    m_bShouldRespawn             = false;
    m_bDeathSpectator            = false;

    m_vViewPos = vec_zero;

    //
    // Added in OPM
    //

    m_bFrozen  = false;

    for (int i = 0; i < MAX_SPEED_MULTIPLIERS; i++) {
        speed_multiplier[i] = 1.0f;
    }

    m_fpsTiki    = NULL;
    animDoneVM = true;
    m_fVMAtime = 0;

#ifdef OPM_FEATURES
    m_bShowingHint = false;
#endif
}

Player::~Player()
{
    int          i, num;
    Conditional *cond;

    num = legs_conditionals.NumObjects();
    for (i = num; i > 0; i--) {
        cond = legs_conditionals.ObjectAt(i);
        delete cond;
    }

    num = torso_conditionals.NumObjects();
    for (i = num; i > 0; i--) {
        cond = torso_conditionals.ObjectAt(i);
        delete cond;
    }

    legs_conditionals.FreeObjectList();
    torso_conditionals.FreeObjectList();

    // Added in 2.11
    //  Make sure to clean turret stuff up
    //  when the player is deleted
    RemoveFromVehiclesAndTurrets();

    // Added in OPM
    //  Remove the player at destructor
    if (g_gametype->integer != GT_SINGLE_PLAYER && dmManager.PlayerCount()) {
        dmManager.RemovePlayer(this);
    }

    entflags &= ~ECF_PLAYER;
}

static qboolean logfile_started = qfalse;

void Player::Init(void)
{
    InitClient();
    InitPhysics();
    InitPowerups();
    InitWorldEffects();
    InitSound();
    InitView();
    InitState();
    InitEdict();
    InitMaxAmmo();
    InitWeapons();
    InitInventory();
    InitHealth();
    InitStats();
    InitModel();
    InitInvulnerable();

    LoadStateTable();

    if (g_gametype->integer != GT_SINGLE_PLAYER) {
        InitDeathmatch();
    } else if (!LoadingSavegame) {
        ChooseSpawnPoint();
        JoinNearbySquads();
    }

    // make sure we put the player back into the world
    link();
    logfile_started = qfalse;

    // notify scripts for the spawning player
    parm.other = this;
    parm.owner = this;
    level.Unregister(STRING_PLAYERSPAWN);

    //
    // Added in OPM
    //
    if (!m_bConnected) {
        m_bConnected = true;

        Event *ev = new Event;
        ev->AddEntity(this);

        scriptDelegate_connected.Trigger(this, *ev);
        scriptedEvents[SE_CONNECTED].Trigger(ev);
    }

    Spawned();
}

void Player::InitStats(void)
{
    m_iNumObjectives       = 0;
    m_iObjectivesCompleted = 0;
    m_iNumHitsTaken        = 0;
    m_iNumEnemiesKilled    = 0;
    m_iNumObjectsDestroyed = 0;
}

void Player::InitEdict(void)
{
    // entity state stuff
    setSolidType(SOLID_BBOX);
    if (m_bSpectator) {
        //
        // 2.0: always noclip when spectating
        //
        setMoveType(MOVETYPE_NOCLIP);
    } else {
        setMoveType(MOVETYPE_WALK);
    }

    setSize(Vector(-16, -16, 0), Vector(16, 16, 72));

    edict->clipmask   = MASK_PLAYERSOLID;
    edict->r.ownerNum = ENTITYNUM_NONE;

    // clear entity state values
    edict->s.eFlags   = 0;
    edict->s.wasframe = 0;

    // players have precise shadows
    edict->s.renderfx |= RF_SHADOW_PRECISE | RF_SHADOW;
}

void Player::InitSound(void)
{
    //
    // reset the music
    //
    client->ps.current_music_mood  = mood_normal;
    client->ps.fallback_music_mood = mood_normal;
    ChangeMusic("normal", "normal", false);

    client->ps.music_volume           = 1.0;
    client->ps.music_volume_fade_time = 0.0;
    ChangeMusicVolume(1.0, 0.0);

    music_forced = false;

    // Reset the reverb stuff

    client->ps.reverb_type  = eax_generic;
    client->ps.reverb_level = 0;
    SetReverb(client->ps.reverb_type, client->ps.reverb_level);
}

void Player::InitClient(void)
{
    client_persistant_t saved;

    // deathmatch wipes most client data every spawn
    if (g_gametype->integer != GT_SINGLE_PLAYER) {
        char       userinfo[MAX_INFO_STRING];
        char       dm_primary[MAX_QPATH];
        float      enterTime   = client->pers.enterTime;
        teamtype_t team        = client->pers.teamnum;
        int        round_kills = client->pers.round_kills;

        memcpy(userinfo, client->pers.userinfo, sizeof(userinfo));
        memcpy(dm_primary, client->pers.dm_primary, sizeof(dm_primary));
        G_InitClientPersistant(client);
        G_ClientUserinfoChanged(edict, userinfo);

        memcpy(client->pers.dm_primary, dm_primary, sizeof(client->pers.dm_primary));
        client->pers.enterTime   = enterTime;
        client->pers.teamnum     = team;
        client->pers.round_kills = round_kills;
    }

    // clear everything but the persistant data and fov
    saved = client->pers;

    memset(client, 0, sizeof(*client));
    client->pers = saved;

    client->ps.clientNum   = client - game.clients;
    client->lastActiveTime = level.inttime;
    client->ps.commandTime = level.svsTime;

    SetStopwatch(0);

#ifdef OPM_FEATURES
    m_bShowingHint = false;
#endif
}

void Player::InitState(void)
{
    gibbed       = false;
    pain         = 0;
    nextpaintime = 0;

    m_fMineDist      = 1000;
    m_fMineCheckTime = 0;
    m_sDmPrimary     = "";

    knockdown     = false;
    pain_dir      = PAIN_NONE;
    pain_type     = MOD_NONE;
    pain_location = -2;
    takedamage    = DAMAGE_AIM;
    deadflag      = DEAD_NO;
    flags &= ~FL_TEAMSLAVE;
    flags |= (FL_POSTTHINK | FL_THINK | FL_DIE_EXPLODE | FL_BLOOD);
    m_iMovePosFlags = MPF_POSITION_STANDING;

    if (!com_blood->integer) {
        flags &= ~(FL_DIE_EXPLODE | FL_BLOOD);
    }
}

void Player::InitHealth(void)
{
    static cvar_t *pMaxHealth = gi.Cvar_Get("g_maxplayerhealth", "250", 0);
    static cvar_t *pDMHealth  = gi.Cvar_Get("g_playerdmhealth", "100", 0);

    // Don't do anything if we're loading a server game.
    // This is either a loadgame or a restart
    if (LoadingSavegame) {
        return;
    }

    if (g_gametype->integer == GT_SINGLE_PLAYER && !g_realismmode->integer) {
        max_health = pMaxHealth->integer;
    } else if (g_gametype->integer != GT_SINGLE_PLAYER && pDMHealth->integer > 0) {
        max_health = pDMHealth->integer;
    } else {
        // reset the health values
        max_health = 100;
    }

    health = max_health;

    // 2.0:
    //  Make sure to clear the heal rate and the dead flag when respawning
    //
    m_fHealRate = 0;
    m_fRecoilTarget  = 0; // HZM coop - clear view-recoil on (re)spawn (delta_angles is reset here too)
    m_fRecoilApplied = 0;
    // HZM coop - gore tier 1: fresh spawn = clean uniform. InitModel already wipes the surface skin
    // bits; the accumulated-damage counter + tier must follow or the first hit re-bloodies instantly.
    m_fCoopGoreDamage   = 0;
    m_iCoopGoreSkinTier = 0;
    edict->s.eFlags &= ~EF_DEAD;

    // Fixed in OPM
    //  This avoid losing weapons when dying and then immediately respawning
    CancelEventsOfType(EV_Player_DMDeathDrop);
    //  And this prevents the player from dying when respawning immediately after getting killed
    CancelEventsOfType(EV_Player_Dead);
}

void Player::InitModel(void)
{
    static const char *defaultAxisModel   = "models/player/german_wehrmacht_soldier.tik";
    static const char *defaultAlliedModel = "models/player/american_army.tik";

    // 2.0:
    //  Make sure to detach from any object before initializing
    //  To prevent any glitches
    RemoveFromVehiclesAndTurrets();
    UnattachFromLadder(NULL);

    gi.clearmodel(edict);

    if (g_gametype->integer == GT_SINGLE_PLAYER) {
        setModel("models/player/" + str(g_playermodel->string) + ".tik");
    } else if (dm_team == TEAM_AXIS) {
        size_t len = strlen(client->pers.dm_playergermanmodel);

        if (len >= 4 && !Q_stricmp(client->pers.dm_playergermanmodel + len - 4, "_fps")) {
            // Fixed in OPM
            //  Prevent the player from using the first-person model
            setModel(defaultAxisModel);
        } else if (Q_stricmpn(client->pers.dm_playergermanmodel, "german", 6)
                   && Q_stricmpn(client->pers.dm_playergermanmodel, "axis", 4)
                   //
                   // 2.30 models
                   //
                   && Q_stricmpn(client->pers.dm_playergermanmodel, "it", 2)
                   && Q_stricmpn(client->pers.dm_playergermanmodel, "sc", 2)) {
            setModel(defaultAxisModel);
        } else {
            setModel("models/player/" + str(client->pers.dm_playergermanmodel) + ".tik");
        }
    } else {
        size_t len = strlen(client->pers.dm_playermodel);

        if (len >= 4 && !Q_stricmp(client->pers.dm_playermodel + len - 4, "_fps")) {
            // Fixed in OPM
            //  Prevent the player from using the first-person model
            setModel(defaultAlliedModel);
        } else if (Q_stricmpn(client->pers.dm_playermodel, "american", 8)
                   && Q_stricmpn(client->pers.dm_playermodel, "allied", 6)) {
            setModel(defaultAlliedModel);
        } else {
            setModel("models/player/" + str(client->pers.dm_playermodel) + ".tik");
        }
    }

    //
    // Fallback to a default model if not found
    //
    if (!edict->tiki) {
        if (dm_team == TEAM_AXIS) {
            setModel("models/player/german_wehrmacht_soldier.tik");
        } else {
            setModel("models/player/american_army.tik");
        }
    }

    SetControllerTag(HEAD_TAG, gi.Tag_NumForName(edict->tiki, "Bip01 Head"));
    SetControllerTag(TORSO_TAG, gi.Tag_NumForName(edict->tiki, "Bip01 Spine2"));
    SetControllerTag(ARMS_TAG, gi.Tag_NumForName(edict->tiki, "Bip01 Spine1"));
    SetControllerTag(PELVIS_TAG, gi.Tag_NumForName(edict->tiki, "Bip01 Pelvis"));
    // HZM coop [v3] remember the resting assignments: while supine these two controllers are
    // TEMPORARILY re-pointed at the clavicles to reverse the arms, and must go back afterwards.
    m_bCoopSupineArmsOn = false;

    if (g_gametype->integer != GT_SINGLE_PLAYER && IsSpectator()) {
        hideModel();
    } else {
        showModel();
    }

    if (GetActiveWeapon(WEAPON_MAIN)) {
        // Show the arms
        edict->s.eFlags &= ~EF_UNARMED;
    } else {
        edict->s.eFlags |= EF_UNARMED;
    }

    edict->s.eFlags &= ~EF_ANY_TEAM;

    if (dm_team == TEAM_ALLIES) {
        edict->s.eFlags |= EF_ALLIES;
    } else if (dm_team == TEAM_AXIS) {
        edict->s.eFlags |= EF_AXIS;
    }

    G_SetClientConfigString(edict);

    client->ps.iViewModelAnim        = 0;
    client->ps.iViewModelAnimChanged = 0;

    if (g_protocol >= protocol_e::PROTOCOL_MOHTA_MIN) {
        if (dm_team == TEAM_AXIS) {
            if (m_voiceType <= PVT_AXIS_START || m_voiceType >= PVT_AXIS_END) {
                m_voiceType = PVT_AXIS_GERMAN;
            }
        } else {
            if (m_voiceType <= PVT_ALLIED_START || m_voiceType >= PVT_ALLIED_END) {
                m_voiceType = PVT_ALLIED_AMERICAN;
            }
        }
    } else {
        if (dm_team == TEAM_AXIS) {
            if (m_voiceType >= PVT_AXIS_END) {
                m_voiceType = PVT_AXIS_AXIS4;
            }
        } else {
            if (m_voiceType >= PVT_ALLIED_END) {
                m_voiceType = PVT_ALLIED_PILOT;
            }
        }
    }

    InitModelFps();
}

void Player::InitPhysics(void)
{
    // Physics stuff
    oldvelocity  = vec_zero;
    velocity     = vec_zero;
    old_v_angle  = v_angle;
    gravity      = 1.0;
    falling      = false;
    mediumimpact = false;
    hardimpact   = false;
    setContents(CONTENTS_BODY);
    mass = 500;
    memset(&last_ucmd, 0, sizeof(last_ucmd));

    client->ps.groundTrace.fraction = 1.0f;

    // Added in OPM
    //  Prevent the player from being stuck
    flags &= ~FL_PARTIAL_IMMOBILE;
}

void Player::InitPowerups(void)
{
    // powerups
    poweruptimer = 0;
    poweruptype  = 0;
}

void Player::InitWorldEffects(void)
{
    // world effects
    next_painsound_time = 0;
}

void Player::InitMaxAmmo(void)
{
    GiveAmmo("pistol", 0, 200);
    GiveAmmo("rifle", 0, 200);
    GiveAmmo("smg", 0, 300);
    GiveAmmo("mg", 0, 500);
    GiveAmmo("grenade", 0, 5);
    GiveAmmo("agrenade", 0, 5);
    GiveAmmo("heavy", 0, 5);
    GiveAmmo("shotgun", 0, 50);

    if (g_target_game >= target_game_e::TG_MOHTT) {
        //
        // Team tactics ammunition
        //
        GiveAmmo("landmine", 0, 5);
    }

    if (g_target_game >= target_game_e::TG_MOHTA) {
        //
        // Team assault ammunition
        //
        GiveAmmo("smokegrenade", 0, 5);
        GiveAmmo("asmokegrenade", 0, 5);
        GiveAmmo("riflegrenade", 0, 3);
    }
}

void Player::InitWeapons(void)
{
    // Don't do anything if we're loading a server game.
    // This is either a loadgame or a restart
    if (LoadingSavegame) {
        return;
    }

    // Added in OPM
    //  This fixes a bug where player can charge then go to spectator or respawn.
    //  The grenade would immediately explode when firing
    charge_start_time = 0;
}

void Player::InitInventory(void) {}

void Player::InitView(void)
{
    // view stuff
    camera  = NULL;
    v_angle = vec_zero;
    SetViewAngles(v_angle);
    viewheight = DEFAULT_VIEWHEIGHT;

    // blend stuff
    damage_blend = vec_zero;
}

void Player::ChooseSpawnPoint(void)
{
    // set up the player's spawn location
    PlayerStart *p = SelectSpawnPoint(this);
    setOrigin(p->origin + Vector(0, 0, 1));
    origin.copyTo(edict->s.origin2);
    edict->s.renderfx |= RF_FRAMELERP;

    if (g_gametype->integer != GT_SINGLE_PLAYER && !IsSpectator()) {
        KillBox(this);
    }

    setAngles(p->angles);
    SetViewAngles(p->angles);
    SetupView();

    VectorCopy(origin, client->ps.vEyePos);
    client->ps.vEyePos[2] += client->ps.viewheight;

    if (g_gametype->integer != GT_SINGLE_PLAYER) {
        for (int i = 1; i <= 4; i++) {
            Event *ev = new Event(EV_SetViewangles);
            ev->AddVector(p->angles);
            PostEvent(ev, level.frametime * i);
        }
    }

    if (p->m_bDeleteOnSpawn) {
        delete p;
    } else {
        p->Unregister(STRING_SPAWN);
        m_pLastSpawnpoint = p;
    }
}

void Player::EndLevel(Event *ev)
{
    if (IsDead()) {
        ScriptError("cannot do player.endlevel if the player is dead");
        return;
    }

    InitPowerups();
    if (health > max_health) {
        health = max_health;
    }

    if (health < 1) {
        health = 1;
    }
}

void Player::Respawn(Event *ev)
{
    // HZM 2026-08-06 (bug-1498) - trust the SERVER SHAPE, not the live cvar. The coop mod's
    // changeGameType hack force-sets g_gametype to 0 (gi.cvar_set bypasses CVAR_LATCH) around
    // SP-only engine calls - the loadout kit give and the disguise-on-spawn give both open such
    // windows on EVERY player spawn. When 4 clients join in the same second, their queued
    // EV_Player_Respawn events drain while another client's window is open; this function then
    // read gametype 0 and took the else-branch below, whose gi.SendConsoleCommand("restart")
    // SILENTLY reloads the whole map (no log line at all). Run qconsole.run.112610.log shows FIVE
    // such hidden restarts on m2l2a, ~48s apart - every script-side restart path's mandatory
    // print is absent, which is how the engine branch was identified. Worse, a restart issued
    // while the cvar is live-0 LATCHES gametype 0 on the reload, so the map comes back running
    // its single-player branches on a 4-client server: every coop gate off, all clients stuck
    // T:spectator H:100. A multiplayer server (maxclients > 1) must NEVER take the SP
    // restart-on-death path, whatever the cvar momentarily claims - fall through to the normal
    // MP respawn instead (reviewer-preferred over a bare return, which would drop the event and
    // strand the player in spectator). Genuine SP (maxclients 1) is unchanged.
    if (g_gametype->integer == GT_SINGLE_PLAYER && game.maxclients > 1) {
        Com_Printf("^~^~^ COV RESPAWN_SP_SUPPRESSED %s (live gametype 0 on a %d-client server)\n",
                   client ? client->pers.netname : "?", game.maxclients);
    }
    if (g_gametype->integer != GT_SINGLE_PLAYER || game.maxclients > 1) {
        bool bOldVoted;

        if (health <= 0.0f) {
            DeadBody(NULL);
            hideModel();
        }

        respawn_time = level.time;

        // HZM coop - gore tier 2: fresh life = clean. Kill any corpse-drip FX still attached to this entity
        // (the player entity respawns in place - without this the bleed-out drip would follow the LIVE body)
        // and reset the accumulated-damage gore counter that drives the wounded-drip tier.
        m_fCoopGoreDamage = 0;
        // bug-754: also reset the TIER latch. InitModel wipes the surface bits but a stale
        // m_iCoopGoreSkinTier=2 made CoopGoreUpdateSkinTier early-out if the next life jumped
        // straight back to the same tier (big first hit) - the bits were never rewritten.
        m_iCoopGoreSkinTier = 0;
        if (m_pCoopDripEmitter) {
            m_pCoopDripEmitter->PostEvent(EV_Remove, 0);
            m_pCoopDripEmitter = NULL;
        }
        // HZM coop - gore tier 3: fresh life = no wound props either (the player entity respawns in
        // place, so last life's attached wound patches must go with the corpse view, not the new body)
        for (int i = 0; i < 4; i++) { // 4 = COOP_GORE_MAX_WOUNDPROPS (sentient.cpp)
            if (m_pCoopWoundProp[i]) {
                m_pCoopWoundProp[i]->PostEvent(EV_Remove, 0);
                m_pCoopWoundProp[i] = NULL;
            }
        }

        // This is not present in MOHAA
        ProcessEvent(EV_Player_UnattachFromLadder);
        RemoveFromVehiclesAndTurrets();

        FreeInventory();

        // Save the previous vote value
        bOldVoted = client->ps.voted;
        Init();
        client->ps.voted = bOldVoted;
        client->ps.pm_flags |= PMF_RESPAWNED;

        SetInvulnerable();

        // Clear the center message
        gi.centerprintf(edict, " ");
        m_bShouldRespawn = false;
    } else {
        if (g_lastsave->string && *g_lastsave->string) {
            gi.SendConsoleCommand("loadlastgame\n");
        } else {
            gi.SendConsoleCommand("restart\n");
        }

        logfile_started = qfalse;
    }

    //
    // Added in OPM
    //
    Unregister(STRING_RESPAWN);
}

void Player::SetDeltaAngles(void)
{
    int i;

    // Use v_angle since we may be in a camera
    for (i = 0; i < 3; i++) {
        client->ps.delta_angles[i] = ANGLE2SHORT(v_angle[i]);
    }
}

void Player::Obituary(Entity *attacker, Entity *inflictor, int meansofdeath, int iLocation)
{
    str      s1;
    str      s2;
    qboolean bDispLocation;

    if (g_gametype->integer == GT_SINGLE_PLAYER) {
        return;
    }

    s1            = "x";
    s2            = "x";
    bDispLocation = qfalse;

    if (attacker == this) {
        //
        // Player killed themselves
        //
        switch (meansofdeath) {
        case MOD_SUICIDE:
            s1 = "took himself out of commision";
            break;
        case MOD_LAVA:
            s1 = "was burned to a crisp";
            break;
        case MOD_SLIME:
            s1 = "was melted to nothing";
            break;
        case MOD_FALLING:
            s1 = "cratered";
            break;
        case MOD_EXPLOSION:
            s1 = "blew himself up";
            break;
        case MOD_GRENADE:
            if (G_Random() >= 0.5f) {
                s1 = "played catch with himself";
            } else {
                s1 = "tripped on his own grenade";
            }
            break;
        case MOD_ROCKET:
            s1 = "rocketed himself";
            break;
        case MOD_BULLET:
            if (iLocation > -1) {
                s1 = "shot himself";
            } else {
                s1            = "shot himself in the";
                bDispLocation = qtrue;
            }
            break;
        case MOD_FAST_BULLET:
            if (iLocation == HITLOC_GENERAL || iLocation == HITLOC_MISS) {
                s1 = "shot himself";
            } else {
                s1            = "shot himself in the";
                bDispLocation = qtrue;
            }
            break;
        case MOD_LANDMINE:
            s1 = "was hoist on his own pitard";
            break;
        default:
            s1 = "died";
            break;
        }

        if (bDispLocation && g_obituarylocation->integer) {
            str szConv2 = s2 + " in the " + G_LocationNumToDispString(iLocation);

            G_PrintfClient(edict, "%s\n", szConv2.c_str());

            G_PrintDeathMessage(s1, szConv2.c_str(), "x", client->pers.netname, this, "s");
        } else {
            G_PrintfClient(edict, "%s\n", s1.c_str());

            G_PrintDeathMessage(s1.c_str(), s2.c_str(), "x", client->pers.netname, this, "s");
        }
    } else if (attacker && attacker->client) {
        //
        // Killed by another player
        //
        Weapon *pAttackerWeap = NULL;

        if (attacker->IsSubclassOfPlayer()) {
            pAttackerWeap = ((Player *)attacker)->GetActiveWeapon(WEAPON_MAIN);
        }

        switch (meansofdeath) {
        case MOD_CRUSH:
        case MOD_CRUSH_EVERY_FRAME:
            s1 = "was crushed by";
            break;
        case MOD_TELEFRAG:
            s1 = "was telefragged by";
            break;
        case MOD_LAVA:
        case MOD_FIRE:
        case MOD_ON_FIRE:
            s1 = "was burned up by";
            break;
        case MOD_SLIME:
            s1 = "was melted by";
            break;
        case MOD_FALLING:
            s1 = "was pushed over the edge by";
            break;
        case MOD_EXPLOSION:
            s1 = "was blown away by";
            break;
        case MOD_GRENADE:
            if (G_Random() >= 0.5f) {
                s1 = "tripped on";
                s2 = "'s grenade";
            } else {
                s1 = "is picking";
                s2 = "'s shrapnel out of his teeth";
            }
            break;
        case MOD_ROCKET:
            s1 = "took";
            if (G_Random() >= 0.5f) {
                s2 = "'s rocket right in the kisser";
            } else {
                s2 = "'s rocket in the face";
            }
            break;
        case MOD_IMPACT:
            s1 = "was knocked out by";
            break;
        case MOD_BULLET:
        case MOD_FAST_BULLET:
            s1 = "was shot by";

            if (pAttackerWeap) {
                if (pAttackerWeap->GetWeaponClass() & WEAPON_CLASS_PISTOL) {
                    s1 = "was gunned down by";
                } else if (pAttackerWeap->GetWeaponClass() & WEAPON_CLASS_RIFLE) {
                    if (pAttackerWeap->GetZoom()) {
                        s1 = "was sniped by";
                    } else {
                        s1 = "was rifled by";
                    }
                } else if (pAttackerWeap->GetWeaponClass() & WEAPON_CLASS_SMG) {
                    s1 = "was perforated by";
                    s2 = "'s' SMG";
                } else if (pAttackerWeap->GetWeaponClass() & WEAPON_CLASS_MG) {
                    s1 = "was machine-gunned by";
                }
            }

            if (iLocation > -1) {
                bDispLocation = qtrue;
            }
            break;
        case MOD_VEHICLE:
            s1 = "was run over by";
            break;
        case MOD_IMPALE:
            s1 = "was impaled by";
            break;
        case MOD_BASH:
            if (G_Random() >= 0.5f) {
                s1 = "was bashed by";
            } else {
                s1 = "was clubbed by";
            }
            break;
        case MOD_SHOTGUN:
            if (G_Random() >= 0.5f) {
                s1 = "was hunted down by";
            } else {
                s1 = "was pumped full of buckshot by";
            }
            break;
        case MOD_LANDMINE:
            s1 = "stepped on";
            s2 = "'s landmine";
            break;
        default:
            s1 = "was killed by";
            break;
        }

        if (bDispLocation && g_obituarylocation->integer) {
            str szConv2 = s2 + " in the " + G_LocationNumToDispString(iLocation);

            G_PrintDeathMessage(
                s1.c_str(), szConv2.c_str(), attacker->client->pers.netname, client->pers.netname, this, "p"
            );

            if (dedicated->integer) {
                str szLoc1, szLoc2;

                szLoc1 = gi.LV_ConvertString(s1.c_str());
                if (s2 == 'x') {
                    G_PrintfClient(edict, "%s %s\n", szLoc1.c_str(), attacker->client->pers.netname);
                } else {
                    szLoc2 = gi.LV_ConvertString(szConv2.c_str());
                    G_PrintfClient(edict, "%s %s%s\n", szLoc1.c_str(), attacker->client->pers.netname, szLoc2.c_str());
                }
            }
        } else {
            G_PrintDeathMessage(
                s1.c_str(), s2.c_str(), attacker->client->pers.netname, client->pers.netname, this, "p"
            );

            if (dedicated->integer) {
                str szLoc1, szLoc2;

                szLoc1 = gi.LV_ConvertString(s1.c_str());
                if (s2 == 'x') {
                    G_PrintfClient(edict, "%s %s\n", szLoc1.c_str(), attacker->client->pers.netname);
                } else {
                    szLoc2 = gi.LV_ConvertString(s2.c_str());
                    G_PrintfClient(edict, "%s %s%s\n", szLoc1.c_str(), attacker->client->pers.netname, szLoc2.c_str());
                }
            }
        }
    } else {
        //
        // No attacker and not self
        //
        switch (meansofdeath) {
        case MOD_LAVA:
            s1 = "was burned to a crisp";
            break;
        case MOD_SLIME:
            s1 = "was melted to nothing";
            break;
        case MOD_FALLING:
            s1 = "cratered";
            break;
        case MOD_EXPLOSION:
            s1 = "blew up";
            break;
        case MOD_GRENADE:
            s1 = "caught some shrapnel";
            break;
        case MOD_ROCKET:
            s1 = "caught a rocket";
            break;
        case MOD_BULLET:
        case MOD_FAST_BULLET:
            if (iLocation == HITLOC_GENERAL || iLocation == HITLOC_MISS) {
                s1 = "was shot";
            } else {
                s1            = "was shot in the";
                bDispLocation = qtrue;
            }
            break;
        case MOD_LANDMINE:
            s1 = "stepped on a land mine";
            break;
        default:
            s1 = "died";
            break;
        }

        if (bDispLocation && g_obituarylocation->integer) {
            str szConv2 = s2 + " in the " + G_LocationNumToDispString(iLocation);

            G_PrintDeathMessage(s1.c_str(), szConv2.c_str(), "x", client->pers.netname, this, "w");
        } else {
            G_PrintDeathMessage(s1.c_str(), s2.c_str(), "x", client->pers.netname, this, "w");
        }

        G_PrintfClient(edict, "%s\n", gi.LV_ConvertString(s1.c_str()));
    }
}

void Player::Dead(Event *ev)
{
    if (deadflag == DEAD_DEAD) {
        return;
    }

    health   = 0;
    deadflag = DEAD_DEAD;

    edict->s.renderfx &= ~RF_SHADOW;
    server_new_buttons = 0;

    CancelEventsOfType(EV_Player_Dead);

    // stop animating
    StopPartAnimating(legs);

    // pause the torso anim
    PausePartAnim(torso);

    partAnim[torso] = "";

    if (m_fPainBlend != 0) {
        // Clear pain blend
        StopAnimating(ANIMSLOT_PAIN);
        edict->s.frameInfo[ANIMSLOT_PAIN].weight = 0;
        m_fPainBlend                             = 0;
        animdone_Pain                            = false;
    }

    if (g_gametype->integer != GT_SINGLE_PLAYER) {
        if (dmManager.AllowRespawn()) {
            respawn_time = level.time + 1.0f;
        } else {
            respawn_time = level.time + 2.0f;
        }
    } else if (level.current_map && *level.current_map) {
        G_BeginIntermission(level.current_map, TRANS_LEVEL);
    } else {
        respawn_time = level.time + 1.f;
    }

    ZoomOff();

    if (ShouldForceSpectatorOnDeath()) {
        m_bDeathSpectator = true;

        Spectator();
        SetPlayerSpectateRandom();
    }
}

void Player::Killed(Event *ev)
{
    Entity *attacker;
    Entity *inflictor;
    int     meansofdeath;
    int     location;
    Event  *event;

    // [review F3+F4, bug-2124] died-supine latch, HERE and nowhere else. The first version
    // latched in TickCoopProne's forced-leave: (a) nothing ever cleared it while alive, so ONE
    // supine death made every later death that map play the on-back anim; (b) TickCoopProne is
    // usercmd-driven while the KILLED statemap dispatch is server-frame-driven, so on a dedicated
    // server a remote victim's latch could lose the race and a real supine death played belly-down
    // (the standing dedicated/listen parity rule). Killed() runs on EVERY death on the server
    // frame itself, with the stance flags still live - assigning unconditionally makes the value
    // correct per-death and self-overwriting: no stale state can survive to the next death.
    // [vet, bug-2139] recoil debt must not outlive the life that earned it. The constructor runs once
    // per CONNECTION, not per life, so without this a player who died mid-burst respawned still owing
    // recoil and the recovery immediately rotated their fresh view.
    m_vCoopRecoilOwed     = Vector(0, 0, 0);
    m_fCoopRecoilRecenter = 0.0f;
    m_fCoopRecoilLast     = level.time;

    m_bCoopDiedSupine = (m_bCoopProne && m_bCoopSupine);

    //
    // Added in OPM
    // This one is openmohaa-specific
    //  Custom killed event will do the job
    //
    if (m_killedLabel.IsSet()) {
        event = new Event(0, ev->NumArgs());
        for (int i = 1; i <= ev->NumArgs(); i++) {
            event->AddValue(ev->GetValue(i));
        }
        m_killedLabel.Execute(this, event);
        delete event;

        Unregister(STRING_DEATH);
        return;
    }

    if (g_gametype->integer != GT_SINGLE_PLAYER) {
        current_team->AddDeaths(this, 1);
    } else {
        AddDeaths(1);
    }

    attacker     = ev->GetEntity(1);
    inflictor    = ev->GetEntity(3);
    meansofdeath = ev->GetInteger(9);
    location     = ev->GetInteger(10);

    if (attacker && inflictor) {
        Obituary(attacker, inflictor, meansofdeath, location);
    }

    RemoveFromVehiclesAndTurrets();

    if (g_gametype->integer != GT_SINGLE_PLAYER && attacker && attacker->IsSubclassOfPlayer()) {
        static_cast<Player *>(attacker)->KilledPlayerInDeathmatch(this, (meansOfDeath_t)meansofdeath);
    }

    deadflag = DEAD_DYING;
    health   = 0;

    event = new Event(EV_Pain, 10);

    event->AddEntity(attacker);
    event->AddFloat(ev->GetFloat(2));
    event->AddEntity(inflictor);
    event->AddVector(ev->GetVector(4));
    event->AddVector(ev->GetVector(5));
    event->AddVector(ev->GetVector(6));
    event->AddInteger(ev->GetInteger(7));
    event->AddInteger(ev->GetInteger(8));
    event->AddInteger(ev->GetInteger(9));
    event->AddInteger(ev->GetInteger(10));

    ProcessEvent(event);

    if (g_gametype->integer != GT_SINGLE_PLAYER) {
        if (HasItem("Binoculars")) {
            takeItem("Binoculars");
        }

        PostEvent(EV_Player_DMDeathDrop, 0.1f);
        edict->s.eFlags |= EF_DEAD;
    }

    edict->clipmask = MASK_DEADSOLID;
    setContents(CONTENTS_CORPSE);
    setSolidType(SOLID_NOT);
    setMoveType(MOVETYPE_TOSS);

    angles.x = 0;
    angles.z = 0;
    setAngles(angles);

    //
    // change music
    //
    ChangeMusic("failure", "normal", true);

    takedamage = DAMAGE_NO;

    // Post a dead event just in case
    PostEvent(EV_Player_Dead, 5.0f);
    ZoomOff();

    if (g_voiceChat->integer) {
        if (m_voiceType == PVT_ALLIED_MANON) {
            //
            // manon_death doesn't exist in 2.0 anymore.
            // The code is left just in case
            //
            Sound("manon_death", CHAN_VOICE, -1.0f, 160, NULL, -1.0f, 1, 0, 1, 1200);
        } else {
            Sound("player_death");
        }
    } else {
        Sound("player_death");
    }

    if (m_fPainBlend) {
        //
        // 2.0: No more pain animation after death
        //
        animdone_Pain = true;
    }

    //
    // Added in OPM
    //  Scripted events
    //
    event = new Event(0, 11);

    event->AddEntity(ev->GetEntity(1));
    event->AddFloat(ev->GetFloat(2));
    event->AddEntity(ev->GetEntity(3));
    event->AddVector(ev->GetVector(4));
    event->AddVector(ev->GetVector(5));
    event->AddVector(ev->GetVector(6));
    event->AddInteger(ev->GetInteger(7));
    event->AddInteger(ev->GetInteger(8));
    event->AddInteger(ev->GetInteger(9));
    event->AddInteger(ev->GetInteger(10));
    event->AddEntity(this);

    scriptDelegate_kill.Trigger(this, *event);
    scriptedEvents[SE_KILL].Trigger(event);

    Unregister(STRING_DEATH);
}

void Player::EventDMDeathDrop(Event *ev)
{
    Weapon   *weapon = GetActiveWeapon(WEAPON_MAIN);
    SpawnArgs args;
    ClassDef *cls;

    if (!m_bDontDropWeapons && weapon && weapon->IsSubclassOfWeapon()) {
        weapon->Drop();
    }

    args.setArg("model", "models/items/dm_50_healthbox.tik");

    cls = args.getClassDef();
    if (cls) {
        Item *item = (Item *)cls->newInstance();
        if (item) {
            if (item->IsSubclassOfItem()) {
                item->setModel("models/items/dm_50_healthbox.tik");

                item->SetOwner(this);
                item->ProcessPendingEvents();
                item->Drop();
            } else {
                // useless and not pickupable, delete it
                delete item;
            }
        }
    }

    FreeInventory();
}

void Player::EventStopwatch(Event *ev)
{
    stopWatchType_t eType = SWT_NORMAL;

    int iDuration = ev->GetInteger(1);
    if (iDuration < 0) {
        ScriptError("duration < 0");
    }

    if (ev->NumArgs() > 1) {
        eType = static_cast<stopWatchType_t>(ev->GetInteger(2));
    } else {
        eType = SWT_NORMAL;
    }

    SetStopwatch(iDuration, eType);
}

void Player::SetStopwatch(int iDuration, stopWatchType_t type)
{
    int  iStartTime;
    char szCmd[256];

    if (g_protocol >= protocol_e::PROTOCOL_MOHTA_MIN) {
        if (type != SWT_NORMAL) {
            iStartTime = (int)(level.svsFloatTime * 1000.f);
        } else {
            iStartTime = 0;
            if (iDuration) {
                iStartTime = ceil(level.svsFloatTime * 1000.f);
            }
        }

        Com_sprintf(szCmd, sizeof(szCmd), "stopwatch %i %i %i", iStartTime, iDuration, type);
    } else {
        iStartTime = 0;
        if (iDuration) {
            iStartTime = (int)level.svsFloatTime;
        }

        Com_sprintf(szCmd, sizeof(szCmd), "stopwatch %i %i", iStartTime, iDuration);
    }

    gi.SendServerCommand(edict - g_entities, szCmd);
}

void Player::KilledPlayerInDeathmatch(Player *killed, meansOfDeath_t meansofdeath)
{
    DM_Team *pDMTeam;

    pDMTeam = killed->GetDM_Team();

    if (meansofdeath == MOD_TELEFRAG) {
        //
        // Added in OPM
        //  Telefrag isn't the fault of anyone
        //  so don't count any kill
        //
        return;
    }

    if (killed == this) {
        pDMTeam->AddKills(this, -1);
        gi.SendServerCommand(
            edict - g_entities, "print \"" HUD_MESSAGE_WHITE "%s\n\"", gi.LV_ConvertString("You killed yourself")
        );

        return;
    }

    if (pDMTeam == GetDM_Team() && g_gametype->integer >= GT_TEAM) {
        //
        // A teammate was killed
        //
        current_team->AddKills(this, -1);
        num_team_kills++;
    } else {
        current_team->AddKills(this, 1);
    }

    gi.SendServerCommand(
        edict - g_entities,
        "print \"" HUD_MESSAGE_WHITE "%s %s\n\"",
        gi.LV_ConvertString("You killed"),
        killed->client->pers.netname
    );
}

void Player::Pain(Event *ev)
{
    float   damage, yawdiff;
    Entity *attacker;
    int     meansofdeath;
    Vector  dir, pos, attack_angle;
    int     iLocation;

    attacker     = ev->GetEntity(1);
    damage       = ev->GetFloat(2);
    pos          = ev->GetVector(4);
    dir          = ev->GetVector(5);
    meansofdeath = ev->GetInteger(9);
    iLocation    = ev->GetInteger(10);

    if (!damage && !knockdown) {
        return;
    }

    client->ps.stats[STAT_LAST_PAIN] = damage;

    // Determine direction
    attack_angle = dir.toAngles();
    yawdiff      = angles[YAW] - attack_angle[YAW] + 180;
    yawdiff      = AngleNormalize180(yawdiff);

    if (yawdiff > -45 && yawdiff < 45) {
        pain_dir = PAIN_FRONT;
    } else if (yawdiff < -45 && yawdiff > -135) {
        pain_dir = PAIN_LEFT;
    } else if (yawdiff > 45 && yawdiff < 135) {
        pain_dir = PAIN_RIGHT;
    } else {
        pain_dir = PAIN_REAR;
    }

    pain_type     = (meansOfDeath_t)meansofdeath;
    pain_location = iLocation;

    // Only set the regular pain level if enough time since last pain has passed
    if (((level.time > nextpaintime) && take_pain) || IsDead()) {
        pain = damage;
    }

    // add to the damage inflicted on a player this frame
    // the total will be turned into screen blends and view angle kicks
    // at the end of the frame
    damage_blood += damage;
    damage_from += dir * damage;
    damage_yaw = dir.toYaw() * 10.0f;

    if (damage_yaw == client->ps.stats[STAT_DAMAGEDIR]) {
        if (damage_yaw < 1800.0f) {
            damage_yaw += 1.0f;
        } else {
            damage_yaw -= 1.0f;
        }
    }

    if (g_gametype->integer != GT_SINGLE_PLAYER && attacker && attacker->client && attacker != this) {
        gi.MSG_SetClient(attacker->edict - g_entities);
        if (IsDead()) {
            gi.MSG_StartCGM(BG_MapCGMToProtocol(g_protocol, CGM_NOTIFY_KILL));
        } else {
            gi.MSG_StartCGM(BG_MapCGMToProtocol(g_protocol, CGM_NOTIFY_HIT));
        }
        gi.MSG_EndCGM();
    }

    if (IsDead()) {
        return;
    }

    /*
    HZM coop [user 2026-08-21] SEVERITY-TIERED PAIN.

    This function ended in an UNCONDITIONAL Sound("player_pain"), and the engine resolves that by
    PREFIX (Alias_ListFindRandomRange), drawing UNIFORMLY across the pool. So a 5-damage graze and a
    near-fatal hit produced the identical sound. The 24 real takes make it worse rather than better:
    one actor, all peak-normalised to -0.0 dBFS, so they are the same LOUDNESS as well as the same
    intensity. Nothing in the audio ever told the player how badly they had just been hit.

    The earlier plan was to sort the takes into tiers, which was wrong twice over: they are
    IMA-ADPCM and of roughly equal intensity so any sort is arbitrary, and splitting 24 takes three
    ways would cut each tier's variety to a third. You do not need to SORT them - you need to PLAY
    them differently. ubersound/coop_paintiers.scr (generated) therefore carries ALL 24 takes in
    each of three tiers at different volume/pitch/rolloff: quieter and higher for a graze, louder,
    lower and carrying further for a grave hit.

    NAMING. The tier prefixes deliberately do NOT start with "player_pain". Sound() matches by
    prefix, so a name like player_painlt01 would ALSO be drawn by Sound("player_pain") and would
    quadruple the base pool for any other caller. No bare "coop_hurt" alias exists either, so
    nothing can draw ACROSS tiers.

    Thresholds are a PERCENT of max_health, not absolute damage, so they follow coop_health instead
    of silently re-tiering when the server changes it. `damage` is already in scope here
    (ev->GetFloat(2) at the top of this function) - no new plumbing.

    coop_painTiers 0 restores the retail single-pool behaviour exactly.
    */
    const char *pszPain = "player_pain";
    {
        static cvar_t *pTierOn = NULL, *pTierLo = NULL, *pTierHi = NULL;

        if (!pTierOn) { pTierOn = gi.Cvar_Get("coop_painTiers",  "1",  CVAR_ARCHIVE); }
        if (!pTierLo) { pTierLo = gi.Cvar_Get("coop_painTierLo", "5",  CVAR_ARCHIVE); }
        if (!pTierHi) { pTierHi = gi.Cvar_Get("coop_painTierHi", "15", CVAR_ARCHIVE); }

        if (pTierOn->integer > 0) {
            float fMax = (max_health > 0.0f) ? max_health : 100.0f;
            float fPct = (damage / fMax) * 100.0f;

            if (fPct >= pTierHi->value) {
                pszPain = "coop_hurthv";
            } else if (fPct >= pTierLo->value) {
                pszPain = "coop_hurtmd";
            } else {
                pszPain = "coop_hurtlt";
            }
        }
    }

    if (g_voiceChat->integer) {
        if (m_voiceType == PVT_ALLIED_MANON) {
            //
            // Should have been removed since 2.0
            //
            Sound("manon_pain", CHAN_DIALOG, -1, 160, NULL, -1, 1, 0, 1, 1200);
        } else {
            Sound(pszPain);
        }
    } else {
        Sound(pszPain);
    }
}

void Player::DoUse(Event *ev)
{
    gentity_t *hit;
    int        touch[MAX_GENTITIES];
    int        num;
    int        i;
    bool       bWasInTurretOrVehicle;

    if (g_gametype->integer != GT_SINGLE_PLAYER && IsSpectator()) {
        // Prevent using stuff while spectating
        return;
    }

    if (IsDead()) {
        // Dead players mustn't use
        return;
    }

    if (edict->r.svFlags & SVF_NOCLIENT) {
        // Fixed in OPM
        //  Clients that are not sent to other clients cannot use objects.
        //  Some mods make players non-solid, hide them and turn physics off
        //  as a way to spectate other players or for cinematics.
        //  This prevent players to use objects such as doors
        return;
    }

    bWasInTurretOrVehicle = m_pVehicle || m_pTurret;

    if (bWasInTurretOrVehicle) {
        RemoveFromVehiclesAndTurretsInternal();
        return;
    }

    if (g_protocol >= protocol_e::PROTOCOL_MOHTA_MIN) {
        if ((buttons & BUTTON_ATTACKLEFT) || (buttons & BUTTON_ATTACKRIGHT)) {
            //
            // Added in 2.0
            //  Only allow use if the player isn't holding attack buttons
            //
            return;
        }
    }

    num = getUseableEntities(touch, MAX_GENTITIES, true);

    if (g_protocol >= protocol_e::PROTOCOL_MOHTA_MIN) {
        // Fixed in 2.0
        //  Since 2.0, the loop stops when the player
        //  uses a turret, this prevents the turret from being deleted
        //  after being attached to the player
        //
        for (i = 0; i < num; i++) {
            hit = &g_entities[touch[i]];

            if (!hit->inuse) {
                continue;
            }

            Event *event = new Event(EV_Use);
            event->AddListener(this);

            hit->entity->ProcessEvent(event);

            if (m_pVehicle || m_pTurret) {
                break;
            }
        }
    } else {
        //
        // Backward compatibility
        // It still allows 1.11 SP to work properly
        // Such as in m1l1 when the player must man the mounted machine gun
        for (i = 0; i < num; i++) {
            hit = &g_entities[touch[i]];

            if (!hit->inuse) {
                continue;
            }

            Event *event = new Event(EV_Use);
            event->AddListener(this);

            hit->entity->ProcessEvent(event);
        }
    }

    if (!bWasInTurretOrVehicle && m_pVehicle) {
        //
        // Added in 2.30
        //  Make the vehicle also invincible if the player is invincible
        //
        if (flags & FL_GODMODE) {
            m_pVehicle->flags |= FL_GODMODE;
        } else {
            m_pVehicle->flags &= ~FL_GODMODE;
        }
    }
}

void Player::TouchStuff(pmove_t *pm)
{
    gentity_t *other;
    Event     *event;
    int        i;
    int        j;

    //
    // clear out any conditionals that are controlled by touching
    //
    toucheduseanim = NULL;

    if (getMoveType() != MOVETYPE_NOCLIP) {
        G_TouchTriggers(this);
    }

    // touch other objects
    for (i = 0; i < pm->numtouch; i++) {
        other = &g_entities[pm->touchents[i]];

        for (j = 0; j < i; j++) {
            gentity_t *ge = &g_entities[j];

            if (ge == other) {
                break;
            }
        }

        if (j != i) {
            // duplicated
            continue;
        }

        // Don't bother touching the world
        if ((!other->entity) || (other->entity == world)) {
            continue;
        }

        event = new Event(EV_Touch);
        event->AddEntity(this);
        other->entity->ProcessEvent(event);

        event = new Event(EV_Touch);
        event->AddEntity(other->entity);
        ProcessEvent(event);
    }
}

void Player::GetMoveInfo(pmove_t *pm)
{
    moveresult = pm->moveresult;

    if (!deadflag || (g_gametype->integer != GT_SINGLE_PLAYER && IsSpectator())) {
        v_angle[0] = pm->ps->viewangles[0];
        v_angle[1] = pm->ps->viewangles[1];
        v_angle[2] = pm->ps->viewangles[2];

        if (moveresult == MOVERESULT_TURNED) {
            angles.y = v_angle[1];
            setAngles(angles);
            SetViewAngles(angles);
        }
    }

    setOrigin(Vector(pm->ps->origin[0], pm->ps->origin[1], pm->ps->origin[2]));

    if (pm->ps->groundEntityNum != ENTITYNUM_NONE) {
        float backoff;
        float change;
        int   i;

        backoff = DotProduct(pm->ps->groundTrace.plane.normal, pm->ps->velocity);

        for (i = 0; i < 3; i++) {
            change = pm->ps->groundTrace.plane.normal[i] * backoff;
            pm->ps->velocity[i] -= change;
        }
    }

    // Set the ground entity
    groundentity = NULL;
    if (pm->ps->groundEntityNum != ENTITYNUM_NONE) {
        groundentity = &g_entities[pm->ps->groundEntityNum];
        airspeed     = 200;

        if (!groundentity->entity || groundentity->entity->getMoveType() == MOVETYPE_NONE) {
            m_vPushVelocity = vec_zero;
        }

        //
        // Fixed in OPM
        //  Disable predictions when the groundentity is moving up/down, looks like shaky otherwise
        if (groundentity->entity && groundentity->entity != this && groundentity->entity->velocity[2] != 0) {
            pm->ps->pm_flags |= PMF_NO_PREDICTION;
        }
    } else if (m_pGlueMaster) {
        // Added in OPM
        //  Use the glue master for the ground entity to make the viewmodel will stay still
        pm->ps->groundEntityNum = m_pGlueMaster->entnum;
    }

    velocity = Vector(pm->ps->velocity[0], pm->ps->velocity[1], pm->ps->velocity[2]);

    if ((client->ps.pm_flags & PMF_FROZEN) || (client->ps.pm_flags & PMF_NO_MOVE)) {
        velocity = vec_zero;
    } else {
        setSize(pm->mins, pm->maxs);
        viewheight = pm->ps->viewheight;
    }

    // water type and level is set in the predicted code
    waterlevel = pm->waterlevel;
    watertype  = pm->watertype;
}

void Player::SetMoveInfo(pmove_t *pm, usercmd_t *ucmd)
{
    Vector move;

    // set up for pmove
    memset(pm, 0, sizeof(pmove_t));

    velocity.copyTo(client->ps.velocity);

    pm->ps = &client->ps;

    if (ucmd) {
        pm->cmd = *ucmd;
    }

    if (sv_drawtrace->integer <= 1) {
        pm->trace = gi.trace;
    } else {
        pm->trace = &G_PMDrawTrace;
    }

    pm->tracemask     = MASK_PLAYERSOLID;
    // HZM 07-20 (user approved): coop_noPlayerClip 1 = PLAYER movement ignores CONTENTS_PLAYERCLIP,
    // the invisible designer fences painted over rocks/ledges to keep SP players on the intended
    // path (e.g. the e1l2 gun-emplacement rocks). Real geometry (CONTENTS_SOLID) is untouched -
    // players can only stand where actual surfaces exist, and floors/walls behave normally. AI
    // keep their own masks (pathing unaffected). Trade-off: retail fences occasionally guard
    // unfinished map edges - set 0 to restore stock fencing.
    {
        static cvar_t *coop_noplayerclip = NULL;
        if (!coop_noplayerclip) {
            coop_noplayerclip = gi.Cvar_Get("coop_noPlayerClip", "0", 0);
        }
        if (coop_noplayerclip->integer) {
            pm->tracemask &= ~CONTENTS_PLAYERCLIP;
        }
    }
    // HZM coop bug-946/949: REGIONAL invisible-wall strip (see CoopClipStripZoneContains,
    // g_utils.cpp). Inside a zone the player ignores PLAYERCLIP and FENCE - both invisible
    // blocker species (common/clip webs + nodraw/bspindleclip fence brushes); collision
    // falls back to the real visible geometry. Zones are validated per-map against the
    // BSP brush data so visible barbed-wire fences are never inside one.
    if (CoopClipStripZoneContains(origin)) {
        pm->tracemask &= ~(CONTENTS_PLAYERCLIP | CONTENTS_FENCE);
    }
    // HZM coop bug-947/952/953: invisible-wall self-reporting, v4.
    // (a) STUCK DETECTOR: player pushing (ucmd move input) but not moving for ~0.5s
    //     -> forensic trace in the push direction logging brush id + SHADER NAME +
    //     surfaceflags + entity, EVEN for species the radial sweep would classify as
    //     ordinary geometry (catches terrain/patch phantoms - the "silent walls").
    // (b) RADIAL SWEEP: 1 Hz, 8 directions, 48u, three heights (shin/waist/head),
    //     species-classified (clip/fence/solidnodraw/ent), exact brush id + shader.
    // BRUSH -1 on a world hit = non-brush collision (patch/terrain) - special species.
    // coop_wallProbe 0 disables. All output ^~^~^ WALLPROBE, qconsole.log parseable.
    {
        static cvar_t *coop_wallprobe = NULL;
        static int     nextProbeMs[MAX_CLIENTS];
        static float   lastOrg[MAX_CLIENTS][2];
        static int     stuckFrames[MAX_CLIENTS];
        static int     nextStuckMs[MAX_CLIENTS];

        if (!coop_wallprobe) {
            coop_wallprobe = gi.Cvar_Get("coop_wallProbe", "0", 0);
        }
        int cn = edict->s.number;
        if (coop_wallprobe->integer && cn >= 0 && cn < MAX_CLIENTS && !IsDead() && !IsSpectator()) {
            int baseMask = MASK_PLAYERSOLID & ~CONTENTS_BODY;
            if (CoopClipStripZoneContains(origin)) {
                baseMask &= ~(CONTENTS_PLAYERCLIP | CONTENTS_FENCE);
            }

            // ---- (a) stuck detector ----
            {
                float dx = origin.x - lastOrg[cn][0];
                float dy = origin.y - lastOrg[cn][1];
                if ((ucmd->forwardmove || ucmd->rightmove) && (dx * dx + dy * dy) < 0.25f) {
                    stuckFrames[cn]++;
                } else {
                    stuckFrames[cn] = 0;
                }
                lastOrg[cn][0] = origin.x;
                lastOrg[cn][1] = origin.y;
                if (stuckFrames[cn] >= 10 && level.inttime >= nextStuckMs[cn]) {
                    nextStuckMs[cn] = level.inttime + 1000;
                    stuckFrames[cn] = 0;
                    Vector fwd, right, wish;
                    AngleVectors(v_angle, fwd, right, NULL);
                    wish   = fwd * (float)ucmd->forwardmove + right * (float)ucmd->rightmove;
                    wish.z = 0;
                    if (wish.length() > 0.1f) {
                        wish.normalize();
                        // bug-959: probe BOTH shin and waist - step-edge lips live at shin height
                        // and a waist-only trace sails over the very thing pinning the player.
                        // Whichever height hits NEAREST is reported as the true BLOCKER.
                        Vector  eyeW = origin + Vector(0, 0, 40);
                        Vector  eyeS = origin + Vector(0, 0, 12);
                        trace_t stW  = G_Trace(eyeW, vec_zero, vec_zero, eyeW + wish * 48, this, baseMask, qfalse, "coop_wallProbe_stuckW");
                        trace_t stS  = G_Trace(eyeS, vec_zero, vec_zero, eyeS + wish * 48, this, baseMask, qfalse, "coop_wallProbe_stuckS");
                        trace_t st   = (stS.fraction < stW.fraction) ? stS : stW;
                        Vector  eye  = (stS.fraction < stW.fraction) ? eyeS : eyeW;
                        Vector  to   = eye + wish * 48;
                        if (st.fraction < 1.0f) {
                            Vector        inside   = Vector(st.endpos) + wish * 2;
                            int           brushNum = gi.PointBrushnum(inside, 0);
                            baseshader_t *bs       = (st.shaderNum >= 0) ? gi.GetShader(st.shaderNum) : NULL;
                            gi.Printf(
                                "^~^~^ WALLPROBE STUCK-BLOCKER BRUSH %d shader '%s' sf 0x%x ent %d at %.0f %.0f %.0f h %.0f push %.0f %.0f\n",
                                brushNum,
                                bs ? bs->shader : "?",
                                st.surfaceFlags,
                                st.entityNum,
                                st.endpos[0],
                                st.endpos[1],
                                st.endpos[2],
                                eye.z - origin.z,
                                wish.x * 10,
                                wish.y * 10
                            );
                        } else {
                            gi.Printf(
                                "^~^~^ WALLPROBE STUCK-NOHIT at %.0f %.0f %.0f push %.0f %.0f\n",
                                origin.x,
                                origin.y,
                                origin.z,
                                wish.x * 10,
                                wish.y * 10
                            );
                        }
                    }
                }
            }

            // ---- (b) radial sweep ----
            if (level.inttime >= nextProbeMs[cn]) {
                nextProbeMs[cn] = level.inttime + 1000;

                // v5: CEILING probe - invisible overhead blockers (jump stoppers)
                {
                    Vector  up0 = origin + Vector(0, 0, 72);
                    Vector  up1 = origin + Vector(0, 0, 168);
                    trace_t tu  = G_Trace(up0, vec_zero, vec_zero, up1, this, baseMask, qfalse, "coop_wallProbe_up");
                    if (tu.fraction < 1.0f && !tu.startsolid) {
                        trace_t tu2 = G_Trace(up0, vec_zero, vec_zero, up1, this,
                                              baseMask & ~(CONTENTS_PLAYERCLIP | CONTENTS_FENCE), qfalse, "coop_wallProbe_up2");
                        qboolean invis = (tu2.fraction > tu.fraction + 0.001f)
                                      || (tu.entityNum == ENTITYNUM_WORLD && (tu.surfaceFlags & SURF_NODRAW));
                        if (invis) {
                            Vector        insideU  = Vector(tu.endpos) + Vector(0, 0, 2);
                            int           bnU      = gi.PointBrushnum(insideU, 0);
                            baseshader_t *bsU      = (tu.shaderNum >= 0) ? gi.GetShader(tu.shaderNum) : NULL;
                            gi.Printf("^~^~^ WALLPROBE CEIL BRUSH %d shader '%s' at %.0f %.0f %.0f\n",
                                      bnU, bsU ? bsU->shader : "?", tu.endpos[0], tu.endpos[1], tu.endpos[2]);
                        }
                    }
                }
                // v5: FLOOR identity - standing on invisible clip (floating-platform feel)
                {
                    Vector  dn0 = origin + Vector(0, 0, 4);
                    Vector  dn1 = origin - Vector(0, 0, 16);
                    trace_t td  = G_Trace(dn0, vec_zero, vec_zero, dn1, this, baseMask, qfalse, "coop_wallProbe_dn");
                    if (td.fraction < 1.0f && td.entityNum == ENTITYNUM_WORLD && td.shaderNum >= 0) {
                        baseshader_t *bsD = gi.GetShader(td.shaderNum);
                        if (bsD && (strstr(bsD->shader, "common/clip") || strstr(bsD->shader, "playerclip")
                                    || strstr(bsD->shader, "nodraw"))) {
                            Vector insideD = Vector(td.endpos) - Vector(0, 0, 2);
                            gi.Printf("^~^~^ WALLPROBE FLOOR BRUSH %d shader '%s' at %.0f %.0f %.0f\n",
                                      gi.PointBrushnum(insideD, 0), bsD->shader, td.endpos[0], td.endpos[1], td.endpos[2]);
                        }
                    }
                }

                static const float probeHeights[3] = {40, 14, 64};
                for (int hi = 0; hi < 3; hi++) {
                    Vector eye = origin + Vector(0, 0, probeHeights[hi]);
                    for (int di = 0; di < 8; di++) {
                        float  ang = di * (360.0f / 8.0f) * (M_PI / 180.0f);
                        Vector dir(cos(ang), sin(ang), 0);
                        Vector to = eye + dir * 48;
                        trace_t trA = G_Trace(eye, vec_zero, vec_zero, to, this, baseMask, qfalse, "coop_wallProbe_A");
                        if (trA.fraction >= 1.0f || trA.startsolid) {
                            continue;
                        }
                        const char *kind = NULL;
                        trace_t     trB =
                            G_Trace(eye, vec_zero, vec_zero, to, this, baseMask & ~CONTENTS_PLAYERCLIP, qfalse, "coop_wallProbe_B");
                        if (trB.fraction >= 1.0f) {
                            kind = "clip";
                        } else {
                            trace_t trC =
                                G_Trace(eye, vec_zero, vec_zero, to, this, baseMask & ~CONTENTS_FENCE, qfalse, "coop_wallProbe_C");
                            if (trC.fraction >= 1.0f) {
                                kind = "fence";
                            } else if (trA.entityNum == ENTITYNUM_WORLD && (trA.surfaceFlags & SURF_NODRAW)) {
                                kind = "solidnodraw";
                            } else if (trA.ent && trA.entityNum != ENTITYNUM_WORLD && trA.ent->entity
                                       && (trA.ent->s.modelindex == 0 || (trA.ent->s.renderfx & RF_DONTDRAW))) {
                                gi.Printf(
                                    "^~^~^ WALLPROBE ent %d %s at %.0f %.0f %.0f h %.0f\n",
                                    trA.entityNum,
                                    trA.ent->entity->getClassname(),
                                    trA.endpos[0],
                                    trA.endpos[1],
                                    trA.endpos[2],
                                    probeHeights[hi]
                                );
                                continue;
                            }
                        }
                        if (kind) {
                            Vector        inside   = Vector(trA.endpos) + dir * 2;
                            int           brushNum = gi.PointBrushnum(inside, 0);
                            baseshader_t *bs       = (trA.shaderNum >= 0) ? gi.GetShader(trA.shaderNum) : NULL;
                            if (coop_wallprobe->integer >= 2) {
                                gi.SendServerCommand(edict - g_entities,
                                    va("print \"[wallprobe] %s brush %d logged\n\"", kind, brushNum));
                            }
                            gi.Printf(
                                "^~^~^ WALLPROBE %s BRUSH %d shader '%s' at %.0f %.0f %.0f dir %.0f %.0f h %.0f\n",
                                kind,
                                brushNum,
                                bs ? bs->shader : "?",
                                trA.endpos[0],
                                trA.endpos[1],
                                trA.endpos[2],
                                dir.x * 10,
                                dir.y * 10,
                                probeHeights[hi]
                            );
                        }
                    }
                }
            }
        }
    }
    pm->pointcontents = gi.pointcontents;

    pm->ps->origin[0] = origin.x;
    pm->ps->origin[1] = origin.y;
    pm->ps->origin[2] = origin.z;

    /*
    pm->mins[0] = mins.x;
    pm->mins[1] = mins.y;
    pm->mins[2] = mins.z;

    pm->maxs[0] = maxs.x;
    pm->maxs[1] = maxs.y;
    pm->maxs[2] = maxs.z;
    */

    pm->ps->velocity[0] = velocity.x;
    pm->ps->velocity[1] = velocity.y;
    pm->ps->velocity[2] = velocity.z;

    pm->pmove_fixed = pmove_fixed->integer;
    pm->pmove_msec  = pmove_msec->integer;

    if (pmove_msec->integer < 8) {
        pm->pmove_msec = 8;
    } else if (pmove_msec->integer > 33) {
        pm->pmove_msec = 33;
    }

    if (g_protocol >= protocol_e::PROTOCOL_MOHTA_MIN) {
        if (g_gametype->integer != GT_SINGLE_PLAYER) {
            //
            // Added in 2.0
            // In multiplayer mode, specify if the player can lean while moving
            //
            if (dmflags->integer & DF_ALLOW_LEAN_MOVEMENT) {
                pm->alwaysAllowLean = qtrue;
            } else {
                pm->alwaysAllowLean = qfalse;
            }
        } else {
            pm->alwaysAllowLean = qfalse;
        }

        pm->leanMax          = 45.f;
        pm->leanAdd          = 6.f;
        pm->leanRecoverSpeed = 8.5f;
        pm->leanSpeed        = 2.f;

        // HZM coop [user 2026-08-22] WALL-COVER LEAN (Phase 2), server half. MUST stay paired
        // with the identical block in cg_predict.c - if the two disagree the predictor and the
        // server produce different fLeanAngle and the view judders. Ships behind
        // coop_coverLean, DEFAULT 0: the plan gates Phase 2 on a Phase 1 playtest that has not
        // happened yet, and this touches SHARED pmove, so a mistake here would affect every
        // player rather than only those using cover.
        {
            static cvar_t *pLean = NULL, *pLeanMax = NULL;

            if (!pLean)    { pLean    = // [user 2026-08-23] SHIPS ON. "I am good with coop lean cover on." It defaulted to 0, so the
        // lean out of wall cover reached nobody but the one machine that had it archived at 1 -
        // which meant even a working side solver would have produced no lean for any player.
        // Found by the config-fossil sweep (docs/tools/config_fossils.py), not by testing.
        gi.Cvar_Get("coop_coverLean",    "1",  CVAR_ARCHIVE); }
            if (!pLeanMax) { pLeanMax = gi.Cvar_Get("coop_coverLeanMax", "28", CVAR_ARCHIVE); }

            pm->coopCoverLeanSide = 0;
            pm->coopCoverLeanMax  = 0.0f;
            if (pLean->integer > 0 && m_bCoopCoverWall && m_bCoopCoverPeek && m_iCoopCoverSide != 0) {
                pm->coopCoverLeanSide = m_iCoopCoverSide;
                // scale the lean by the edge we measured - a shallow jamb leans less, so the
                // silhouette never swings past cover that is not there
                pm->coopCoverLeanMax = pLeanMax->value;
                if (m_fCoopCoverEdge > 0.0f && m_fCoopCoverEdge < 48.0f) {
                    pm->coopCoverLeanMax = pLeanMax->value * (m_fCoopCoverEdge / 48.0f);
                }
            }
        }
    } else {
        pm->alwaysAllowLean = qtrue;
        if (g_gametype->integer != GT_SINGLE_PLAYER) {
            pm->leanMax = 40.f;
        } else {
            // Don't allow lean in single-player, like in the original game
            pm->leanMax = 0;
        }

        pm->leanAdd          = 10.f;
        pm->leanRecoverSpeed = 15.f;
        pm->leanSpeed        = 4.f;
    }

    pm->protocol = g_protocol;

    // Added in OPM
    //  Initialize the ground entity
    pm->ps->groundEntityNum = ENTITYNUM_NONE;
}

pmtype_t Player::GetMovePlayerMoveType(void)
{
    if (getMoveType() == MOVETYPE_NOCLIP || IsSpectator()) {
        return PM_NOCLIP;
    } else if (deadflag) {
        return PM_DEAD;
    } else if (movecontrol == MOVECONTROL_CLIMBWALL) {
        return PM_CLIMBWALL;
    } else {
        return PM_NORMAL;
    }
}

void Player::CheckGround(void)
{
    pmove_t pm;

    SetMoveInfo(&pm, current_ucmd);
    Pmove_GroundTrace(&pm);
    GetMoveInfo(&pm);
}

qboolean Player::AnimMove(Vector& move, Vector *endpos)
{
    Vector  up;
    Vector  down;
    trace_t trace;
    int     mask;
    Vector  start(origin);
    Vector  end(origin + move);

    mask = MASK_PLAYERSOLID;

    // test the player position if they were a stepheight higher
    trace = G_Trace(start, mins, maxs, end, this, mask, true, "AnimMove");
    if (trace.fraction < 1) {
        if ((movecontrol == MOVECONTROL_HANGING) || (movecontrol == MOVECONTROL_CLIMBWALL)) {
            up = origin;
            up.z += move.z;
            trace = G_Trace(origin, mins, maxs, up, this, mask, true, "AnimMove");
            if (trace.fraction < 1) {
                if (endpos) {
                    *endpos = origin;
                }
                return qfalse;
            }

            origin = trace.endpos;
            end    = origin;
            end.x += move.x;
            end.y += move.y;

            trace = G_Trace(origin, mins, maxs, end, this, mask, true, "AnimMove");
            if (endpos) {
                *endpos = trace.endpos;
            }

            return (trace.fraction > 0);
        } else {
            return TestMove(move, endpos);
        }
    } else {
        if (endpos) {
            *endpos = trace.endpos;
        }

        return qtrue;
    }
}

qboolean Player::TestMove(Vector& move, Vector *endpos)
{
    trace_t trace;
    Vector  pos(origin + move);

    trace = G_Trace(origin, mins, maxs, pos, this, MASK_PLAYERSOLID, true, "TestMove");
    if (trace.allsolid) {
        // player is completely trapped in another solid
        if (endpos) {
            *endpos = origin;
        }
        return qfalse;
    }

    if (trace.fraction < 1.0f) {
        Vector up(origin);
        up.z += STEPSIZE;

        trace = G_Trace(origin, mins, maxs, up, this, MASK_PLAYERSOLID, true, "TestMove");
        if (trace.fraction == 0.0f) {
            if (endpos) {
                *endpos = origin;
            }
            return qfalse;
        }

        Vector temp(trace.endpos);
        Vector end(temp + move);

        trace = G_Trace(temp, mins, maxs, end, this, MASK_PLAYERSOLID, true, "TestMove");
        if (trace.fraction == 0.0f) {
            if (endpos) {
                *endpos = origin;
            }
            return qfalse;
        }

        temp = trace.endpos;

        Vector down(trace.endpos);
        down.z = origin.z;

        trace = G_Trace(temp, mins, maxs, down, this, MASK_PLAYERSOLID, true, "TestMove");
    }

    if (endpos) {
        *endpos = trace.endpos;
    }

    return qtrue;
}

float Player::TestMoveDist(Vector& move)
{
    Vector endpos;

    TestMove(move, &endpos);
    endpos -= origin;

    return endpos.length();
}

static Vector vec_up = Vector(0, 0, 1);

void Player::CheckMoveFlags(void)
{
    trace_t trace;
    Vector  start;
    Vector  end;
    float   oldsp;
    Vector  olddir(oldvelocity.x, oldvelocity.y, 0);

    //
    // Check if moving forward will cause the player to fall
    //
    start = origin + yaw_forward * 52.0f;
    end   = start;
    end.z -= STEPSIZE * 2;

    trace   = G_Trace(start, mins, maxs, end, this, MASK_PLAYERSOLID, true, "CheckMoveFlags");
    canfall = (trace.fraction >= 1.0f);

    if (!groundentity && !(client->ps.walking)) {
        falling      = true;
        hardimpact   = false;
        mediumimpact = false;
    } else {
        falling      = false;
        mediumimpact = oldvelocity.z <= -180.0f;
        hardimpact   = oldvelocity.z < -400.0f;
    }

    // check for running into walls
    oldsp = VectorNormalize(olddir);
    if ((oldsp > 220.0f) && (velocity * olddir < 2.0f)) {
        moveresult = MOVERESULT_HITWALL;
    }

    move_forward_vel  = DotProduct(yaw_forward, velocity);
    move_backward_vel = -move_forward_vel;

    if (move_forward_vel < 0.0f) {
        move_forward_vel = 0.0f;
    }

    if (move_backward_vel < 0.0f) {
        move_backward_vel = 0.0f;
    }

    move_left_vel  = DotProduct(yaw_left, velocity);
    move_right_vel = -move_left_vel;

    if (move_left_vel < 0.0f) {
        move_left_vel = 0.0f;
    }

    if (move_right_vel < 0.0f) {
        move_right_vel = 0.0f;
    }

    move_up_vel   = DotProduct(vec_up, velocity);
    move_down_vel = -move_up_vel;

    if (move_up_vel < 0.0f) {
        move_up_vel = 0.0f;
    }

    if (move_down_vel < 0.0f) {
        move_down_vel = 0.0f;
    }
}

qboolean Player::CheckMove(Vector& move, Vector *endpos)
{
    return AnimMove(move, endpos);
}

float Player::CheckMoveDist(Vector& move)
{
    Vector endpos;

    CheckMove(move, &endpos);
    endpos -= origin;

    return endpos.length();
}

void Player::ClientMove(usercmd_t *ucmd)
{
    pmove_t pm;
    Vector  move;

#ifdef OPM_FEATURES
    int  touch[MAX_GENTITIES];
    int  num        = getUseableEntities(touch, MAX_GENTITIES, true);
    bool bHintShown = false;

    for (int i = 0; i < num; i++) {
        Entity *entity = g_entities[touch[i]].entity;
        if (entity && entity->m_HintString.length()) {
            entity->ProcessHint(edict, true);
            bHintShown     = true;
            m_bShowingHint = true;
            break;
        }
    }

    if (!bHintShown && m_bShowingHint) {
        m_bShowingHint = false;

        // FIXME: delete
        if (sv_specialgame->integer) {
            gi.MSG_SetClient(edict - g_entities);

            // Send the hint string once
            gi.MSG_StartCGM(CGM_HINTSTRING);
            gi.MSG_WriteString("");
            gi.MSG_EndCGM();
        }
    }
#endif

    oldorigin = origin;

    client->ps.pm_type = GetMovePlayerMoveType();
    // set move flags
    client->ps.pm_flags &=
        ~(PMF_FROZEN | PMF_NO_PREDICTION | PMF_NO_MOVE | PMF_DUCKED | PMF_TURRET | PMF_VIEW_PRONE | PMF_VIEW_DUCK_RUN
          | PMF_VIEW_JUMP_START);

    if (level.playerfrozen || m_bFrozen) {
        client->ps.pm_flags |= PMF_FROZEN;
    }

    if ((flags & FL_IMMOBILE) || (flags & FL_PARTIAL_IMMOBILE)) {
        client->ps.pm_flags |= PMF_NO_MOVE;
        client->ps.pm_flags |= PMF_NO_PREDICTION;
    }

    if (m_pGlueMaster) {
        //
        // Added in 2.0.
        // Disable movement prediction/movement if the player is glued to something
        //
        client->ps.pm_flags |= PMF_NO_PREDICTION;
        client->ps.pm_flags |= PMF_NO_MOVE;

        // HZM coop: a pinned rider can't drive the normal crouch state machine - PMF_NO_MOVE makes pmove
        // return before processing movement, so the crouch/stand toggle input never reaches it (proven: the
        // legs statemap never evaluates CHECK_HEIGHT for a seated rider). For a DUCKABLE-glued seat, drive the
        // height straight from the crouch axis instead: hold crouch = ducked, release = stand. The PMF_DUCKED
        // derivation just below picks up maxs.z, and PM_CheckDuck applies the matching bbox/viewheight.
        if (m_bGlueDuckable) {
            if (last_ucmd.upmove < 0) {
                maxs.z     = 54.0f;
                viewheight = CROUCH_VIEWHEIGHT;
            } else {
                maxs.z     = 94.0f;
                viewheight = DEFAULT_VIEWHEIGHT;
            }
        }
    }

    if (g_protocol >= protocol_e::PROTOCOL_MOHTA_MIN) {
        // HZM coop [user 2026-08-24] the 2.0+ branch never derived PRONE from the hull - prone was
        // REMOVED in 2.0 (PM_CheckDuck says so in as many words) and coop runs that protocol. Without
        // the flag PM_CheckDuck cannot recognise a prone player and resets him to standing height on
        // the next pmove frame, which is why the camera and gun stayed at eye level.
        // AND m_bCoopProne, not the hull alone. A 20-unit hull is not unique to prone - DBNO uses one
        // too - and deriving the flag from height alone handed a DOWNED player PRONE_VIEWHEIGHT (16).
        // The DBNO camera then applies cg_dbnoCamVert -30 from the view origin, so the camera resolved
        // to 16 - 30 = -14 and sat UNDER THE MAP (user, 2026-08-26). Before prone existed a downed
        // player kept a standing viewheight and -30 landed safely above the floor; requiring the coop
        // prone state restores that exactly while leaving real prone untouched.
        if (maxs.z == 20.0f && m_bCoopProne) {
            client->ps.pm_flags |= PMF_VIEW_PRONE;
        } else if (maxs.z == 54.0f || maxs.z == 60.0f) {
            client->ps.pm_flags |= PMF_DUCKED;
        } else if (viewheight == JUMP_START_VIEWHEIGHT) {
            client->ps.pm_flags |= PMF_VIEW_JUMP_START;
        }
    } else {
        if (maxs.z == 60.0f) {
            client->ps.pm_flags |= PMF_DUCKED;
        } else if (maxs.z == 54.0f) {
            client->ps.pm_flags |= PMF_DUCKED | PMF_VIEW_PRONE;
        } else if (maxs.z == 20.0f) {
            client->ps.pm_flags |= PMF_VIEW_PRONE;
        } else if (maxs.z == 53.0f) {
            client->ps.pm_flags |= PMF_VIEW_DUCK_RUN;
        } else if (viewheight == JUMP_START_VIEWHEIGHT) {
            client->ps.pm_flags |= PMF_VIEW_JUMP_START;
        }
    }

    switch (movecontrol) {
    case MOVECONTROL_USER:
    case MOVECONTROL_LEGS:
    case MOVECONTROL_USER_MOVEANIM:
        break;

    case MOVECONTROL_CROUCH:
        client->ps.pm_flags |= PMF_NO_PREDICTION | PMF_DUCKED | PMF_VIEW_PRONE;
        break;

    default:
        client->ps.pm_flags |= PMF_NO_PREDICTION;
    }

    if (movetype == MOVETYPE_NOCLIP) {
        if (!(last_ucmd.buttons & BUTTON_RUN)) {
            client->ps.speed = sv_runspeed->value * sv_walkspeedmult->value;
        } else {
            client->ps.speed = sv_runspeed->value;
        }
    } else if (!groundentity) {
        client->ps.speed = airspeed;
    } else {
        Weapon *pWeap;

        if (last_ucmd.buttons & BUTTON_RUN) {
            client->ps.speed = GetRunSpeed();
        } else {
            // BUTTON_RUN clear = the walk key (Shift). With coop_sprint enabled, Shift is the SPRINT key, so
            // holding it must NEVER drop below normal run: while stamina lasts the sprint boost below adds on
            // top, and once stamina is spent we fall back to RUN (not the slow walk that made sprint feel
            // like it ended after a couple seconds / "competed with walk"). The dedicated Alt walk key
            // (BUTTON_COOPWALK, handled just below) is the only thing that forces the slow walk now.
            cvar_t *pSprintBase = gi.Cvar_Get("coop_sprint", "1", CVAR_ARCHIVE);
            if (pSprintBase && pSprintBase->integer) {
                client->ps.speed = GetRunSpeed();
            } else {
                client->ps.speed = sv_runspeed->value * sv_walkspeedmult->value;
            }
        }

        // HZM coop - SPRINT: when the per-frame sprint state is set (computed in TickSprint: Shift held +
        // not aiming + moving forward + stamina left), scale ABOVE the normal run speed by coop_sprintMult.
        // Replaces the walk-slow value the BUTTON_RUN-clear branch above just set (Shift = walk key). When
        // sprint is disabled or stamina is exhausted, m_bCoopSprinting is false so we keep vanilla behavior.
        if (m_bCoopSprinting) {
            // [user 2026-08-23] 1.3 -> 1.15. Friend feedback: sprint is too fast on every gun. Note the
            // reporter was running an ARCHIVED 1.9 (a tuning fossil; 1.9 was never shipped and is not
            // in coop_defaults.cfg), and this is a SERVER cvar, so the host's value applied to both
            // players - most of what they were feeling was that. 1.15 verified in live play. Lowering
            // the multiplier scales every class down together and leaves the weight spread
            // (coop_weaponMoveByClass 0.98..0.74) intact, which is the part they wanted kept.
            cvar_t *pMult = gi.Cvar_Get("coop_sprintMult", "1.05", CVAR_ARCHIVE);
            float   mult  = pMult ? pMult->value : 1.3f;
            if (mult < 1.0f) { mult = 1.0f; } // sprint is never slower than run
            client->ps.speed = sv_runspeed->value * mult;
        }

        // HZM coop - Alt WALK key (BUTTON_COOPWALK) forces a slow walk. Shift is now sprint/breath, so the
        // dedicated walk-slow moved to Alt. The run/walk branch above keys off BUTTON_RUN (Shift), which is
        // still SET when only Alt is held, so without this you'd keep running. altWalk already suppresses
        // sprint in TickSprint, so just clamp to walk speed here (crouch mult below still stacks).
        if ((last_ucmd.buttons & BUTTON_COOPWALK) && !m_bCoopSprinting) {
            cvar_t *pSprintOn = gi.Cvar_Get("coop_sprint", "1", CVAR_ARCHIVE);
            if (pSprintOn && pSprintOn->integer) {
                client->ps.speed = sv_runspeed->value * sv_walkspeedmult->value;
            }
        }

        // HZM coop - aiming down the IRON SIGHTS (BUTTON_COOPADS) slows you to a careful aimed walk so movement
        // AND the footstep cadence match the pose. Scoped/zoomed weapons already slow via GetZoomMovement below,
        // but iron-sight ADS is not IsZoomed, so without this you stroll at full run speed (fast footsteps).
        // coop_adsSpeedMult scales it (1.0 = no slowdown). Suppressed while sprinting (you can't sprint + ADS).
        // DEFAULT 1.0 = OFF (reverted: the 0.55 aimed-walk felt far too slow). Footstep cadence will be
        // handled separately. Left in place + tunable: lower coop_adsSpeedMult below 1.0 to re-enable.
        m_iCoopSpeedBase = client->ps.speed; // HZM coop [222] - SPEEDPROBE: speed before the ADS/weapon mults

        if ((last_ucmd.buttons & BUTTON_COOPADS) && !m_bCoopSprinting) {
            cvar_t *pAdsMult = gi.Cvar_Get("coop_adsSpeedMult", "1.0", CVAR_ARCHIVE);
            float   amult    = pAdsMult ? pAdsMult->value : 1.0f;
            if (amult < 0.1f) { amult = 0.1f; } else if (amult > 1.0f) { amult = 1.0f; }
            if (amult < 1.0f) {
                client->ps.speed = (float)client->ps.speed * amult;
            }

            // HZM coop [227] - the 3P shoulder-aim SLOW-walk block that lived here was removed:
            // shoulder movement now gets a hard speed FLOOR after the full multiplier chain (see
            // the block just above SPEEDPROBE below). coop_adsSpeedMult3p scales that floor.
        }

        if (m_iMovePosFlags & MPF_POSITION_CROUCHING) {
            client->ps.speed = (float)client->ps.speed * sv_crouchspeedmult->value;
        }

        // HZM coop [user 2026-08-24] SLIDE SPEED. Placed immediately AFTER the crouch multiplier
        // deliberately: a slide IS a crouched state, so without this the crouch penalty would be exactly
        // the thing the slide exists to carry you through. Everything downstream (dmspeedmult, the
        // weapon-weight mults, the limp floors) still applies normally on top.
        //
        // The curve decays to 1.0 across the slide, so it ENDS at a normal crouch-walk rather than
        // stopping dead - the deceleration is the whole feel of the move.
        // HZM coop [user 2026-08-24] CRAWL SPEED. Placed with the other stance multipliers so the
        // later global mults (dmspeedmult, weapon weight, the limp floors) still apply on top.
        if (m_bCoopProne) {
            static cvar_t *pPS = NULL;
            if (!pPS) { pPS = gi.Cvar_Get("coop_proneSpeed", "0.42", CVAR_ARCHIVE); } // [user 2026-08-25] 0.30 -> 0.42. NOTE: the original reason given here ("a slope already costs speed") was WRONG - measurement showed the ground was flat (nrmZ 0.97-1.00). The real cause was the flat pm_stopspeed friction floor, fixed in PM_Friction; this multiplier is now just the feel knob it was meant to be.
            {
                float m = pPS->value;
                if (m < 0.05f) { m = 0.05f; } else if (m > 1.0f) { m = 1.0f; }
                client->ps.speed = (float)client->ps.speed * m;
            }
        }

        // [pass4, bug-2127] the supine/flip movement-freeze must reach the CLIENT PREDICTOR.
        // The TickCoopProne ucmd zeroing is server-only, so the client replayed raw usercmds
        // and predicted the ~84 u/s crawl the server discarded - rubber-banding whenever a
        // move key was held while supine or mid-flip (worse with latency, dedicated parity
        // rule), plus a FALSE client-side crawl no-fire gun-dip keyed on PREDICTED velocity.
        // ps.speed is replicated and feeds PM_CmdScale on BOTH sides, so zeroing it here
        // keeps prediction honest; the ucmd zeroing stays as the authoritative backstop.
        // Jump exits are unaffected (PM_CheckJump reads upmove directly, not CmdScale).
        if (m_bCoopProne && (m_bCoopSupine || level.time < m_fCoopSupineFlip)) {
            static cvar_t *pSupMoveSpd = NULL;
            if (!pSupMoveSpd) { pSupMoveSpd = gi.Cvar_Get("coop_supineMove", "0", CVAR_ARCHIVE); }
            if (!pSupMoveSpd->integer) {
                client->ps.speed = 0;
            }
        }

        if (m_bCoopSliding && m_fCoopSlideEnd > level.time) {
            cvar_t *pSpd = gi.Cvar_Get("coop_slideSpeed", "1.9", CVAR_ARCHIVE);
            cvar_t *pDur = gi.Cvar_Get("coop_slideTime", "0.75", CVAR_ARCHIVE);
            float   dur  = (pDur && pDur->value > 0.05f) ? pDur->value : 0.75f;
            float   frac = (m_fCoopSlideEnd - level.time) / dur; // 1 at entry -> 0 at the end
            float   top  = (pSpd ? pSpd->value : 1.9f);
            float   mult;

            if (frac < 0.0f) { frac = 0.0f; } else if (frac > 1.0f) { frac = 1.0f; }
            if (top < 1.0f) { top = 1.0f; } // a slide is never slower than the crouch it rides
            // frac*frac, not frac: a linear decay reads as being dragged backwards. Quadratic holds
            // most of the speed early and sheds it late, which is what a slide actually feels like.
            mult = 1.0f + (top - 1.0f) * frac * frac;
            client->ps.speed = (float)client->ps.speed * mult;
        }

        pWeap = GetActiveWeapon(WEAPON_MAIN);
        if (pWeap) {
            //
            // Also use the weapon movement speed
            //
            // HZM coop - UNIFORM weapon move speed: every gun moves at the same multiplier (default 0.89 =
            // the BAR's, the heaviest) so movement is consistent + a touch slower, instead of varying per
            // weapon. coop_weaponMoveSpeed <= 0 falls back to each weapon's own movementspeed (vanilla).
            static cvar_t *pWMS = NULL;
            static cvar_t *pWByClass = NULL;
            float          fwms;
            if (!pWMS)      { pWMS      = gi.Cvar_Get("coop_weaponMoveSpeed", "0.89", CVAR_ARCHIVE); }
            // [user 2026-08-22] WEIGHT AFFECTS YOUR LEGS, not just your hands. The weapon-weight
            // system already scales recoil, sway and recovery per class; this extends the same
            // model to movement so a BAR feels heavy to CARRY as well as to fire.
            //
            // Derived from GetWeaponClass(), NOT from the tiki's movementspeed field, and that is
            // deliberate: only 79 of 551 weapon tiks declare movementspeed at all, and OUR OWN
            // overrides are among the ones that do not - our bar.tik has none, so falling back to
            // the tiki value would have run the BAR at 1.0, FASTER than a pistol, and every skin
            // variant would inherit the same hole. One class table covers all 481 of our tiks and
            // every future variant for free, and it is the SAME class split the recoil feel uses,
            // so the two can never drift apart.
            //
            // Multipliers are chosen against the old uniform 0.89 so the average player speed is
            // roughly unchanged: pistols gain a little, rifles sit near where everyone was, MGs
            // and heavies pay for it.
            if (!pWByClass) { pWByClass = gi.Cvar_Get("coop_weaponMoveByClass", "1", CVAR_ARCHIVE); }
            if (!IsZoomed()) {
                if (pWByClass && pWByClass->integer) {
                    int iWC = pWeap->GetWeaponClass();

                    if (iWC & WEAPON_CLASS_PISTOL)     { fwms = 0.98f; }
                    else if (iWC & WEAPON_CLASS_SMG)   { fwms = 0.94f; }
                    else if (iWC & WEAPON_CLASS_RIFLE) { fwms = 0.89f; }
                    else if (iWC & WEAPON_CLASS_MG)    { fwms = 0.78f; }
                    else if (iWC & WEAPON_CLASS_HEAVY) { fwms = 0.74f; }
                    else                               { fwms = 0.92f; } // grenades, items, untyped
                } else {
                    fwms = (pWMS && pWMS->value > 0.0f) ? pWMS->value : pWeap->GetMovementSpeed();
                }
                client->ps.speed = (float)client->ps.speed * fwms;
            } else {
                client->ps.speed = (float)client->ps.speed * pWeap->GetZoomMovement();
            }
        }
    }

    if (g_gametype->integer != GT_SINGLE_PLAYER) {
        client->ps.speed = (int)((float)client->ps.speed * sv_dmspeedmult->value);
    }

    //====
    // Added in OPM
    for (int i = 0; i < MAX_SPEED_MULTIPLIERS; i++) {
        client->ps.speed = (int)((float)client->ps.speed * speed_multiplier[i]);
    }
    //====

    // HZM coop [user 2026-08-02] bug-1291 - LIMP SPEED. Applied AFTER the whole multiplier chain, as a
    // SCALE of the speed that survived it, not as an absolute `sv_runspeed * k` written mid-chain. That
    // placement matters: written earlier it would be silently overwritten by the Alt-walk branch and
    // then re-scaled by sv_dmspeedmult and speed_multiplier[], so the tuned number would not be the
    // number the player actually moves at - which is precisely how the walk-animation-at-run-speed
    // skating of bugs 319/554 happened. Scaling here keeps the limp proportional to whatever the rest
    // of the chain decided (crouch, ADS, gametype) instead of fighting it.
    if (m_bCoopLimping) {
        cvar_t *pLimpMult = gi.Cvar_Get("coop_limpSpeedMult", "0.60", CVAR_ARCHIVE);
        float   m         = pLimpMult ? pLimpMult->value : 0.60f;
        float   fPreLimp  = (float)client->ps.speed;
        float   fFloor;

        if (m < 0.2f) { m = 0.2f; } else if (m > 1.0f) { m = 1.0f; }
        client->ps.speed = (int)(fPreLimp * m);

        // [user 2026-08-02] bug-1292 - NEVER FREEZE. Every slowdown in this function MULTIPLIES:
        // iron-sight ADS, scope GetZoomMovement, the weapon weight mult, crouch, sv_dmspeedmult and
        // speed_multiplier[] all stack, and the limp then multiplies again - so an ADS or scoped
        // limping player collapses toward a standstill. "you should be able to move just slower than
        // normal ... when injured" applies to first-person ADS as much as to the shoulder stage.
        // Floor it at coop_limpMinFrac of the player's own run speed, but NEVER above what the same
        // player would have been doing WITHOUT the limp (fPreLimp) - so an injured player can never
        // end up faster than a healthy one in the identical stance, which a naive floor would allow
        // for a scoped sniper whose healthy speed is already below the floor.
        fFloor = GetRunSpeed() * sv_dmspeedmult->value
               * gi.Cvar_Get("coop_limpMinFrac", "0.35", CVAR_ARCHIVE)->value;
        if (m_iMovePosFlags & MPF_POSITION_CROUCHING) {
            fFloor *= sv_crouchspeedmult->value;
        } else if (m_bCoopProne) {
            // HZM coop [user 2026-08-25] BOTH floors discounted for CROUCH and neither for PRONE, so a
            // prone player who aimed (or was limping) had their speed RAISED back to ~0.7x run: "when
            // you hold right mouse down in prone and move around you move at the normal over shoulder
            // speed as walking". Measured directly - ps.speed read 172 while prone.
            cvar_t *pPSf = gi.Cvar_Get("coop_proneSpeed", "0.42", CVAR_ARCHIVE);
            float   mp   = pPSf ? pPSf->value : 0.42f;
            if (mp < 0.05f) { mp = 0.05f; } else if (mp > 1.0f) { mp = 1.0f; }
            fFloor *= mp;
        }
        if (fFloor > fPreLimp) {
            fFloor = fPreLimp; // injured is never faster than healthy in the same stance
        }
        if ((float)client->ps.speed < fFloor) {
            client->ps.speed = (int)fFloor;
        }
    }

    // HZM coop [227] - 3P SHOULDER-AIM SPEED FLOOR. The measured shoulder-move speed (45) never
    // matched anything the audited multiplier chain could produce ("starts off very very slow" -
    // user), so instead of scaling the possibly-poisoned value, FLOOR it after ALL multipliers:
    // while the shoulder stage is live you move at least GetRunSpeed * sv_dmspeedmult *
    // coop_adsSpeedMult3p (def 1.0 = full run pace, no heavy-weapon drag; raiseable to 1.6).
    // Crouch keeps its own scale so crouch-aiming doesn't rocket. SPEEDPROBE below still reports
    // the chain so the underlying culprit can be identified.
    // HZM coop [2026-08-02] bug-1291 - `&& !m_bCoopLimping`: this FLOOR would otherwise raise a
    // limping player back to full run pace whenever the 3P shoulder stage is up, undoing the limp
    // clamp above and skating the injured clip at run speed.
    // [pass5, bug-2130] ...and `&& !(supine || flip window)` for the same reason, found by three of
    // six auditors independently: this floor runs AFTER the pass-4 supine freeze (ps.speed = 0) and
    // raised it straight back to ~84 u/s, resurrecting the exact bug-2127 rubber-band it fixed. It
    // hits the feature's PRIMARY path, not an edge: settled supine REQUIRES ADS held, and staged 3P
    // ADS stage 0 IS the shoulder, so every third-person supine player was floored. The limp clamp
    // above survives only because it clamps the floor to a pre-freeze capture; this one did not.
    if (m_bCoopShoulderAim && !m_bCoopSprinting
        && !(m_bCoopProne && (m_bCoopSupine || level.time < m_fCoopSupineFlip))) {
        cvar_t *p3pMult = gi.Cvar_Get("coop_adsSpeedMult3p", "0.7", CVAR_ARCHIVE); // [237] -0.10 again per user (was 0.8); live-tunable 0.5-1.6
        float   m3      = p3pMult ? p3pMult->value : 1.0f;
        float   fFloor;

        if (m3 < 0.5f) { m3 = 0.5f; } else if (m3 > 1.6f) { m3 = 1.6f; }
        // [user 2026-08-28] "when over shoulder ads and moving in third person, you move faster than
        // you move when you sprint". Correct, and it was arithmetic rather than tuning - two errors
        // stacking:
        //
        // 1. GetRunSpeed() DOES NOT RETURN THE RUN SPEED. Under the engine's legacy sprint rule it
        //    returns sv_runspeed * sv_sprintmult_dm (1.20), so the floor was really 1.20 * 0.7 =
        //    0.84 of run. Coop sprint is sv_runspeed * coop_sprintMult (1.05) and then takes the
        //    weapon-class multiplier - 0.89 rifle, 0.78 MG - landing at 0.93 and 0.82. So with any
        //    heavy weapon the aimed walk genuinely outran the sprint.
        //
        // 2. THE FLOOR IGNORED WEAPON WEIGHT, because it is applied after the class multiplier and
        //    simply overwrote it - a Panzerschreck aimed-walked exactly as fast as a Luger. The user
        //    asked whether weight should factor in here; it should, and it did not.
        //
        // Base it on the real run speed, carry the same weight multiplier the rest of the chain uses,
        // and cap it at what this player would actually be doing sprinting - the discipline the limp
        // floor beside it already follows, so no floor can ever make a stance faster than the stance
        // that is supposed to be the fast one.
        fFloor = sv_runspeed->value * sv_dmspeedmult->value * m3;
        {
            Weapon *pFW = GetActiveWeapon(WEAPON_MAIN);
            int     iFC = pFW ? pFW->GetWeaponClass() : 0;
            float   fWm = 0.92f;

            if (iFC & WEAPON_CLASS_PISTOL)     { fWm = 0.98f; }
            else if (iFC & WEAPON_CLASS_SMG)   { fWm = 0.94f; }
            else if (iFC & WEAPON_CLASS_RIFLE) { fWm = 0.89f; }
            else if (iFC & WEAPON_CLASS_MG)    { fWm = 0.78f; }
            else if (iFC & WEAPON_CLASS_HEAVY) { fWm = 0.74f; }
            fFloor *= fWm;

            // never at or above this player's own sprint
            {
                static cvar_t *pCap = NULL;
                cvar_t        *pSM  = gi.Cvar_Get("coop_sprintMult", "1.05", CVAR_ARCHIVE);
                float          fSprint;
                if (!pCap) { pCap = gi.Cvar_Get("coop_adsFloorCap", "0.85", CVAR_ARCHIVE); }
                fSprint = sv_runspeed->value * sv_dmspeedmult->value * fWm
                        * ((pSM && pSM->value > 1.0f) ? pSM->value : 1.0f);
                if (fFloor > fSprint * pCap->value) {
                    fFloor = fSprint * pCap->value;
                }
            }
        }
        if (m_iMovePosFlags & MPF_POSITION_CROUCHING) {
            fFloor *= sv_crouchspeedmult->value;
        } else if (m_bCoopProne) {
            // HZM coop [user 2026-08-25] BOTH floors discounted for CROUCH and neither for PRONE, so a
            // prone player who aimed (or was limping) had their speed RAISED back to ~0.7x run: "when
            // you hold right mouse down in prone and move around you move at the normal over shoulder
            // speed as walking". Measured directly - ps.speed read 172 while prone.
            cvar_t *pPSf = gi.Cvar_Get("coop_proneSpeed", "0.42", CVAR_ARCHIVE);
            float   mp   = pPSf ? pPSf->value : 0.42f;
            if (mp < 0.05f) { mp = 0.05f; } else if (mp > 1.0f) { mp = 1.0f; }
            fFloor *= mp;
        }
        // HZM coop [user 2026-08-02] bug-1292 - SCALE the floor while limping, do not SKIP it.
        // This floor is the only thing that gives the 3P shoulder stage a usable movement speed
        // (the raw chain produces ~45 - see the note above), so excluding limping players from it
        // entirely left them at ~45 * coop_limpSpeedMult, i.e. standing still. Applying the limp
        // multiplier to the FLOOR instead keeps them mobile but slower than an uninjured player
        // in the same stance, which is the intent: "you should be able to move just slower than
        // normal when using over shoulder when injured".
        if (m_bCoopLimping) {
            cvar_t *pLimpMult = gi.Cvar_Get("coop_limpSpeedMult", "0.60", CVAR_ARCHIVE);
            float   ml        = pLimpMult ? pLimpMult->value : 0.60f;
            if (ml < 0.2f) { ml = 0.2f; } else if (ml > 1.0f) { ml = 1.0f; }
            fFloor *= ml;
        }
        if ((float)client->ps.speed < fFloor) {
            client->ps.speed = (int)fFloor;
        }
    }

    // HZM coop [222] - SPEEDPROBE: once/sec while ADS is held (1P irons OR the 3P shoulder stage),
    // print the FULL speed chain: base (before ADS/weapon mults), final, and every factor - the
    // measured 45 could not be reproduced from the audited mults, so dump them all. Remove once the
    // "ADS side-step slow/weird" report closes.
    if (((last_ucmd.buttons & BUTTON_COOPADS) || m_bCoopShoulderAim) && level.time - m_fCoopProbeTime > 1.0f) {
        m_fCoopProbeTime = level.time;
        Weapon *pWProbe  = GetActiveWeapon(WEAPON_MAIN);
        gi.Printf(
            "^~^~^ SPEEDPROBE final=%d base=%d runspd=%.0f svrun=%.0f zoomed=%d wms=%.2f zmv=%.2f dm=%.2f gun='%s' fwd=%d side=%d legs='%s' torso='%s'\n",
            client->ps.speed,
            m_iCoopSpeedBase,
            GetRunSpeed(),
            sv_runspeed->value,
            IsZoomed() ? 1 : 0,
            pWProbe ? pWProbe->GetMovementSpeed() : -1.0f,
            pWProbe ? pWProbe->GetZoomMovement() : -1.0f,
            sv_dmspeedmult->value,
            pWProbe ? pWProbe->item_name.c_str() : "none",
            (int)last_ucmd.forwardmove,
            (int)last_ucmd.rightmove,
            currentState_Legs ? currentState_Legs->getName() : "?",
            currentState_Torso ? currentState_Torso->getName() : "?"
        );
    }

    client->ps.gravity = sv_gravity->value * gravity;

    if ((movecontrol != MOVECONTROL_ABSOLUTE) && (movecontrol != MOVECONTROL_PUSH)
        && (movecontrol != MOVECONTROL_CLIMBWALL)) {
        Vector oldpos(origin);

        SetMoveInfo(&pm, ucmd);
        Pmove(&pm);
        GetMoveInfo(&pm);

        if (g_gametype->integer != GT_SINGLE_PLAYER && groundentity && groundentity->entity
            && groundentity->entity->IsSubclassOfSentient()) {
            //
            // Added in 2.0
            // If the player is on another sentient, try to make it fall off
            //
            velocity -= Vector(orientation[0]) * (random() * 20.f);
            velocity -= Vector(orientation[1]) * (random() * 10.f);
        }

        ProcessPmoveEvents(pm.pmoveEvent);

        // if we're not moving, set the blocked flag in case the user is trying to move
        if ((ucmd->forwardmove || ucmd->rightmove) && ((oldpos - origin).length() < 0.005f)) {
            moveresult = MOVERESULT_BLOCKED;
        }

        // HZM coop [user 2026-08-25] CRAWL PROBE (coop_crawlDebug 1). Prone crawl is slow and stalls on
        // slopes. The suspects are separable and this prints all of them together: ps.speed is what the
        // stance multiplier produced, disp is what the frame ACTUALLY moved, and blocked is
        // `disp < 0.005` - the single condition that sets MOVERESULT_BLOCKED, which both rewinds the
        // player to oldpos AND kicks the legs statemap out of PRONE_FORWARD (it requires !BLOCKED), so
        // one stalled frame costs the crawl animation as well as the distance.
        {
            static cvar_t *pCDbg = NULL;
            static int     s_cLast = 0;
            static int     s_blocked = 0, s_frames = 0;

            if (!pCDbg) { pCDbg = gi.Cvar_Get("coop_crawlDebug", "0", 0); }
            if (pCDbg->integer && (client->ps.pm_flags & PMF_VIEW_PRONE) && (ucmd->forwardmove || ucmd->rightmove)) {
                float disp = (oldpos - origin).length();
                s_frames++;
                if (moveresult >= MOVERESULT_BLOCKED) { s_blocked++; }
                if (level.inttime - s_cLast > 500) {
                    s_cLast = level.inttime;
                    gi.Printf("^~^~^ CRAWL speed=%.1f vel=%.1f disp=%.3f blocked=%d/%d gnd=%d velz=%.1f | nrmZ=%.3f walking=%d stepped=%d xy=%.1f\n",
                              (float)client->ps.speed, velocity.length(), disp,
                              s_blocked, s_frames, groundentity ? 1 : 0, velocity[2],
                              pm.coopDbgGroundNormalZ, (int)client->ps.walking, (int)pm.stepped, pm.xyspeed);
                    s_blocked = 0; s_frames = 0;
                }
            }
        }
        if (client->ps.walking && moveresult >= MOVERESULT_BLOCKED) {
            setOrigin(oldpos);
            VectorCopy(origin, client->ps.origin);
        }
    } else {
        if (movecontrol == MOVECONTROL_CLIMBWALL) {
            PM_UpdateViewAngles(&client->ps, ucmd);
            v_angle = client->ps.viewangles;
        } else if (!deadflag) {
            v_angle = client->ps.viewangles;
        }

        // should collect objects to touch against
        memset(&pm, 0, sizeof(pmove_t));

        // keep the command time up to date or else the next PMove we run will try to catch up
        client->ps.commandTime = ucmd->serverTime;

        velocity = vec_zero;
    }

    if ((getMoveType() != MOVETYPE_NOCLIP) && (client->ps.pm_flags & PMF_NO_PREDICTION)) {
        if ((movecontrol == MOVECONTROL_ABSOLUTE) || (movecontrol == MOVECONTROL_CLIMBWALL)) {
            velocity = vec_zero;
        }

        if ((movecontrol == MOVECONTROL_ANIM) || (movecontrol == MOVECONTROL_CLIMBWALL)
            || (movecontrol == MOVECONTROL_USEANIM) || (movecontrol == MOVECONTROL_LOOPUSEANIM)
            || (movecontrol == MOVECONTROL_USER_MOVEANIM)) {
            Vector delta = vec_zero;
            PlayerAnimDelta(delta);

            // using PM_NOCLIP for a smooth move
            //client->ps.pm_type = PM_NOCLIP;

            if (delta != vec_zero) {
                float mat[3][3];
                AngleVectors(angles, mat[0], mat[1], mat[2]);
                MatrixTransformVector(delta, mat, move);
                AnimMove(move, &origin);
                setOrigin(origin);
                CheckGround();
            }
        }
    }

    m_fLastDeltaTime = level.time;

    TouchStuff(&pm);
}

void Player::VehicleMove(usercmd_t *ucmd)
{
    if (!m_pVehicle) {
        return;
    }

    oldorigin = origin;

    client->ps.pm_type = GetMovePlayerMoveType();

    // set move flags
    client->ps.pm_flags &=
        ~(PMF_FROZEN | PMF_NO_PREDICTION | PMF_NO_MOVE | PMF_DUCKED | PMF_TURRET | PMF_VIEW_PRONE | PMF_VIEW_DUCK_RUN
          | PMF_VIEW_JUMP_START);

    // disable prediction
    client->ps.pm_flags |= PMF_TURRET | PMF_NO_PREDICTION;

    // HZM coop [219] - bug-309 root cause: the jeep .30cal gunner runs THIS path (m_pVehicle set,
    // m_pTurret NULL - TurretMove/COOP_ON_TURRET never fired). Legs-state diagnostic mirrored here.
    {
        static str sLastVehLegs;

        if (currentState_Legs && sLastVehLegs != currentState_Legs->getName()) {
            sLastVehLegs = currentState_Legs->getName();
            gi.Printf("^~^~^ VEHSTATE legs='%s' manned=%d\n", sLastVehLegs.c_str(),
                (level.time - m_fCoopVehTurretTime < 0.25f) ? 1 : 0);
        }
    }

    if (level.playerfrozen || m_bFrozen) {
        client->ps.pm_flags |= PMF_FROZEN;
    }

    client->ps.gravity = gravity * sv_gravity->value;

    if (m_pVehicle->Drive(current_ucmd)) {
        client->ps.commandTime = ucmd->serverTime;
        // Added in OPM
        //  The player can't walk while attached to a vehicle
        client->ps.groundEntityNum = ENTITYNUM_NONE;
        client->ps.walking         = false;
    } else {
        ClientMove(ucmd);
    }
}

void Player::TurretMove(usercmd_t *ucmd)
{
    if (!m_pTurret) {
        return;
    }

    // HZM coop [217] - bug-309 diagnostic: name the ACTUAL legs state while manning a turret
    // (the COOP_TURRET_MAN pose never engaged; hub-edge theory unverified - measure, don't guess)
    {
        static str sLastTurretLegs;

        if (currentState_Legs && sLastTurretLegs != currentState_Legs->getName()) {
            sLastTurretLegs = currentState_Legs->getName();
            gi.Printf("^~^~^ TURRETSTATE legs='%s'\n", sLastTurretLegs.c_str());
        }
    }

    oldorigin = origin;

    client->ps.pm_type = GetMovePlayerMoveType();

    // set move flags
    client->ps.pm_flags &=
        ~(PMF_FROZEN | PMF_NO_PREDICTION | PMF_NO_MOVE | PMF_DUCKED | PMF_TURRET | PMF_VIEW_PRONE | PMF_VIEW_DUCK_RUN
          | PMF_VIEW_JUMP_START);

    // disable prediction
    client->ps.pm_flags |= PMF_TURRET | PMF_NO_PREDICTION;
    if (getMoveType() == MOVETYPE_PORTABLE_TURRET) {
        client->ps.pm_flags |= PMF_TURRET;
    }

    if (level.playerfrozen || m_bFrozen) {
        client->ps.pm_flags |= PMF_FROZEN;
    }

    client->ps.gravity = gravity * sv_gravity->value;

    if (m_pVehicle) {
        // Added in 2.30
        m_pVehicle->PathDrive(current_ucmd);
    }

    if (m_pTurret->IsSubclassOfTurretGun() && m_pTurret->UserAim(current_ucmd)) {
        client->ps.commandTime = ucmd->serverTime;
        // Added in OPM
        //  The player can't walk while attached to a turret
        client->ps.groundEntityNum = ENTITYNUM_NONE;
        client->ps.walking         = false;
    } else {
        ClientMove(ucmd);
    }
}

void Player::ClientInactivityTimer(void)
{
    if (g_gametype->integer == GT_SINGLE_PLAYER) {
        return;
    }

    if (g_inactivekick->integer && g_inactivekick->integer < 60) {
        gi.cvar_set("g_inactiveKick", "60");
    }

    if (g_inactivespectate->integer && g_inactivespectate->integer < 20) {
        gi.cvar_set("g_inactiveSpectate", "20");
    }

    if (num_team_kills >= g_teamkillkick->integer) {
        const str message = gi.LV_ConvertString("was removed from the server for killing too many teammates.");

        //
        // The player reached maximum team kills
        //
        G_PrintToAllClients(va("%s %s\n", client->pers.netname, message.c_str()), 2);

        if (Q_stricmp(Info_ValueForKey(client->pers.userinfo, "ip"), "localhost")) {
            //
            // Make sure to not kick the local host
            //
            gi.DropClient(client->ps.clientNum, message.c_str());
        } else if (!m_bSpectator) {
            // if it's the host, put it back in spectator mode
            num_team_kills      = 0;
            m_iLastNumTeamKills = 0;

            PostEvent(EV_Player_Spectator, 0);
        }

        return;
    }

    if (num_team_kills >= g_teamkillwarn->integer && num_team_kills > m_iLastNumTeamKills) {
        const str sWarning   = gi.LV_ConvertString("Warning:");
        const str sTeamKills = gi.LV_ConvertString("more team kill(s) and you will be removed from the server.");

        m_iLastNumTeamKills = num_team_kills;

        gi.centerprintf(
            edict, "%s %i %s", sWarning.c_str(), g_teamkillkick->integer - num_team_kills, sTeamKills.c_str()
        );
    }

    if (current_ucmd->buttons & BUTTON_ANY || (!g_inactivespectate->integer && !g_inactivekick->integer)
        || current_ucmd->forwardmove || current_ucmd->rightmove || current_ucmd->upmove
        || (m_bTempSpectator && client->lastActiveTime >= level.inttime - 5000)) {
        client->lastActiveTime = level.inttime;
        client->activeWarning  = 0;
        return;
    }

    if (g_inactivekick->integer && client->lastActiveTime < level.inttime - 1000 * g_inactivekick->integer) {
        const char *s = Info_ValueForKey(client->pers.userinfo, "ip");

        if (Q_stricmp(s, "localhost")) {
            gi.DropClient(client->ps.clientNum, "was dropped for inactivity");
            return;
        }

        if (m_bSpectator) {
            return;
        }

        PostEvent(EV_Player_Spectator, 0);
        return;
    }

    if (g_inactivespectate->integer && client->lastActiveTime < level.inttime - g_inactivespectate->integer * 1000
        && !m_bSpectator) {
        PostEvent(EV_Player_Spectator, 0);
        return;
    }

    if (g_inactivekick->integer) {
        static struct {
            int iLevel;
            int iTime;
        } warnkick[7] = {
            {1,  30},
            {8,  15},
            {9,  5 },
            {10, 4 },
            {11, 3 },
            {12, 2 },
            {13, 1 }
        };

        int iKickWait = g_inactivekick->integer - (level.inttime - client->lastActiveTime) / 1000 - 1;

        for (int i = 0; i < 7; i++) {
            if (client->activeWarning < warnkick[i].iLevel && iKickWait < warnkick[i].iTime) {
                const str sActionIn = gi.LV_ConvertString("You will be kicked for inactivity in");
                const str sSeconds  = gi.LV_ConvertString("seconds");

                client->activeWarning = warnkick[i].iLevel;

                gi.centerprintf(edict, "%s %i %s", sActionIn.c_str(), warnkick[i].iTime, sSeconds.c_str());

                return;
            }
        }
    }

    if (g_inactivespectate->integer && dm_team != TEAM_SPECTATOR) {
        static struct {
            int iLevel;
            int iTime;
        } warnspectate[6] = {2, 15, 3, 5, 4, 4, 5, 3, 6, 2, 7, 1};

        int iSpectateWait = g_inactivespectate->integer - (level.inttime - client->lastActiveTime) / 1000 - 1;

        for (int i = 0; i < 6; i++) {
            if (client->activeWarning < warnspectate[i].iLevel && iSpectateWait < warnspectate[i].iTime) {
                const str sActionIn = gi.LV_ConvertString("You will be moved to spectator for inactivity in");
                const str sSeconds  = gi.LV_ConvertString("seconds");

                client->activeWarning = warnspectate[i].iLevel;

                gi.centerprintf(edict, "%s %i %s", sActionIn.c_str(), warnspectate[i].iTime, sSeconds.c_str());

                return;
            }
        }
    }
}

void Player::UpdateEnemies(void)
{
    float  fFov;
    float  fMaxDist;
    float  fMaxCosSquared;
    Vector vLookDir;

    if (g_gametype->integer != GT_SINGLE_PLAYER) {
        return;
    }

    if (m_pNextSquadMate == this) {
        return;
    }

    fFov           = fov * 0.9f;
    fMaxDist       = world->farplane_distance * 0.7867f;
    fMaxCosSquared = 0.0f;

    AngleVectors(m_vViewAng, vLookDir, NULL, NULL);

    if (m_Enemy) {
        m_Enemy->m_iAttackerCount -= 3;
        m_Enemy = NULL;
    }

    for (Sentient *obj = level.m_HeadSentient[0]; obj != NULL; obj = obj->m_NextSentient) {
        Vector vDelta;
        float  fDot;
        float  fDotSquared;

        if (CanSee(obj, fFov, fMaxDist, false)) {
            obj->m_fPlayerSightLevel += level.frametime;

            vDelta      = obj->origin - origin;
            fDot        = DotProduct(vDelta, vLookDir);
            fDotSquared = fDot * fDot;

            if (fDotSquared > fMaxCosSquared * vDelta.lengthSquared()) {
                fMaxCosSquared = fDotSquared / vDelta.lengthSquared();
                m_Enemy        = obj;
            }
        } else {
            obj->m_fPlayerSightLevel = 0.0f;
        }
    }

    if (m_Enemy) {
        m_Enemy->m_iAttackerCount += 3;
    }
}

/*
==============
CoopBotDrive        HZM coop - dev/test only (coop_botInput)

Server-side usercmd injection: overwrites THIS frame's usercmd so a connected
client automatically aims at, fires on, and advances toward the nearest visible
German. This is how the 4 test clients hold a real firefight with the AI while
nobody is at the keyboard - because the bot fires actual bullets with real
line-of-sight, the enemy AI genuinely engages, retaliates, and (with the dynamic
-AI stack on) repositions, which the script-side damage simulation could never
make it do. Called from the very top of ClientThink; a no-op unless coop_botInput
is set, so vanilla behavior is completely unchanged when the cvar is 0.
==============
*/
void Player::CoopBotDrive(usercmd_t *ucmd)
{
    if (IsDead() || IsSpectator() || m_pVehicle || m_pTurret) {
        return;
    }

    Vector eye = EyePosition();

    // Revalidate / rescan the target at most a few times a second (cheap + steady aim). The cached
    // SafePtr auto-clears if the enemy is freed; we still drop it when it dies or breaks LOS.
    Sentient *target = m_pCoopBotTarget;
    if (target && (target->health <= 0 || target->deadflag || !CanSee(target, 360.0f, 8192.0f, false))) {
        target = NULL;
    }
    if (!target || level.inttime >= m_iCoopBotRetarget) {
        // HZM coop [user 2026-08-24] TARGET SELECTION IS A MEASUREMENT BIAS, so make it switchable.
        // The original always took the NEAREST visible German. Combined with the close-the-distance
        // movement below that made this rig sample close-quarters combat by construction - the
        // engagement-distance histogram (coop_shotdump) read a 20 m mean on m3l2 purely because the
        // bot walks to 7-18 m and shoots whatever is closest. Any conclusion about "the range players
        // fight at" drawn from that would have been an artifact of the test tool.
        //   coop_botTargetMode 0 = nearest (default, unchanged)
        //                      1 = FARTHEST visible - samples the long tail
        //                      2 = RANDOM visible (reservoir) - the unbiased one
        static cvar_t *pTgtMode = NULL;
        int            iTgtMode;
        int            iSeen = 0;

        if (!pTgtMode) { pTgtMode = gi.Cvar_Get("coop_botTargetMode", "0", 0); }
        iTgtMode = pTgtMode->integer;

        m_iCoopBotRetarget = level.inttime + 400;
        Sentient *best     = NULL;
        float     bestDist = (iTgtMode == 1) ? -1.0f : 1.0e18f;
        for (Sentient *obj = level.m_HeadSentient[TEAM_GERMAN]; obj != NULL; obj = obj->m_NextSentient) {
            if (obj == this || obj->health <= 0 || obj->deadflag) {
                continue;
            }
            float d = (obj->centroid - origin).lengthSquared();
            // cheap rejects BEFORE the trace, exactly as before - CanSee is the expensive part
            if (iTgtMode == 0 && d >= bestDist) { continue; }
            if (iTgtMode == 1 && d <= bestDist) { continue; }
            if (!CanSee(obj, 360.0f, 8192.0f, false)) {
                continue;
            }
            if (iTgtMode == 2) {
                // reservoir sample: every visible enemy gets an equal chance, so the distance
                // distribution of the CHOSEN target matches the distribution of what is visible.
                iSeen++;
                if (G_Random() * (float)iSeen < 1.0f) { best = obj; }
                continue;
            }
            best     = obj;
            bestDist = d;
        }
        if (best) {
            target = best;
        }
        m_pCoopBotTarget = target;
    }

    if (!target) {
        return; // nothing to engage this frame - leave the raw input untouched (bot idles)
    }

    // --- aim: point the view at the target's centroid via the usercmd angles ---
    // pmove computes viewangle = SHORT2ANGLE(ucmd->angles + delta_angles), so to land on the desired
    // world angle we set ucmd->angles = ANGLE2SHORT(desired) - delta_angles (short arithmetic wraps).
    Vector aimAng = (target->centroid - eye).toAngles();
    ucmd->angles[0] = (short)(ANGLE2SHORT(aimAng[0]) - client->ps.delta_angles[0]);
    ucmd->angles[1] = (short)(ANGLE2SHORT(aimAng[1]) - client->ps.delta_angles[1]);
    ucmd->angles[2] = (short)(0 - client->ps.delta_angles[2]);

    // --- fire in bursts (~600ms on / ~400ms off) so autos don't jam and the AI gets gaps to move ---
    if ((level.inttime % 1000) < 600) {
        ucmd->buttons |= BUTTON_ATTACKLEFT;
    }

    // --- movement: close to mid range, hold there, gentle strafe so the bot isn't a static target ---
    // HZM coop [user 2026-08-24] THE STAND-OFF RANGE IS THE OTHER HALF OF THE BIAS. Hard-coded 700/300
    // meant the bot always fought at 7.6-17.8 m whatever the map offered. coop_botRange sets the band
    // it holds at; coop_botRange 0 makes it STAND ITS GROUND and engage from wherever it already is,
    // which is what an unbiased distance sample needs (pair with coop_botTargetMode 2).
    float dist = (target->centroid - origin).length();
    {
        static cvar_t *pRange = NULL;
        float          fHold;

        if (!pRange) { pRange = gi.Cvar_Get("coop_botRange", "700", 0); }
        fHold = pRange->value;
        if (fHold <= 0.0f) {
            ucmd->forwardmove = 0; // stand ground
        } else if (dist > fHold) {
            ucmd->forwardmove = 127;
        } else if (dist < fHold * 0.43f) {
            ucmd->forwardmove = (signed char)-80;
        } else {
            ucmd->forwardmove = 0;
        }
    }
    ucmd->rightmove = ((level.inttime % 3000) < 1500) ? (signed char)90 : (signed char)-90;
}

/*
==============
ClientThink

This will be called once for each client frame, which will
usually be a couple times for each server frame.
==============
*/
void Player::ClientThink(void)
{
    // HZM coop - BOT COMBAT DRIVE (dev/test, coop_botInput): inject an auto-combat usercmd before any
    // of the frame's input is consumed, so the whole downstream (button diff, weapon fire, ClientMove)
    // sees the bot's aim/fire/move. Master cvar default 0 => never called => pure vanilla.
    {
        static cvar_t *pCoopBotInput = NULL;
        if (!pCoopBotInput) {
            pCoopBotInput = gi.Cvar_Get("coop_botInput", "0", 0);
        }
        if (pCoopBotInput->integer && current_ucmd) {
            CoopBotDrive(current_ucmd);
        }
    }

    // sanity check the command time to prevent speedup cheating
    if (current_ucmd->serverTime > level.svsTime) {
        //
        // we don't want any future commands, these could be from the previous game
        //
        return;
    }

    if (current_ucmd->serverTime < level.svsTime - 1000) {
        current_ucmd->serverTime = level.svsTime - 1000;
    }

    if ((current_ucmd->serverTime - client->ps.commandTime) < 1) {
        return;
    }

    // HZM coop - TickLimp MUST run before TickSprint: TickSprint reads m_bCoopLimping to suppress
    // sprinting while wounded, and reading a stale flag for one frame is exactly the order-dependent
    // class of bug this codebase has already paid for twice (bugs 319 / 554).
    TickLimp();
    TickSprint();
    TickSlide(); // HZM coop - MUST follow TickSprint: it reads m_bCoopSprinting for THIS frame
    TickCoopNade(); // HZM coop - quick grenade (bind g "+coopnade")
    TickCoopLook(); // HZM coop - head tracking + torso counter-rotation
    TickCoopProne(); // HZM coop - hold crouch to go prone
    TickCoopStress(); // HZM coop - server-side stress envelope (drives weapon spread)
    TickCoopRecoil(); // HZM coop - hand the authored recoil back at the weapon's recentre speed
    // HZM coop [user 2026-08-27] publish the active weapon's handling weight, change-only. The client
    // uses it to slow the raise and weight the punch; only the server can resolve it, because the
    // lookup is keyed by the weapon's model name and the client only ever sees its display name.
    {
        Weapon *w = GetActiveWeapon(WEAPON_MAIN);
        int     iHeft = w ? (int)(w->CoopHeft() * 100.0f + 0.5f) : 0;
        if (iHeft != m_iCoopGunHeftSent) {
            m_iCoopGunHeftSent = iHeft;
            gi.SendServerCommand(edict - g_entities, "stufftext \"set coop_gunHeft %d\"", iHeft);
        }
    }

    // HZM coop [user 2026-08-26] NO FIRING WHILE CRAWLING. "you must be stopped to shoot" - prone
    // fire is the stance's whole payoff, so shooting mid-crawl is stripped AT THE INPUT, the same
    // level the bot drive injects at: the statemap never sees the attack press, so no shoot
    // animation starts, and the block is server-authoritative rather than cosmetic (sprint's gun
    // lower, by contrast, never blocked anything - measured before copying, for once).
    // Hysteresis: blocks above 30 u/s, releases below 15, so the gun does not flicker at the
    // crawl's stop-start boundary.
    // [user 2026-08-26] "I don't think you should be able to shoot while sprinting either" - same
    // strip, same reasoning: sprint's gun-lower was ONLY ever visual (measured before the crawl gate
    // was built - it never blocked a shot). Stripping at the input means stopping the sprint is what
    // brings the trigger back, which is the tradeoff sprint is supposed to carry. m_bCoopSprinting is
    // this frame's value because TickSprint runs earlier in this same function.
    if (m_bCoopSprinting && current_ucmd) {
        static cvar_t *pSNF = NULL;
        if (!pSNF) { pSNF = gi.Cvar_Get("coop_sprintNoFire", "1", CVAR_ARCHIVE); }
        if (pSNF->integer) {
            current_ucmd->buttons &= ~(BUTTON_ATTACKLEFT | BUTTON_ATTACKRIGHT);
        }
    }

    // [weight 7] the ready-up window: the trigger is dead until the weapon is back on target. Held
    // beside the other fire strips so every reason the trigger can be inert lives in one place.
    if (current_ucmd && m_fCoopReadyUpAt > level.time && !deadflag) {
        current_ucmd->buttons &= ~(BUTTON_ATTACKLEFT | BUTTON_ATTACKRIGHT);
    }

    if (m_bCoopProne && (client->ps.pm_flags & PMF_VIEW_PRONE) /*[pass4] gate on BOTH*/ && current_ucmd) {
        static cvar_t *pPNF = NULL;
        if (!pPNF) { pPNF = gi.Cvar_Get("coop_proneMoveNoFire", "1", CVAR_ARCHIVE); }
        if (pPNF->integer) {
            float v2 = velocity[0] * velocity[0] + velocity[1] * velocity[1];
            if (v2 > 30.0f * 30.0f) {
                m_bCoopCrawlNoFire = true;
            } else if (v2 < 15.0f * 15.0f) {
                m_bCoopCrawlNoFire = false;
            }
            if (m_bCoopCrawlNoFire) {
                current_ucmd->buttons &= ~(BUTTON_ATTACKLEFT | BUTTON_ATTACKRIGHT);
            }
        }

        // HZM coop [user 2026-08-27] DO NOT FIRE WHERE THE GUN IS NOT POINTING.
        //
        // "when aiming in free cam your bullets still follow your crosshair even if its aimed behind
        // you even though your gun is in front of you." Exactly right, and it is structural: the shot
        // is cast along the VIEW (Weapon::GetMuzzlePosition uses m_vViewAng), while a prone body only
        // eases toward that view at 25-120 deg/s - so between the aim arriving and the body catching
        // up there is a window where the crosshair is behind you and the weapon is still pointing
        // ahead. Firing there sends rounds through your own torso and reads as a cheat.
        //
        // The trigger is held until the weapon actually agrees with the crosshair. Settled supine is
        // exempt: on your back the gun tracks the aim by design, so the offset there is not a lie.
        // Standing is unaffected - pmove keeps body yaw locked to view, so it can never disagree.
        {
            static cvar_t *pAimGate = NULL;
            if (!pAimGate) { pAimGate = gi.Cvar_Get("coop_proneAimGate", "45", CVAR_ARCHIVE); }
            if (pAimGate->value > 0.0f && !m_bCoopSupine) {
                float fOff = AngleSubtract(client->ps.viewangles[YAW], m_fCoopProneBodyYaw);
                if (fOff > pAimGate->value || fOff < -pAimGate->value) {
                    current_ucmd->buttons &= ~(BUTTON_ATTACKLEFT | BUTTON_ATTACKRIGHT);
                }
            }
        }
    } else {
        m_bCoopCrawlNoFire = false;
    }

    // HZM coop [223] - Shift is the SPRINT key, so BUTTON_RUN arrives CLEAR while it's held (legacy
    // walk semantics). The speed branch already keeps RUN speed in that case, but the LEGS statemap
    // still saw BUTTON_RUN clear and picked the WALK anims - run speed + walk animation = skating,
    // worst right after stamina runs out mid-sprint (user report). With the sprint system enabled,
    // re-assert BUTTON_RUN for everything downstream (statemap RUN conditions + ClientMove) unless
    // the dedicated Alt walk key is held (that one really means walk). TickSprint above already read
    // the raw ucmd, so sprint detection is unaffected; last_ucmd is re-copied fresh every frame.
    {
        static cvar_t *pSprintRunBtn = NULL;
        if (!pSprintRunBtn) { pSprintRunBtn = gi.Cvar_Get("coop_sprint", "1", CVAR_ARCHIVE); }
        if (pSprintRunBtn->integer && !(last_ucmd.buttons & BUTTON_RUN)
            && !(last_ucmd.buttons & BUTTON_COOPWALK)) {
            last_ucmd.buttons |= BUTTON_RUN;
        }
    }

    TickCoopCover(); // HZM coop - take cover [214]: validate the pose with this frame's traces
    // HZM coop [user 2026-08-27] BRACING ticks HERE, immediately after cover, because scheme C
    // consumes THIS frame's m_bCoopCoverPeek - the tick-order trap already documented above.
    TickCoopBrace();
    // [vet] and ASSERT the aim state, rather than overriding one client predicate. Mounting forced
    // ADS by overriding CG_AimingDownSights alone, while every other consumer on both sides still
    // read the raw button - so the server computed a hip-fire cone while the player was looking down
    // the sights. Setting the bit here makes one decision drive all of them, the same way the sprint
    // block already re-asserts BUTTON_RUN.
    if (m_bCoopBraceMounted && current_ucmd) {
        current_ucmd->buttons |= BUTTON_COOPADS;
    }
    // [user 2026-08-07] Nav recorder ticks HERE, not inside TickCoopCover - that function returns
    // early whenever cover is not requested, so hanging the recorder off its tail meant nodes only
    // dropped while hugging a wall. Sixteen minutes of walking produced one node. Same early-return
    // trap that broke the cover-state mirror earlier today.
    TickCoopNavRec();
    TickCoopLobbyInput(); // HZM coop - lobby A/D/F input (no binds) -> self.coop_lobbyInput
    TickCoopLobbyCursor(); // HZM coop - lobby mouse cursor (no binds) -> self.coop_lobbyCurX/Y + coop_lobbyClick
    // HZM coop [221] - bug-309 GUNNERPROBE: once/sec truth table of every candidate manning
    // signal while any is live (or the player is entity-attached, e.g. script-seated gunner).
    if (level.time - m_fCoopProbeTime > 1.0f
        && (m_pTurret || m_pVehicle || edict->s.parent != ENTITYNUM_NONE
            || (client->ps.pm_flags & PMF_TURRET))) {
        m_fCoopProbeTime = level.time;
        gi.Printf(
            "^~^~^ GUNNERPROBE pTur=%d pVeh=%d parent=%d pmTURRET=%d legs='%s' torso='%s' tAnim='%s'\n",
            m_pTurret ? 1 : 0, m_pVehicle ? 1 : 0, edict->s.parent,
            (client->ps.pm_flags & PMF_TURRET) ? 1 : 0,
            currentState_Legs ? currentState_Legs->getName() : "?",
            currentState_Torso ? currentState_Torso->getName() : "?",
            partAnim[torso].c_str());
    }


    // HZM coop [219] - manning diagnostic (bug-309): neither TurretMove nor VehicleMove runs for
    // the jeep .30cal gunner (remote-control path) - watch the legs state from HERE instead
    if (m_pTurret || (level.time - m_fCoopVehTurretTime) < 0.25f) {
        static str sLastManLegs;

        if (currentState_Legs && sLastManLegs != currentState_Legs->getName()) {
            sLastManLegs = currentState_Legs->getName();
            gi.Printf("^~^~^ MANSTATE legs='%s'\n", sLastManLegs.c_str());
        }
    }

    if (g_gametype->integer != GT_SINGLE_PLAYER && dm_team == TEAM_SPECTATOR && !IsSpectator()) {
        Spectator();
    }

    // HZM 2026-08-11 (bug-1712): mask the spawn click out until it is genuinely released. Done
    // HERE, above the edge computation, so every consumer downstream - new_buttons,
    // server_new_buttons, buttons, last_ucmd and the weapon's own held-fire test - sees the same
    // thing. One release clears it for good; it costs nothing after that.
    if (m_bFireLockUntilRelease) {
        if (!(current_ucmd->buttons & (BUTTON_ATTACKLEFT | BUTTON_ATTACKRIGHT))) {
            m_bFireLockUntilRelease = false;
        } else {
            current_ucmd->buttons &= ~(BUTTON_ATTACKLEFT | BUTTON_ATTACKRIGHT);
        }
    }

    last_ucmd = *current_ucmd;

    new_buttons = current_ucmd->buttons & ~buttons;
    if (new_buttons & G_GetWeaponCommandMask()) {
        // Fixed in OPM
        //  There can't be multiple weapon commands
        //  So clear the weapon commands and use the latest one
        server_new_buttons &= ~G_GetWeaponCommandMask();
    }
    server_new_buttons |= current_ucmd->buttons & ~buttons;
    buttons = current_ucmd->buttons;

    if (camera) {
        m_vViewPos = camera->origin;
        m_vViewAng = camera->angles;
    } else {
        m_vViewPos[0] = (float)current_eyeinfo->ofs[0] + origin[0];
        m_vViewPos[1] = (float)current_eyeinfo->ofs[1] + origin[1];
        m_vViewPos[2] = (float)current_eyeinfo->ofs[2] + origin[2];

        m_vViewAng[0] = current_eyeinfo->angles[0];
        m_vViewAng[1] = current_eyeinfo->angles[1];
        m_vViewAng[2] = 0.0f;
    }

    VectorCopy(m_vViewPos, client->ps.vEyePos);

    if (!level.intermissiontime) {
        if (new_buttons & BUTTON_ATTACKRIGHT) {
            Weapon *weapon = GetActiveWeapon(WEAPON_MAIN);

            if (weapon && (weapon->GetZoom())) {
                ToggleZoom(weapon->GetZoom());
            }
        }

        // HZM coop - scoped weapons (snipers) zoom on the ADS button (RMB / BUTTON_COOPADS) too. ADS was
        // decoupled from secondary-fire (which moved to V), but a sniper's "ADS" IS its zoom, so RMB must
        // still scope it. Only zoom weapons respond (iron-sight guns have GetZoom()==0 and ADS via statemap).
        if (new_buttons & BUTTON_COOPADS) {
            Weapon *zw = GetActiveWeapon(WEAPON_MAIN);

            if (zw && (zw->GetZoom())) {
                ToggleZoom(zw->GetZoom());
            }
        }

        if (new_buttons & BUTTON_USE) {
            DoUse(NULL);
        }

        moveresult = MOVERESULT_NONE;

        if (m_pTurret) {
            TurretMove(current_ucmd);
        } else if (m_pVehicle) {
            VehicleMove(current_ucmd);
        } else {
            ClientMove(current_ucmd);
        }

        // Save cmd angles so that we can get delta angle movements next frame
        client->cmd_angles[0] = SHORT2ANGLE(current_ucmd->angles[0]);
        client->cmd_angles[1] = SHORT2ANGLE(current_ucmd->angles[1]);
        client->cmd_angles[2] = SHORT2ANGLE(current_ucmd->angles[2]);

        if (g_gametype->integer != GT_SINGLE_PLAYER && g_smoothClients->integer) {
            VectorCopy(client->ps.velocity, edict->s.pos.trDelta);
            edict->s.pos.trTime = client->ps.commandTime;
        } else {
            VectorClear(edict->s.pos.trDelta);
            edict->s.pos.trTime = 0;
        }

        ClientInactivityTimer();
    } else {
        if (g_gametype->integer != GT_SINGLE_PLAYER) {
            client->ps.pm_flags |= PMF_FROZEN;
            client->ps.pm_flags |= PMF_INTERMISSION;
            VectorClear(client->ps.velocity);

            if (level.time - level.intermissiontime > 5.0f
                && (new_buttons & (BUTTON_ATTACKLEFT | BUTTON_ATTACKRIGHT))) {
                level.exitintermission = true;
            }
        } else {
            if (level.intermissiontype == TRANS_MISSION_FAILED || IsDead()) {
                gi.cvar_set("g_success", "0");
                gi.cvar_set("g_failed", "1");
            } else {
                gi.cvar_set("g_success", "1");
                gi.cvar_set("g_failed", "0");
            }

            // prevent getting medals from cheats
            if (g_medal0->modificationCount > 1 || g_medal1->modificationCount > 1 || g_medal2->modificationCount > 1
                || g_medal3->modificationCount > 1 || g_medal4->modificationCount > 1 || g_medal5->modificationCount > 1
                || g_medalbt1->modificationCount > 1 || g_medalbt2->modificationCount > 1
                || g_medalbt3->modificationCount > 1 || g_medalbt4->modificationCount > 1
                || g_medalbt5->modificationCount > 1 || g_eogmedal0->modificationCount > 1
                || g_eogmedal1->modificationCount > 1 || g_eogmedal2->modificationCount > 1) {
                gi.cvar_set("g_gotmedal", "1");
            } else {
                gi.cvar_set("g_gotmedal", "0");
            }

            client->ps.pm_flags |= PMF_FROZEN;
            VectorClear(client->ps.velocity);

            if (level.time - level.intermissiontime > 4.0f) {
                if (level.intermissiontype) {
                    if (level.intermissiontype == TRANS_MISSION_FAILED) {
                        if ((new_buttons & BUTTON_ATTACKLEFT) || (new_buttons & BUTTON_ATTACKRIGHT)) {
                            G_MissionFailed();
                        }
                    } else if ((new_buttons & BUTTON_ATTACKLEFT) || (new_buttons & BUTTON_ATTACKRIGHT)) {
                        g_medal0->modificationCount    = 1;
                        g_medal1->modificationCount    = 1;
                        g_medal2->modificationCount    = 1;
                        g_medal3->modificationCount    = 1;
                        g_medal4->modificationCount    = 1;
                        g_medal5->modificationCount    = 1;
                        g_medalbt0->modificationCount  = 1;
                        g_medalbt1->modificationCount  = 1;
                        g_medalbt2->modificationCount  = 1;
                        g_medalbt3->modificationCount  = 1;
                        g_medalbt4->modificationCount  = 1;
                        g_medalbt5->modificationCount  = 1;
                        g_eogmedal0->modificationCount = 1;
                        g_eogmedal1->modificationCount = 1;
                        g_eogmedal2->modificationCount = 1;
                        g_medal0->modified             = false;
                        g_medal1->modified             = false;
                        g_medal2->modified             = false;
                        g_medal3->modified             = false;
                        g_medal4->modified             = false;
                        g_medal5->modified             = false;
                        g_eogmedal0->modified          = false;
                        g_eogmedal1->modified          = false;
                        g_eogmedal2->modified          = false;

                        level.exitintermission = true;
                    }
                } else {
                    level.exitintermission = true;
                }
            }
        }

        // Save cmd angles so that we can get delta angle movements next frame
        client->cmd_angles[0]  = SHORT2ANGLE(current_ucmd->angles[0]);
        client->cmd_angles[1]  = SHORT2ANGLE(current_ucmd->angles[1]);
        client->cmd_angles[2]  = SHORT2ANGLE(current_ucmd->angles[2]);
        client->ps.commandTime = current_ucmd->serverTime;
    }
}

void Player::Think(void)
{
    static cvar_t *g_aimLagTime = NULL;

    int     m_iClientWeaponCommand;
    Event  *m_pWeaponCommand = NULL;
    Weapon *pWeap;

    if (whereami->integer && origin != oldorigin) {
        gi.DPrintf("x %8.2f y %8.2f z %8.2f area %2d\n", origin[0], origin[1], origin[2], edict->r.areanum);
    }


    if (g_gametype->integer == GT_SINGLE_PLAYER && g_playermodel->modified) {
        setModel("models/player/" + str(g_playermodel->string) + ".tik");

        if (!edict->tiki) {
            setModel("models/player/american_army.tik");
        }

        g_playermodel->modified = qfalse;
    }

    // HZM bug-1638 THE ROOT OF THE m2l2a STEALTH SAGA: this maintenance ran ONLY in SP, so in
    // coop m_bIsDisguised froze at whatever the last changeGameType window computed - grant the
    // disguise with a rifle in hand and you are permanently "not disguised"; holstering never
    // helps because nothing recomputes. Run it in EVERY gametype (g_coopDisgParity 0 reverts
    // live): holster = disguised, draw = blown, per frame per player - retail semantics. The
    // guards' whole reaction suite (challenges, disguise levels, see-through watchers, alarm
    // cascade) is engine-native and simply works once this flag is alive.
    static cvar_t *s_coopDisgParity = NULL;
    if (!s_coopDisgParity) {
        s_coopDisgParity = gi.Cvar_Get("g_coopDisgParity", "1", 0);
    }
    if (g_gametype->integer == GT_SINGLE_PLAYER || s_coopDisgParity->integer) {
        m_bIsDisguised = false;

        // HZM [user 2026-08-12] E3 - ONCE BLOWN, STAYS BLOWN (user decision).
        // Vanilla asks only whether the alarm is up RIGHT NOW. m2l2a can get away with that because
        // its alarm is one-way. m6l2a's is a TOGGLE: threat_condition_delta sets alarm_always_on
        // with a 6-10s re-ring and the switch can be turned back off, so cover would flicker on and
        // off for the rest of the mission - the player is disguised, then not, then is again, with
        // nothing they did causing it. m6l2a.scr:1095 already carries a comment about that.
        // The latch is set in Level::SetAlarm and cleared only by Level::Init, i.e. by loading a map.
        // Guarded on m_bStealthNative so this reads exactly as vanilla everywhere else.
        if (m_bHasDisguise && !level.m_bAlarm && !(level.m_bStealthNative && level.m_bAlarmLatched)) {
            pWeap = GetActiveWeapon(WEAPON_MAIN);

            if (!pWeap || pWeap->IsSubclassOfInventoryItem()) {
                m_bIsDisguised = true;

                for (Sentient *pSent = level.m_HeadSentient[0]; pSent != NULL; pSent = pSent->m_NextSentient) {
                    Actor *act = (Actor *)pSent;

                    // HZM bug-1631 ROOT: this veto is vanilla-correct when an actor is genuinely
                    // engaging (shooting = blown), but in coop the actorenemy retention keeps
                    // harmless ZERO-THREAT actors parked in attack thinkstate against a disguised
                    // player indefinitely. Every changeGameType-0 window then ran this scan, found
                    // one, and flipped m_bIsDisguised false until the next window - which is what
                    // oscillated the papers-checker (CheckEnemies drops a non-disguised zero-threat
                    // enemy before the retention clause) and made guards look suspicious. Require
                    // real threat, which is a no-op for genuine SP attacks.
                    if (pSent->m_Enemy == this && act->IsAttacking()
                        && act->m_PotentialEnemies.GetCurrentThreat() > 0) {
                        m_bIsDisguised = false;
                        break;
                    }
                }
            }
        }
    }

    // scope deliberately narrowed to the disguise flag: cover-map feed and player enemy
    // bookkeeping stay SP-only, exactly as before
    if (g_gametype->integer == GT_SINGLE_PLAYER) {
        PathSearch::PlayerCover(this);
        UpdateEnemies();
    }

    // HZM [user 2026-08-12] FIRE PROBE. Six rounds of script-side patching failed to explain
    // "pistol out, will not shoot, then the gun with papers stuck inside". Script can only see the
    // mod's OWN bookkeeping (coop_activeWeapon and friends); it cannot see what the engine actually
    // holds, whether that thing is an InventoryItem (papers) rather than a Weapon, whether the clip
    // is empty, or whether the fire button is reaching the weapon at all. Those facts decide this.
    // Off unless g_coopFireProbe is 1; prints on change, or every frame while fire is held.
    {
        static cvar_t *s_coopFireProbe = NULL;
        if (!s_coopFireProbe) {
            s_coopFireProbe = gi.Cvar_Get("g_coopFireProbe", "0", 0);
        }
        if (s_coopFireProbe->integer) {
            Weapon     *pw    = GetActiveWeapon(WEAPON_MAIN);
            // `buttons` is the Player's own per-frame button state; current_ucmd is only valid
            // during client movement processing and is NULL here, which silently skipped the
            // whole probe for two sessions.
            int         held  = (buttons & (BUTTON_ATTACKLEFT | BUTTON_ATTACKRIGHT)) ? 1 : 0;
            const char *wname = pw ? pw->getName().c_str() : "(none)";
            int         isItm = (pw && pw->IsSubclassOfInventoryItem()) ? 1 : 0;
            int         clip  = pw ? pw->ClipAmmo(FIRE_PRIMARY) : -1;
            char        line[512];
            Com_sprintf(line, sizeof(line),
                        "^~^~^ FIREPROBE hand=%s item=%d clip=%d held=%d firelock=%d disg=%d\n",
                        wname, isItm, clip, held, m_bFireLockUntilRelease ? 1 : 0,
                        m_bIsDisguised ? 1 : 0);
            if (held || Q_stricmp(line, m_szCoopFireLast)) {
                Q_strncpyz(m_szCoopFireLast, line, sizeof(m_szCoopFireLast));
                gi.Printf("%s", line);
            }
        }
    }

    if (movetype == MOVETYPE_NOCLIP) {
        StopPartAnimating(torso);
        SetPartAnim("idle");

        client->ps.walking = qfalse;
        groundentity       = 0;
    } else {
        CheckMoveFlags();
        EvaluateState();
    }

    oldvelocity = velocity;
    old_v_angle = v_angle;

    if (g_gametype->integer == GT_SINGLE_PLAYER) {
        if ((server_new_buttons & BUTTON_ATTACKLEFT) && (!GetActiveWeapon(WEAPON_MAIN)) && !IsDead()
            && !IsNewActiveWeapon() && !LoadingSavegame) {
            Event *ev = new Event("useweaponclass");
            ev->AddString("item1");

            ProcessEvent(ev);
        }
    } else {
        // Added in 2.0: Invulnerability and team spawn
        TickInvulnerable();
        TickTeamSpawn();

        if (deadflag == DEAD_DEAD && level.time > respawn_time) {
            if (dmManager.AllowRespawn() && AllowTeamRespawn()) {
                if (((server_new_buttons & BUTTON_ATTACKLEFT) || (server_new_buttons & BUTTON_ATTACKRIGHT))
                    || (g_forcerespawn->integer > 0 && level.time > g_forcerespawn->integer + respawn_time)) {
                    // 2.0
                    //  Check for team respawn
                    //
                    if (!m_fSpawnTimeLeft) {
                        if (AllowTeamRespawn()) {
                            EndSpectator();
                            PostEvent(EV_Player_Respawn, 0);
                        }
                    } else {
                        m_bWaitingForRespawn = true;
                    }
                }
            } else if (!IsSpectator()) {
                BeginTempSpectator();
            }
        }

        if (IsSpectator()) {
            if (!m_bTempSpectator && level.time > respawn_time && (server_new_buttons & BUTTON_ATTACKLEFT)) {
                if (current_team && dm_team != TEAM_SPECTATOR) {
                    if (client->pers.dm_primary[0]) {
                        if ((g_gametype->integer == GT_FFA
                             || (g_gametype->integer >= GT_TEAM && dm_team > TEAM_FREEFORALL))
                            && deadflag != DEAD_DEAD) {
                            // 2.0
                            //  Check for team respawn
                            //
                            if (!m_fSpawnTimeLeft) {
                                if (AllowTeamRespawn()) {
                                    EndSpectator();
                                    PostEvent(EV_Player_Respawn, 0);
                                }
                            } else {
                                m_bWaitingForRespawn = true;
                            }
                        }
                    } else if (m_fWeapSelectTime < level.time) {
                        m_fWeapSelectTime = level.time + 1.0;
                        UserSelectWeapon(false);
                    }
                } else if (m_fWeapSelectTime < level.time) {
                    m_fWeapSelectTime = level.time + 1.0;
                    gi.SendServerCommand(edict - g_entities, "stufftext \"pushmenu_teamselect\"");
                }
            }
            // Removed in 2.0
            //else if (level.time > m_fWeapSelectTime + 10.0) {
            //    m_fWeapSelectTime = level.time;
            //    gi.centerprintf(edict, "\n\n\n%s", gi.LV_ConvertString("Press fire to join the battle!"));
            //}
        } else if (!client->pers.dm_primary[0]) {
            Spectator();
            if (m_fWeapSelectTime < level.time) {
                m_fWeapSelectTime = level.time + 1.0;
                UserSelectWeapon(false);
            }
        }

        if (IsSpectator()) {
            if (g_protocol >= PROTOCOL_MOHTA_MIN) {
                if (m_iPlayerSpectating) {
                    if (last_ucmd.upmove) {
                        if (!m_bSpectatorSwitching) {
                            m_bSpectatorSwitching = true;

                            if (last_ucmd.upmove > 0) {
                                SetPlayerSpectate(true);
                            } else {
                                SetPlayerSpectate(false);
                            }
                        }
                    } else {
                        m_bSpectatorSwitching = false;
                    }
                } else if ((server_new_buttons & BUTTON_USE)) {
                    SetPlayerSpectateRandom();
                    server_new_buttons &= ~BUTTON_USE;
                }
            } else {
                if ((server_new_buttons & BUTTON_USE)) {
                    SetPlayerSpectate(true);
                }
            }

            if (g_gametype->integer >= GT_TEAM && g_forceteamspectate->integer && GetTeam() > TEAM_FREEFORALL) {
                if (!m_iPlayerSpectating) {
                    SetPlayerSpectateRandom();
                } else {
                    gentity_t *ent = g_entities + m_iPlayerSpectating - 1;

                    if (!ent->inuse || !ent->entity) {
                        // Invalid spectate entity
                        SetPlayerSpectateRandom();
                    } else if (ent->entity->deadflag >= DEAD_DEAD || static_cast<Player *>(ent->entity)->IsSpectator()
                               || !IsValidSpectatePlayer(static_cast<Player *>(ent->entity))) {
                        SetPlayerSpectateRandom();
                    }
                }
            } else {
                if (g_protocol >= protocol_e::PROTOCOL_MOHTA_MIN) {
                    // Since 2.0, use = clear spectator
                    if (m_iPlayerSpectating && (server_new_buttons & BUTTON_USE)) {
                        m_iPlayerSpectating = 0;
                    }
                } else {
                    // On 1.11 and below, up = clear spectator
                    if (last_ucmd.upmove) {
                        m_iPlayerSpectating = 0;
                    }
                }

                if (m_iPlayerSpectating) {
                    gentity_t *ent = g_entities + m_iPlayerSpectating - 1;

                    if (!ent->inuse || !ent->entity) {
                        // Invalid spectate entity
                        SetPlayerSpectateRandom();
                    } else if (ent->entity->deadflag >= DEAD_DEAD || static_cast<Player *>(ent->entity)->IsSpectator()
                               || !IsValidSpectatePlayer(static_cast<Player *>(ent->entity))) {
                        SetPlayerSpectateRandom();
                    } else if (g_gametype->integer >= GT_TEAM && GetTeam() > TEAM_FREEFORALL
                               && static_cast<Player *>(ent->entity)->GetTeam() != GetTeam()) {
                        SetPlayerSpectateRandom();
                    }
                }
            }
        } else {
            m_iPlayerSpectating = 0;
        }
    }

    if (g_logstats->integer) {
        if (!logfile_started) {
            ProcessEvent(EV_Player_LogStats);
            logfile_started = qtrue;
        }
    }

    if (!IsDead()) {
        m_iClientWeaponCommand = G_GetWeaponCommand(server_new_buttons);

        switch (m_iClientWeaponCommand) {
        case 0:
            // No command
            break;
        case WEAPON_COMMAND_USE_PISTOL:
            m_pWeaponCommand = new Event(EV_Sentient_UseWeaponClass);
            m_pWeaponCommand->AddString("pistol");
            break;
        case WEAPON_COMMAND_USE_RIFLE:
            m_pWeaponCommand = new Event(EV_Sentient_UseWeaponClass);
            m_pWeaponCommand->AddString("rifle");
            break;
        case WEAPON_COMMAND_USE_SMG:
            m_pWeaponCommand = new Event(EV_Sentient_UseWeaponClass);
            m_pWeaponCommand->AddString("smg");
            break;
        case WEAPON_COMMAND_USE_MG:
            m_pWeaponCommand = new Event(EV_Sentient_UseWeaponClass);
            m_pWeaponCommand->AddString("mg");
            break;
        case WEAPON_COMMAND_USE_GRENADE:
            m_pWeaponCommand = new Event(EV_Sentient_UseWeaponClass);
            m_pWeaponCommand->AddString("grenade");
            break;
        case WEAPON_COMMAND_USE_HEAVY:
            m_pWeaponCommand = new Event(EV_Sentient_UseWeaponClass);
            m_pWeaponCommand->AddString("heavy");
            break;
        case WEAPON_COMMAND_USE_ITEM1:
            m_pWeaponCommand = new Event(EV_Sentient_ToggleItemUse);
            break;
        case WEAPON_COMMAND_USE_ITEM2:
            m_pWeaponCommand = new Event(EV_Sentient_UseWeaponClass);
            m_pWeaponCommand->AddString("item2");
            break;
        case WEAPON_COMMAND_USE_ITEM3:
            m_pWeaponCommand = new Event(EV_Sentient_UseWeaponClass);
            m_pWeaponCommand->AddString("item3");
            break;
        case WEAPON_COMMAND_USE_ITEM4:
            m_pWeaponCommand = new Event(EV_Sentient_UseWeaponClass);
            m_pWeaponCommand->AddString("item4");
            break;
        case WEAPON_COMMAND_USE_PREV_WEAPON:
            m_pWeaponCommand = new Event(EV_Player_PrevWeapon);
            break;
        case WEAPON_COMMAND_USE_NEXT_WEAPON:
            m_pWeaponCommand = new Event(EV_Player_NextWeapon);
            break;
        case WEAPON_COMMAND_USE_LAST_WEAPON:
            m_pWeaponCommand = new Event(EV_Sentient_UseLastWeapon);
            break;
        case WEAPON_COMMAND_HOLSTER:
            m_pWeaponCommand = new Event(EV_Player_Holster);
            break;
        case WEAPON_COMMAND_DROP:
            m_pWeaponCommand = new Event(EV_Player_DropWeapon);
            break;
        default:
            gi.DPrintf("Unrecognized weapon command %d\n", m_iClientWeaponCommand);
        }

        if (m_pWeaponCommand) {
            PostEvent(m_pWeaponCommand, 0);
        }
    }

    if (g_gametype->integer == GT_SINGLE_PLAYER) {
        if (!g_aimLagTime) {
            g_aimLagTime = gi.Cvar_Get("g_aimLagTime", "250", 0);
        }

        if (mLastTrailTime + g_aimLagTime->integer < level.inttime) {
            mLastTrailTime = level.inttime;

            mvTrail[0]        = centroid;
            mvTrailEyes[0]    = centroid;
            mvTrailEyes[0][0] = EyePosition()[0];
        }
    }
    UpdateFootsteps();

    //
    // Added in 2.0
    // Heal rate
    //
    if (m_fHealRate && !IsDead()) {
        float newrate;

        if (g_healrate->value && g_gametype->integer != GT_SINGLE_PLAYER) {
            newrate = max_health * (g_healrate->value / 100.f) * level.frametime;
            if (newrate >= m_fHealRate) {
                newrate     = m_fHealRate;
                m_fHealRate = 0;
            } else {
                m_fHealRate -= newrate;
            }
        } else {
            newrate     = m_fHealRate;
            m_fHealRate = 0;
        }

        // heal
        health += newrate;
        if (health > max_health) {
            // make sure it doesn't go above the maximum player health
            health = max_health;
        }
    }

    //
    // Added in 2.0: talk icon
    //
    if (buttons & BUTTON_TALK) {
        edict->s.eFlags |= EF_PLAYER_IN_MENU;
    } else {
        edict->s.eFlags &= ~EF_PLAYER_IN_MENU;
    }

    if (m_fTalkTime > level.time) {
        edict->s.eFlags |= EF_PLAYER_TALKING;
    } else {
        edict->s.eFlags &= ~EF_PLAYER_TALKING;
    }

    //
    // Added in OPM
    //
    ThinkFPS();

    server_new_buttons = 0;

    edict->r.svFlags &= ~(SVF_SINGLECLIENT | SVF_NOTSINGLECLIENT);
}

void Player::InitLegsStateTable(void)
{
    animdone_Legs     = false;
    currentState_Legs = statemap_Legs->FindState("STAND");

    str legsAnim(currentState_Legs->getLegAnim(*this, &legs_conditionals));
    if (legsAnim == "") {
        StopPartAnimating(legs);
    } else if (legsAnim != "none") {
        SetPartAnim(legsAnim.c_str(), legs);
    }
}

void Player::InitTorsoStateTable(void)
{
    animdone_Torso = false;

    currentState_Torso = statemap_Torso->FindState("STAND");

    str torsoAnim(currentState_Torso->getActionAnim(*this, &torso_conditionals));
    if (torsoAnim == "") {
        StopPartAnimating(torso);
    } else if (torsoAnim != "none") {
        SetPartAnim(torsoAnim.c_str(), torso);
    }
}

void Player::LoadStateTable(void)
{
    int          i;
    Conditional *cond;

    statemap_Legs  = NULL;
    statemap_Torso = NULL;

    //
    // Free existing conditionals
    //
    for (i = legs_conditionals.NumObjects(); i > 0; i--) {
        cond = legs_conditionals.ObjectAt(i);
        delete cond;
    }
    legs_conditionals.FreeObjectList();

    for (i = torso_conditionals.NumObjects(); i > 0; i--) {
        cond = torso_conditionals.ObjectAt(i);
        delete cond;
    }
    torso_conditionals.FreeObjectList();

    statemap_Legs =
        GetStatemap(str(g_statefile->string) + "_Legs.st", (Condition<Class> *)m_conditions, &legs_conditionals, false);
    statemap_Torso = GetStatemap(
        str(g_statefile->string) + "_Torso.st", (Condition<Class> *)m_conditions, &torso_conditionals, false
    );

    movecontrol = MOVECONTROL_LEGS;

    InitLegsStateTable();
    InitTorsoStateTable();

    movecontrol = currentState_Legs->getMoveType();
    if (!movecontrol) {
        movecontrol = MOVECONTROL_LEGS;
    }

    for (int i = 1; i <= legs_conditionals.NumObjects(); i++) {
        Conditional *c = legs_conditionals.ObjectAt(i);

        if (Q_stricmp(c->getName(), "PAIN") && !c->parmList.NumObjects()) {
            m_pLegsPainCond = c;
            break;
        }
    }

    for (int i = 1; i <= torso_conditionals.NumObjects(); i++) {
        Conditional *c = torso_conditionals.ObjectAt(i);

        if (Q_stricmp(c->getName(), "PAIN") && !c->parmList.NumObjects()) {
            m_pTorsoPainCond = c;
            break;
        }
    }

    if ((movecontrol < (sizeof(MoveStartFuncs) / sizeof(MoveStartFuncs[0]))) && (MoveStartFuncs[movecontrol])) {
        (this->*MoveStartFuncs[movecontrol])();
    }

    SetViewAngles(v_angle);
}

void Player::ResetState(Event *ev)
{
    movecontrol = MOVECONTROL_LEGS;
    LoadStateTable();
}

void Player::StartPush(void)
{
    trace_t trace;
    Vector  end(origin + yaw_forward * 64.0f);

    trace = G_Trace(origin, mins, maxs, end, this, MASK_SOLID, true, "StartPush");
    if (trace.fraction == 1.0f) {
        return;
    }
    v_angle.y = vectoyaw(trace.plane.normal) - 180;
    SetViewAngles(v_angle);

    setOrigin(trace.endpos - yaw_forward * 0.4f);
}

void Player::StartClimbLadder(void)
{
    trace_t trace;
    Vector  end(origin + yaw_forward * 20.0f);

    trace = G_Trace(origin, mins, maxs, end, this, MASK_SOLID, true, "StartClimbLadder");
    if ((trace.fraction == 1.0f) || !(trace.surfaceFlags & SURF_LADDER)) {
        return;
    }

    v_angle.y = vectoyaw(trace.plane.normal) - 180;
    SetViewAngles(v_angle);

    setOrigin(trace.endpos - yaw_forward * 0.4f);
}

void Player::StartUseAnim(void)
{
    UseAnim *ua;
    Vector   neworg;
    Vector   newangles;
    str      newanim;
    str      state;
    str      camera;
    trace_t  trace;

    if (toucheduseanim) {
        ua = (UseAnim *)(Entity *)toucheduseanim;
    } else if (atobject) {
        ua = (UseAnim *)(Entity *)atobject;
    } else {
        return;
    }

    useitem_in_use = ua;
    toucheduseanim = NULL;
    atobject       = NULL;

    if (ua->GetInformation(this, &neworg, &newangles, &newanim, &useanim_numloops, &state, &camera)) {
        trace = G_Trace(origin, mins, maxs, neworg, this, MASK_PLAYERSOLID, true, "StartUseAnim");
        if (trace.startsolid || (trace.fraction < 1.0f)) {
            gi.DPrintf("Move to UseAnim was blocked.\n");
        }

        if (!trace.startsolid) {
            setOrigin(trace.endpos);
        }

        setAngles(newangles);
        v_angle.y = newangles.y;
        SetViewAngles(v_angle);

        movecontrol = MOVECONTROL_ABSOLUTE;

        if (state.length()) {
            State *newState;

            newState = statemap_Torso->FindState(state);
            if (newState) {
                EvaluateState(newState);
            } else {
                gi.DPrintf("Could not find state %s on UseAnim\n", state.c_str());
            }
        } else {
            if (currentState_Torso) {
                if (camera.length()) {
                    currentState_Torso->setCameraType(camera);
                } else {
                    currentState_Torso->setCameraType("behind");
                }
            }
            SetPartAnim(newanim, legs);
        }
    }
}

void Player::StartLoopUseAnim(void)
{
    useanim_numloops--;
}

void Player::FinishUseAnim(Event *ev)
{
    UseAnim *ua;

    if (!useitem_in_use) {
        return;
    }

    ua = (UseAnim *)(Entity *)useitem_in_use;
    ua->TriggerTargets(this);
    useitem_in_use = NULL;
}

void Player::SetupUseObject(void)
{
    UseObject *uo;
    Vector     neworg;
    Vector     newangles;
    str        state;
    trace_t    trace;

    if (atobject) {
        uo = (UseObject *)(Entity *)atobject;
    } else {
        return;
    }

    useitem_in_use = uo;

    uo->Setup(this, &neworg, &newangles, &state);
    {
        trace = G_Trace(neworg, mins, maxs, neworg, this, MASK_PLAYERSOLID, true, "SetupUseObject - 1");
        if (trace.startsolid || trace.allsolid) {
            trace = G_Trace(origin, mins, maxs, neworg, this, MASK_PLAYERSOLID, true, "SetupUseObject - 2");
            if (trace.startsolid || (trace.fraction < 1.0f)) {
                gi.DPrintf("Move to UseObject was blocked.\n");
            }
        }

        if (!trace.startsolid) {
            setOrigin(trace.endpos);
        }

        setAngles(newangles);
        v_angle.y = newangles.y;
        SetViewAngles(v_angle);

        movecontrol = MOVECONTROL_ABSOLUTE;

        if (state.length()) {
            State *newState;

            newState = statemap_Torso->FindState(state);
            if (newState) {
                EvaluateState(newState);
            } else {
                gi.DPrintf("Could not find state %s on UseObject\n", state.c_str());
            }
        }
    }
}

void Player::StartUseObject(Event *ev)
{
    UseObject *uo;

    if (!useitem_in_use) {
        return;
    }

    uo = (UseObject *)(Entity *)useitem_in_use;
    uo->Start();
}

void Player::FinishUseObject(Event *ev)
{
    UseObject *uo;

    if (!useitem_in_use) {
        return;
    }

    uo = (UseObject *)(Entity *)useitem_in_use;
    uo->Stop(this);
    useitem_in_use = NULL;
}

void Player::Turn(Event *ev)
{
    float  yaw;
    Vector oldang(v_angle);

    yaw = ev->GetFloat(1);

    v_angle[YAW] = (int)(anglemod(v_angle[YAW]) / 22.5f) * 22.5f;
    SetViewAngles(v_angle);

    if (!CheckMove(vec_zero)) {
        SetViewAngles(oldang);
        return;
    }

    CancelEventsOfType(EV_Player_TurnUpdate);

    ev = new Event(EV_Player_TurnUpdate);
    ev->AddFloat(yaw / 5.0f);
    ev->AddFloat(0.5f);
    ProcessEvent(ev);
}

void Player::TurnUpdate(Event *ev)
{
    float  yaw;
    float  timeleft;
    Vector oldang(v_angle);

    yaw      = ev->GetFloat(1);
    timeleft = ev->GetFloat(2);
    timeleft -= 0.1f;

    if (timeleft > 0) {
        ev = new Event(EV_Player_TurnUpdate);
        ev->AddFloat(yaw);
        ev->AddFloat(timeleft);
        PostEvent(ev, 0.1f);

        v_angle[YAW] += yaw;
        SetViewAngles(v_angle);
    } else {
        v_angle[YAW] = (int)(anglemod(v_angle[YAW]) / 22.5f) * 22.5f;
        SetViewAngles(v_angle);
    }

    if (!CheckMove(vec_zero)) {
        SetViewAngles(oldang);
    }
}

void Player::TurnLegs(Event *ev)
{
    float yaw;

    yaw = ev->GetFloat(1);

    angles[YAW] += yaw;
    setAngles(angles);
}

void Player::EvaluateState(State *forceTorso, State *forceLegs)
{
    int           count;
    State        *laststate_Legs;
    State        *laststate_Torso;
    State        *startstate_Legs;
    State        *startstate_Torso;
    movecontrol_t move;

    if (getMoveType() == MOVETYPE_NOCLIP) {
        return;
    }

    if (flags & FL_IMMOBILE) {
        // Don't evaluate state when immobile
        return;
    }

    if (getMoveType() == MOVETYPE_PORTABLE_TURRET) {
        // Added in 2.0
        //  Animations are handled hardcodedly
        currentState_Torso = NULL;
        currentState_Legs  = NULL;
        return;
    }

    // Evaluate the current state.
    // When the state changes, we reevaluate the state so that if the
    // conditions aren't met in the new state, we don't play one frame of
    // the animation for that state before going to the next state.
    startstate_Torso = laststate_Torso = currentState_Torso;
    count                              = 0;
    do {
        // since we could get into an infinite loop here, do a check
        // to make sure we don't.
        count++;
        if (count > 10) {
            gi.DPrintf("Possible infinite loop in state '%s'\n", currentState_Torso->getName());
            if (count > 20) {
                assert(0);
                gi.Error(ERR_DROP, "Stopping due to possible infinite state loop\n");
                break;
            }
        }

        laststate_Torso = currentState_Torso;

        if (currentState_Torso) {
            laststate_Torso = currentState_Torso;

            if (forceTorso) {
                currentState_Torso = forceTorso;
            } else {
                currentState_Torso = currentState_Torso->Evaluate(*this, &torso_conditionals);
            }

            // HZM coop [user 07-12] fire diagnostics: trace every torso state hop (coop_fireDebug 1)
            {
                static cvar_t *pFireDbg = NULL;
                if (!pFireDbg) { pFireDbg = gi.Cvar_Get("coop_fireDebug", "0", 0); }
                if (pFireDbg->integer && currentState_Torso != laststate_Torso) {
                    gi.Printf(
                        "^~^~^ FIREDBG TORSO %s -> %s\n",
                        laststate_Torso ? laststate_Torso->getName() : "(none)",
                        currentState_Torso ? currentState_Torso->getName() : "(none)"
                    );
                }
            }
        } else {
            // Added in 2.0
            //  Switch to the default torso state if it's NULL
            if (forceTorso) {
                currentState_Torso = forceTorso;
            } else {
                currentState_Torso = statemap_Torso->FindState("STAND");
            }

            laststate_Torso = NULL;
        }

        if (currentState_Torso) {
            if (laststate_Torso) {
                // Process exit commands of the last state
                laststate_Torso->ProcessExitCommands(this);
            }

            // Process entry commands of the new state
            currentState_Torso->ProcessEntryCommands(this);

            if (waitForState.length() && (!waitForState.icmpn(currentState_Torso->getName(), waitForState.length()))) {
                waitForState = "";
            }

            move = currentState_Torso->getMoveType();

            // use the current movecontrol
            if (move == MOVECONTROL_NONE) {
                move = movecontrol;
            }

            str legsAnim;
            str torsoAnim(currentState_Torso->getActionAnim(*this, &torso_conditionals));

            if (move == MOVECONTROL_LEGS) {
                if (!currentState_Legs) {
                    animdone_Legs     = false;
                    currentState_Legs = statemap_Legs->FindState("STAND");
                    legsAnim          = currentState_Legs->getLegAnim(*this, &legs_conditionals);

                    if (legsAnim == "") {
                        StopPartAnimating(legs);
                    } else if (legsAnim != "none") {
                        SetPartAnim(legsAnim.c_str(), legs);
                    }
                }

                if (torsoAnim == "none") {
                    StopPartAnimating(torso);
                    animdone_Torso = true;
                } else if (torsoAnim != "") {
                    if (torsoAnim == partAnim[torso]) {
                        // Fixed in OPM
                        //  Stop the part if it's the same animation
                        //  so the new animation can play and make some action
                        //  like activate the new weapon.
                        StopPartAnimating(torso);
                    }
                    SetPartAnim(torsoAnim.c_str(), torso);
                }
            } else {
                if (torsoAnim == "none") {
                    StopPartAnimating(torso);
                    animdone_Torso = true;
                } else if (torsoAnim != "") {
                    if (torsoAnim == partAnim[torso]) {
                        // Fixed in OPM
                        //  See above
                        StopPartAnimating(torso);
                    }
                    SetPartAnim(torsoAnim.c_str(), torso);
                }

                legsAnim = currentState_Torso->getLegAnim(*this, &torso_conditionals);

                if (legsAnim == "none" || legsAnim == "") {
                    StopPartAnimating(legs);
                } else {
                    SetPartAnim(legsAnim.c_str(), legs);
                }

                // Fixed in OPM
                //  Clear the legs state, so the torso state can reset it to STAND
                //  in subsequent iterations.
                //  As the legs animation is stopped, there would be no anim to wait on.
                //
                //  This prevents the current legs state to be stuck
                //  when the move control is set to non-legs and then to legs
                //  in the same iteration before the legs state is being processed.
                currentState_Legs = NULL;
            }

            if (movecontrol != move) {
                movecontrol = move;
                if ((move < (sizeof(MoveStartFuncs) / sizeof(MoveStartFuncs[0]))) && (MoveStartFuncs[move])) {
                    (this->*MoveStartFuncs[move])();
                }

                if (movecontrol == MOVECONTROL_CLIMBWALL) {
                    edict->s.eFlags |= EF_CLIMBWALL;
                } else {
                    edict->s.eFlags &= ~EF_CLIMBWALL;
                }
            }

            SetViewAngles(v_angle);
        } else {
            currentState_Torso = laststate_Torso;
        }
    } while (laststate_Torso != currentState_Torso);

    // Evaluate the current state.
    // When the state changes, we reevaluate the state so that if the
    // conditions aren't met in the new state, we don't play one frame of
    // the animation for that state before going to the next state.
    startstate_Legs = laststate_Legs = currentState_Legs;
    if (movecontrol == MOVECONTROL_LEGS) {
        count = 0;
        do {
            // since we could get into an infinite loop here, do a check
            // to make sure we don't.
            count++;
            if (count > 10) {
                gi.DPrintf("Possible infinite loop in state '%s'\n", currentState_Legs->getName());
                if (count > 20) {
                    assert(0);
                    gi.Error(ERR_DROP, "Stopping due to possible infinite state loop\n");
                    break;
                }
            }

            if (currentState_Legs) {
                laststate_Legs = currentState_Legs;

                if (forceLegs) {
                    currentState_Legs = forceLegs;
                } else {
                    currentState_Legs = currentState_Legs->Evaluate(*this, &legs_conditionals);
                }
            } else {
                // Added in 2.0
                //  Switch to the default legs state if it's NULL
                if (forceLegs) {
                    currentState_Legs = forceLegs;
                } else if ((m_iMovePosFlags & MPF_POSITION_CROUCHING)) {
                    currentState_Legs = statemap_Legs->FindState("CROUCH_IDLE");
                } else {
                    currentState_Legs = statemap_Legs->FindState("STAND");
                }

                laststate_Legs = NULL;
            }

            if (currentState_Legs) {
                if (laststate_Legs) {
                    // Process exit commands of the last state
                    laststate_Legs->ProcessExitCommands(this);
                }

                // Process entry commands of the new state
                currentState_Legs->ProcessEntryCommands(this);

                if (waitForState.length()
                    && (!waitForState.icmpn(currentState_Legs->getName(), waitForState.length()))) {
                    waitForState = "";
                }

                str legsAnim(currentState_Legs->getLegAnim(*this, &legs_conditionals));

                if (legsAnim == "none") {
                    StopPartAnimating(legs);
                    animdone_Legs = true;
                } else if (legsAnim != "") {
                    float oldTime;

                    if (currentState_Legs == laststate_Legs) {
                        //
                        // Added in OPM
                        //  This allows different animations in the same state
                        //  to be continued at the same moment.
                        //  This is used to avoid "ghost walking" where the player
                        //  would switch weapons indefinitely to avoid footstep sounds
                        //

                        oldTime = GetTime(m_iPartSlot[legs]);
                        SetPartAnim(legsAnim, legs);

                        if (animtimes[m_iPartSlot[legs]] > 0) {
                            SetTime(m_iPartSlot[legs], fmod(oldTime, animtimes[m_iPartSlot[legs]]));
                        }
                    } else {
                        SetPartAnim(legsAnim, legs);
                    }
                }
            } else {
                currentState_Legs = laststate_Legs;
            }
        } while (laststate_Legs != currentState_Legs);
    } else {
        currentState_Legs = NULL;
    }

    if (g_showplayeranim->integer) {
        str sNewAnim;

        sNewAnim = AnimName(m_iPartSlot[legs]);
        if (last_leg_anim_name != sNewAnim) {
            gi.DPrintf("Legs anim change from %s to %s\n", last_leg_anim_name.c_str(), sNewAnim.c_str());
            last_leg_anim_name = sNewAnim;
        }

        sNewAnim = AnimName(m_iPartSlot[torso]);
        if (last_torso_anim_name != sNewAnim) {
            gi.DPrintf("Torso anim change from %s to %s\n", last_torso_anim_name.c_str(), sNewAnim.c_str());
            last_torso_anim_name = sNewAnim;
        }
    }

    if (g_showplayerstate->integer) {
        if (startstate_Legs != currentState_Legs) {
            gi.DPrintf(
                "Legs state change from %s to %s\n",
                startstate_Legs ? startstate_Legs->getName() : "NULL",
                currentState_Legs ? currentState_Legs->getName() : "NULL"
            );
        }

        if (startstate_Torso != currentState_Torso) {
            gi.DPrintf(
                "Torso state change from %s to %s\n",
                startstate_Torso ? startstate_Torso->getName() : "NULL",
                currentState_Torso ? currentState_Torso->getName() : "NULL"
            );
        }
    }

    // This is so we don't remember pain when we change to a state that has a PAIN condition
    pain = 0;
}

void Player::SelectPreviousItem(Event *ev)
{
    if (deadflag) {
        return;
    }

    Item *item = GetActiveWeapon(WEAPON_MAIN);

    item = PrevItem(item);

    if (item) {
        useWeapon((Weapon *)item, WEAPON_MAIN);
    }
}

void Player::SelectNextItem(Event *ev)
{
    if (deadflag) {
        return;
    }

    Item *item = GetActiveWeapon(WEAPON_MAIN);

    item = NextItem(item);

    if (item) {
        useWeapon((Weapon *)item, WEAPON_MAIN);
    }
}

void Player::SelectPreviousWeapon(Event *ev)
{
    Weapon *weapon;
    Weapon *initialWeapon;
    Weapon *activeWeapon;

    if (deadflag) {
        return;
    }

    activeWeapon = GetActiveWeapon(WEAPON_MAIN);
    if (activeWeapon && activeWeapon->IsSubclassOfInventoryItem()) {
        activeWeapon = NULL;
    }

    if (!activeWeapon) {
        activeWeapon = newActiveWeapon.weapon;
        if (activeWeapon && activeWeapon->IsSubclassOfInventoryItem()) {
            activeWeapon = NULL;
        }
    }

    if (activeWeapon) {
        // Fixed in OPM
        //  Fixes the bug that cause infinite loop when the last weapon has no ammo
        //  and the only weapon is an inventory item
        for (weapon = initialWeapon = PreviousWeapon(activeWeapon); weapon && weapon != activeWeapon;) {
            if (g_gametype->integer == GT_SINGLE_PLAYER || !weapon->IsSubclassOfInventoryItem()) {
                break;
            }

            weapon = PreviousWeapon(weapon);
            if (weapon == initialWeapon) {
                break;
            }
        }
    } else {
        weapon = BestWeapon();
    }

    if (weapon && weapon != activeWeapon) {
        useWeapon(weapon);
    }

    if (deadflag) {
        return;
    }
}

void Player::SelectNextWeapon(Event *ev)
{
    Weapon *weapon;
    Weapon *initialWeapon;
    Weapon *activeWeapon;

    if (deadflag) {
        return;
    }

    activeWeapon = GetActiveWeapon(WEAPON_MAIN);
    if (activeWeapon && activeWeapon->IsSubclassOfInventoryItem()) {
        activeWeapon = NULL;
    }

    if (!activeWeapon) {
        activeWeapon = newActiveWeapon.weapon;
        if (activeWeapon && activeWeapon->IsSubclassOfInventoryItem()) {
            activeWeapon = NULL;
        }
    }

    if (activeWeapon) {
        // Fixed in OPM
        //  Fixes the bug that cause infinite loop when the last weapon has no ammo
        //  and the only weapon is an inventory item
        for (weapon = initialWeapon = NextWeapon(activeWeapon); weapon && weapon != activeWeapon;) {
            if (g_gametype->integer == GT_SINGLE_PLAYER || !weapon->IsSubclassOfInventoryItem()) {
                break;
            }

            weapon = NextWeapon(weapon);
            if (weapon == initialWeapon) {
                break;
            }
        }
    } else {
        weapon = WorstWeapon();
    }

    if (weapon && weapon != activeWeapon) {
        useWeapon(weapon);
    }
}

void Player::DropCurrentWeapon(Event *ev)
{
    Weapon *weapon;
    Vector  forward;

    if (g_gametype->integer == GT_SINGLE_PLAYER) {
        return;
    }

    weapon = GetActiveWeapon(WEAPON_MAIN);

    if (!weapon) {
        return;
    }

    // Don't drop the weapon if we're charging
    if (charge_start_time) {
        return;
    }

    if ((weapon->GetWeaponClass() & WEAPON_CLASS_ITEM)) {
        SelectNextWeapon(NULL);
        takeItem(weapon->model);
    } else {
        if (weapon->GetCurrentAttachToTag() != "tag_weapon_right") {
            EventCorrectWeaponAttachments(NULL);
        }

        // This check isn't in MOHAA
        if (!weapon->IsDroppable()) {
            return;
        }

        weapon->Drop();

        AngleVectors(m_vViewAng, forward, NULL, NULL);

        // make the weapon looks like it's thrown
        weapon->velocity = forward * 200.0f;

        edict->s.eFlags |= EF_UNARMED;

        SelectNextWeapon(NULL);

        if (holsteredWeapon == weapon) {
            holsteredWeapon = NULL;
        }
        if (lastActiveWeapon.weapon == weapon) {
            lastActiveWeapon.weapon = NULL;
        }
    }
}

void Player::GiveWeaponCheat(Event *ev)
{
    giveItem(ev->GetString(1));
}

void Player::GiveCheat(Event *ev)
{
    str name;

    if (deadflag) {
        return;
    }

    name = ev->GetString(1);

    if (!name.icmp("all")) {
        GiveAllCheat(ev);
        return;
    }
    EventGiveItem(ev);
}

void Player::GiveAllCheat(Event *ev)
{
    char *buffer;
    char *buf;
    char  com_token[MAX_STRING_CHARS];

    if (deadflag) {
        return;
    }

    if (gi.FS_ReadFile("global/giveall.scr", (void **)&buf, true) != -1) {
        buffer = buf;
        while (1) {
            Q_strncpyz(com_token, COM_ParseExt(&buffer, qtrue), sizeof(com_token));

            if (!com_token[0]) {
                break;
            }

            // Create the event
            ev = new Event(com_token);

            // get the rest of the line
            while (1) {
                Q_strncpyz(com_token, COM_ParseExt(&buffer, qfalse), sizeof(com_token));
                if (!com_token[0]) {
                    break;
                }

                ev->AddToken(com_token);
            }

            this->ProcessEvent(ev);
        }
        gi.FS_FreeFile(buf);
    }
}

void Player::GiveNewWeaponsCheat(Event *ev)
{
    char       *buffer;
    char       *current;
    const char *token;

    if (deadflag != DEAD_NO) {
        return;
    }

    if (gi.FS_ReadFile("global/givenewweapons.scr", (void **)&buffer, qtrue) != -1) {
        return;
    }

    current = buffer;
    for (;;) {
        Event *event;

        token = COM_ParseExt(&current, qtrue);
        if (!token[0]) {
            break;
        }

        event = new Event(token);

        for (;;) {
            token = COM_ParseExt(&current, qfalse);
            if (!token[0]) {
                break;
            }

            event->AddToken(token);
        }

        ProcessEvent(event);
    }

    gi.FS_FreeFile(buffer);
}

void Player::GodCheat(Event *ev)
{
    const char *msg;

    if (ev->NumArgs() > 0) {
        if (ev->GetInteger(1)) {
            flags |= FL_GODMODE;
            // Also enable the god mode for the vehicle
            if (m_pVehicle) {
                m_pVehicle->flags |= FL_GODMODE;
            }
        } else {
            flags &= ~FL_GODMODE;
            // Also disable the god mode for the vehicle
            if (m_pVehicle) {
                m_pVehicle->flags &= ~FL_GODMODE;
            }
        }
    } else {
        if (flags & FL_GODMODE) {
            flags &= ~FL_GODMODE;
            if (m_pVehicle) {
                m_pVehicle->flags &= ~FL_GODMODE;
            }
        } else {
            flags |= FL_GODMODE;
            if (m_pVehicle) {
                m_pVehicle->flags |= FL_GODMODE;
            }
        }
    }

    if (ev->isSubclassOf(ConsoleEvent)) {
        if (!(flags & FL_GODMODE)) {
            msg = "CHEAT: godmode OFF\n";
        } else {
            msg = "CHEAT: godmode ON\n";
        }

        gi.SendServerCommand(edict - g_entities, "print \"%s\"", msg);
    }
}

void Player::Kill(Event *ev)
{
    if ((level.time - respawn_time) < 5) {
        return;
    }

    flags &= ~FL_GODMODE;
    health = 1;
    Damage(this, this, 10, origin, vec_zero, vec_zero, 0, DAMAGE_NO_PROTECTION, MOD_SUICIDE);
}

void Player::NoTargetCheat(Event *ev)
{
    const char *msg;

    // [bug-2064] THIS EVENT SHADOWS Entity::NoTarget FOR PLAYERS, AND IT IS A TOGGLE.
    // Both this event and EV_NoTarget (entity.cpp) are declared EV_NORMAL under the command name
    // "notarget", and ScriptMaster builds the script command table as a plain name -> eventnum
    // map with last-write-wins (scriptmaster.cpp:616). On this build THIS one wins for Player
    // targets, so every script `player notarget 1` reached here, DISCARDED ITS ARGUMENT, and
    // flipped the flag. Measured on three consecutive live m1l1 runs of one identical build: the
    // scripted intro ride emitted "notarget ON" / "notarget OFF" / "notarget ON" every single
    // time - the two spawn-time asserts cancelled each other, so the player rode the whole intro
    // targetable, and the ride-end clear then turned the flag back ON and left it there.
    // That is why four separate attempts to hold FL_NOTARGET through that ride all failed, and
    // why the failures looked intermittent rather than systematic.
    // An explicit argument now SETS, exactly as Entity::NoTarget does, and stays silent - the
    // cheat message belongs to the console form. No argument keeps the original toggle, so the
    // `notarget` console command and coop_mod/developer.scr:812 behave exactly as before.
    if (ev->NumArgs() >= 1) {
        if (ev->GetBoolean(1)) {
            flags |= FL_NOTARGET;
        } else {
            flags &= ~FL_NOTARGET;
        }
        return;
    }

    flags ^= FL_NOTARGET;
    if (!(flags & FL_NOTARGET)) {
        msg = "notarget OFF\n";
    } else {
        msg = "notarget ON\n";
    }

    gi.SendServerCommand(edict - g_entities, "print \"%s\"", msg);
}

void Player::NoclipCheat(Event *ev)
{
    const char *msg;

    if (m_pVehicle) {
        msg = "Must exit vehicle first\n";
    } else if (m_pTurret) {
        msg = "Must exit turret first\n";
    } else if (getMoveType() == MOVETYPE_NOCLIP) {
        setMoveType(MOVETYPE_WALK);
        msg = "noclip OFF\n";

        // reset the state machine so that his animations are correct
        ResetState(NULL);
        charge_start_time = 0;
    } else {
        client->ps.feetfalling = false;
        movecontrol            = MOVECONTROL_LEGS;

        setMoveType(MOVETYPE_NOCLIP);
        msg = "noclip ON\n";
    }

    gi.SendServerCommand(edict - g_entities, "print \"%s\"", msg);
}

/*
==============
Player::EventCoopProf

HZM: one chunk of this client's stored profile mirror, arriving as a console command.
Reassembled here and published to script as `coop_profdata`.

DELIBERATELY DUMB. This only concatenates and publishes - it does not parse, validate or apply
anything. The server stays authoritative: script decides whether to import, and only when it has no
record of its own for that id. Treat the contents as untrusted player-supplied data, because that is
exactly what it is - a player can type this command.
==============
*/
void Player::EventCoopProf(Event *ev)
{
    int         idx;
    const char *chunk;

    if (ev->NumArgs() < 2) {
        return;
    }

    idx   = ev->GetInteger(1);
    chunk = ev->GetString(2);

    if (idx <= 0) {
        // index 0 (or anything odd) starts a fresh buffer - a reconnecting client must not append
        // onto the tail of whatever it sent last session.
        m_sCoopProf = "";
    }

    // Hard cap. Without one, a client could grow this without bound by spamming the command.
    if (m_sCoopProf.length() + strlen(chunk) > 8192) {
        return;
    }

    m_sCoopProf += chunk;
    Vars()->SetVariable("coop_profdata", m_sCoopProf.c_str());
}

void Player::GameVersion(Event *ev)
{
    gi.SendServerCommand(edict - g_entities, "print \"%s : %s\n\"", GAMEVERSION, __DATE__);
}

void Player::SetFov(float newFov)
{
    fov = newFov;

    if (fov < 1) {
        fov = 80;
    } else if (fov > 160) {
        fov = 160;
    }
}

void Player::EventSetSelectedFov(Event *ev)
{
    float fOldSelectedFov;

    if (ev->NumArgs() < 1) {
        gi.SendServerCommand(edict - g_entities, "print \"Fov = %d\n\"", (unsigned int)fov);
        return;
    }

    fOldSelectedFov = selectedfov;
    SetSelectedFov(ev->GetFloat(1));
    if (fov == fOldSelectedFov) {
        SetFov(selectedfov);
    }
}

void Player::SetSelectedFov(float newFov)
{
    selectedfov = newFov;

    if (selectedfov < 1) {
        selectedfov = 80;
    } else if (selectedfov > 160) {
        selectedfov = 160;
    }

    /*
    if( g_gametype->integer != GT_SINGLE_PLAYER && !developer->integer )
    {
        if( selectedfov < 80 )
        {
            selectedfov = 80;
        }
        else if( selectedfov > 80 )
        {
            selectedfov = 80;
        }
    }
    */
}

/*
===============
CalcRoll

===============
*/
float Player::CalcRoll(void)
{
    float  sign;
    float  side;
    float  value;
    Vector l;

    angles.AngleVectors(NULL, &l, NULL);
    side = velocity * l;
    sign = side < 0 ? 4 : -4;
    side = fabs(side);

    value = sv_rollangle->value;

    if (side < sv_rollspeed->value) {
        side = side * value / sv_rollspeed->value;
    } else {
        side = value;
    }

    return side * sign;
}

//
// PMove Events
//
void Player::ProcessPmoveEvents(int event)
{
    float damage;

    switch (event) {
    case EV_NONE:
        break;
    case EV_FALL_SHORT:
    case EV_FALL_MEDIUM:
    case EV_FALL_FAR:
    case EV_FALL_FATAL:
        if (event == EV_FALL_FATAL) {
            if (g_protocol >= protocol_e::PROTOCOL_MOHTA_MIN) {
                damage = 101;
            } else {
                damage = max_health + 1.0f;
            }
        } else if (event == EV_FALL_FAR) {
            if (g_protocol >= protocol_e::PROTOCOL_MOHTA_MIN) {
                damage = 25;
            } else {
                damage = 20;
            }
        } else if (event == EV_FALL_MEDIUM) {
            if (g_protocol >= protocol_e::PROTOCOL_MOHTA_MIN) {
                damage = 15;
            } else {
                damage = 10;
            }
        } else {
            damage = 5;
        }

        if (g_protocol >= protocol_e::PROTOCOL_MOHTA_MIN) {
            // since 2.0, remove a percentage of the health
            damage = damage * (max_health / 100.0);
        }
        if (g_gametype->integer == GT_SINGLE_PLAYER || !DM_FLAG(DF_NO_FALLING)) {
            Damage(this, this, (int)damage, origin, vec_zero, vec_zero, 0, DAMAGE_NO_ARMOR, MOD_FALLING);
        }
        break;
    case EV_TERMINAL_VELOCITY:
        Sound("snd_fall", CHAN_VOICE);
        break;
    case EV_WATER_LEAVE: // foot leaves
        Sound("impact_playerleavewater", CHAN_AUTO);
        break;
    case EV_WATER_UNDER: // head touches
        Sound("impact_playersubmerge", CHAN_AUTO);
        break;
    case EV_WATER_CLEAR: // head leaves
        Sound("snd_gasp", CHAN_LOCAL);
        break;
    }
}

/*
=============
WorldEffects
=============
*/
void Player::WorldEffects(void)
{
    if (deadflag == DEAD_DEAD || getMoveType() == MOVETYPE_NOCLIP) {
        // if we are dead or no-cliping, no world effects
        return;
    }

    //
    // check for on fire
    //
    if (on_fire) {
        if (next_painsound_time < level.time) {
            next_painsound_time = level.time + 4;
            Sound("snd_onfire", CHAN_LOCAL);
        }
    }
}

/*
=============
AddBlend
=============
*/
void Player::AddBlend(float r, float g, float b, float a)
{
    float a2;
    float a3;

    if (a <= 0) {
        return;
    }

    // new total alpha
    a2 = blend[3] + (1 - blend[3]) * a;

    // fraction of color from old
    a3 = blend[3] / a2;

    blend[0] = blend[0] * a3 + r * (1 - a3);
    blend[1] = blend[1] * a3 + g * (1 - a3);
    blend[2] = blend[2] * a3 + b * (1 - a3);
    blend[3] = a2;
}

/*
=============
CalcBlend
=============
*/
void Player::CalcBlend(void)
{
    int    contents;
    Vector vieworg;

    client->ps.stats[STAT_ADDFADE] = 0;
    blend[0] = blend[1] = blend[2] = blend[3] = 0;

    // add for contents
    vieworg = m_vViewPos;

    contents = gi.pointcontents(vieworg, 0);

    if (contents & CONTENTS_SOLID) {
        // Outside of world
        //AddBlend( 0.8, 0.5, 0.0, 0.2 );
    } else if (contents & CONTENTS_LAVA) {
        AddBlend(level.lava_color[0], level.lava_color[1], level.lava_color[2], level.lava_alpha);
    } else if (contents & CONTENTS_WATER) {
        AddBlend(level.water_color[0], level.water_color[1], level.water_color[2], level.water_alpha);
    }

    // add for damage
    if (damage_alpha > 0) {
        AddBlend(damage_blend[0], damage_blend[1], damage_blend[2], damage_alpha);

        // drop the damage value
        damage_alpha -= 0.06f;
        if (damage_alpha < 0) {
            damage_alpha = 0;
        }
        client->ps.blend[0] = blend[0];
        client->ps.blend[1] = blend[1];
        client->ps.blend[2] = blend[2];
        client->ps.blend[3] = blend[3];
    }

    // Do the cinematic fading
    float alpha = 1;

    // HZM coop [found 2026-08-28] LEVEL-GLOBAL TIMER, PER-PLAYER FUNCTION. CalcBlend runs once per
    // player per frame, so with N players this decremented the level's single fade timer N times a
    // frame and every scripted fadeout/fadein ran N times too fast - four times too fast on a full
    // coop server. Decrement once per frame, whoever gets here first.
    {
        static int s_iFadeFrame = -1;
        if (s_iFadeFrame != level.framenum) {
            s_iFadeFrame = level.framenum;
            level.m_fade_time -= level.frametime;
        }
    }

    // Return if we are completely faded in
    if ((level.m_fade_time <= 0) && (level.m_fade_type == fadein)) {
        client->ps.blend[3] = 0 + damage_alpha;
        return;
    }

    // If we are faded out, and another fade out is coming in, then don't bother
    if ((level.m_fade_time_start > 0) && (level.m_fade_type == fadeout)) {
        if (client->ps.blend[3] >= 1) {
            return;
        }
    }

    if (level.m_fade_time_start > 0) {
        alpha = level.m_fade_time / level.m_fade_time_start;
    }

    if (level.m_fade_type == fadeout) {
        alpha = 1.0f - alpha;
    }

    if (alpha < 0) {
        alpha = 0;
    }

    if (alpha > 1) {
        alpha = 1;
    }

    if (level.m_fade_style == additive) {
        client->ps.blend[0]            = level.m_fade_color[0] * level.m_fade_alpha * alpha;
        client->ps.blend[1]            = level.m_fade_color[1] * level.m_fade_alpha * alpha;
        client->ps.blend[2]            = level.m_fade_color[2] * level.m_fade_alpha * alpha;
        client->ps.blend[3]            = level.m_fade_alpha * alpha;
        client->ps.stats[STAT_ADDFADE] = 1;
    } else {
        client->ps.blend[0]            = level.m_fade_color[0];
        client->ps.blend[1]            = level.m_fade_color[1];
        client->ps.blend[2]            = level.m_fade_color[2];
        client->ps.blend[3]            = level.m_fade_alpha * alpha;
        client->ps.stats[STAT_ADDFADE] = 0;
    }
}

/*
===============
P_DamageFeedback

Handles color blends and view kicks
===============
*/

void Player::DamageFeedback(void)
{
    float  realcount;
    float  count;
    vec3_t vDir;
    str    painAnim;
    int    animnum;

    // if we are dead, don't setup any feedback
    if (IsDead()) {
        damage_count = 0;
        damage_blood = 0;
        damage_alpha = 0;
        VectorClear(damage_angles);
        return;
    }

    if (damage_count) {
        // decay damage_count over time
        damage_count *= 0.8f;
        damage_from *= 0.8f;
        damage_angles *= 0.8f;
        if (damage_count < 0.1f) {
            damage_count = 0;
            damage_from  = Vector(0, 0, 0);
        }
    }

    // total points of damage shot at the player this frame
    if (!damage_blood) {
        // didn't take any damage
        return;
    }

    VectorNormalize2(damage_from, vDir);

    damage_angles.x -=
        DotProduct(vDir, orientation[0]) * damage_blood * g_viewkick_pitch->value * g_viewkick_dmmult->value;
    damage_angles.x = Q_clamp_float(damage_angles.x, -30, 30);

    damage_angles.y -=
        DotProduct(vDir, orientation[1]) * damage_blood * g_viewkick_yaw->value * g_viewkick_dmmult->value;
    damage_angles.y = Q_clamp_float(damage_angles.y, -30, 30);

    damage_angles.z +=
        DotProduct(vDir, orientation[2]) * damage_blood * g_viewkick_roll->value * g_viewkick_dmmult->value;
    // HZM coop [bug-2092] .y -> .z. This clamped the YAW member into the ROLL member, throwing away the
    // roll computed on the line directly above and replacing it with the yaw. Net effect for the life of
    // the project: incoming fire rolled the view by the yaw kick, and g_viewkick_roll did nothing at all.
    // Its "0.15" default is therefore an UNTESTED author intent, not a tuned value - see gamecvars.cpp.
    damage_angles.z = Q_clamp_float(damage_angles.z, -25, 25);

    damage_count += damage_blood;
    count     = damage_blood;
    realcount = count;
    if (count < 10) {
        // always make a visible effect
        count = 10;
    }

    // the total alpha of the blend is always proportional to count
    if (damage_alpha < 0) {
        damage_alpha = 0;
    }

    damage_alpha += count * 0.001;
    if (damage_alpha < 0.2f) {
        damage_alpha = 0.2f;
    }
    if (damage_alpha > 0.6f) {
        // don't go too saturated
        damage_alpha = 0.6f;
    }

    // the color of the blend will vary based on how much was absorbed
    // by different armors
    damage_blend = vec_zero;
    if (damage_blood) {
        damage_blend += (damage_blood / realcount) * bcolor;
    }

    if (g_target_game >= target_game_e::TG_MOHTA) {
        //
        // Since 2.0: Try to find and play pain animation
        //
        if (getMoveType() == MOVETYPE_PORTABLE_TURRET) {
            // use mg42 pain animation
            painAnim = "mg42_tripod_";
        } else {
            Weapon     *pWeap;
            const char *itemName;
            // try to find an animation

            pWeap = GetActiveWeapon(WEAPON_MAIN);
            if (pWeap) {
                int weapon_class;

                weapon_class = pWeap->GetWeaponClass();
                if (weapon_class & WEAPON_CLASS_PISTOL) {
                    painAnim = "pistol_";
                } else if (weapon_class & WEAPON_CLASS_RIFLE) {
                    painAnim = "rifle_";
                } else if (weapon_class & WEAPON_CLASS_SMG) {
                    // get the animation name from the item name
                    itemName = pWeap->GetItemName();

                    if (!Q_stricmp(itemName, "MP40")) {
                        painAnim = "mp40_";
                    } else if (!Q_stricmp(itemName, "Sten Mark II")) {
                        painAnim = "sten_";
                    } else {
                        painAnim = "smg_";
                    }
                } else if (weapon_class & WEAPON_CLASS_MG) {
                    itemName = pWeap->GetItemName();

                    if (!Q_stricmp(itemName, "StG 44")) {
                        painAnim = "mp44_";
                    } else {
                        painAnim = "mg_";
                    }
                } else if (weapon_class & WEAPON_CLASS_GRENADE) {
                    itemName = pWeap->GetItemName();

                    // 2.30: use landmine animations
                    if (!Q_stricmp(itemName, "Minedetector")) {
                        painAnim = "minedetector_";
                    } else if (!Q_stricmp(itemName, "Minensuchgerat")) {
                        painAnim = "minedetectoraxis_";
                    } else if (!Q_stricmp(itemName, "LandmineAllies")) {
                        painAnim = "mine_";
                    } else if (!Q_stricmp(itemName, "LandmineAxis")) {
                        painAnim = "mine_";
                    } else if (!Q_stricmp(itemName, "LandmineAxis")) {
                        painAnim = "grenade_";
                    }
                } else if (weapon_class & WEAPON_CLASS_HEAVY) {
                    itemName = pWeap->GetItemName();

                    if (!Q_stricmp(itemName, "Shotgun")) {
                        painAnim = "shotgun_";
                    } else {
                        // Defaults to bazooka
                        painAnim = "bazooka_";
                    }
                } else {
                    itemName = pWeap->GetItemName();

                    if (!Q_stricmp(itemName, "Packed MG42 Turret")) {
                        painAnim = "mg42_";
                    } else {
                        // Default animation if not found
                        painAnim = "unarmed_";
                    }
                }
            } else {
                painAnim = "unarmed_";
            }

            // use the animation based on the movement
            // HZM coop [user 2026-08-26, bug-2119] PRONE pain. There was no prone case, so a prone
            // player snapped into the STANDING pain animation on every hit ("jolting"). Retail ships
            // prone pain skcs but aliases them only on the AI tik, and only for rifles - so rather
            // than 15 weapon-group alias sets, prone REPLACES the weapon prefix with one shared
            // coop_prone_ set (7 aliases in anims_shared.txt, rifle skcs for every gun - the same
            // rifle-for-all rule the crawl already uses). Verified notetrack-free before aliasing.
            if (m_bCoopProne && (client->ps.pm_flags & PMF_VIEW_PRONE)) {
                // [spec P1] a supine body plays the FLIPPED pain set - the prone hit files are
                // movement-class (they carry root pos+rot), so unflipped they wrench the whole
                // body toward its side at pain-blend weight for up to 2.7s per hit.
                // [pass4, bug-2127] during a flip window the FLAG already reports the
                // DESTINATION pose (both flip paths toggle it at roll start), but the body
                // is still mostly in the SOURCE pose - a destination-set pain is exactly the
                // root-rot wrench P1 removed. XOR with the window routes by the source:
                // settled supine (1,0)->supine; flip-in (1,1)->prone; flip-out (0,1)->supine.
                if (m_bCoopSupine != (level.time < m_fCoopSupineFlip)) {
                    painAnim = "coop_supine_";
                } else {
                    painAnim = "coop_prone_";
                }
            } else if (m_iMovePosFlags & MPF_POSITION_CROUCHING) {
                painAnim += "crouch_";
            } else {
                painAnim += "stand_";
            }
        }

        painAnim += "hit_";

        // [pass3, bug-2126] upstream bug: the third operand was the bare constant
        // HITLOC_TORSO_LOWER (always truthy), so the per-location switch below was DEAD and
        // every pain played *_hit_back. Fixed for the coop prone/supine sets, whose 7 location
        // aliases all ship (anims_shared 621-635) - supine_hit_helmet/_legs were unreachable
        // assets. The always-back result is deliberately PRESERVED for stand/crouch: retail
        // per-weapon hit_<loc> alias coverage is unaudited, and a missing alias there skips
        // the flinch entirely (worse than a wrong 'back').
        if (pain_dir == PAIN_REAR || pain_location == HITLOC_TORSO_MID || pain_location == HITLOC_TORSO_LOWER
            || !(m_bCoopProne && (client->ps.pm_flags & PMF_VIEW_PRONE))) {
            painAnim += "back";
        } else {
            switch (pain_location) {
            case HITLOC_HEAD:
            case HITLOC_HELMET:
            case HITLOC_NECK:
                painAnim += "head";
                break;
            case HITLOC_TORSO_UPPER:
            case HITLOC_TORSO_MID:
                painAnim += "uppertorso";
                break;
            case HITLOC_TORSO_LOWER:
            case HITLOC_PELVIS:
                painAnim += "lowertorso";
                break;
            case HITLOC_R_ARM_UPPER:
            case HITLOC_R_ARM_LOWER:
            case HITLOC_R_HAND:
                painAnim += "rarm";
                break;
            case HITLOC_L_ARM_UPPER:
            case HITLOC_L_ARM_LOWER:
            case HITLOC_L_HAND:
                painAnim += "larm";
                break;
            case HITLOC_R_LEG_UPPER:
            case HITLOC_L_LEG_UPPER:
            case HITLOC_R_LEG_LOWER:
            case HITLOC_L_LEG_LOWER:
            case HITLOC_R_FOOT:
            case HITLOC_L_FOOT:
                painAnim += "leg";
                break;
            default:
                painAnim += "uppertorso";
                break;
            }
        }

        animnum = gi.Anim_NumForName(edict->tiki, painAnim.c_str());
        if (animnum == -1) {
            gi.DPrintf("WARNING: Could not find player pain animation '%s'\n", painAnim.c_str());
        } else {
            NewAnim(animnum, EV_Player_AnimLoop_Pain, ANIMSLOT_PAIN);
            RestartAnimSlot(ANIMSLOT_PAIN);
            m_sPainAnim   = painAnim;
            m_fPainBlend  = 1.f;
            animdone_Pain = false;
        }
    }

    //
    // clear totals
    //
    damage_blood = 0;

    //
    // Added in 2.0
    //  Don't show damage when in god mode
    //
    if (flags & FL_GODMODE) {
        damage_count  = 0;
        damage_blood  = 0;
        damage_alpha  = 0;
        damage_angles = vec_zero;
    }
}

void Player::GetPlayerView(Vector *pos, Vector *angle)
{
    if (pos) {
        *pos = origin;
        pos->z += viewheight;
    }

    if (angle) {
        *angle = Vector(client->ps.viewangles);
    }
}

void Player::SetPlayerView(
    Camera *camera, Vector position, float cameraoffset, Vector ang, Vector vel, float camerablend[4], float camerafov
)
{
    VectorCopy(ang, client->ps.viewangles);
    client->ps.viewheight = cameraoffset;

    VectorCopy(position, client->ps.origin);
    VectorCopy(vel, client->ps.velocity);

    /*
    client->ps.blend[ 0 ] = camerablend[ 0 ];
    client->ps.blend[ 1 ] = camerablend[ 1 ];
    client->ps.blend[ 2 ] = camerablend[ 2 ];
    client->ps.blend[ 3 ] = camerablend[ 3 ];
    */

    client->ps.fov = camerafov;

    if (camera) {
        if (camera->IsSubclassOfCamera()) {
            VectorCopy(camera->angles, client->ps.camera_angles);
            VectorCopy(camera->origin, client->ps.camera_origin);

            Vector vOfs = camera->GetPositionOffset();
            VectorCopy(vOfs, client->ps.camera_posofs);

            client->ps.pm_flags |= PMF_CAMERA_VIEW;

            if (camera->ShowQuakes()) {
                client->ps.pm_flags |= PMF_DAMAGE_ANGLES;
            } else {
                client->ps.pm_flags &= ~PMF_DAMAGE_ANGLES;
            }

            //
            // clear out the flags, but preserve the CF_CAMERA_CUT_BIT
            //
            client->ps.camera_flags = client->ps.camera_flags & CF_CAMERA_CUT_BIT;
        } else {
            Vector vVec;

            if (camera->IsSubclassOfPlayer()) {
                Vector  vPos;
                Player *pPlayer = (Player *)camera;

                GetSpectateFollowOrientation(pPlayer, vPos, vVec);

                VectorCopy(vVec, client->ps.camera_angles);
                VectorCopy(vPos, client->ps.camera_origin);

                SetViewAngles(vVec);

                vPos[2] -= viewheight;
                setOrigin(vPos);

                vVec.setXYZ(0, 0, 0);
            } else {
                VectorCopy(ang, client->ps.camera_angles);
                VectorCopy(position, client->ps.camera_angles);

                vVec.setXYZ(0, 0, 0);
            }

            VectorCopy(vVec, client->ps.camera_posofs);
            client->ps.pm_flags |= PMF_CAMERA_VIEW;
            client->ps.camera_flags = client->ps.camera_flags & CF_CAMERA_CUT_BIT;
        }
    } else {
        client->ps.pm_flags &= ~PMF_CAMERA_VIEW;
        //
        // make sure the third person camera is setup correctly.
        //

        if (getMoveType() != MOVETYPE_NOCLIP) {
            qboolean do_cut;
            int      camera_type;

            if (currentState_Torso) {
                camera_type = currentState_Torso->getCameraType();
            } else {
                camera_type = CAMERA_BEHIND;
            }
            if (last_camera_type != camera_type) {
                //
                // clear out the flags, but preserve the CF_CAMERA_CUT_BIT
                //
                client->ps.camera_flags = client->ps.camera_flags & CF_CAMERA_CUT_BIT;
                do_cut                  = qtrue;
                switch (camera_type) {
                case CAMERA_TOPDOWN:
                    client->ps.camera_flags |= CF_CAMERA_ANGLES_IGNORE_PITCH;
                    client->ps.camera_offset[PITCH] = -75;
                    client->ps.camera_flags |= CF_CAMERA_ANGLES_ALLOWOFFSET;
                    do_cut = qfalse;
                    break;
                case CAMERA_FRONT:
                    client->ps.camera_flags |= CF_CAMERA_ANGLES_IGNORE_PITCH;
                    client->ps.camera_flags |= CF_CAMERA_ANGLES_ALLOWOFFSET;
                    client->ps.camera_offset[YAW]   = 180;
                    client->ps.camera_offset[PITCH] = 0;
                    break;
                case CAMERA_SIDE:
                    client->ps.camera_flags |= CF_CAMERA_ANGLES_IGNORE_PITCH;
                    client->ps.camera_flags |= CF_CAMERA_ANGLES_ALLOWOFFSET;
                    // randomly invert the YAW
                    if (G_Random(1) > 0.5f) {
                        client->ps.camera_offset[YAW] = -90;
                    } else {
                        client->ps.camera_offset[YAW] = 90;
                    }
                    client->ps.camera_offset[PITCH] = 0;
                    break;
                case CAMERA_SIDE_LEFT:
                    client->ps.camera_flags |= CF_CAMERA_ANGLES_IGNORE_PITCH;
                    client->ps.camera_flags |= CF_CAMERA_ANGLES_ALLOWOFFSET;
                    client->ps.camera_offset[YAW]   = 90;
                    client->ps.camera_offset[PITCH] = 0;
                    break;
                case CAMERA_SIDE_RIGHT:
                    client->ps.camera_flags |= CF_CAMERA_ANGLES_IGNORE_PITCH;
                    client->ps.camera_flags |= CF_CAMERA_ANGLES_ALLOWOFFSET;
                    client->ps.camera_offset[YAW]   = -90;
                    client->ps.camera_offset[PITCH] = 0;
                    break;
                case CAMERA_BEHIND_FIXED:
                    do_cut                          = qfalse;
                    client->ps.camera_offset[YAW]   = 0;
                    client->ps.camera_offset[PITCH] = 0;
                    client->ps.camera_flags |= CF_CAMERA_ANGLES_ALLOWOFFSET;
                    break;
                case CAMERA_BEHIND_NOPITCH:
                    do_cut = qfalse;
                    client->ps.camera_flags |= CF_CAMERA_ANGLES_IGNORE_PITCH;
                    client->ps.camera_offset[YAW]   = 0;
                    client->ps.camera_offset[PITCH] = 0;
                    break;
                case CAMERA_BEHIND:
                    do_cut                          = qfalse;
                    client->ps.camera_offset[YAW]   = 0;
                    client->ps.camera_offset[PITCH] = 0;
                    break;
                default:
                    do_cut                          = qfalse;
                    client->ps.camera_offset[YAW]   = 0;
                    client->ps.camera_offset[PITCH] = 0;
                    break;
                }
                last_camera_type = camera_type;
                if (do_cut) {
                    CameraCut();
                }
            }
        } else {
            client->ps.camera_flags = client->ps.camera_flags & CF_CAMERA_CUT_BIT;
        }

        //
        // these are explicitly not cleared so that when the client lerps it still has the last
        // camera position for reference. Additionally this causes no extra hits to the network
        // traffic.
        //
        //VectorClear( client->ps.camera_angles );
        //VectorClear( client->ps.camera_origin );
    }

#define EARTHQUAKE_SCREENSHAKE_PITCH 2
#define EARTHQUAKE_SCREENSHAKE_YAW   2
#define EARTHQUAKE_SCREENSHAKE_ROLL  3

    if (level.earthquake_magnitude != 0.0f) {
        client->ps.damage_angles[PITCH] = G_CRandom() * level.earthquake_magnitude * EARTHQUAKE_SCREENSHAKE_PITCH;
        client->ps.damage_angles[YAW]   = G_CRandom() * level.earthquake_magnitude * EARTHQUAKE_SCREENSHAKE_YAW;
        client->ps.damage_angles[ROLL]  = G_CRandom() * level.earthquake_magnitude * EARTHQUAKE_SCREENSHAKE_ROLL;
    } else if (damage_count) {
        client->ps.damage_angles[PITCH] = damage_angles[PITCH];
        client->ps.damage_angles[YAW]   = damage_angles[YAW];
        client->ps.damage_angles[ROLL]  = damage_angles[ROLL];
    } else {
        VectorClear(client->ps.damage_angles);
    }

    if (m_vViewVariation != vec_zero) {
        for (int i = 0; i < 3; i++) {
            if (m_vViewVariation[i] == 0.0f) {
                continue;
            }

            client->ps.damage_angles[i] += G_CRandom() * m_vViewVariation[i];

            m_vViewVariation[i] = m_vViewVariation[i] - m_vViewVariation[i] * level.frametime * 8.0f;

            if (m_vViewVariation[i] < 0.01f) {
                m_vViewVariation[i] = 0.0f;
            }
        }
    }
}

void Player::SetupView(void)
{
    // if we currently are not in a camera or the camera we are looking through is automatic, evaluate our camera choices

    if (actor_to_watch || actor_camera) {
        Vector   dir;
        Vector   watch_angles;
        float    dist = 0;
        Vector   focal_point;
        Vector   left;
        trace_t  trace;
        qboolean delete_actor_camera = false;
        Vector   camera_mins;
        Vector   camera_maxs;

        if (actor_to_watch) {
            dir  = actor_to_watch->origin - origin;
            dist = dir.length();
        }

        // See if we still want to watch this actor

        if (!actor_to_watch || dist > 150 || actor_to_watch->deadflag) {
            delete_actor_camera = true;
        } else {
            // Create the camera if we don't have one yet

            if (!actor_camera) {
                actor_camera = new Camera();

                if (G_Random() < .5) {
                    actor_camera_right          = true;
                    starting_actor_camera_right = true;
                } else {
                    actor_camera_right          = false;
                    starting_actor_camera_right = false;
                }
            }

            // Setup the new position of the actor camera

            // Go a little above the view height

            actor_camera->origin = origin;
            actor_camera->origin[2] += DEFAULT_VIEWHEIGHT + 10;

            // Find the focal point ( either the actor's watch offset or top of the bounding box)

            if (actor_to_watch->watch_offset != vec_zero) {
                MatrixTransformVector(actor_to_watch->watch_offset, actor_to_watch->orientation, focal_point);
                focal_point += actor_to_watch->origin;
            } else {
                focal_point    = actor_to_watch->origin;
                focal_point[2] = actor_to_watch->maxs[2];
            }

            // Shift the camera back just a little

            dir = focal_point - actor_camera->origin;
            dir.normalize();
            actor_camera->origin -= dir * 15;

            // Shift the camera a little to the left or right

            watch_angles = dir.toAngles();
            watch_angles.AngleVectors(NULL, &left);

            if (actor_camera_right) {
                actor_camera->origin -= left * 15;
            } else {
                actor_camera->origin += left * 15;
            }

            // Make sure this camera position is ok

            camera_mins = "-5 -5 -5";
            camera_maxs = "5 5 5";

            trace = G_Trace(
                actor_camera->origin,
                camera_mins,
                camera_maxs,
                actor_camera->origin,
                actor_camera,
                MASK_DEADSOLID,
                false,
                "SetupView"
            );

            if (trace.startsolid) {
                // Try other side

                if (actor_camera_right == starting_actor_camera_right) {
                    if (actor_camera_right) {
                        actor_camera->origin += left * 30;
                    } else {
                        actor_camera->origin -= left * 30;
                    }

                    actor_camera_right = !actor_camera_right;

                    trace = G_Trace(
                        actor_camera->origin,
                        camera_mins,
                        camera_maxs,
                        actor_camera->origin,
                        actor_camera,
                        MASK_DEADSOLID,
                        false,
                        "SetupView2"
                    );

                    if (trace.startsolid) {
                        // Both spots have failed stop doing actor camera
                        delete_actor_camera = true;
                    }
                } else {
                    // Both spots have failed stop doing actor camera
                    delete_actor_camera = true;
                }
            }

            if (!delete_actor_camera) {
                // Set the camera's position

                actor_camera->setOrigin(actor_camera->origin);

                // Set the camera's angles

                dir          = focal_point - actor_camera->origin;
                watch_angles = dir.toAngles();
                actor_camera->setAngles(watch_angles);

                // Set this as our camera

                SetCamera(actor_camera, .5);
            }
        }

        if (delete_actor_camera) {
            // Get rid of this camera

            actor_to_watch = NULL;

            if (actor_camera) {
                delete actor_camera;
                actor_camera = NULL;
                SetCamera(NULL, .5);
            }
        }
    } else if ((level.automatic_cameras.NumObjects() > 0) && (!camera || camera->IsAutomatic())) {
        int     i;
        float   score, bestScore;
        Camera *cam, *bestCamera;

        bestScore  = 999;
        bestCamera = NULL;
        for (i = 1; i <= level.automatic_cameras.NumObjects(); i++) {
            cam   = level.automatic_cameras.ObjectAt(i);
            score = cam->CalculateScore(this, currentState_Torso->getName());
            // if this is our current camera, scale down the score a bit to favor it.
            if (cam == camera) {
                score *= 0.9f;
            }

            if (score < bestScore) {
                bestScore  = score;
                bestCamera = cam;
            }
        }
        if (bestScore <= 1.0f) {
            // we have a camera to switch to
            if (bestCamera != camera) {
                float time;

                if (camera) {
                    camera->AutomaticStop(this);
                }
                time = bestCamera->AutomaticStart(this);
                SetCamera(bestCamera, time);
            }
        } else {
            // we don't have a camera to switch to
            if (camera) {
                float time;

                time = camera->AutomaticStop(this);
                SetCamera(NULL, time);
            }
        }
    }

    // If there is no camera, use the player's view
    if (!camera) {
        if (g_gametype->integer != GT_SINGLE_PLAYER && IsSpectator() && m_iPlayerSpectating != 0) {
            gentity_t *ent = g_entities + m_iPlayerSpectating - 1;

            if (ent->inuse && ent->entity && ent->entity->deadflag <= DEAD_DYING) {
                Player *m_player = (Player *)ent->entity;
                Vector  vAngles;

                m_player->GetPlayerView(NULL, &vAngles);

                SetPlayerView(
                    (Camera *)m_player,
                    m_player->origin,
                    m_player->viewheight,
                    vAngles,
                    m_player->velocity,
                    blend,
                    m_player->fov
                );
            } else {
                SetPlayerView(NULL, origin, viewheight, v_angle, velocity, blend, fov);
            }
        } else {
            SetPlayerView(NULL, origin, viewheight, v_angle, velocity, blend, fov);
        }
    } else {
        SetPlayerView(camera, origin, viewheight, v_angle, velocity, blend, camera->Fov());
    }
}

Vector Player::GetAngleToTarget(Entity *ent, str tag, float yawclamp, float pitchclamp, Vector baseangles)
{
    assert(ent);

    if (ent) {
        Vector        delta, angs;
        orientation_t tag_or;

        int tagnum = gi.Tag_NumForName(edict->tiki, tag.c_str());

        if (tagnum < 0) {
            return Vector(0, 0, 0);
        }

        GetTagPositionAndOrientation(tagnum, &tag_or);

        delta = ent->centroid - tag_or.origin;
        delta.normalize();

        angs = delta.toAngles();

        AnglesSubtract(angs, baseangles, angs);

        angs[PITCH] = AngleNormalize180(angs[PITCH]);
        angs[YAW]   = AngleNormalize180(angs[YAW]);

        if (angs[PITCH] > pitchclamp) {
            angs[PITCH] = pitchclamp;
        } else if (angs[PITCH] < -pitchclamp) {
            angs[PITCH] = -pitchclamp;
        }

        if (angs[YAW] > yawclamp) {
            angs[YAW] = yawclamp;
        } else if (angs[YAW] < -yawclamp) {
            angs[YAW] = -yawclamp;
        }

        return angs;
    } else {
        return Vector(0, 0, 0);
    }
}

void Player::DebugWeaponTags(int controller_tag, Weapon *weapon, str weapon_tagname)
{
    int           i;
    orientation_t bone_or, tag_weapon_or, barrel_or, final_barrel_or;

    GetTagPositionAndOrientation(edict->s.bone_tag[controller_tag], &bone_or);
    //G_DrawCoordSystem( Vector( bone_or.origin ), Vector( bone_or.axis[0] ), Vector( bone_or.axis[1] ), Vector( bone_or.axis[2] ), 20 );

    GetTagPositionAndOrientation(gi.Tag_NumForName(edict->tiki, weapon_tagname), &tag_weapon_or);
    //G_DrawCoordSystem( Vector( tag_weapon_or.origin ), Vector( tag_weapon_or.axis[0] ), Vector( tag_weapon_or.axis[1] ), Vector( tag_weapon_or.axis[2] ), 40 );

    weapon->GetRawTag("tag_barrel", &barrel_or);
    VectorCopy(tag_weapon_or.origin, final_barrel_or.origin);

    for (i = 0; i < 3; i++) {
        VectorMA(final_barrel_or.origin, barrel_or.origin[i], tag_weapon_or.axis[i], final_barrel_or.origin);
    }

    MatrixMultiply(barrel_or.axis, tag_weapon_or.axis, final_barrel_or.axis);
    //G_DrawCoordSystem( Vector( final_barrel_or.origin ), Vector( final_barrel_or.axis[0] ), Vector( final_barrel_or.axis[1] ), Vector( final_barrel_or.axis[2] ), 80 );

#if 0
   if ( g_crosshair->integer )
      {
      trace_t trace;
      Vector  start,end,ang,dir,delta;
      vec3_t  mat[3];

      AnglesToAxis( v_angle, mat );

      dir   = mat[0];
      start = final_barrel_or.origin;
      end   = start + ( dir *  MAX_MAP_BOUNDS ); 

      G_DrawCoordSystem( start, Vector( mat[0] ), Vector( mat[1] ), Vector( mat[2] ), 80 );
      
      trace = G_Trace( start, vec_zero, vec_zero, end, this, MASK_PROJECTILE|MASK_WATER, qfalse, "Crosshair" );
      crosshair->setOrigin( trace.endpos );

      delta = trace.endpos - start;
      float length = delta.length();
      float scale  = g_crosshair_maxscale->value * length / MAX_MAP_BOUNDS;
      
      if ( scale < 1 )
         scale = 1;

      crosshair->setScale( scale );

      if ( trace.ent )
         {
         vectoangles( trace.plane.normal, ang );
         }
      else
         {
         vectoangles( dir, ang );
         }

      crosshair->setAngles( ang );
      }
#endif
}

void Player::AcquireTarget(void) {}

void Player::RemoveTarget(Entity *ent_to_remove) {}

void Player::AutoAim(void) {}

/*
===============
PlayerAngles
===============
*/
void Player::PlayerAngles(void)
{
    if (getMoveType() == MOVETYPE_PORTABLE_TURRET) {
        PortableTurret *portableTurret = static_cast<PortableTurret *>(m_pTurret.Pointer());
        angles[0]                      = portableTurret->GetGroundPitch();
        angles[1]                      = portableTurret->GetStartYaw();
    }

    PmoveAdjustAngleSettings(v_angle, angles, &client->ps, &edict->s);

    // HZM coop - P1 prone fluidity: MUST run before ApplyCoopBoneOffsets so the aim-lead below
    // reads this frame's eased body yaw.
    CoopProneBodyYaw(angles);

    // HZM coop [user 2026-08-25] - MUST be immediately after the call above. PmoveAdjustAngleSettings
    // owns all four player bone controllers and rewrites them with VectorCopy every frame, so any
    // offset written earlier (e.g. from ClientThink) is erased before it is ever networked - which is
    // exactly how head tracking and torso lag shipped completely inert (bug-2101, measured).
    ApplyCoopBoneOffsets();

    SetViewAngles(v_angle);
    setAngles(angles);
}

void Player::FinishMove(void)
{
    //
    // If the origin or velocity have changed since ClientThink(),
    // update the pmove values.  This will happen when the client
    // is pushed by a bmodel or kicked by an explosion.
    //
    // If it wasn't updated here, the view position would lag a frame
    // behind the body position when pushed -- "sinking into plats",
    //
    if (!(client->ps.pm_flags & PMF_FROZEN) && !(client->ps.pm_flags & PMF_NO_MOVE)) {
        origin.copyTo(client->ps.origin);
        velocity.copyTo(client->ps.velocity);
    }

    // This check is in mohaa but the animation will look bad
    if (!(client->ps.pm_flags & PMF_FROZEN)) {
        PlayerAngles();
        AdjustAnimBlends();
    }

    // burn from lava, etc
    WorldEffects();

    // determine the view offsets
    DamageFeedback();
    CalcBlend();

    if (g_gametype->integer != GT_SINGLE_PLAYER && g_smoothClients->integer) {
        VectorCopy(client->ps.velocity, edict->s.pos.trDelta);
        edict->s.pos.trTime = client->ps.commandTime;
    } else {
        VectorClear(edict->s.pos.trDelta);
        edict->s.pos.trTime = 0;
    }
}

void Player::CopyStats(Player *player)
{
    gentity_t *ent;
    int        i;

    origin = player->origin;
    SetViewAngles(player->GetViewAngles());

    client->ps.bobCycle = player->client->ps.bobCycle;

    client->ps.pm_flags |=
        player->client->ps.pm_flags & (PMF_DUCKED | PMF_VIEW_DUCK_RUN | PMF_VIEW_JUMP_START | PMF_VIEW_PRONE);

    memcpy(&client->ps.stats, &player->client->ps.stats, sizeof(client->ps.stats));
    memcpy(&client->ps.activeItems, &player->client->ps.activeItems, sizeof(client->ps.activeItems));
    memcpy(&client->ps.ammo_name_index, &player->client->ps.ammo_name_index, sizeof(client->ps.ammo_name_index));
    memcpy(&client->ps.ammo_amount, &player->client->ps.ammo_amount, sizeof(client->ps.ammo_amount));
    memcpy(&client->ps.max_ammo_amount, &player->client->ps.max_ammo_amount, sizeof(client->ps.max_ammo_amount));

    VectorCopy(player->client->ps.origin, client->ps.origin);
    VectorCopy(player->client->ps.velocity, client->ps.velocity);

    client->ps.iViewModelAnim        = player->client->ps.iViewModelAnim;
    client->ps.iViewModelAnimChanged = player->client->ps.iViewModelAnimChanged;

    client->ps.gravity = player->client->ps.gravity;
    client->ps.speed   = player->client->ps.speed;

    // copy angles
    memcpy(&client->ps.delta_angles, &player->client->ps.delta_angles, sizeof(client->ps.delta_angles));

    memcpy(&client->ps.blend, &player->client->ps.blend, sizeof(client->ps.blend));
    memcpy(&client->ps.damage_angles, &player->client->ps.damage_angles, sizeof(client->ps.damage_angles));
    memcpy(&client->ps.viewangles, &player->client->ps.viewangles, sizeof(client->ps.delta_angles));

    // copy camera stuff
    //memcpy( &client->ps.camera_origin, &player->client->ps.camera_origin, sizeof( client->ps.camera_origin ) );
    //memcpy( &client->ps.camera_angles, &player->client->ps.camera_angles, sizeof( client->ps.camera_angles ) );
    //memcpy( &client->ps.camera_offset, &player->client->ps.camera_offset, sizeof( client->ps.camera_offset ) );
    //memcpy( &client->ps.camera_posofs, &player->client->ps.camera_posofs, sizeof( client->ps.camera_posofs ) );
    //client->ps.camera_time = player->client->ps.camera_time;
    //client->ps.camera_flags = player->client->ps.camera_flags;

    client->ps.fLeanAngle = player->client->ps.fLeanAngle;
    client->ps.fov        = player->client->ps.fov;

    client->ps.viewheight      = player->client->ps.viewheight;
    client->ps.walking         = player->client->ps.walking;
    client->ps.groundPlane     = player->client->ps.groundPlane;
    client->ps.groundEntityNum = player->client->ps.groundEntityNum;
    memcpy(&client->ps.groundTrace, &player->client->ps.groundTrace, sizeof(trace_t));

    edict->s.eFlags &= ~EF_UNARMED;
    edict->r.svFlags &= ~SVF_NOCLIENT;
    edict->s.renderfx &= ~RF_DONTDRAW;

    player->edict->r.svFlags |= SVF_NOTSINGLECLIENT;
    player->edict->r.singleClient = client->ps.clientNum;

    edict->r.svFlags |= SVF_SINGLECLIENT;
    edict->r.singleClient = client->ps.clientNum;

    client->ps.pm_flags |= PMF_FROZEN | PMF_NO_MOVE | PMF_NO_PREDICTION;

    memcpy(&edict->s.frameInfo, &player->edict->s.frameInfo, sizeof(edict->s.frameInfo));

    DetachAllChildren(NULL);

    for (i = 0; i < MAX_MODEL_CHILDREN; i++) {
        Entity *dest;

        if (player->children[i] == ENTITYNUM_NONE) {
            continue;
        }

        ent = g_entities + player->children[i];

        if (!ent->inuse || !ent->entity) {
            continue;
        }

        dest = new Entity;

        CloneEntity(dest, ent->entity);

        dest->edict->s.modelindex   = ent->entity->edict->s.modelindex;
        dest->edict->tiki           = ent->entity->edict->tiki;
        dest->edict->s.actionWeight = ent->entity->edict->s.actionWeight;
        memcpy(&dest->edict->s.frameInfo, &ent->entity->edict->s.frameInfo, sizeof(dest->edict->s.frameInfo));
        dest->CancelPendingEvents();
        dest->attach(entnum, ent->entity->edict->s.tag_num);

        dest->PostEvent(EV_DetachAllChildren, level.frametime);
    }
}

void Player::UpdateStats(void)
{
    int    i, count;
    Vector vObjectiveLocation;
    float  healthfrac;
    float  healfrac;

    //
    // Health
    //

    if (g_spectatefollow_firstperson->integer && IsSpectator() && m_iPlayerSpectating != 0) {
        //
        // Added in OPM
        //  First-person spectate
        //
        gentity_t *ent = g_entities + (m_iPlayerSpectating - 1);

        if (ent->inuse && ent->entity && ent->entity->deadflag <= DEAD_DYING) {
            CopyStats((Player *)ent->entity);
            return;
        }
    }

    if (g_gametype->integer == GT_SINGLE_PLAYER) {
        client->ps.stats[STAT_TEAM]              = TEAM_ALLIES;
        client->ps.stats[STAT_KILLS]             = 0;
        client->ps.stats[STAT_DEATHS]            = 0;
        client->ps.stats[STAT_HIGHEST_SCORE]     = 0;
        client->ps.stats[STAT_ATTACKERCLIENT]    = -1;
        client->ps.stats[STAT_INFOCLIENT]        = -1;
        client->ps.stats[STAT_INFOCLIENT_HEALTH] = 0;

        vObjectiveLocation = level.m_vObjectiveLocation;
    } else {
        client->ps.stats[STAT_TEAM] = dm_team;

        if (g_gametype->integer >= GT_TEAM && current_team != NULL) {
            client->ps.stats[STAT_KILLS]  = current_team->m_teamwins;
            client->ps.stats[STAT_DEATHS] = current_team->m_iDeaths;
        } else {
            client->ps.stats[STAT_KILLS]  = num_kills;
            client->ps.stats[STAT_DEATHS] = num_deaths;
        }

        if (g_gametype->integer < GT_TEAM) {
            gentity_t *ent;
            int        i;
            int        bestKills = -9999;

            // Get the best player
            for (i = 0, ent = g_entities; i < game.maxclients; i++, ent++) {
                if (!ent->inuse || !ent->client || !ent->entity) {
                    continue;
                }

                Player *p = (Player *)ent->entity;
                if (p->GetNumKills() > bestKills) {
                    bestKills = p->GetNumKills();
                }
            }

            client->ps.stats[STAT_HIGHEST_SCORE] = bestKills;
        } else {
            if (dmManager.GetTeamAxis()->m_teamwins > dmManager.GetTeamAllies()->m_teamwins) {
                client->ps.stats[STAT_HIGHEST_SCORE] = dmManager.GetTeamAxis()->m_teamwins;
            } else {
                client->ps.stats[STAT_HIGHEST_SCORE] = dmManager.GetTeamAllies()->m_teamwins;
            }
        }

        if (!pAttackerDistPointer) {
            client->ps.stats[STAT_ATTACKERCLIENT] = -1;
        } else if (fAttackerDispTime <= level.time && deadflag == DEAD_NO) {
            pAttackerDistPointer                  = NULL;
            client->ps.stats[STAT_ATTACKERCLIENT] = -1;
        } else {
            client->ps.stats[STAT_ATTACKERCLIENT] = pAttackerDistPointer->edict - g_entities;
        }

        client->ps.stats[STAT_INFOCLIENT]        = -1;
        client->ps.stats[STAT_INFOCLIENT_HEALTH] = 0;

        if (IsSpectator() || g_gametype->integer >= GT_TEAM) {
            if (m_iPlayerSpectating && IsSpectator()) {
                gentity_t *ent = g_entities + (m_iPlayerSpectating - 1);

                if (ent->inuse && ent->entity && deadflag < DEAD_DEAD) {
                    m_iInfoClient       = ent - g_entities;
                    m_iInfoClientHealth = ent->entity->health;
                    m_fInfoClientTime   = level.time;

                    float percent = ent->entity->health / ent->entity->max_health * 100.0f;

                    if (percent > 0.0f && percent < 1.0f) {
                        percent = 1.0f;
                    }

                    client->ps.stats[STAT_INFOCLIENT_HEALTH] = percent;
                }
            } else {
                Vector  vForward;
                trace_t trace;

                AngleVectors(m_vViewAng, vForward, NULL, NULL);

                Vector vEnd = m_vViewPos + vForward * 2048.0f;

                trace = G_Trace(m_vViewPos, vec_zero, vec_zero, vEnd, this, MASK_BEAM, qfalse, "infoclientcheck");

                if (trace.ent && trace.ent->entity && trace.ent->entity->IsSubclassOfPlayer() && !(trace.ent->r.svFlags & SVF_NOCLIENT)) {
                    Player *p = static_cast<Player *>(trace.ent->entity);

                    if (IsSpectator() || p->GetTeam() == GetTeam()) {
                        m_iInfoClient       = trace.ent - g_entities;
                        m_iInfoClientHealth = p->health;
                        m_fInfoClientTime   = level.time;

                        float percent = trace.ent->entity->health / trace.ent->entity->max_health * 100.0f;

                        if (percent > 0.0f && percent < 1.0f) {
                            percent = 1.0f;
                        }

                        client->ps.stats[STAT_INFOCLIENT_HEALTH] = percent;
                    }
                }
            }

            if (m_iInfoClient != -1) {
                if (level.time <= m_fInfoClientTime + 1.5f) {
                    client->ps.stats[STAT_INFOCLIENT]        = m_iInfoClient;
                    client->ps.stats[STAT_INFOCLIENT_HEALTH] = m_iInfoClientHealth;
                } else {
                    m_iInfoClient = -1;
                }
            }
        }

        if (g_gametype->integer >= GT_TOW || level.m_bForceTeamObjectiveLocation) {
            if (GetTeam() == TEAM_AXIS) {
                vObjectiveLocation = level.m_vAxisObjectiveLocation;
            } else if (GetTeam() == TEAM_ALLIES) {
                vObjectiveLocation = level.m_vAlliedObjectiveLocation;
            }
        } else {
            vObjectiveLocation = level.m_vObjectiveLocation;
        }

        if (g_protocol < protocol_e::PROTOCOL_MOHTA_MIN && vObjectiveLocation == vec_zero) {
            //
            // try to use the nearest teammate instead.
            // the reason is that mohaa 1.11 and below doesn't have a radar
            // for teammates
            //
            if (g_gametype->integer > GT_FFA && !IsDead() && !IsSpectator()) {
                gentity_t *ent;
                int        i;
                Player    *p;
                float      fNearest = 9999.0f;
                float      fLength;

                // match the compass direction to the nearest player
                for (i = 0, ent = g_entities; i < game.maxclients; i++, ent++) {
                    if (!ent->inuse || !ent->client || !ent->entity || ent->entity == this) {
                        continue;
                    }

                    p = (Player *)ent->entity;
                    if (p->IsDead() || p->IsSpectator() || p->dm_team != dm_team) {
                        continue;
                    }

                    fLength = (p->centroid - centroid).length();

                    if (fLength < fNearest) {
                        fNearest           = fLength;
                        vObjectiveLocation = p->centroid;
                    }
                }
            }
        }
    }

    if (m_pVehicle && !m_pTurret) {
        client->ps.stats[STAT_VEHICLE_HEALTH]     = m_pVehicle->health;
        client->ps.stats[STAT_VEHICLE_MAX_HEALTH] = m_pVehicle->max_health;
    }

    //
    // Health fraction
    //
    healthfrac = (health / max_health * 100.0f);

    if (m_pVehicle && !m_pTurret) {
        if (!m_pVehicle->isSubclassOf(FixedTurret)) {
            healthfrac = (m_pVehicle->health / m_pVehicle->max_health * 100.f);
        }
    }

    if (healthfrac < 1 && healthfrac > 0) {
        healthfrac = 1;
    }
    if (healthfrac < 0) {
        healthfrac = 0;
    }

    client->ps.stats[STAT_HEALTH] = healthfrac;

    //
    // Healing
    //
    if (m_fHealRate && (!m_pVehicle || m_pTurret || m_pVehicle->isSubclassOf(FixedTurret))) {
        healfrac = (health + m_fHealRate) / max_health * 100.f;
    } else {
        healfrac = 0;
    }
    if (healfrac < 1 && healfrac > 0) {
        healfrac = 1;
    }
    if (healfrac < 0) {
        healfrac = 0;
    }

    client->ps.stats[STAT_NEXTHEALTH] = healfrac;
    client->ps.stats[STAT_MAXHEALTH]  = 100;

    Weapon *activeweap = GetActiveWeapon(WEAPON_MAIN);

    client->ps.stats[STAT_WEAPONS]         = 0;
    client->ps.stats[STAT_EQUIPPED_WEAPON] = 0;
    client->ps.stats[STAT_AMMO]            = 0;
    client->ps.stats[STAT_MAXAMMO]         = 0;
    client->ps.stats[STAT_CLIPAMMO]        = 0;
    client->ps.stats[STAT_MAXCLIPAMMO]     = 0;

    client->ps.activeItems[ITEM_AMMO]   = -1;
    client->ps.activeItems[ITEM_WEAPON] = -1;
    client->ps.activeItems[2]           = -1;
    client->ps.activeItems[3]           = -1;
    client->ps.activeItems[4]           = -1;
    client->ps.activeItems[5]           = -1;

    if (m_pTurret) {
        client->ps.activeItems[ITEM_WEAPON] = m_pTurret->getIndex();
        if (getMoveType() == MOVETYPE_PORTABLE_TURRET || getMoveType() == MOVETYPE_TURRET) {
            // Use the turret's ammo
            client->ps.stats[STAT_CLIPAMMO]    = m_pTurret->ammo_in_clip[FIRE_PRIMARY];
            client->ps.stats[STAT_MAXCLIPAMMO] = m_pTurret->ammo_clip_size[FIRE_PRIMARY];
        }
    } else if (activeweap) {
        if (activeweap->m_bSecondaryAmmoInHud) {
            client->ps.stats[STAT_AMMO]           = AmmoCount(activeweap->GetAmmoType(FIRE_SECONDARY));
            client->ps.stats[STAT_MAXAMMO]        = MaxAmmoCount(activeweap->GetAmmoType(FIRE_SECONDARY));
            client->ps.stats[STAT_SECONDARY_AMMO] = AmmoCount(activeweap->GetAmmoType(FIRE_PRIMARY));
        } else {
            client->ps.stats[STAT_AMMO]    = AmmoCount(activeweap->GetAmmoType(FIRE_PRIMARY));
            client->ps.stats[STAT_MAXAMMO] = MaxAmmoCount(activeweap->GetAmmoType(FIRE_PRIMARY));
        }

        client->ps.stats[STAT_CLIPAMMO]    = activeweap->ClipAmmo(FIRE_PRIMARY);
        client->ps.stats[STAT_MAXCLIPAMMO] = activeweap->GetClipSize(FIRE_PRIMARY);

        client->ps.activeItems[ITEM_AMMO] = AmmoIndex(activeweap->GetAmmoType(FIRE_PRIMARY));

        // grenade and rockets must match the number of ammo
        if (client->ps.stats[STAT_MAXCLIPAMMO] == 1) {
            client->ps.stats[STAT_MAXAMMO]++;
            client->ps.stats[STAT_AMMO] += client->ps.stats[STAT_CLIPAMMO];
        }

        if (!activeweap->IsSubclassOfInventoryItem()) {
            client->ps.stats[STAT_EQUIPPED_WEAPON] = activeweap->GetWeaponClass();
        }

        client->ps.activeItems[ITEM_WEAPON] = activeweap->getIndex();
    } else if (m_pVehicle) {
        Entity *pEnt = m_pVehicle->QueryTurretSlotEntity(0);
        if (pEnt && pEnt->IsSubclassOfVehicleTurretGun()) {
            VehicleTurretGun *vt = static_cast<VehicleTurretGun *>(pEnt);

            client->ps.activeItems[ITEM_WEAPON]   = vt->getIndex();
            client->ps.stats[STAT_CLIPAMMO]       = vt->ammo_in_clip[FIRE_PRIMARY];
            client->ps.stats[STAT_MAXCLIPAMMO]    = vt->ammo_clip_size[FIRE_PRIMARY];
            client->ps.stats[STAT_SECONDARY_AMMO] = vt->GetWarmupFraction() * 100.f;
        }
    }

    //
    // set boss health
    //
    client->ps.stats[STAT_BOSSHEALTH] = bosshealth->value * 100.0f;

    if (bosshealth->value * 100.0f > 0 && client->ps.stats[STAT_BOSSHEALTH] == 0) {
        client->ps.stats[STAT_BOSSHEALTH] = 1;
    }

    // Set cinematic stuff

    client->ps.stats[STAT_CINEMATIC] = 0;

    if (level.cinematic) {
        client->ps.stats[STAT_CINEMATIC] = (1 << 0);
    }

    if (actor_camera) {
        client->ps.stats[STAT_CINEMATIC] += (1 << 1);
    }

    count = inventory.NumObjects();

    int iItem = 0;

    for (i = 1; i <= count; i++) {
        int     entnum = inventory.ObjectAt(i);
        Weapon *weapon = (Weapon *)G_GetEntity(entnum);
        int     weapon_class;

        if (weapon && weapon->IsSubclassOfWeapon()) { // HZM 07-19 (bug-920): stale slot guard (live dump: UpdateStats crash)
            if (weapon->IsSubclassOfInventoryItem()) {
                if (iItem > 3) {
                    weapon->SetItemSlot(0);
                } else {
                    client->ps.activeItems[iItem + 2] = weapon->getIndex();
                    weapon->SetItemSlot(WEAPON_CLASS_ITEM1 << iItem);

                    if (activeweap && weapon == activeweap) {
                        client->ps.stats[STAT_EQUIPPED_WEAPON] = WEAPON_CLASS_ITEM1 << iItem;
                    }

                    iItem++;
                }
            } else {
                weapon_class = weapon->GetWeaponClass();

                if (weapon_class & WEAPON_CLASS_GRENADE) {
                    if (weapon->HasAmmo(FIRE_PRIMARY)) {
                        client->ps.stats[STAT_WEAPONS] |= weapon_class;
                    }
                } else {
                    client->ps.stats[STAT_WEAPONS] |= weapon_class;
                }
            }
        }
    }

    // Go through all the player's ammo and send over the names/amounts
    memset(client->ps.ammo_amount, 0, sizeof(client->ps.ammo_amount));
    memset(client->ps.ammo_name_index, 0, sizeof(client->ps.ammo_name_index));
    memset(client->ps.max_ammo_amount, 0, sizeof(client->ps.max_ammo_amount));

    count = ammo_inventory.NumObjects();

    for (i = 1; i <= count; i++) {
        Ammo *ammo = ammo_inventory.ObjectAt(i);

        if (ammo) {
            client->ps.ammo_amount[i - 1]     = ammo->getAmount();
            client->ps.max_ammo_amount[i - 1] = ammo->getMaxAmount();
            client->ps.ammo_name_index[i - 1] = ammo->getIndex();
        }
    }

    if (m_iInZoomMode == -1) {
        client->ps.stats[STAT_INZOOM] = fov;
    } else {
        client->ps.stats[STAT_INZOOM] = 0;
    }

    // HZM coop - replicate the cover pose to the client (auto third-person while covered;
    // drops the same frame cover ends, so the view snaps back to the player's own choice)
    if (m_bCoopCoverWall || m_bCoopCoverLow) {
        client->ps.pm_flags |= PMF_COOP_COVER;
    } else {
        client->ps.pm_flags &= ~PMF_COOP_COVER;
    }

    // HZM coop [user 2026-08-23, bug-2090] TELL THE CLIENT WHICH COVER IT IS.
    // USER: "these need to be separately controlled. My crouch cover angle was perfect."
    // PMF_COOP_COVER is set for BOTH the standing wall pose and the crouch pose, so any client-side
    // work gated on that flag silently applies to both - which is how a wall-camera experiment
    // damaged a crouch framing that was already right. There was no way for cgame to distinguish
    // them at all; coop_coverSide was the only hint and it is a side, not a type, so it goes stale.
    // 0 = not covered, 1 = WALL (standing, back to wall), 2 = LOW (crouch). Change-only push:
    // integer payload, no embedded quote (TRAPS T8), and the coop_ prefix is allowed through
    // cg_servercmds_filter.cpp:173 like every other script->client bridge.
    {
        int iCovType = m_bCoopCoverWall ? 1 : (m_bCoopCoverLow ? 2 : 0);

        if (iCovType != m_iCoopCoverTypeLast) {
            m_iCoopCoverTypeLast = iCovType;
            gi.SendServerCommand(edict - g_entities, "stufftext \"set coop_coverType %d\"\n", iCovType);
        }
    }

    client->ps.stats[STAT_CROSSHAIR] =
        ((!client->ps.stats[STAT_INZOOM] || client->ps.stats[STAT_INZOOM] > 30)
         && (activeweap && !activeweap->IsSubclassOfInventoryItem() && activeweap->GetUseCrosshair()))
        || m_pTurret || (m_pVehicle && m_pVehicle->IsSubclassOfVehicleTank());

    client->ps.stats[STAT_COMPASSNORTH] = ANGLE2SHORT(world->m_fNorth);

    if (VectorCompare(vObjectiveLocation, vec_zero)) {
        client->ps.stats[STAT_OBJECTIVELEFT]   = 1730;
        client->ps.stats[STAT_OBJECTIVERIGHT]  = 1870;
        client->ps.stats[STAT_OBJECTIVECENTER] = 1800;
    } else {
        Vector vDelta;
        float  yaw;
        float  fOffset;

        vDelta = vObjectiveLocation - centroid;
        yaw    = AngleSubtract(v_angle[1], vDelta.toYaw()) + 180.0f;

        vDelta  = yaw_left * 300.0f + yaw_forward * vDelta.length() + centroid - centroid;
        fOffset = AngleSubtract(vDelta.toYaw(), v_angle[1]);
        if (fOffset < 0.0f) {
            fOffset = -fOffset;
        }

        fOffset = 53.0f - fOffset + 7.0f;
        if (fOffset < 7.0f) {
            fOffset = 7.0f;
        }

        client->ps.stats[STAT_OBJECTIVELEFT] = anglemod(yaw - fOffset) * 10.0f;
        if (client->ps.stats[STAT_OBJECTIVELEFT] <= 0) {
            client->ps.stats[STAT_OBJECTIVELEFT] = 1;
        } else if (client->ps.stats[STAT_OBJECTIVELEFT] > 3599) {
            client->ps.stats[STAT_OBJECTIVELEFT] = 3599;
        }

        client->ps.stats[STAT_OBJECTIVERIGHT] = anglemod(yaw + fOffset) * 10.0f;
        if (client->ps.stats[STAT_OBJECTIVERIGHT] <= 0) {
            client->ps.stats[STAT_OBJECTIVERIGHT] = 1;
        } else if (client->ps.stats[STAT_OBJECTIVERIGHT] > 3599) {
            client->ps.stats[STAT_OBJECTIVERIGHT] = 3599;
        }

        client->ps.stats[STAT_OBJECTIVECENTER] = anglemod(yaw) * 10.0f;
        if (client->ps.stats[STAT_OBJECTIVECENTER] <= 0) {
            client->ps.stats[STAT_OBJECTIVECENTER] = 1;
        } else if (client->ps.stats[STAT_OBJECTIVECENTER] > 3599) {
            client->ps.stats[STAT_OBJECTIVECENTER] = 3599;
        }
    }

    client->ps.stats[STAT_DAMAGEDIR] = damage_yaw;
    if (client->ps.stats[STAT_DAMAGEDIR] < 0) {
        client->ps.stats[STAT_DAMAGEDIR] = 0;
    } else if (client->ps.stats[STAT_DAMAGEDIR] > 3600) {
        client->ps.stats[STAT_DAMAGEDIR] = 3600;
    }

    // Do letterbox

    // Check for letterbox fully out
    if ((level.m_letterbox_time <= 0) && (level.m_letterbox_dir == letterbox_in)) {
        client->ps.stats[STAT_LETTERBOX] = level.m_letterbox_fraction * MAX_LETTERBOX_SIZE;
        return;
    } else if ((level.m_letterbox_time <= 0) && (level.m_letterbox_dir == letterbox_out)) {
        client->ps.stats[STAT_LETTERBOX] = 0;
        return;
    }

    float frac;

    level.m_letterbox_time -= level.frametime;

    frac = level.m_letterbox_time / level.m_letterbox_time_start;

    if (frac > 1) {
        frac = 1;
    }
    if (frac < 0) {
        frac = 0;
    }

    if (level.m_letterbox_dir == letterbox_in) {
        frac = 1.0f - frac;
    }

    client->ps.stats[STAT_LETTERBOX] = (frac * level.m_letterbox_fraction) * MAX_LETTERBOX_SIZE;
}

void Player::UpdateMusic(void)
{
    // Always copy mood to snapshot so trigger_music entities propagate correctly.
    // The original guard (music_forced) meant non-forced mood changes (trigger_music,
    // spawn success cue) never reached cgame. mood_forced is preserved for callers
    // that need to override the SoundManager, but snapshot write is unconditional.
    client->ps.current_music_mood  = music_current_mood;
    client->ps.fallback_music_mood = music_fallback_mood;

    // Copy music volume and fade time to player state
    client->ps.music_volume           = music_current_volume;
    client->ps.music_volume_fade_time = music_volume_fade_time;
}

void Player::SetReverb(int type, float level)
{
    reverb_type  = type;
    reverb_level = level;
}

void Player::SetReverb(str type, float level)
{
    reverb_type  = EAXMode_NameToNum(type);
    reverb_level = level;
}

void Player::SetReverb(Event *ev)
{
    if (ev->NumArgs() < 2) {
        return;
    }

    SetReverb(ev->GetInteger(1), ev->GetFloat(2));
}

void Player::UpdateReverb(void)
{
    client->ps.reverb_type  = reverb_type;
    client->ps.reverb_level = reverb_level;
}

void Player::UpdateMisc(void)
{
    //
    // clear out the level exit flag
    //
    client->ps.pm_flags &= ~PMF_LEVELEXIT;

    //
    // see if our camera is the level exit camera
    //
    if (camera && camera->IsLevelExit()) {
        client->ps.pm_flags |= PMF_LEVELEXIT;
    } else if (level.near_exit) {
        client->ps.pm_flags |= PMF_LEVELEXIT;
    }

    //
    // do anything special for respawns
    //
    if (client->ps.pm_flags & PMF_RESPAWNED) {
        //
        // change music
        //
        if (music_current_mood != mood_success) {
            ChangeMusic("success", "normal", false);
        }
    }
}

/*
=================
EndFrame

Called for each player at the end of the server frame
and right after spawning
=================
*/
void Player::EndFrame(void)
{
    FinishMove();
    UpdateStats();
    UpdateMusic();
    UpdateReverb();
    UpdateMisc();

    if (!g_spectatefollow_firstperson->integer || !IsSpectator() || !m_iPlayerSpectating) {
        SetupView();
    } else {
        gentity_t *ent = g_entities + m_iPlayerSpectating - 1;

        if (!ent->inuse || !ent->entity || ent->entity->deadflag >= DEAD_DEAD) {
            SetupView();
        }
    }
}

void Player::GotKill(Event *ev)
{
    /*
    Entity *victim;
   Entity *inflictor;
   float   damage;
   int     meansofdeath;
   qboolean gibbed;

   if ( deathmatch->integer )
        {
      return;
        }

    victim = ev->GetEntity( 1 );
    damage = ev->GetInteger( 2 );
    inflictor = ev->GetEntity( 3 );
    meansofdeath = ev->GetInteger( 4 );
    gibbed = ev->GetInteger( 5 );
*/
}

void Player::SetPowerupTimer(Event *ev)
{
    Event *event;

    poweruptimer = ev->GetInteger(1);
    poweruptype  = ev->GetInteger(2);
    event        = new Event(EV_Player_UpdatePowerupTimer);
    PostEvent(event, 1);
}

void Player::UpdatePowerupTimer(Event *ev)
{
    poweruptimer -= 1;
    if (poweruptimer > 0) {
        PostEvent(ev, 1);
    } else {
        poweruptype = 0;
    }
}

void Player::ChangeMusic(const char *current, const char *fallback, qboolean force)
{
    int current_mood_num;
    int fallback_mood_num;

    music_forced = force;

    if (current) {
        current_mood_num = MusicMood_NameToNum(current);
        if (current_mood_num < 0) {
            gi.DPrintf("current music mood %s not found", current);
        } else {
            music_current_mood = current_mood_num;
        }
    }

    if (fallback) {
        fallback_mood_num = MusicMood_NameToNum(fallback);
        if (fallback_mood_num < 0) {
            gi.DPrintf("fallback music mood %s not found", fallback);
            fallback = NULL;
        } else {
            music_fallback_mood = fallback_mood_num;
        }
    }
}

void Player::ChangeMusicVolume(float volume, float fade_time)
{
    music_volume_fade_time = fade_time;
    music_saved_volume     = music_current_volume;
    music_current_volume   = volume;
}

void Player::RestoreMusicVolume(float fade_time)
{
    music_volume_fade_time = fade_time;
    music_current_volume   = music_saved_volume;
    music_saved_volume     = -1.0;
}

void Player::addOrigin(Vector org)
{
    setLocalOrigin(localorigin + org);

    animspeed         = org.x * (1.f / level.frametime);
    airspeed          = org.y * (1.f / level.frametime);
    m_vPushVelocity.x = org.z * (1.f / level.frametime);
}

void Player::Jump(Event *ev)
{
    float maxheight;

    if (m_pTurret || m_pVehicle) {
        // Don't jump when inside a vehicle or turret
        return;
    }

    if (g_gametype->integer != GT_SINGLE_PLAYER) {
        // Added in 2.0
        //  Don't jump when on top of another sentient
        if (groundentity && groundentity->entity && groundentity->entity->IsSubclassOfSentient()) {
            return;
        }
    }

    maxheight = ev->GetFloat(1);

    if (maxheight > 16) {
        // v^2 = 2ad
        velocity[2] += sqrt(2 * sv_gravity->integer * maxheight);

        if (client->ps.groundEntityNum != ENTITYNUM_NONE) {
            velocity += m_vPushVelocity;
        }

        // make sure the player leaves the ground
        client->ps.walking = qfalse;

        // Added in 2.0
        m_bHasJumped = true;
    }
}

void Player::JumpXY(Event *ev)
{
    float forwardmove;
    float sidemove;
    float distance;
    float time;
    float speed;

    if (m_pTurret || m_pVehicle) {
        // Don't jump when inside a vehicle or turret
        return;
    }

    forwardmove = ev->GetFloat(1);
    sidemove    = ev->GetFloat(2);
    speed       = ev->GetFloat(3);

    velocity = yaw_forward * forwardmove - yaw_left * sidemove;
    distance = velocity.length();
    velocity *= speed / distance;
    time        = distance / speed;
    velocity[2] = sv_gravity->integer * time * 0.5f;

    if (client->ps.groundEntityNum != ENTITYNUM_NONE) {
        velocity += G_GetEntity(client->ps.groundEntityNum)->velocity;
    }

    airspeed = distance;

    // make sure the player leaves the ground
    client->ps.walking = qfalse;
}

// HZM coop - accumulate an upward view-recoil kick (degrees). Capped so sustained auto climbs to a limit
// rather than running away. Player::Think eases it back down and folds it into delta_angles.
void Player::AddViewRecoil(float fPitch)
{
    m_fRecoilTarget += fPitch;
    if (m_fRecoilTarget > 6.0f) {
        m_fRecoilTarget = 6.0f;
    }
}

void Player::SetViewAngles(Vector newViewangles)
{
    // set the delta angle
    client->ps.delta_angles[0] = ANGLE2SHORT(newViewangles.x - client->cmd_angles[0]);
    client->ps.delta_angles[1] = ANGLE2SHORT(newViewangles.y - client->cmd_angles[1]);
    client->ps.delta_angles[2] = ANGLE2SHORT(newViewangles.z - client->cmd_angles[2]);

    v_angle = newViewangles;
    // Fixed in OPM
    //  Normalize angles to the range (-180, +180)
    //  so interpolation is done properly client-side
    v_angle.EulerNormalize();

    // get the pitch and roll from our leg angles
    newViewangles.x = angles.x;
    newViewangles.z = angles.z;
    AnglesToMat(newViewangles, orientation);
    yaw_forward = orientation[0];
    yaw_left    = orientation[1];
}

void Player::SetTargetViewAngles(Vector angles)
{
    v_angle = angles;
}

void Player::DumpState(Event *ev)
{
    gi.DPrintf(
        "Legs: %s Torso: %s\n", currentState_Legs ? currentState_Legs->getName() : "NULL", currentState_Torso->getName()
    );
}

void Player::ForceTorsoState(Event *ev)
{
    State *ts = statemap_Torso->FindState(ev->GetString(1));
    // HZM coop - a missing state used to be a SILENT no-op (FindState returns NULL and
    // EvaluateState just re-evaluates) - cost hours on the emote feature. Say so.
    if (!ts) {
        gi.Printf("^~^~^ ForceTorsoState: state '%s' not found in %s\n", ev->GetString(1).c_str(), g_statefile->string);
        return;
    }
    EvaluateState(ts);
}

void Player::ForceLegsState(Event *ev)
{
    State *ls = statemap_Legs->FindState(ev->GetString(1));
    if (!ls) {
        gi.Printf("^~^~^ ForceLegsState: state '%s' not found in %s\n", ev->GetString(1).c_str(), g_statefile->string);
        return;
    }
    EvaluateState(NULL, ls);
}

void Player::TouchedUseAnim(Entity *ent)
{
    toucheduseanim = ent;
}

void Player::NextPainTime(Event *ev)
{
    float time = ev->GetFloat(1);

    nextpaintime = level.time + time;

    if (time >= 0.0f) {
        pain          = 0.0f;
        pain_type     = MOD_NONE;
        pain_location = HITLOC_MISS;

        m_pLegsPainCond->clearCheck();
        m_pTorsoPainCond->clearCheck();
    }
}

void Player::EnterVehicle(Event *ev)
{
    Entity *ent;

    ent = ev->GetEntity(1);
    if (ent && ent->IsSubclassOfVehicle()) {
        flags |= FL_PARTIAL_IMMOBILE;
        viewheight = STAND_EYE_HEIGHT;
        velocity   = vec_zero;
        m_pVehicle = (Vehicle *)ent;
        if (m_pVehicle->IsDrivable()) {
            setMoveType(MOVETYPE_VEHICLE);
        } else {
            setMoveType(MOVETYPE_NOCLIP);
        }

        SafeHolster(true);
    }
}

void Player::ExitVehicle(Event *ev)
{
    flags &= ~FL_PARTIAL_IMMOBILE;
    setMoveType(MOVETYPE_WALK);
    m_pVehicle = NULL;

    if (camera) {
        SetCamera(NULL, 0.5f);
        ZoomOff();
    }

    SafeHolster(false);
    takedamage = DAMAGE_YES;
    setSolidType(SOLID_BBOX);
}

void Player::EnterTurret(TurretGun *ent)
{
    flags |= FL_PARTIAL_IMMOBILE;
    viewheight = DEFAULT_VIEWHEIGHT;
    velocity   = vec_zero;
    m_pTurret  = ent;

    if (ent->inheritsFrom(PortableTurret::classinfostatic())) {
        // carryable turret
        setMoveType(MOVETYPE_PORTABLE_TURRET);
        StopPartAnimating(torso);
        SetPartAnim("mg42tripod_aim_straight_straight");
    } else {
        // standard turret
        setMoveType(MOVETYPE_TURRET);
    }

    SafeHolster(true);
}

void Player::EnterTurret(Event *ev)
{
    TurretGun *ent = (TurretGun *)ev->GetEntity(1);

    if (!ent) {
        return;
    }

    if (!ent->inheritsFrom(TurretGun::classinfostatic())) {
        return;
    }

    EnterTurret(ent);
}

void Player::ExitTurret(void)
{
    if (m_pTurret->inheritsFrom(PortableTurret::classinfostatic())) {
        StopPartAnimating(torso);
        SetPartAnim("mg42tripod_aim_straight_straight");
    }

    flags &= ~FL_PARTIAL_IMMOBILE;
    setMoveType(MOVETYPE_WALK);
    m_pTurret = NULL;

    SafeHolster(qfalse);

    new_buttons        = 0;
    server_new_buttons = 0;
}

void Player::ExitTurret(Event *ev)
{
    ExitTurret();
}

void Player::HolsterToggle(Event *ev)
{
    if (deadflag) {
        return;
    }

    if (WeaponsOut()) {
        // fucking compiler bug
        // it won't call the parent's override function
        ((Sentient *)this)->Holster(qtrue);
    } else {
        ((Sentient *)this)->Holster(qfalse);
    }
}

void Player::Holster(Event *ev)
{
    SafeHolster(ev->GetBoolean(1));
}

//
// HZM coop lobby - freeze the player as a parade-rest mannequin.
//
// The coop lobby seats each player at a fixed slot for a shared static camera. Posing them at ease
// from script alone loses a per-frame fight: the anim state machine (EvaluateState) re-runs every tick,
// and while the forced-respawn deploy leaves the weapon in the "banned -> re-allowed rifle" limbo the
// weapon-change edges keep firing - STAND torso "RAISE_WEAPON : NEW_WEAPON" redraws the rifle, and
// EMOTE_ATEASE legs "STAND : +/-HAS_WEAPON" kicks the pose out - so the rifle twitches in his hands.
//
// This does it the robust way, in one shot, at the point EvaluateState is defined:
//   1. force EMOTE_ATEASE legs + STAND (action none) torso WHILE the statemap is still live,
//   2. sling the main weapon straight onto the back holster tag (no putaway animation, no statemap
//      dependency - AttachToHolster just re-parents the gun), and
//   3. set FL_IMMOBILE, which makes EvaluateState early-return (see EvaluateState above), so no edge can
//      ever fire again: the rifle can't be redrawn and the pose can't be twitched.
// Paired with coop_lobbyunpose to release before the mission launches.
//
void Player::CoopLobbyPose(Event *ev)
{
    // The forced-respawn lobby deploy can leave the player noclip-flying: InitEdict sets MOVETYPE_NOCLIP
    // whenever m_bSpectator is still true (player.cpp:2380), and the script 'respawn' skips the
    // EndSpectator() that the fire-click deploy does. NOCLIP also makes EvaluateState early-return
    // (player.cpp:5567), so the pose freezes but the body flies on WASD/space/ctrl. Force a normal
    // grounded walker so movement can actually be locked.
    if (IsSpectator()) {
        EndSpectator();
    }
    setMoveType(MOVETYPE_WALK);

    // Sling the wielded main weapon onto the back tag FIRST, so HAS_WEAPON / weapon-out is stable before
    // we force the at-ease state. If we pose first (weapon still out), EMOTE_ATEASE immediately takes its
    // "STAND : +/-HAS_WEAPON" exit back to STAND (that is why the pose came up as a plain stand). Kept in
    // inventory so HAS_WEAPON stays true for the legs state, but out of the hands.
    {
        Weapon *rightWeap = GetActiveWeapon(WEAPON_MAIN);
        if (rightWeap) {
            rightWeap->AttachToHolster(WEAPON_MAIN);
            holsteredWeapon = rightWeap;
        }
    }

    // Static hands-on-hips parade rest: torso STAND (action none) + legs EMOTE_LOBBY_SELECT (plays
    // coop_pose_g100 = misc/00G100_Axis_idle.skc, the hands-on-hips pose), then FL_IMMOBILE.
    //
    // FL_IMMOBILE IS REQUIRED here (this is exactly what the coop_lobbycycleanim dev tool does, which is
    // how the pose was confirmed to render hands-on-hips). Without it the per-frame EvaluateState keeps
    // running and the TORSO drifts off STAND (weapon-carry) which pulls the arms back to the sides,
    // overriding g100's hands-on-hips -> "just standing". Freezing holds torso=STAND so the g100 legs clip
    // owns the arms. The clip still self-loops (living idle). No cycling in this single-pose build.
    if (statemap_Torso) {
        State *torso = statemap_Torso->FindState("STAND");
        if (torso) {
            EvaluateState(torso, NULL);
        }
    }

    // Set the legs anim DIRECTLY (exactly what the coop_lobbycycleanim dev tool does, which rendered
    // hands-on-hips). Do NOT route this through EvaluateState(EMOTE_LOBBY_SELECT): EvaluateState
    // re-evaluates the TORSO too and lets it drift off STAND (arms snap back to the sides, overriding
    // g100). SetPartAnim touches only the legs channel, so torso stays the STAND we just forced and the
    // g100 clip owns the arms (on hips). coop_pose_g100 = misc/00G100_Axis_idle.skc.
    SetPartAnim("coop_pose_g100", legs);

    flags |= FL_IMMOBILE;
}

void Player::CoopLobbyUnpose(Event *ev)
{
    flags &= ~FL_IMMOBILE;

    if (holsteredWeapon) {
        useWeapon(holsteredWeapon, WEAPON_MAIN);
        holsteredWeapon = NULL;
    }
}

//
// HZM coop lobby - swap the frozen mannequin's idle pose (lobby idle-pose rotation).
//
// Momentarily lifts the FL_IMMOBILE freeze so EvaluateState will actually apply the forced states,
// forces STAND torso + the requested legs state, then re-freezes - all inside this single call, so the
// per-frame EvaluateState never ticks in between (no unfrozen window = no weapon/pose twitch). The
// weapon was already slung onto the back tag by CoopLobbyPose and is deliberately left untouched.
//
void Player::CoopLobbyRepose(Event *ev)
{
    str legsName = ev->GetString(1);

    flags &= ~FL_IMMOBILE;

    if (statemap_Torso) {
        State *torso = statemap_Torso->FindState("STAND");
        if (torso) {
            EvaluateState(torso, NULL);
        }
    }
    if (statemap_Legs) {
        State *legs = statemap_Legs->FindState(legsName);
        if (legs) {
            EvaluateState(NULL, legs);
        }
    }

    flags |= FL_IMMOBILE;
}

//
// HZM coop lobby DEV tool - cycle candidate standing idles to visually identify a pose (e.g. hands-on-hips).
// Bound to keys (numpad +/-). Sets the legs directly to the next/prev candidate anim (torso STAND so the
// full-body clip owns the skeleton), holds it, and prints the anim name on screen + console.
//
static int s_coopLobbyPoseIdx = 0;
static const char *s_coopLobbyPoseList[] = {
    "coop_pose_offic",   "coop_pose_g100",     "coop_pose_g101",
    "coop_pose_stand1",  "coop_pose_stand2",   "coop_pose_stand3",
    "coop_pose_stand4",  "coop_pose_stand5",   "coop_pose_generic",
    "coop_pose_neutral1","coop_pose_neutral2", "coop_pose_a100",
    "coop_pose_atease"
};

void Player::CoopLobbyCycleAnim(Event *ev)
{
    const int n   = (int)(sizeof(s_coopLobbyPoseList) / sizeof(s_coopLobbyPoseList[0]));
    int       dir = (ev->GetInteger(1) < 0) ? -1 : 1;

    s_coopLobbyPoseIdx = (s_coopLobbyPoseIdx + dir + n) % n;
    const char *alias = s_coopLobbyPoseList[s_coopLobbyPoseIdx];

    // lift the freeze, force STAND torso (action none), override the legs with the candidate anim, re-freeze
    flags &= ~FL_IMMOBILE;
    if (statemap_Torso) {
        State *torso = statemap_Torso->FindState("STAND");
        if (torso) {
            EvaluateState(torso, NULL);
        }
    }
    SetPartAnim(alias, legs);
    flags |= FL_IMMOBILE;

    gi.SendServerCommand(
        edict - g_entities, "print \"" HUD_MESSAGE_WHITE "lobby pose %d/%d: %s\n\"", s_coopLobbyPoseIdx, n - 1, alias
    );
    Com_Printf("^~^~^ LOBBY POSE %d: %s\n", s_coopLobbyPoseIdx, alias);
}

//
// HZM coop lobby - per-frame re-assert of the hands-on-hips pose (lobby.scr::lobbyLockWatch).
//
// coop_lobbypose sets the pose once at spawn, but the spawn/weapon settle churn then clobbers the legs
// anim (direct SetPartAnim from the weapon/anim code bypasses the FL_IMMOBILE statemap freeze), leaving
// him arms-at-side. This re-asserts the EXACT pose the cycler uses every frame, which just corrects any
// clobber and is a no-op when already correct (SetPartAnim early-returns on the same anim). Torso is
// re-forced to STAND (action none) so the g100 legs clip owns the arms; FL_IMMOBILE is briefly lifted so
// EvaluateState can apply, then re-set - all within this one synchronous call, so no drift window.
//
void Player::CoopLobbyHoldPose(Event *ev)
{
    flags &= ~FL_IMMOBILE;
    if (statemap_Torso) {
        State *torso = statemap_Torso->FindState("STAND");
        if (torso) {
            EvaluateState(torso, NULL);
        }
    }
    SetPartAnim("coop_pose_g100", legs);
    flags |= FL_IMMOBILE;
}

void Player::WatchActor(Event *ev)
{
    if (camera || currentState_Torso->getCameraType() != CAMERA_BEHIND) {
        return;
    }

    actor_to_watch = (Actor *)ev->GetEntity(1);
}

void Player::StopWatchingActor(Event *ev)
{
    Actor *old_actor;

    old_actor = (Actor *)ev->GetEntity(1);

    if (old_actor == actor_to_watch) {
        actor_to_watch = NULL;
    }
}

void Player::setAngles(Vector ang)
{
    // set the angles normally

    if (bindmaster) {
        ang -= bindmaster->angles;
    }

    Entity::setAngles(ang);
}

painDirection_t Player::Pain_string_to_int(str pain)
{
    if (!pain.icmp(pain, "Front")) {
        return PAIN_FRONT;
    } else if (!pain.icmp(pain, "Left")) {
        return PAIN_LEFT;
    } else if (!pain.icmp(pain, "Right")) {
        return PAIN_RIGHT;
    } else if (!pain.icmp(pain, "Rear")) {
        return PAIN_REAR;
    } else {
        return PAIN_NONE;
    }
}

void Player::ArchivePersistantData(Archiver& arc)
{
    str model_name;
    str name;

    Sentient::ArchivePersistantData(arc);

    model_name = g_playermodel->string;

    arc.ArchiveString(&model_name);

    if (arc.Loading()) {
        // set the cvar
        gi.cvar_set("g_playermodel", model_name.c_str());

        setModel("models/player/" + model_name + ".tik");
    }

    if (arc.Saving()) {
        if (holsteredWeapon) {
            name = holsteredWeapon->getName();
        } else {
            name = "none";
        }
    }

    arc.ArchiveString(&name);
    if (arc.Loading() && name != "none") {
        holsteredWeapon = (Weapon *)FindItem(name);
    }

    UpdateWeapons();

    // Force a re-evaluation of the player's state
    LoadStateTable();
}

void Player::VelocityModified(void) {}

int Player::GetKnockback(int original_knockback, qboolean blocked)
{
    int new_knockback;

    new_knockback = original_knockback - 50;

    // See if we still have enough knockback to knock the player down
    if (new_knockback >= 200 && take_pain) {
        knockdown = true;

        if (blocked) {
            float damage;

            damage = new_knockback / 50;

            if (damage > 10) {
                damage = 10;
            }

            Damage(world, world, damage, origin, vec_zero, vec_zero, 0, DAMAGE_NO_ARMOR, MOD_CRUSH);
        }
    }

    // Make sure knockback is still at least 0

    if (new_knockback < 0) {
        new_knockback = 0;
    }

    return new_knockback;
}

void Player::ResetHaveItem(Event *ev)
{
    str             fullname;
    ScriptVariable *var;

    fullname = str("playeritem_") + ev->GetString(1);

    var = game.vars->GetVariable(fullname.c_str());

    if (var) {
        var->setIntValue(0);
    }
}

void Player::ReceivedItem(Item *item) {}

void Player::RemovedItem(Item *item) {}

void Player::AmmoAmountChanged(Ammo *ammo, int ammo_in_clip)
{
    str             fullname;
    ScriptVariable *var;

    //
    // set our level variables
    //
    fullname = str("playerammo_") + ammo->getName();

    var = level.vars->GetVariable(fullname.c_str());
    if (!var) {
        level.vars->SetVariable(fullname.c_str(), ammo->getAmount() + ammo_in_clip);
    } else {
        var->setIntValue(ammo->getAmount() + ammo_in_clip);
    }
}

void Player::WaitForState(Event *ev)
{
    waitForState = ev->GetString(1);
}

void Player::SetDamageMultiplier(Event *ev)
{
    damage_multiplier = ev->GetFloat(1);
}

void Player::SetTakePain(Event *ev)
{
    take_pain = ev->GetBoolean(1);
}

void Player::Loaded(void)
{
    UpdateWeapons();
}

void Player::PlayerShowModel(Event *ev)
{
    Entity::showModel();
    UpdateWeapons();
}

void Player::showModel(void)
{
    Entity::showModel();
    UpdateWeapons();
}

Vector Player::EyePosition(void)
{
    return m_vViewPos;
}

void Player::ModifyHeight(Event *ev)
{
    str height = ev->GetString(1);

    if (!height.icmp("stand")) {
        viewheight   = DEFAULT_VIEWHEIGHT;
        maxs.z       = 94.0f;
        m_bHasJumped = false;
    } else if (!height.icmp("jumpstart")) {
        if (g_protocol < protocol_e::PROTOCOL_MOHTA_MIN) {
            viewheight = JUMP_START_VIEWHEIGHT;
        }
        maxs.z = 94.0f;
    } else if (!height.icmp("duck")) {
        viewheight = CROUCH_VIEWHEIGHT;
        maxs.z     = 54.0f;
    } else if (!height.icmp("duckrun")) {
        viewheight = CROUCH_RUN_VIEWHEIGHT;
        maxs.z     = 60.0f;
    } else if (!height.icmp("prone")) {
        //
        // Added in OPM
        //  (prone)
        viewheight = PRONE_VIEWHEIGHT;
        maxs.z     = 20.0f;
    } else {
        gi.Printf("Unknown modheight '%s' defaulting to stand\n", height.c_str());
        viewheight = DEFAULT_VIEWHEIGHT;
        maxs.z     = 94.0f;
    }
}

// Specify the view height of the player and the height of his bounding box
void Player::ModifyHeightFloat(Event *ev)
{
    // params
    int   height;
    float max_z;

    height = ev->GetInteger(1);
    max_z  = ev->GetFloat(2);

    viewheight = height;

    if (max_z >= 94.0) {
        max_z = 94.0;
    } else if (max_z >= 74.0 && max_z < 94.0) {
        max_z = 54.0;
    } else if (max_z >= 30.0 && max_z < 54.0) {
        max_z = 20.0;
    } else if (max_z <= 20.0) {
        max_z = 20.0;
    }

    maxs.z = max_z;

    client->ps.pm_flags &= ~(PMF_DUCKED | PMF_VIEW_PRONE | PMF_VIEW_DUCK_RUN | PMF_VIEW_JUMP_START);

    // FIXME...
    /*
    gi.MSG_SetClient(edict - g_entities);

    gi.MSG_StartCGM(CGM_MODHEIGHTFLOAT);
    gi.MSG_WriteLong(height);
    gi.MSG_WriteFloat(max_z);
    gi.MSG_EndCGM();
    */
}

void Player::SetMovePosFlags(Event *ev)
{
    str sParm;

    if (ev->NumArgs() <= 0) {
        Com_Printf("moveposflags command without any parameters\n");
        return;
    }

    sParm = ev->GetString(1);

    if (!sParm.icmp("crouching")) {
        m_iMovePosFlags = MPF_POSITION_CROUCHING;
    } else if (!sParm.icmp("prone")) {
        m_iMovePosFlags = MPF_POSITION_PRONE;
    } else if (!sParm.icmp("offground")) {
        m_iMovePosFlags = MPF_POSITION_OFFGROUND;
    } else {
        m_iMovePosFlags = MPF_POSITION_STANDING;
    }

    if (ev->NumArgs() > 1) {
        sParm = ev->GetString(2);

        if (!sParm.icmp("walking") || !sParm.icmp("walking\"") // there is a mistake in WALK_FORWARD
        ) {
            m_iMovePosFlags |= MPF_MOVEMENT_WALKING;
        } else if (!sParm.icmp("running")) {
            m_iMovePosFlags |= MPF_MOVEMENT_RUNNING;
        } else if (!sParm.icmp("falling")) {
            m_iMovePosFlags |= MPF_MOVEMENT_FALLING;
        }
    }
}

void Player::GetPositionForScript(Event *ev)
{
    if (m_iMovePosFlags & MPF_POSITION_CROUCHING) {
        ev->AddConstString(STRING_CROUCHING);
    } else if (m_iMovePosFlags & MPF_POSITION_PRONE) {
        ev->AddConstString(STRING_PRONE);
    } else if (m_iMovePosFlags & MPF_POSITION_OFFGROUND) {
        ev->AddConstString(STRING_OFFGROUND);
    } else {
        ev->AddConstString(STRING_STANDING);
    }
}

void Player::GetMovementForScript(Event *ev)
{
    if (m_iMovePosFlags & MPF_MOVEMENT_WALKING) {
        ev->AddConstString(STRING_WALKING);
    } else if (m_iMovePosFlags & MPF_MOVEMENT_RUNNING) {
        ev->AddConstString(STRING_RUNNING);
    } else if (m_iMovePosFlags & MPF_MOVEMENT_FALLING) {
        ev->AddConstString(STRING_FALLING);
    } else {
        ev->AddConstString(STRING_STANDING);
    }
}

void Player::ToggleZoom(int iZoom)
{
    if (iZoom && m_iInZoomMode == -1) {
        SetFov(selectedfov);
        m_iInZoomMode = 0;
    } else {
        SetFov(iZoom);
        m_iInZoomMode = -1;
    }
}

void Player::ZoomOff(void)
{
    SetFov(selectedfov);
    m_iInZoomMode = 0;
}

void Player::ZoomOffEvent(Event *ev)
{
    ZoomOff();
}

qboolean Player::IsZoomed(void)
{
    return m_iInZoomMode == -1;
}

void Player::SafeZoomed(Event *ev)
{
    if (ev->GetInteger(1)) {
        if (m_iInZoomMode > 0) {
            SetFov(m_iInZoomMode);
            m_iInZoomMode = -1;
        }
    } else {
        if (m_iInZoomMode == -1) {
            m_iInZoomMode = fov;
            SetFov(selectedfov);
        }
    }
}

void Player::AttachToLadder(Event *ev)
{
    Vector      vStart, vEnd, vOffset;
    trace_t     trace;
    FuncLadder *pLadder;

    if (deadflag) {
        return;
    }

    AngleVectors(m_vViewAng, vOffset, NULL, NULL);

    vStart = m_vViewPos - vOffset * 12.0f;
    vEnd   = m_vViewPos + vOffset * 128.0f;

    trace = G_Trace(vStart, vec_zero, vec_zero, vEnd, this, MASK_LADDER, qfalse, "Player::AttachToLadder");

    if (trace.fraction == 1.0f || !trace.ent || !trace.ent->entity || !trace.ent->entity->isSubclassOf(FuncLadder)) {
        return;
    }

    pLadder   = (FuncLadder *)trace.ent->entity;
    m_pLadder = pLadder;

    pLadder->PositionOnLadder(this);

    SetViewAngles(Vector(v_angle[0], angles[1], v_angle[2]));
}

void Player::UnattachFromLadder(Event *ev)
{
    m_pLadder = NULL;
}

void Player::TweakLadderPos(Event *ev)
{
    FuncLadder *pLadder = (FuncLadder *)m_pLadder.Pointer();

    if (pLadder) {
        pLadder->AdjustPositionOnLadder(this);
    }
}

void Player::EnsureOverLadder(Event *ev)
{
    FuncLadder *pLadder = (FuncLadder *)m_pLadder.Pointer();

    if (pLadder) {
        pLadder->EnsureOverLadder(this);
    }
}

void Player::EnsureForwardOffLadder(Event *ev)
{
    FuncLadder *pLadder = (FuncLadder *)m_pLadder.Pointer();

    if (pLadder) {
        pLadder->EnsureForwardOffLadder(this);
    }
}

void Player::EventForceLandmineMeasure(Event *ev)
{
    MeasureLandmineDistances();
}

str Player::GetCurrentDMWeaponType() const
{
    return m_sDmPrimary;
}

void Player::Score(Event *ev)
{
    if (g_gametype->integer == GT_SINGLE_PLAYER) {
        // Of course useless in single-player mode
        return;
    }

    dmManager.Score(this);
}

// Was between 2.0 and 2.15
/*
nationality_t GetAlliedType(const char* name)
{
    if (!Q_stricmpn(name, "american", 8)) {
        return NA_AMERICAN;
    } else if (!Q_stricmpn(name, "allied_russian", 14)) {
        return NA_RUSSIAN;
    } else if (!Q_stricmpn(name, "allied_british", 14)) {
        return NA_BRITISH;
    } else if (!Q_stricmpn(name, "allied", 6)) {
        return NA_AMERICAN;
    } else {
        return NA_NONE;
    }
}
*/

// Commented out in OPM. See the other comment below.
/*
nationality_t GetPlayerTeamType(const char *name)
{
    if (!Q_stricmpn(name, "american", 8)) {
        return NA_AMERICAN;
    } else if (!Q_stricmpn(name, "allied_russian", 14)) {
        return NA_RUSSIAN;
    } else if (!Q_stricmpn(name, "allied_british", 14)) {
        return NA_BRITISH;
    } else if (!Q_stricmpn(name, "allied_sas", 10)) {
        return NA_BRITISH;
    } else if (!Q_stricmpn(name, "allied", 6)) {
        return NA_AMERICAN;
    } else if (!Q_stricmpn(name, "german", 6)) {
        return NA_GERMAN;
    } else if (!Q_stricmpn(name, "it", 2)) {
        return NA_ITALIAN;
    } else if (!Q_stricmpn(name, "sc", 2)) {
        return NA_ITALIAN;
    } else {
        return NA_NONE;
    }
}
*/

// Fixed in OPM.
//  This fixes the issue where the player can equip weapons
//  from the other team
nationality_t GetPlayerAxisTeamType(const char *name)
{
    if (g_target_game < target_game_e::TG_MOHTA) {
        // Only american and german are supported on older versions of the game
        return NA_GERMAN;
    }

    if (!Q_stricmpn(name, "german", 6)) {
        return NA_GERMAN;
    }

    if (g_target_game < target_game_e::TG_MOHTT) {
        // Italian skins are supported only in mohaab
        return NA_GERMAN;
    }

    if (!Q_stricmpn(name, "it", 2)) {
        return NA_ITALIAN;
    } else if (!Q_stricmpn(name, "sc", 2)) {
        return NA_ITALIAN;
    }

    // fallback to german
    return NA_GERMAN;
}

nationality_t GetPlayerAlliedTeamType(const char *name)
{
    if (g_target_game < target_game_e::TG_MOHTA) {
        // Only american and german are supported on older versions of the game
        return NA_AMERICAN;
    }

    if (!Q_stricmpn(name, "american", 8)) {
        return NA_AMERICAN;
    } else if (!Q_stricmpn(name, "allied_russian", 14)) {
        return NA_RUSSIAN;
    } else if (!Q_stricmpn(name, "allied_british", 14)) {
        return NA_BRITISH;
    } else if (!Q_stricmpn(name, "allied_sas", 10)) {
        return NA_BRITISH;
    } else if (!Q_stricmpn(name, "allied", 6)) {
        return NA_AMERICAN;
    }

    // fallback to american
    return NA_AMERICAN;
}

void Player::InitDeathmatch(void)
{
    fAttackerDispTime    = 0.0f;
    pAttackerDistPointer = nullptr;
    m_iInfoClient        = -1;
    m_fWeapSelectTime    = level.time - 9.0f;

    if (!g_realismmode->integer) {
        m_fDamageMultipliers[HITLOC_HEAD]        = 2.0f;
        m_fDamageMultipliers[HITLOC_HELMET]      = 2.0f;
        m_fDamageMultipliers[HITLOC_NECK]        = 2.0f;
        m_fDamageMultipliers[HITLOC_TORSO_UPPER] = 1.0f;
        m_fDamageMultipliers[HITLOC_TORSO_MID]   = 0.95f;
        m_fDamageMultipliers[HITLOC_TORSO_LOWER] = 0.90f;
        m_fDamageMultipliers[HITLOC_PELVIS]      = 0.85f;
        m_fDamageMultipliers[HITLOC_R_ARM_UPPER] = 0.80f;
        m_fDamageMultipliers[HITLOC_L_ARM_UPPER] = 0.80f;
        m_fDamageMultipliers[HITLOC_R_LEG_UPPER] = 0.80f;
        m_fDamageMultipliers[HITLOC_L_LEG_UPPER] = 0.80f;
        m_fDamageMultipliers[HITLOC_R_ARM_LOWER] = 0.60f;
        m_fDamageMultipliers[HITLOC_L_ARM_LOWER] = 0.60f;
        m_fDamageMultipliers[HITLOC_R_LEG_LOWER] = 0.60f;
        m_fDamageMultipliers[HITLOC_L_LEG_LOWER] = 0.60f;
        m_fDamageMultipliers[HITLOC_R_HAND]      = 0.50f;
        m_fDamageMultipliers[HITLOC_L_HAND]      = 0.50f;
        m_fDamageMultipliers[HITLOC_R_FOOT]      = 0.50f;
        m_fDamageMultipliers[HITLOC_L_FOOT]      = 0.50f;
    }

    if (current_team) {
        if (AllowTeamRespawn()) {
            EndSpectator();

            if (dmManager.GetMatchStartTime() > 0.0f && !dmManager.AllowRespawn() && g_allowjointime->value > 0.0f
                && (level.time - dmManager.GetMatchStartTime()) > g_allowjointime->value) {
                m_bTempSpectator = true;
            }

            switch (g_gametype->integer) {
            case GT_TEAM_ROUNDS:
            case GT_OBJECTIVE:
            case GT_TOW:
            case GT_LIBERATION:
                if (!m_bTempSpectator) {
                    BeginFight();
                } else {
                    Spectator();
                }
                break;
            default:
                BeginFight();
                break;
            }
        }
    } else {
        if (client->pers.teamnum) {
            SetTeam(client->pers.teamnum);
        } else {
            SetTeam(TEAM_SPECTATOR);
        }
    }

    edict->s.eFlags &= ~(TEAM_ALLIES | TEAM_AXIS);

    if (GetTeam() == TEAM_ALLIES) {
        edict->s.eFlags |= TEAM_ALLIES;
    } else if (GetTeam() == TEAM_AXIS) {
        edict->s.eFlags |= TEAM_AXIS;
    }

    G_SetClientConfigString(edict);

    if (g_gametype->integer >= GT_TEAM_ROUNDS) {
        if (client->pers.round_kills) {
            num_deaths               = client->pers.round_kills;
            client->pers.round_kills = 0;
        }
    }

    ChooseSpawnPoint();
    EquipWeapons();

    if (current_team) {
        current_team->m_bHasSpawnedPlayers = qtrue;
    }
}

bool Player::QueryLandminesAllowed() const
{
    const char *mapname;

    if (g_target_game < target_game_e::TG_MOHTT) {
        return false;
    }

    if (dmflags->integer & DF_WEAPON_NO_LANDMINE) {
        return false;
    }

    if (dmflags->integer & DF_WEAPON_LANDMINE_ALWAYS) {
        return true;
    }

    mapname = level.mapname.c_str();

    if (!Q_stricmpn(mapname, "obj/obj_", 8u)) {
        return false;
    }
    if (!Q_stricmpn(mapname, "dm/mohdm", 8u)) {
        return false;
    }
    if (!Q_stricmp(mapname, "DM/MP_Bahnhof_DM")) {
        return false;
    }
    if (!Q_stricmp(mapname, "obj/MP_Ardennes_TOW")) {
        return false;
    }
    if (!Q_stricmp(mapname, "DM/MP_Bazaar_DM")) {
        return false;
    }
    if (!Q_stricmp(mapname, "obj/MP_Berlin_TOW")) {
        return false;
    }
    if (!Q_stricmp(mapname, "DM/MP_Brest_DM")) {
        return false;
    }
    if (!Q_stricmp(mapname, "obj/MP_Druckkammern_TOW")) {
        return false;
    }
    if (!Q_stricmp(mapname, "DM/MP_Gewitter_DM")) {
        return false;
    }
    if (!Q_stricmp(mapname, "obj/MP_Flughafen_TOW")) {
        return false;
    }
    if (!Q_stricmp(mapname, "DM/MP_Holland_DM")) {
        return false;
    }
    if (!Q_stricmp(mapname, "DM/MP_Malta_DM")) {
        return false;
    }
    if (!Q_stricmp(mapname, "DM/MP_Stadt_DM")) {
        return false;
    }
    if (!Q_stricmp(mapname, "DM/MP_Unterseite_DM")) {
        return false;
    }
    if (!Q_stricmp(mapname, "DM/MP_Verschneit_DM")) {
        return false;
    }
    if (!Q_stricmp(mapname, "lib/mp_ship_lib")) {
        return false;
    }

    return true;
}

void Player::EnsurePlayerHasAllowedWeapons()
{
    int i;

    if (!client) {
        return;
    }

    if (!client->pers.dm_primary[0]) {
        return;
    }

    for (i = 0; i < 7; i++) {
        if (!Q_stricmp(client->pers.dm_primary, "sniper")) {
            if (!(dmflags->integer & DF_WEAPON_NO_SNIPER)) {
                return;
            }

            Q_strncpyz(client->pers.dm_primary, "rifle", sizeof(client->pers.dm_primary));
        } else if (!Q_stricmp(client->pers.dm_primary, "rifle")) {
            if (!(dmflags->integer & DF_WEAPON_NO_RIFLE)) {
                return;
            }

            Q_strncpyz(client->pers.dm_primary, "smg", sizeof(client->pers.dm_primary));
        } else if (!Q_stricmp(client->pers.dm_primary, "smg")) {
            if (!(dmflags->integer & DF_WEAPON_NO_SMG)) {
                return;
            }

            Q_strncpyz(client->pers.dm_primary, "mg", sizeof(client->pers.dm_primary));
        } else if (!Q_stricmp(client->pers.dm_primary, "mg")) {
            if (!(dmflags->integer & DF_WEAPON_NO_MG)) {
                return;
            }

            Q_strncpyz(client->pers.dm_primary, "shotgun", sizeof(client->pers.dm_primary));
        } else if (!Q_stricmp(client->pers.dm_primary, "shotgun")) {
            if (!(dmflags->integer & DF_WEAPON_NO_SHOTGUN)) {
                return;
            }

            Q_strncpyz(client->pers.dm_primary, "heavy", sizeof(client->pers.dm_primary));
        } else if (!Q_stricmp(client->pers.dm_primary, "heavy")) {
            if (!(dmflags->integer & DF_WEAPON_NO_ROCKET)) {
                return;
            }

            Q_strncpyz(client->pers.dm_primary, "landmine", sizeof(client->pers.dm_primary));
        } else if (!Q_stricmp(client->pers.dm_primary, "landmine")) {
            if (QueryLandminesAllowed()) {
                return;
            }

            Q_strncpyz(client->pers.dm_primary, "sniper", sizeof(client->pers.dm_primary));
        }
    }

    gi.cvar_set("dmflags", va("%i", dmflags->integer & ~DF_WEAPON_NO_RIFLE));
    Com_Printf("No valid weapons -- re-allowing the rifle\n");
    Q_strncpyz(client->pers.dm_primary, "rifle", sizeof(client->pers.dm_primary));
}

void Player::EquipWeapons()
{
    Event *event;

    nationality_t nationality;

    if (IsSpectator()) {
        FreeInventory();
        return;
    }

    // Fixed in OPM
    //  Old behavior was calling GetPlayerTeamType() regardless of the team
    if (GetTeam() == TEAM_AXIS) {
        nationality = GetPlayerAxisTeamType(client->pers.dm_playergermanmodel);
    } else {
        nationality = GetPlayerAlliedTeamType(client->pers.dm_playermodel);
    }

    event = new Event(EV_Sentient_UseItem);

    if (!m_sDmPrimary.length()) {
        // Set the primary weapon
        m_sDmPrimary = client->pers.dm_primary;
    }

    EnsurePlayerHasAllowedWeapons();

    if (!Q_stricmp(client->pers.dm_primary, "sniper") && !(dmflags->integer & DF_WEAPON_NO_SNIPER)) {
        switch (nationality) {
        case NA_BRITISH:
            if (g_target_game < target_game_e::TG_MOHTT) {
                giveItem("weapons/springfield.tik");
                event->AddString("Springfield '03 Sniper");
            } else {
                giveItem("weapons/Uk_W_L42A1.tik");
                event->AddString("Enfield L42A1");
            }
            break;
        case NA_RUSSIAN:
            if (g_target_game < target_game_e::TG_MOHTA || dmflags->integer & DF_OLD_SNIPER) {
                // Old snipers are forced older versions of the game
                giveItem("weapons/springfield.tik");
                event->AddString("Springfield '03 Sniper");
            } else {
                giveItem("weapons/svt_rifle.tik");
                event->AddString("SVT 40");
            }
            break;
        case NA_GERMAN:
            if (g_target_game < target_game_e::TG_MOHTA
                || dmflags->integer & DF_OLD_SNIPER
                // Added in OPM
                //  This was also a feature of Daven's fixes
                //  Use KAR98 for panzer skins
                || !Q_stricmpn(client->pers.dm_playergermanmodel, "german_panzer", 13)) {
                // Old snipers are forced older versions of the game
                giveItem("weapons/kar98sniper.tik");
                event->AddString("KAR98 - Sniper");
            } else {
                giveItem("weapons/g43.tik");
                event->AddString("G 43");
            }
            break;
        case NA_ITALIAN:
            giveItem("weapons/kar98sniper.tik");
            event->AddString("KAR98 - Sniper");
            break;
        case NA_AMERICAN:
        default:
            giveItem("weapons/springfield.tik");
            event->AddString("Springfield '03 Sniper");
            break;
        }
    } else if (!Q_stricmp(client->pers.dm_primary, "smg") && !(dmflags->integer & DF_WEAPON_NO_SMG)) {
        switch (nationality) {
        case NA_BRITISH:
            giveItem("weapons/sten.tik");
            event->AddString("Sten Mark II");
            break;
        case NA_RUSSIAN:
            giveItem("weapons/ppsh_smg.tik");
            event->AddString("PPSH SMG");
            break;
        case NA_GERMAN:
            giveItem("weapons/mp40.tik");
            event->AddString("MP40");
            break;
        case NA_ITALIAN:
            giveItem("weapons/it_w_moschetto.tik");
            event->AddString("Moschetto");
            break;
        case NA_AMERICAN:
        default:
            giveItem("weapons/thompsonsmg.tik");
            event->AddString("Thompson");
            break;
        }
    } else if (!Q_stricmp(client->pers.dm_primary, "mg") && !(dmflags->integer & DF_WEAPON_NO_MG)) {
        switch (nationality) {
        case NA_BRITISH:
            if (g_target_game < target_game_e::TG_MOHTT) {
                giveItem("weapons/bar.tik");
                event->AddString("BAR");
                break;
            } else {
                giveItem("weapons/Uk_W_Vickers.tik");
                event->AddString("Vickers-Berthier");
            }
            break;
        case NA_GERMAN:
            giveItem("weapons/mp44.tik");
            event->AddString("StG 44");
            break;
        case NA_ITALIAN:
            giveItem("weapons/It_W_Breda.tik");
            event->AddString("Breda");
            break;
        case NA_AMERICAN:
        default:
            giveItem("weapons/bar.tik");
            event->AddString("BAR");
            break;
        }
    } else if (!Q_stricmp(client->pers.dm_primary, "heavy") && !(dmflags->integer & DF_WEAPON_NO_ROCKET)) {
        switch (nationality) {
        case NA_GERMAN:
        case NA_ITALIAN:
            giveItem("weapons/panzerschreck.tik");
            event->AddString("Panzerschreck");
            break;
        case NA_BRITISH:
            if (g_target_game < target_game_e::TG_MOHTT) {
                giveItem("weapons/bazooka.tik");
                event->AddString("Bazooka");
            } else {
                giveItem("weapons/Uk_W_Piat.tik");
                event->AddString("PIAT");
            }
            break;
        case NA_AMERICAN:
        default:
            giveItem("weapons/bazooka.tik");
            event->AddString("Bazooka");
            break;
        }
    } else if (!Q_stricmp(client->pers.dm_primary, "shotgun") && !(dmflags->integer & DF_WEAPON_NO_SHOTGUN)) {
        switch (nationality) {
        case NA_BRITISH:
            if (g_target_game < target_game_e::TG_MOHTT) {
                giveItem("weapons/shotgun.tik");
                event->AddString("Shotgun");
            } else {
                giveItem("weapons/DeLisle.tik");
                event->AddString("DeLisle");
            }
            break;
        case NA_GERMAN:
            if (g_target_game < target_game_e::TG_MOHTA || dmflags->integer & DF_DISALLOW_KAR98_MORTAR) {
                // Fallback to shotgun
                // The shotgun is forced on older versions of the game
                giveItem("weapons/shotgun.tik");
                event->AddString("Shotgun");
            } else {
                giveItem("weapons/kar98_mortar.tik");
                event->AddString("Gewehrgranate");
            }
            break;
        case NA_AMERICAN:
        default:
            giveItem("weapons/shotgun.tik");
            event->AddString("Shotgun");
            break;
        }
    } else if (!Q_stricmp(client->pers.dm_primary, "landmine") && QueryLandminesAllowed()) {
        //gi.Cvar_Get("g_rifles_for_sweepers", "0", 0);

        switch (nationality) {
        case NA_BRITISH:
            giveItem("weapons/US_W_Minedetector.tik");
            event->AddString("Minedetector");

            if (g_rifles_for_sweepers->integer) {
                // Give a lite version of the rifle
                giveItem("weapons/enfield_lite.tik");
            } else {
                // Just give some ammo for the pistol
                GiveAmmo("pistol", 12);
            }
            break;
        case NA_RUSSIAN:
            giveItem("weapons/US_W_Minedetector.tik");
            event->AddString("Minedetector");

            if (g_rifles_for_sweepers->integer) {
                // Give a lite version of the rifle
                giveItem("weapons/Mosin_Nagant_Rifle_lite.tik");
            } else {
                // Just give some ammo for the pistol
                GiveAmmo("pistol", 14);
            }
            break;
        case NA_GERMAN:
            giveItem("weapons/Gr_W_Minedetector.tik");
            event->AddString("Minensuchgerat");

            if (g_rifles_for_sweepers->integer) {
                // Give a lite version of the rifle
                giveItem("weapons/kar98_lite.tik");
            } else {
                // Just give some ammo for the pistol
                GiveAmmo("pistol", 16);
            }
            break;
        case NA_AMERICAN:
        default:
            giveItem("weapons/US_W_Minedetector.tik");
            event->AddString("Minedetector");

            if (g_rifles_for_sweepers->integer) {
                // Give a lite version of the rifle
                giveItem("weapons/m1_garand_lite.tik");
            } else {
                // Just give some ammo for the pistol
                GiveAmmo("pistol", 14);
            }
            break;
        }
    } else if (!(dmflags->integer & DF_WEAPON_NO_RIFLE)) {
        switch (nationality) {
        case NA_BRITISH:
            giveItem("weapons/enfield.tik");
            event->AddString("Lee-Enfield");
            break;
        case NA_RUSSIAN:
            giveItem("weapons/Mosin_Nagant_Rifle.tik");
            event->AddString("Mosin Nagant Rifle");
            break;
        case NA_GERMAN:
            giveItem("weapons/kar98.tik");
            event->AddString("Mauser KAR 98K");
            break;
        case NA_ITALIAN:
            giveItem("weapons/it_w_carcano.tik");
            event->AddString("Carcano");
            break;
        case NA_AMERICAN:
        default:
            giveItem("weapons/m1_garand.tik");
            event->AddString("M1 Garand");
            break;
        }
    }

    // Make the player switch to the weapon some time after spawning
    PostEvent(event, 0.3f);

    //
    // Pistols and grenades
    //
    switch (nationality) {
    case NA_BRITISH:
        giveItem("weapons/mills_grenade.tik");
        if (g_target_game >= target_game_e::TG_MOHTA) {
            giveItem("weapons/M18_smoke_grenade.tik");
        }
        giveItem("weapons/Webley_Revolver.tik");
        break;
    case NA_RUSSIAN:
        giveItem("weapons/Russian_F1_grenade.tik");
        if (g_target_game >= target_game_e::TG_MOHTA) {
            giveItem("weapons/RDG-1_Smoke_grenade.tik");
        }
        giveItem("weapons/Nagant_revolver.tik");
        break;
    case NA_GERMAN:
        giveItem("weapons/steilhandgranate.tik");
        if (g_target_game >= target_game_e::TG_MOHTA) {
            giveItem("weapons/nebelhandgranate.tik");
        }
        giveItem("weapons/p38.tik");
        break;
    case NA_ITALIAN:
        giveItem("weapons/it_w_bomba.tik");
        if (g_target_game >= target_game_e::TG_MOHTA) {
            giveItem("weapons/it_w_bombabreda.tik");
        }
        giveItem("weapons/it_w_beretta.tik");
        break;
    default:
        giveItem("weapons/m2frag_grenade.tik");
        if (g_target_game >= target_game_e::TG_MOHTA) {
            giveItem("weapons/M18_smoke_grenade.tik");
        }
        giveItem("weapons/colt45.tik");
        break;
    }
}

void Player::EquipWeapons_ver8()
{
    // spectators should not have weapons
    if (IsSpectator()) {
        FreeInventory();
    } else {
        Event *ev = new Event("use");

        if (!Q_stricmp(client->pers.dm_primary, "rifle")) {
            if (dm_team == TEAM_ALLIES) {
                giveItem("models/weapons/m1_garand.tik");
                ev->AddString("models/weapons/m1_garand.tik");
            } else {
                giveItem("models/weapons/kar98.tik");
                ev->AddString("models/weapons/kar98.tik");
            }

            GiveAmmo("rifle", 100);
        } else if (!Q_stricmp(client->pers.dm_primary, "sniper")) {
            if (dm_team == TEAM_ALLIES) {
                giveItem("models/weapons/springfield.tik");
                ev->AddString("models/weapons/springfield.tik");
            } else {
                giveItem("models/weapons/kar98sniper.tik");
                ev->AddString("models/weapons/kar98sniper.tik");
            }
        } else if (!Q_stricmp(client->pers.dm_primary, "smg")) {
            if (dm_team == TEAM_ALLIES) {
                giveItem("models/weapons/thompsonsmg.tik");
                ev->AddString("models/weapons/thompsonsmg.tik");
            } else {
                giveItem("models/weapons/mp40.tik");
                ev->AddString("models/weapons/mp40.tik");
            }
        } else if (!Q_stricmp(client->pers.dm_primary, "mg")) {
            if (dm_team == TEAM_ALLIES) {
                giveItem("models/weapons/bar.tik");
                ev->AddString("models/weapons/bar.tik");
            } else {
                giveItem("models/weapons/mp44.tik");
                ev->AddString("models/weapons/mp44.tik");
            }
        } else if (!Q_stricmp(client->pers.dm_primary, "heavy")) {
            if (dm_team == TEAM_ALLIES) {
                giveItem("models/weapons/bazooka.tik");
                ev->AddString("models/weapons/bazooka.tik");
            } else {
                giveItem("models/weapons/panzerschreck.tik");
                ev->AddString("models/weapons/panzerschreck.tik");
            }
        } else if (!Q_stricmp(client->pers.dm_primary, "shotgun")) {
            giveItem("models/weapons/shotgun.tik");
            ev->AddString("models/weapons/shotgun.tik");
        }

        PostEvent(ev, 0.3f);

        if (dm_team == TEAM_ALLIES) {
            giveItem("models/weapons/colt45.tik");
            giveItem("models/weapons/m2frag_grenade.tik");
        } else {
            giveItem("models/weapons/p38.tik");
            giveItem("models/weapons/steilhandgranate.tik");
        }

        giveItem("models/items/binoculars.tik");
    }
}

void Player::Spectator(void)
{
    if (!IsSpectator()) {
        respawn_time = level.time + 1.0f;
    }

    RemoveFromVehiclesAndTurrets();

    m_bSpectator        = !m_bTempSpectator;
    m_iPlayerSpectating = 0;
    takedamage          = DAMAGE_NO;
    deadflag            = DEAD_NO;
    health              = max_health;

    client->ps.feetfalling = 0;
    movecontrol            = MOVECONTROL_USER;
    client->ps.pm_flags |= PMF_SPECTATING;

    EvaluateState(statemap_Torso->FindState("STAND"), statemap_Legs->FindState("STAND"));

    setSolidType(SOLID_NOT);
    setMoveType(MOVETYPE_NOCLIP);

    FreeInventory();

    hideModel();

    SetPlayerSpectateRandom();
}

bool Player::IsValidSpectatePlayer(Player *pPlayer)
{
    if (g_gametype->integer <= GT_FFA) {
        return true;
    }

    if (GetTeam() <= TEAM_FREEFORALL) {
        return true;
    }

    if (g_forceteamspectate->integer) {
        if (!GetDM_Team()->NumLivePlayers()) {
            return true;
        }

        if (pPlayer->GetTeam() == GetTeam()) {
            return true;
        }

        return false;
    }

    return true;
}

void Player::SetPlayerSpectate(bool bNext)
{
    int        i;
    int        dir;
    int        num;
    gentity_t *ent;
    Player    *pPlayer;

    if (bNext) {
        dir = 1;
        num = m_iPlayerSpectating;
    } else {
        dir = -1;
        if (m_iPlayerSpectating) {
            num = m_iPlayerSpectating - 2;
        } else {
            num = game.maxclients - 1;
        }
    }

    for (i = num; i < game.maxclients && i >= 0; i += dir) {
        ent = &g_entities[i];
        if (!ent->inuse || !ent->entity) {
            continue;
        }

        pPlayer = (Player *)ent->entity;

        if (!pPlayer->IsDead() && !pPlayer->IsSpectator() && IsValidSpectatePlayer(pPlayer)) {
            m_iPlayerSpectating = i + 1;
            client->ps.camera_flags &= ~CF_CAMERA_CUT_BIT;
            client->ps.camera_flags |= (client->ps.camera_flags & CF_CAMERA_CUT_BIT) ^ CF_CAMERA_CUT_BIT;
            return;
        }
    }

    if (m_iPlayerSpectating) {
        m_iPlayerSpectating = 0;
        SetPlayerSpectate(bNext);
    }
}

void Player::SetPlayerSpectateRandom(void)
{
    Player *pPlayer;
    int     i;
    int     numvalid;
    int     iRandom;

    numvalid = 0;

    for (i = 0; i < game.maxclients; i++) {
        gentity_t *ent = &g_entities[i];
        if (!ent->inuse || !ent->entity) {
            continue;
        }

        pPlayer = static_cast<Player *>(ent->entity);
        if (!pPlayer->IsDead() && !pPlayer->IsSpectator() && IsValidSpectatePlayer(pPlayer)) {
            numvalid++;
        }
    }

    if (!numvalid) {
        // There is no valid player to spectate

        // Added in OPM.
        //  Clear the player spectating value
        m_iPlayerSpectating = 0;
        return;
    }

    iRandom = (int)(random() * numvalid);

    for (i = 0; i < game.maxclients; i++) {
        gentity_t *ent = &g_entities[i];
        if (!ent->inuse || !ent->entity) {
            continue;
        }

        pPlayer = static_cast<Player *>(ent->entity);
        if (!pPlayer->IsDead() && !pPlayer->IsSpectator() && IsValidSpectatePlayer(pPlayer)) {
            if (!iRandom) {
                m_iPlayerSpectating = i + 1;

                client->ps.camera_flags &= ~CF_CAMERA_CUT_BIT;
                client->ps.camera_flags |= (client->ps.camera_flags & CF_CAMERA_CUT_BIT) ^ CF_CAMERA_CUT_BIT;
                break;
            }

            iRandom--;
        }
    }
}

void Player::GetSpectateFollowOrientation(Player *pPlayer, Vector& vPos, Vector& vAng)
{
    Vector  forward, right, up;
    Vector  vCamOfs;
    Vector  start;
    trace_t trace;

    if (!g_spectatefollow_firstperson->integer) {
        // spectating a player
        vAng = pPlayer->GetVAngles();

        AngleVectors(vAng, forward, right, up);

        vCamOfs = pPlayer->origin;
        vCamOfs[2] += pPlayer->viewheight;

        vCamOfs += forward * g_spectatefollow_forward->value;
        vCamOfs += right * g_spectatefollow_right->value;
        vCamOfs += up * g_spectatefollow_up->value;

        if (pPlayer->client->ps.fLeanAngle != 0.0f) {
            vCamOfs += pPlayer->client->ps.fLeanAngle * 0.65f * right;
        }

        start = pPlayer->origin;
        start[2] += pPlayer->maxs[2] - 2.0;

        Vector vMins = Vector(-2, -2, 2);
        Vector vMaxs = Vector(2, 2, 2);

        trace =
            G_Trace(start, vMins, vMaxs, vCamOfs, pPlayer, MASK_SHOT, false, "Player::GetSpectateFollowOrientation");

        vAng[0] += g_spectatefollow_pitch->value * trace.fraction;
        vPos = trace.endpos;
    } else {
        vAng = pPlayer->angles;
        vPos = pPlayer->origin;
    }
}

void Player::Spectator(Event *ev)
{
    if (g_gametype->integer == GT_SINGLE_PLAYER) {
        // Added in OPM
        //  No team in single player
        return;
    };

    client->pers.dm_primary[0] = 0;
    SetTeam(TEAM_SPECTATOR);
}

void Player::Leave_DM_Team(Event *ev)
{
    // Fixed in OPM
    //  FIXME: should it be permanently disabled ?
#if 0
    if (current_team)
    {
        dmManager.LeaveTeam(this);
    }
    else
    {
        gi.centerprintf(edict, gi.LV_ConvertString("You are not on a team"));
    }
#endif
}

void Player::Join_DM_Team(Event *ev)
{
    teamtype_t  team;
    str         teamname;
    const char *join_message;
    Entity     *ent;

    if (ev->isSubclassOf(ConsoleEvent) && disable_team_change) {
        // Added in OPM
        return;
    }

    if (g_gametype->integer == GT_SINGLE_PLAYER) {
        // Added in OPM
        //  No team in single player
        return;
    }

    teamname = ev->GetString(1);

    if (!teamname.icmp("allies")) {
        team = TEAM_ALLIES;
    } else if (!teamname.icmp("axis") || !teamname.icmp("german") || !teamname.icmp("nazi")) {
        team = TEAM_AXIS;
    } else {
        team = TEAM_AXIS;
    }

    if (current_team && current_team->m_teamnumber == team) {
        //
        // don't switch if on same team
        //
        return;
    }

    if (deadflag && deadflag != DEAD_DEAD) {
        // ignore team switching if the player is dying and not dead
        return;
    }

    if (ev->isSubclassOf(ConsoleEvent) && !CheckCanSwitchTeam(team)) {
        return;
    }

    m_fTeamSelectTime = level.time;
    SetTeam(team);
    // Make sure to remove player from turret
    RemoveFromVehiclesAndTurrets();

    //
    // Since 2.0: Remove projectiles that the player own
    //
    for (ent = G_NextEntity(NULL); ent; ent = G_NextEntity(ent)) {
        // Fixed in OPM
        //  2.0 accidentally use the player's ownerNum which is always ENTITYNUM_NONE.
        //  It causes projectiles with no owner to be deleted.
        if (ent->IsSubclassOfProjectile() && ent->edict->r.ownerNum == entnum) {
            ent->PostEvent(EV_Remove, 0);
        }
    }

    if (client->pers.dm_primary[0]) {
        if (IsSpectator()) {
            if (m_fSpawnTimeLeft) {
                m_bWaitingForRespawn = true;
            } else if (AllowTeamRespawn()) {
                EndSpectator();

                if (deadflag) {
                    deadflag = DEAD_DEAD;
                }

                PostEvent(EV_Player_Respawn, 0);
                gi.centerprintf(edict, " ");
            }
        } else if (g_gametype->integer >= GT_TEAM) {
            client->pers.dm_primary[0] = 0;
            UserSelectWeapon(false);
            Spectator();
        } else {
            PostEvent(EV_Player_Respawn, 0);
        }
    } else if (IsSpectator()) {
        UserSelectWeapon(true);
    }

    if (g_gametype->integer >= GT_TEAM) {
        //
        // in team game modes, display a message to indicate
        // a player joined a team
        //
        if (GetTeam() == TEAM_ALLIES) {
            join_message = gi.LV_ConvertString("has joined the Allies");
        } else if (GetTeam() == TEAM_AXIS) {
            join_message = gi.LV_ConvertString("has joined the Axis");
        } else {
            return;
        }

        G_PrintfClient(edict, "%s\n", join_message);

        G_PrintToAllClients(va("%s %s\n", client->pers.netname, join_message), 2);
    }
}

void Player::Auto_Join_DM_Team(Event *ev)
{
    if (g_gametype->integer == GT_SINGLE_PLAYER) {
        // Added in OPM
        //  No team in single player
        return;
    };

    Event event(EV_Player_JoinDMTeam, 1);

    if (dmManager.GetAutoJoinTeam() == TEAM_AXIS) {
        event.AddString("axis");
    } else {
        event.AddString("allies");
    }

    ProcessEvent(event);
}

teamtype_t Player::GetTeam() const
{
    return dm_team;
}

void Player::SetTeam(teamtype_t team)
{
    dmManager.JoinTeam(this, team);

    if (dm_team == TEAM_SPECTATOR) {
        Spectator();
    }
}

void Player::SetDM_Team(DM_Team *team)
{
    current_team = team;

    // clear the player's team
    edict->s.eFlags &= ~EF_ANY_TEAM;

    if (team) {
        dm_team = static_cast<teamtype_t>(team->getNumber());
        if (dm_team == TEAM_ALLIES) {
            edict->s.eFlags |= EF_ALLIES;
        } else if (dm_team == TEAM_AXIS) {
            edict->s.eFlags |= EF_AXIS;
        }
    } else {
        dm_team = TEAM_NONE;
    }

    client->pers.teamnum = dm_team;
    G_SetClientConfigString(edict);

    if (m_fTeamSelectTime != level.time && (edict->s.eFlags & (EF_ANY_TEAM))) {
        InitModel();
    }
}

DM_Team *Player::GetDM_Team()
{
    return current_team;
}

bool Player::IsSpectator(void)
{
    return (m_bSpectator || m_bTempSpectator);
}

void Player::BeginFight(void)
{
    m_bAllowFighting = true;
}

void Player::EndFight(void)
{
    m_bAllowFighting = false;
}

void Player::WarpToPoint(Entity *spawnpoint)
{
    if (spawnpoint) {
        setOrigin(spawnpoint->origin + Vector(0, 0, 1));
        setAngles(spawnpoint->angles);
        SetViewAngles(angles);
        client->ps.camera_flags &= ~CF_CAMERA_CUT_BIT;
        client->ps.camera_flags |= (client->ps.camera_flags & CF_CAMERA_CUT_BIT) ^ CF_CAMERA_CUT_BIT;
    }
}

void Player::UpdateStatus(const char *s)
{
    gi.SendServerCommand(edict - g_entities, "status \"%s\"", s);
}

void Player::HUDPrint(const char *s)
{
    gi.SendServerCommand(edict - g_entities, "hudprint \"%s\"\n", s);
}

void Player::GibEvent(Event *ev)
{
    qboolean hidemodel;

    hidemodel = !ev->GetInteger(1);

    if (com_blood->integer) {
        if (hidemodel) {
            gibbed     = true;
            takedamage = DAMAGE_NO;
            setSolidType(SOLID_NOT);
            hideModel();
        }

        CreateGibs(this, health, 0.75f, 3);
    }
}

void Player::ArmorDamage(Event *ev)
{
    int mod = ev->GetInteger(9);

    // HZM coop [user 2026-08-23, bug-2083] DAMAGE ATTRIBUTION, at the only place it is knowable.
    //
    // "Do you have a way to tell what the hell is hurting me right now / Myself and the paradroopers
    // are taking some kind of invisible damage." There was no way to answer that, and the FIRST
    // attempt at one - a script `waittill damage` watcher in probe.scr - could never have worked:
    // Entity::Damage posts EV_Damage, and the Unregister(STRING_DAMAGE) that would wake a script
    // lives in Entity::DamageEvent, which PLAYERS OVERRIDE with this very function. Nothing on the
    // Player path unregisters it. Worse, `self.fact` - which that watcher read for the attacker - is
    // written only by aihandler.scr and global/pain.scr and never for a player, so every field would
    // have come back WORLD / -1, the exact signature the probe treats as "invisible area hazard".
    // It would have manufactured evidence for the hypothesis it existed to test.
    //
    // So it goes here instead, where the attacker, the inflictor and the means-of-death are already
    // in hand as event arguments. Damage that merely HURTS leaves no other trace anywhere: XPKILL
    // only fires on a kill, so a 15-per-tick hazard against 750hp is completely silent.
    //
    // mod is the means-of-death index (bg_public.h): 9 = MOD_EXPLOSION, which is what a script
    // `radiusdamage` call lands as because it has no owner - that pair, attacker=WORLD with mod=9,
    // is the fingerprint of an unattributable area hazard and is what makes this print worth having.
    {
        static cvar_t *pDmgPrb = NULL;

        if (!pDmgPrb) {
            pDmgPrb = gi.Cvar_Get("coop_dmgProbe", "0", 0);
        }
        if (pDmgPrb->integer) {
            Entity     *pAtk = ev->GetEntity(1);
            Entity     *pInf = ev->GetEntity(3);
            const char *cAtk = pAtk ? pAtk->getClassname() : "WORLD";
            const char *cInf = pInf ? pInf->getClassname() : "-";
            const char *tAtk = (pAtk && pAtk->targetname.length()) ? pAtk->targetname.c_str() : "-";
            float       fDist = pAtk ? (origin - pAtk->origin).length() : -1.0f;

            gi.Printf(
                "^~^~^ DMG victim=%d hp=%d dmg=%.0f mod=%d atk=%s atkTn=%s atkEnt=%d infl=%s "
                "dist=%.0f at=%.0f %.0f %.0f\n",
                entnum,
                (int)health,
                ev->GetFloat(2),
                mod,
                cAtk,
                tAtk,
                pAtk ? pAtk->entnum : -1,
                cInf,
                fDist,
                origin.x,
                origin.y,
                origin.z
            );
        }
    }

    if (g_gametype->integer != GT_SINGLE_PLAYER) {
        // players that are not allowed fighting mustn't take damage
        if (!m_bAllowFighting && mod != MOD_TELEFRAG) {
            return;
        }

        Player *attacker = (Player *)ev->GetEntity(1);

        if (attacker && attacker->IsSubclassOfPlayer()) {
            if (attacker != this) {
                if (g_gametype->integer >= GT_TEAM && !g_teamdamage->integer) {
                    // check for team damage
                    if (attacker->GetDM_Team() == GetDM_Team() && mod != MOD_TELEFRAG) {
                        return;
                    }
                }

                pAttackerDistPointer = attacker;
                fAttackerDispTime    = g_drawattackertime->value + level.time;
            }
        }
    }

    m_iNumHitsTaken++;

    Sentient::ArmorDamage(ev);

    Event *event = new Event;

    event->AddEntity(ev->GetEntity(1));
    event->AddFloat(ev->GetFloat(2));
    event->AddEntity(ev->GetEntity(3));
    event->AddVector(ev->GetVector(4));
    event->AddVector(ev->GetVector(5));
    event->AddVector(ev->GetVector(6));
    event->AddInteger(ev->GetInteger(7));
    event->AddInteger(ev->GetInteger(8));
    event->AddInteger(ev->GetInteger(9));
    event->AddInteger(ev->GetInteger(10));
    event->AddEntity(this);

    scriptDelegate_damage.Trigger(this, *event);
    scriptedEvents[SE_DAMAGE].Trigger(event);
}

void Player::Disconnect(void)
{
    Event *ev = new Event;
    ev->AddListener(this);

    scriptDelegate_disconnecting.Trigger(this, *ev);
    scriptedEvents[SE_DISCONNECTED].Trigger(ev);

    //     if (g_gametype->integer != GT_SINGLE_PLAYER) {
    //         dmManager.RemovePlayer(this);
    //     }
}

void Player::CallVote(Event *ev)
{
    str arg1;
    str arg2;
    int numVoters;

    if (g_gametype->integer == GT_SINGLE_PLAYER) {
        return;
    }

    if (!g_allowvote->integer) {
        HUDPrint(va("%s\n", gi.LV_ConvertString("Voting not allowed here.")));
        return;
    }

    if (level.m_voteTime != 0.0f) {
        HUDPrint(va("%s\n", gi.LV_ConvertString("A vote is already in progress.")));
        return;
    }

    if (votecount >= MAX_VOTE_COUNT) {
        if (m_fLastVoteTime) {
            while (m_fLastVoteTime < level.time && votecount > 0) {
                m_fLastVoteTime += 60;
                votecount--;
            }
        }

        if (votecount >= MAX_VOTE_COUNT) {
            HUDPrint(
                va("%s %d %s.\n",
                   gi.LV_ConvertString("You cannot call another vote for"),
                   (unsigned int)(m_fLastVoteTime - level.time + 1),
                   gi.LV_ConvertString("seconds"))
            );
            return;
        }
    }

    if (IsSpectator() || IsDead()) {
        HUDPrint(va("%s\n", gi.LV_ConvertString("You are not allowed to call a vote as a spectator.")));
        return;
    }

    arg1 = ev->GetString(1);
    if (ev->NumArgs() > 1) {
        arg2 = ev->GetString(2);
    }

    if (!atoi(arg1.c_str())) {
        if (strchr(arg1.c_str(), ';') || strchr(arg2.c_str(), ';')) {
            HUDPrint(va("%s\n", gi.LV_ConvertString("Invalid vote string.")));
            return;
        }

        if (Q_stricmp(arg1.c_str(), "restart") && Q_stricmp(arg1.c_str(), "nextmap") && Q_stricmp(arg1.c_str(), "map")
            && Q_stricmp(arg1.c_str(), "g_gametype") && Q_stricmp(arg1.c_str(), "kick")
            && Q_stricmp(arg1.c_str(), "clientkick") && Q_stricmp(arg1.c_str(), "fraglimit")) {
            HUDPrint(va("%s\n", gi.LV_ConvertString("Invalid vote string.")));
            HUDPrint(va(
                "%s restart, nextmap, map <mapname>, g_gametype <n>, fraglimit <n>, timelimit <n>, kick <player>, and "
                "clientkick <player #>.\n",
                gi.LV_ConvertString("Vote commands are:")
            ));

            return;
        }

        if (!Q_stricmp(arg1.c_str(), "kick")) {
            //
            // check for a valid player
            //
            gentity_t *ent;
            int        i;

            for (i = 0; i < game.maxclients; i++) {
                ent = &g_entities[i];

                if (!ent->inuse || !ent->client || !ent->entity) {
                    continue;
                }

                if (!Q_stricmp(ent->client->pers.netname, arg2.c_str())) {
                    // Prevent the player from kicking himself out
                    if (ent->entity == this) {
                        HUDPrint(va("%s\n", gi.LV_ConvertString("You are not allowed to kick yourself.")));
                        return;
                    }

                    break;
                }
            }

            if (i == game.maxclients) {
                HUDPrint(va("%s %s\n", arg2.c_str(), gi.LV_ConvertString("is not a valid player name to kick.")));
                return;
            }
        } else if (!Q_stricmp(arg1.c_str(), "map") && *sv_nextmap->string) {
            level.m_voteString = va("%s %s; set next map \"%s\"", arg1.c_str(), arg2.c_str(), arg2.c_str());
        } else {
            level.m_voteString = va("%s %s", arg1.c_str(), arg2.c_str());
        }

        if (level.m_nextVoteTime) {
            level.m_nextVoteTime = 0;
            gi.SendConsoleCommand(va("%s", level.m_voteString.c_str()));
        }

        if (!Q_stricmp(arg1.c_str(), "g_gametype")) {
            int gametypeNum;

            // get the gametype number
            gametypeNum = atoi(arg2.c_str());
            if (gametypeNum <= GT_SINGLE_PLAYER || gametypeNum >= GT_MAX_GAME_TYPE) {
                HUDPrint(va("%s\n", gi.LV_ConvertString("Invalid gametype for a vote.")));
                return;
            }

            level.m_voteString = va("%s %i", arg1.c_str(), gametypeNum);

            switch (gametypeNum) {
            case GT_FFA:
                level.m_voteName = "Game Type Free-For-All";
                break;
            case GT_TEAM:
                level.m_voteName = "Game Type Match";
                break;
            case GT_TEAM_ROUNDS:
                level.m_voteName = "Game Type Round-Based-Match";
                break;
            case GT_OBJECTIVE:
                level.m_voteName = "Game Type Objective-Match";
                break;
            case GT_TOW:
                level.m_voteName = "Game Type Tug of War";
                break;
            case GT_LIBERATION:
                level.m_voteName = "Game Type Liberation";
                break;
            default:
                HUDPrint(va("%s %s %d\n", gi.LV_ConvertString("Game Type"), arg1.c_str(), gametypeNum));
                return;
            }
        } else if (!Q_stricmp(arg1.c_str(), "map")) {
            if (*sv_nextmap->string) {
                level.m_voteString = va("%s %s; set nextmap \"%s\"", arg1.c_str(), arg2.c_str(), sv_nextmap->string);
            } else {
                level.m_voteString = va("%s %s", arg1.c_str(), arg2.c_str());
            }

            level.m_voteName = va("Map %s", arg2.c_str());
        } else {
            level.m_voteString = va("%s %s", arg1.c_str(), arg2.c_str());
            level.m_voteName   = level.m_voteString;
        }
    } else {
        int              voteIndex;
        int              subListIndex;
        str              voteOptionCommand;
        str              voteOptionSubCommand;
        str              voteOptionName;
        str              voteOptionSubName;
        voteoptiontype_t optionType;

        union {
            int   optionInteger;
            float optionFloat;
            int   optionClientNum;
        };

        gentity_t *ent;

        char buffer[64];

        voteIndex = atoi(arg1.c_str());
        if (!level.GetVoteOptionMain(voteIndex, &voteOptionCommand, &optionType)) {
            HUDPrint(va("%s\n", gi.LV_ConvertString("Invalid vote option.")));
            return;
        }

        level.GetVoteOptionMainName(voteIndex, &voteOptionName);

        switch (optionType) {
        case VOTE_NO_CHOICES:
            level.m_voteString = voteOptionCommand;
            level.m_voteName   = voteOptionName;
            break;
        case VOTE_OPTION_LIST:
            subListIndex = atoi(arg2.c_str());

            if (!level.GetVoteOptionSub(voteIndex, subListIndex, &voteOptionSubCommand)) {
                HUDPrint(
                    va("%s %i %s \"%s\".\n",
                       gi.LV_ConvertString("Invalid vote choice"),
                       subListIndex,
                       gi.LV_ConvertString("for vote option"),
                       voteOptionName.c_str())
                );
                return;
            }

            level.m_voteString = va("%s %s", voteOptionCommand.c_str(), voteOptionSubCommand.c_str());
            // get the sub-option name
            level.GetVoteOptionSubName(voteIndex, subListIndex, &voteOptionSubName);
            level.m_voteName =
                va("%s %s", gi.LV_ConvertString(voteOptionName.c_str()), gi.LV_ConvertString(voteOptionSubName.c_str())
                );
            break;
        case VOTE_OPTION_TEXT:
            if (strchr(arg2.c_str(), ';')) {
                HUDPrint(va("%s\n", gi.LV_ConvertString("Invalid vote text entered.")));
                return;
            }

            level.m_voteString = va("%s %s", voteOptionCommand.c_str(), arg2.c_str());
            level.m_voteName   = va("%s %s", gi.LV_ConvertString(voteOptionName.c_str()), arg2.c_str());
            break;
        case VOTE_OPTION_INTEGER:
            optionInteger = atoi(arg2.c_str());
            Com_sprintf(buffer, sizeof(buffer), "%d", optionInteger);

            if (Q_stricmp(buffer, arg2.c_str())) {
                HUDPrint(va("%s\n", gi.LV_ConvertString("Invalid vote integer entered.")));
                return;
            }

            level.m_voteString = va("%s %i", voteOptionCommand.c_str(), optionInteger);
            level.m_voteName   = va("%s %i", gi.LV_ConvertString(voteOptionName.c_str()), optionInteger);
            break;
        case VOTE_OPTION_FLOAT:
            optionFloat = atof(arg2.c_str());
            Com_sprintf(buffer, sizeof(buffer), "%f", optionFloat);

            if (Q_stricmp(buffer, arg2.c_str())) {
                HUDPrint(va("%s\n", gi.LV_ConvertString("Invalid vote float entered.")));
                return;
            }

            level.m_voteString = va("%s %g", voteOptionCommand.c_str(), optionFloat);
            level.m_voteName   = va("%s %g", gi.LV_ConvertString(voteOptionName.c_str()), optionFloat);
            break;
        case VOTE_OPTION_CLIENT:
        case VOTE_OPTION_CLIENT_NOT_SELF:
            optionClientNum = atoi(arg2.c_str());
            if (optionClientNum < 0 || optionClientNum >= game.maxclients) {
                HUDPrint(va("%s\n", gi.LV_ConvertString("Invalid client number for a vote.")));
                return;
            }

            ent = &g_entities[optionClientNum];
            if (!ent->inuse || !ent->client || !ent->entity) {
                HUDPrint(va("%s\n", gi.LV_ConvertString("Client selected for the vote is not connected.")));
                return;
            }

            level.m_voteString = va("%s %i", voteOptionCommand.c_str(), optionClientNum);
            level.m_voteName =
                va("%s #%i: %s", gi.LV_ConvertString(voteOptionName.c_str()), optionClientNum, ent->client->pers.netname
                );
            break;
        default:
            level.GetVoteOptionMainName(voteIndex, &voteOptionName);
            gi.Printf(
                "ERROR: Vote option (\"%s\" \"%s\") with unknown vote option type\n",
                voteOptionName.c_str(),
                voteOptionCommand.c_str()
            );
            return;
        }
    }

    G_PrintfClient(edict, "called a vote (%s %s)\n", arg1.c_str(), arg2.c_str());
    G_PrintToAllClients(va("%s %s.\n", client->pers.netname, gi.LV_ConvertString("called a vote")));

    level.m_voteTime = (level.svsFloatTime - level.svsStartFloatTime) * 1000;
    level.m_voteYes  = 1;
    level.m_voteNo   = 0;

    // Reset all player's vote
    numVoters = 0;

    for (int i = 0; i < game.maxclients; i++) {
        gentity_t *ent = &g_entities[i];

        if (!ent->client || !ent->inuse) {
            continue;
        }

        Player *p = (Player *)ent->entity;
        p->voted  = false;

        numVoters++;
    }

    level.m_numVoters = numVoters;
    client->ps.voted  = true;
    voted             = true;
    votecount++;

    m_fLastVoteTime = level.time + 60;

    if (g_protocol >= protocol_e::PROTOCOL_MOHTA_MIN) {
        //
        // clients below version 2.0 don't support vote cs
        //
        gi.setConfigstring(CS_VOTE_TIME, va("%i", level.m_voteTime));
        gi.setConfigstring(CS_VOTE_STRING, level.m_voteName.c_str());
        gi.setConfigstring(CS_VOTE_YES, va("%i", level.m_voteYes));
        gi.setConfigstring(CS_VOTE_NO, va("%i", level.m_voteNo));
        gi.setConfigstring(CS_VOTE_UNDECIDED, va("%i", level.m_numVoters - (level.m_voteYes + level.m_voteNo)));
    }
}

void Player::Vote(Event *ev)
{
    str arg1;

    if (level.m_voteTime == 0.0f) {
        HUDPrint(gi.LV_ConvertString("No vote in progress."));
        return;
    }

    if (client->ps.voted) {
        HUDPrint(gi.LV_ConvertString("Vote already cast."));
        return;
    }

    if (ev->NumArgs() != 1) {
        HUDPrint(va("%s: vote <1|0|y|n>", gi.LV_ConvertString("Usage")));
        return;
    }

    HUDPrint(gi.LV_ConvertString("Vote cast."));
    client->ps.voted = true;

    arg1  = ev->GetString(1);
    voted = (arg1[0] == 'y') || (arg1[0] == 'Y') || (arg1[0] == '1');
}

void Player::RetrieveVoteOptions(Event *ev)
{
    if (m_fNextVoteOptionTime > level.time) {
        gi.SendServerCommand(edict - g_entities, "vo0 \"\"\n");
        gi.SendServerCommand(edict - g_entities, "vo2 \"\"\n");
    } else {
        m_fNextVoteOptionTime = level.time + 2.0;
        level.SendVoteOptionsFile(edict);
    }
}

void Player::EventPrimaryDMWeapon(Event *ev)
{
    str  dm_weapon = ev->GetString(1);
    bool bIsBanned = false;

    if (!dm_weapon.length()) {
        // Added in OPM.
        //  Prevent the player from cheating by going into spectator
        return;
    }

    if (g_gametype->integer == GT_SINGLE_PLAYER) {
        // Added in OPM
        //  No primary weapon in single player
        return;
    }

    if (!str::icmp(dm_weapon, "shotgun")) {
        bIsBanned = (dmflags->integer & DF_WEAPON_NO_SHOTGUN);
    } else if (!str::icmp(dm_weapon, "rifle")) {
        bIsBanned = (dmflags->integer & DF_WEAPON_NO_RIFLE);
    } else if (!str::icmp(dm_weapon, "sniper")) {
        bIsBanned = (dmflags->integer & DF_WEAPON_NO_SNIPER);
    } else if (!str::icmp(dm_weapon, "smg")) {
        bIsBanned = (dmflags->integer & DF_WEAPON_NO_SMG);
    } else if (!str::icmp(dm_weapon, "mg")) {
        bIsBanned = (dmflags->integer & DF_WEAPON_NO_MG);
    } else if (!str::icmp(dm_weapon, "heavy")) {
        bIsBanned = (dmflags->integer & DF_WEAPON_NO_ROCKET);
    } else if (!str::icmp(dm_weapon, "landmine")) {
        bIsBanned = (dmflags->integer & DF_WEAPON_NO_LANDMINE) || !QueryLandminesAllowed();
    } else if (!str::icmp(dm_weapon, "auto")) {
        const char *primaryList[7];
        size_t      numPrimaries = 0;

        //
        // Added in OPM
        //  Choose a random allowed weapon
        //
        if (!(dmflags->integer & DF_WEAPON_NO_SHOTGUN)) {
            primaryList[numPrimaries++] = "shotgun";
        }
        if (!(dmflags->integer & DF_WEAPON_NO_RIFLE)) {
            primaryList[numPrimaries++] = "rifle";
        }
        if (!(dmflags->integer & DF_WEAPON_NO_SNIPER)) {
            primaryList[numPrimaries++] = "sniper";
        }
        if (!(dmflags->integer & DF_WEAPON_NO_SMG)) {
            primaryList[numPrimaries++] = "smg";
        }
        if (!(dmflags->integer & DF_WEAPON_NO_MG)) {
            primaryList[numPrimaries++] = "mg";
        }
        if (!(dmflags->integer & DF_WEAPON_NO_ROCKET)) {
            primaryList[numPrimaries++] = "heavy";
        }
        if (!(dmflags->integer & DF_WEAPON_NO_LANDMINE) && QueryLandminesAllowed()) {
            primaryList[numPrimaries++] = "landmine";
        }

        if (numPrimaries) {
            dm_weapon = primaryList[rand() % numPrimaries];
        } else {
            bIsBanned = qtrue;
        }
    }

    if (bIsBanned) {
        gi.SendServerCommand(
            edict - g_entities, "print \"" HUD_MESSAGE_WHITE "%s\n\"", "That weapon is currently banned."
        );
        return;
    }

    Q_strncpyz(client->pers.dm_primary, dm_weapon.c_str(), sizeof(client->pers.dm_primary));

    if (m_bSpectator) {
        if (current_team && (current_team->m_teamnumber == TEAM_AXIS || current_team->m_teamnumber == TEAM_ALLIES)) {
            if (m_fSpawnTimeLeft) {
                m_bWaitingForRespawn = true;
            } else if (AllowTeamRespawn()) {
                EndSpectator();

                if (deadflag) {
                    deadflag = DEAD_DEAD;
                }

                PostEvent(EV_Player_Respawn, 0);

                gi.centerprintf(edict, "");
            }
        } else {
            gi.SendServerCommand(edict - g_entities, "stufftext \"wait 250;pushmenu_teamselect\"");
        }
    } else {
        gi.SendServerCommand(
            edict - g_entities, "print \"" HUD_MESSAGE_WHITE "%s\n\"", "Will switch to new weapon next time you respawn"
        );
    }
}

void Player::DeadBody(Event *ev)
{
    Body *body;

    if (knockdown) {
        return;
    }

    knockdown = true;

    body = new Body;
    body->setModel(model);

    for (int i = 0; i < MAX_FRAMEINFOS; i++) {
        body->edict->s.frameInfo[i] = edict->s.frameInfo[i];
    }

    body->edict->s.actionWeight = edict->s.actionWeight;
    body->edict->s.scale        = edict->s.scale;

    body->setOrigin(origin);
    body->setAngles(angles);

    body->edict->s.eFlags &= ~(EF_AXIS | EF_ALLIES);

    if (GetTeam() == TEAM_ALLIES) {
        body->edict->s.eFlags |= EF_ALLIES;
    } else if (GetTeam() == TEAM_AXIS) {
        body->edict->s.eFlags |= EF_AXIS;
    }
}

void Player::WonMatch(void)
{
    num_won_matches++;
}

void Player::LostMatch(void)
{
    num_lost_matches++;
}

void Player::ArmWithWeapons(Event *ev)
{
    EquipWeapons();
}

void Player::EventGetCurrentDMWeaponType(Event *ev)
{
    ev->AddString(GetCurrentDMWeaponType());
}

void Player::PhysicsOff(Event *ev)
{
    if (g_target_game > TG_MOH || g_gametype->integer != GT_SINGLE_PLAYER) {
        // Added in 2.0
        //  Reset the state to STAND before disabling physics
        EvaluateState(statemap_Torso->FindState("STAND"), statemap_Legs->FindState("STAND"));
    }

    flags |= FL_IMMOBILE;
}

void Player::PhysicsOn(Event *ev)
{
    flags &= ~FL_IMMOBILE;
}

void Player::GetIsSpectator(Event *ev)
{
    ev->AddInteger(IsSpectator());
}

void Player::EventSetInJail(Event *ev)
{
    m_bIsInJail = ev->GetBoolean(1);
}

bool Player::IsInJail() const
{
    return m_bIsInJail;
}

void Player::EventGetInJail(Event *ev)
{
    ev->AddInteger(m_bIsInJail);
}

void Player::GetNationalityPrefix(Event *ev)
{
    nationality_t nationality;

    if (GetTeam() == TEAM_AXIS) {
        nationality = GetPlayerAxisTeamType(client->pers.dm_playergermanmodel);
    } else {
        nationality = GetPlayerAlliedTeamType(client->pers.dm_playermodel);
    }

    switch (nationality) {
    case NA_RUSSIAN:
        ev->AddString("dfrru");
        break;
    case NA_ITALIAN:
        ev->AddString("denit");
        break;
    case NA_BRITISH:
        ev->AddString("dfruk");
        break;
    case NA_AMERICAN:
        ev->AddString("dfr");
        break;

    case NA_NONE:
    default:
        ev->AddString("dfr");
        break;
    }
}

void Player::GetIsDisguised(Event *ev)
{
    ev->AddInteger(m_bIsDisguised);
}

void Player::GetHasDisguise(Event *ev)
{
    ev->AddInteger(m_bHasDisguise);
}

void Player::SetHasDisguise(Event *ev)
{
    m_bHasDisguise = ev->GetBoolean(1);
}

void Player::SetObjectiveCount(Event *ev)
{
    m_iObjectivesCompleted = ev->GetInteger(1);
    m_iNumObjectives       = ev->GetInteger(2);
}

void Player::Stats(Event *ev)
{
    char entry[2048];
    int  i;
    str  szPreferredWeapon;
    str  szGunneryEvaluation;
    int  iNumHeadShots;
    int  iNumTorsoShots;
    int  iNumLeftLegShots;
    int  iNumRightLegShots;
    int  iNumGroinShots;
    int  iNumLeftArmShots;
    int  iNumRightArmShots;
    int  iNumShotsFired;
    int  iNumHits;
    int  iBestNumHits;

    if (g_gametype->integer != GT_SINGLE_PLAYER) {
        // Only works in singleplayer
        return;
    }

    szPreferredWeapon   = "none";
    szGunneryEvaluation = "none";
    iNumHeadShots       = m_iNumHeadShots;
    iNumTorsoShots      = m_iNumTorsoShots;
    iNumLeftLegShots    = m_iNumLeftLegShots;
    iNumRightLegShots   = m_iNumRightLegShots;
    iNumGroinShots      = m_iNumGroinShots;
    iNumLeftArmShots    = m_iNumLeftArmShots;
    iNumRightArmShots   = m_iNumRightArmShots;
    iNumShotsFired      = m_iNumShotsFired;
    iNumHits            = m_iNumHits;
    iBestNumHits        = 0;

    for (i = 1; i <= inventory.NumObjects(); i++) {
        Entity *pEnt = G_GetEntity(inventory.ObjectAt(i));
        if (pEnt && pEnt->IsSubclassOfWeapon()) {
            Weapon *pWeap = static_cast<Weapon *>(pEnt);

            iNumHeadShots += pWeap->m_iNumHeadShots;
            iNumTorsoShots += pWeap->m_iNumTorsoShots;
            iNumLeftLegShots += pWeap->m_iNumLeftLegShots;
            iNumRightLegShots += pWeap->m_iNumRightLegShots;
            iNumGroinShots += pWeap->m_iNumGroinShots;
            iNumLeftArmShots += pWeap->m_iNumLeftArmShots;
            iNumRightArmShots += pWeap->m_iNumRightArmShots;
            iNumShotsFired += pWeap->m_iNumShotsFired;
            iNumHits += pWeap->m_iNumHits;

            if (pWeap->m_iNumHits > iBestNumHits) {
                szPreferredWeapon = pWeap->item_name;
                iBestNumHits      = pWeap->m_iNumHits;
            }
        }
    }

    if (m_sPerferredWeaponOverride.length()) {
        szPreferredWeapon = m_sPerferredWeaponOverride;
    }

    if (iNumHits) {
        Com_sprintf(
            entry,
            sizeof(entry),
            "%i %i %i %i %.1f \"%s\" %i %i %i \"%.1f\" \"%.1f\" \"%.1f\" \"%.1f\" \"%.1f\" \"%.1f\" \"%.1f\" \"%s\" %i "
            "%i %i",
            m_iNumObjectives,
            m_iObjectivesCompleted,
            iNumShotsFired,
            iNumHits,
            ((float)iNumHits / (float)iNumShotsFired * 100.f),
            szPreferredWeapon.c_str(),
            m_iNumHitsTaken,
            m_iNumObjectsDestroyed,
            m_iNumEnemiesKilled,
            iNumHeadShots * 100.f / iNumHits,
            iNumTorsoShots * 100.f / iNumHits,
            iNumLeftLegShots * 100.f / iNumHits,
            iNumRightLegShots * 100.f / iNumHits,
            iNumGroinShots * 100.f / iNumHits,
            iNumLeftArmShots * 100.f / iNumHits,
            iNumRightArmShots * 100.f / iNumHits,
            szGunneryEvaluation.c_str(),
            g_gotmedal->integer,
            g_success->integer,
            g_failed->integer
        );
    } else {
        Com_sprintf(
            entry,
            sizeof(entry),
            "%i %i %i %i %i \"%s\" %i %i %i \"%i\" \"%i\" \"%i\" \"%i\" \"%i\" \"%i\" \"%i\" \"%s\" %i %i %i",
            m_iNumObjectives,
            m_iObjectivesCompleted,
            iNumShotsFired,
            0,
            0,
            szPreferredWeapon.c_str(),
            m_iNumHitsTaken,
            m_iNumObjectsDestroyed,
            m_iNumEnemiesKilled,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            szGunneryEvaluation.c_str(),
            g_gotmedal->integer,
            g_success->integer,
            g_failed->integer
        );
    }

    gi.SendServerCommand(edict - g_entities, "stats %s", entry);
}

void Player::EventStuffText(Event *ev)
{
    if (level.spawning) {
        Event *event = new Event(EV_Player_StuffText);
        event->AddValue(ev->GetValue(1));
        PostEvent(event, level.frametime, 0);
        return;
    }

    gi.SendServerCommand(edict - g_entities, "stufftext \"%s\"", ev->GetString(1).c_str());

    delegate_stufftext.Execute(ev->GetString(1));
}

void Player::EventSetVoiceType(Event *ev)
{
    str sVoiceName = ev->GetString(1);

    if (g_protocol >= protocol_e::PROTOCOL_MOHTA_MIN) {
        if (!sVoiceName.icmp("american")) {
            m_voiceType = PVT_ALLIED_AMERICAN;
        } else if (!sVoiceName.icmp("british")) {
            m_voiceType = PVT_ALLIED_BRITISH;
        } else if (!sVoiceName.icmp("russian")) {
            m_voiceType = PVT_ALLIED_RUSSIAN;
        } else if (!sVoiceName.icmp("german")) {
            m_voiceType = PVT_AXIS_GERMAN;
        } else if (!sVoiceName.icmp("italian")) {
            m_voiceType = PVT_AXIS_ITALIAN;
        } else {
            m_voiceType = PVT_NONE_SET;
        }
    } else {
        if (!sVoiceName.icmp("airborne")) {
            m_voiceType = PVT_ALLIED_AIRBORNE;
        } else if (!sVoiceName.icmp("manon")) {
            m_voiceType = PVT_ALLIED_MANON;
        } else if (!sVoiceName.icmp("SAS")) {
            m_voiceType = PVT_ALLIED_SAS;
        } else if (!sVoiceName.icmp("pilot")) {
            m_voiceType = PVT_ALLIED_PILOT;
        } else if (!sVoiceName.icmp("army")) {
            m_voiceType = PVT_ALLIED_ARMY;
        } else if (!sVoiceName.icmp("ranger")) {
            m_voiceType = PVT_ALLIED_RANGER;
        } else if (!sVoiceName.icmp("axis1")) {
            m_voiceType = PVT_AXIS_AXIS1;
        } else if (!sVoiceName.icmp("axis2")) {
            m_voiceType = PVT_AXIS_AXIS2;
        } else if (!sVoiceName.icmp("axis3")) {
            m_voiceType = PVT_AXIS_AXIS3;
        } else if (!sVoiceName.icmp("axis4")) {
            m_voiceType = PVT_AXIS_AXIS4;
        } else if (!sVoiceName.icmp("axis5")) {
            m_voiceType = PVT_AXIS_AXIS5;
        } else {
            m_voiceType = PVT_NONE_SET;
        }
    }
}

void Player::GetTeamDialogPrefix(str& outPrefix)
{
    if (GetTeam() == TEAM_AXIS) {
        outPrefix = "axis_";
    } else {
        outPrefix = "allied_";
    }

    if (g_protocol >= protocol_e::PROTOCOL_MOHTA_MIN) {
        switch (m_voiceType) {
        case PVT_ALLIED_AMERICAN:
            outPrefix += "american_";
            break;
        case PVT_ALLIED_BRITISH:
            outPrefix += "british_";
            break;
        case PVT_ALLIED_RUSSIAN:
            outPrefix += "russian_";
            break;
        case PVT_AXIS_START:
            outPrefix += "german_";
            break;
        case PVT_AXIS_ITALIAN:
            outPrefix += "italian_";
            break;
        default:
            if (GetTeam() == TEAM_AXIS) {
                outPrefix += "german_";
            } else {
                outPrefix += "american_";
            }
            break;
        }
    } else {
        switch (m_voiceType) {
        case PVT_ALLIED_AIRBORNE:
            outPrefix += "airborne_";
            break;

        case PVT_ALLIED_MANON:
            outPrefix += "manon_";
            break;

        case PVT_ALLIED_SAS:
            outPrefix += "sas_";
            break;

        case PVT_ALLIED_PILOT:
            outPrefix += "pilot_";
            break;

        case PVT_ALLIED_ARMY:
            outPrefix += "army_";
            break;

        case PVT_ALLIED_RANGER:
            outPrefix += "ranger_";
            break;

        case PVT_AXIS_AXIS1:
            outPrefix += "axis1_";
            break;

        case PVT_AXIS_AXIS2:
            outPrefix += "axis2_";
            break;

        case PVT_AXIS_AXIS3:
            outPrefix += "axis3_";
            break;

        case PVT_AXIS_AXIS4:
            outPrefix += "axis4_";
            break;

        case PVT_AXIS_AXIS5:
            outPrefix += "axis5_";
            break;

        default:
            if (dm_team != TEAM_AXIS) {
                outPrefix += "army_";
            } else {
                outPrefix += "axis4_";
            }
        }
    }
}

void Player::PlayInstantMessageSound(const char *name)
{
    str soundName;

    if (g_protocol < PROTOCOL_MOHTA_MIN) {
        gi.DPrintf2("Instant message sound isn't supported on protocol below version 15");
        return;
    }

    GetTeamDialogPrefix(soundName);

    soundName += name;

    gi.MSG_SetClient(client->ps.clientNum);
    gi.MSG_StartCGM(CGM_VOICE_CHAT);
    gi.MSG_WriteCoord(m_vViewPos[0]);
    gi.MSG_WriteCoord(m_vViewPos[1]);
    gi.MSG_WriteCoord(m_vViewPos[2]);
    gi.MSG_WriteBits(0, 1);
    gi.MSG_WriteBits(edict - g_entities, 6);
    gi.MSG_WriteString(soundName.c_str());
    gi.MSG_EndCGM();
}

void Player::EventDMMessage(Event *ev)
{
    static constexpr unsigned int MAX_SAY_TEXT_LENGTH = 400;

    int i;
    //int              iStringLength;
    int              iMode = 0;
    str              sToken;
    // Changed in OPM
    //  it's MAX_STRING_CHARS in mohaa
    // Add 1 character for the newline
    char             szPrintString[MAX_SAY_TEXT_LENGTH];
    const char       *pStartMessage = szPrintString;
    size_t           iStringLength;
    const char      *pTmpInstantMsg = "";
    qboolean         bInstaMessage  = qfalse;
    AliasListNode_t *pSoundAlias    = NULL;
    const char      *pszAliasname   = NULL;
    str              sAliasName;
    str              sRandomAlias;
    gentity_t       *ent;

    if (g_gametype->integer == GT_SINGLE_PLAYER) {
        return;
    }

    if (ev->NumArgs() <= 1) {
        return;
    }

    if (!Q_stricmp(client->pers.netname, "console")) {
        // Added in OPM
        //  Reserved name
        gi.Printf(
            "Client %d trying to send a message using a reserved name ('%s')\n",
            edict - g_entities,
            client->pers.netname
        );
        return;
    }

    sToken = ev->GetString(2);

    // Check for taunts
    if (sToken.length() == 3 && *sToken == '*' && sToken[1] > '0' && sToken[1] <= '9' && sToken[2] > '0'
        && sToken[2] <= '9') {
        unsigned int n1, n2;
        unsigned int globalTauntIndex;

        if (IsSpectator() || IsDead()) {
            // spectators or death players can't talk
            return;
        }

        if (edict->r.svFlags & SVF_NOCLIENT) {
            // Changed in OPM
            //  Don't send a voice chat message if the entity is not sent to client
            return;
        }

        if (!g_instamsg_allowed->integer) {
            // Added in OPM
            return;
        }

        if (g_instamsg_minDelay->integer > 0 && level.inttime < m_iInstantMessageTime + g_instamsg_minDelay->integer) {
            // Added in OPM
            return;
        }

        GetTeamDialogPrefix(sAliasName);
        if (g_target_game >= target_game_e::TG_MOHTT && sToken[1] == '6') {
            // Added in 2.30
            //  Liberation messages
            sAliasName += va("lib%c", (sToken[2] + '0'));
        } else {
            sAliasName += va("%c%c", (sToken[1] + '0'), (sToken[2] + '0'));
        }

        sRandomAlias = GetRandomAlias(sAliasName, &pSoundAlias);

        // find a random alias
        if (sRandomAlias.length() > 0) {
            pszAliasname = sRandomAlias.c_str();
        }

        if (!pszAliasname) {
            pszAliasname = gi.GlobalAlias_FindRandom(sAliasName, &pSoundAlias);
        }

        if (!pszAliasname || !pSoundAlias) {
            return;
        }

        n1 = sToken[1] - '1';
        n2 = sToken[2] - '1';

        if (g_protocol >= PROTOCOL_MOHTA_MIN) {
            if (n1 >= ARRAY_LEN(pInstantMsgEng) || n2 >= ARRAY_LEN(pInstantMsgEng[0])) {
                return;
            }

            pTmpInstantMsg   = pInstantMsgEng[n1][n2];
            globalTauntIndex = 4;
        } else {
            if (n1 >= ARRAY_LEN(pInstantMsgEng_ver6) || n2 >= ARRAY_LEN(pInstantMsgEng_ver6[0])) {
                return;
            }

            // fallback to old version
            pTmpInstantMsg   = pInstantMsgEng_ver6[n1][n2];
            globalTauntIndex = 3;
        }

        if (g_gametype->integer != GT_FFA) {
            if (n1 == globalTauntIndex) {
                // Taunts
                iMode = 0;
            } else {
                iMode = -1;
            }
        } else {
            iMode = 0;
        }

        bInstaMessage = qtrue;
    } else {
        if (!g_textmsg_allowed->integer) {
            // Added in OPM

            str errorString  = gi.LV_ConvertString("Message Error");
            str reasonString = gi.LV_ConvertString("Text chat is disabled on this server");

            gi.SendServerCommand(
                edict - g_entities,
                "print \"" HUD_MESSAGE_CHAT_WHITE "%s: %s.\n\"",
                errorString.c_str(),
                reasonString.c_str()
            );
            return;
        }

        if (g_textmsg_minDelay->integer > 0 && level.inttime < m_iTextChatTime + g_textmsg_minDelay->integer) {
            // Added in OPM
            return;
        }
    }

    if (bInstaMessage) {
        if (g_voiceChatTime->value > 0) {
            m_fTalkTime = g_voiceChatTime->value + level.time;
        }
        m_iInstantMessageTime = level.inttime;
    } else {
        iMode = ev->GetInteger(1);
        if (g_textChatTime->value > 0) {
            m_fTalkTime = g_textChatTime->value + level.time;
        }
        m_iTextChatTime = level.inttime;
    }

    Q_strncpyz(szPrintString, "print \"" HUD_MESSAGE_CHAT_WHITE, sizeof(szPrintString));

    if (m_bSpectator) {
        if (iMode <= 0) {
            Q_strcat(szPrintString, sizeof(szPrintString), gi.CL_LV_ConvertString("(spectator)"));
            Q_strcat(szPrintString, sizeof(szPrintString), " ");
        } else if (iMode <= game.maxclients) {
            ent = &g_entities[iMode - 1];

            if (ent->inuse && ent->entity && !static_cast<Player *>(ent->entity)->IsSpectator()) {
                str errorString = gi.LV_ConvertString("Message Error");
                str reasonString =
                    gi.LV_ConvertString("Spectators are not allowed to send private messages to non-spectators");

                gi.SendServerCommand(
                    edict - g_entities,
                    "print \"" HUD_MESSAGE_CHAT_WHITE "%s: %s.\n\"",
                    errorString.c_str(),
                    reasonString.c_str()
                );
                return;
            }
        }
    } else if (IsDead() || m_bTempSpectator) {
        if (iMode <= 0) {
            Q_strcat(szPrintString, sizeof(szPrintString), gi.CL_LV_ConvertString("(dead)"));
            Q_strcat(szPrintString, sizeof(szPrintString), " ");
        } else if (iMode <= game.maxclients) {
            ent = &g_entities[iMode - 1];

            if (ent->inuse && ent->entity && !static_cast<Player *>(ent->entity)->IsSpectator()) {
                str errorString = gi.LV_ConvertString("Message Error");
                str reasonString =
                    gi.LV_ConvertString("Dead players are not allowed to send private messages to active players");

                gi.SendServerCommand(
                    edict - g_entities,
                    "print \"" HUD_MESSAGE_CHAT_WHITE "%s: %s.\n\"",
                    errorString.c_str(),
                    reasonString.c_str()
                );
                return;
            }
        }
    } else if (iMode < 0) {
        Q_strcat(szPrintString, sizeof(szPrintString), gi.CL_LV_ConvertString("(team)"));
        Q_strcat(szPrintString, sizeof(szPrintString), " ");
    } else if (iMode > 0) {
        Q_strcat(szPrintString, sizeof(szPrintString), gi.CL_LV_ConvertString("(private)"));
        Q_strcat(szPrintString, sizeof(szPrintString), " ");
    } else {
        // Added in OPM
        //  Specify that the client is talking to everyone
        //  This was also a feature of Daven's fixes
        Q_strcat(szPrintString, sizeof(szPrintString), gi.CL_LV_ConvertString("(all)"));
        Q_strcat(szPrintString, sizeof(szPrintString), " ");
    }

    Q_strcat(szPrintString, sizeof(szPrintString), client->pers.netname);

    if (bInstaMessage) {
        Q_strcat(szPrintString, sizeof(szPrintString), ": ");
        Q_strcat(szPrintString, sizeof(szPrintString), gi.LV_ConvertString(pTmpInstantMsg));

        pStartMessage = pTmpInstantMsg;
    } else {
        bool met_comment = false;

        Q_strcat(szPrintString, sizeof(szPrintString), ":");

        // Added in OPM.
        //  Checks for comments in string (as COM_Parse will parse them)
        //  This was fixed in 2.0 but make the fix compatible with older versions
        if (g_protocol < protocol_e::PROTOCOL_MOHTA_MIN && strstr(client->pers.netname, "/*")) {
            met_comment = true;
        }

        pStartMessage = szPrintString + strlen(szPrintString);
        if (ev->NumArgs() > 1) {
            pStartMessage++;
        }

        for (i = 2; i <= ev->NumArgs(); i++) {
            sToken = ev->GetString(i);
            // Added in 2.40
            //  Special battle language tokens
            //  So players can easily tell their position, health, etc.
            sToken = TranslateBattleLanguageTokens(sToken);

            if (met_comment && strstr(sToken, "*/")) {
                // ignore messages containing comments
                return;
            }

            Q_strcat(szPrintString, sizeof(szPrintString), " ");
            Q_strcat(szPrintString, sizeof(szPrintString), gi.LV_ConvertString(sToken));

            iStringLength = strlen(szPrintString);
            if (iStringLength + sToken.length() > (ARRAY_LEN(szPrintString) - 1)) {
                break;
            }
        }
    }

    // ignore names containing comments
    if (g_protocol < protocol_e::PROTOCOL_MOHTA_MIN) {
        if (strstr(client->pers.netname, "//")
            || (strstr(client->pers.netname, "/*") && strstr(client->pers.netname, "*/"))) {
            return;
        }
    }

    if (iMode == 0) {
        //
        // everyone
        //

        if (!bInstaMessage) {
            Event          event;
            ScriptVariable result;
            // sent to everyone (not a team)
            event.AddString(sToken);
            event.AddInteger(false);

            result = scriptDelegate_textMessage.Trigger(this, event);
            if (result.HasValue()) {
                // Filtered out by a script
                G_PrintfClient(edict, "says @all (filtered out): %s\n", pStartMessage);

                if (result.IsString()) {
                    const str value = result.stringValue();
                    gi.SendServerCommand(
                        edict - g_entities, "print \"" HUD_MESSAGE_CHAT_WHITE "Script reply: %s\n\"", value.c_str()
                    );
                    return;
                } else if (!result.booleanValue()) {
                    return;
                }
            }
        }

        // Added in OPM
        if (bInstaMessage) {
            G_PrintfClient(edict, "shouts @all: %s\n", pStartMessage);
        } else {
            G_PrintfClient(edict, "says @all: %s\n", pStartMessage);
        }

        if (!IsSpectator() || g_spectate_allow_full_chat->integer) {
            for (i = 0; i < game.maxclients; i++) {
                ent = &g_entities[i];

                if (!ent->inuse || !ent->entity) {
                    continue;
                }

                gi.SendServerCommand(i, "%s\n", szPrintString);

                if (bInstaMessage) {
                    gi.MSG_SetClient(i);
                    gi.MSG_StartCGM(BG_MapCGMToProtocol(g_protocol, CGM_VOICE_CHAT));
                    gi.MSG_WriteCoord(m_vViewPos[0]);
                    gi.MSG_WriteCoord(m_vViewPos[1]);
                    gi.MSG_WriteCoord(m_vViewPos[2]);
                    gi.MSG_WriteBits(qtrue, 1);
                    gi.MSG_WriteBits(edict - g_entities, 6);
                    gi.MSG_WriteString(sAliasName.c_str());
                    gi.MSG_EndCGM();
                }
            }
        } else {
            //
            // send a message to spectators
            //
            for (i = 0; i < game.maxclients; i++) {
                ent = &g_entities[i];

                if (!ent->inuse || !ent->entity) {
                    continue;
                }

                if (!static_cast<Player *>(ent->entity)->IsSpectator()) {
                    continue;
                }

                gi.SendServerCommand(i, "%s\n", szPrintString);
            }
        }
    } else if (iMode < 0) {
        //
        // team message
        //

        if (!bInstaMessage) {
            Event          event;
            ScriptVariable result;
            // sent to team
            event.AddString(sToken);
            event.AddInteger(true);

            result = scriptDelegate_textMessage.Trigger(this, event);
            if (result.HasValue()) {
                // Filtered out by a script
                G_PrintfClient(edict, "says @team (filtered out): %s\n", pStartMessage);

                if (result.IsString()) {
                    const str value = result.stringValue();
                    gi.SendServerCommand(
                        edict - g_entities, "print \"" HUD_MESSAGE_CHAT_WHITE "Script reply: %s\n\"", value.c_str()
                    );
                    return;
                } else if (!result.booleanValue()) {
                    return;
                }
            }
        }

        // Added in OPM
        if (bInstaMessage) {
            G_PrintfClient(edict, "shouts @team: %s\n", pStartMessage);
        } else {
            G_PrintfClient(edict, "says @team: %s\n", pStartMessage);
        }

        if (IsSpectator()) {
            for (i = 0; i < game.maxclients; i++) {
                ent = &g_entities[i];

                if (!ent->inuse || !ent->entity) {
                    continue;
                }

                if (!static_cast<Player *>(ent->entity)->IsSpectator()) {
                    continue;
                }

                gi.SendServerCommand(i, "%s\n", szPrintString);
            }
        } else {
            for (i = 0; i < game.maxclients; i++) {
                bool bSameTeam;

                ent = &g_entities[i];

                if (!ent->inuse || !ent->entity) {
                    continue;
                }

                bSameTeam = static_cast<Player *>(ent->entity)->GetTeam() == GetTeam();
                if (bSameTeam) {
                    gi.SendServerCommand(i, "%s\n", szPrintString);
                }

                if (bInstaMessage) {
                    gi.MSG_SetClient(i);
                    gi.MSG_StartCGM(BG_MapCGMToProtocol(g_protocol, CGM_VOICE_CHAT));
                    gi.MSG_WriteCoord(m_vViewPos[0]);
                    gi.MSG_WriteCoord(m_vViewPos[1]);
                    gi.MSG_WriteCoord(m_vViewPos[2]);
                    gi.MSG_WriteBits(!bSameTeam, 1);
                    gi.MSG_WriteBits(edict - g_entities, 6);
                    gi.MSG_WriteString(sAliasName.c_str());
                    gi.MSG_EndCGM();
                }
            }
        }
    } else if (iMode <= game.maxclients) {
        ent = &g_entities[iMode - 1];

        if (!ent->inuse || !ent->entity) {
            str errorString  = gi.LV_ConvertString("Message Error");
            str reasonString = gi.LV_ConvertString("is not a connected client");

            gi.SendServerCommand(
                edict - g_entities,
                "print \"" HUD_MESSAGE_CHAT_WHITE "%s: %i %s.\n\"",
                errorString.c_str(),
                iMode,
                reasonString.c_str()
            );
            return;
        }

        // Added in OPM
        if (bInstaMessage) {
            G_PrintfClient(edict, "shouts @#%d: %s\n", iMode - 1, pStartMessage);
        } else {
            G_PrintfClient(edict, "says @#%d: %s\n", iMode - 1, pStartMessage);
        }

        gi.SendServerCommand(iMode - 1, "%s\n", szPrintString);

        if (ent->entity != this) {
            gi.SendServerCommand(
                edict - g_entities,
                "print \"" HUD_MESSAGE_CHAT_WHITE "%s %i:\n\"",
                gi.LV_ConvertString("Message to player"),
                iMode
            );
            gi.SendServerCommand(edict - g_entities, "%s\n", szPrintString);
        }
    } else {
        str errorString  = gi.LV_ConvertString("Message Error");
        str reasonString = gi.LV_ConvertString("is a bad client number");

        gi.SendServerCommand(
            edict - g_entities,
            "print \"" HUD_MESSAGE_CHAT_WHITE "%s: %i %s.\n\"",
            errorString.c_str(),
            iMode,
            reasonString.c_str()
        );
        return;
    }
}

str Player::GetBattleLanguageCondition() const
{
    int healthRatio;

    if (health <= 0) {
        return "dead";
    }

    if (health >= max_health) {
        return "just peachy";
    }

    healthRatio = (health * 5.f) / max_health;
    switch (healthRatio) {
    case 0:
        return "almost dead";
    case 1:
        return "severely wounded";
    case 2:
        return "wounded";
    case 3:
        return "slightly wounded";
    case 4:
    default:
        return "pretty good";
    }
}

str Player::GetBattleLanguageDirection() const
{
    int dir = ((m_vViewAng.y - world->m_fNorth) + 22.5f + 360.f) / 45.f;
    switch (dir % 8) {
    case 0:
        return "North";
    case 1:
        return "North West";
    case 2:
        return "West";
    case 3:
        return "South West";
    case 4:
        return "South";
    case 5:
        return "South East";
    case 6:
        return "East";
    case 7:
        return "North East";
    default:
        return "???";
    }
}

str Player::GetBattleLanguageLocation() const
{
    return gi.CL_LV_ConvertString(level.GetDMLocation(m_vViewPos).c_str());
}

str Player::GetBattleLanguageLocalFolks()
{
    static char buf[256];
    char       *p;
    char       *curP;
    size_t      remaining;
    size_t      length;
    Player     *pPlayer;
    Player     *pFolk;
    gentity_t  *ent;
    int         i;

    remaining = ARRAY_LEN(buf) - 1;
    p         = buf;
    curP      = NULL;
    pFolk     = NULL;

    for (i = 0; i < game.maxclients; i++) {
        ent = &g_entities[i];
        if (!ent->inuse || !ent->entity) {
            continue;
        }

        pPlayer = static_cast<Player *>(ent->entity);
        if (pPlayer != this && pPlayer->GetTeam() == GetTeam() && CanSee(pPlayer, 360, 1600, false)) {
            if (p != buf) {
                if (remaining < 2) {
                    // No more space remaining
                    break;
                }

                Q_strncpyz(p, ", ", sizeof(buf) - (p - buf));
                p += 2;
                curP = p;
                remaining -= 2;
            }

            length = strlen(client->pers.netname);
            if (remaining < length) {
                break;
            }

            Q_strncpyz(p, client->pers.netname, sizeof(buf) - (p - buf));
            p += length;
            remaining -= length;
            pFolk = pPlayer;
        }
    }

    if (curP && remaining >= 2) {
        Q_strncpyz(curP, "and ", sizeof(buf) - (curP - buf));
        Q_strncpyz(curP + strlen(curP), pFolk->client->pers.netname, sizeof(buf) - (curP + strlen(curP) - buf));
    } else if (!pFolk) {
        return "nobody";
    }

    return buf;
}

str Player::GetBattleLanguageWeapon() const
{
    return GetCurrentDMWeaponType().c_str();
}

str Player::GetBattleLanguageDistance() const
{
    Vector  vStart, vEnd;
    Vector  vForward;
    trace_t trace;
    float   dist;

    vStart = m_vViewPos;
    AngleVectors(m_vViewAng, vForward, NULL, NULL);

    vEnd = vStart + vForward * 10240;

    trace = G_Trace(
        vStart,
        vec_zero,
        vec_zero,
        vEnd,
        static_cast<const Entity *>(this),
        MASK_BATTLELANGUAGE,
        qfalse,
        "Player::GetBattleLanguageDistance"
    );

    dist = (vStart - trace.endpos).length();

    if (g_qunits_to_feet->integer) {
        return GetBattleLanguageDistanceFeet(dist);
    } else {
        return GetBattleLanguageDistanceMeters(dist);
    }
}

str Player::GetBattleLanguageDistanceMeters(float dist) const
{
    int meters;

    meters = (int)((dist + 26.f) / 52.f);
    if (meters >= 5) {
        if (meters < 21) {
            meters = 5 * ((meters + 2) / 5);
        } else if (meters < 101) {
            meters = 10 * ((meters + 5) / 10);
        } else {
            meters = 25 * ((meters + 12) / 25);
        }
    }

    return va("%d meters", meters);
}

str Player::GetBattleLanguageDistanceFeet(float dist) const
{
    int ft;

    ft = (int)((dist + 26.f) / 52.f);
    if (ft >= 11) {
        if (ft < 51) {
            ft = 5 * ((ft + 2) / 5);
        } else if (ft < 251) {
            ft = 10 * ((ft + 5) / 10);
        } else {
            ft = 25 * ((ft + 12) / 25);
        }
    }

    return va("%d feet", ft);
}

str Player::GetBattleLanguageTarget() const
{
    Vector  vStart, vEnd;
    Vector  vForward;
    trace_t trace;

    vStart = m_vViewPos;
    AngleVectors(m_vViewAng, vForward, NULL, NULL);

    vEnd = vStart + vForward * 10240;

    trace = G_Trace(
        vStart,
        vec_zero,
        vec_zero,
        vEnd,
        static_cast<const Entity *>(this),
        MASK_BATTLELANGUAGE,
        qfalse,
        "Player::GetBattleLanguageDistance"
    );

    if (!trace.ent) {
        return "something";
    }

    if (!trace.ent->entity || trace.ent->entity == world) {
        return "something";
    }

    if (trace.ent->entity->IsSubclassOfPlayer()) {
        Player *pPlayer = static_cast<Player *>(trace.ent->entity);
        return pPlayer->client->pers.netname;
    }

    if (trace.ent->entity->IsSubclassOfSentient()) {
        return "someone";
    }

    return "something";
}

str Player::TranslateBattleLanguageTokens(const char *string)
{
    str token;
    int type;

    if (!g_chat_expansions->integer) {
        return string;
    }

    if (!string) {
        return str();
    }

    if (string[0] != '$') {
        return string;
    }

    type = string[1];
    if (!type || string[2]) {
        return string;
    }

    switch (type) {
    case 'a':
        token = GetBattleLanguageTarget();
        break;
    case 'c':
        token = GetBattleLanguageCondition();
        break;
    case 'd':
        token = GetBattleLanguageDirection();
        break;
    case 'l':
        token = GetBattleLanguageLocation();
        break;
    case 'n':
        token = GetBattleLanguageLocalFolks();
        break;
    case 'r':
        token = GetBattleLanguageDistance();
        break;
    case 'w':
        token = GetBattleLanguageWeapon();
        break;
    default:
        return string;
    }

    return gi.LV_ConvertString(token);
}

void Player::EventIPrint(Event *ev)
{
    str      sString = ev->GetString(1);
    qboolean iBold   = qfalse;

    if (ev->NumArgs() > 1) {
        iBold = ev->GetInteger(2);
    }

    if (iBold) {
        gi.SendServerCommand(
            edict - g_entities, "print \"" HUD_MESSAGE_WHITE "%s\n\"", gi.LV_ConvertString(sString.c_str())
        );
    } else {
        gi.SendServerCommand(
            edict - g_entities, "print \"" HUD_MESSAGE_YELLOW "%s\n\"", gi.LV_ConvertString(sString.c_str())
        );
    }
}

void Player::SetViewangles(Event *ev)
{
    SetViewAngles(ev->GetVector(1));
}

void Player::GetViewangles(Event *ev)
{
    ev->AddVector(GetVAngles());
}

void Player::EventGetUseHeld(Event *ev)
{
    ev->AddInteger((buttons & BUTTON_USE) ? true : false);
}

void Player::EventGetFireHeld(Event *ev)
{
    ev->AddInteger(buttons & (BUTTON_ATTACKLEFT | BUTTON_ATTACKRIGHT) ? qtrue : qfalse);
}

void Player::EventGetPrimaryFireHeld(Event *ev)
{
    ev->AddInteger(buttons & BUTTON_ATTACKLEFT ? true : false);
}

void Player::EventGetSecondaryFireHeld(Event *ev)
{
    ev->AddInteger(buttons & BUTTON_ATTACKRIGHT ? true : false);
}

void Player::EventGetCoopAdsHeld(Event *ev) // HZM coop - aim-down-sights button held
{
    ev->AddInteger(buttons & BUTTON_COOPADS ? true : false);
}

// HZM coop - TAKE COVER [214]: script-side toggle (coop_mod/takecover.scr) requests/releases the
// pose. Validate IMMEDIATELY so the script can read coop_incover right after coop_setcover 1 for
// instant feedback; if nothing coverable is around on the initial engage, clear the request on the
// spot (no grace) so the toggle stays in sync ("No cover here" instead of a latent request).
/*
===============
Player::EventCoopMarkWall

HZM coop bug-953 wall probe v5: "markwall" console command (bindable). Traces 512u
along the view direction and prints the full identity of whatever it hits - species,
brush id (for cmpatch surgery), shader name, surfaceflags, entity - to the console
log AND the player's HUD. Lets the player report a suspect wall from range without
touching it.
===============
*/
/*
===============
Player::EventCoopKillWall

HZM coop bug-953 wall probe v5: "killwall" console command (bindable). Traces the
view direction; if it hits an INVISIBLE wall species (clip/fence/solidnodraw) on the
world, dispatches cm_killbrush on that brush - the wall vanishes live and the id is
persisted to the loose cmpatch file. Refuses visible geometry and entities so a
stray keypress can never hole a real wall.
===============
*/
void Player::EventCoopKillWall(Event *ev)
{
    Vector fwd;
    Vector eye = origin + Vector(0, 0, viewheight);

    AngleVectors(v_angle, fwd, NULL, NULL);
    Vector  to   = eye + fwd * 512;
    int     mask = MASK_PLAYERSOLID & ~CONTENTS_BODY;
    trace_t tr   = G_Trace(eye, vec_zero, vec_zero, to, this, mask, qfalse, "coop_killwall");

    if (tr.fraction >= 1.0f) {
        gi.SendServerCommand(edict - g_entities, "print \"[killwall] nothing within 512u\n\"");
        return;
    }
    if (tr.entityNum != ENTITYNUM_WORLD) {
        gi.SendServerCommand(edict - g_entities, "print \"[killwall] that is an entity, not a map brush\n\"");
        return;
    }

    qboolean invisible = qfalse;
    trace_t  t2 = G_Trace(eye, vec_zero, vec_zero, to, this, mask & ~(CONTENTS_PLAYERCLIP | CONTENTS_FENCE), qfalse, "coop_killwall_B");
    if (t2.fraction > tr.fraction + 0.001f) {
        invisible = qtrue;
    } else if (tr.surfaceFlags & SURF_NODRAW) {
        invisible = qtrue;
    }
    if (!invisible) {
        gi.SendServerCommand(edict - g_entities, "print \"[killwall] that wall is VISIBLE geometry - refusing (use markwall to report it)\n\"");
        return;
    }

    Vector inside   = Vector(tr.endpos) + fwd * 2;
    int    brushNum = gi.PointBrushnum(inside, 0);
    if (brushNum < 0) {
        gi.SendServerCommand(edict - g_entities, "print \"[killwall] non-brush collision (terrain/patch) - logged for engine fix\n\"");
        baseshader_t *bs = (tr.shaderNum >= 0) ? gi.GetShader(tr.shaderNum) : NULL;
        gi.Printf("^~^~^ WALLPROBE KILLWALL-NONBRUSH shader '%s' sf 0x%x at %.0f %.0f %.0f\n",
                  bs ? bs->shader : "?", tr.surfaceFlags, tr.endpos[0], tr.endpos[1], tr.endpos[2]);
        return;
    }

    baseshader_t *bs = (tr.shaderNum >= 0) ? gi.GetShader(tr.shaderNum) : NULL;
    gi.Printf("^~^~^ WALLPROBE KILLWALL BRUSH %d shader '%s' at %.0f %.0f %.0f\n",
              brushNum, bs ? bs->shader : "?", tr.endpos[0], tr.endpos[1], tr.endpos[2]);
    gi.SendConsoleCommand(va("cm_killbrush %d\n", brushNum));
    gi.SendServerCommand(edict - g_entities,
        va("print \"[killwall] brush %d '%s' KILLED - if a visible wall went ghost, run: cm_restorebrush %d\n\"",
           brushNum, bs ? bs->shader : "?", brushNum));
}

void Player::EventCoopMarkWall(Event *ev)
{
    Vector fwd;
    Vector eye = origin + Vector(0, 0, viewheight);

    AngleVectors(v_angle, fwd, NULL, NULL);
    Vector  to = eye + fwd * 512;
    int     mask = MASK_PLAYERSOLID & ~CONTENTS_BODY;
    trace_t tr = G_Trace(eye, vec_zero, vec_zero, to, this, mask, qfalse, "coop_markwall");

    if (tr.fraction >= 1.0f) {
        gi.SendServerCommand(edict - g_entities, "print \"[markwall] nothing within 512u\n\"");
        return;
    }

    const char *kind = "solid";
    trace_t     t2 = G_Trace(eye, vec_zero, vec_zero, to, this, mask & ~CONTENTS_PLAYERCLIP, qfalse, "coop_markwall_B");
    if (t2.fraction > tr.fraction + 0.001f) {
        kind = "clip";
    } else {
        trace_t t3 = G_Trace(eye, vec_zero, vec_zero, to, this, mask & ~CONTENTS_FENCE, qfalse, "coop_markwall_C");
        if (t3.fraction > tr.fraction + 0.001f) {
            kind = "fence";
        } else if (tr.entityNum == ENTITYNUM_WORLD && (tr.surfaceFlags & SURF_NODRAW)) {
            kind = "solidnodraw";
        } else if (tr.entityNum != ENTITYNUM_WORLD) {
            kind = "entity";
        }
    }

    Vector        inside   = Vector(tr.endpos) + fwd * 2;
    int           brushNum = gi.PointBrushnum(inside, 0);
    baseshader_t *bs       = (tr.shaderNum >= 0) ? gi.GetShader(tr.shaderNum) : NULL;

    gi.Printf(
        "^~^~^ WALLPROBE MARK %s BRUSH %d shader '%s' sf 0x%x ent %d at %.0f %.0f %.0f dist %.0f\n",
        kind,
        brushNum,
        bs ? bs->shader : "?",
        tr.surfaceFlags,
        tr.entityNum,
        tr.endpos[0],
        tr.endpos[1],
        tr.endpos[2],
        tr.fraction * 512.0f
    );
    gi.SendServerCommand(
        edict - g_entities,
        va("print \"[markwall] %s brush %d '%s' dist %.0f - logged\n\"",
           kind, brushNum, bs ? bs->shader : "?", tr.fraction * 512.0f)
    );
}

// HZM coop [user 08-02]: script publishes DBNO here (coop_mod/dbno.scr). Read by
// TurretGun::P_TurretUsed and VehicleTurretGun::TurretUsed to refuse mounting while downed -
// a crawling player was previously able to man paks, cannons, flaks and MG42s.
void Player::EventCoopSetDbno(Event *ev)
{
    m_bCoopDbno = ev->GetInteger(1) ? true : false;

    // If they went down while already manning something, evict them.
    if (m_bCoopDbno) {
        RemoveFromVehiclesAndTurrets();
    }
}

void Player::EventCoopSetCover(Event *ev)
{
    m_bCoopCoverRequested = ev->GetInteger(1) ? true : false;
    m_fCoopCoverBadTime   = 0.0f;

    TickCoopCover();
    TickCoopNavRec();

    if (m_bCoopCoverRequested && !m_bCoopCoverWall && !m_bCoopCoverLow) {
        m_bCoopCoverRequested = false;
        m_bCoopBlindfire      = false;
    }
}

// HZM coop - TAKE COVER [214]: state getter for script (property syntax: local.player.coop_incover)
void Player::EventGetCoopCover(Event *ev)
{
    int state = 0;

    if (m_bCoopCoverRequested) {
        state = 1;
        if (m_bCoopCoverWall) {
            state = 2;
        }
        if (m_bCoopCoverLow) {
            state = 3;
        }
    }

    ev->AddInteger(state);
}

void Player::BeginTempSpectator(void)
{
    m_bTempSpectator = true;
    Spectator();
}

void Player::EndSpectator(void)
{
    m_bSpectator     = false;
    m_bTempSpectator = false;

    client->ps.pm_flags &= ~(PMF_SPECTATING | PMF_SPECTATE_FOLLOW);
}

void Player::EventGetReady(Event *ev)
{
    ev->AddInteger(m_bReady);
}

void Player::EventSetReady(Event *ev)
{
    if (m_bReady) {
        return;
    }

    m_bReady = true;
    gi.Printf("%s is ready\n", client->pers.netname);
}

void Player::EventSetNotReady(Event *ev)
{
    if (!m_bReady) {
        return;
    }

    m_bReady = false;
    gi.Printf("%s is not ready\n", client->pers.netname);
}

void Player::EventGetDMTeam(Event *ev)
{
    if (dm_team == TEAM_FREEFORALL) {
        ev->AddConstString(STRING_FREEFORALL);
    } else if (dm_team == TEAM_AXIS) {
        ev->AddConstString(STRING_AXIS);
    } else if (dm_team == TEAM_ALLIES) {
        ev->AddConstString(STRING_ALLIES);
    } else if (dm_team == TEAM_SPECTATOR) {
        ev->AddConstString(STRING_SPECTATOR);
    } else {
        ScriptError("dmteam is invalid in single player");
    }
}

void Player::EventGetNetName(Event *ev)
{
    ev->AddString(client->pers.netname);
}

void Player::EventSetViewModelAnim(Event *ev)
{
    str      anim;
    int      force_restart = 0;
    qboolean bfullanim     = 0;

    anim = ev->GetString(1);

    if (ev->NumArgs() > 1) {
        force_restart = ev->GetInteger(2);
    }

    if (ev->NumArgs() > 2) {
        bfullanim = ev->GetInteger(3);
    }

    ViewModelAnim(anim, force_restart, bfullanim);
}

void Player::FullHeal(Event *ev)
{
    if (IsDead()) {
        if (!ev->IsFromScript()) {
            HUDPrint("TESTING:  Cannot resurrect yourself with the fullheal.\n");
        }
    } else {
        if (!ev->IsFromScript()) {
            HUDPrint("TESTING:  You used the fullheal.\n");
        }

        health = max_health;
    }
}

void Player::RemoveFromVehiclesAndTurretsInternal(void)
{
    if (m_pVehicle) {
        Event *event;

        m_pVehicle->flags &= ~FL_GODMODE;

        event = new Event(EV_Use);
        event->AddEntity(this);
        m_pVehicle->ProcessEvent(event);
    } else if (m_pTurret) {
        m_pTurret->TurretUsed(this);
    }
}

void Player::RemoveFromVehiclesAndTurrets(void)
{
    Weapon *activeWeap = GetActiveWeapon(WEAPON_MAIN);
    if (activeWeap && activeWeap->IsCarryableTurret()) {
        CarryableTurret *pTurret = static_cast<CarryableTurret *>(activeWeap);
        pTurret->DropTurret(NULL);
    }

    if (!m_pVehicle && !m_pTurret) {
        return;
    }

    if (m_pVehicle && m_pVehicle->isLocked()) {
        m_pVehicle->UnLock();

        if (m_pTurret && m_pTurret->IsSubclassOfVehicleTurretGun()) {
            VehicleTurretGun *turret = (VehicleTurretGun *)m_pTurret.Pointer();

            if (turret->isLocked()) {
                turret->UnLock();
                RemoveFromVehiclesAndTurretsInternal();
                turret->Lock();
            } else {
                RemoveFromVehiclesAndTurretsInternal();
            }
        } else {
            RemoveFromVehiclesAndTurretsInternal();
        }

        // the vehicle might have been modified
        if (m_pVehicle) {
            m_pVehicle->Lock();
        }
    } else if (m_pTurret && m_pTurret->IsSubclassOfVehicleTurretGun()) {
        VehicleTurretGun *turret = (VehicleTurretGun *)m_pTurret.Pointer();

        if (turret->isLocked()) {
            turret->UnLock();
            RemoveFromVehiclesAndTurretsInternal();

            // the turret might have been modified
            if (m_pTurret) {
                turret->Lock();
            }
        } else {
            RemoveFromVehiclesAndTurretsInternal();
        }
    } else {
        RemoveFromVehiclesAndTurretsInternal();
    }
}

void Player::EventEnterIntermission(Event *ev)
{
    if (!level.intermissiontime) {
        return;
    }

    if (level.intermissiontype) {
        G_DisplayScores(this);

        if (level.intermissiontype == TRANS_MISSION_FAILED || IsDead()) {
            gi.cvar_set("g_success", "0");
            gi.cvar_set("g_failed", "1");
        } else {
            gi.cvar_set("g_success", "1");
            gi.cvar_set("g_failed", "0");
        }
    } else {
        G_HideScores(this);
    }
}

bool Player::BlocksAIMovement()
{
    return false;
}

void Player::EventSetPerferredWeapon(Event *ev)
{
    m_sPerferredWeaponOverride = ev->GetString(1);
}

void Player::SetMouthAngle(Event *ev)
{
    int    tag_num;
    float  angle_percent;
    Vector mouth_angles;

    angle_percent = ev->GetFloat(1);

    if (angle_percent < 0) {
        angle_percent = 0;
    }

    if (angle_percent > 1) {
        angle_percent = 1;
    }

    tag_num = gi.Tag_NumForName(edict->tiki, "tag_mouth");

    if (tag_num != -1) {
        SetControllerTag(MOUTH_TAG, tag_num);

        mouth_angles        = vec_zero;
        mouth_angles[PITCH] = max_mouth_angle * angle_percent;

        SetControllerAngles(MOUTH_TAG, mouth_angles);
    }
}

int Player::GetMoveResult(void)
{
    return moveresult;
}

qboolean Player::CheckCanSwitchTeam(teamtype_t team)
{
    float startTime;

    startTime = dmManager.GetMatchStartTime();

    if (startTime >= 0.0f && (level.time - startTime) > 30.0
        && (level.time - m_fTeamSelectTime) < g_teamswitchdelay->integer) {
        int seconds = g_teamswitchdelay->integer - (level.time - m_fTeamSelectTime);

        gi.SendServerCommand(
            edict - g_entities,
            "print \"" HUD_MESSAGE_WHITE "%s %i %s\n\"",
            gi.LV_ConvertString("Can not change teams again for another"),
            seconds + 1,
            gi.LV_ConvertString("seconds")
        );
        return qfalse;
    }

    // Added in OPM
    //  Check and prevent joining the team with the highest number of players
    if (g_teambalance->integer && g_gametype->integer >= GT_TEAM && !dmManager.WaitingForPlayers()) {
        DM_Team *pNewTeam = dmManager.GetTeam(team);
        int      i;

        for (i = 0; i < 2; i++) {
            DM_Team *pTeam          = dmManager.GetTeam((teamtype_t)(TEAM_ALLIES + i));
            int      numTeamPlayers = pTeam->m_players.NumObjects();

            if (pTeam->m_players.IndexOfObject(this)) {
                // Don't count the current player
                numTeamPlayers--;
            }

            if (pNewTeam->m_players.NumObjects() > numTeamPlayers) {
                const char *message = gi.LV_ConvertString(
                    "That team has enough players. Choose the team that has the lowest number of players."
                );

                gi.SendServerCommand(
                    edict - g_entities, "print \"" HUD_MESSAGE_WHITE "%s\n\"", gi.LV_ConvertString(message)
                );

                gi.centerprintf(edict, message);
                return qfalse;
            }
        }
    }

    return qtrue;
}

qboolean Player::ViewModelAnim(str anim, qboolean force_restart, qboolean bFullAnim)
{
    Unregister(STRING_VIEWMODELANIM_DONE);

    if (client == NULL) {
        return true;
    }

    int            viewModelAnim;
    playerState_t *playerState = &client->ps;
    Weapon        *weapon;

    if (!anim.length()) {
        anim = "";
    }

    // Copy the item prefix and the anim name
    weapon = GetActiveWeapon(WEAPON_MAIN);

    if (!Q_stricmp(anim, "charge")) {
        viewModelAnim = VM_ANIM_CHARGE;
    } else if (!Q_stricmp(anim, "fire")) {
        viewModelAnim = VM_ANIM_FIRE;
    } else if (!Q_stricmp(anim, "fire_secondary")) {
        viewModelAnim = VM_ANIM_FIRE_SECONDARY;
    } else if (!Q_stricmp(anim, "rechamber")) {
        viewModelAnim = VM_ANIM_RECHAMBER;
    } else if (!Q_stricmp(anim, "reload")) {
        viewModelAnim = VM_ANIM_RELOAD;
    } else if (!Q_stricmp(anim, "reload_single")) {
        viewModelAnim = VM_ANIM_RELOAD_SINGLE;
    } else if (!Q_stricmp(anim, "reload_end")) {
        viewModelAnim = VM_ANIM_RELOAD_END;
    } else if (!Q_stricmp(anim, "pullout")) {
        viewModelAnim = VM_ANIM_PULLOUT;
    } else if (!Q_stricmp(anim, "putaway")) {
        viewModelAnim = VM_ANIM_PUTAWAY;
    } else if (!Q_stricmp(anim, "ladderstep")) {
        viewModelAnim = VM_ANIM_LADDERSTEP;
    } else {
        if (!Q_stricmp(anim, "idle")) {
            viewModelAnim = VM_ANIM_IDLE;
        } else if (!Q_stricmp(anim, "idle0")) {
            viewModelAnim = VM_ANIM_IDLE_0;
        } else if (!Q_stricmp(anim, "idle1")) {
            viewModelAnim = VM_ANIM_IDLE_1;
        } else if (!Q_stricmp(anim, "idle2")) {
            viewModelAnim = VM_ANIM_IDLE_2;
        } else {
            // Defaults to idle
            viewModelAnim = VM_ANIM_IDLE;
        }

        //
        // check the fire movement speed if the weapon has a max fire movement
        //
        if (weapon && weapon->m_fMaxFireMovement < 1) {
            float length;

            length = velocity.length();
            if (length / sv_runspeed->value > ((weapon->m_fMaxFireMovement * weapon->m_fMovementSpeed) + 0.1f)) {
                // Set the view model animation to disabled
                viewModelAnim = VM_ANIM_DISABLED;
            }
        }
    }

    if (viewModelAnim != playerState->iViewModelAnim || force_restart) {
        playerState->iViewModelAnimChanged = (playerState->iViewModelAnimChanged + 1) & 3;
    }

    playerState->iViewModelAnim = viewModelAnim;

    if (!weapon) {
        weapon = newActiveWeapon.weapon;
    }

    if (weapon) {
        m_sVMAcurrent = GetItemPrefix(weapon->getName()) + str("_") + anim;
    } else {
        m_sVMAcurrent = "unarmed_" + anim;
    }

    m_sVMcurrent = anim;

    if (m_fpsTiki && gi.Anim_NumForName(m_fpsTiki, m_sVMAcurrent) < 0) {
        //gi.DPrintf("WARNING: Invalid view model anim \"%s\"\n", m_sVMAcurrent.c_str());
    }

    animDoneVM = false;

    m_fVMAtime = 0;

    return true;
}

void Player::FindAlias(str& output, str name, AliasListNode_t **node)
{
    const char *alias = gi.Alias_FindRandom(edict->tiki, name, node);

    if (alias == NULL) {
        alias = gi.GlobalAlias_FindRandom(name, node);
    }

    if (alias != NULL) {
        output = alias;
    }
}

bool Player::HasVotedYes() const
{
    return voted;
}

bool Player::HasVotedNo() const
{
    return !voted;
}

//====
// HZM coop [user 2026-08-02] - LOW-HEALTH LIMP (bug-1291).
// "when the player gets really low health they should start playing the same limp animation the
// actors do, and you should see the limp in first person".
//
// SINGLE AUTHORITY. This runs on the server and decides the whole feature: the flag drives the
// COOP_LIMPING statemap conditional (3P body) AND the ClientMove speed clamp, and the same decision
// is stuffed to the owning client as coop_limpView for the first-person camera. The client never
// re-derives a threshold, so `coop_limp 0` on the server genuinely disables it everywhere - not just
// the body, which is what a client-side threshold read would have given.
//
// HEALTH SIGNAL. health / max_health, clamped. Deliberately NOT a "peak health this life" tracker:
// that pattern was proposed and rejected because Entity::EventSetHealthOnly CLAMPS to max_health
// (entity.cpp), so DBNO's `healthonly 9999` can never inflate a peak in the first place, and a peak
// tracker would instead mis-read a legitimately weakened player. max_health is the honest divisor
// and is what stats[STAT_HEALTH] is already normalised against at player.cpp:8113 - so the server
// and the client are reading the SAME quantity and cannot disagree about when the limp starts.
//====
void Player::TickLimp()
{
    cvar_t  *pOn    = gi.Cvar_Get("coop_limp", "1", CVAR_ARCHIVE);
    cvar_t  *pStart = gi.Cvar_Get("coop_limpStart", "0.30", CVAR_ARCHIVE);
    float    start  = pStart ? pStart->value : 0.30f;
    float    frac   = 1.0f;
    qboolean enabled = (pOn && pOn->integer) ? qtrue : qfalse;
    int      want;

    if (start < 0.0f) { start = 0.0f; } else if (start > 1.0f) { start = 1.0f; }

    if (max_health > 0.0f) {
        frac = health / max_health;
        if (frac < 0.0f) { frac = 0.0f; } else if (frac > 1.0f) { frac = 1.0f; }
    }

    // A limp is a LOCOMOTION state, so every pose that owns locomotion outright suppresses it:
    // dead, downed (DBNO has its own crawl + haze), on a turret or in a vehicle (the body is not
    // driving movement at all), and while airborne.
    // [user 2026-08-03] bug-1324 - the sprint gate must NOT include the ground term. Limp state
    // (statemap + speed clamp + FP camera) requires groundentity, but every airborne frame (jump,
    // stair edge, slope bounce, knockback) cleared m_bCoopLimping - and with Shift+W still held,
    // TickSprint saw "not limping" and fired a genuine sprint burst mid-limp: gear-rattle loop,
    // full-volume run footsteps from SPRINT_FORWARD, stamina drain, then the out-of-breath pant on
    // landing. Split the decision: m_bCoopWounded = the pure health test, m_bCoopLimping = wounded
    // AND grounded (unchanged consumers).
    m_bCoopWounded = false;
    if (enabled && !deadflag && !m_bCoopDbno && !m_pVehicle && !m_pTurret && frac < start) {
        m_bCoopWounded = true;
    }
    m_bCoopLimping = (m_bCoopWounded && groundentity) ? true : false;

    // Tell the OWNING client only, and only when it CHANGES - a per-frame stuff would flood the
    // reliable command buffer (the same rule dbno.scr follows for coop_dbnoView).
    want = m_bCoopLimping ? 1 : 0;
    if (want != m_iCoopLimpSent) {
        m_iCoopLimpSent = want;
        gi.SendServerCommand(edict - g_entities, "stufftext \"set coop_limpView %d\"", want);
    }
}

//====
// HZM coop [user 2026-08-02] - DEV: jump straight to a chosen health fraction so the low-health limp
// (and the injury vignette, and anything else health-gated) can be tested without first being shot to
// pieces. Sets health directly rather than applying damage, so there is no pain animation, no DBNO
// trigger and no attacker bookkeeping - a test harness, not a simulated hit.
//====
void Player::EventCoopLimpTest(Event *ev)
{
    float frac = (ev->NumArgs() > 0) ? ev->GetFloat(1) : 0.25f;
    float target;

    if (frac < 0.0f) { frac = 0.0f; } else if (frac > 1.0f) { frac = 1.0f; }
    if (deadflag || max_health <= 0.0f) {
        gi.Printf("coop_limptest: not while dead\n");
        return;
    }

    target = max_health * frac;
    if (target < 1.0f) { target = 1.0f; } // never kill via the test command

    health = target;
    gi.Printf("coop_limptest: health %.0f / %.0f (%.0f%%) - limp starts below %s\n",
              health, max_health, (health / max_health) * 100.0f,
              gi.Cvar_Get("coop_limpStart", "0.30", CVAR_ARCHIVE)->string);
}

// HZM coop [user 2026-08-24] PRONE - hold the crouch key.
//
// DIVISION OF LABOUR, and it is not arbitrary. In MOHAA the STATEMAP owns stance: `height prone`
// (player_conditionals.cpp:1798 -> maxs.z 20) and `moveposflags prone` are statemap commands, and
// the legs/torso .st files choose the animation. So the engine's job here is only to DECIDE and to
// GUARD; it publishes COOP_PRONE and the .st does the rest. Trying to drive height or pose from C++
// would fight the state system rather than use it.
//
// THE STAND-UP GUARD IS THE WHOLE RISK. A prone hull is short and long (maxs.z 20 vs 94), so you can
// crawl into somewhere you cannot stand - under a truck, a low pipe, a collapsed beam. Every game
// with prone has shipped this bug at least once. Leaving prone is therefore CONDITIONAL on a real
// trace for standing clearance, and a blocked player simply stays prone rather than being teleported
// or wedged inside geometry.
//
// The hold is deliberate rather than a tap: tap-crouch is already the crouch toggle and thousands of
// existing muscle-memory inputs use it. coop_proneHold is the dwell in seconds.
void Player::TickCoopProne()
{
    static cvar_t *pOn = NULL, *pHold = NULL;
    qboolean       bCrouchKey, bCanEnter, bMustLeave;
    float          dt = level.frametime;

    if (!pOn)   { pOn   = gi.Cvar_Get("coop_prone", "1", CVAR_ARCHIVE); } // statemap (player_legs.st PRONE_*) and anims (anims_shared.txt coop_prone_*) landed 2026-08-24, all three boot-verified
    // [user 2026-08-27] 0.35s sat inside the range of an ordinary crouch tap, which is half of why
    // a gentle press could drop you prone. A deliberate hold should feel deliberate.
    if (!pHold) { pHold = gi.Cvar_Get("coop_proneHold", "0.5", CVAR_ARCHIVE); }

    // [user 2026-08-24] ENTRY and EXIT conditions are NOT the same set, and conflating them was a bug:
    // "I drop into prone and then quickly back into crouch automatically".
    //
    // groundentity belongs ONLY to entry. Going prone shrinks the hull from 94 to 20, and on the frame
    // that happens the ground trace can miss, so groundentity is momentarily NULL. Testing it every
    // frame meant the state cleared itself one tick after it was set - the drop-in was real, and so was
    // the instant pop back to crouch.
    //
    // What genuinely forces a prone player upright is a different, smaller list: dying, boarding a
    // vehicle or turret, spectating, or being frozen. Losing footing for a frame is not on it.
    // [spec A1, bug-2123] m_bCoopDbno was MISSING here: going down WHILE prone kept m_bCoopProne,
    // PMF_VIEW_PRONE re-derived from the 20u hull, viewheight fell to 16, and the DBNO camera's
    // -30 vertical offset went under the map again - bug-2112's exact failure, returned by a new
    // path. Supine made it worse: the yaw ease kept driving a downed player's crawl backwards.
    // [pass4, bug-2127] COVER was missing: holding crouch 0.35s while in wall/low cover
    // latched m_bCoopProne on a STANDING body (the cover legs states have no COOP_PRONE
    // route) - dead trigger above 30 u/s, frozen WASD once supine latched, flipped anims
    // on an upright body. Cover can no longer engage while prone either (TickCoopCover),
    // so these terms are the belt-and-braces direction. Ladder and swim close the spec's
    // two known bMustLeave gaps while we are here.
    bMustLeave = (qboolean)(deadflag || m_bCoopDbno || m_pVehicle || m_pTurret
                            || m_bCoopCoverRequested || m_bCoopCoverWall || m_bCoopCoverLow
                            || m_pLadder || waterlevel > 1
                            || (client->ps.pm_flags & (PMF_SPECTATING | PMF_INTERMISSION
                                                       | PMF_FROZEN | PMF_NO_MOVE)));
    bCanEnter  = (qboolean)(!bMustLeave && groundentity && !m_bCoopSliding);

    if (!pOn->integer || bMustLeave) {
        // gated - this used to print unconditionally, i.e. on every death and every vehicle mount, for
        // every player, on a shipped build. The user's standing rule is no dev prints to players.
        if (m_bCoopProne && gi.Cvar_Get("coop_proneDebug", "0", 0)->integer) {
            gi.Printf("^~^~^ PRONE-EXIT engine: on=%d dead=%d veh=%d tur=%d pmflags=0x%x\n",
                      pOn->integer, (int)(deadflag != 0), m_pVehicle ? 1 : 0, m_pTurret ? 1 : 0,
                      client->ps.pm_flags);
        }
        // never strand the flag - a player who dies or boards a vehicle prone must not stay prone
        // ([review F3+F4] the died-supine latch that lived here moved to Player::Killed - this
        // block is usercmd-driven and nothing here could ever clear the latch while alive.)
        m_bCoopSupine     = false;
        m_fCoopSupineFlip = 0;
        m_fCoopProneExitAt = 0; // [review F2] a death/DBNO/vehicle inside the deferred-exit
                                // window must not force-stand the NEXT prone session
        m_bCoopProne      = false;
        m_bCoopProneWant  = false;
        m_fCoopCrouchHeld = 0;
        m_fCoopCrouchUp   = 0; // never let the arming latch survive a death or a vehicle
        return;
    }

    bCrouchKey = (last_ucmd.upmove < 0) ? qtrue : qfalse;

    // [user 2026-08-26] EVASIVE ROLLS - lean keys while prone. Edge-triggered on the lean BUTTONS
    // (bits ship in every usercmd; leaning itself is meaningless while prone, so the keys are free).
    // The statemap plays the 0.9s roll anim via COOP_PRONE_ROLLL/R while this window is open, and
    // the impulse below actually displaces the body - without it the roll animates in place, because
    // player legs anims never drive origin (pmove owns it).
    // [spec A6, closes H10/Q5] while ON YOUR BACK: no move input (the supine pose has no locomotion
    // - WASD slid a frozen statue at ~84 u/s and tripped the fire strip), and no lean-rolls (they
    // fired the 170 u/s impulse with no animation routed). Rolling back to your front IS the
    // mobility. coop_supineMove 1 restores the slide for A/B.
    // [pass2] ...and during the flip WINDOWS: supine clears at the start of a flip-out, which
    // un-froze WASD for the entire roll back onto the front (the ~84 u/s statue-slide again).
    if ((m_bCoopSupine || level.time < m_fCoopSupineFlip) && current_ucmd) {
        static cvar_t *pSupMove = NULL;
        if (!pSupMove) { pSupMove = gi.Cvar_Get("coop_supineMove", "0", CVAR_ARCHIVE); }
        if (!pSupMove->integer) {
            current_ucmd->forwardmove = 0;
            current_ucmd->rightmove   = 0;
        }
    }

    if (m_bCoopProne && !m_bCoopSupine) {
        int iLean = last_ucmd.buttons & (BUTTON_LEAN_LEFT | BUTTON_LEAN_RIGHT);
        int iEdge = iLean & ~m_iCoopProneLeanPrev;
        m_iCoopProneLeanPrev = iLean;
        // [pass2, bug-2125] same yield set the supine enter-latch got: no evasive rolls during a
        // flip window or an armed exit - supine clears at the START of a flip-out, so a lean press
        // mid-roll fired the 170 u/s impulse and armed a COMPETING legs condition against the
        // still-running flip (winner = statemap row order, the bug-1291 class).
        if (iEdge && level.time >= m_fCoopProneRollEnd
            && level.time >= m_fCoopSupineFlip && m_fCoopProneExitAt <= 0.0f) {
            static cvar_t *pRollOn = NULL, *pRollImp = NULL;
            if (!pRollOn)  { pRollOn  = gi.Cvar_Get("coop_proneRoll", "1", CVAR_ARCHIVE); }
            if (!pRollImp) { pRollImp = gi.Cvar_Get("coop_proneRollImpulse", "170", CVAR_ARCHIVE); }
            if (pRollOn->integer) {
                vec3_t vF, vR;
                m_iCoopProneRollDir = (iEdge & BUTTON_LEAN_LEFT) ? 1 : -1;
                m_fCoopProneRollEnd = level.time + 0.9f; // = the roll animation's real length (9f)
                AngleVectors(client->ps.viewangles, vF, vR, NULL);
                velocity[0] += vR[0] * pRollImp->value * (float)-m_iCoopProneRollDir;
                velocity[1] += vR[1] * pRollImp->value * (float)-m_iCoopProneRollDir;
            }
        }
        if (level.time >= m_fCoopProneRollEnd) { m_iCoopProneRollDir = 0; }
    } else {
        m_fCoopProneRollEnd = 0; m_iCoopProneRollDir = 0; m_iCoopProneLeanPrev = 0;
    }

    // PROBE PLACEMENT: this MUST sit above the m_bCoopProne early-return below. It used to sit at
    // the bottom of the function, which meant the prone branch returned before ever reaching it and
    // the probe could only ever print prone=0 - blind to the exact state it was meant to measure
    // (TRAPS T14, and the second time this shape has cost a round trip).

    // PROBE (coop_proneDebug 1). Prone did not engage on the first live test and the input source is
    // provably right - CondCrouch, the engine's own crouch condition, reads the identical
    // `last_ucmd.upmove < 0`. So print the deciding inputs rather than reason about them again.
    {
        static cvar_t *pDbg = NULL;
        static int     s_last = 0;
        if (!pDbg) { pDbg = gi.Cvar_Get("coop_proneDebug", "0", 0); } // ships OFF - set 1 to diagnose
        if (pDbg->integer && level.inttime - s_last > 250) {
            s_last = level.inttime;
            gi.Printf("^~^~^ PRONE up=%d held=%.2f canbe=%d prone=%d ground=%d slide=%d posflags=%d\n",
                      (int)last_ucmd.upmove,
                      m_fCoopCrouchHeld,
                      (int)bCanEnter, (int)m_bCoopProne,
                      groundentity ? 1 : 0, (int)m_bCoopSliding, m_iMovePosFlags);
        }
    }


    if (m_bCoopProne) {
        // EXIT IS EDGE-TRIGGERED, and this is the third shape this has taken - the reasoning matters.
        //
        // It used to leave prone whenever the crouch key was simply NOT held. That silently demanded the
        // player hold crouch forever to stay down, which is not how anyone plays: you press to go prone
        // and let go. The bug was hidden for a while because the standup clearance trace was refusing
        // every frame (785 probe samples of up=0 with prone=1 - the player could not get up at all).
        // Adding an escape valve for that then exposed the real design: "NOW IT SEEMS LIKE EVERYTIME I
        // TRY TO PRONE ANYWHERE I GET PUT BACK INTO CROUCH" - the valve was force-standing them one
        // second after they released the key they had just used to go down.
        //
        // So: releasing crouch ARMS the exit, it does not perform it. Getting up needs a deliberate new
        // input - another crouch press, or jump. m_fCoopCrouchUp holds the arming state, which is exactly
        // what its declaration always said it was for.
        qboolean bJump   = (last_ucmd.upmove > 0) ? qtrue : qfalse;
        qboolean bWantUp;

        if (!bCrouchKey) {
            if (!m_fCoopCrouchUp) {
                m_fCoopCrouchUp = level.time; // released - a later press now means 'get up'
            }
        }
        bWantUp = (qboolean)((m_fCoopCrouchUp && bCrouchKey) || bJump);

        // [spec A5, closes H6] leaving prone from ON YOUR BACK used to clear m_bCoopProne at once;
        // CoopProneBodyYaw's reset branch then snapped the eased body yaw to the view - a 180-degree
        // teleport into the crouch rise. A crouch-press now rolls you onto your front FIRST (the
        // same flip-out arming the aim exit uses) and the stand is deferred until the roll ends.
        // Jump exits stay immediate per the spec - a panic exit may cut the roll.
        if (bWantUp && !bJump && m_bCoopSupine) {
            m_bCoopSupine        = false;
            m_fCoopSupineFlip    = level.time + ((m_iCoopSupineFlipDir >= 0) ? 0.9f : 1.0f);
            m_iCoopSupineFlipDir = (m_iCoopSupineFlipDir >= 0) ? 1 : -1;
            m_fCoopProneExitAt   = m_fCoopSupineFlip;
        } else if (bWantUp && !bJump && level.time < m_fCoopSupineFlip && m_fCoopProneExitAt <= 0.0f) {
            // [pass2, bug-2125] release-ADS first, press crouch a beat later - the natural way to
            // get up - had already cleared m_bCoopSupine, so the press bypassed the defer and
            // teleported the body up to ~180 degrees mid-roll. Reuse the RUNNING flip-out window
            // (never re-arm a fresh one); belly-prone sessions have flip==0, so their immediate
            // exit is untouched.
            m_fCoopProneExitAt = m_fCoopSupineFlip;
        }
        // [pass3, bug-2126] 'Jump exits stay immediate' held only until a crouch-press armed
        // the defer - after that the early-return below swallowed the panic jump for up to
        // 1.0s. A jump cancels the defer and falls through to the immediate standup trace.
        if (bJump && m_fCoopProneExitAt > 0.0f) {
            m_fCoopProneExitAt = 0;
        }
        if (m_fCoopProneExitAt > 0.0f) {
            if (level.time < m_fCoopProneExitAt) {
                return; // rolling onto the front; the stand comes when the roll ends
            }
            m_fCoopProneExitAt = 0;
            bWantUp = qtrue; // the deferred press is honoured even if the key came up meanwhile
            m_bCoopProneKeyBlock = true;
        }

        if (!bWantUp) {
            return; // lying down, no request to rise
        }

        {
            trace_t tr;
            Vector  vUpMaxs = maxs;

            vUpMaxs[2] = 60.0f; // crouch height - the cheapest stance that is not prone
            tr = G_Trace(origin, mins, vUpMaxs, origin, this, MASK_PLAYERSOLID, false,
                         "Player::TickCoopProne standup");
            {
                static cvar_t *pSDbg = NULL;
                static int     s_sLast = 0;
                if (!pSDbg) { pSDbg = gi.Cvar_Get("coop_proneDebug", "0", 0); }
                if (pSDbg->integer && level.inttime - s_sLast > 400) {
                    s_sLast = level.inttime;
                    gi.Printf("^~^~^ STANDUP start=%d all=%d frac=%.2f | mins=%.0f/%.0f/%.0f maxs=%.0f/%.0f/%.0f org=%.0f/%.0f/%.0f\n",
                              (int)tr.startsolid, (int)tr.allsolid, tr.fraction,
                              mins[0], mins[1], mins[2], vUpMaxs[0], vUpMaxs[1], vUpMaxs[2],
                              origin[0], origin[1], origin[2]);
                }
            }

            // Blocked means genuinely no headroom. The player ASKED to get up, so refusing is correct -
            // but it must never be permanent, and it is no longer time-based: they can simply press
            // again, and each press re-runs the honest trace.
            if (tr.startsolid || tr.allsolid) {
                return;
            }
        }

        // [vet] drop to the CROUCH hull on the same statement as the state clear. The flag
        // derivation needs both maxs.z == 20 and m_bCoopProne, so clearing one without the other
        // matched neither case and every stand-up ran a full server frame at standing height - a
        // 94u hull appearing for one frame wherever the player was lying.
        maxs.z            = 54.0f;
        m_bCoopProne      = false;
        m_bCoopProneWant  = false;
        m_fCoopCrouchUp   = 0;
        m_fCoopCrouchHeld = 0; // do not let the entry accumulator re-trigger prone on this same press
        m_bCoopProneKeyBlock = true; // and not on this held key either
        return;
    }

    // [user 2026-08-27] "sometimes if I gently touch control to crouch ill go into prone instead".
    //
    // The accumulator DECAYED on release instead of resetting, so a run of short taps banked partial
    // credit - 0.2s down, a quick release giving back only 0.1, 0.2s down again - and crossed the
    // threshold without any press ever being a deliberate hold. A completed short press means CROUCH.
    // It is a discrete decision, so it resets the dwell rather than half-remembering it.
    //
    // The block latch is the other half: standing up from prone left the key still down, the
    // accumulator started building again immediately, and you dropped straight back onto your face -
    // the "and then get stuck" half of the report. After any deliberate exit the key must be
    // released before it can arm prone again.
    // [vet] ELAPSED TIME, not accumulated frametime. ClientThink runs once per USERCMD, not once per
    // server frame, so this added a whole server frametime several times per frame: with the shipped
    // com_maxfps 180 against sv_fps 40 that is ~4.5 additions per frame, and the advertised 0.5s hold
    // actually fired after ~0.11s - inside the duration of an ordinary crouch tap. Which is to say
    // the 'gentle touch puts me prone' report was only half fixed: resetting the accumulator stopped
    // taps ACCUMULATING, but each individual tap was still being credited four times over.
    if (bCrouchKey) {
        if (!m_bCoopProneKeyBlock && m_fCoopCrouchHeld == 0.0f) {
            m_fCoopCrouchHeld = level.time;   // the moment the press began
        }
    } else {
        m_fCoopCrouchHeld    = 0.0f;
        m_bCoopProneWant     = false;
        m_bCoopProneKeyBlock = false; // key is up: prone may arm again
        return;
    }
    // [spec A9] a leftover from the first elapsed-time design assigned level.time into what is
    // now a SECONDS accumulator - on a zero-length frame that poisoned the hold with a huge
    // timestamp and entered prone instantly. The accumulator needs no seeding; deleted.
    if (bCanEnter && m_fCoopCrouchHeld != 0.0f && (level.time - m_fCoopCrouchHeld) >= pHold->value) {
        // MUST clear the arming latch. Entry happens with crouch still DOWN, so a latch left set by a
        // previous prone session would satisfy (m_fCoopCrouchUp && bCrouchKey) on the very next frame
        // and stand the player straight back up - the same symptom that brought this rewrite about.
        m_fCoopCrouchUp   = 0;
        m_fCoopProneExitAt = 0; // [review F2] belt-and-braces: entry never inherits a stale defer
        m_bCoopProne      = true;
        m_bCoopProneWant  = true;
        m_fCoopProneEnter = level.time;
    }
}

// HZM coop [user 2026-08-25] SERVER-SIDE STRESS.
//
// WHY THIS EXISTS RATHER THAN REUSING THE CLIENT ONE. cg_view.c already computes exactly this scalar,
// and its own comment forbids this use: 'STAMINA is a client-side re-simulation of the server's pool
// and is known to diverge ... do not reuse the stamina term for anything a 3P player sees.' The whole
// point of the change is that stress now decides where rounds go, so it must be computed from state
// the SERVER owns. Every term below is authoritative here.
//
// The weights, the ducked discount and the ease rates are copied from CG_FeelStressAdvance deliberately
// - two stress numbers that disagree would be worse than one that is slightly wrong, and the user tunes
// the FEEL through coop_stressSpread rather than by drifting the two formulas apart.
//
// SUPPRESSION is the headline term (weight 0.45) and the only one the client had that the server did
// not. It is fed by BulletAttack the same way player fire already suppresses AI (coop_aiSuppress,
// weaputils.cpp) - one trace, one radius scan, per fire event.
void Player::TickCoopStress()
{
    // [vet] same per-usercmd trap as the recoil recovery and the prone dwell: this runs once per
    // COMMAND, so a 125fps client advanced the envelope roughly six times faster than a 20fps one
    // on the same server - and stress multiplies bullet spread directly, so framerate was quietly
    // buying accuracy. dt is elapsed level time, clamped so a hitch cannot dump the whole envelope.
    float dtReal = level.time - m_fCoopStressLast;
    m_fCoopStressLast = level.time;
    if (dtReal <= 0.0f) { return; }
    if (dtReal > 0.25f) { dtReal = 0.25f; }
    static cvar_t *pOn = NULL, *pSuppFade = NULL, *pStamina = NULL, *pRun = NULL, *pDbg = NULL;
    static int     s_last = 0;
    float          raw = 0.0f, hp = 1.0f, stam = 1.0f, spd = 0.0f, dt = dtReal, rate;

    if (!pOn)       { pOn       = gi.Cvar_Get("coop_stress", "1", CVAR_ARCHIVE); }
    if (!pSuppFade) { pSuppFade = gi.Cvar_Get("coop_stressSuppFade", "2.5", CVAR_ARCHIVE); }
    if (!pStamina)  { pStamina  = gi.Cvar_Get("coop_sprintStamina", "5", CVAR_ARCHIVE); }
    if (!pRun)      { pRun      = gi.Cvar_Get("sv_runspeed", "287", 0); }
    if (!pDbg)      { pDbg      = gi.Cvar_Get("coop_stressDebug", "0", 0); }

    if (dt <= 0.0f) { return; }
    if (dt > 0.1f)  { dt = 0.1f; } // a hitch must not teleport the envelope

    // coop_stressDebug 2 injects suppression on a standing player, so the envelope -> stress -> spread
    // chain can be exercised without needing an enemy to actually shoot at you. Separates "the maths is
    // wrong" from "nothing ever called CoopAddSuppression" - two failures that look identical in play.
    if (pDbg->integer >= 2) { CoopAddSuppression(dt * 1.5f); }

    // suppression decays on its own clock, exactly like the client FX it mirrors
    if (m_fCoopStressSupp > 0.0f) {
        float fade = (pSuppFade->value > 0.1f) ? pSuppFade->value : 2.5f;
        m_fCoopStressSupp -= dt / fade;
        if (m_fCoopStressSupp < 0.0f) { m_fCoopStressSupp = 0.0f; }
    }

    if (pOn->integer && !deadflag && !m_pVehicle && !m_pTurret
        && !(client->ps.pm_flags & (PMF_SPECTATING | PMF_INTERMISSION | PMF_FROZEN))) {
        // HEALTH: real hit points, not STAT_HEALTH - that is a 0..100 percentage and a vehicle hijacks
        // it (the client hit exactly this trap and routed around it via r_ppHealthFrac). A DOWNED
        // player reads as maximally stressed rather than as healthy, because dbno.scr restores health.
        if (IsCoopDbno()) {
            hp = 0.02f;
        } else if (max_health > 0.0f) {
            hp = health / max_health;
        }
        hp = Q_clamp_float(hp, 0.0f, 1.0f);

        {
            float maxStam = (pStamina->value > 0.1f) ? pStamina->value : 5.0f;
            stam = Q_clamp_float(m_fCoopStamina / maxStam, 0.0f, 1.0f);
        }
        {
            float runRef = (pRun->value > 1.0f) ? pRun->value : 287.0f;
            spd = Q_clamp_float(velocity.length() / runRef, 0.0f, 1.0f);
        }

        raw = 0.45f * m_fCoopStressSupp + 0.25f * (1.0f - hp) + 0.18f * (1.0f - stam) + 0.12f * spd;
        // same overloaded-flag trap as ApplyCoopBoneOffsets - MOVECONTROL_CROUCH raises PMF_VIEW_PRONE
        if (m_bCoopProne && (client->ps.pm_flags & PMF_VIEW_PRONE)) {
            raw *= 0.70f; // steadier than crouching - the whole body is supported
        } else if (client->ps.pm_flags & PMF_DUCKED) {
            raw *= 0.85f;
        }
        // [user 2026-08-27] BRACED calms the stress spread too - a supported gun does not shake.
        // Applied AFTER the stance chain so it composes with it rather than replacing it. This
        // damper has an identical TWIN in the cgame (CG_FeelStressAdvance) and the two must carry
        // the same weights, or the sway you feel and the spread you actually get drift apart.
        if (m_fCoopBrace > 0.0f) {
            static cvar_t *pBS = NULL;
            if (!pBS) { pBS = gi.Cvar_Get("coop_braceStress", "0.50", CVAR_ARCHIVE); }
            raw *= 1.0f - CoopBraceBonus() * (pBS ? pBS->value : 0.50f);
        }
    }

    rate = (raw > m_fCoopStress) ? 9.0f : 0.8f; // spike fast, bleed off slowly - adrenaline, not a meter
    {
        float k = dt * rate;
        if (k > 1.0f) { k = 1.0f; } // MANDATORY: two-sided ease with no floor to rescue an overshoot
        m_fCoopStress += (raw - m_fCoopStress) * k;
    }
    if (m_fCoopStress < 0.0005f && raw == 0.0f) { m_fCoopStress = 0.0f; }

    if (pDbg->integer && level.inttime - s_last > 500) {
        s_last = level.inttime;
        gi.Printf("^~^~^ STRESS cur=%.2f raw=%.2f | supp=%.2f hp=%.2f stam=%.2f spd=%.2f hits=%d\n",
                  m_fCoopStress, raw, m_fCoopStressSupp, hp, stam, spd, m_iCoopSuppHits);
        m_iCoopSuppHits = 0; // per-window count of rounds that cracked past - 0 here means the
                             // BulletAttack hook never fired, which is a different bug to a bad curve
    }
}

// Called from BulletAttack when a round not fired by this player cracks past them. Mirrors the client
// FX curve (cg_parsemsg.cpp: (1 - dist/255) * 0.75) so the screen effect and the aim penalty rise
// together - the player SEES the thing that is costing them accuracy.
// How long the pool waits before refilling after ANY spend. One place, so sprint, jump and vault
// cannot drift apart into three different economies.
// The active weapon's 0..1 weight. One accessor, because weight only reads as CONTRAST and six
// systems already degrade the player - scattering separate derivations would stack into sludge.
// HZM coop [user 2026-08-28] The brace envelope AS THE GAMEPLAY BONUS SEES IT. Prone gets a fraction,
// because prone is already the steadiest stance and a full brace on top would flatten the stance
// ladder. Every stability consumer reads this rather than m_fCoopBrace directly, so the two cannot
// drift apart - the raw envelope still drives the visuals and the HUD pip at full value.
float Player::CoopBraceBonus()
{
    static cvar_t *pPS = NULL;

    if (!pPS) { pPS = gi.Cvar_Get("coop_braceProneScale", "0.35", CVAR_ARCHIVE); }
    if (m_bCoopProne) {
        float f = pPS->value;
        if (f < 0.0f) { f = 0.0f; } else if (f > 1.0f) { f = 1.0f; }
        return m_fCoopBrace * f;
    }
    return m_fCoopBrace;
}


float Player::CoopActiveHeft()
{
    Weapon *w = GetActiveWeapon(WEAPON_MAIN);
    return w ? w->CoopHeft() : 0.0f;
}

float Player::CoopStaminaDelay()
{
    static cvar_t *pDelay = NULL;
    if (!pDelay) { pDelay = gi.Cvar_Get("coop_staminaRegenDelay", "1.2", CVAR_ARCHIVE); }
    return (pDelay->value > 0.0f) ? pDelay->value : 0.0f;
}

// HZM coop [user 2026-08-27] RECOIL THAT RECOVERS.
//
// The old model was one flat pitch nudge per shot with no recovery at all - the aim simply inched
// upward and stayed there. Every modern shooter kicks hard and then RETURNS most of it, and the
// authored TIKI data has always carried a per-weapon recentre speed for exactly that. The kick lands
// immediately, so the shot goes where the climbing gun points; the tick below hands it back at the
// weapon's own rate. coop_recoilRecover is the fraction that returns on its own - the remainder is
// permanent walk-up you compensate by hand, which is what stops sustained fire being free.
void Player::CoopAddRecoil(float fPitch, float fYaw, qboolean bVee, float fPitchClamp,
                           float fYawClamp, float fRecenter, float fMinDecay, float fMaxDecay)
{
    Vector vAng = GetViewAngles();

    // The authored clamps bound the ACCUMULATED climb, not the single shot.
    //
    // [vet, bug-2139] The pitch test was written in the wrong sign and never bounded anything. A
    // climb is NEGATIVE pitch here and the whole table is negative, so both owed and fPitch are
    // negative and 'owed - fPitch' moves TOWARD zero - it could only exceed +clamp if the
    // accumulator were large and POSITIVE, which never happens. On the rare shot where it did fire
    // it set fPitch = owed - clamp, whose magnitude is |owed| + clamp: it roughly DOUBLED the debt.
    // Simulated over ten seconds of fire, an M1 Garand settled near -66 degrees against its authored
    // ceiling of 8 - which pins the view at the engine's pitch stop and then leaves the player facing
    // the floor when the recovery hands back a figure the view no longer holds. Symmetric now.
    if (fPitchClamp > 0.0f) {
        float fNext = m_vCoopRecoilOwed[0] + fPitch;
        if (fNext < -fPitchClamp)     { fPitch = -fPitchClamp - m_vCoopRecoilOwed[0]; }
        else if (fNext > fPitchClamp) { fPitch =  fPitchClamp - m_vCoopRecoilOwed[0]; }
    }
    m_vCoopRecoilOwed[0] += fPitch;

    // [vet] V resolves against the accumulator, matching the cgame - the step that makes the muzzle
    // walk in a V instead of a straight line.
    if (bVee) {
        fYaw = m_vCoopRecoilOwed[0] * fYaw;
    }
    if (fYawClamp > 0.0f) {
        float fNext = m_vCoopRecoilOwed[1] + fYaw;
        if (fNext > fYawClamp)       { fYaw = fYawClamp - m_vCoopRecoilOwed[1]; }
        else if (fNext < -fYawClamp) { fYaw = -fYawClamp - m_vCoopRecoilOwed[1]; }
    }
    m_vCoopRecoilOwed[1] += fYaw;

    vAng[0] += fPitch;
    vAng[1] += fYaw;
    SetViewAngles(vAng);

    // [vet] do NOT adopt a new weapon's recentre on top of an old accumulator - a fast weapon would
    // dump a slow weapon's accumulated climb in a single frame, which reads as a view teleport rather
    // than a recovery. Blend by how much of the debt each weapon actually contributed.
    {
        float fOwed = (float)fabs(m_vCoopRecoilOwed[0]);
        float fNew  = (float)fabs(fPitch);
        if (m_fCoopRecoilRecenter <= 0.0f || (fOwed + fNew) <= 0.0001f) {
            m_fCoopRecoilRecenter = (fRecenter > 0.01f) ? fRecenter : 2.0f;
        } else {
            m_fCoopRecoilRecenter =
                (m_fCoopRecoilRecenter * fOwed + fRecenter * fNew) / (fOwed + fNew);
        }
    }
    m_fCoopRecoilMinDecay = (fMinDecay > 0.0f) ? fMinDecay : 12.0f;
    m_fCoopRecoilMaxDecay = (fMaxDecay > 0.0f) ? fMaxDecay : 25.0f;
}

void Player::TickCoopRecoil()
{
    static cvar_t *pRecover = NULL;
    float          dt;
    int            ax;
    Vector         vAng;

    if (m_fCoopRecoilRecenter <= 0.0f
        || (m_vCoopRecoilOwed[0] == 0.0f && m_vCoopRecoilOwed[1] == 0.0f)) {
        m_fCoopRecoilLast = level.time;
        return;
    }
    if (!pRecover) { pRecover = gi.Cvar_Get("coop_recoilRecover", "0.85", CVAR_ARCHIVE); }

    // [vet] ELAPSED TIME, not level.frametime. ClientThink runs once per usercmd, not once per server
    // frame, so integrating a fixed frame delta made the recovery scale with the client's framerate -
    // a 125 fps player recovered roughly six times faster than a 20 fps one on the same server.
    // Clamped so a hitch cannot dump the whole accumulator in one step.
    dt = level.time - m_fCoopRecoilLast;
    m_fCoopRecoilLast = level.time;
    if (dt <= 0.0f) { return; }
    if (dt > 0.1f)  { dt = 0.1f; }

    vAng = GetViewAngles();
    for (ax = 0; ax < 2; ax++) {
        float owed = m_vCoopRecoilOwed[ax];
        float rate, back;

        if (owed == 0.0f) { continue; }

        // [vet] the cgame bounds its decay to [minDecay, maxDecay] degrees per second and every row
        // carries those bounds. Without the floor a weapon with a low recentre - the Garand's 0.15 -
        // took the better part of a minute to hand the climb back. Proportional decay alone is not
        // the model EA shipped.
        rate = (float)fabs(owed) * m_fCoopRecoilRecenter;
        if (rate < m_fCoopRecoilMinDecay) { rate = m_fCoopRecoilMinDecay; }
        if (rate > m_fCoopRecoilMaxDecay) { rate = m_fCoopRecoilMaxDecay; }

        back = rate * dt;
        if (back > (float)fabs(owed)) { back = (float)fabs(owed); }
        if (owed < 0.0f) { back = -back; }

        vAng[ax] -= back * pRecover->value;
        m_vCoopRecoilOwed[ax] -= back;
        if (m_vCoopRecoilOwed[ax] < 0.01f && m_vCoopRecoilOwed[ax] > -0.01f) {
            m_vCoopRecoilOwed[ax] = 0.0f;
        }
    }
    SetViewAngles(vAng);
}
void Player::CoopAddSuppression(float amount)
{
    if (amount <= 0.0f || deadflag) {
        return;
    }
    // HZM coop [user 2026-08-27] MOUNTED SOLDIERS DO NOT FLINCH. Rounds cracking past normally load
    // the stress scalar, which shakes the hands and opens the cone - the single most felt thing in a
    // firefight. A weapon resting on a surface is not being held up by a startled man, and every
    // milsim that ships mounting cuts incoming flinch for exactly that reason. Damped at the SOURCE
    // so one change reaches every consumer downstream (spread, sway, breathing) instead of three
    // separate ones that could drift apart.
    if (m_fCoopBrace > 0.0f) {
        static cvar_t *pBF = NULL;
        if (!pBF) { pBF = gi.Cvar_Get("coop_braceFlinch", "0.80", CVAR_ARCHIVE); }
        amount *= 1.0f - CoopBraceBonus() * ((pBF ? pBF->value : 0.80f));
        if (amount <= 0.0f) {
            return;
        }
    }
    m_fCoopStressSupp += amount;
    m_iCoopSuppHits++;
    if (m_fCoopStressSupp > 1.0f) { m_fCoopStressSupp = 1.0f; }
}

// HZM coop [user 2026-08-26] MP3 PRONE FLUIDITY P1 - RATE-LIMITED BODY YAW.
//
// PmoveAdjustAngleSettings sets body yaw = view yaw EVERY FRAME, so a prone body pivoted instantly
// on its belly - it read as sliding, the exact opposite of the fluid ground movement asked for
// ("similar to max payne 3"). While prone, the body now EASES toward the view at coop_proneTurnRate
// deg/s and the legs statemap plays the shipped rifle_prone_turn_left/right anims while it catches
// up (COOP_PRONE_TURNL/R, 25-degree engage / 5-degree settle hysteresis).
//
// SERVER-ONLY is safe for the same reason the bone offsets are: first person never draws your own
// body, and PmoveAdjustAngleSettings_Client only runs in first person - so the eased yaw reaches
// every observer through the networked entity angles with no client change.
//
// KNOWN LIMIT, stated now: crawl direction stays VIEW-relative (pmove moves along viewangles), so
// during a large catch-up the feet can slide slightly against the crawl direction. Acceptable at
// 120 deg/s; steering by eased body yaw instead would change how crawling handles - a separate call.
//
// NOT gated on the hull (bug-2112 lesson): m_bCoopProne is the coop decision, and DBNO shares the
// 20u hull but must keep vanilla snap behaviour.
void Player::CoopProneBodyYaw(vec3_t vAngles)
{
    static cvar_t *pRate = NULL;
    float          fDelta, fStep, fAbs, fStepRate;

    if (!pRate) { pRate = gi.Cvar_Get("coop_proneTurnRate", "120", CVAR_ARCHIVE); }
    fStepRate = pRate->value;

    if (!m_bCoopProne || pRate->value <= 0.0f) {
        // keep the ease state synced to the true body yaw so prone ENTRY starts from reality
        m_fCoopProneBodyYaw = vAngles[1];
        m_iCoopProneTurnDir = 0;
        m_bCoopSupine = false;
        m_fCoopSupineFlip = 0;
        return;
    }

    // ---- P2: ROLL ONTO YOUR BACK -------------------------------------------------------------
    // The ask, verbatim: aim behind you in freecam, hold ADS, and 'your body flipped so you are
    // now laying on your back facing that direction. The trigger is therefore the AIM, not a key:
    // while ADS is held and the view is more than coop_supineEnter degrees off the body facing,
    // latch supine. On your back the ease drives the body toward view+180 (feet away from the aim,
    // firing over your head), so continuing to sweep keeps pivoting you smoothly ON your back.
    // Aim returning to within coop_supineExit of the body facing rolls you back onto your front -
    // wide hysteresis (110 in / 60 out) so the boundary cannot flap. The lateral roll anims play
    // as the flip via COOP_SUPINE_FLIPL/R; coop_supine 0 disables the trigger (coop_supineTest
    // still forces the pose for eyeballing).
    {
        static cvar_t *pSupOn = NULL, *pSupIn = NULL, *pSupOut = NULL;
        float          fOff;

        // [user 2026-08-27] DEFAULT OFF. The screenshot settles it: the rigid-transform supine pose puts
// the legs in the air, and not by a tuning margin. The flip mirrors every point about a plane
// through the body centreline at z=19, so a boot resting flat ~15 units BELOW that plane lands ~15
// units above it - thirty units off the deck. A person rolling over re-plants their legs; a mirror
// cannot, because it does not know the floor exists. No value of untwist, lift, arms angle or cone
// fixes that: it is the transform, not the parameters. Belly prone is unaffected and stays on.
// coop_supine 1 re-enables the experiment for anyone iterating on it.
if (!pSupOn)  { pSupOn  = gi.Cvar_Get("coop_supine", "0", CVAR_ARCHIVE); }
        if (!pSupIn)  { pSupIn  = gi.Cvar_Get("coop_supineEnter", "110", CVAR_ARCHIVE); }
        if (!pSupOut) { pSupOut = gi.Cvar_Get("coop_supineExit", "60", CVAR_ARCHIVE); }

        fOff = AngleSubtract(vAngles[1], m_fCoopProneBodyYaw); // aim vs the body's BELLY facing
        if (!pSupOn->integer) {
            m_bCoopSupine = false;
        } else if (!m_bCoopSupine) {
            // [review F1, bug-2124] the enter trigger MUST yield to an armed crouch-press exit and
            // to a running flip window. Without this, a crouch-exit while ADS was held re-latched
            // supine on the SAME frame (TickCoopProne clears in ClientThink; this runs later in
            // PlayerAngles, and with the aim still ~180 off the belly the enter condition is
            // trivially true) - the exit block then re-armed every frame: a soft-strand while
            // crouch+ADS were held, ending in exactly the 180-degree stand-up snap the deferred
            // exit exists to prevent.
            if ((last_ucmd.buttons & BUTTON_COOPADS) && m_fCoopProneExitAt <= 0.0f
                && (client->ps.pm_flags & PMF_VIEW_PRONE) // [pass4] never latch supine on a non-prone hull
                && level.time >= m_fCoopSupineFlip
                // [pass3, bug-2126] and to a RUNNING evasive roll - the roll gate yields to
                // flips (13836) but not vice versa, so a latch on the roll's first frame cut
                // the anim yet kept its 170 u/s side impulse: the body slid through the flip.
                && level.time >= m_fCoopProneRollEnd
                && (fOff > pSupIn->value || fOff < -pSupIn->value)) {
                m_bCoopSupine        = true;
                // [supine-yaw] latch the belly facing we are turning away FROM: once on your
                // back the body tracks the view, so the view-vs-body offset is ~0 and can no
                // longer say whether the aim has come back forward. This reference can.
                m_fCoopSupineRefYaw  = m_fCoopProneBodyYaw;
                m_iCoopSupineFlipDir = (fOff > 0.0f) ? 1 : -1;
                // [spec A4] per-direction: rolll is 9 frames (0.9s), rollr is 10 (1.0s)
                m_fCoopSupineFlip    = level.time + ((m_iCoopSupineFlipDir > 0) ? 0.9f : 1.0f);
            }
        } else {
            // [user 2026-08-26] RELEASING ADS ALSO ROLLS YOU BACK. The first cut only exited when
            // the aim returned toward the feet, so letting go of ADS left you lying on your back
            // ('I get stuck in the ground when I let go of it'). ADS is the hold: release it, or
            // bring the aim forward, and you roll onto your front either way.
            // [v3] THE SUPINE CONE. On your back you can traverse a limited arc over your own feet;
            // past it a real person rolls back onto their front and turns, which retail already
            // animates. So leaving the cone in EITHER direction rolls out - it never spins you round
            // on your back, which is the failure mode both earlier versions had.
            static cvar_t *pCone = NULL;
            float          fCone;

            if (!pCone) { pCone = gi.Cvar_Get("coop_supineCone", "60", CVAR_ARCHIVE); }
            fCone = AngleSubtract(vAngles[1], AngleMod(m_fCoopSupineRefYaw + 180.0f));
            if (fCone > pCone->value || fCone < -pCone->value
                || !(last_ucmd.buttons & BUTTON_COOPADS)) {
                m_bCoopSupine        = false;
                // [pass2] settled supine sits at the +/-180 wrap, where fOff's sign is numerical
                // noise - the roll-out shoulder was a coin toss. Reverse the shoulder you rolled
                // IN on instead (the crouch-press exit already does).
                m_iCoopSupineFlipDir = (m_iCoopSupineFlipDir >= 0) ? 1 : -1;
                m_fCoopSupineFlip    = level.time + ((m_iCoopSupineFlipDir > 0) ? 0.9f : 1.0f);
            }
        }
    }

    {
        // [spec A4] during the flip window the ease runs at coop_supineFlipRate (200 deg/s =
        // 180/0.9) so the yaw finishes WITH the roll animation - at the old 120 the anim ended
        // 0.6s early and the settled pose pivoted flat like a record player for the remainder.
        static cvar_t *pFlipRate = NULL;
        // [supine-yaw, user 2026-08-27, bug-2129] THE TARGET IS THE VIEW, NOT VIEW+180.
        //
        // "even though im on my back, my torso and gun dont turn and face the correct way, so
        // essentially my head and gun are just upside down." The +180 rested on a false premise:
        // that rolling onto your back reverses which way the body faces. It does not - skc_flip
        // rolls 180 about the model's own X, the head-to-toe long axis, so the head stays at the
        // head end and everything that pointed forward still points forward. The animation's gun
        // direction IS the body yaw; only the belly now faces the sky.
        //
        // With the +180 the arithmetic cancelled exactly: flipping while aiming 180 off meant a
        // target of (view+180) == the belly facing the body ALREADY held, so the body never
        // rotated at all and the gun kept pointing where you used to be aiming. The 200 deg/s
        // flip rate exists to sweep 180 degrees across the roll (spec A4) and had nothing to
        // sweep - the tell that the target, not the rate, was wrong.
        // HZM coop [user 2026-08-27, v3] TWO SUPINE MODES, because two guesses were already wrong.
        //
        // The research settled the mechanic: TLOU2's rule, shared by R6 Siege and MGSV, is that you
        // aim toward your HEAD end belly-down and toward your FEET end belly-up. The threat is past
        // your feet; your head stays exactly where it was, your body never rotates, and you shoot out
        // over your own abdomen. That is coop_supineMode 1 - the body PLANTS, target view+180.
        //
        // The catch, and the reason two attempts failed: in every prone source the head axis and the
        // gun both point along model +X, and a rigid rotation moves both identically. So no transform
        // of a prone clip can aim the gun backward while leaving the head put. v1 had this yaw policy
        // with a pose whose gun pointed the old way; v2 fixed the gun by spinning the whole body,
        // which is the record-player artifact the user rejected. The missing piece is not the yaw at
        // all - it is reversing the ARMS at the shoulders, below.
        //
        // Mode 0 keeps the v2 somersault so the two can be compared in play rather than argued about.
        static cvar_t *pMode = NULL;
        float          fTarget;

        if (!pMode) { pMode = gi.Cvar_Get("coop_supineMode", "1", CVAR_ARCHIVE); }
        fTarget = (m_bCoopSupine && pMode->integer) ? AngleMod(vAngles[1] + 180.0f) : vAngles[1];

        if (!pFlipRate) { pFlipRate = gi.Cvar_Get("coop_supineFlipRate", "200", CVAR_ARCHIVE); }
        fDelta = AngleSubtract(fTarget, m_fCoopProneBodyYaw);
        m_fCoopProneYawTarget = fTarget; // [spec A3] hysteresis + aim-lead re-base on the TARGET
        // [v3] the sweep only exists in mode 0. In mode 1 there is nothing to sweep - and the sweep
        // IS the artifact the user objected to, so it must not be merely retuned.
        if (!pMode->integer && level.time < m_fCoopSupineFlip && pFlipRate->value > pRate->value) {
            fStepRate = pFlipRate->value;
        }
    }
    // HZM coop [user 2026-08-27] PLANT THE BODY WHILE AIMING - the Max Payne 3 rule.
    //
    // The supine trigger measures the aim against the body facing, but the body CHASED the
    // view at coop_proneTurnRate (120 deg/s) unconditionally, so the offset could never
    // accumulate: "the camera just follows the crosshair as it moves around and my players
    // body also moves accordingly. But in no instance are we laying on our back." You cannot
    // out-sweep a body that is glued to your reticle - the 110-degree threshold was
    // unreachable by construction, not by tuning.
    //
    // While ADS is HELD and belly-down, the body now creeps at coop_proneAdsTurnRate (25 deg/s)
    // instead: it still settles toward where you are looking, but the aim outruns it easily,
    // the offset builds, and sweeping past the threshold rolls you onto your back. Aiming is
    // the plant; letting go of ADS resumes the normal 120 chase.
    //
    // NOT applied while supine or mid-flip: on your back the ease must keep driving toward
    // view+180 so continuing to sweep pivots you smoothly ON your back (that part already
    // worked - "most of it works"), and the flip windows own the rate outright (spec A4).
    // [user 2026-08-27] A STILL PRONE BODY DOES NOT FOLLOW YOUR EYES.
    //
    // The plant used to require ADS to be HELD, but the user's actual sequence - and the one that
    // matches how people describe this - is "move your camera to look behind you and THEN hold your
    // ads button". During that look the body was still chasing at the full 120 deg/s, so by the time
    // the button went down the aim and the body agreed again, there was no offset left, and the flip
    // could never trigger. What the user saw instead was the body pivoting round to face the new
    // direction: not a bug in the flip, a bug in what happened BEFORE it.
    //
    // A prone soldier turns his head freely and his whole body only deliberately. So while lying
    // still the body barely tracks at all (coop_proneStillTurnRate), and holding ADS plants it harder
    // still. CRAWLING keeps the full rate - if you are moving, you go where you look.
    // [vet] ...but never so slowly that the trigger stays dead. The aim gate strips fire while the
    // gun disagrees with the crosshair by more than coop_proneAimGate, and the plant was allowed to
    // close that gap at 25 deg/s - so an ordinary 90 degree turn to a new target cost nearly two
    // seconds of dead trigger. Outside the gate the body turns at the full rate; the plant only
    // holds it still INSIDE the arc the player can already shoot into.
    {
        static cvar_t *pGate = NULL;
        float          fAim;
        if (!pGate) { pGate = gi.Cvar_Get("coop_proneAimGate", "45", CVAR_ARCHIVE); }
        fAim = AngleSubtract(vAngles[1], m_fCoopProneBodyYaw);
        if (pGate->value > 0.0f && (fAim > pGate->value || fAim < -pGate->value)) {
            fStep = fStepRate * level.frametime;
            fDelta = Q_clamp_float(fDelta, -fStep, fStep);
            m_fCoopProneBodyYaw = AngleMod(m_fCoopProneBodyYaw + fDelta);
            fAbs = AngleSubtract(m_fCoopProneYawTarget, m_fCoopProneBodyYaw);
            if (fAbs > 25.0f)       { m_iCoopProneTurnDir = 1; }
            else if (fAbs < -25.0f) { m_iCoopProneTurnDir = -1; }
            else if (fAbs < 5.0f && fAbs > -5.0f) { m_iCoopProneTurnDir = 0; }
            vAngles[1] = m_fCoopProneBodyYaw;
            return;
        }
    }

    if (!m_bCoopSupine && level.time >= m_fCoopSupineFlip
        && !last_ucmd.forwardmove && !last_ucmd.rightmove) {
        static cvar_t *pStillRate = NULL, *pAdsRate = NULL;
        float          fWant;

        if (!pStillRate) {
            pStillRate = gi.Cvar_Get("coop_proneStillTurnRate", "55", CVAR_ARCHIVE);
            pAdsRate   = gi.Cvar_Get("coop_proneAdsTurnRate", "25", CVAR_ARCHIVE);
        }
        fWant = (last_ucmd.buttons & BUTTON_COOPADS) ? pAdsRate->value : pStillRate->value;
        if (fWant >= 0.0f && fWant < fStepRate) {
            fStepRate = fWant;
        }
    }

    // HZM coop [user 2026-08-27, v3] REVERSE THE ARMS AT THE SHOULDERS.
    //
    // This is the piece that makes the whole thing possible, and it exists only because a standing
    // assumption was wrong. These four controllers are NOT small clamped additive nudges on the spine
    // - they are full-range, networked, RE-POINTABLE model-space subtree rotations, and the deadband
    // clamps we believed in live in Actor code, not on the player path. So the arms can simply be
    // turned around at the clavicles: measured on this rig, a 180 on both clavicle subtrees over the
    // existing flip leaves the head and pelvis untouched and puts the muzzle over the feet, sights up.
    //
    // It also cures a defect nobody had noticed: the pure long-axis flip was holding the rifle UPSIDE
    // DOWN, because that roll maps the weapon's up vector to down.
    //
    // Bone angles are already networked entityState fields, so this reaches every observer with no
    // protocol change and no exe.
    {
        static cvar_t *pArms = NULL;
        qboolean       bWant;

        if (!pArms) { pArms = gi.Cvar_Get("coop_supineArms", "1", CVAR_ARCHIVE); }
        bWant = (qboolean)(m_bCoopSupine && pArms->integer);
        if (bWant && !m_bCoopSupineArmsOn) {
            SetControllerTag(ARMS_TAG, gi.Tag_NumForName(edict->tiki, "Bip01 L Clavicle"));
            SetControllerTag(PELVIS_TAG, gi.Tag_NumForName(edict->tiki, "Bip01 R Clavicle"));
            m_bCoopSupineArmsOn = true;
        } else if (!bWant && m_bCoopSupineArmsOn) {
            SetControllerTag(ARMS_TAG, gi.Tag_NumForName(edict->tiki, "Bip01 Spine1"));
            SetControllerTag(PELVIS_TAG, gi.Tag_NumForName(edict->tiki, "Bip01 Pelvis"));
            m_bCoopSupineArmsOn = false;
        }
    }

    fStep  = fStepRate * level.frametime;
    fDelta = Q_clamp_float(fDelta, -fStep, fStep);
    m_fCoopProneBodyYaw = AngleMod(m_fCoopProneBodyYaw + fDelta);

    // hysteresis so the turn anim does not flicker at the boundary; sign convention: positive
    // delta = view is to the LEFT (MOHAA yaw increases CCW), body turns left. If play shows the
    // anims mirrored, swap the two conditionals - one line each.
    // [spec A3] against the TARGET (view+180 while supine) - measuring against the raw view left
    // m_iCoopProneTurnDir pinned at +/-1 for the entire time on the back.
    fAbs = AngleSubtract(m_fCoopProneYawTarget, m_fCoopProneBodyYaw);
    if (fAbs > 25.0f)       { m_iCoopProneTurnDir = 1; }
    else if (fAbs < -25.0f) { m_iCoopProneTurnDir = -1; }
    else if (fAbs < 5.0f && fAbs > -5.0f) { m_iCoopProneTurnDir = 0; }

    vAngles[1] = m_fCoopProneBodyYaw;
}


// HZM coop [user 2026-08-25] COOP BONE OFFSETS - prone spine bias, head tracking, torso lag.
//
// WHY IT LIVES HERE AND NOWHERE ELSE. PmoveAdjustAngleSettings (bg_pmove.cpp:1622) is the SOLE
// writer of the player's four bone controllers - a sweep of every bone_angles writer in the tree
// found no other. It runs from Player::EndFrame -> FinishMove -> PlayerAngles, i.e. AFTER
// ClientThink, and it assigns with VectorCopy rather than accumulating. Anything written before it
// is gone. That was measured, not reasoned: writing an impossible sentinel (head 11/22, torso 33/44)
// from TickCoopLook read back as 0.00/0.00 on every one of 328 samples (bug-2101).
//
// So these offsets are applied HERE, and ADDITIVELY, so the vanilla behaviour survives underneath.
// That matters: pmove distributes your view pitch down the spine chain (pelvis/Spine1/Spine2/head
// shares that sum to exactly 1.0), and that distribution IS the look-down body bend. Replacing it
// would flatten the player every time they looked at their feet.
//
// PRONE. The same arithmetic explains the prone complaint. Lying down and aiming level means a view
// pitch near zero, so every share is near zero, so the spine is STRAIGHT - and a straight spine on
// top of the flat pelvis the prone legs animation produces is a chest standing vertically. Hence
// 'my torso pops up as though I am crouched'. The fix is to feed the chain a pitch it cannot get
// from the view: a constant bias while prone.
//
// The head is counter-rotated by the FULL bias because it is a descendant of both spine bones and
// therefore inherits Spine1 + Spine2 in full - without it the player would be face-down in the dirt
// while the camera looked at the horizon.
// HZM coop [user 2026-08-27] GUN BRACING - "if you are in first person and leaning by a wall, or
// crouched up against an object (like one that does not get considered cover) the feeling of the
// gun should feel supported with stronger accuracy and weight adjustments".
//
// AUTOMATIC, never a bind (the Rising Storm 2 model, and the user's call): the surveyed games that
// ask for a button do it because their buff is enormous (CoD mounts ~90% of recoil away); ours is
// deliberately mid-pack, so it can simply be granted. It is a BUFF and never blocks the gun - the
// games that BLOCK firing near geometry are the ones players resent.
//
// Three ways to be supported, cheapest first:
//   C  cover peek       - zero new traces; the anchored cover sustain already revalidated geometry
//                         this frame. Aimed cover fire is otherwise UNREWARDED today (blindfire is
//                         penalised 3x, peek gets plain standing spread).
//   B' lean at a corner - lean PLUS a confirming trace. Never lean alone: vanilla lean performs no
//                         geometry test at all, so raw lean would hand out free accuracy in an open
//                         field.
//   A  muzzle support   - the sills, crates, sandbags and fences the cover system never claims. The
//                         support trace must HIT below the gun line while the eye line stays CLEAR,
//                         the same inverse pair the vault code uses: you brace on what you can
//                         shoot OVER, not on what you are facing.
//
// Generosity is deliberate. The most-praised part of RS2's resting is that it is pure geometry with
// no eligibility list; the most-complained-about part of Sandstorm's and Hell Let Loose's is that
// theirs is strict. The anti-camping price here is the stillness gate, not a stingy trigger.
void Player::TickCoopBrace()
{
    // [vet] elapsed time, for the same reason as the stress envelope above.
    float dtBrace = level.time - m_fCoopBraceLast;
    m_fCoopBraceLast = level.time;
    if (dtBrace < 0.0f)      { dtBrace = 0.0f; }
    else if (dtBrace > 0.25f){ dtBrace = 0.25f; }
    static cvar_t *pOn = NULL, *pDist = NULL, *pDelay = NULL, *pGrace = NULL, *pDbg = NULL;
    qboolean bGeom = qfalse;
    float    fSpeed2, fTarget, fStep;

    if (!pOn) {
        pOn    = gi.Cvar_Get("coop_brace", "1", CVAR_ARCHIVE);
        pDist  = gi.Cvar_Get("coop_braceDist", "36", CVAR_ARCHIVE);
        pDelay = gi.Cvar_Get("coop_braceDelay", "0.12", CVAR_ARCHIVE);
        pGrace = gi.Cvar_Get("coop_braceGrace", "0.35", CVAR_ARCHIVE);
        pDbg   = gi.Cvar_Get("coop_braceDebug", "0", 0);
    }

    // --- gates: integer compares only, before any trace ---------------------------------------
    // [user 2026-08-28] PRONE CAN NOW BRACE. It was excluded because it 'already owns the tightest
    // spread lane in the game, and stacking brace on top would collapse the stance ladder it earns' -
    // a real balance point, not an oversight, so it is answered rather than deleted: prone still
    // mounts, but the stability it gains is scaled by coop_braceProneScale (0.35) instead of the full
    // bonus. Prone+braced therefore sits just above prone rather than lapping it, and the ladder
    // stand -> crouch -> prone -> prone-braced still climbs. Resting a rifle on a wall while prone is
    // also simply what a soldier does, and the pose already reads as braced.
    if (!pOn->integer || deadflag || IsSpectator() || m_pVehicle || m_pTurret
        || m_pLadder || level.playerfrozen || m_bFrozen || (flags & FL_IMMOBILE) || !groundentity
        || m_bCoopBlindfire
        // [bug-2133] DBNO was missing: going down clears m_bCoopProne and keeps deadflag false, so a
        // downed player could mount and crawl around with forced ADS and the full buff.
        || m_bCoopDbno
        || (client->ps.pm_flags & (PMF_SPECTATING | PMF_INTERMISSION | PMF_FROZEN | PMF_NO_MOVE))
        // [user 2026-08-27] FIRST PERSON ONLY. Mounting forces ADS, clamps the aim to a cone and
        // plants the viewmodel on the surface - three things that only mean anything down the sights.
        // In third person it would be a silent stat buff with a camera that ignores all of it.
        // m_bCoopView3p is the cgame's own final view mode, mirrored through the u_view3p userinfo.
        || m_bCoopView3p /*[user 08-27] brace is a first-person mechanic*/) {
        m_fCoopBraceDwell = 0.0f;
        m_fCoopBraceHold  = 0.0f;
        m_bCoopBraceStill = false;
        bGeom             = qfalse;
        m_bCoopBraceMounted = false; // switching to 3P (or any gate) stands the mount down
    } else {
        // stillness, with the same shape of hysteresis the crawl no-fire strip uses: a braced gun
        // is a planted gun. This is what stops the mounted-and-strafing look that every surveyed
        // game's players complain about.
        fSpeed2 = velocity[0] * velocity[0] + velocity[1] * velocity[1];
        if (fSpeed2 > 35.0f * 35.0f)      { m_bCoopBraceStill = false; }
        else if (fSpeed2 < 20.0f * 20.0f) { m_bCoopBraceStill = true; }

        if (m_bCoopBraceStill) {
            Vector vFwd, vRight, vAng, vStart;
            float  fDist = (pDist->value > 8.0f) ? pDist->value : 8.0f;
            float  fSup, fEye;
            trace_t tr;

            // FLAT yaw forward, not the 3D aim: aiming down over a sill must still qualify.
            vAng = Vector(0.0f, client->ps.viewangles[YAW], 0.0f);
            AngleVectors(vAng, vFwd, vRight, NULL);

            // [user 2026-08-27] mount and cover are ALTERNATIVES, not partners. Cover-peek used to
            // grant a brace outright, which meant the two systems raced for the same moment and got
            // in each other's way. Availability is now PURELY geometric - "as long as it's the proper
            // crouch height" - so from cover you get a real choice: keep blindfiring or peeking, or
            // press Use and mount. Whichever you pick, the other steps out of the way.

            // B' - leaning past halfway, confirmed by one shoulder trace toward the lean side
            if (!bGeom && (client->ps.fLeanAngle > 12.0f || client->ps.fLeanAngle < -12.0f)) {
                Vector vSide = (client->ps.fLeanAngle > 0.0f) ? vRight : (vRight * -1.0f);
                vStart = origin + Vector(0.0f, 0.0f, (float)viewheight);
                tr = G_Trace(vStart, vec_zero, vec_zero, vStart + vSide * 48.0f, this, MASK_SOLID,
                             false, "Player::TickCoopBrace lean-confirm");
                if (!tr.startsolid && tr.fraction < 1.0f && tr.plane.normal[2] < 0.7f
                    && tr.plane.normal[2] > -0.7f
                    && !(tr.ent && tr.ent->entity && tr.ent->entity->IsSubclassOfSentient())) {
                    bGeom = qtrue;
                }
            }

            // A - support under the gun line, sightline clear above it.
            //
            // [user 2026-08-27] brace v2 - SCAN A BAND OF HEIGHTS, not one. The first cut probed a
            // single height (70 standing) and so only ever found CHEST-high surfaces; almost all
            // usable cover in this game is waist-high, which sits below that probe and was missed
            // entirely - "I couldnt get it to work in a doorway or using q and e on a wall". The
            // band runs from just under the gun line down to waist height, which is exactly the
            // range a soldier can actually rest a weapon on.
            if (!bGeom) {
                static const float kStand[3] = {70.0f, 60.0f, 50.0f};
                static const float kCrouch[3] = {46.0f, 38.0f, 30.0f};
                const float *pBand = (m_iMovePosFlags & MPF_POSITION_CROUCHING) ? kCrouch : kStand;
                float fEye = (m_iMovePosFlags & MPF_POSITION_CROUCHING) ? 52.0f : 82.0f;
                float fSupFrac = 1.0f;
                int   k;

                for (k = 0; k < 3 && !bGeom; k++) {
                    vStart = origin + Vector(0.0f, 0.0f, pBand[k]);
                    tr = G_Trace(vStart, vec_zero, vec_zero, vStart + vFwd * fDist, this, MASK_SOLID,
                                 false, "Player::TickCoopBrace support");
                    if (tr.startsolid || tr.fraction >= 1.0f || tr.plane.normal[2] >= 0.7f
                        || tr.plane.normal[2] <= -0.7f || DotProduct(tr.plane.normal, vFwd) >= -0.5f
                        || (tr.ent && tr.ent->entity && tr.ent->entity->IsSubclassOfSentient())) {
                        continue;
                    }
                    fSupFrac = tr.fraction; // keep it: the eye-clear trace below reuses tr
                    // the surface must be something we can shoot OVER, not something we face
                    vStart = origin + Vector(0.0f, 0.0f, fEye);
                    tr = G_Trace(vStart, vec_zero, vec_zero, vStart + vFwd * (fDist + 8.0f), this,
                                 MASK_SOLID, false, "Player::TickCoopBrace eye-clear");
                    if (tr.fraction >= 1.0f) {
                        bGeom = qtrue;
                        // [user 2026-08-27] remember HOW FAR the support actually is, so the weapon
                        // can be rested against the real surface instead of a guessed offset.
                        m_fCoopBraceRest = fDist * fSupFrac;
                    }
                }
            }

            // D - SHOULDER/SIDE CONTACT: a doorway jamb, the corner of a building, the wall you are
            // stood flat against. [user 2026-08-27] These are the cases the user actually reached for
            // and none of them put geometry in FRONT of the gun, so no forward probe could ever have
            // found them. Bracing a weapon against the side of an opening is one of the most common
            // real supported positions there is; it needs no lean and no cover state.
            if (!bGeom) {
                // [user 2026-08-27] "doesnt seem to catch leaning into doorways well". 26u was the
                // problem: a MOHAA doorway is ~64-72 units wide, so standing anywhere near its middle
                // puts each jamb 32-36u away and the probe simply fell short. Reach is now a cvar and
                // defaults past the half-width of a standard opening, and it samples three heights so
                // a low sill or a waist-high frame counts too.
                static cvar_t *pSide = NULL;
                float          fSideLen;
                int            iSide, iSh;
                float          kSideZ[3];

                if (!pSide) { pSide = gi.Cvar_Get("coop_braceSideDist", "44", CVAR_ARCHIVE); }
                fSideLen = (pSide->value > 8.0f) ? pSide->value : 8.0f;
                if (m_iMovePosFlags & MPF_POSITION_CROUCHING) {
                    kSideZ[0] = 44.0f; kSideZ[1] = 36.0f; kSideZ[2] = 26.0f;
                } else {
                    kSideZ[0] = 62.0f; kSideZ[1] = 52.0f; kSideZ[2] = 42.0f;
                }
                for (iSide = 0; iSide < 2 && !bGeom; iSide++) {
                  Vector vS = (iSide == 0) ? vRight : (vRight * -1.0f);
                  for (iSh = 0; iSh < 3 && !bGeom; iSh++) {
                    vStart = origin + Vector(0.0f, 0.0f, kSideZ[iSh]);
                    tr = G_Trace(vStart, vec_zero, vec_zero, vStart + vS * fSideLen, this, MASK_SOLID,
                                 false, "Player::TickCoopBrace side");
                    if (!tr.startsolid && tr.fraction < 1.0f && tr.plane.normal[2] < 0.7f
                        && tr.plane.normal[2] > -0.7f
                        && !(tr.ent && tr.ent->entity && tr.ent->entity->IsSubclassOfSentient())) {
                        bGeom = qtrue;
                        m_fCoopBraceRest = fSideLen * tr.fraction;
                    }
                  }
                }
            }
        }
    }

    // --- hysteresis: small ENTER threshold, generous EXIT ---------------------------------------
    // Sticky mounts are the CoD complaint; a brace that drops the instant a trace flickers is the
    // Squad one. Dwell in, grace out.
    // [user 2026-08-27] MOUNTING, not auto-bracing. The automatic version worked but was invisible:
    // "It might be working but there's nothing that really suggests it... its hard to really tell
    // youre actually braced". Without a moment of commitment there is no before and after to notice.
    // So geometry now only OFFERS the mount (prompt icon); pressing Use takes it. That single change
    // also earns a far stronger effect set, because the player asked for it deliberately - the same
    // reason CoD can afford to mount ~90% of recoil away while an automatic system cannot.
    if (bGeom) {
        m_fCoopBraceDwell += dtBrace;
        m_fCoopBraceHold   = level.time + ((pGrace->value > 0.0f) ? pGrace->value : 0.35f);
    } else {
        m_fCoopBraceDwell = 0.0f;
    }
    m_bCoopBraceAvail = (m_fCoopBraceDwell >= pDelay->value || level.time < m_fCoopBraceHold);

    // Use TOGGLES the mount, and moving breaks it - the user's own exit rule. The stillness latch
    // above already went false the moment they moved, which drops availability and unmounts here.
    {
        // [vet] read the LIVE command, not last frame's copy, and CONSUME the press when the mount
        // takes it. This tick runs before last_ucmd is refreshed and before DoUse is dispatched, so
        // clearing the bit here means one press does one thing. Without it, standing near any wall -
        // and the side probe offers a mount off a wall 44u away with no facing requirement - made
        // every Use press both mount the weapon and work whatever was behind it.
        // [user 2026-08-28] MOVING BREAKS THE MOUNT - "moving in any direction while braced should exit
        // brace (W,S,A,D)". Tested on the INPUT, not on the stillness/velocity latch that already feeds
        // availability: that one runs through a grace window and a dwell timer, so it let a mount survive
        // a step. Reading the movement axes makes the rule exactly what the player pressed, and it means
        // walking away always releases even if the surface probe still reports a wall.
        if (m_bCoopBraceMounted && current_ucmd
            && (current_ucmd->forwardmove || current_ucmd->rightmove)) {
            m_bCoopBraceMounted   = false;
            m_fCoopCoverAutoDwell = 0.0f;
            m_fCoopCoverAutoRetry = level.time + 0.6f;
        }

        // [user 2026-08-28] AND THE MOUNT OWNS THE BODY FOR AS LONG AS IT IS HELD. Taking the mount
        // cleared these once, but auto-cover re-requests on the very next frame, and the torso statemap
        // evaluates COOP_COVER_LOW above the ADS row - so crouched at a wall the cover pose kept stealing
        // the torso and a braced player never reached the sights (standing looked fine only because low
        // cover was not triggering there). Re-asserting every frame is the server half; the !COOP_BRACED
        // guard on those two statemap rows is the client half. Both, because either alone leaves a frame
        // where the two systems disagree about who owns the pose.
        if (m_bCoopBraceMounted) {
            m_bCoopCoverRequested = false;
            m_bCoopCoverWall      = false;
            m_bCoopCoverLow       = false;
            m_bCoopBlindfire      = false;
        }

        qboolean bUseNow = (current_ucmd && (current_ucmd->buttons & BUTTON_USE)) ? qtrue : qfalse;
        if (bUseNow && !m_bCoopBraceUsePrev) {
            if (m_bCoopBraceMounted) {
                m_bCoopBraceMounted = false;
                // give cover a beat before its auto-dwell can grab you again, so unmounting does not
                // instantly snap you into the pose you just chose to leave
                m_fCoopCoverAutoDwell = 0.0f;
                m_fCoopCoverAutoRetry = level.time + 0.6f;
                if (current_ucmd) { current_ucmd->buttons &= ~BUTTON_USE; }
            } else if (m_bCoopBraceAvail && !m_pLadder && !m_pTurret && !m_pVehicle) {
                m_bCoopBraceMounted = true;
                if (current_ucmd) { current_ucmd->buttons &= ~BUTTON_USE; }
                // [user 2026-08-27] latch the direction the weapon was set down facing. The client
                // clamps the aim to a cone around it - you pivot ON the rest instead of turning
                // freely, which is what makes mounting a decision rather than a free buff: you trade
                // your field of view for the stability, and being flanked while mounted costs you.
                // [bug-2133] publish the arc centre in the USERCMD frame, which is the frame the
                // client clamps in. ps.viewangles carries delta_angles (spawn facing, script view
                // nudges, recoil) on top of the command angles, so centring on it yanked the aim by
                // that offset the instant you mounted and locked the cone somewhere you never aimed.
                m_fCoopBraceYaw = client->cmd_angles[YAW];
                // stand cover down the moment the mount is taken - one pose owns the body at a time
                m_bCoopCoverRequested = false;
                m_bCoopCoverWall      = false;
                m_bCoopCoverLow       = false;
                m_bCoopBlindfire      = false;
                m_bCoopCoverPeek      = false;
                SendCoopCoverView();
            }
        }
        m_bCoopBraceUsePrev = bUseNow;
    }
    if (!m_bCoopBraceAvail) {
        m_bCoopBraceMounted = false; // walked away, or the surface moved
    }
    fTarget = m_bCoopBraceMounted ? 1.0f : 0.0f;

    // rise faster than it falls - the gun settles onto the surface and leaves it reluctantly
    fStep = dtBrace * ((fTarget > m_fCoopBrace) ? 6.0f : 3.0f);
    if (m_fCoopBrace < fTarget) {
        m_fCoopBrace += fStep;
        if (m_fCoopBrace > fTarget) { m_fCoopBrace = fTarget; }
    } else if (m_fCoopBrace > fTarget) {
        m_fCoopBrace -= fStep;
        if (m_fCoopBrace < fTarget) { m_fCoopBrace = fTarget; }
    }
    // [bug-2133] hard bound: the ease alone cannot rescue an out-of-range value, and everything
    // downstream multiplies by this.
    if (m_fCoopBrace < 0.0f)      { m_fCoopBrace = 0.0f; }
    else if (m_fCoopBrace > 1.0f) { m_fCoopBrace = 1.0f; }

    // --- publish to our own client, change-only ------------------------------------------------
    // Every pm_flags bit is allocated, so the envelope rides the same change-only stufftext channel
    // cover uses; `set coop_*` is auto-allowed by the servercmd filter, so nothing else changes.
    {
        // [user 2026-08-28, bug] "using the brace feature doesnt actually put you down ads, just sorta
        // zooms into the gun". Two defects met here.
        //
        // (1) THE MOUNT STATE IS BINARY; ONLY THE VISUAL IS AN ENVELOPE. The client decided "am I
        // aiming?" by testing this eased envelope against 0.5, so the ADS pose did not even BEGIN
        // until the brace was half risen - and then eased again on its own curve. Three serial eases
        // (server envelope -> client envelope -> ADS factor) put the sight picture far behind the FOV,
        // which is what reads as a zoom rather than an aim. Publish the mount as a flag, let the ONE
        // tuned ADS envelope do the smoothing, exactly as this system's own comment intended.
        //
        // (2) THE ENVELOPE PUBLISH WAS A MESSAGE STORM. At 1/100 granularity the value changed on
        // essentially every frame of the rise, so "change-only" sent ~100 server commands per mount.
        // 5% steps carry the pip and the recoil portrayal just as well for a twentieth of the traffic.
        int iSend  = ((int)(m_fCoopBrace * 100.0f + 0.5f) / 5) * 5;
        int iAvail = (m_bCoopBraceAvail && !m_bCoopBraceMounted) ? 1 : 0;
        int iMnt   = m_bCoopBraceMounted ? 1 : 0;

        // [user 2026-08-28] TIME OF DAY REPLICATES. coop_daylight is consumed by the CLIENT grade, so
        // a script setcvar reaches nobody on a dedicated server - listen-only is a defect by standing
        // rule. Published change-only here; `set coop_*` is auto-allowed by the servercmd filter, so
        // this needs no whitelist change. Quantised to 1% because the value is a slow scripted ramp
        // and a raw float would send a command every frame of it.
        {
            static cvar_t *pDay = NULL;
            int            iDay;
            if (!pDay) { pDay = gi.Cvar_Get("coop_daylight", "1", 0); }
            iDay = (int)(pDay->value * 100.0f + 0.5f);
            if (iDay < 0)        { iDay = 0; }
            else if (iDay > 100) { iDay = 100; }
            if (iDay != m_iCoopDaylightSent) {
                m_iCoopDaylightSent = iDay;
                gi.SendServerCommand(edict - g_entities,
                                     "stufftext \"set coop_daylight %g\"", iDay / 100.0f);
            }
        }

        if (iMnt != m_iCoopBraceMountedSent) {
            m_iCoopBraceMountedSent = iMnt;
            gi.SendServerCommand(edict - g_entities, "stufftext \"set coop_braceMounted %d\"", iMnt);
        }
        if (iSend != m_iCoopBraceSent) {
            m_iCoopBraceSent = iSend;
            gi.SendServerCommand(edict - g_entities, "stufftext \"set coop_braceView %d\"", iSend);
        }
        if (iAvail != m_iCoopBraceAvailSent) {
            m_iCoopBraceAvailSent = iAvail;
            gi.SendServerCommand(edict - g_entities, "stufftext \"set coop_braceAvail %d\"",
                                 iAvail);
        }
        {
            int iRest = (int)m_fCoopBraceRest;
            if (m_bCoopBraceMounted && iRest != m_iCoopBraceRestSent) {
                m_iCoopBraceRestSent = iRest;
                gi.SendServerCommand(edict - g_entities, "stufftext \"set coop_braceRest %d\"", iRest);
            }
        }
        {
            int iYaw = (int)AngleMod(m_fCoopBraceYaw);
            if (m_bCoopBraceMounted && iYaw != m_iCoopBraceYawSent) {
                m_iCoopBraceYawSent = iYaw;
                gi.SendServerCommand(edict - g_entities, "stufftext \"set coop_braceYaw %d\"",
                                     iYaw);
            }
        }
    }

    if (pDbg->integer) {
        gi.DPrintf("^~^~^ BRACE env=%.2f geom=%d avail=%d mounted=%d still=%d lean=%.1f\n",
                   m_fCoopBrace, (int)bGeom, (int)m_bCoopBraceAvail,
                   (int)m_bCoopBraceMounted, (int)m_bCoopBraceStill,
                   client->ps.fLeanAngle);
    }
}

void Player::ApplyCoopBoneOffsets()
{
    static cvar_t *pProneSpine = NULL, *pSplit = NULL, *pDbg = NULL, *pAct = NULL;
    static int     s_last      = 0;
    qboolean       bProne;

    if (!pProneSpine) { pProneSpine = gi.Cvar_Get("coop_proneSpine", "-5", CVAR_ARCHIVE); } // user-tuned 2026-08-25
    if (!pSplit)      { pSplit      = gi.Cvar_Get("coop_proneSpineSplit", "0.6", CVAR_ARCHIVE); }
    if (!pDbg)        { pDbg        = gi.Cvar_Get("coop_boneDebug", "0", 0); }
    if (!pAct)        { pAct        = gi.Cvar_Get("coop_proneSpineAction", "35", CVAR_ARCHIVE); } // user-tuned 2026-08-25

    // GATE ON BOTH, and PMF_VIEW_PRONE alone is NOT enough. That flag is OVERLOADED: MOVECONTROL_CROUCH
    // - the scripted-crouch state used by set pieces like the m3l1a landing craft - also raises it
    // (player.cpp:4690), so gating on it alone bent the spine during scripted crouch scenes. Caught by
    // measurement, not by reading: a forced-stress run on m3l1a printed raw exactly 0.70x its expected
    // value (the prone discount) while the prone probe reported prone=0 on every line.
    // m_bCoopProne is the coop decision; PMF_VIEW_PRONE confirms the hull actually got there.
    bProne = (qboolean)(m_bCoopProne && (client->ps.pm_flags & PMF_VIEW_PRONE));

    // coop_boneDebug 2 FORCES the prone branch on a standing player. This exists to separate two
    // failures that look identical from the outside: 'the prone spine bias is wrong' and 'prone
    // never engaged'. Testing them together is how this feature already burned a session.
    if (pDbg->integer >= 2) { bProne = qtrue; }

    {
        Vector vHead  = Vector(edict->s.bone_angles[HEAD_TAG]);
        Vector vTorso = Vector(edict->s.bone_angles[TORSO_TAG]);
        Vector vArms  = Vector(edict->s.bone_angles[ARMS_TAG]);

        // SECOND TIER, scaled by the torso action blend. The constant tier alone cannot be right in both
        // states: at rest the LEGS animation drives the spine and it is already lying down, so only a few
        // degrees of trim are wanted - but the moment a torso ACTION plays (reload, aim), that action
        // poses the spine from a STANDING animation and the correction needed jumps to tens of degrees.
        // edict->s.actionWeight is the engine's own 0..1 blend for exactly that slot (animate.cpp:762),
        // so it is 0 while the legs drive and ramps to 1 as the action takes the torso over.
        if (bProne && (pProneSpine->value != 0.0f || pAct->value != 0.0f)) {
            // THIRD TIER, RELOAD ONLY.
            //
            // One coefficient cannot serve every torso action. At coop_proneSpineAction 35 the user
            // reported shooting fixed and reloading still wrong: 'this only impacts shooting (half of
            // the problem) reloading is still messed up and his torso comes up to reload'. That is what
            // you would expect - a reload animation lifts the chest to work the bolt and seat a clip,
            // a far larger deviation from the prone pose than a firing animation, so it needs a bigger
            // correction.
            //
            // actionWeight CANNOT separate them: it is one blend for the whole torso slot, and on the
            // PLAYER it never even gates, because ANIM_NOACTION is set only on actors (actor.cpp,
            // simpleactor.cpp) and never here - so hasAction is true whenever any slot has weight.
            //
            // weaponstate is the honest discriminator. WEAPON_RELOADING covers exactly the window the
            // reload animation owns the torso, so the extra bend begins and ends with the animation
            // instead of being timed by hand.
            // RELOAD REPLACES THE ACTION TIER RATHER THAN STACKING ON IT. Since 2026-08-25 a prone
            // reload plays a real body-space animation (coop_prone_reload -> pistol_prone_reload), so the
            // chest is ALREADY down and the +35 correction meant for a standing firing pose would bend it
            // through the floor. coop_proneSpineReload therefore defaults to 0: trim only if the borrowed
            // pistol animation sits slightly wrong for a longer weapon.
            // [user 2026-08-26] The reload branch is GONE. It was written for the swapped-animation
            // world that has since been reverted, and it left a prone reload with LESS spine correction
            // than a prone shot (coop_proneSpineReload defaults 0, replacing the 35 action tier) - which
            // was actively making the observed rise worse. The torso now carries zero render weight
            // through a prone reload (player_animation.cpp), so there is nothing there to correct.
            float fBias = pProneSpine->value;
            if (!m_bCoopSupine) { // [spec A2a] the action tier un-stands STANDING anims; flipped
                                  // supine anims are already correct and it acts inverted there
                fBias += pAct->value * Q_clamp_float(edict->s.actionWeight, 0.0f, 1.0f);
            }
            float fLower = Q_clamp_float(pSplit->value, 0.0f, 1.0f);

            vArms[0]  += fBias * fLower;          // Bip01 Spine1 - lower spine carries most of the bend
            vTorso[0] += fBias * (1.0f - fLower); // Bip01 Spine2
            vHead[0]  -= fBias;                   // cancel the inherited bend so the head stays level
        }

        // [user 2026-08-26] P1 AIM-LEAD: while the prone body yaw is still catching up, the head and
        // upper spine lean toward the aim - the body follows the eyes, which is most of what reads
        // as MP3's fluidity. Additive on the eased base, inside the AI head caps.
        // [spec A2] SUPINE BONE FRAME. The 180 model-X roll reverses every bone's local Y and Z,
        // so pmove's view-pitch spine distribution and our additive tiers all act INVERTED on a
        // flipped body - aiming at your feet arched the chest INTO the ground, and the +35 action
        // tier (tuned to un-stand a standing fire anim) bent the already-correct flipped fire pose
        // the wrong way. While supine: negate the inherited pitch on all three spine bones, drop
        // the action tier entirely, and re-base the aim-lead on view+180 (the raw delta is +/-180
        // when settled - it clamped to +/-50 and FLAPPED sign as the aim wobbled across the
        // boundary: the head snapped +/-27 degrees).
        if (bProne && m_bCoopSupine) {
            static cvar_t *pSupSpine = NULL;
            if (!pSupSpine) { pSupSpine = gi.Cvar_Get("coop_supineSpine", "0", CVAR_ARCHIVE); }
            vHead[0]  = -vHead[0]  + pSupSpine->value;
            vTorso[0] = -vTorso[0];
            vArms[0]  = -vArms[0];
        }
        if (bProne) {
            static cvar_t *pLead = NULL;
            if (!pLead) { pLead = gi.Cvar_Get("coop_proneAimLead", "1", CVAR_ARCHIVE); }
            if (pLead->integer) {
                // [supine-yaw] the +180 re-base went with the old view+180 target - the body
                // tracks the view in both stances now, so the lead is measured the same way.
                float fBase = client->ps.viewangles[YAW];
                float fLag  = AngleSubtract(fBase, m_fCoopProneBodyYaw);
                fLag = Q_clamp_float(fLag, -50.0f, 50.0f);
                if (m_bCoopSupine) { fLag = -fLag; } // inverted local frame on the flipped body
                vHead[1]  += fLag * 0.55f;
                vTorso[1] += fLag * 0.35f;
            }
        }
        // [review F5] the flipped body's local yaw axis is reversed (the A2 rule), so every yaw
        // term needs the mirror, not just the aim-lead - the torso-lag (up to +/-18 deg on fast
        // sweeps, and sweeping while ADS-ing is precisely the supine activity) was counter-rotating
        // the wrong way and fighting the correctly-mirrored aim-lead on the same bone.
        // HZM coop [user 2026-08-27] THE SHAKE OTHER PLAYERS SEE. First person has a rich weapon-feel
        // layer - stress sway, breathing, the settle after a shot - and none of it existed in third
        // person, so a rattled soldier looked identical to a calm one to everybody else. Same stress
        // scalar, expressed through the two bone controllers that carry the weapon (TORSO and ARMS)
        // so it reaches every observer through the networked bone angles with no client change.
        //
        // Deliberately SMALL and slow: this is a body under strain, not a vibrating prop. Bracing
        // calms it by the same factor it calms the first-person sway, so a mounted gun visibly steadies
        // to your teammates too - which is the whole point of showing it at all.
        {
            static cvar_t *pShake = NULL;
            float          fAmp;

            if (!pShake) { pShake = gi.Cvar_Get("coop_shake3p", "1.7", CVAR_ARCHIVE); }
            fAmp = m_fCoopStress * pShake->value;
            if (m_fCoopBrace > 0.0f) {
                static cvar_t *pBS2 = NULL;
                if (!pBS2) { pBS2 = gi.Cvar_Get("coop_braceStress", "0.50", CVAR_ARCHIVE); }
                fAmp *= 1.0f - CoopBraceBonus() * (pBS2 ? pBS2->value : 0.50f);
            }
            if (fAmp > 0.02f) {
                float t = level.time;
                vTorso[1] += (float)sin(t * 7.3f) * fAmp * 0.6f;
                vTorso[0] += (float)sin(t * 5.1f + 1.1f) * fAmp * 0.4f;
                vArms[1]  += (float)sin(t * 9.7f + 0.4f) * fAmp;
                vArms[0]  += (float)sin(t * 6.3f + 2.2f) * fAmp * 0.7f;
            }
        }

        {
            float fMir = m_bCoopSupine ? -1.0f : 1.0f;
            vHead[0]  += m_fCoopHeadPitch;
            vHead[1]  += m_fCoopHeadYaw * fMir;
            vTorso[1] += m_fCoopTorsoLag * fMir;
        }

        // SetControllerAngles re-derives bone_quat, which the SERVER skeleton uses for tag positions
        // (g_main.cpp:915). bone_quat is not networked - msg.cpp:3115 rebuilds it from bone_angles.
        // [pass3, bug-2126] pmove distributes a view-pitch share into PELVIS too (bg_pmove
        // PmoveAdjustAngleSettings: 0.3*pitch when looking down, ~11 deg at 60) - the same
        // inverted-local-frame rule as the three bones above, so while supine it arched the
        // hips INTO the floor. Mirror the pitch only; roll is about the flip axis and stays.
        // Written every frame this block runs - the same lifecycle as the other three tags.
        // [v3] while the arms are reversed these two controllers are pointing at the CLAVICLES, not
        // the spine and pelvis, so the spine-mirror maths below does not apply to them - they carry
        // one job, the 180 that turns the arms around.
        if (m_bCoopSupineArmsOn) {
            static cvar_t *pArmAng = NULL;
            Vector         vClav;

            if (!pArmAng) { pArmAng = gi.Cvar_Get("coop_supineArmsAng", "180", CVAR_ARCHIVE); }
            vClav = Vector(pArmAng->value, 0.0f, 0.0f);
            SetControllerAngles(ARMS_TAG, vClav);
            SetControllerAngles(PELVIS_TAG, vClav);
        } else {
            Vector vPelvis = Vector(edict->s.bone_angles[PELVIS_TAG]);
            if (bProne && m_bCoopSupine) {
                vPelvis[0] = -vPelvis[0];
            }
            SetControllerAngles(PELVIS_TAG, vPelvis);
        }
        SetControllerAngles(HEAD_TAG, vHead);
        SetControllerAngles(TORSO_TAG, vTorso);
        if (!m_bCoopSupineArmsOn) {
            SetControllerAngles(ARMS_TAG, vArms);
        }

        if (pDbg->integer && level.inttime - s_last > 500) {
            s_last = level.inttime;
            gi.Printf("^~^~^ BONEOFF prone=%d bias=%.0f aw=%.2f rld=%d | head=%.1f/%.1f torso=%.1f/%.1f arms=%.1f\n",
                      (int)bProne, pProneSpine->value, edict->s.actionWeight,
                      (GetActiveWeapon(WEAPON_MAIN)
                       && GetActiveWeapon(WEAPON_MAIN)->GetState() == WEAPON_RELOADING) ? 1 : 0,
                      edict->s.bone_angles[HEAD_TAG][0], edict->s.bone_angles[HEAD_TAG][1],
                      edict->s.bone_angles[TORSO_TAG][0], edict->s.bone_angles[TORSO_TAG][1],
                      edict->s.bone_angles[ARMS_TAG][0]);
        }
    }
}

// HZM coop [user 2026-08-24] HEAD TRACKING + TORSO COUNTER-ROTATION.
//
// Both write bone controllers the player ALREADY registers and then never uses. player.cpp claims
// HEAD_TAG, TORSO_TAG, ARMS_TAG and PELVIS_TAG at spawn, but the only SetControllerAngles call
// anywhere in Player is MOUTH_TAG for lipsync - so HEAD and TORSO are registered, wired through to
// the renderer, and sitting at zero. That is the whole reason this is cheap: NUM_BONE_CONTROLLERS is
// 5, hardcoded, raising it means editing the exe and breaking the protocol, and four slots are
// already claimed (bug-2013 - the finger system got exactly one and the left hand silently got none).
// Driving an idle slot costs nothing.
//
// Controller angles are OFFSETS added on top of the animated pose, and they live in
// edict->s.bone_angles[] which is entityState - so this networks, and other players see it.
//
// Conventions copied from the AI head-aim (actor.cpp:3806-3846) rather than invented: [0]=pitch,
// [1]=yaw, [2]=roll and always zero, yaw clamped +/-60, pitch +/-35, and the change per frame
// rate-limited so the head turns rather than snapping.
void Player::TickCoopLook()
{
    static cvar_t *pHead = NULL, *pHeadRange = NULL, *pTorso = NULL, *pTorsoAmt = NULL;
    Vector         vBodyFwd, vToTarget;
    float          fWantYaw = 0.0f, fWantPitch = 0.0f;
    float          dt       = level.frametime;
    qboolean       bAlive;

    if (!pHead)      { pHead      = gi.Cvar_Get("coop_headLook", "0", CVAR_ARCHIVE); } // [user 2026-08-25] off - the head following the view reads better than glancing at contacts
    if (!pHeadRange) { pHeadRange = gi.Cvar_Get("coop_headLookRange", "1400", CVAR_ARCHIVE); }
    if (!pTorso)     { pTorso     = gi.Cvar_Get("coop_torsoLag", "1", CVAR_ARCHIVE); }
    if (!pTorsoAmt)  { pTorsoAmt  = gi.Cvar_Get("coop_torsoLagAmount", "0.35", CVAR_ARCHIVE); }

    if (dt <= 0.0f) {
        return; // a zero-length frame must not be integrated - it would divide by nothing below
    }

    bAlive = (qboolean)(!deadflag && !m_pVehicle && !m_pTurret
                        && !(client->ps.pm_flags & (PMF_SPECTATING | PMF_INTERMISSION | PMF_FROZEN)));

    // ---- HEAD: glance toward the nearest visible German -----------------------------------
    // Suppressed while aiming: down the sights you look exactly where you aim, and a head that
    // wanders there reads as a bug rather than as life.
    if (bAlive && pHead->integer && !(last_ucmd.buttons & BUTTON_COOPADS)) {
        Sentient *best     = NULL;
        float     bestDist = pHeadRange->value * pHeadRange->value;

        for (Sentient *obj = level.m_HeadSentient[TEAM_GERMAN]; obj != NULL; obj = obj->m_NextSentient) {
            float d;

            if (obj == this || obj->health <= 0 || obj->deadflag) {
                continue;
            }
            d = (obj->centroid - origin).lengthSquared();
            if (d >= bestDist) {
                continue;
            }
            if (!CanSee(obj, 200.0f, pHeadRange->value, false)) {
                continue; // 200 deg cone: you can glance at something well off to the side
            }
            best     = obj;
            bestDist = d;
        }

        // [user 2026-08-24] "i dont notice my head moving with free cam". The mechanism was fine - the
        // TRIGGER was too narrow. Nearest-visible-GERMAN-within-35m means the head does nothing at all
        // in the situation you are most likely to be looking at yourself in freecam: standing around
        // with no enemy in view. Fall back to a visible TEAMMATE, which is also just better behaviour -
        // soldiers look at each other.
        if (!best) {
            float bestMate = 900.0f * 900.0f;
            for (Sentient *obj = level.m_HeadSentient[TEAM_AMERICAN]; obj != NULL; obj = obj->m_NextSentient) {
                float d;

                if (obj == this || obj->health <= 0 || obj->deadflag) {
                    continue;
                }
                d = (obj->centroid - origin).lengthSquared();
                if (d >= bestMate) {
                    continue;
                }
                if (!CanSee(obj, 200.0f, 900.0f, false)) {
                    continue;
                }
                best     = obj;
                bestMate = d;
            }
        }

        if (best) {
            Vector vDelta = best->centroid - (origin + Vector(0, 0, viewheight));
            Vector vWant  = vDelta.toAngles();

            // relative to where the BODY faces, not the world
            fWantYaw   = AngleSubtract(vWant[YAW], angles[YAW]);
            fWantPitch = AngleSubtract(vWant[PITCH], 0.0f);
            if (fWantPitch > 180.0f) { fWantPitch -= 360.0f; }
        }
    }

    // clamps identical to the AI head so a player head can never out-turn a German one
    fWantYaw   = Q_clamp_float(fWantYaw, -60.0f, 60.0f);
    fWantPitch = Q_clamp_float(fWantPitch, -35.0f, 35.0f);

    {
        float fMaxStep = 220.0f * dt; // degrees/sec - a look, not a snap
        float dY       = Q_clamp_float(fWantYaw - m_fCoopHeadYaw, -fMaxStep, fMaxStep);
        float dP       = Q_clamp_float(fWantPitch - m_fCoopHeadPitch, -fMaxStep, fMaxStep);

        m_fCoopHeadYaw += dY;
        m_fCoopHeadPitch += dP;
    }

    {
        static cvar_t *pHDbg = NULL;
        static int     s_hLast = 0;
        if (!pHDbg) { pHDbg = gi.Cvar_Get("coop_headLookDebug", "0", 0); } // verified live 2026-08-25 (bug-2101)
        if (pHDbg->integer && level.inttime - s_hLast > 500) {
            s_hLast = level.inttime;
            // READBACK: what is in the slot BEFORE we write it - i.e. whoever wrote it LAST frame.
            // PmoveAdjustAngleSettings (bg_pmove.cpp:1698) writes HEAD_TAG with VectorCopy from
            // Player::EndFrame, which runs AFTER ClientThink - so if it is reclaiming the slot, the
            // readback yaw will be pinned at 0.00 (pmove's head write has [1]=0 in normal play)
            // no matter what we put there. Non-zero readback yaw = our write survived.
            gi.Printf("^~^~^ HEADLOOK want=%.0f/%.0f cur=%.0f/%.0f ads=%d | readback head=%.2f/%.2f torso=%.2f/%.2f\n",
                      fWantYaw, fWantPitch, m_fCoopHeadYaw, m_fCoopHeadPitch,
                      (int)((last_ucmd.buttons & BUTTON_COOPADS) ? 1 : 0),
                      edict->s.bone_angles[HEAD_TAG][0], edict->s.bone_angles[HEAD_TAG][1],
                      edict->s.bone_angles[TORSO_TAG][0], edict->s.bone_angles[TORSO_TAG][1]);
        }
        Vector vHead(m_fCoopHeadPitch, m_fCoopHeadYaw, 0.0f);
        SetControllerAngles(HEAD_TAG, vHead);
    }

    // ---- TORSO: lag behind a fast turn, then catch up --------------------------------------
    // The body currently pivots rigidly with the camera. Counter-rotating Spine2 against the turn
    // rate makes the hips lead and the chest follow, which is what reads as weight. Spine2 sits
    // directly above ARMS_TAG (Spine1), which already carries view pitch - so the two compose
    // instead of fighting: pitch on the lower spine, yaw lag on the upper.
    {
        float fYawRate = AngleSubtract(client->ps.viewangles[YAW], m_fCoopPrevViewYaw) / dt;
        float fTarget  = 0.0f;

        m_fCoopPrevViewYaw = client->ps.viewangles[YAW];

        if (bAlive && pTorso->integer) {
            fTarget = Q_clamp_float(-fYawRate * pTorsoAmt->value * 0.02f, -18.0f, 18.0f);
        }
        // one-pole toward the target: builds while turning, bleeds off when you stop
        {
            float k = dt * 9.0f;
            if (k > 1.0f) { k = 1.0f; }
            // [weight 3] a man carrying a Panzerschreck should turn like one. This lag is written to
            // a networked bone channel, so unlike every other weight cue it is visible to TEAMMATES -
            // the cheapest way to put mass on screen for somebody other than the person holding it.
            {
                static cvar_t *pTL = NULL;
                if (!pTL) { pTL = gi.Cvar_Get("coop_heftTorsoLag", "0.9", CVAR_ARCHIVE); }
                fTarget *= 1.0f + CoopActiveHeft() * pTL->value;
            }
            m_fCoopTorsoLag += (fTarget - m_fCoopTorsoLag) * k;
            if (m_fCoopTorsoLag < 0.02f && m_fCoopTorsoLag > -0.02f) { m_fCoopTorsoLag = 0.0f; }
        }
        {
            Vector vTorso(0.0f, m_fCoopTorsoLag, 0.0f); // pitch/roll cleared, as the AI torso does
            SetControllerAngles(TORSO_TAG, vTorso);
        }
    }

}

// HZM coop [user 2026-08-26] DIRECT GRENADE THROW - no weapon switch.
//
// The old quick-grenade was a scripted WEAPON SWAP: UseWeaponClass grenade, wait for the takeout
// animation to finish (ReadyToFire/MuzzleClear both gate on it), charge, throw, UseLastWeapon 0.9 s
// later. Four weapon-system operations where a modern shooter has none, and the wait is what the user
// saw: 'it actually equips the grenade first and then throws versus just throwing'.
//
// THE RECIPE IS NOT INVENTED. Every AI grenade in the game already throws this way - actor.cpp:10877
// computes a throw point and velocity, calls ProjectileAttack with a projectile tik, and decrements
// ammo. No weapon is ever made active. That is exactly what is wanted here, so it is copied rather than
// designed: same call, same ammo name, same per-team projectile choice.
//
// SCOPE, stated honestly: this is the WORLD half. The projectile, the ammo and the arc are correct and
// authoritative. The first-person viewmodel is driven by cg.snap->ps.activeItems[1] - the ACTIVE weapon
// - so with no swap the viewmodel keeps showing the current gun. The throw animations exist
// (viewmodel/vm_grenadeload.skc, vm_grenaderelease.skc) but hooking them without an active grenade is a
// separate client-side job. Third person is correct now; first person shows the grenade leave without a
// hand animation until that lands.
void Player::CoopDirectThrow()
{
    static cvar_t *pSpeed = NULL, *pMin = NULL, *pUp = NULL;
    Vector         vPos, vDir, vAng;
    float          fCharge, fSpeed;
    str            sGrenade;

    if (!pSpeed) { pSpeed = gi.Cvar_Get("coop_nadeThrowSpeed", "900", CVAR_ARCHIVE); }
    if (!pMin)   { pMin   = gi.Cvar_Get("coop_nadeThrowMin", "420", CVAR_ARCHIVE); }
    if (!pUp)    { pUp    = gi.Cvar_Get("coop_nadeThrowUp", "9", CVAR_ARCHIVE); }

    if (AmmoCount("grenade") < 1) {
        return;
    }

    // charge -> distance, the same hold-to-throw-further the weapon path gave for free
    fCharge = level.time - m_fCoopNadeT0;
    if (fCharge < 0.0f) { fCharge = 0.0f; } else if (fCharge > 1.5f) { fCharge = 1.5f; }
    fSpeed = pMin->value + (pSpeed->value - pMin->value) * (fCharge / 1.5f);

    // throw from the eye, along the view, pitched up a little so a level throw still arcs
    vAng    = client->ps.viewangles;
    vAng[0] -= pUp->value;
    vAng.AngleVectors(&vDir);
    vDir.normalize();
    vPos = origin + Vector(0, 0, (float)viewheight) + vDir * 16.0f;

    // same per-team choice the actor code makes, so the model matches the thrower
    sGrenade = (m_Team == TEAM_GERMAN) ? "models/projectiles/steilhandgranate.tik"
                                       : "models/projectiles/M2FGrenade.tik";

    ProjectileAttack(vPos, vDir, this, sGrenade, 0, fSpeed);
    UseAmmo("grenade", 1);
}

// HZM coop [user 2026-08-24] QUICK GRENADE. bind g "+coopnade"
//
// Press: select a grenade and start charging it. Hold: charge further and cook. Release: throw,
// then go back to the gun you were holding.
//
// EVERYTHING THE FEATURE DOES IS ALREADY IN THE ENGINE and this deliberately adds no new physics:
//   * distance   - Sentient::ReleaseFireWeapon computes charge_time and Weapon::ReleaseFire turns
//                  it into charge_fraction (weapon.cpp:2948), clamped by the tik's own
//                  min/max_charge_time. Per-weapon tuning therefore already exists, in the data.
//   * cooking    - Weapon::Charge arms EV_OverCooked / EV_OverCooked_Warning, and ReleaseFire
//                  cancels them. Hold too long and it goes off in your hand, which is stock.
//   * restoring  - EV_Sentient_UseLastWeapon + the engine's own lastActiveWeapon.
// So this is a SEQUENCER, not a new mechanic - the project's rule is to find the working recipe
// and copy it rather than invent, and here the recipe was already complete.
//
// WHY A CONSOLE COMMAND rather than a usercmd button bit: there are none left. Bits 0-6 are stock,
// 7-11 are the weapon-command field (GetWeaponCommandMask), 12 is COOPWALK, 13 was taken by
// COOPADS as 'the last free one', 14-15 are ANY/MOUSE. The +/- bind convention is intact in
// cl_keys.cpp (press builds '+cmd key time' at :1266, release builds '-cmd key time' at :1078) and
// unmatched commands forward to the server, so a console command gives a genuine held state with
// no protocol change at all. G_ConsoleCmds dispatch ignores the trailing key/time args.
void Player::TickCoopNade()
{
    static cvar_t *pOn = NULL;
    Weapon        *pW;

    if (!pOn) { pOn = gi.Cvar_Get("coop_quickNade", "1", CVAR_ARCHIVE); }

    if (!m_iCoopNadeState) {
        return;
    }

    // Abort on anything that makes throwing meaningless. Deliberately generous: leaving this state
    // stuck would leave the player holding a charging grenade with no key to release it.
    if (!pOn->integer || deadflag || m_pVehicle || m_pTurret
        || (client->ps.pm_flags & (PMF_SPECTATING | PMF_INTERMISSION | PMF_FROZEN))) {
        if (charge_start_time) {
            ReleaseFireWeapon(WEAPON_MAIN, FIRE_PRIMARY); // never leave one cooking in the hand
        }
        m_iCoopNadeState = 0;
        m_bCoopNadeHeld  = false;
        return;
    }

    pW = GetActiveWeapon(WEAPON_MAIN);

    // ---- 1: waiting for the grenade to come up -------------------------------------------
    if (m_iCoopNadeState == 1) {
        // Timeout. A player with no grenades, or mid-reload, would otherwise sit here forever.
        if (level.time - m_fCoopNadeT0 > 2.5f) {
            m_iCoopNadeState = 0;
            m_bCoopNadeHeld  = false;
            return;
        }
        if (pW && (pW->GetWeaponClass() & WEAPON_CLASS_GRENADE)) {
            // HZM coop [user 2026-08-26] INSTANT PRESENT. The whole 'it equips first and then throws'
            // delay lives in ReadyToFire, which stays false until weaponstate reaches WEAPON_READY -
            // i.e. until the TAKEOUT ANIMATION finishes. ForceState just assigns weaponstate
            // (weapon.cpp:5176) and MuzzleClear is a hard `return qtrue` (:5185), so skipping the wait
            // costs nothing except the pullout animation, which is precisely what we do not want to
            // watch. The grenade IS still genuinely equipped, which is why first person stays correct:
            // the weapon model is a server-side attachment to tag_weapon_right (weapon.cpp:3236), and
            // no client-only trick can put a grenade in your hand without one.
            static cvar_t *pInst = NULL;
            if (!pInst) { pInst = gi.Cvar_Get("coop_quickNadeInstant", "1", CVAR_ARCHIVE); }
            if (pInst->integer && pW->GetState() != WEAPON_READY) {
                pW->ForceState(WEAPON_READY);
            }
            if (pW->ReadyToFire(FIRE_PRIMARY) && pW->MuzzleClear()) {
                ChargeWeapon(WEAPON_MAIN, FIRE_PRIMARY);
                m_iCoopNadeState = 2;

                // [user 2026-08-26] THROW ON PRESS. "hitting G equips the grenade, I was hoping it
                // would prime the grenade to be thrown ... basically skipping the step of just holding
                // it in our hand." Waiting for the key to come up is what produced that dwell: the
                // grenade is genuinely equipped for as long as you hold, and it has to be, because the
                // model in your hand is a server-side attachment (weapon.cpp:3236). Clearing the held
                // flag here releases it on the SAME tick it charges, so pullout and throw run as one
                // motion and the gun comes straight back.
                //
                // coop_quickNadeCook 1 restores hold-to-cook for anyone who wants to bake one.
                {
                    static cvar_t *pCook = NULL;
                    if (!pCook) { pCook = gi.Cvar_Get("coop_quickNadeCook", "0", CVAR_ARCHIVE); }
                    if (!pCook->integer) {
                        m_bCoopNadeHeld = false;
                    }
                }
            }
        }
        return;
    }

    // ---- 2: charging ----------------------------------------------------------------------
    if (m_iCoopNadeState == 2) {
        // The weapon can be taken out from under us - overcook detonates it, or a script strips it.
        if (!pW || !(pW->GetWeaponClass() & WEAPON_CLASS_GRENADE)) {
            m_iCoopNadeState = 3;
            m_fCoopNadeThrow = level.time;
            return;
        }
        if (!m_bCoopNadeHeld) {
            ReleaseFireWeapon(WEAPON_MAIN, FIRE_PRIMARY);
            m_iCoopNadeState = 3;
            m_fCoopNadeThrow = level.time;
        }
        return;
    }

    // ---- 3: restore the previous weapon ---------------------------------------------------
    // Delayed, not immediate: switching on the same frame as the throw cuts the throw animation
    // and (worse) can pull the weapon before the projectile is actually released.
    if (m_iCoopNadeState == 4) {
        // direct-throw hold. Release throws; a hard cap stops a forgotten key cooking forever.
        if (!m_bCoopNadeHeld || level.time - m_fCoopNadeT0 > 1.5f) {
            CoopDirectThrow();
            m_iCoopNadeState = 0;
            m_bCoopNadeHeld  = false;
        }
        return;
    }

    if (m_iCoopNadeState == 3) {
        static cvar_t *pBack = NULL;
        if (!pBack) { pBack = gi.Cvar_Get("coop_quickNadeReturn", "0.9", CVAR_ARCHIVE); }

        if (level.time - m_fCoopNadeThrow >= (pBack->value > 0.0f ? pBack->value : 0.9f)) {
            m_iCoopNadeState = 0;
            // Only go back if we are still holding a grenade. If the player has already switched
            // by hand, or is out of grenades and the engine moved him on, respect that.
            pW = GetActiveWeapon(WEAPON_MAIN);
            if (pW && (pW->GetWeaponClass() & WEAPON_CLASS_GRENADE)) {
                ProcessEvent(EV_Sentient_UseLastWeapon);
            }
        }
        return;
    }
}

void Player::CoopNadeDown()
{
    static cvar_t *pOn = NULL;
    Weapon        *pW;

    if (!pOn) { pOn = gi.Cvar_Get("coop_quickNade", "1", CVAR_ARCHIVE); }
    if (!pOn->integer || deadflag || m_pVehicle || m_pTurret
        || (client->ps.pm_flags & (PMF_SPECTATING | PMF_INTERMISSION | PMF_FROZEN))) {
        return;
    }
    if (m_iCoopNadeState) {
        return; // already mid-throw; a second press must not restart the sequence
    }

    m_bCoopNadeHeld = true;
    m_fCoopNadeT0   = level.time;

    // coop_quickNade 2 = DIRECT THROW. No UseWeaponClass, so no putaway, no takeout, no wait, and
    // nothing to switch back from. State 4 just holds the charge until release.
    if (pOn->integer >= 2) {
        m_iCoopNadeState = 4;
        return;
    }

    pW = GetActiveWeapon(WEAPON_MAIN);
    if (pW && (pW->GetWeaponClass() & WEAPON_CLASS_GRENADE)) {
        // already holding one - charge immediately, no switch and no restore surprise
        m_iCoopNadeState = 1; // the tick charges it once ReadyToFire/MuzzleClear agree
        return;
    }

    // Same event the stock WEAPON_COMMAND_USE_GRENADE path posts (player.cpp:5954), so grenade
    // SELECTION is not reimplemented here - inventory order, dual-wield rules and the
    // no-grenades case all behave exactly as they do for the normal weapon key.
    {
        Event *ev = new Event(EV_Sentient_UseWeaponClass);
        ev->AddString("grenade");
        ProcessEvent(ev);
    }
    m_iCoopNadeState = 1;
}

void Player::CoopNadeUp()
{
    // Only records the key state. The throw itself happens in TickCoopNade, so a TAP that is
    // released before the grenade has even finished coming up still throws (at minimum charge)
    // instead of being swallowed - which is what 'quick throw' has to mean.
    m_bCoopNadeHeld = false;
}

// HZM coop [user 2026-08-24] SPRINT-TO-SLIDE.
//
// Sprint, then hold crouch: you keep (and briefly exceed) your speed for coop_slideTime while
// crouched, decaying back to a crouch-walk. Every piece it needs already existed - sprint state,
// stamina, and the crouch the engine already owns.
//
// DELIBERATELY A SPEED CHANGE ONLY, copying the sprint recipe rather than inventing one. No bbox
// surgery, no origin pushes, no forced PMF_DUCKED: the player is genuinely crouched because HOLDING
// CROUCH IS THE TRIGGER, so PM_CheckDuck owns the hull exactly as it does for any other crouch. That
// is what stops this wedging anyone inside geometry - the failure every hand-rolled slide finds.
//
// Costs stamina, so it cannot be chained to cross a map faster than sprinting, and carries its own
// cooldown on top so it cannot be spammed on the spot.
void Player::TickSlide()
{
    static cvar_t *pOn   = NULL;
    static cvar_t *pDur  = NULL;
    static cvar_t *pCool = NULL;
    static cvar_t *pCost = NULL;
    qboolean       bCrouch;

    if (!pOn)   { pOn   = gi.Cvar_Get("coop_slide", "1", CVAR_ARCHIVE); }
    if (!pDur)  { pDur  = gi.Cvar_Get("coop_slideTime", "0.75", CVAR_ARCHIVE); }
    if (!pCool) { pCool = gi.Cvar_Get("coop_slideCooldown", "1.2", CVAR_ARCHIVE); }
    if (!pCost) { pCost = gi.Cvar_Get("coop_slideStamina", "0.55", CVAR_ARCHIVE); }

    // Exit is tested BEFORE entry, so a slide ending this frame cannot also re-enter on the same tick.
    if (m_bCoopSliding) {
        if (m_fCoopSlideEnd <= level.time || deadflag || !groundentity || m_pVehicle || m_pTurret
            || last_ucmd.upmove > 0) { // jump cancels it
            // HZM coop [user 2026-08-26] P3 SLIDE-INTO-PRONE (the shootdodge substitute). If crouch is
            // STILL held as the slide runs out naturally, seed the prone hold accumulator full so
            // TickCoopProne (which runs later this same frame - order is TickSlide then TickCoopProne)
            // enters prone immediately: sprint -> slide -> prone in one continuous hold, no new
            // assets. Natural expiry only - a jump-cancel or death must not put you on the ground.
            if (m_fCoopSlideEnd <= level.time && !deadflag && groundentity && !m_pVehicle && !m_pTurret
                && last_ucmd.upmove < 0) {
                static cvar_t *pS2P = NULL, *pHoldS = NULL;
                if (!pS2P)   { pS2P   = gi.Cvar_Get("coop_slideToProne", "1", CVAR_ARCHIVE); }
                if (!pHoldS) { pHoldS = gi.Cvar_Get("coop_proneHold", "0.35", CVAR_ARCHIVE); }
                if (pS2P->integer) {
                    m_fCoopCrouchHeld = level.time - pHoldS->value; // timestamp model: already satisfied
                }
            }
            m_bCoopSliding  = false;
            m_fCoopSlideEnd = 0;
        }
        return;
    }

    // [pass2, bug-2125] mirror of bCanEnter's !m_bCoopSliding: a crouch press during a shift-held
    // crawl entered a SLIDE on the same frame the prone exit processed the press.
    if (m_bCoopProne) { return; }
    if (!pOn->integer) {
        m_bCoopSliding = false;
        return;
    }

    bCrouch = (last_ucmd.upmove < 0) ? qtrue : qfalse;

    // m_bCoopSprinting is THIS frame's value because TickSlide runs after TickSprint. If that order is
    // ever reversed this silently reads a stale frame and the slide fires a tick late.
    // Wounded is excluded for the same reason sprint excludes it (bugs 1291/1324): a limping player who
    // could still slide would be faster hurt than healthy.
    if (m_bCoopSprinting && bCrouch && groundentity && !deadflag && !m_pVehicle && !m_pTurret
        && !m_bCoopWounded && level.time >= m_fCoopSlideNext && m_fCoopStamina >= pCost->value) {
        float dur = (pDur->value > 0.05f) ? pDur->value : 0.75f;

        m_bCoopSliding   = true;
        m_fCoopSlideEnd  = level.time + dur;
        m_fCoopSlideNext = level.time + dur + (pCool->value > 0.0f ? pCool->value : 0.0f);
        m_fCoopStamina -= pCost->value;
        if (m_fCoopStamina < 0.0f) { m_fCoopStamina = 0.0f; }
    }
}

void Player::TickSprint()
{
    float timeHeld;

    if (last_ucmd.buttons & BUTTON_RUN && last_ucmd.forwardmove) {
        timeHeld = 0;

        if (!m_fLastSprintTime) {
            m_fLastSprintTime = level.time;
        }
    } else {
        timeHeld          = 0;
        m_fLastSprintTime = 0;
    }

    if (last_ucmd.rightmove) {
        m_fLastSprintTime = timeHeld;
    }
    if (last_ucmd.upmove) {
        m_fLastSprintTime = timeHeld;
    }

    //====
    // HZM coop - SPRINT stamina + state.
    // Decides whether the player is sprinting THIS frame and drains/regens the stamina pool. The actual
    // speed boost is applied in ClientMove (the GetRunSpeed/walk branch). Sprint = the walk key (Shift,
    // BUTTON_RUN clear in default "always run") held while NOT aiming + moving forward + stamina left.
    // While AIMING (ADS / scope) the walk key keeps its existing walk + breath-hold behavior untouched.
    // The dedicated Alt walk key (BUTTON_COOPWALK) always forces a slow walk and never sprints.
    {
        cvar_t *pOn      = gi.Cvar_Get("coop_sprint", "1", CVAR_ARCHIVE);
        cvar_t *pStamina = gi.Cvar_Get("coop_sprintStamina", "5", CVAR_ARCHIVE);
        cvar_t *pRegen   = gi.Cvar_Get("coop_sprintRegen", "0.6", CVAR_ARCHIVE);
        float   maxStam  = pStamina ? pStamina->value : 5.0f;
        float   regen    = pRegen ? pRegen->value : 0.6f;
        float   dt       = level.frametime;
        qboolean enabled = (pOn && pOn->integer) ? qtrue : qfalse;
        qboolean aiming;
        qboolean walkKey;
        qboolean altWalk;
        qboolean wantSprint;

        if (maxStam < 0.1f) { maxStam = 0.1f; }
        if (dt < 0.0f || dt > 0.5f) { dt = 0.0f; } // clamp pauses / map loads

        // clamp the (possibly spawn-seeded huge) pool to the current max
        if (m_fCoopStamina > maxStam) { m_fCoopStamina = maxStam; }

        // [user 2026-08-27] a JUMP costs stamina - it is the cheapest movement in the game and was
        // free, so it undercut every other option. Edge-triggered: holding jump is not a tax.
        {
            static cvar_t *pJC = NULL;
            qboolean       bJumpNow = (last_ucmd.upmove > 0) ? qtrue : qfalse;
            if (!pJC) { pJC = gi.Cvar_Get("coop_staminaJump", "1.4", CVAR_ARCHIVE); }
            if (bJumpNow && !m_bCoopJumpPrev && groundentity && !deadflag && pJC->value > 0.0f) {
                m_fCoopStamina -= pJC->value;
                if (m_fCoopStamina < 0.0f) { m_fCoopStamina = 0.0f; }
                m_fCoopStaminaHold = level.time + CoopStaminaDelay();
            }
            m_bCoopJumpPrev = bJumpNow;
        }

        aiming  = (IsZoomed() || (last_ucmd.buttons & BUTTON_COOPADS)) ? qtrue : qfalse; // ADS now on its own button
        walkKey = (last_ucmd.buttons & BUTTON_RUN) ? qfalse : qtrue;       // Shift held = walk-key state
        altWalk = (last_ucmd.buttons & BUTTON_COOPWALK) ? qtrue : qfalse;  // Alt held = forced slow walk

        // want to sprint: enabled, alive, not on a turret/vehicle, Shift held, NOT aiming, NOT forcing walk,
        // actually moving forward (forwardmove > 0; rules out standing still / walking backward / strafing).
        // HZM coop [2026-08-02] bug-1291 - a limping player cannot sprint. Without this, both
        // COOP_SPRINTING and COOP_LIMPING are true out of RUN_FORWARD and which one wins depends on
        // row order inside one statemap file - a silent, order-dependent bug.
        wantSprint = qfalse;
        if (enabled && !deadflag && !m_pVehicle && !m_pTurret && walkKey && !aiming && !altWalk
            && !m_bCoopWounded && last_ucmd.forwardmove > 0) { // bug-1324: wounded, not limping - see TickLimp
            wantSprint = qtrue;
        }

        // [pass2, bug-2125] a belly-crawl with Shift held latched SPRINT: fire stripped while
        // effectively stationary, gear-rattle loop, stamina drain, and the sprint speed re-base
        // fighting the crawl multiplier. Prone crawling is never a sprint.
        if (m_bCoopProne) { wantSprint = qfalse; }
        if (wantSprint && m_fCoopStamina > 0.0f) {
            m_bCoopSprinting = true;
            // [weight 9] the only weight cue that changes what you DO rather than what you feel:
            // a heavy loadout plans shorter sprints.
            {
                static cvar_t *pSD = NULL;
                float          fDrain;
                if (!pSD) { pSD = gi.Cvar_Get("coop_heftStamina", "0.6", CVAR_ARCHIVE); }
                fDrain = dt * (1.0f + CoopActiveHeft() * pSD->value);
                m_fCoopStamina -= fDrain;
            }
            if (m_fCoopStamina < 0.0f) { m_fCoopStamina = 0.0f; }
            m_fCoopStaminaHold = level.time + CoopStaminaDelay();
        } else {
            // [weight 7] READY-UP. The animation of bringing the weapon back down out of a sprint
            // already plays; the server never honoured it, so you could fire on the exact frame you
            // stopped running and the sprint carried no cost at all. The delay scales with the
            // weapon's weight, which is the whole point: a Panzerschreck takes real time to come
            // back on target, a Luger almost none.
            if (m_bCoopSprinting) {
                static cvar_t *pRU = NULL;
                if (!pRU) { pRU = gi.Cvar_Get("coop_readyUp", "0.35", CVAR_ARCHIVE); }
                m_fCoopReadyUpAt = level.time + pRU->value * (0.45f + CoopActiveHeft());
            }
            m_bCoopSprinting = false;
            // regen only when NOT actively trying to sprint, so you can't "pump" it
            // HZM coop [user 2026-08-27] ONE ECONOMY, NOT FOUR TOYS.
            //
            // Sprint, slide, vault and jump each existed on their own terms, and stamina refilled the
            // instant you stopped sprinting - so the pool never actually constrained anything: you
            // could sprint, vault, jump and sprint again with no accounting. A regen DELAY is what
            // turns a meter into a resource; every spend now pushes the refill back, so bursts of
            // movement have to be paid for and the pause afterwards is the price.
            if (!wantSprint && level.time >= m_fCoopStaminaHold) {
                m_fCoopStamina += dt * regen;
                if (m_fCoopStamina > maxStam) { m_fCoopStamina = maxStam; }
            }
        }

        // HZM coop - GEAR RATTLE: a soft equipment-rattle loop rides the sprint state (3D on the player,
        // so nearby teammates/enemies hear the sprinter too). coop_sprintGear 0 disables.
        {
            cvar_t *pGear = gi.Cvar_Get("coop_sprintGear", "1", CVAR_ARCHIVE);
            bool    gearOn = (pGear && pGear->integer) ? true : false;
            if (gearOn && m_bCoopSprinting && !m_bCoopGearLoop) {
                LoopSound("coop_gear_run", 0.85f);
                m_bCoopGearLoop = true;
            } else if (m_bCoopGearLoop && (!m_bCoopSprinting || !gearOn)) {
                StopLoopSound();
                m_bCoopGearLoop = false;
            }
        }

        // HZM coop - OUT-OF-BREATH pant. Accumulate CONTINUOUS sprint time; the moment a sprint ENDS having
        // lasted at least coop_sprintBreathTime seconds, play one of two interchangeable "out of breath" takes
        // on the player (same "breath sound on the player entity" idea as DBNO). A sprint shorter than the
        // threshold just resets the timer with no sound. The -0.05 tolerance lets a full-pool sprint (default
        // coop_sprintStamina 5 == the default 5s threshold) reliably trigger despite frame-step rounding.
        if (m_bCoopSprinting) {
            m_fCoopSprintDur += dt;
        } else if (m_fCoopSprintDur > 0.0f) {
            cvar_t *pBreathOn = gi.Cvar_Get("coop_sprintBreath", "1", CVAR_ARCHIVE);
            cvar_t *pBreathT  = gi.Cvar_Get("coop_sprintBreathTime", "5", CVAR_ARCHIVE);
            float   breathT   = pBreathT ? pBreathT->value : 5.0f;

            // bug-1324: no effort-pant when the sprint was ENDED BY getting wounded - the pant right
            // as the limp began read as a sprint SFX bug on top of an injury.
            if (pBreathOn && pBreathOn->integer && !deadflag && !m_bCoopWounded
                && m_fCoopSprintDur >= (breathT - 0.05f)) {
                const char *snd = (G_Random() < 0.5f) ? "coop_sprint_breath1" : "coop_sprint_breath2";
                Sound(snd, CHAN_VOICE, -1.0f, 160, NULL, -1.0f, 1, 0, 1, 1200);
            }
            m_fCoopSprintDur = 0.0f;
        }
    }
    //====

    TickCoopBreath(); // HZM coop [user 2026-08-17] - server-side breath budget, same tick as sprint
}

//====
// HZM coop [user 2026-08-17] - SERVER-SIDE BREATH-HOLD BUDGET.
//
// The breath-hold shipped as a CLIENT-ONLY effect: cg_view.c suppresses ADS sway while the walk key
// is held, on a cg_breathHoldTime budget with a cg_breathCooldown recharge. When the accuracy bonus
// was added to Weapon::Fire it keyed on the raw buttons only, so the bonus outlived the breath -
// hold the key long enough and the sway returned while the shots stayed pinpoint.
//
// This mirrors the client state machine exactly (same cvar names, same defaults, same order of
// tests) so the bonus dies on the same frame the sway comes back. On a listen server these are
// literally the same cvar objects, so the host is always in lockstep; a remote client who edits
// their own cg_breathHoldTime would drift, which is why the SERVER copy is the authority for
// accuracy - the client only ever owns the picture.
//
// NOTE the button sense, which is easy to get backwards and was: BUTTON_RUN is the run/walk SPEED
// state, not the raw key. With default always-run it is SET while running and CLEARED while the
// walk key is held, so "holding breath" means the bit is CLEAR.
//====
void Player::TickCoopBreath(void)
{
    static cvar_t *pHold = NULL, *pCool = NULL;
    int            iHoldMs, iCoolMs, nowMs, dt;
    qboolean       bWalkHeld, bAds;

    if (!pHold) {
        pHold = gi.Cvar_Get("cg_breathHoldTime", "7", CVAR_ARCHIVE);
        pCool = gi.Cvar_Get("cg_breathCooldown", "5", CVAR_ARCHIVE);
    }
    iHoldMs = (int)(pHold->value * 1000.0f);
    iCoolMs = (int)(pCool->value * 1000.0f);
    if (iHoldMs < 100) { iHoldMs = 100; }
    if (iCoolMs < 100) { iCoolMs = 100; }

    nowMs = (int)(level.time * 1000.0f);
    if (m_iCoopBreathRemainMs < 0) {
        m_iCoopBreathRemainMs = iHoldMs;
        m_iCoopBreathLastMs   = nowMs;
    }
    dt = nowMs - m_iCoopBreathLastMs;
    if (dt < 0 || dt > 500) { dt = 0; } // clamp pauses / map loads, exactly as the client does
    m_iCoopBreathLastMs = nowMs;

    bAds      = (last_ucmd.buttons & BUTTON_COOPADS) ? qtrue : qfalse;
    bWalkHeld = (last_ucmd.buttons & BUTTON_RUN) ? qfalse : qtrue;
    m_bCoopBreathSteady = qfalse;

    if (deadflag || m_bCoopWounded) {
        return; // no steady aim while down or dying
    }

    if (m_iCoopBreathCooldownMs != 0) {
        if (nowMs >= m_iCoopBreathCooldownMs) {
            m_iCoopBreathCooldownMs = 0;
            m_iCoopBreathRemainMs   = iHoldMs; // recharge complete
        }
    } else if (bAds && bWalkHeld && m_iCoopBreathRemainMs > 0) {
        m_bCoopBreathSteady = qtrue;
        m_iCoopBreathRemainMs -= dt;
        if (m_iCoopBreathRemainMs <= 0) {
            m_iCoopBreathRemainMs   = 0;
            m_iCoopBreathCooldownMs = nowMs + iCoolMs; // ran out -> start recharge
        }
    }
}

//====
// HZM coop - TAKE COVER [214] per-frame validation.
// Runs from ClientThink (right after TickSprint, same last_ucmd timing). While the player has
// cover REQUESTED (coop_setcover 1 via the keybind bus), decide which pose is valid THIS frame:
//
//   WALL (standing): a chest-height trace BACKWARD along -yaw_forward hits a mostly-vertical
//   solid within coop_coverWallDist -> the character presses their back to it (the AI cornering
//   wall_alert pose faces out from the wall, so back-to-wall is the direction that reads right).
//
//   LOW (crouched tuck): a crouch-chest trace FORWARD hits a mostly-vertical solid within
//   coop_coverLowDist AND a head-height trace over the same line is CLEAR -> waist-high cover
//   (sandbags, low walls, crates) the character can tuck behind and blind-fire OVER.
//
// Stance is NOT checked here - the statemap owns height (the COVER_LOW state's modheight "duck"
// tucks the player automatically when the low pose engages from STAND).
//
// The request is CANCELLED outright on: death, spectate, vehicle/turret/ladder mounts, freeze,
// jump or any WASD movement input (matching the emote UX - moving breaks the pose). If only the
// GEOMETRY goes invalid (crouch/stand morph, doorway edge), a short grace window
// (coop_coverGrace) keeps the request alive so the pose can re-engage without re-pressing.
// BLIND FIRE: pose valid + primary fire held + a shootable-class weapon -> m_bCoopBlindfire,
// which drives the COVER_*_FIRE statemap states (their anims carry the vanilla "N fire" frame
// commands) plus the spread penalty in Weapon::Shoot and the low-cover muzzle raise in
// Weapon::GetMuzzlePosition.
//====
// HZM coop [user 2026-08-07] Mirror the cover state to the OWNING client (change-only - a per-frame
// stufftext would flood the reliable command buffer, the same rule coop_limpView follows).
// MUST be called on EVERY exit path of TickCoopCover. It was originally only at the tail, and both
// early returns skipped it - so releasing cover never sent 0 and the client kept the camera lift in
// normal third person. Exactly the "make sure this cannot affect non-cover play" case.
void Player::SendCoopCoverView()
{
    int coverWant = ((m_bCoopCoverLow || m_bCoopCoverWall) && !m_bCoopCoverPeek) ? 1 : 0;

    if (coverWant != m_iCoopCoverSent) {
        m_iCoopCoverSent = coverWant;
        gi.SendServerCommand(edict - g_entities, "stufftext \"set coop_coverView %d\"", coverWant);
    }
}

// HZM coop [user 2026-08-07] ONE CLICK, ONE SHOT for semi-auto blind fire.
// Asked by Weapon::Shoot for every round the cover-fire animation tries to emit. Limiting the
// STATE was not enough - the COVER_*_FIRE clips carry several "fire" frame commands each, so one
// click still produced a G43 triple-tap and emptied a revolver. Full-auto weapons are unaffected;
// they keep firing for as long as the trigger is held.
bool Player::CoopBlindfireAllowShot()
{
    Weapon *weapon;

    if (!m_bCoopBlindfire) {
        return true;   // not blind firing - never interfere
    }

    weapon = GetActiveWeapon(WEAPON_MAIN);
    if (!weapon || !weapon->IsSemiAuto()) {
        return true;   // full auto holds down as normal
    }

    if (m_bCoopBfShotDone) {
        return false;  // this press already sent one
    }

    m_bCoopBfShotDone = true;

    // [user 2026-08-07] CUT THE ANIMATION SHORT. Dropping the flag here exits the COVER_*_FIRE
    // statemap state immediately after this round, so the clip stops instead of playing out its
    // remaining "fire" frames - one click now looks like one shot as well as being one shot.
    // Safe only because the low-cover muzzle raise was moved off this flag onto the cover POSE:
    // were it still gated on m_bCoopBlindfire, clearing it mid-burst would drop the raise and put
    // the round into the cover - the exact bug reported for release and dry-clip.
    // It cannot re-arm on its own: TickCoopCover only re-sets the flag for a semi-auto on the
    // press EDGE, and the trigger is still held at this point.
    m_bCoopBlindfire = false;

    return true;
}

//====
// HZM coop [user 2026-08-07] NAV NODE RECORDER.
// Half this map's rear area has no AI path coverage, so actors spawned there cannot move at all
// (proven by the nav probe: 12 of 44 points dead). MOHAA can create nodes at runtime - NavMaster
// does exactly this behind ai_editmode - and PathSearch::CreatePaths() rebuilds the whole graph
// from the current node set. This exposes that as a bindable toggle: walk the dead ground, get
// nodes, rebuild, done.
// Nodes are also printed so they can be baked into the map script (via coop_navnode) and recreated
// on every load - runtime nodes themselves do not survive a map change.
//====
// [bug 2026-08-07] CreatePaths() opens with `if (m_bNodesloaded) return;`, and that flag is set
// for the rest of the map once the graph is built at load. Calling it bare from here was a silent
// no-op: nodes were allocated but never linked, and ArchiveSaveNodes() at the tail never ran, so
// no .pth was written. The engine's own editor (NavMaster::CreatePaths) calls ClearNodes() first -
// that clears the flag and drops the stale links while KEEPING the node objects. Do the same.
void Player::CoopNavRebuild(void)
{
    // Two different teardowns, and picking the wrong one corrupts the heap either way.
    // Nodes from a baked .pth live in one bulk block that must NOT be freed (ClearNodes would),
    // because the map's named info_pathnode entities are bound to those objects. Nodes built at
    // load time from BSP entities were allocated individually and ClearNodes frees what it owns.
    //
    // Gate on CoopBulkOwnsNodes(), NOT CoopNodesAreBulk(): by the time a bake reaches here the
    // first coop_navnode has already run prepare, which nulls bulkNavMemory - so the allocation
    // predicate reads "not bulk" and this would pick ClearNodes and free the block. That is
    // exactly the crash in droptofloor.
    if (PathSearch::CoopBulkOwnsNodes()) {
        PathSearch::CoopPrepareRuntimeRebuild();   // no-op if a coop_navnode already prepared
    } else {
        PathSearch::ClearNodes();
    }

    PathSearch::CreatePaths();

    // Put FreePathNode back to bulk behaviour before anything can delete a node - see
    // PathSearch::CoopFinishRuntimeRebuild.
    PathSearch::CoopFinishRuntimeRebuild();
}

void Player::EventCoopNavRec(Event *ev)
{
    m_bCoopNavRec = !m_bCoopNavRec;

    if (m_bCoopNavRec) {
        m_vCoopNavLast = origin;
        m_iCoopNavCount = 0;
        CoopNavDropNode(origin);   // anchor the run where you stand
        gi.centerprintf(edict, "NAV RECORD: ON - walk the ground AI should use");
    } else {
        CoopNavRebuild();
        gi.centerprintf(edict, va("NAV RECORD: OFF - %d nodes, graph rebuilt + saved", m_iCoopNavCount));
        gi.Printf("^~^~^ NAVREC DONE %d nodes\n", m_iCoopNavCount);
    }
}

// [user 2026-08-07] No DOOR-node command: PathSearch::CreatePaths UNLINKS every door before it
// builds links and relinks them after, so it connects straight through doorways already. The DOOR
// spawnflag only governs the conditional "skip this route while the door is locked" case, and the
// QUAKED doc's bit for it collides with AI_CONCEALMENT in the header - not worth guessing at.
void Player::CoopNavDropNode(const Vector& pos, int flags)
{
    // First runtime node on a map that loaded a baked .pth: detach from the archive's bulk block
    // before allocating, or this walks the allocation pointer off the front of it. Idempotent, so
    // it is safe to call per node - the baked cfgs create hundreds in one frame.
    if (PathSearch::CoopNodesAreBulk()) {
        PathSearch::CoopPrepareRuntimeRebuild();
    }

    // AI_AddNode calls gi.Error(ERR_DROP) past MAX_PATHNODES, which would kick the player
    // mid-bake. Refuse quietly instead and say so once.
    if (PathSearch::nodecount >= MAX_PATHNODES - 8) {
        if (!m_bCoopNavFull) {
            m_bCoopNavFull = true;
            gi.Printf("^~^~^ NAVNODE refused - MAX_PATHNODES (%d) reached\n", MAX_PATHNODES);
        }
        return;
    }

    PathNode *node = new PathNode;

    node->nodeflags = flags;
    node->setOrigin(pos);

    m_vCoopNavLast = pos;
    m_iCoopNavCount++;
    gi.Printf("^~^~^ NAVNODE %d %d %d %d\n", (int)pos[0], (int)pos[1], (int)pos[2], flags);
}

void Player::EventCoopNavNode(Event *ev)
{
    Vector pos(ev->GetFloat(1), ev->GetFloat(2), ev->GetFloat(3));

    CoopNavDropNode(pos);
}


// Rebuild and re-save. CreatePaths() ends in ArchiveSaveNodes(), which writes the FULL node set -
// the map's own nodes plus anything created since - to level.m_pathfile in the homepath. That file
// is the shippable artefact: drop it in a pk3 and the engine loads it instead of the stock one,
// because ArchiveLoadNodes reads through gi.FS_ReadFile and FS_FileNewer is a stub returning 0, so
// the only gate is the BSP timestamp, which is identical on every install.
void Player::EventCoopNavBuild(Event *ev)
{
    int before = PathSearch::nodecount;

    CoopNavRebuild();
    gi.Printf("^~^~^ NAVBUILD rebuilt %d nodes -> %s\n", before, level.m_pathfile.c_str());
}

// Called every frame from TickCoopCover's caller: drop a node once the player has walked far enough.
void Player::TickCoopNavRec(void)
{
    cvar_t *pSpacing;
    float   fSpacing;

    if (!m_bCoopNavRec) {
        return;
    }

    pSpacing = gi.Cvar_Get("coop_navRecSpacing", "96", CVAR_ARCHIVE);
    fSpacing = pSpacing ? pSpacing->value : 96.0f;
    if (fSpacing < 16.0f) { fSpacing = 16.0f; }

    if ((origin - m_vCoopNavLast).length() >= fSpacing) {
        CoopNavDropNode(origin);
    }
}

void Player::TickCoopCover()
{
    qboolean wallValid = qfalse;
    qboolean lowValid  = qfalse;

    if (!m_bCoopCoverRequested) {
        // HZM coop [user 2026-08-09] AUTO COVER. The user's ask verbatim: "crouch behind cover and
        // after a second it will automatically put you behind cover like modern games do". While
        // crouched, still, alive and unmounted, a dwell timer runs; when it matures we set the
        // SAME request flag the bus-26 bind sets and fall through - the existing validation either
        // locks the LOW pose or clears the request again, silently (the "No cover here" feedback is
        // script-side and only fires on the manual bind). A failed attempt retries every 0.4s while
        // the player keeps holding position; every hard-cancel below pushes a backoff so a
        // DELIBERATE exit (moving away) does not re-grab the wall the player just left.
        // coop_coverAuto 0 disables; coop_coverAutoDelay tunes the dwell (default 0.9s).
        static cvar_t *s_coopCoverAuto      = NULL;
        static cvar_t *s_coopCoverAutoDelay = NULL;
        if (!s_coopCoverAuto) {
            s_coopCoverAuto      = gi.Cvar_Get("coop_coverAuto", "1", 0);
            s_coopCoverAutoDelay = gi.Cvar_Get("coop_coverAutoDelay", "0.9", 0);
        }
        if (s_coopCoverAuto->integer && !deadflag && !IsSpectator() && !m_pVehicle && !m_pTurret && !m_bCoopProne // [pass4] a lying player is not a wall-cover candidate
            && !m_bCoopBraceMounted // [user 2026-08-27] mounted first = mounted wins; cover stays out
            && !m_pLadder && !level.playerfrozen && !m_bFrozen && !(flags & FL_IMMOBILE)
            && (client->ps.pm_flags & PMF_DUCKED) && !last_ucmd.forwardmove && !last_ucmd.rightmove
            && last_ucmd.upmove <= 0) {
            m_fCoopCoverAutoDwell += level.frametime;
            if (m_fCoopCoverAutoDwell >= s_coopCoverAutoDelay->value && level.time >= m_fCoopCoverAutoRetry) {
                m_bCoopCoverRequested = true; // speculative - validation below is the judge
                m_fCoopCoverAutoRetry = level.time + 0.4f;
            }
        } else {
            m_fCoopCoverAutoDwell = 0.0f;
        }
        if (!m_bCoopCoverRequested) {
            m_bCoopCoverWall    = false;
            m_bCoopCoverLow     = false;
            m_bCoopBlindfire    = false;
            m_bCoopCoverPeek    = false;
            m_fCoopCoverBadTime = 0.0f;
            SendCoopCoverView();
            return;
        }
    }

    // hard cancels - things that end cover instantly (the statemap "!" exits fire this frame)
    if (deadflag || IsSpectator() || m_bCoopProne /*[pass4]*/ || m_bCoopBraceMounted /*[user 08-27]*/ || m_pVehicle || m_pTurret || m_pLadder || level.playerfrozen || m_bFrozen
        || (flags & FL_IMMOBILE) || last_ucmd.forwardmove || last_ucmd.rightmove || last_ucmd.upmove > 0) {
        m_bCoopCoverRequested = false;
        // HZM coop [user 2026-08-09] auto-cover backoff: a deliberate exit must not re-grab
        m_fCoopCoverAutoDwell = 0.0f;
        m_fCoopCoverAutoRetry = level.time + 0.9f;
        m_bCoopCoverWall      = false;
        m_bCoopCoverLow       = false;
        m_bCoopBlindfire      = false;
        m_bCoopCoverPeek      = false;
        m_fCoopCoverBadTime   = 0.0f;
        SendCoopCoverView();
        return;
    }

    {
        cvar_t *pWallD = gi.Cvar_Get("coop_coverWallDist", "40", CVAR_ARCHIVE);
        cvar_t *pLowD  = gi.Cvar_Get("coop_coverLowDist", "48", CVAR_ARCHIVE);
        cvar_t *pLowH  = gi.Cvar_Get("coop_coverLowHeight", "72", CVAR_ARCHIVE); // HZM coop [user 07-19]: head-clear height, was hardcoded 58 - raised so slightly-taller crates/cover register as low cover (tunable)
        cvar_t *pGrace = gi.Cvar_Get("coop_coverGrace", "0.5", CVAR_ARCHIVE);
        float   fWallD = pWallD ? pWallD->value : 40.0f;
        float   fLowD  = pLowD ? pLowD->value : 48.0f;
        float   fLowH  = pLowH ? pLowH->value : 72.0f;
        float   fGrace = pGrace ? pGrace->value : 0.5f;
        Vector  vFwd   = yaw_forward; // flat view yaw (same source CondSolidForward uses)
        trace_t trace;

        // WALL: standing chest height (48u). ENTRY is what the player FACES (walk up to a wall,
        // press cover, the character TURNS and puts their back to it); the wall OUT normal is
        // ANCHORED (m_vCoopCoverNormal) so the SUSTAIN check is view-independent: the mouse can
        // orbit (free-look) and the RMB peek can aim anywhere without breaking the pose - only
        // physically leaving the wall (or moving) drops it.
        // [user 2026-08-22] WALL COVER RE-ENABLED (was `if (false)`). The two faults named in the
        // old tombstone are both gone rather than re-hidden: the peek step-out setOrigin is
        // DELETED (see below), and the entry view-snap is NOT restored - the normal is anchored
        // and the view is left alone, so nothing yanks the camera on entry. The side probe is
        // replaced by a solver with a real NONE state (bug-2028).
        {
            Vector vStart = origin + Vector(0, 0, 48);

            if (m_bCoopCoverWall) {
                // sustain: the ANCHORED wall must still be behind the POSE POSITION - while the peek
                // step-out displaces the body toward the corner (often past the wall edge!), the check
                // runs from the stored base so peeking can never drop the cover
                Vector vSusStart = vStart;

                if (m_fCoopPeekFrac > 0.01f) {
                    vSusStart = m_vCoopCoverBaseOrg + Vector(0, 0, 48);
                }
                trace = G_Trace(
                    vSusStart, vec_zero, vec_zero, vSusStart - m_vCoopCoverNormal * fWallD, this, MASK_SOLID, false,
                    "Player::TickCoopCover wall"
                );
                if (!trace.startsolid && trace.fraction < 1.0f && trace.plane.normal[2] < 0.7f
                    && trace.plane.normal[2] > -0.7f && DotProduct(trace.plane.normal, m_vCoopCoverNormal) > 0.5f) {
                    wallValid = qtrue;
                }
            } else {
                // entry: wall we are FACING - snap our back onto it (face along its normal)
                trace = G_Trace(
                    vStart, vec_zero, vec_zero, vStart + vFwd * fWallD, this, MASK_SOLID, false,
                    "Player::TickCoopCover wall-enter"
                );
                if (!trace.startsolid && trace.fraction < 1.0f && trace.plane.normal[2] < 0.7f
                    && trace.plane.normal[2] > -0.7f && DotProduct(trace.plane.normal, vFwd) < -0.5f) {
                    // [user 2026-08-22, bug-2056] BACK TO THE WALL. Removing the entry view-snap
                    // (note below) left the player FACING the wall, which is not what taking cover
                    // looks like and is not what any downstream code assumed. User: "when I take
                    // cover I am facing the wall, not back against the wall".
                    // Turn the BODY only - setAngles, never SetViewAngles - so the pose is right
                    // and the mouse stays the player's. Taking the VIEW is what "fights free-look"
                    // meant. One-time, on entry, behind a switch so it can be dropped live.
                    // [user 2026-08-23, bug-2055] THE PER-FRAME setAngles THAT USED TO SIT HERE IS
                    // REMOVED. It was bug-2056's attempt at "back to the wall", written to the rule
                    // "turn the BODY only, never the view" - but a player's body yaw is re-derived
                    // from their own command angles every frame, so a bare setAngles is overwritten
                    // before it is ever drawn. It was a per-frame NO-OP that read as a working
                    // feature, which is worse than nothing: it is what a reader checks, finds
                    // present, and concludes is not the problem.
                    // The turn now happens ONCE on the rising edge of wall cover, further down,
                    // and moves body AND view together the way this codebase actually rotates a
                    // player (player.cpp:6201, :6277).
                    // [user 2026-08-22] NO ENTRY VIEW-SNAP. The original yanked the yaw to the
                    // wall normal on entry; it was half the reported "crash-prone" feel and it
                    // fights free-look. The normal is ANCHORED instead, so the pose is
                    // view-independent and the mouse stays the player's.
                    m_vCoopCoverNormal  = Vector(trace.plane.normal);
                    m_vCoopCoverBaseOrg = origin;
                    wallValid           = qtrue;
                } else {
                    // fallback: already standing back-to-wall
                    trace = G_Trace(
                        vStart, vec_zero, vec_zero, vStart - vFwd * fWallD, this, MASK_SOLID, false,
                        "Player::TickCoopCover wall"
                    );
                    if (!trace.startsolid && trace.fraction < 1.0f && trace.plane.normal[2] < 0.7f
                        && trace.plane.normal[2] > -0.7f && DotProduct(trace.plane.normal, vFwd) > 0.5f) {
                        m_vCoopCoverNormal  = Vector(trace.plane.normal);
                        m_vCoopCoverBaseOrg = origin;
                        wallValid           = qtrue;
                    }
                }
            }

            // [user 2026-08-22] OPEN-SIDE SOLVER. "It needs to be smart enough to know which side
            // you are wanting to blindfire and pop out of cover from... think of a doorway, you
            // could be in cover on either side."
            //
            // The old probe took ONE 44u sample per side, accepted the first hit, let LEFT win
            // every tie, and had no way to say "neither" - so in a doorway (both sides open) it
            // always answered LEFT, and against unbroken wall it kept whatever it last said.
            // Combined with an init of 1 and writers that only ran here, that is bug-2028: a side
            // that was permanently "LEFT" and would have steered blindfire into the wall.
            //
            // Three changes: SCAN outward (the edge can be at any distance, and that distance is
            // worth knowing - it scales the lean and the muzzle slide), check HEAD height too (a
            // waist-high recess is not something you can pop out of), and allow NONE.
            if (wallValid) {
                static cvar_t *pScanMin = NULL, *pScanMax = NULL, *pScanStep = NULL;
                static cvar_t *pHeadZ = NULL, *pDead = NULL, *pCommit = NULL, *pMaxDelta = NULL;
                // [user 2026-08-22, bug-2056] PLAYER'S LEFT, NOT THE WALL'S. This was derived
                // purely from the wall normal, which is consistent in WORLD terms (it always
                // points away from the wall) but says nothing about which way the PLAYER is
                // facing. There are two entry paths and they leave the player facing opposite
                // ways: entering while facing the wall (player.cpp ~13890, dot(normal,fwd)<-0.5)
                // leaves forward roughly -normal, while backing in (dot>+0.5) leaves it +normal.
                // So a wall-derived "left" was the player's left only when they had backed in -
                // and mirrored when they walked up facing it, which is the normal way anyone
                // takes cover. User: "when I take cover I am facing the wall, not back against
                // the wall... I think lean technically works... but its leaning the wrong side."
                // The lean and the blindfire offset are BODY actions, so the side has to be
                // expressed relative to the body. Compute the opening in world space as before,
                // then flip it into the player's frame.
                Vector   vWallLeft = Vector(0.0f - m_vCoopCoverNormal[1], m_vCoopCoverNormal[0], 0);
                Vector   vLeft     = vWallLeft;
                {
                    Vector vBodyFwd, vBodyRight;

                    AngleVectors(GetViewAngles(), vBodyFwd, vBodyRight, NULL);
                    vBodyFwd[2] = 0;
                    vBodyFwd.normalize();
                    // MOHAA's AngleVectors right vector points to the player's RIGHT, so the
                    // player's left is -right. If the wall-derived left disagrees with the body's
                    // left, the player entered facing the wall and every side is mirrored.
                    vBodyRight[2] = 0;
                    vBodyRight.normalize();
                    if (DotProduct(vWallLeft, vBodyRight) > 0.0f) {
                        vLeft = vWallLeft * -1.0f;
                    }
                }
                Vector   vFwdFlat, vSideDir;
                float    fEdge[2];
                qboolean bOpen[2];
                qboolean bSweepSolid[2];   // [bug-2055] did the hull sweep START in contact?
                float    fSweepFrac[2];    // [bug-2055] ...and if not, how far did it get?
                int      iSideOf[2] = {1, -1}; // index 0 = LEFT, 1 = RIGHT
                int      k, iWant;
                float    fIntent, fYawNow, fYawDelta;

                if (!pScanMin)  { pScanMin  = gi.Cvar_Get("coop_coverSideScanMin",    "16",   CVAR_ARCHIVE); }
                if (!pScanMax)  { pScanMax  = gi.Cvar_Get("coop_coverSideScanMax",    "72",   CVAR_ARCHIVE); }
                if (!pScanStep) { pScanStep = gi.Cvar_Get("coop_coverSideScanStep",   "14",   CVAR_ARCHIVE); }
                if (!pHeadZ)    { pHeadZ    = gi.Cvar_Get("coop_coverSideHeadZ",      "62",   CVAR_ARCHIVE); }
                if (!pDead)     { pDead     = gi.Cvar_Get("coop_coverSideIntentDead", "0.25", CVAR_ARCHIVE); }
                if (!pCommit)   { pCommit   = gi.Cvar_Get("coop_coverSideCommitMs",   "180",  CVAR_ARCHIVE); }
                if (!pMaxDelta) { pMaxDelta = gi.Cvar_Get("coop_coverSideMaxDelta",   "10",   CVAR_ARCHIVE); }

                // ---- STEP 1+2: find the edge, then prove the silhouette can clear it ----------
                for (k = 0; k < 2; k++) {
                    float d;

                    fEdge[k]       = -1.0f;
                    bOpen[k]       = qfalse;
                    bSweepSolid[k] = qfalse;
                    fSweepFrac[k]  = -1.0f;   // -1 = the sweep never ran (no edge found)
                    vSideDir = vLeft * (float)iSideOf[k];

                    for (d = pScanMin->value; d <= pScanMax->value; d += pScanStep->value) {
                        Vector  vBase = origin + vSideDir * d;
                        Vector  vChestFrom = vBase + Vector(0, 0, 48);
                        trace_t tChest = G_Trace(
                            vChestFrom, vec_zero, vec_zero,
                            vChestFrom - m_vCoopCoverNormal * (fWallD + 24), this, MASK_SOLID, false,
                            "Player::TickCoopCover side-scan-chest"
                        );

                        if (tChest.startsolid) {
                            break; // jammed laterally - that is not an edge, it is a corner we are in
                        }
                        if (tChest.fraction >= 1.0f) {
                            Vector  vHeadFrom = vBase + Vector(0, 0, pHeadZ->value);
                            trace_t tHead     = G_Trace(
                                vHeadFrom, vec_zero, vec_zero,
                                vHeadFrom - m_vCoopCoverNormal * (fWallD + 24), this, MASK_SOLID, false,
                                "Player::TickCoopCover side-scan-head"
                            );

                            if (tHead.fraction >= 1.0f) {
                                fEdge[k] = d; // wall gone at BOTH heights = a real opening
                            }
                            break;
                        }
                    }

                    if (fEdge[k] >= 0.0f) {
                        // full player hull, laterally: a gap the body cannot fit through is
                        // scenery, not a pop-out
                        // [bug-2055] START OFF THE WALL. 4864 of 6271 measured samples read
                        // `edgeL=44 edgeR=58 openL=0 openR=0` - both edges FOUND, both judged
                        // unfittable, because the sweep began at the player's own origin while he
                        // was pressed against the wall and started in contact. With no side,
                        // blindfire has no direction: "blindfire occurred on the wrong side".
                        Vector  vFrom  = origin + m_vCoopCoverNormal * 12.0f;
                        Vector  vWant  = vFrom + vSideDir * (fEdge[k] + 8.0f);
                        trace_t tSweep = G_Trace(
                            vFrom, mins, maxs, vWant, this, MASK_PLAYERSOLID, false,
                            "Player::TickCoopCover side-sweep"
                        );

                        bOpen[k] = (!tSweep.startsolid && tSweep.fraction >= 0.70f) ? qtrue : qfalse;
                        // [bug-2055 phase 1] RECORD WHY, NOT JUST WHETHER. startsolid and
                        // "swept but not far enough" are different failures with different fixes
                        // (move the start point vs relax the fraction), and the probe could not
                        // tell them apart - so the previous pass had to guess. It guessed the
                        // start point, shipped the 12u normal offset above, and was never
                        // re-measured. These two fields make the next playtest decide it.
                        bSweepSolid[k] = tSweep.startsolid ? qtrue : qfalse;
                        fSweepFrac[k]  = tSweep.fraction;
                    }
                }

                // ---- STEP 3: score ------------------------------------------------------------
                AngleVectors(GetViewAngles(), vFwdFlat, NULL, NULL);
                vFwdFlat[2] = 0;
                vFwdFlat.normalize();
                fIntent = DotProduct(vFwdFlat, vLeft); // + = looking left, - = looking right

                if (bOpen[0] && !bOpen[1]) {
                    iWant = 1;
                } else if (bOpen[1] && !bOpen[0]) {
                    iWant = -1;
                } else if (!bOpen[0] && !bOpen[1]) {
                    iWant = 0; // NONE - the value that never existed
                } else {
                    // the doorway: both jambs are poppable, so the player's LOOK is the intent
                    if (fIntent > pDead->value) {
                        iWant = 1;
                    } else if (fIntent < 0.0f - pDead->value) {
                        iWant = -1;
                    } else if (fEdge[0] + 8.0f < fEdge[1]) {
                        iWant = 1; // no intent expressed: nearer edge wins
                    } else if (fEdge[1] + 8.0f < fEdge[0]) {
                        iWant = -1;
                    } else {
                        iWant = m_iCoopCoverSide; // dead heat: do not change
                    }
                }
                m_iCoopCoverSideWant = iWant;

                // ---- STEP 4: commit, with hysteresis ------------------------------------------
                // The side drives an ANIMATION and (Phase 2) the camera shoulder. Flipping either
                // mid-burst or mid-peek strobes, so both states LATCH it.
                fYawNow   = GetViewAngles()[YAW];
                fYawDelta = AngleSubtract(fYawNow, m_fCoopCoverLastYaw);
                if (fYawDelta < 0.0f) {
                    fYawDelta = 0.0f - fYawDelta;
                }
                m_fCoopCoverLastYaw = fYawNow;

                if (iWant == m_iCoopCoverSide) {
                    m_fCoopCoverSideDwell = 0.0f;
                } else if (m_iCoopCoverSide == 0 && iWant != 0) {
                    // [user 2026-08-23, bug-2055] FIRST ACQUISITION IS IMMEDIATE - no hysteresis.
                    // The dwell exists to stop the side FLIP-FLOPPING between two real sides
                    // mid-burst, which strobes an animation and the camera. Coming from NONE there
                    // is nothing to strobe, and making the first acquisition wait 180ms broke the
                    // feature outright: the cover ENTRY turn reads this side to decide which way to
                    // face, and it runs on the rising edge - when the side was still 0. So the
                    // player was always turned straight out of the wall and never toward the
                    // opening. User: "camera is still facing away from the opening... I have to
                    // move my mouse around to the opening."
                    // It was self-reinforcing, too: the entry turn swings the view, that yaw delta
                    // exceeds coop_coverSideMaxDelta, and the delta RESETS the dwell - so the turn
                    // pushed away the very value it needed.
                    m_iCoopCoverSide      = iWant;
                    m_fCoopCoverSideDwell = 0.0f;
                } else if (m_bCoopBlindfire || m_fCoopPeekFrac > 0.01f) {
                    m_fCoopCoverSideDwell = 0.0f; // latched mid-action
                } else {
                    m_fCoopCoverSideDwell += level.frametime;
                    if (fYawDelta > pMaxDelta->value) {
                        m_fCoopCoverSideDwell = 0.0f; // still swinging - wait for a decision
                    }
                    if (m_fCoopCoverSideDwell >= pCommit->value * 0.001f) {
                        m_iCoopCoverSide      = iWant;
                        m_fCoopCoverSideDwell = 0.0f;
                    }
                }

                if (m_iCoopCoverSide == 1) {
                    m_fCoopCoverEdge = fEdge[0];
                } else if (m_iCoopCoverSide == -1) {
                    m_fCoopCoverEdge = fEdge[1];
                } else {
                    m_fCoopCoverEdge = 0.0f;
                }
                if (m_fCoopCoverEdge < 0.0f) {
                    m_fCoopCoverEdge = 0.0f;
                }

                // ---- publish the side to this client (change-only) -----------------------------
                // The predictor needs the side to synthesise the lean, and there is NO free pmove
                // bit to carry it (0..15 are all allocated and net_pm_flags is a hard 16-bit
                // netfield). Same per-client change-only stufftext pattern as coop_vaultView:
                // sending only on change costs nothing per frame, and a dropped command
                // self-corrects on the next change instead of sticking.
                if (m_iCoopCoverSide != m_iCoopCoverSideSent) {
                    m_iCoopCoverSideSent = m_iCoopCoverSide;
                    gi.SendServerCommand(edict - g_entities,
                                         "stufftext \"set coop_coverSide %d\"", m_iCoopCoverSide);
                }

                // ---- probe P1/P2 --------------------------------------------------------------
                {
                    static cvar_t *pPr = NULL;
                    static float   s_nextPr = 0.0f;

                    if (!pPr) {
                        // [bug-2055 phase 1] DEFAULT ON. Left at 0 it was not armed for the one playtest that
                        // mattered - the user took cover, reported it broken, and the log held ZERO
                        // COVERSIDE samples, so the only distribution we have is from a build two
                        // fixes ago. Same failure as the gun-visibility probe (bug-2048). Flags 0,
                        // never CVAR_ARCHIVE, so it cannot fossilise into a saved config (TRAPS T7).
                        pPr = gi.Cvar_Get("coop_coverProbe", "1", 0);
                    }
                    // [bug-2072] RATE-LIMITED, because this ships. GUNVIS could default ON safely
                    // because it is EDGE-triggered - a handful of lines a session. This one is
                    // per-tick by design (the 78%-both-closed distribution is the whole point), so
                    // unbounded it would put a debug print in every player's console for as long as
                    // they hold cover. 4/sec keeps the distribution readable and the volume sane.
                    // Same staleness reseed as the wall=0 line: level.time restarts on map load.
                    if (level.time < s_nextPr) { s_nextPr = 0.0f; }
                    if (pPr->integer && level.time >= s_nextPr) {
                        s_nextPr = level.time + 0.25f;
                        gi.Printf(
                            "^~^~^ COVERSIDE wall=1 want=%d have=%d edgeL=%.0f edgeR=%.0f "
                            "openL=%d openR=%d ssL=%d ssR=%d frL=%.2f frR=%.2f "
                            "intent=%.2f dwell=%.2f\n",
                            iWant, m_iCoopCoverSide, fEdge[0], fEdge[1], (int)bOpen[0], (int)bOpen[1],
                            (int)bSweepSolid[0], (int)bSweepSolid[1], fSweepFrac[0], fSweepFrac[1],
                            fIntent, m_fCoopCoverSideDwell
                        );
                    }
                }
            } else {
                // no valid wall pose = no side. Never leave a stale one standing (bug-2028).
                m_iCoopCoverSide = 0;
                m_fCoopCoverEdge = 0.0f;
                // [bug-2055 phase 1] SILENCE WAS AMBIGUOUS. The probe only ever printed from
                // inside the wallValid branch, so "no COVERSIDE lines" could mean the solver is
                // fine and no wall was ever detected, OR the wall detect never succeeded at all -
                // two opposite diagnoses. bug-2055 hit exactly that and had to reason around it.
                // A wall=0 line makes the distinction free. Rate-limited because this branch runs
                // every tick the player is in cover without a wall.
                {
                    static cvar_t *pPrNo   = NULL;
                    static float   s_nextNo = 0.0f;

                    if (!pPrNo) { pPrNo = gi.Cvar_Get("coop_coverProbe", "1", 0); }
                    // level.time restarts at 0 on every map load while this static does not, so a
                    // stale future stamp would silence the probe for up to the PREVIOUS map's
                    // length - the same staleness trap the camera/sprint/vault reseeds all carry.
                    if (level.time < s_nextNo) { s_nextNo = 0.0f; }
                    if (pPrNo->integer && level.time >= s_nextNo) {
                        s_nextNo = level.time + 0.5f;
                        gi.Printf("^~^~^ COVERSIDE wall=0 (no valid wall pose this tick)\n");
                    }
                }
            }
        }

        // LOW: crouch-chest height (36u) forward must HIT, head height (coop_coverLowHeight, default
        // 72u - raised from 58 so slightly-taller crates/cover register) forward must be CLEAR.
        // The obstacle OUT normal is anchored on entry too, so peeking (RMB aim over the top)
        // does not break the tuck.
        {
            Vector vChest = origin + Vector(0, 0, 36);
            Vector vHead  = origin + Vector(0, 0, fLowH);
            Vector vProbe = vFwd;

            if (m_bCoopCoverLow) {
                vProbe = Vector(0, 0, 0) - m_vCoopCoverNormal;
            }

            trace = G_Trace(
                vChest, vec_zero, vec_zero, vChest + vProbe * fLowD, this, MASK_SOLID, false,
                "Player::TickCoopCover low"
            );
            if (!trace.startsolid && trace.fraction < 1.0f && trace.plane.normal[2] < 0.7f
                && trace.plane.normal[2] > -0.7f && DotProduct(trace.plane.normal, vProbe) < -0.5f) {
                trace = G_Trace(
                    vHead, vec_zero, vec_zero, vHead + vProbe * fLowD, this, MASK_SOLID, false,
                    "Player::TickCoopCover head"
                );
                if (!trace.startsolid && trace.fraction >= 1.0f) {
                    if (!m_bCoopCoverLow) {
                        m_vCoopCoverNormal  = Vector(0, 0, 0) - vProbe; // out = back toward the player
                        m_vCoopCoverBaseOrg = origin;
                    }
                    lowValid = qtrue;
                }
            }
        }

        // geometry grace: brief invalid gaps (stance morph, clipping a doorway edge while turning)
        // keep the request; a sustained miss drops it so the toggle can't go stale out in the open
        if (!wallValid && !lowValid) {
            if (m_fCoopCoverBadTime <= 0.0f) {
                m_fCoopCoverBadTime = level.time;
            } else if (level.time - m_fCoopCoverBadTime > fGrace) {
                m_bCoopCoverRequested = false;
            }
        } else {
            m_fCoopCoverBadTime = 0.0f;
        }
    }

    {
        bool bWasPeek = m_bCoopCoverPeek;

        // [user 2026-08-23, bug-2055] WALL AND LOW ARE MUTUALLY EXCLUSIVE, AND THE TYPE LATCHES.
        // "I think its triggering standing wall cover when I am in crouch wall cover and then hold
        // right mouse to aim over shoulder." Correct, and the mechanism is this pair of lines:
        // both flags were computed independently from the same request, so BOTH could be true.
        // Peeking from low cover raises you to standing height (COVER_LOW_PEEK entry is
        // modheight "stand"), which makes the standing wall test start passing MID-SESSION - so
        // crouch cover silently promoted itself to wall cover the moment you aimed.
        // Whichever type you established is now held until you actually leave cover; when neither
        // is established yet and both are geometrically valid, being crouched picks LOW.
        bool bWallOk = (m_bCoopCoverRequested && wallValid);
        bool bLowOk  = (m_bCoopCoverRequested && lowValid);

        if (m_bCoopCoverLow && bLowOk) {
            bWallOk = false;                 // already in low - wall may not take over
        } else if (m_bCoopCoverWall && bWallOk) {
            bLowOk = false;                  // already at a wall - low may not take over
        } else if (bWallOk && bLowOk) {
            if (client->ps.pm_flags & PMF_DUCKED) { bWallOk = false; }  // fresh + crouched -> LOW
            else                                  { bLowOk  = false; }
        }

        // [user 2026-08-23, bug-2055] TURN THE PLAYER ONCE, ON ENTRY - BODY AND VIEW TOGETHER.
        // User: "my characters body is still facing the wall though, not up against the wall, both
        // when first getting behind cover and when aiming. I do not lean when I aim, I just aim
        // straight into the wall in front of where my character is facing."
        //
        // bug-2056 tried to fix this with setAngles() alone, on the rule "turn the BODY only,
        // never the view". That rule cannot work for a PLAYER: the body yaw is re-derived from the
        // client's own command angles every frame, so a bare setAngles is overwritten immediately.
        // It is an ACTOR technique. The two places this codebase actually rotates a player
        // (player.cpp:6201, :6277) both do `setAngles(); v_angle.y = ...; SetViewAngles();` - body
        // and view together - and that is the only thing that sticks.
        //
        // So do it deliberately and exactly ONCE, on the rising edge of wall cover. Pressing the
        // cover key is an explicit act and a turn there reads as taking cover; the same turn on
        // RMB release read as the camera being stolen, which is what was just removed. Skipped
        // when already roughly back-to-wall (within 60 degrees) so it never snaps for no reason.
        if (bWallOk && !m_bCoopCoverWall) {
            static cvar_t *pSnapIn = NULL;

            if (!pSnapIn) { pSnapIn = gi.Cvar_Get("coop_coverSnapBody", "1", CVAR_ARCHIVE); }
            if (pSnapIn->integer) {
                Vector vOut  = m_vCoopCoverNormal;   // points OUT of the wall, toward the player
                Vector vFwdN;

                AngleVectors(GetViewAngles(), vFwdN, NULL, NULL);
                vFwdN[2] = 0; vFwdN.normalize();
                vOut[2]  = 0; vOut.normalize();

                // [user 2026-08-23] FACE THE OPENING, NOT STRAIGHT OUT FROM THE WALL.
                // User, and the geometry is theirs: "if your back is up against a wall and you use
                // lean into the doorway, you're not going to be leaning into the opening, you're
                // going to be physically leaning AWAY from the opening. That's due to the fact
                // that your back is to the wall to begin with."
                //
                // Exactly right, and it falls out of how the lean is built: the eye-shift rotates
                // about the FORWARD axis (cg_view.c, pivot 28.7u below the eye), so the lateral
                // travel is always perpendicular to where you are LOOKING. Facing straight out of
                // the wall, leaning slides your eye along the wall face - more wall. Facing ALONG
                // the wall toward the opening, the same lean swings your eye past the jamb, which
                // is a peek.
                //
                // So bias the entry yaw toward the open side the solver found. coop_coverFaceOpen
                // is the blend: 0 = straight out of the wall (old behaviour), 1 = fully along the
                // wall at the opening. Default 0.65 - angled out AND down the wall, so you can see
                // the room you are in and still lean into the doorway.
                {
                    static cvar_t *pFace = NULL;
                    Vector         vAim = vOut;

                    // [user 2026-08-24] DEFAULT 0.65 -> 0. THIS WAS THE REPORTED DEFECT.
                    // User: "Anytime I hit the key my camera is looking at the direction of the
                    // opening and my character faces the door. The whole point is for the character
                    // to stay back against the wall."
                    //
                    // At 0.65 the entry yaw was blended 65% toward vAlong - along the wall, at the
                    // opening - which IS "face the door". vOut points out of the wall toward the
                    // player, so 0 means face straight out = back flat against the wall, which is
                    // what was asked for and what a modern cover system does.
                    //
                    // NOTE: the comment previously here claimed the default was already 0 and that
                    // "the per-frame wall pin below replaces it". Both were false - the registered
                    // default was 0.65 and NO wall pin exists anywhere in this file (grep it). A
                    // comment asserting a default it does not set is the cg_adsLeanRoll fossil
                    // again: the reader checks, believes it, and rules out the real cause.
                    // (Its "[bug-2089]" anchor is also a collision - 2089 is the stale-plan-header
                    // entry filed 2026-08-24.)
                    if (!pFace) { pFace = gi.Cvar_Get("coop_coverFaceOpen", "0", CVAR_ARCHIVE); }

                    // m_iCoopCoverSide: +1 = opening LEFT, -1 = opening RIGHT (0 = none yet)
                    if (m_iCoopCoverSide != 0 && pFace->value > 0.0f) {
                        Vector vAlong;

                        // along the wall = up x out, done by hand (CrossProduct here is the
                        // 3-argument C macro, not a returning function)
                        vAlong[0] = -vOut[1];
                        vAlong[1] =  vOut[0];
                        vAlong[2] =  0.0f;

                        // [user 2026-08-23, bug-2086] RECONCILE THE FRAMES BEFORE USING THE SIGN.
                        // vAlong is built from the wall NORMAL, so it is WORLD-space wall-left. But
                        // m_iCoopCoverSide is deliberately expressed in the player's BODY frame -
                        // see the flip at the top of the solver, which mirrors it when the player
                        // walked up FACING the wall, because lean and blindfire are body actions.
                        // Applying a body-frame sign to a world-frame vector is a 180 when those
                        // two disagree, which is every normal approach: you walk at a wall, so you
                        // face it, so the frames are opposite - and the entry turn spun the player
                        // AWAY from the opening. Measured: probe reported edgeL=30 edgeR=-1 openL=1
                        // (opening genuinely left, wire correct), and the user still ended up facing
                        // "the opposite direction of the opening". Redo the same flip test here so
                        // the sign means the same thing as the vector it multiplies.
                        {
                            Vector vbF, vbR;

                            AngleVectors(GetViewAngles(), vbF, vbR, NULL);
                            vbR[2] = 0.0f;
                            vbR.normalize();
                            if (DotProduct(vAlong, vbR) > 0.0f) { vAlong = vAlong * -1.0f; }
                        }
                        if (m_iCoopCoverSide < 0) { vAlong = vAlong * -1.0f; }
                        vAlong.normalize();
                        vAim = vOut * (1.0f - pFace->value) + vAlong * pFace->value;
                        vAim[2] = 0;
                        if (vAim.length() > 0.01f) { vAim.normalize(); } else { vAim = vOut; }
                    }

                    // only turn if we are not already facing that way (dot < 0.5 == >~60 deg off)
                    AngleVectors(GetViewAngles(), vFwdN, NULL, NULL);
                    vFwdN[2] = 0; vFwdN.normalize();
                    if (DotProduct(vFwdN, vAim) < 0.5f) {
                        Vector vNew = angles;

                        vNew[YAW]   = vectoyaw(vAim);
                        vNew[PITCH] = 0;
                        vNew[ROLL]  = 0;
                        setAngles(vNew);
                        v_angle.y = vNew[YAW];
                        SetViewAngles(v_angle);
                    }
                }
            }
        }

        m_bCoopCoverWall = bWallOk;
        m_bCoopCoverLow  = bLowOk;
        // PEEK (RMB while covered): pop out and AIM for real - the torso leaves COVER_TORSO for
        // the normal aim chain (statemap COOP_COVER_PEEK edge), the cgame shoulder-ADS camera
        // engages on the same button, and the anchored sustain above keeps the cover alive while
        // the view swings. Releasing RMB snaps the facing back onto the wall pose.
        m_bCoopCoverPeek =
            ((m_bCoopCoverWall || m_bCoopCoverLow) && (last_ucmd.buttons & BUTTON_COOPADS)) ? true : false;
        if (bWasPeek && !m_bCoopCoverPeek && (m_bCoopCoverWall || m_bCoopCoverLow)) {
            // [user 2026-08-23, bug-2055] TURN THE BODY, NEVER THE VIEW.
            // This used to SetViewAngles() on RMB release, which yanked the player's camera round
            // to the wall normal. User, live: "let go and now the camera faces the opposite
            // direction (away from the wall)... the camera is facing the wrong way entirely."
            // A 180 degree view snap, and it takes the mouse off the player for that frame.
            //
            // The correct rule was already written down 350 lines up, in the cover body-snap:
            // "Turn the BODY only - setAngles, never SetViewAngles - so the pose is right and the
            // mouse stays the player's. Taking the VIEW is what fights free-look." It was applied
            // there and not here - one entry point gated, the feature not gated (TRAPS T3).
            //
            // Body only now. The pose returns to the wall; where you are LOOKING stays yours.
            Vector vBody = angles;

            if (m_bCoopCoverWall) {
                vBody[YAW] = vectoyaw(m_vCoopCoverNormal); // back to the wall, facing out
            } else {
                Vector vIn  = Vector(0, 0, 0) - m_vCoopCoverNormal;
                vBody[YAW]  = vectoyaw(vIn); // face the low obstacle again
            }
            vBody[PITCH] = 0;
            vBody[ROLL]  = 0;
            setAngles(vBody);
        }
    }

    // [user 2026-08-22] THE PEEK STEP-OUT IS DELETED, not disabled.
    //
    // It was the real cause of bug-463: a per-frame server setOrigin() easing the collision hull
    // toward the corner while the client predictor pushed back, which is what shoved players into
    // geometry. The `if (false)` above hid it for three days by making m_bCoopCoverWall
    // unreachable; the code stayed live and would have re-armed the instant the guard came off.
    //
    // Phase 2 replaces displacement with ps->fLeanAngle - already replicated, already
    // client-predicted in shared pmove, and structurally UNABLE to move the collision hull.
    // Lean, do not teleport. m_fCoopPeekFrac survives as the pure 0..1 envelope.
    {
        float fTgt  = (m_bCoopCoverPeek && m_bCoopCoverWall) ? 1.0f : 0.0f;
        float fRate = 6.0f * level.frametime;

        if (fRate > 1.0f) {
            fRate = 1.0f;
        }
        m_fCoopPeekFrac += (fTgt - m_fCoopPeekFrac) * fRate;
        if (m_fCoopPeekFrac < 0.01f && fTgt == 0.0f) {
            m_fCoopPeekFrac = 0.0f;
        }
    }

    // BLIND FIRE: covered + fire held + a gun that makes sense poked over/around cover - but
    // NOT while peek-aiming (RMB = real aimed fire at full accuracy instead)
    // [user 2026-08-07] FIRE MODE now follows the weapon, not the animation:
    //   full-auto (SMG/MG/auto rifle) - blindfires for as long as the trigger is held
    //   semi-auto, bolt, shotgun, rocket - one press, one burst; holding does nothing more
    // and it STOPS on an empty clip so the reload can play instead of the anim dry-firing into
    // the cover. IsSemiAuto() is the weapon's own declaration, so every gun classifies itself.
    m_bCoopBlindfire = false;
    if ((m_bCoopCoverWall || m_bCoopCoverLow) && !m_bCoopCoverPeek && (last_ucmd.buttons & BUTTON_ATTACKLEFT)) {
        Weapon *weapon = GetActiveWeapon(WEAPON_MAIN);

        if (weapon
            && (weapon->GetWeaponClass() & (WEAPON_CLASS_PISTOL | WEAPON_CLASS_RIFLE | WEAPON_CLASS_SMG | WEAPON_CLASS_MG))) {
            if (!weapon->HasAmmoInClip(FIRE_PRIMARY)) {
                m_bCoopBlindfire = false;   // dry - drop out so the reload can run
            } else if (weapon->IsSemiAuto()) {
                // press EDGE only: rearming needs the trigger released first
                if (!(m_iCoopBfButtons & BUTTON_ATTACKLEFT)) {
                    m_bCoopBlindfire = true;
                }
            } else {
                m_bCoopBlindfire = true;
            }
        }
    }
    if (!(last_ucmd.buttons & BUTTON_ATTACKLEFT)) {
        m_bCoopBfShotDone = false;   // trigger released - rearm the single shot
    }
    m_iCoopBfButtons = last_ucmd.buttons;

    // HZM coop [235] - script-visible stamps for the XP system (xp.scr xp_ai_killed reads
    // self.coop_incover for the +3 covered-kill bonus and self.coop_bf_t, a level.time stamp
    // refreshed every blind-firing frame, for the +5 blindfire-kill bonus).
    {
        int iCov = (m_bCoopCoverWall || m_bCoopCoverLow) ? 1 : 0;
        if (iCov != m_iCoopVarCoverLast) {
            m_iCoopVarCoverLast = iCov;
            Vars()->SetVariable("coop_incover", iCov);
        }
        if (m_bCoopBlindfire) {
            Vars()->SetVariable("coop_bf_t", level.time);
        }
    }


    SendCoopCoverView();
}
//====

// HZM coop - lobby input bridge: enable/disable reading A/D/F from the usercmd for this player.
void Player::CoopLobbyInput(Event *ev)
{
    m_bCoopLobbyInputOn = ev->GetInteger(1) ? true : false;
    // clear the edge state so enabling mid-hold doesn't fire a phantom tap on the first frame
    m_iCoopLobbyRightPrev = 0;
    m_bCoopLobbyUsePrev   = false;
}

// HZM coop - per-frame while in the lobby: turn A/D strafe + F (+use) into self.coop_lobbyInput edges.
// No client binds are touched, so every client (host + remote) can pick a uniform and ready up, and there
// is nothing to restore when the mission launches. 31 = next uniform (D), 32 = prev (A), 33 = ready (F).
void Player::TickCoopLobbyInput(void)
{
    int  iRight;
    bool bUse;

    if (!m_bCoopLobbyInputOn) {
        return;
    }

    iRight = 0;
    if (last_ucmd.rightmove < -20) {
        iRight = -1;
    } else if (last_ucmd.rightmove > 20) {
        iRight = 1;
    }
    if (iRight != 0 && m_iCoopLobbyRightPrev == 0) {
        Vars()->SetVariable("coop_lobbyInput", (iRight > 0) ? 31 : 32);
    }
    m_iCoopLobbyRightPrev = iRight;

    bUse = (last_ucmd.buttons & BUTTON_USE) ? true : false;
    if (bUse && !m_bCoopLobbyUsePrev) {
        Vars()->SetVariable("coop_lobbyInput", 33);
    }
    m_bCoopLobbyUsePrev = bUse;
}

// HZM coop - enable/disable the lobby MOUSE-CURSOR bridge (clickable lobby UI foundation).
void Player::CoopLobbyCursor(Event *ev)
{
    m_bCoopLobbyCursorOn = ev->GetInteger(1) ? true : false;
    m_bCoopLobbyCurInit  = false; // re-seed prev-angles on the next tick so the cursor doesn't jump
}

// HZM coop - per-frame while a clickable lobby UI is up: turn the mouse (usercmd view-angle deltas) into a
// screen cursor + read the left mouse button, publishing self.coop_lobbyCurX/Y (virtual 640x480) and
// self.coop_lobbyClick (1 on a press edge; the script consumes + zeroes it). Server-side only, mirrors the
// A/D bridge - the ucmd is live during the lobby even though the view is a locked camera.
void Player::TickCoopLobbyCursor(void)
{
    int    iYaw, iPitch, dYaw, dPitch;
    float  fSens;
    bool   bAtk;
    cvar_t *pSens;

    if (!m_bCoopLobbyCursorOn) {
        return;
    }

    iYaw   = last_ucmd.angles[YAW];
    iPitch = last_ucmd.angles[PITCH];

    if (!m_bCoopLobbyCurInit) {
        m_iCoopLobbyYawPrev   = iYaw;
        m_iCoopLobbyPitchPrev = iPitch;
        m_bCoopLobbyCurInit   = true;
    }

    // short-angle wrap-safe deltas (a small mouse move stays small even across the 360 wrap)
    dYaw   = (short)(iYaw - m_iCoopLobbyYawPrev);
    dPitch = (short)(iPitch - m_iCoopLobbyPitchPrev);
    m_iCoopLobbyYawPrev   = iYaw;
    m_iCoopLobbyPitchPrev = iPitch;

    pSens = gi.Cvar_Get("coop_lobbyCursorSens", "0.03", 0);
    fSens = (pSens && pSens->value > 0.0f) ? pSens->value : 0.03f;

    // mouse RIGHT turns yaw NEGATIVE (yaw increases counter-clockwise), so negate for a natural cursor:
    // mouse right -> cursor right (+X). Pitch increases looking down -> cursor down (+Y).
    m_fCoopLobbyCurX -= (float)dYaw * fSens;
    m_fCoopLobbyCurY += (float)dPitch * fSens;

    if (m_fCoopLobbyCurX < 0.0f)   { m_fCoopLobbyCurX = 0.0f; }
    if (m_fCoopLobbyCurX > 640.0f) { m_fCoopLobbyCurX = 640.0f; }
    if (m_fCoopLobbyCurY < 0.0f)   { m_fCoopLobbyCurY = 0.0f; }
    if (m_fCoopLobbyCurY > 480.0f) { m_fCoopLobbyCurY = 480.0f; }

    Vars()->SetVariable("coop_lobbyCurX", (int)m_fCoopLobbyCurX);
    Vars()->SetVariable("coop_lobbyCurY", (int)m_fCoopLobbyCurY);

    bAtk = (last_ucmd.buttons & BUTTON_ATTACKLEFT) ? true : false;
    if (bAtk && !m_bCoopLobbyAtkPrev) {
        Vars()->SetVariable("coop_lobbyClick", 1);
    }
    m_bCoopLobbyAtkPrev = bAtk;
}
//====

float Player::GetRunSpeed() const
{
    float sprintTime;
    float sprintMult;

    sprintTime = sv_sprinttime->value;
    sprintMult = sv_sprintmult->value;
    if (g_gametype->integer != GT_SINGLE_PLAYER) {
        sprintTime = sv_sprinttime_dm->value;
        sprintMult = sv_sprintmult_dm->value;
    }

    if (sv_sprinton->integer == 1 && m_fLastSprintTime && (level.time - m_fLastSprintTime) > sprintTime) {
        return sv_runspeed->value * sprintMult;
    } else {
        return sv_runspeed->value;
    }
}

void Player::FireWeapon(int number, firemode_t mode)
{
    if (m_pVehicle || m_pTurret) {
        return;
    }

    // HZM coop [user 2026-08-07] ONE CLICK ONE SHOT for semi-auto blind fire.
    // This is the path the player's shots actually take. The gate was first put in Weapon::Shoot,
    // which is the EV_Weapon_Shoot event used by scripted/AI fire - the player's cover burst never
    // goes through it, so the revolver still emptied and the G43 still triple-tapped. Only ONE gate
    // may exist: two would each consume the single-shot allowance and suppress the round entirely.
    if (!CoopBlindfireAllowShot()) {
        return;
    }

    if (G_GetWeaponCommand(last_ucmd.buttons)) {
        // Added in OPM
        //  If there is a weapon command (like DROP), then just don't fire
        //  this prevent tricky behaviors, like silent firing
        return;
    }

    Sentient::FireWeapon(number, mode);

    if (g_gametype->integer != GT_SINGLE_PLAYER) {
        //
        // Make sure to remove the player's invulnerability
        //
        CancelInvulnerable();
    }
}

void Player::SetInvulnerable()
{
    if (IsInvulnerable()) {
        return;
    }

    if (!sv_invulnerabletime->integer) {
        return;
    }

    if (gi.Cvar_Get("g_invulnoverride", "0", 0)->integer == 1) {
        return;
    }

    if (IsDead()) {
        return;
    }

    if (IsSpectator() || GetTeam() == TEAM_SPECTATOR) {
        return;
    }

    //
    // The player can now be invulnerable
    //
    takedamage                   = DAMAGE_NO;
    m_iInvulnerableTimeRemaining = sv_invulnerabletime->integer;
    m_fLastInvulnerableTime      = level.time;
    m_fInvulnerableTimeElapsed   = level.time;

    TickInvulnerable();
}

void Player::TickInvulnerable()
{
    if (m_iInvulnerableTimeRemaining >= 0 && level.time >= m_fInvulnerableTimeElapsed) {
        if (m_iInvulnerableTimeRemaining) {
            m_fInvulnerableTimeElapsed = m_fInvulnerableTimeElapsed + 1.f;
        } else {
            SetVulnerable();
            m_fInvulnerableTimeElapsed = 0;
        }

        m_iInvulnerableTimeRemaining--;
    }
}

void Player::SetVulnerable()
{
    if (IsInvulnerable()) {
        takedamage              = DAMAGE_AIM;
        m_fLastInvulnerableTime = 0;
    }
}

bool Player::IsInvulnerable()
{
    return m_fLastInvulnerableTime != 0;
}

void Player::CancelInvulnerable()
{
    if (IsInvulnerable()) {
        SetVulnerable();
        m_iInvulnerableTimeRemaining = -1;
        gi.centerprintf(edict, " ");
    }
}

void Player::InitInvulnerable()
{
    m_fLastInvulnerableTime      = 0;
    m_iInvulnerableTimeRemaining = -1;
}

void Player::TickTeamSpawn()
{
    int timeLeft;

    if (!IsSpectator() && !IsDead() || (GetTeam() == TEAM_SPECTATOR || !client->pers.dm_primary[0])) {
        return;
    }

    timeLeft = dmManager.GetTeamSpawnTimeLeft();
    if (timeLeft == -1) {
        // Can spawn
        m_fSpawnTimeLeft = 0;
        return;
    }

    if (timeLeft == m_fSpawnTimeLeft) {
        // Still waiting
        return;
    }

    if (m_bShouldRespawn) {
        // The player can spawn
        m_fSpawnTimeLeft = 0;
        return;
    }

    m_fSpawnTimeLeft = timeLeft;
    if (timeLeft) {
        if (AllowTeamRespawn()) {
            const char *string;

            if (timeLeft == 1) {
                string = va("Next respawn in 1 second");
            } else {
                string = va("Next respawn in %d seconds", timeLeft);
            }

            gi.centerprintf(edict, string);
        }
    } else if (m_bWaitingForRespawn && AllowTeamRespawn()) {
        m_bWaitingForRespawn = false;
        m_bDeathSpectator    = false;
        EndSpectator();

        PostEvent(EV_Player_Respawn, 0);
    } else {
        m_bShouldRespawn = true;
    }
}

bool Player::ShouldForceSpectatorOnDeath() const
{
    return dmManager.GetTeamSpawnTimeLeft() > 0;
}

bool Player::HasVehicle() const
{
    return m_pVehicle != NULL;
}

void Player::setContentsSolid()
{
    edict->r.contents = CONTENTS_BODY;
}

void Player::UserSelectWeapon(bool bWait)
{
    nationality_t nationality;
    char          buf[256];

    if (g_protocol < PROTOCOL_MOHTA_MIN) {
        //
        // nationality was first introduced in 2.0
        //
        if (bWait) {
            gi.SendServerCommand(edict - g_entities, "stufftext \"wait 250;pushmenu_weaponselect\"");
        } else {
            gi.SendServerCommand(edict - g_entities, "stufftext \"pushmenu_weaponselect\"");
        }
        return;
    }

    if (GetTeam() == TEAM_AXIS) {
        nationality = GetPlayerAxisTeamType(client->pers.dm_playergermanmodel);
    } else {
        nationality = GetPlayerAlliedTeamType(client->pers.dm_playermodel);
    }

    if (bWait) {
        Q_strncpyz(buf, "stufftext \"wait 250;pushmenu ", sizeof(buf));
    } else {
        Q_strncpyz(buf, "stufftext \"pushmenu ", sizeof(buf));
    }

    if (dmflags->integer & DF_WEAPON_NO_RIFLE && dmflags->integer & DF_WEAPON_NO_SNIPER
        && dmflags->integer & DF_WEAPON_NO_SMG && dmflags->integer & DF_WEAPON_NO_MG
        && dmflags->integer & DF_WEAPON_NO_ROCKET && dmflags->integer & DF_WEAPON_NO_SHOTGUN
        && dmflags->integer & DF_WEAPON_NO_LANDMINE && !QueryLandminesAllowed()) {
        gi.cvar_set("dmflags", va("%i", dmflags->integer & ~DF_WEAPON_NO_RIFLE));
        Com_Printf("No valid weapons -- re-allowing the rifle\n");
        Q_strncpyz(client->pers.dm_primary, "rifle", sizeof(client->pers.dm_primary));
    }

    switch (nationality) {
    case NA_BRITISH:
        Q_strcat(buf, sizeof(buf), "SelectPrimaryWeapon_british\"");
        break;
    case NA_RUSSIAN:
        Q_strcat(buf, sizeof(buf), "SelectPrimaryWeapon_russian\"");
        break;
    case NA_GERMAN:
        Q_strcat(buf, sizeof(buf), "SelectPrimaryWeapon_german\"");
        break;
    case NA_ITALIAN:
        Q_strcat(buf, sizeof(buf), "SelectPrimaryWeapon_italian\"");
        break;
    default:
        Q_strcat(buf, sizeof(buf), "SelectPrimaryWeapon\"");
        break;
    }

    gi.SendServerCommand(edict - g_entities, buf);
}

void Player::PickWeaponEvent(Event *ev)
{
    if (g_gametype->integer == GT_SINGLE_PLAYER) {
        return;
    }

    UserSelectWeapon(false);
}

bool Player::AllowTeamRespawn() const
{
    if (m_bSpectator && !m_bDeathSpectator
        && (!dmManager.AllowTeamRespawn(TEAM_ALLIES) || !dmManager.AllowTeamRespawn(TEAM_AXIS))) {
        return false;
    }

    if (GetTeam() > TEAM_AXIS || GetTeam() < TEAM_ALLIES) {
        return false;
    }

    return dmManager.AllowTeamRespawn(GetTeam());
}

void Player::EventUseWeaponClass(Event *ev)
{
    if (m_pTurret || level.playerfrozen) {
        return;
    }

    Sentient::EventUseWeaponClass(ev);
}

void Player::EventAddKills(Event *ev)
{
    SafePtr<DM_Team> pTeam = GetDM_Team();
    if (pTeam) {
        // Add kills to the team
        pTeam->AddKills(this, ev->GetInteger(1));
    }
}

bool Player::CanKnockback(float minHealth) const
{
    if (m_pTurret || m_pVehicle) {
        return minHealth >= health;
    } else {
        return 1;
    }
}

void Player::EventKillAxis(Event *ev)
{
    float radius = 0;

    if (ev->NumArgs() >= 1) {
        radius = ev->GetFloat(1);
    }

    for (Sentient *pSent = level.m_HeadSentient[TEAM_GERMAN]; pSent; pSent = pSent->m_NextSentient) {
        if (radius > 0) {
            Vector delta = pSent->origin - origin;
            if (radius < delta.length()) {
                continue;
            }
        }

        pSent->Damage(this, this, pSent->max_health + 25, origin, vec_zero, vec_zero, 0, 0, MOD_NONE);
    }
}

void Player::EventGetTurret(Event *ev)
{
    ev->AddEntity(m_pTurret);
}

void Player::EventGetVehicle(Event *ev)
{
    ev->AddEntity(m_pVehicle);
}

bool Player::IsReady(void) const
{
    return m_bReady && !IsDead();
}

void Player::Spawned(void)
{
    // HZM 2026-08-11 (bug-1712) - USER: "when I click fire to spawn I also almost always fire a
    // shot, that's been an issue for a long time".
    // Respawn is triggered by BUTTON_ATTACKLEFT (player.cpp:5605). The player is necessarily
    // still holding that button on the frame he spawns, and MOHAA weapons fire from the button
    // being HELD, not only from a fresh press - so the click that asked for a spawn also pulls
    // the trigger. On a stealth map that single shot is the difference between a clean approach
    // and a blown one, which is how it kept costing whole runs.
    // Latch here rather than clearing the button mask: zeroing `buttons` would make the still-held
    // button read as a BRAND NEW press next frame (new_buttons = ucmd->buttons & ~buttons at
    // :5330), which is the same bug wearing a different hat.
    m_bFireLockUntilRelease = true;

    delegate_spawned.Execute();

    Event *ev = new Event;
    ev->AddEntity(this);

    scriptDelegate_spawned.Trigger(this, *ev);
    scriptedEvents[SE_SPAWN].Trigger(ev);
}

void Player::AddKills(int num)
{
    num_kills += num;
}

void Player::AddDeaths(int num)
{
    num_deaths += num;
}

////////////////////////////
//
// Added in OPM
//
////////////////////////////

void Player::ResetClient() {
    client->pers.teamnum = GetTeam();
    client->pers.dm_primary[0] = 0;

    //
    // Initialize important information about the client
    // and put it back into spectator
    //

    InitClient();

    if (g_gametype->integer != GT_SINGLE_PLAYER) {
        Spectator();
    }
}

qboolean Player::canUse()
{
    int touch[MAX_GENTITIES];
    int num = getUseableEntities(touch, MAX_GENTITIES);

    return num ? true : false;
}

qboolean Player::canUse(Entity *entity, bool requiresLookAt)
{
    gentity_t *hit;
    int        touch[MAX_GENTITIES];
    int        num;
    int        i;

    num = getUseableEntities(touch, MAX_GENTITIES, requiresLookAt);

    for (i = 0; i < num; i++) {
        hit = &g_entities[touch[i]];

        if (!hit->inuse || hit->entity == NULL) {
            continue;
        }

        if (hit->entity == entity) {
            return true;
        }
    }

    return false;
}

int Player::getUseableEntities(int *touch, int maxcount, bool requiresLookAt)
{
    Vector  end;
    Vector  start;
    trace_t trace;
    Vector  offset;
    Vector  max;
    Vector  min;

    if ((g_gametype->integer != GT_SINGLE_PLAYER && IsSpectator()) || IsDead()) {
        return 0;
    }

    if (m_pTurret) {
        *touch = m_pTurret->entnum;
        return 1;
    }

    if (m_pTurret) {
        return 0;
    }

    AngleVectors(m_vViewAng, offset, NULL, NULL);

    start = m_vViewPos;

    if (requiresLookAt) {
        min = Vector(-4.f, -4.f, -4.f);
        max = Vector(4.f, 4.f, 4.f);

        end[0] = start[0] + (offset[0] * 64.f);
        end[1] = start[1] + (offset[1] * 64.f);

        if (m_vViewAng[0] > 0) {
            end[2] = start[2] + (offset[2] * 88.f);
        } else {
            end[2] = start[2] + (offset[2] * 40.f);
        }

        trace = G_Trace(start, min, max, end, this, MASK_USE, false, "Player::getUseableEntity");

        offset = trace.endpos;

        min = offset - Vector(16.f, 16.f, 16.f);
        max = offset + Vector(16.f, 16.f, 16.f);
    } else {
        min = start - Vector(31.f, 31.f, 31.f);
        max = start + Vector(31.f, 31.f, 31.f);
    }

    return gi.AreaEntities(min, max, touch, maxcount);
}

void Player::Postthink(void)
{
    if (bindmaster) {
        SetViewAngles(GetViewAngles() + Vector(0, bindmaster->avelocity[YAW] * level.frametime, 0));
    }

    // HZM coop - PLAYER BLOOD TRAIL: when wounded + moving, drip ground-blood splats, exactly like wounded
    // AI (Sentient::TryDropBloodTrail). Behind its OWN toggle (coop_playerBloodTrail), DEFAULT OFF: the
    // player-spawned decal was rendering as untextured white wedges in the field (the AI hit-splat using the
    // IDENTICAL Decal code renders fine, so the cause is runtime/player-specific, not the shader) - until
    // that's root-caused live, players don't drip by default. AI blood trails (coop_bloodTrail) are unaffected.
    {
        cvar_t *pPB = gi.Cvar_Get("coop_playerBloodTrail", "1", CVAR_ARCHIVE);
        if (pPB && pPB->integer) {
            if (!blood_model.length()) {
                blood_model = "fx_bspurt.tik";
            }
            TryDropBloodTrail();
        }
    }

    // HZM coop [user 2026-08-19] GRENADE KICK: "we should be able to kick grenades when they
    // get close too" - a live grenade lying or rolling at your feet gets booted the way you
    // face. In-flight grenades (speed > 260) are not kickable; each grenade takes one kick
    // per 0.6s so a pileup doesn't machine-gun impulses. Pairs with the AI diveongrenade /
    // martyr behaviors. coop_grenadeKick 0 disables.
    {
        static cvar_t *pGK = NULL;
        if (!pGK) {
            pGK = gi.Cvar_Get("coop_grenadeKick", "1", CVAR_ARCHIVE);
        }
        static byte s_bKickIcon[MAX_CLIENTS]; // [user 2026-08-19] "you need some kind of prompt or icon"
        qboolean    bKickableNear = qfalse;
        if (pGK->integer && !IsDead() && !IsSpectator()) {
            Entity *pEnt;
            // awareness pass (wider than the kick itself): a kickable grenade within 120u in
            // front lights the KICK hud icon, so the mechanic is discoverable before it fires
            for (pEnt = findradius(NULL, origin, 120.0f); pEnt; pEnt = findradius(pEnt, origin, 120.0f)) {
                if (!pEnt->isSubclassOf(Projectile)) {
                    continue;
                }
                Projectile *pP = static_cast<Projectile *>(pEnt);
                // [user 2026-08-20] "I haven't seen the kick grenade icon in a minute even though
                // I've had tons of grenades thrown at me". The ICON's gates were the KICK's gates,
                // and all four had to hold at once - within 10 feet, slow, level with you, and
                // looked at - a window that barely opens on a grenade that lands and detonates in
                // two seconds. Awareness is now deliberately looser than the kick itself: you get
                // told a grenade is at your feet even while it is still rolling.
                if (pP->velocity.length() > 500.0f) {
                    continue;
                }
                const char *pszM = pP->model.c_str();
                // [user 2026-08-26] "granate" IS THE WHOLE POINT. The German stick grenade is
                // steilhandgranate.tik - GRANATE, the German spelling - so a filter testing only for
                // "grenade" matched every Allied, British, Italian and Russian projectile and MISSED
                // the one the player is actually being shelled with. Five models were invisible here:
                // steilhandgranate{,_ai,_primary}.tik and nebelhandgranate{,_primary}.tik. The kick
                // prompt therefore never appeared for enemy grenades in the entire trilogy.
                if (!pszM
                    || (!Q_stristr(pszM, "grenade") && !Q_stristr(pszM, "granate")
                        && !Q_stristr(pszM, "masher") && !Q_stristr(pszM, "mills")
                        && !Q_stristr(pszM, "bomba"))) {
                    continue;
                }
                Vector vT = pP->origin - origin;
                if (fabs(vT.z) > 72.0f) {
                    continue;
                }
                vT.z = 0;
                float fD = vT.length();
                if (fD > 200.0f) {
                    continue;
                }
                // no facing test for the ICON: a grenade behind you is exactly the one you most
                // need telling about, and the kick itself still requires you to turn and face it
                bKickableNear = qtrue;
                break;
            }
        }
        if (client && client->ps.clientNum < MAX_CLIENTS) {
            int cl = client->ps.clientNum;
            if (bKickableNear && !s_bKickIcon[cl]) {
                {
                    static cvar_t *pWD = NULL;
                    if (!pWD) { pWD = gi.Cvar_Get("coop_weapDebug", "0", 0); }
                    if (pWD->integer) { gi.Printf("^~^~^ KICKICON show cl=%d\n", cl); }
                }
                s_bKickIcon[cl] = 1;
                iHudDrawShader(cl, 47, "textures/hud/coop_kick");
                iHudDrawVirtualSize(cl, 47, 1);
                iHudDrawAlign(cl, 47, 1, 1); // center / center
                iHudDrawRect(cl, 47, -24, 90, 48, 48);
                {
                    static float s_fKickWhite[3] = {1.0f, 1.0f, 1.0f};
                    iHudDrawColor(cl, 47, s_fKickWhite);
                }
                iHudDrawAlpha(cl, 47, 0.85f);
            } else if (!bKickableNear && s_bKickIcon[cl]) {
                s_bKickIcon[cl] = 0;
                iHudDrawAlpha(cl, 47, 0.0f);
            }
        }
        if (pGK->integer && !IsDead() && !IsSpectator()) {
            Entity *pEnt;
            for (pEnt = findradius(NULL, origin, 64.0f); pEnt; pEnt = findradius(pEnt, origin, 64.0f)) {
                if (!pEnt->isSubclassOf(Projectile)) {
                    continue;
                }
                Projectile *pProj = static_cast<Projectile *>(pEnt);
                if (level.time < pProj->m_fCoopKickOk) {
                    continue;
                }
                if (pProj->velocity.length() > 260.0f) {
                    continue;
                }
                const char *pszModel = pProj->model.c_str();
                // same "granate" fix as the detection pass above - these two filters MUST agree, or the
                // prompt appears and the kick then refuses the very grenade it offered to kick
                if (!pszModel
                    || (!Q_stristr(pszModel, "grenade") && !Q_stristr(pszModel, "granate")
                        && !Q_stristr(pszModel, "masher")
                        && !Q_stristr(pszModel, "mills") && !Q_stristr(pszModel, "bomba"))) {
                    continue;
                }
                Vector vTo = pProj->origin - origin;
                if (fabs(vTo.z) > 56.0f) {
                    continue;
                }
                vTo.z       = 0;
                float fDist = vTo.length();
                if (fDist > 56.0f) {
                    continue;
                }
                Vector vFwd;
                AngleVectors(GetViewAngles(), vFwd, NULL, NULL);
                vFwd.z = 0;
                vFwd.normalize();
                if (fDist > 12.0f) {
                    Vector vDir = vTo * (1.0f / fDist);
                    if ((vDir * vFwd) < 0.35f) {
                        continue; // it's behind/beside you - no heel flicks
                    }
                }
                pProj->m_fCoopKickOk = level.time + 0.6f;
                pProj->Sound("coop_kick", CHAN_AUTO); // [user 2026-08-19] audible boot
                pProj->velocity      = vFwd * 460.0f + Vector(0, 0, 230.0f);
                pProj->avelocity     = Vector(crandom() * 260.0f, crandom() * 260.0f, crandom() * 260.0f);
                pProj->groundentity  = NULL;
                break; // one boot per frame
            }
        }
    }

    // HZM coop [user 2026-08-19] VAULT: "can we add vault mechanic for waist high objects" -
    // jumping into a waist-high obstacle mantles you over it instead of face-planting: knee
    // trace blocked + chest trace clear + headroom -> a stronger, forward-carried jump (the
    // jump-pad pattern: server-side velocity, prediction tolerates it). coop_vault 0 disables.
    {
        static cvar_t *pGV = NULL;
        static float   s_fVaultOk[MAX_CLIENTS];
        if (!pGV) {
            pGV = gi.Cvar_Get("coop_vault", "1", CVAR_ARCHIVE);
        }
        if (pGV->integer && !IsDead() && !IsSpectator() && groundentity && current_ucmd
            && current_ucmd->upmove > 0 && client->ps.clientNum < MAX_CLIENTS
            && level.time >= s_fVaultOk[client->ps.clientNum]) {
            Vector vFwd;
            AngleVectors(Vector(0, GetViewAngles().y, 0), vFwd, NULL, NULL);
            if ((velocity * vFwd) > 40.0f) {
                trace_t trKnee = G_Trace(
                    origin + Vector(0, 0, 20), vec_zero, vec_zero, origin + Vector(0, 0, 20) + vFwd * 46.0f, this,
                    MASK_PLAYERSOLID, qfalse, "coop_vault_knee");
                if (trKnee.fraction < 1.0f && trKnee.plane.normal[2] < 0.7f) {
                    trace_t trChest = G_Trace(
                        origin + Vector(0, 0, 56), vec_zero, vec_zero, origin + Vector(0, 0, 56) + vFwd * 52.0f,
                        this, MASK_PLAYERSOLID, qfalse, "coop_vault_chest");
                    if (trChest.fraction >= 1.0f) {
                        // [user 2026-08-27] a vault is the most athletic thing in the movement set
                        // and cost nothing at all; it is now the most expensive.
                        {
                            static cvar_t *pVC = NULL;
                            if (!pVC) { pVC = gi.Cvar_Get("coop_staminaVault", "2.5", CVAR_ARCHIVE); }
                            m_fCoopStamina -= pVC->value;
                            if (m_fCoopStamina < 0.0f) { m_fCoopStamina = 0.0f; }
                            m_fCoopStaminaHold = level.time + CoopStaminaDelay();
                        }
                        s_fVaultOk[client->ps.clientNum] = level.time + 0.8f;
                        velocity                         = vFwd * 150.0f + Vector(0, 0, 310.0f);

                        // HZM coop [user 2026-08-21] "Vaulting still doesn't seem like im really
                        // moving over something, feels more like sliding and its between the camera
                        // and animation."
                        //
                        // It slid because NOTHING presented it. The whole mechanic is the velocity
                        // line above - there was no camera work, no viewmodel work, and no state a
                        // client could even observe: no free pm_flags bit (all 16 are allocated) and
                        // no free viewmodel anim id worth spending. So tell the owning client
                        // directly, the same way limp/DBNO/cover already do.
                        //
                        // The counter is the message. An instant has no duration to describe, so
                        // cgame treats any CHANGE as "a vault just happened" and runs its own
                        // envelope; nothing has to be turned back off, and a dropped or duplicated
                        // command degrades to a missed or doubled flourish rather than a stuck view.
                        m_iCoopVaultSent++;
                        gi.SendServerCommand(edict - g_entities,
                                             "stufftext \"set coop_vaultView %d\"", m_iCoopVaultSent);
                    }
                }
            }
        }
    }
}

void Player::AdminRights(Event *ev)
{
    // FIXME: Admin manager ?
    ev->AddInteger(0);
    UNIMPLEMENTED();
}

void Player::IsAdmin(Event *ev)
{
    // FIXME: Admin manager ?
    ev->AddInteger(0);
    UNIMPLEMENTED();
}

void Player::BindWeap(Event *ev)
{
    Entity   *ent = ev->GetEntity(1);
    Listener *scriptOwner;

    //
    // FIXME: deprecate and use something else instead
    //  like implement this in the Item class directly
    //  this is dangerous to use especially if the weapon
    //  is a VehicleTurretGun, could easily mess up the camera

    if (ent) {
        scriptOwner = ent->GetScriptOwner();

        if (scriptOwner != this) {
            ent->SetScriptOwner(this);
        } else {
            ent->SetScriptOwner(NULL);
        }
    }
}

void Player::AddDeaths(Event *ev)
{
    AddDeaths(ev->GetInteger(1));
}

void Player::Dive(Event *ev)
{
    float height, airborne_duration, speed;

    Vector forwardvector = orientation[0];

    height = ev->GetFloat(1);

    if (ev->NumArgs() < 2 || ev->IsNilAt(2)) {
        airborne_duration = 1;
    } else {
        airborne_duration = ev->GetFloat(2);
    }

    speed = height * airborne_duration;

    velocity[0] += height * forwardvector[0] * (speed / 16);
    velocity[1] += height * forwardvector[1] * (speed / 16);
    velocity[2] += height * speed / 6.80f;
}

void Player::EventSetTeam(Event *ev)
{
    str        team_name;
    teamtype_t teamType;

    team_name = ev->GetString(1);
    if (!team_name.length()) {
        ScriptError("Invalid team name !");
        return;
    }

    if (Q_stricmp(team_name, "none") == 0) {
        teamType = TEAM_NONE;
    } else if (Q_stricmp(team_name, "spectator") == 0) {
        teamType = TEAM_SPECTATOR;
    } else if (Q_stricmp(team_name, "freeforall") == 0) {
        teamType = TEAM_FREEFORALL;
    } else if (Q_stricmp(team_name, "allies") == 0) {
        teamType = TEAM_ALLIES;
    } else if (Q_stricmp(team_name, "axis") == 0) {
        teamType = TEAM_AXIS;
    } else {
        ScriptError("Unknown team name \"%s\"\n", team_name.c_str());
        return;
    }

    SetTeam(teamType);

    gi.DPrintf("Player::SetTeam : Player is now on team \"%s\"\n", team_name.c_str());
}

void Player::FreezeControls(Event *ev)
{
    m_bFrozen = ev->GetBoolean(1);
}

void Player::GetConnState(Event *ev)
{
    // Assume CS_ACTIVE
    ev->AddInteger(4);

    gi.DPrintf(
        "getconnstate is deprecated and will always return 4 (CS_ACTIVE).\nThe player is created only when the client "
        "begins (CS_ACTIVE state).\n"
    );
}

void Player::GetDamageMultiplier(Event *ev)
{
    ev->AddFloat(damage_multiplier);
}

void Player::GetDeaths(Event *ev)
{
    ev->AddInteger(num_deaths);
}

void Player::GetKillHandler(Event *ev)
{
    if (m_killedLabel.IsSet()) {
        m_killedLabel.GetScriptValue(&ev->GetValue());
    } else {
        ev->AddNil();
    }
}

void Player::GetKills(Event *ev)
{
    ev->AddInteger(num_kills);
}

void Player::GetMoveSpeedScale(Event *ev)
{
    ev->AddFloat(speed_multiplier[0]);
}

void Player::GetLegsState(Event *ev)
{
    const char *name;

    if (currentState_Legs != NULL) {
        name = currentState_Legs->getName();
    } else {
        name = "none";
    }

    ev->AddString(name);
}

void Player::GetStateFile(Event *ev)
{
    int clientNum = G_GetClientNumber(this);

    if (m_sStateFile.length()) {
        ev->AddString(m_sStateFile);
    } else {
        ev->AddString(g_statefile->string);
    }
}

void Player::GetTorsoState(Event *ev)
{
    const char *name;

    if (currentState_Torso != NULL) {
        name = currentState_Torso->getName();
    } else {
        name = "none";
    }

    ev->AddString(name);
}

void Player::GetUserInfo(Event *ev)
{
    ev->AddString(client->pers.userinfo);
}

void Player::HideEntity(Event *ev)
{
    // FIXME: todo
    UNIMPLEMENTED();
}

void Player::ShowEntity(Event *ev)
{
    // FIXME: REDO
    UNIMPLEMENTED();
}

void Player::Inventory(Event *ev)
{
    Entity         *ent = NULL;
    ScriptVariable *ref = new ScriptVariable, *array = new ScriptVariable;
    int             i = 0;

    ref->setRefValue(array);

    for (i = 0; i < inventory.NumObjects(); i++) {
        ent = G_GetEntity(inventory[i]);

        if (ent == NULL) {
            continue;
        }

        ScriptVariable *index = new ScriptVariable, *value = new ScriptVariable;

        index->setIntValue(i + 1);
        value->setListenerValue((Listener *)ent);

        ref->setArrayAt(*index, *value);
    }

    ev->AddValue(*array);
}

void Player::InventorySet(Event *ev)
{
    ScriptVariable  array;
    ScriptVariable *value;
    Entity         *ent;
    int             arraysize;

    if (ev->IsNilAt(1)) {
        // Just clear the inventory
        inventory.ClearObjectList();
        return;
    }

    // Retrieve the array
    array = ev->GetValue(1);

    // Cast the array
    array.CastConstArrayValue();
    arraysize = array.arraysize();

    // Detach all active weapons and free the inventory

    if (inventory.NumObjects() > 0) {
        inventory.FreeObjectList();
    }

    if (arraysize < 1) {
        return;
    }

    // Allocate an inventory
    for (int i = 1; i <= arraysize; i++) {
        // Retrieve the value from the array
        value = array[i];

        // Get the entity from the value
        ent = (Entity *)value->entityValue();

        if (ent == NULL || !ent->edict->inuse) {
            continue;
        }

        // Add the entity to the inventory
        inventory.AddObject(ent->entnum);
    }

    // Clear the variable
    array.Clear();
}

void Player::LeanLeftHeld(Event *ev)
{
    Player *player     = NULL;
    int     buttonheld = 0;

    player = (Player *)this;

    buttonheld = !!(player->buttons & BUTTON_LEAN_LEFT);

    ev->AddInteger(buttonheld);
}

void Player::LeanRightHeld(Event *ev)
{
    Player *player     = NULL;
    int     buttonheld = 0;

    player = (Player *)this;

    buttonheld = !!(player->buttons & BUTTON_LEAN_RIGHT);

    ev->AddInteger(buttonheld);
}

void Player::PlayLocalSound(Event *ev)
{
    str      soundName = ev->GetString(1);
    qboolean loop      = false;
    float    time;

    if (ev->NumArgs() > 1) {
        loop = ev->GetBoolean(2);
    }

    if (ev->NumArgs() > 2) {
        time = ev->GetFloat(3);
    } else {
        time = 0.0f;
    }

    AliasListNode_t *alias = NULL;

    const char *found = gi.GlobalAlias_FindRandom(soundName, &alias);

    if (found == NULL) {
        gi.DPrintf("ERROR: Player::PlayLocalSound: %s needs to be aliased - Please fix.\n", soundName.c_str());
        return;
    }

#ifdef OPM_FEATURES
    gi.MSG_SetClient(client->ps.clientNum);

    gi.MSG_StartCGM(CGM_PLAYLOCALSOUND);
    gi.MSG_WriteString(found);
    gi.MSG_WriteBits(!!loop, 1);
    gi.MSG_WriteFloat(time);
    gi.MSG_WriteFloat(alias->volume);
    gi.MSG_EndCGM();

    return;
#endif

    if (loop) {
        edict->s.loopSound        = gi.soundindex(found, alias->streamed);
        edict->s.loopSoundVolume  = 1.0f;
        edict->s.loopSoundMinDist = 0;
        edict->s.loopSoundMaxDist = 96;
        edict->s.loopSoundPitch   = 1.0f;
        edict->s.loopSoundFlags   = 1; // local sound
    } else {
        gi.Sound(&edict->s.origin, entnum, CHAN_LOCAL, found, 1.0f, 0, 1.0f, 96, alias->streamed);
    }
}

void Player::RunHeld(Event *ev)
{
    Player *player     = NULL;
    int     buttonheld = 0;

    player = (Player *)this;

    buttonheld = !!(player->buttons & BUTTON_RUN);

    ev->AddInteger(buttonheld);
}

void Player::SecFireHeld(Event *ev)
{
    Player *player     = NULL;
    int     buttonheld = 0;

    player = (Player *)this;

    buttonheld = !!(player->buttons & BUTTON_ATTACKRIGHT);

    ev->AddInteger(buttonheld);
}

void Player::SetAnimSpeed(Event *ev)
{
    float   speed;
    Player *player = (Player *)this;

    speed = ev->GetFloat(1);

    if (speed < 0.0f) {
        speed = 0.0f;
    }

    UNIMPLEMENTED();
}

void Player::SetKillHandler(Event *ev)
{
    if (ev->IsNilAt(1) || (ev->IsStringAt(1) && !ev->GetString(1).icmp("none"))) {
        m_killedLabel.Clear();
    } else {
        m_killedLabel.SetScript(ev->GetValue(1));
    }
}

void Player::SetSpeed(Event *ev)
{
    float   speed;
    Player *player    = (Player *)this;
    int     clientNum = G_GetClientNumber(this);
    int     index     = 0;

    speed = ev->GetFloat(1);

    if (speed < 0.0f) {
        speed = 0.0f;
    }

    if (ev->NumArgs() > 1) {
        index = ev->GetInteger(2);

        /* Reserve a space for moveSpeedScale */
        if (index < 1 || index > MAX_SPEED_MULTIPLIERS) {
            gi.Printf("Player::SetSpeed : invalid index %d. Index must be between 1-%d\n", index, speed_multiplier);

            return;
        }
    }

    speed_multiplier[index] = speed;
}

void Player::SetStateFile(Event *ev)
{
    int      clientNum = G_GetClientNumber(this);
    qboolean bRemove   = false;
    str      string;

    if (ev->NumArgs() <= 0) {
        bRemove = true;
    } else {
        string = ev->GetString(1);

        if (!string) {
            bRemove = true;
        }
    }

    if (bRemove) {
        m_sStateFile = "";
    } else {
        m_sStateFile = string;
    }
}

void Player::StopLocalSound(Event *ev)
{
    str   soundName = ev->GetString(1);
    float time;

    if (ev->NumArgs() > 1) {
        time = ev->GetFloat(2);
    } else {
        time = 0.0f;
    }

    AliasListNode_t *alias = NULL;
    const char      *found = gi.GlobalAlias_FindRandom(soundName, &alias);

    if (found == NULL) {
        gi.DPrintf("ERROR: Player::StopLocalSound: %s needs to be aliased - Please fix.\n", soundName.c_str());
        return;
    }

    edict->s.loopSound = 0;
    gi.StopSound(entnum, CHAN_LOCAL);
}

void Player::Userinfo(Event *ev)
{
    if (!client) {
        ScriptError("Entity is probably not of player type - userinfo\n");
        return;
    }

    ev->AddString(client->pers.userinfo);
}

int Player::GetNumKills(void) const
{
    return num_kills;
}

int Player::GetNumDeaths(void) const
{
    return num_deaths;
}

void Player::InitModelFps(void)
{
    char  model_name[MAX_STRING_TOKENS];
    char *model_replace;

    Q_strncpyz(model_name, model.c_str(), sizeof(model_name));
    size_t len = strlen(model_name);

    model_replace = model_name + len - 4;

    Q_strncpyz(model_replace, "_fps.tik", sizeof(model_name) - (model_replace - model_name));

    m_fpsTiki = gi.modeltiki(model_name);
}

void Player::ThinkFPS(void)
{
    if (!animDoneVM) {
        int    index;
        float  anim_time;
        vma_t *vma = vmalist.find(m_sVMcurrent);

        index = m_fpsTiki == NULL ? -1 : gi.Anim_NumForName(m_fpsTiki, m_sVMAcurrent);

        if (index >= 0) {
            anim_time = gi.Anim_Time(m_fpsTiki, index);

            if (m_fVMAtime < anim_time) {
                if (vma) {
                    m_fVMAtime += level.frametime * vma->speed;
                } else {
                    m_fVMAtime += level.frametime;
                }
            } else {
                animDoneVM = true;
                m_fVMAtime = 0;

                Unregister(STRING_VIEWMODELANIM_DONE);
            }
        } else {
            animDoneVM = true;
            m_fVMAtime = 0;

            Unregister(STRING_VIEWMODELANIM_DONE);
        }
    }
}

void Player::EventGetViewModelAnim(Event *ev)
{
    ev->AddString(m_sVMcurrent);
}

void Player::EventGetViewModelAnimFinished(Event *ev)
{
    ev->AddInteger(animDoneVM);
}

void Player::EventGetViewModelAnimValid(Event *ev)
{
    str  anim_name = ev->GetString(1);
    str  fullanim;
    bool bFullAnim = false;

    if (ev->NumArgs() > 1) {
        bFullAnim = ev->GetBoolean(2);
    }

    if (!bFullAnim) {
        // Copy the item prefix and the anim name
        Item *item = GetActiveWeapon(WEAPON_MAIN);

        if (!item) {
            item = (Item *)newActiveWeapon.weapon.Pointer();
        }

        if (item) {
            fullanim = GetItemPrefix(item->getName()) + str("_") + anim_name;
        } else {
            fullanim = "unarmed_" + anim_name;
        }
    } else {
        fullanim = anim_name;
    }

    if (!m_fpsTiki || gi.Anim_NumForName(m_fpsTiki, fullanim.c_str()) < 0) {
        ev->AddInteger(0);
    } else {
        ev->AddInteger(1);
    }
}

#ifdef OPM_FEATURES

void Player::EventEarthquake(Event *ev)
{
    float    duration    = ev->GetFloat(1);
    float    magnitude   = ev->GetFloat(2);
    qboolean no_rampup   = ev->GetBoolean(3);
    qboolean no_rampdown = ev->GetBoolean(4);

    // full realistic, smooth earthquake
    if (ev->NumArgs() > 4) {
        Vector location = ev->GetVector(5);
        float  radius   = 1.0f;

        if (ev->NumArgs() > 5) {
            radius = ev->GetFloat(6);
        }

        gi.SendServerCommand(
            edict - g_entities,
            "eq %f %f %d %d %f %f %f %f",
            duration,
            magnitude,
            no_rampup,
            no_rampdown,
            location[0],
            location[1],
            location[2],
            radius
        );
    } else {
        gi.SendServerCommand(edict - g_entities, "eq %f %f %d %d", duration, magnitude, no_rampup, no_rampdown);
    }
}

void Player::SetClientFlag(Event *ev)
{
    str name = ev->GetString(1);

    gi.SendServerCommand(client->ps.clientNum, "cf %s", name.c_str());
}

void Player::SetEntityShader(Event *ev)
{
    Entity  *entity     = ev->GetEntity(1);
    str      shadername = ev->GetString(2);
    qboolean fReset     = false;

    if (entity == NULL) {
        ScriptError("Invalid entity !");
    }

    if (!shadername.length()) {
        shadername = "default";
        fReset     = true;
    }

    gi.SendServerCommand(edict - g_entities, "setshader %d %s %d", entity->entnum, shadername.c_str(), fReset);
}

void Player::SetLocalSoundRate(Event *ev)
{
    str   name = ev->GetString(1);
    float rate = ev->GetFloat(2);
    float time;

    if (ev->NumArgs() > 2) {
        time = ev->GetFloat(3);
    } else {
        time = 0.0f;
    }

    AliasListNode_t *alias = NULL;
    const char      *found = gi.GlobalAlias_FindRandom(name, &alias);

    if (found == NULL) {
        gi.DPrintf("ERROR: Player::SetLocalSoundRate: %s needs to be aliased - Please fix.\n", name.c_str());
        return;
    }

    gi.MSG_SetClient(client->ps.clientNum);

    // FIXME...
    /*
    gi.MSG_StartCGM( CGM_SETLOCALSOUNDRATE );
        gi.MSG_WriteString( found );
        gi.MSG_WriteFloat( rate );
        gi.MSG_WriteFloat( time );
    gi.MSG_EndCGM();
    */
}

void Player::SetVMASpeed(Event *ev)
{
    str   name  = ev->GetString(1);
    float speed = ev->GetFloat(2);

    if (!client || !sv_specialgame->integer) {
        return;
    }

    vma_t *vma = &vmalist[name];

    if (speed < 0.0f) {
        speed = 0.0f;
    }

    vma->name  = name;
    vma->speed = speed;

    // FIXME...
    /*
    gi.MSG_SetClient( edict - g_entities );

    gi.MSG_StartCGM( CGM_SETVMASPEED );
        gi.MSG_WriteString( name );
        gi.MSG_WriteFloat( speed );
    gi.MSG_EndCGM();
    */
}

void Player::VisionGetNaked(Event *ev)
{
    // return the global vision
    if (!m_sVision.length()) {
        ev->AddString(vision_current);
    } else {
        ev->AddString(m_sVision);
    }
}

void Player::VisionSetBlur(Event *ev)
{
    float blur_level = ev->GetFloat(1);
    float fade_time;

    if (ev->NumArgs() > 1) {
        fade_time = ev->GetFloat(2);
    } else {
        fade_time = 0.0f;
    }

    gi.SendServerCommand(edict - g_entities, "vsb %f %f", blur_level, fade_time);
}

void Player::VisionSetNaked(Event *ev)
{
    str   vision = ev->GetString(1);
    float fade_time;
    float phase;

    if (ev->NumArgs() > 1) {
        fade_time = ev->GetFloat(2);
    } else {
        fade_time = 0.0f;
    }

    if (ev->NumArgs() > 2) {
        phase = ev->GetFloat(3);
    } else {
        phase = 0.0f;
    }

    if (!vision.length()) {
        vision = vision_current;
    }

    if (vision.length() >= MAX_STRING_TOKENS) {
        ScriptError("vision_name exceeds the maximum vision name limit (256) !\n");
    }

    m_sVision = vision;

    gi.SendServerCommand(edict - g_entities, "vsn %s %f %f", vision.c_str(), fade_time, phase);
}
#endif
