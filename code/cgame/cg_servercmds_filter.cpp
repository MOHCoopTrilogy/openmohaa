/*
===========================================================================
Copyright (C) 2025 the OpenMoHAA team

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
// cg_servercmds_filter.c -- filtered server commands

#include "cg_local.h"
#include "cg_servercmds_filter.h"

//
// List of variables allowed to be changed by the server
//
static const char *whiteListedVariables[] = {
    // some mods set this variable to make the sky uniform
    "r_fastsky",

    "ui_hud",
    "subtitle0",
    "subtitle1",
    "subtitle2",
    "subtitle3",
    "name",

    // for 3rd person server
    "cg_3rd_person",
    "cg_cameraverticaldisplacement",

    // HZM coop - allow the server to mix client audio for scripted cinematics (e.g. m3l1a Omaha
    // ramp-drop): duck effects/ambient so the music takes over, then restore. Without these the
    // server-stuffed volume changes are silently filtered on the client.
    "s_volume",
    "s_musicvolume",
    "s_ambientvolume",
    "s_sfxduck",

    // HZM coop - pre-mission lobby: let the server lock in each player's chosen uniform as their default
    // (dm_playermodel is CVAR_USERINFO|CVAR_ARCHIVE, so it carries into the launched mission AND persists),
    // and force the bottom-left roster / control-help ihuddraw layer on for every client (host + remote)
    // even while the engine HUD is hidden - neither is settable via server stufftext without this.
    "dm_playermodel",
    "cg_huddraw_force"
};

//
// List of variables allowed to be changed by the server
//
static const char *whiteListedLocalServerVariables[] = {"ui_hidemouse", "ui_showmouse", "cg_marks_add"};

//
// List of commands allowed to be executed by the server
//
static const char *whiteListedCommands[] = {
    //
    // HUD
    //==========
    "pushmenu",
    "pushmenu_teamselect",
    "pushmenu_weaponselect",
    "popmenu",
    "globalwidgetcommand", // used for mods adding custom HUDs
    "ui_addhud",
    "ui_removehud",
    "echo", // to print stuff client-side

    //
    // Sounds
    //==========
    "tmstart",
    "tmstartloop",
    "tmstop",
    "tmvolume",
    "play",
    "playmp3",
    "stopmp3",

    //
    // Misc
    //==========
    "primarydmweapon",
    "wait",
    "+moveup", // workaround for mods that want to prevent inactivity when handling the spectate
    "-moveup",
    "screenshot",
    "screenshotJPEG",
    "levelshot",
    "`stufftext" // Stufftext detection from Reborn, the player gets kicked without it
};

//
// List of commands allowed to be executed locally
// (when the client also runs the server)
//
static const char *whiteListedLocalServerCommands[] = {
    // Used by briefings
    "spmap",
    "map",
    "disconnect",

    "cinematic",
    "showmenu",
    "hidemenu"
};

/*
====================
CG_IsVariableAllowed

Returns whether or not the variable should be filtered
====================
*/
static qboolean CG_IsVariableAllowed(const char *name)
{
    size_t i;

    // HZM coop - always allow the server to set our own mod-namespaced client cvars (coop_*). These are
    // used for script->client HUD/UI bridges (e.g. the briefing ready-up overlay's coop_gate_* cvars).
    // They're user-created and mod-controlled, so this is safe, and it removes any doubt about the
    // user-created-cvar path in CG_IsSetVariableAllowed.
    if (!Q_stricmpn(name, "coop_", 5)) {
        return qtrue;
    }

    for (i = 0; i < ARRAY_LEN(whiteListedVariables); i++) {
        if (!Q_stricmp(name, whiteListedVariables[i])) {
            return qtrue;
        }
    }

    if (cgs.localServer) {
        for (i = 0; i < ARRAY_LEN(whiteListedLocalServerVariables); i++) {
            if (!Q_stricmp(name, whiteListedLocalServerVariables[i])) {
                return qtrue;
            }
        }
    }

    // Filtered
    return qfalse;
}

/*
====================
CG_IsSetVariableFiltered

Returns whether or not the variable should be filtered
====================
*/
static qboolean CG_IsSetVariableAllowed(const char *name, char type)
{
    cvar_t *var;

    if (CG_IsVariableAllowed(name)) {
        return qtrue;
    }

    if (type != 'a' && type != 's') {
        // Only allow ephemeral or userinfo variables

        var = cgi.Cvar_Find(name);
        if (!var) {
            // Allow as it doesn't exist
            return qtrue;
        }

        if (var->flags & CVAR_USER_CREATED) {
            // Allow, it's user-created, wouldn't cause issues
            return qtrue;
        }
    }

    // Filtered
    return qfalse;
}

