/*
===========================================================================
Copyright (C) 2025-2026 the OpenMoHAA team

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
// cmd_filter.c -- the server-command statement rules, shared by cgame (layer 1) and the exe (layer 2)
//
// HZM coop [SEC2] The whitelists and helpers below were moved here verbatim from
// cgame/cg_servercmds_filter.cpp (bug-597, bug-1502, bug-1849, bug-1991, bug-2580) so the reception
// filter in cgame.dll and the execution filter in openmohaa.exe run ONE implementation. Any change here
// changes both layers; docs/tools/sec2_filter_selftest pins the two against each other.

#include "cmd_filter.h"

//
// List of variables allowed to be changed by the server
//
static const char *whiteListedVariables[] = {
    // some mods set this variable to make the sky uniform
    "r_fastsky",
    // HZM coop [user 2026-08-16] bug-1849: r_novis, so a map whose vis was never compiled for
    // the coop route can switch PVS culling off for the players who take it. Precedent is
    // r_fastsky directly above - a renderer cvar the server is already trusted to set. r_novis
    // disables CULLING only; solid geometry still draws and still occludes.
    "r_novis",

    "ui_hud",
    "subtitle0",
    "subtitle1",
    "subtitle2",
    "subtitle3",
    "name",

    // for 3rd person server
    "cg_3rd_person",
    "cg_cameraverticaldisplacement",

    // HZM coop [user 2026-08-21] thirdperson.scr switches free cam / chase by stuffing cg_freecam next
    // to cg_3rd_person; with only cg_3rd_person listed every "set cg_freecam" was silently dropped, so
    // both third-person modes rendered identically and the archived value locked players in free cam.
    "cg_freecam",

    // HZM coop - allow the server to mix client audio for scripted cinematics (e.g. m3l1a Omaha
    // ramp-drop): duck effects/ambient so the music takes over, then restore. Without these the
    // server-stuffed volume changes are silently filtered on the client.
    "s_volume",
    "s_musicvolume",
    "s_ambientvolume",
    "s_sfxduck",

    // HZM coop [user 08-06] bug-1502 - marker cvars for the CLIENT-CAPTURED music/ambient duck
    // (cg_view.c CG_UpdateScriptedAudioDucks): the server stuffs "start/stop ducking" + a target and
    // cgame restores the player's OWN captured volume. Two independent channels.
    "coop_duckMusicTrigger",
    "coop_duckMusicTarget",
    "coop_duckMusicInDur",
    "coop_duckMusicOutDur",
    "coop_duckAmbientTrigger",
    "coop_duckAmbientTarget",
    "coop_duckAmbientInDur",
    "coop_duckAmbientOutDur",

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

//
// HZM coop [SEC2] server-write guard list - GENERATED from the tree by docs/tools/sec2_guardlist.py.
//
typedef struct {
    const char *name;
    int         kind;
    int         cls;
} srvGuard_t;

static const srvGuard_t srvGuards[] = {
#define SRVG(n, k, c) {n, k, c},
#include "cmd_srvguard.h"
#undef SRVG
    {NULL, 0, 0}
};

static qboolean SrvFilter_GuardedWriteAllowed(
    const srvFilterEnv_t *env, const char *name, const char *value, int depth
);

/*
====================
SrvFilter_DropReason

Fail-closed drop with a machine-readable sub-reason, printed under covtrace as
"^~^~^ <tag> VDROP <reason> [detail]". The tag says which layer dropped it (COVC = cgame reception,
COVX = exe execution), so a layer-2 test can never be satisfied by a layer-1 drop (TRAPS T14).
====================
*/
qboolean SrvFilter_DropReason(const srvFilterEnv_t *env, const char *reason, const char *detail)
{
    if (env && env->covtrace && env->Printf) {
        if (detail && *detail) {
            env->Printf("^~^~^ %s VDROP %s %s\n", env->tag, reason, detail);
        } else {
            env->Printf("^~^~^ %s VDROP %s\n", env->tag, reason);
        }
    }
    return qfalse;
}

/*
====================
SrvFilter_Tokenize / SrvFilter_ArgsFrom

A faithful port of the engine command tokenizer (qcommon/cmd.c Cmd_TokenizeString2 with
ignoreQuotes=qfalse) and Cmd_ArgsFrom, into caller storage. cgame does not link cmd.c, and the exe must
not clobber Cmd_Argv while a line is being filtered, so neither side can use the engine's own copy.
Fed one already-split statement at a time: no ';' handling, only quotes and // and slash-star comments.
====================
*/
void SrvFilter_Tokenize(srvTokens_t *tok, const char *text_in)
{
    const char *text;
    char       *textOut;

    tok->argc     = 0;
    tok->overflow = qfalse;
    if (!text_in) {
        return;
    }
    if (strlen(text_in) >= MAX_STRING_CHARS) {
        tok->overflow = qtrue; // the caller fails closed
        return;
    }
    text    = text_in;
    textOut = tok->buf;

    while (1) {
        if (tok->argc == SRVF_MAX_TOKENS) {
            return; // usually something malicious
        }

        while (1) {
            // skip whitespace
            while (*text && (unsigned char)*text <= ' ') {
                text++;
            }
            if (!*text) {
                return; // all tokens parsed
            }
            // skip // comments
            if (text[0] == '/' && text[1] == '/') {
                return; // all tokens parsed
            }
            // skip /* */ comments
            if (text[0] == '/' && text[1] == '*') {
                while (*text && (text[0] != '*' || text[1] != '/')) {
                    text++;
                }
                if (!*text) {
                    return; // all tokens parsed
                }
                text += 2;
            } else {
                break; // ready to parse a token
            }
        }

        // handle quoted strings
        if (*text == '"') {
            tok->argv[tok->argc++] = textOut;
            text++;
            while (*text && *text != '"') {
                *textOut++ = *text++;
            }
            *textOut++ = 0;
            if (!*text) {
                return; // all tokens parsed
            }
            text++;
            continue;
        }

        // regular token
        tok->argv[tok->argc++] = textOut;

        // skip until whitespace, quote, or comment
        while ((unsigned char)*text > ' ') {
            if (text[0] == '"') {
                break;
            }
            if (text[0] == '/' && text[1] == '/') {
                break;
            }
            if (text[0] == '/' && text[1] == '*') {
                break;
            }
            *textOut++ = *text++;
        }
        *textOut++ = 0;
        if (!*text) {
            return; // all tokens parsed
        }
    }
}

// Cmd_ArgsFrom (cmd.c): argv[arg]..argv[argc-1] joined with single spaces. qfalse when it does not fit,
// so a caller refuses a value rather than modelling a truncated prefix.
qboolean SrvFilter_ArgsFrom(int argc, char **argv, int arg, char *out, size_t outSize)
{
    size_t used = 0;
    int    i;

    if (!outSize) {
        return qfalse;
    }
    out[0] = 0;
    if (arg < 0) {
        arg = 0;
    }
    for (i = arg; i < argc; i++) {
        size_t len = strlen(argv[i]);

        if (used + len + (i != argc - 1 ? 1 : 0) >= outSize) {
            return qfalse;
        }
        memcpy(out + used, argv[i], len);
        used += len;
        if (i != argc - 1) {
            out[used++] = ' ';
        }
        out[used] = 0;
    }

    return qtrue;
}

/*
====================
SrvFilter_StatementLength

Cbuf_Execute's line scan (qcommon/cmd.c), verbatim in structure: break on an unquoted ';', a '\n' or
'\r', or just after the '/' that closes a slash-star comment, honoring quote parity and both comment
forms, with the comment state persisting across statements through inStar/inSlash.
====================
*/
int SrvFilter_StatementLength(const char *text, int remaining, qboolean *inStar, qboolean *inSlash)
{
    int quotes = 0;
    int i;

    for (i = 0; i < remaining; i++) {
        if (text[i] == '"') {
            quotes++;
        }

        if (!(quotes & 1)) {
            if (i < remaining - 1) {
                if (!*inStar && text[i] == '/' && text[i + 1] == '/') {
                    *inSlash = qtrue;
                } else if (!*inSlash && text[i] == '/' && text[i + 1] == '*') {
                    *inStar = qtrue;
                } else if (*inStar && text[i] == '*' && text[i + 1] == '/') {
                    *inStar = qfalse;
                    i++; // Cbuf NULs out the terminating '/'
                    break;
                }
            }
            if (!*inSlash && !*inStar && text[i] == ';') {
                break;
            }
        }
        if (!*inStar && (text[i] == '\n' || text[i] == '\r')) {
            *inSlash = qfalse;
            break;
        }
    }

    return i;
}