/*
====================
CG_IsCommandAllowed

Returns whether or not the variable should be filtered
====================
*/
static qboolean CG_IsCommandAllowed(const char *name)
{
    size_t  i;
    cvar_t *var;

    for (i = 0; i < ARRAY_LEN(whiteListedCommands); i++) {
        if (!Q_stricmp(name, whiteListedCommands[i])) {
            return qtrue;
        }
    }

    if (cgs.localServer) {
        // Allow more commands when the client is hosting the server
        // Mostly used on single-player mode, like when briefings switch to the next map
        for (i = 0; i < ARRAY_LEN(whiteListedLocalServerCommands); i++) {
            if (!Q_stricmp(name, whiteListedLocalServerCommands[i])) {
                return qtrue;
            }
        }
    }

    //
    // Test variables
    //
    if (CG_IsVariableAllowed(name)) {
        return qtrue;
    }

    var = cgi.Cvar_Find(name);
    if (var && (var->flags & CVAR_USER_CREATED)) {
        // Allow, it's user-created, wouldn't cause issues
        return qtrue;
    }

    return qfalse;
}

/*
====================
RemoveEndToken
====================
*/
static qboolean RemoveEndToken(char *com_token)
{
    char *p;

    for (p = com_token; p[0]; p++) {
        if (*p == ';') {
            *p = 0;
            return qtrue;
        }
    }

    return qfalse;
}

/*
====================
CG_IsStatementAllowed

Returns whether or not the statement is filtered
====================
*/
qboolean CG_IsStatementAllowed(char *cmd)
{
    char    *parsed;
    char     com_token[256];
    qboolean bNextStatement = qfalse;

    parsed = cmd;

    for (Q_strncpyz(com_token, COM_ParseExt(&parsed, qtrue), sizeof(com_token)); com_token[0];
         Q_strncpyz(com_token, COM_ParseExt(&parsed, qtrue), sizeof(com_token))) {
        bNextStatement = RemoveEndToken(com_token);

        if (com_token[0] == ';') {
            continue;
        }

        if (!Q_stricmp(com_token, "set") || !Q_stricmp(com_token, "setu") || !Q_stricmp(com_token, "seta")
            || !Q_stricmp(com_token, "sets") || !Q_stricmp(com_token, "append")) {
            char type;

            if (Q_stricmp(com_token, "append")) {
                type = com_token[3];
            } else {
                type = 0;
            }

            //
            // variable
            //
            Q_strncpyz(com_token, COM_ParseExt(&parsed, qfalse), sizeof(com_token));
            bNextStatement |= RemoveEndToken(com_token);
            if (com_token[0] == ';') {
                continue;
            }

            if (!CG_IsSetVariableAllowed(com_token, type)) {
                return qfalse;
            }
        } else if (!Q_stricmp(com_token, "exec")) {
            // HZM coop - allow server-driven exec of MOD-NAMESPACED cfg paths only. The coop
            // framework drives clients through tiny cfgs (coop_mod/cfg/detect.cfg = the mod
            // handshake, ui/coop_objectives/*, ui/loadout/* = the armory). A blanket "exec"
            // whitelist would let hostile servers run arbitrary local cfgs - path-scoping keeps
            // the protection while unblocking the mod (bug-597: this filter silently ate the
            // handshake, the FOV re-apply, the armory pick resend AND the lobby LOADOUT button).
            Q_strncpyz(com_token, COM_ParseExt(&parsed, qfalse), sizeof(com_token));
            bNextStatement |= RemoveEndToken(com_token);
            if (Q_stricmpn(com_token, "ui/loadout/", 11) && Q_stricmpn(com_token, "ui/coop_", 8)
                && Q_stricmpn(com_token, "coop_mod/", 9)) {
                return qfalse;
            }
        } else if (!Q_stricmp(com_token, "vstr")) {
            // HZM coop - allow vstr of coop-namespaced or user-created cvars: the armory resend
            // asks the client to fire its own archived coop_loA1..4; the FOV/auth re-applies use
            // user-created cvars (g_m2l1 style). Engine-owned cvars stay off-limits.
            cvar_t *var;

            Q_strncpyz(com_token, COM_ParseExt(&parsed, qfalse), sizeof(com_token));
            bNextStatement |= RemoveEndToken(com_token);
            if (Q_stricmpn(com_token, "coop_", 5)) {
                var = cgi.Cvar_Find(com_token);
                if (!var || !(var->flags & CVAR_USER_CREATED)) {
                    return qfalse;
                }
            }
        } else {
            //
            // normal command
            //
            if (!CG_IsCommandAllowed(com_token)) {
                return qfalse;
            }
        }

        if (!bNextStatement) {
            // Skip up to the next statement
            while (parsed && parsed[0]) {
                char c = parsed[0];

                parsed++;
                if (c == '\n' || c == ';') {
                    break;
                }
            }
        }
    }

    return qtrue;
}