static qboolean SrvFilter_IsExplicitVariable(const srvFilterEnv_t *env, const char *name)
{
    size_t i;

    for (i = 0; i < ARRAY_LEN(whiteListedVariables); i++) {
        if (!Q_stricmp(name, whiteListedVariables[i])) {
            return qtrue;
        }
    }

    if (env->localServer) {
        for (i = 0; i < ARRAY_LEN(whiteListedLocalServerVariables); i++) {
            if (!Q_stricmp(name, whiteListedLocalServerVariables[i])) {
                return qtrue;
            }
        }
    }

    return qfalse;
}

/*
====================
SrvFilter_IsVariableAllowed

Returns whether or not the variable should be filtered
====================
*/
qboolean SrvFilter_IsVariableAllowed(const srvFilterEnv_t *env, const char *name)
{
    // HZM coop - always allow the server to set our own mod-namespaced client cvars (coop_*). These are
    // used for script->client HUD/UI bridges (e.g. the briefing ready-up overlay's coop_gate_* cvars).
    if (!Q_stricmpn(name, "coop_", 5)) {
        return qtrue;
    }

    return SrvFilter_IsExplicitVariable(env, name);
}

/*
====================
SrvFilter_IsSetVariableAllowed

Returns whether or not the variable should be filtered
====================
*/
static qboolean SrvFilter_IsSetVariableAllowed(const srvFilterEnv_t *env, const char *name, char type)
{
    cvar_t *var;

    if (SrvFilter_IsVariableAllowed(env, name)) {
        return qtrue;
    }

    if (type != 'a' && type != 's') {
        // Only allow ephemeral or userinfo variables

        var = env->CvarFind(name);
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
SrvFilter_IsCommandAllowed

Returns whether or not the command should be filtered
====================
*/
static qboolean SrvFilter_IsCommandAllowed(const srvFilterEnv_t *env, const char *name)
{
    size_t  i;
    cvar_t *var;
    qboolean implicit = qfalse;

    for (i = 0; i < ARRAY_LEN(whiteListedCommands); i++) {
        if (!Q_stricmp(name, whiteListedCommands[i])) {
            return qtrue;
        }
    }

    if (env->localServer) {
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
    if (SrvFilter_IsExplicitVariable(env, name)) {
        return qtrue;
    }

    if (!Q_stricmpn(name, "coop_", 5)) {
        implicit = qtrue;
    } else {
        var = env->CvarFind(name);
        if (var && (var->flags & CVAR_USER_CREATED)) {
            // Allow, it's user-created, wouldn't cause issues
            implicit = qtrue;
        }
    }

    if (!implicit) {
        return qfalse;
    }

    // HZM coop [SEC2] exe only. The two IMPLICIT clauses above admit a bare token because a cvar of that
    // name is (or may be) writable - but Cmd_ExecuteString runs a registered command or alias BEFORE it
    // tries a cvar, so the token would run the COMMAND: `set writeconfig 1` then `writeconfig x.cfg`, or
    // `coop_join <host> <port>` (net_rendezvous.c restuffs a connect built from its arguments). Only the
    // explicit lists may admit a command. cgame cannot see the command table, so this is the one decision
    // where layer 2 is stricter than layer 1 (the SHADOW class in the self-test).
    if (env->IsRegisteredCommand && env->IsRegisteredCommand(name)) {
        return SrvFilter_DropReason(env, "shadow", name);
    }

    return qtrue;
}

/*
====================
SrvFilter_DigitRun

True when `s` is between lo and hi digit characters and nothing else.
====================
*/
static qboolean SrvFilter_DigitRun(const char *s, int lo, int hi)
{
    int n = 0;

    if (!s || !*s) {
        return qfalse;
    }
    while (*s) {
        if (*s < '0' || *s > '9') {
            return qfalse;
        }
        n++;
        s++;
    }

    return (n >= lo && n <= hi) ? qtrue : qfalse;
}

/*
====================
SrvFilter_IsNameBusTokenAllowed

Strict grammar for a single server-origin coop name-bus marker (leading comma expected). Only the
markers the coop framework legitimately drives from the server pass: the armory skin/helmet/gloves
pages, the loadout/finish slots, the FOV marker, the dev auth token, the coop handshake and the armory
open ping. Every dev/admin/debug/emote/teleport/noclip/give marker (variables.scr
getNameAppendCommands) is rejected, so a hostile server cannot append one to a client's name (which
would carry to the next server the client joins).
====================
*/
qboolean SrvFilter_IsNameBusTokenAllowed(const char *tok)
{
    if (!tok || tok[0] != ',') {
        return qfalse;
    }
    tok++;

    if (!Q_stricmpn(tok, "sn", 2)) {
        return SrvFilter_DigitRun(tok + 2, 1, 3); // armory skin page (absolute)
    }
    if (!Q_stricmpn(tok, "hn", 2)) {
        return SrvFilter_DigitRun(tok + 2, 1, 3); // armory helmet page (absolute)
    }
    if (!Q_stricmpn(tok, "gn", 2)) {
        return SrvFilter_DigitRun(tok + 2, 1, 2); // armory gloves (absolute)
    }
    if ((tok[0] == 'w' || tok[0] == 'W') && tok[1] >= '1' && tok[1] <= '4') {
        return SrvFilter_DigitRun(tok + 2, 1, 3); // loadout slot 1-4 -> 1-3 digit roster id
    }
    if ((tok[0] == 'f' || tok[0] == 'F') && tok[1] >= '1' && tok[1] <= '4') {
        return SrvFilter_DigitRun(tok + 2, 1, 2); // finish slot 1-4 -> 1-2 digit fid
    }
    // HZM coop [SEC2] the two markers a server-exec'd shipped cfg appends. Layer 2 filters the lines of
    // a server-origin exec, so without these the coop handshake (coop_mod/cfg/detect.cfg: `,0203`,
    // the mod id + 3-digit version) and the armory open ping (ui/loadout/open.cfg: `,w0o`) would be
    // dropped. Neither grants a server anything new: it could already make the client append exactly
    // these by exec'ing those cfgs, which layer 1 has always admitted.
    if (tok[0] == '0') {
        return SrvFilter_DigitRun(tok + 1, 3, 3);
    }
    if (!Q_stricmp(tok, "w0o")) {
        return qtrue;
    }
    if (tok[0] == '1') {
        return SrvFilter_DigitRun(tok + 1, 1, 3); // ,1<fov> (e.g. ,180 = fov 80)
    }
    if (tok[0] == '5') {
        // ,5<auth token> - widened past alnum for admin-set keys (developer.scr devauth); a run of
        // non-space, non-comma chars bounded by the name length limit.
        const char *q = tok + 1;
        int         n = 0;

        if (!*q) {
            return qfalse;
        }
        while (*q) {
            if (*q == ',' || (unsigned char)*q <= ' ') {
                return qfalse;
            }
            n++;
            q++;
        }
        return (n <= 30) ? qtrue : qfalse;
    }

    return qfalse;
}

/*
====================
SrvFilter_NameValueAllowed

Scan the server-controlled part of a value that lands in the `name` cvar for every " ," marker sequence
and require each marker to pass the grammar above. A marker-free name passes untouched - the server's
own cleaned name (player.scr playerCleanName truncates at the first " ,") never carries a marker.
====================
*/
static qboolean SrvFilter_NameValueAllowed(const srvFilterEnv_t *env, const char *value)
{
    const char *p = value;

    if (!p) {
        return qtrue;
    }

    while (*p) {
        if (p[0] == ' ' && p[1] == ',') {
            const char *m = p + 1; // at the comma
            char        tok[64];
            int         n = 0;

            while (m[n] && m[n] != ' ' && n < (int)sizeof(tok) - 1) {
                tok[n] = m[n];
                n++;
            }
            tok[n] = 0;

            if (!SrvFilter_IsNameBusTokenAllowed(tok)) {
                return SrvFilter_DropReason(env, "namebus", tok);
            }

            p = m + n;
            continue;
        }
        p++;
    }

    return qtrue;
}

/*
====================
SrvFilter_IsNumericOrEmpty

True for the vstr no-op values: empty, all-whitespace, or all-digit (the documented cleared armory
state "seta coop_loASkin 0" / "seta coop_loAHelm 0", helmet.scr). None expands to a command.
====================
*/
qboolean SrvFilter_IsNumericOrEmpty(const char *s)
{
    if (!s || !s[0]) {
        return qtrue;
    }
    while (*s) {
        if (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') {
            s++;
            continue;
        }
        if (*s < '0' || *s > '9') {
            return qfalse;
        }
        s++;
    }

    return qtrue;
}

static qboolean SrvFilter_IsExecPathAllowed(const char *path)
{
    // HZM coop - allow server-driven exec of MOD-NAMESPACED cfg paths only. The coop framework
    // drives clients through tiny cfgs (coop_mod/cfg/detect.cfg = the mod handshake,
    // ui/coop_objectives/*, ui/loadout/* = the armory). A blanket "exec" whitelist would let
    // hostile servers run arbitrary local cfgs - path-scoping keeps the protection while
    // unblocking the mod (bug-597: this filter silently ate the handshake, the FOV re-apply,
    // the armory pick resend AND the lobby LOADOUT button).
    if (Q_stricmpn(path, "ui/loadout/", 11) && Q_stricmpn(path, "ui/coop_", 8) && Q_stricmpn(path, "coop_mod/", 9)) {
        return qfalse;
    }
    return qtrue;
}

/*
====================
SrvFilter_GuardClass

HZM coop [SEC2] Is `name` a cvar the CLIENT runs through vstr from its own origin? Case-insensitive
like Cvar_FindVar. A DIGITS entry matches its prefix followed by one or more digits.
====================
*/
int SrvFilter_GuardClass(const char *name)
{
    const srvGuard_t *g;

    for (g = srvGuards; g->name; g++) {
        if (g->kind == SRVG_EXACT) {
            if (!Q_stricmp(name, g->name)) {
                return g->cls;
            }
        } else {
            size_t len = strlen(g->name);

            if (!Q_stricmpn(name, g->name, len) && SrvFilter_DigitRun(name + len, 1, 16)) {
                return g->cls;
            }
        }
    }

    return SRVG_NONE;
}

// Inside a guarded value, a `vstr` may only reach another guarded cvar: that value will expand at CLIENT
// origin, unfiltered, so it must be one the server cannot write freely.
static qboolean SrvFilter_GuardVstrAllowed(const srvFilterEnv_t *env, const char *name, int depth)
{
    (void)depth;
    if (SrvFilter_GuardClass(name) == SRVG_NONE) {
        return SrvFilter_DropReason(env, "guard-vstr", name);
    }
    return qtrue;
}

/*
====================
SrvFilter_ValidateValue

HZM coop [SEC2] A server-origin write to a VALIDATE cvar is admitted only when the value, run later at
CLIENT origin, can do no more than the server could stuff directly: every statement passes these same
rules, a nested write to a guarded cvar validates its own value, and a nested vstr reaches only guarded
cvars. Today's legitimate values (append name ,w101 / exec ui/loadout/p01.cfg / vstr coop_loCcur /
set coop_loFcmt1 vstr coop_loFgo1 / a number) all pass.
====================
*/
static qboolean SrvFilter_ValidateValue(const srvFilterEnv_t *env, const char *name, const char *value, int depth)
{
    srvFilterEnv_t child;
    qboolean       inStar  = qfalse;
    qboolean       inSlash = qfalse;
    int            total;
    int            pos = 0;

    if (SrvFilter_IsNumericOrEmpty(value)) {
        return qtrue;
    }
    if (depth >= SRVF_VALIDATE_MAX_DEPTH) {
        return SrvFilter_DropReason(env, "guard-depth", name);
    }
    total = (int)strlen(value);
    if (total >= SRVF_VALUE_MAX) {
        return SrvFilter_DropReason(env, "length", NULL);
    }

    child                  = *env;
    child.StageGet         = NULL;
    child.StagePut         = NULL;
    child.VstrValueAllowed = SrvFilter_GuardVstrAllowed;
    child.validating       = qtrue;

    while (pos < total) {
        char        line[SRVF_VALUE_MAX];
        srvTokens_t tok;
        int         i = SrvFilter_StatementLength(value + pos, total - pos, &inStar, &inSlash);

        memcpy(line, value + pos, i);
        line[i] = 0;

        SrvFilter_Tokenize(&tok, line);
        if (tok.overflow) {
            return SrvFilter_DropReason(env, "length", NULL);
        }
        if (tok.argc > 0 && !SrvFilter_CheckArgs(&child, tok.argc, tok.argv, depth + 1)) {
            return SrvFilter_DropReason(env, "guard", name);
        }

        if (i == total - pos) {
            pos = total;
        } else {
            pos += i + 1;
        }
    }

    if (inStar) {
        return SrvFilter_DropReason(env, "guard", name);
    }

    return qtrue;
}

static qboolean SrvFilter_GuardedWriteAllowed(
    const srvFilterEnv_t *env, const char *name, const char *value, int depth
)
{
    int cls = SrvFilter_GuardClass(name);

    if (cls == SRVG_NONE) {
        return qtrue;
    }
    // A REFUSE entry is never written by the server today; while validating a guarded value the write
    // will run at CLIENT origin, so there it is judged by its value like any other guarded write.
    if (cls == SRVG_REFUSE && !env->validating) {
        return SrvFilter_DropReason(env, "refuse", name);
    }

    return SrvFilter_ValidateValue(env, name, value, env->validating ? depth : 0);
}

/*
====================
SrvFilter_CheckArgs

The one statement decision. Dispatches exactly as Cmd_ExecuteString does: set-family and bare cvar
writes are modelled with the engine's own value semantics (Cvar_Set_f = ArgsFrom(2), Cvar_Command =
ArgsFrom(1)), append with Cvar_Append_f's, exec by path scope, vstr by name (plus cgame's value gate),
and everything else through the command/variable lists. Writes into `name` pass the name-bus grammar;
writes into a guarded cvar pass the guard.
====================
*/
qboolean SrvFilter_CheckArgs(const srvFilterEnv_t *env, int argc, char **argv, int depth)
{
    const char *verb;

    if (argc < 1) {
        return qtrue; // empty / comment-only statement: the engine runs nothing
    }
    verb = argv[0];

    if (!Q_stricmp(verb, "set") || !Q_stricmp(verb, "setu") || !Q_stricmp(verb, "seta") || !Q_stricmp(verb, "sets")
        || !Q_stricmp(verb, "append")) {
        qboolean    isAppend = !Q_stricmp(verb, "append");
        char        type     = isAppend ? 0 : verb[3];
        const char *nm;

        // `set x` with no value is a print (Cvar_Print_f), not a write - nothing to model or gate.
        if (argc < 3) {
            return qtrue;
        }
        nm = argv[1];
        if ((int)strlen(nm) >= SRVF_NAME_MAX) {
            return SrvFilter_DropReason(env, "length", NULL);
        }
        if (!SrvFilter_IsSetVariableAllowed(env, nm, type)) {
            return qfalse;
        }

        if (isAppend) {
            // Cvar_Append_f (cvar.c): stores Cvar_VariableString(var) + " " + Cmd_Argv(2).
            const char *old = env->StageGet ? env->StageGet(nm) : NULL;
            char        merged[1024]; // engine uses buffer[1024]
            size_t      need;

            if (!old) {
                cvar_t *ov = env->CvarFind(nm);
                old        = ov ? ov->string : "";
            }
            need = Com_sprintf(merged, sizeof(merged), "%s %s", old, argv[2]);
            if (need >= sizeof(merged) || (int)strlen(merged) >= SRVF_VALUE_MAX) {
                return SrvFilter_DropReason(env, "length", NULL);
            }
            if (!Q_stricmp(nm, "name")) {
                // HZM coop [SEC2] only the appended token is server-controlled; markers already in the
                // name were judged when they were written, and a client-origin one (,w0c, ,sn12 from a
                // menu click) must not make a later legitimate resend look hostile.
                char added[1024];

                Com_sprintf(added, sizeof(added), " %s", argv[2]);
                if (!SrvFilter_NameValueAllowed(env, added)) {
                    return qfalse;
                }
            }
            if (!SrvFilter_GuardedWriteAllowed(env, nm, merged, depth)) {
                return qfalse;
            }
            if (env->StagePut) {
                env->StagePut(nm, merged);
            }
        } else {
            // Cvar_Set_f (cvar.c): Cvar_Set2(Argv(1), Cmd_ArgsFrom(2)).
            char val[SRVF_VALUE_MAX];

            if (!SrvFilter_ArgsFrom(argc, argv, 2, val, sizeof(val))) {
                return SrvFilter_DropReason(env, "length", NULL);
            }
            if (!Q_stricmp(nm, "name") && !SrvFilter_NameValueAllowed(env, val)) {
                return qfalse;
            }
            if (!SrvFilter_GuardedWriteAllowed(env, nm, val, depth)) {
                return qfalse;
            }
            if (env->StagePut) {
                env->StagePut(nm, val);
            }
        }
        return qtrue;
    }

    if (!Q_stricmp(verb, "exec")) {
        return SrvFilter_IsExecPathAllowed(argc > 1 ? argv[1] : "");
    }

    if (!Q_stricmp(verb, "vstr")) {
        // HZM coop - allow vstr of coop-namespaced or user-created cvars: the armory resend asks
        // the client to fire its own archived coop_loA1..4; the FOV/auth re-applies use
        // user-created cvars (g_m2l1 style). Engine-owned cvars stay off-limits.
        char varname[256];

        if (argc < 2) {
            return qtrue; // `vstr` with no arg: Cmd_Vstr_f prints usage, no-op
        }
        Q_strncpyz(varname, argv[1], sizeof(varname));
        if (Q_stricmpn(varname, "coop_", 5)) {
            cvar_t *var = env->CvarFind(varname);
            if (!var || !(var->flags & CVAR_USER_CREATED)) {
                return qfalse;
            }
        }

        if (env->VstrValueAllowed && !env->VstrValueAllowed(env, varname, depth)) {
            return qfalse;
        }
        return qtrue;
    }

    //
    // normal command (or a bare `<cvar> <value>` the engine routes through Cvar_Command)
    //
    if (!SrvFilter_IsCommandAllowed(env, verb)) {
        return qfalse;
    }

    // HZM coop [SEC1] - if the bare token is a writable variable with a value following, the
    // engine's Cvar_Command turns it into Cvar_Set2(name, Cmd_Args()); model it (and gate a bare
    // `name <value>`) so the bare-write cannot dodge the vstr value / name-bus / guard checks.
    if (argc >= 2) {
        char val[SRVF_VALUE_MAX];

        if (!SrvFilter_ArgsFrom(argc, argv, 1, val, sizeof(val))) {
            return SrvFilter_DropReason(env, "length", NULL);
        }
        if (!Q_stricmp(verb, "name")) {
            if (!SrvFilter_NameValueAllowed(env, val)) {
                return qfalse;
            }
        } else {
            cvar_t *var      = NULL;
            qboolean writable = SrvFilter_IsVariableAllowed(env, verb);

            if (!writable) {
                var      = env->CvarFind(verb);
                writable = (var && (var->flags & CVAR_USER_CREATED)) ? qtrue : qfalse;
            }
            if (writable) {
                if (!SrvFilter_GuardedWriteAllowed(env, verb, val, depth)) {
                    return qfalse;
                }
                if (env->StagePut) {
                    env->StagePut(verb, val);
                }
            }
        }
    }

    return qtrue;
}
