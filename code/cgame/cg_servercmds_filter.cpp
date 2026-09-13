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

    // HZM coop [user 2026-08-21] "there is seemingly no difference between the two when you use the
    // '/' key... but if you enable it in control settings you get locked to the free cam version of
    // third person regardless of what you hit with '/'. Notwithstanding first person that works fine."
    //
    // The three-mode view cycle (1P -> free cam -> chase) has existed in thirdperson.scr all along and
    // is correct. It switches modes by stuffing BOTH cg_3rd_person and cg_freecam - but only
    // cg_3rd_person was on this list, so every "set cg_freecam" the server sent was silently dropped.
    //
    // That single omission produces the exact three symptoms reported. Free cam and chase differ ONLY
    // by cg_freecam, so with it frozen both modes rendered identically ("no difference"). The archived
    // value became the only thing that ever set it, so a player who once ticked the options box could
    // never get back out ("locked to the free cam version"). And first person kept working because it
    // is the one mode that turns on cg_3rd_person alone, which was whitelisted.
    "cg_freecam",

    // HZM coop - allow the server to mix client audio for scripted cinematics (e.g. m3l1a Omaha
    // ramp-drop): duck effects/ambient so the music takes over, then restore. Without these the
    // server-stuffed volume changes are silently filtered on the client.
    "s_volume",
    "s_musicvolume",
    "s_ambientvolume",
    "s_sfxduck",

    // HZM coop [user 08-06] bug-1502 - marker cvars for the CLIENT-CAPTURED music/ambient duck
    // (cg_view.c CG_UpdateScriptedAudioDucks). The server can never read a client's live cvar back
    // (stufftext has no $cvar substitution), so instead of the server dictating a literal restore
    // value it just stuffs "start/stop ducking" + a target - cgame captures the player's OWN live
    // s_musicvolume/s_ambientvolume before ducking and restores to exactly that, never a hardcoded
    // default. Two independent channels (map scripts trigger them at different times).
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
CG_Tokenize / CG_Argc / CG_Argv / CG_ArgsFrom

A faithful in-file port of the engine command tokenizer (qcommon/cmd.c Cmd_TokenizeString2, called
with ignoreQuotes=qfalse via Cmd_TokenizeString at cmd.c:763) and Cmd_ArgsFrom (cmd.c:576). cmd.c is
NOT linked into cgame, so it is copied here rather than called. This lets the filter model the value
a write stores as the EXACT string Cmd_ExecuteString would produce (set-family = ArgsFrom(2), bare
cvar write = ArgsFrom(1)/Cmd_Args(), append = old + " " + Argv(2)) instead of a raw substring, which
was wrong for quoted arguments (e.g. `set x "" quit` -> the engine stores " quit", not "").

Fed one already-split statement at a time (CG_SplitAndCheck splits on ';'/newline first), so there
is no ';' handling here - only quote and slash-slash / slash-star comment handling, as the engine.
====================
*/
#define CG_MAX_TOKENS 1024
static int   cg_tkArgc = 0;
static char *cg_tkArgv[CG_MAX_TOKENS];
static char  cg_tkBuf[BIG_INFO_STRING + CG_MAX_TOKENS];

static void CG_Tokenize(const char *text_in)
{
    const char *text;
    char       *textOut;

    cg_tkArgc = 0;
    if (!text_in) {
        return;
    }
    text    = text_in;
    textOut = cg_tkBuf;

    while (1) {
        if (cg_tkArgc == CG_MAX_TOKENS) {
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
            cg_tkArgv[cg_tkArgc++] = textOut;
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
        cg_tkArgv[cg_tkArgc++] = textOut;

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

static const char *CG_Argv(int i)
{
    return (i >= 0 && i < cg_tkArgc) ? cg_tkArgv[i] : "";
}

// Cmd_ArgsFrom (cmd.c:576): argv[arg]..argv[argc-1] joined with single spaces.
static char *CG_ArgsFrom(int arg)
{
    static char cg_args[BIG_INFO_STRING];
    int         i;

    cg_args[0] = 0;
    if (arg < 0) {
        arg = 0;
    }
    for (i = arg; i < cg_tkArgc; i++) {
        Q_strcat(cg_args, sizeof(cg_args), cg_tkArgv[i]);
        if (i != cg_tkArgc - 1) {
            Q_strcat(cg_args, sizeof(cg_args), " ");
        }
    }

    return cg_args;
}

// HZM coop [SEC1] - security layer 1: close attack (a) - "seta coop_x <any command>; vstr coop_x".
// The stock vstr branch admitted any coop_/user-created cvar name WITHOUT looking at what the cvar
// EXPANDS to, so a server could park an arbitrary client command in a coop_ cvar and fire it with a
// whitelisted vstr. The fix walks the vstr'd value through this same filter (recursively, depth-
// limited), resolving it from what the server wrote earlier in the SAME text (the in-walk stage
// table below) else the live cvar. It also models every writer the filter admits (set-family,
// append, and the bare `<cvar> <value>` the engine routes through Cvar_Command) so a write earlier
// in one text is seen by a later vstr, and gates any write that lands in the coop name-bus (`name`)
// through a strict marker grammar so a hostile server cannot inject a privileged ,ga/,nc/,3/,6 etc.
//
// NOT closed here (deferred to layer 2, exe-side server-origin taint): a write in one stufftext and
// the matching vstr in a SEPARATE stufftext of the SAME snapshot - all statements of a snapshot are
// filtered and buffered before any Cbuf_Execute, so no in-walk table can span them. The value check
// still blocks the cross-FRAME form (the hostile value is already live when the vstr is filtered).

#define CG_VSTR_MAX_DEPTH 4  // deepest real chain is 3 (coop_loCmt -> coop_loCcur -> coop_loC* -> exec)
#define CG_STAGE_MAX      32 // legit server stufftexts carry 1-2 statements; fail-closed past this

typedef struct {
    char name[64];
    char value[512];
} cgStageEntry_t;

// In-walk stage table: the writes seen so far in the CURRENT public walk (and its vstr expansions).
// Module-static and cleared at the top of CG_IsStatementAllowed - there is no cross-stufftext or
// cross-snapshot state, so nothing outside this file needs to reset it.
static cgStageEntry_t cg_stage[CG_STAGE_MAX];
static int            cg_stageCount    = 0;
static qboolean       cg_stageOverflow = qfalse;

static void CG_StageReset(void)
{
    cg_stageCount    = 0;
    cg_stageOverflow = qfalse;
}

static void CG_StagePut(const char *name, const char *value)
{
    int i;

    for (i = 0; i < cg_stageCount; i++) {
        if (!Q_stricmp(cg_stage[i].name, name)) {
            Q_strncpyz(cg_stage[i].value, value, sizeof(cg_stage[i].value));
            return;
        }
    }

    if (cg_stageCount >= CG_STAGE_MAX) {
        // fail closed: a dropped write would let a later vstr resolve a stale value and wrongly pass
        cg_stageOverflow = qtrue;
        return;
    }

    Q_strncpyz(cg_stage[cg_stageCount].name, name, sizeof(cg_stage[0].name));
    Q_strncpyz(cg_stage[cg_stageCount].value, value, sizeof(cg_stage[0].value));
    cg_stageCount++;
}

static const char *CG_StageGet(const char *name)
{
    int i;

    for (i = cg_stageCount - 1; i >= 0; i--) {
        if (!Q_stricmp(cg_stage[i].name, name)) {
            return cg_stage[i].value;
        }
    }

    return NULL;
}

static qboolean CG_Covtrace(void)
{
    static cvar_t *cv = NULL;

    if (!cv) {
        cv = cgi.Cvar_Get("coop_covtrace", "0", 0);
    }

    return (cv && cv->integer) ? qtrue : qfalse;
}

// Fail-closed drop with the length sub-reason: any name/value/append/vstr string that does not fit
// the model buffers is refused rather than silently truncated (a truncated model could let a longer
// hostile value through). No legitimate armory/name-bus value comes close (inventory replay = 0).
static qboolean CG_DropLength(void)
{
    if (CG_Covtrace()) {
        cgi.Printf("^~^~^ COVC VDROP length\n");
    }
    return qfalse;
}

// A text that ends inside an unterminated /* comment. The newline cg_servercmds.c stuffs after each
// text ends quotes and // comments, but Cbuf_Execute keeps a /* comment open across it, so the NEXT
// buffered stufftext would start inside the comment and a */ there would expose a hidden statement
// ("echo /*" then "echo */quit" runs quit). The filter checks each text with fresh state, so the
// only sound rule is to refuse the text that leaves the comment open.
static qboolean CG_DropComment(void)
{
    if (CG_Covtrace()) {
        cgi.Printf("^~^~^ COVC VDROP comment\n");
    }
    return qfalse;
}

/*
====================
CG_DigitRun

True when `s` is between lo and hi digit characters and nothing else.
====================
*/
static qboolean CG_DigitRun(const char *s, int lo, int hi)
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
CG_IsNameBusTokenAllowed

Strict grammar for a single server-origin coop name-bus marker (leading comma expected). Only the
markers the coop framework legitimately drives from the server pass: the armory skin/helmet/gloves
pages, the loadout/finish slots, the FOV marker and the dev auth token. Every dev/admin/debug/emote/
teleport/noclip/give marker (variables.scr getNameAppendCommands) is rejected, so a hostile server
cannot append one to a client's name (which would carry to the next server the client joins).
====================
*/
static qboolean CG_IsNameBusTokenAllowed(const char *tok)
{
    if (!tok || tok[0] != ',') {
        return qfalse;
    }
    tok++;

    if (!Q_stricmpn(tok, "sn", 2)) {
        return CG_DigitRun(tok + 2, 1, 3); // armory skin page (absolute)
    }
    if (!Q_stricmpn(tok, "hn", 2)) {
        return CG_DigitRun(tok + 2, 1, 3); // armory helmet page (absolute)
    }
    if (!Q_stricmpn(tok, "gn", 2)) {
        return CG_DigitRun(tok + 2, 1, 2); // armory gloves (absolute)
    }
    if ((tok[0] == 'w' || tok[0] == 'W') && tok[1] >= '1' && tok[1] <= '4') {
        return CG_DigitRun(tok + 2, 1, 3); // loadout slot 1-4 -> 1-3 digit roster id
    }
    if ((tok[0] == 'f' || tok[0] == 'F') && tok[1] >= '1' && tok[1] <= '4') {
        return CG_DigitRun(tok + 2, 1, 2); // finish slot 1-4 -> 1-2 digit fid
    }
    if (tok[0] == '1') {
        return CG_DigitRun(tok + 1, 1, 3); // ,1<fov> (e.g. ,180 = fov 80)
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
CG_NameValueAllowed

Scan a server-origin value that lands in the `name` cvar for every " ," marker sequence and require
each marker to pass the grammar above. A marker-free name passes untouched - the server's own
cleaned name (player.scr playerCleanName truncates at the first " ,") never carries a marker, so
this never false-drops a legitimate rename (which would time out the join and lose the FOV re-apply).
====================
*/
static qboolean CG_NameValueAllowed(const char *value)
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

            if (!CG_IsNameBusTokenAllowed(tok)) {
                if (CG_Covtrace()) {
                    cgi.Printf("^~^~^ COVC VDROP namebus %s\n", tok);
                }
                return qfalse;
            }

            p = m + n;
            continue;
        }
        p++;
    }

    return qtrue;
}

static qboolean CG_SplitAndCheck(const char *text, int depth);
static qboolean CG_CheckOneStatement(char *line, int depth);

/*
====================
CG_IsNumericOrEmpty

True for the vstr no-op values: empty, all-whitespace, or all-digit (the documented cleared armory
state "seta coop_loASkin 0" / "seta coop_loAHelm 0", helmet.scr). None expands to a command.
====================
*/
static qboolean CG_IsNumericOrEmpty(const char *s)
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

/*
====================
CG_IsVstrValueAllowed

Resolve what `vstr <name>` will expand to - the in-walk staged write if any, else the live cvar -
and validate it through the same statement filter (recursively, depth-limited). A vstr of a cvar
that does not exist, or whose value is empty/numeric, is a harmless no-op.
====================
*/
static qboolean CG_IsVstrValueAllowed(const char *name, int depth)
{
    const char *val;
    cvar_t     *var;
    char        buf[512];

    if (depth >= CG_VSTR_MAX_DEPTH) {
        if (CG_Covtrace()) {
            cgi.Printf("^~^~^ COVC VDROP depth %s\n", name);
        }
        return qfalse; // fail closed on runaway / self-referential recursion
    }

    val = CG_StageGet(name);
    if (!val) {
        var = cgi.Cvar_Find(name);
        if (!var) {
            return qtrue; // vstr of a nonexistent cvar expands to "" - no-op
        }
        val = var->string;
    }

    if (CG_IsNumericOrEmpty(val)) {
        return qtrue;
    }

    // Fail closed rather than silently truncate: a value the buffer cannot hold could be modeled as
    // a benign prefix while the engine (whose Cbuf line is 8192) runs the hostile tail.
    if ((int)strlen(val) >= (int)sizeof(buf)) {
        return CG_DropLength();
    }

    Q_strncpyz(buf, val, sizeof(buf));
    // Cbuf_InsertText hands the whole value back to Cbuf_Execute, so split it into statements the
    // same way (unquoted ';'/newline, quote+comment aware) before checking each one.
    if (!CG_SplitAndCheck(buf, depth + 1)) {
        if (CG_Covtrace()) {
            cgi.Printf("^~^~^ COVC VDROP vstr %s -> %.60s\n", name, val);
        }
        return qfalse;
    }

    return qtrue;
}

/*
====================
CG_CheckOneStatement

Check ONE already-split statement (no unquoted ';' inside - CG_SplitAndCheck removed them). Tokenize
it with the engine tokenizer port, then dispatch exactly as Cmd_ExecuteString does: set-family and
bare cvar writes are modeled with the engine's own value semantics (ArgsFrom), append with
Cvar_Append_f's semantics, and vstr through the recursive value gate. Writes into the name cvar are
gated by the name-bus marker grammar.
====================
*/
static qboolean CG_CheckOneStatement(char *line, int depth)
{
    const char *verb;

    CG_Tokenize(line);
    if (cg_tkArgc < 1) {
        return qtrue; // empty / comment-only statement: the engine runs nothing
    }
    verb = cg_tkArgv[0];

    if (!Q_stricmp(verb, "set") || !Q_stricmp(verb, "setu") || !Q_stricmp(verb, "seta")
        || !Q_stricmp(verb, "sets") || !Q_stricmp(verb, "append")) {
        qboolean    isAppend = !Q_stricmp(verb, "append");
        char        type     = isAppend ? 0 : verb[3];
        const char *nm;

        // `set x` with no value is a print (Cvar_Print_f), not a write - nothing to model or gate.
        if (cg_tkArgc < 3) {
            return qtrue;
        }
        nm = cg_tkArgv[1];
        if ((int)strlen(nm) >= (int)sizeof(cg_stage[0].name)) {
            return CG_DropLength();
        }
        if (!CG_IsSetVariableAllowed(nm, type)) {
            return qfalse;
        }

        if (isAppend) {
            // Cvar_Append_f (cvar.c:1150): stores Cvar_VariableString(var) + " " + Cmd_Argv(2).
            const char *old = CG_StageGet(nm);
            char        merged[1024]; // engine uses buffer[1024]
            size_t      need;

            if (!old) {
                cvar_t *ov = cgi.Cvar_Find(nm);
                old         = ov ? ov->string : "";
            }
            need = Com_sprintf(merged, sizeof(merged), "%s %s", old, CG_Argv(2));
            if (need >= sizeof(merged) || (int)strlen(merged) >= (int)sizeof(cg_stage[0].value)) {
                return CG_DropLength();
            }
            if (!Q_stricmp(nm, "name") && !CG_NameValueAllowed(merged)) {
                return qfalse;
            }
            CG_StagePut(nm, merged);
        } else {
            // Cvar_Set_f (cvar.c:959): Cvar_Set2(Argv(1), Cmd_ArgsFrom(2)).
            const char *val = CG_ArgsFrom(2);
            if ((int)strlen(val) >= (int)sizeof(cg_stage[0].value)) {
                return CG_DropLength();
            }
            if (!Q_stricmp(nm, "name") && !CG_NameValueAllowed(val)) {
                return qfalse;
            }
            CG_StagePut(nm, val);
        }
        return qtrue;
    }

    if (!Q_stricmp(verb, "exec")) {
        // HZM coop - allow server-driven exec of MOD-NAMESPACED cfg paths only. The coop framework
        // drives clients through tiny cfgs (coop_mod/cfg/detect.cfg = the mod handshake,
        // ui/coop_objectives/*, ui/loadout/* = the armory). A blanket "exec" whitelist would let
        // hostile servers run arbitrary local cfgs - path-scoping keeps the protection while
        // unblocking the mod (bug-597: this filter silently ate the handshake, the FOV re-apply,
        // the armory pick resend AND the lobby LOADOUT button).
        const char *path = CG_Argv(1);
        if (Q_stricmpn(path, "ui/loadout/", 11) && Q_stricmpn(path, "ui/coop_", 8)
            && Q_stricmpn(path, "coop_mod/", 9)) {
            return qfalse;
        }
        return qtrue;
    }

    if (!Q_stricmp(verb, "vstr")) {
        // HZM coop - allow vstr of coop-namespaced or user-created cvars: the armory resend asks
        // the client to fire its own archived coop_loA1..4; the FOV/auth re-applies use
        // user-created cvars (g_m2l1 style). Engine-owned cvars stay off-limits.
        char    varname[256];
        cvar_t *var;

        if (cg_tkArgc < 2) {
            return qtrue; // `vstr` with no arg: Cmd_Vstr_f prints usage, no-op
        }
        Q_strncpyz(varname, cg_tkArgv[1], sizeof(varname));
        if (Q_stricmpn(varname, "coop_", 5)) {
            var = cgi.Cvar_Find(varname);
            if (!var || !(var->flags & CVAR_USER_CREATED)) {
                return qfalse;
            }
        }

        // HZM coop [SEC1] - value gate: the cvar's contents must themselves pass the filter, so
        // "seta coop_x <command>; vstr coop_x" can no longer smuggle an arbitrary command.
        if (!CG_IsVstrValueAllowed(varname, depth)) {
            return qfalse;
        }
        return qtrue;
    }

    //
    // normal command (or a bare `<cvar> <value>` the engine routes through Cvar_Command)
    //
    if (!CG_IsCommandAllowed(verb)) {
        return qfalse;
    }

    // HZM coop [SEC1] - if the bare token is a writable variable with a value following, the
    // engine's Cvar_Command turns it into Cvar_Set2(name, Cmd_Args()); model it (and gate a bare
    // `name <value>`) so the bare-write cannot dodge the vstr value / name-bus checks.
    if (cg_tkArgc >= 2) {
        const char *val = CG_ArgsFrom(1); // Cvar_Command (cvar.c:857): Cvar_Set2(name, Cmd_Args())
        if ((int)strlen(val) >= (int)sizeof(cg_stage[0].value)) {
            return CG_DropLength();
        }
        if (!Q_stricmp(verb, "name")) {
            if (!CG_NameValueAllowed(val)) {
                return qfalse;
            }
        } else if (CG_IsVariableAllowed(verb)) {
            CG_StagePut(verb, val);
        } else {
            cvar_t *var = cgi.Cvar_Find(verb);
            if (var && (var->flags & CVAR_USER_CREATED)) {
                CG_StagePut(verb, val);
            }
        }
    }

    return qtrue;
}

/*
====================
CG_SplitAndCheck

Split `text` into statements EXACTLY the way qcommon/cmd.c Cbuf_Execute does (the loop at
cmd.c:194-268) - break on an unquoted ';', a '\n' or '\r', or the '/' that closes a '/ *' comment,
honoring quote parity and //, / * * / comments, with the comment state persisting across the breaks.
Each resulting line is the exact granularity at which the engine will later run it, so each is
checked as an independent statement. A statement longer than the model buffer fails closed.

This replaces the stock COM_ParseExt + RemoveEndToken walk, which mis-split whenever a ';' was glued
to a token ("echo;quit") or hidden by a '/ * * /' comment or a bare '\r' - the filter saw one benign
statement while Cbuf_Execute ran a second, hostile one (SEC1/SEC2 bypass).
====================
*/
static qboolean CG_SplitAndCheck(const char *text, int depth)
{
    qboolean in_star_comment  = qfalse;
    qboolean in_slash_comment = qfalse;
    int      total;
    int      pos = 0;

    if (!text) {
        return qtrue;
    }
    total = (int)strlen(text);

    while (pos < total) {
        const char *seg       = text + pos;
        int         remaining = total - pos;
        int         quotes    = 0;
        int         i;
        char        line[MAX_STRING_CHARS];

        // ---- Cbuf_Execute's line scan (cmd.c:216-247), verbatim in structure ----
        for (i = 0; i < remaining; i++) {
            if (seg[i] == '"') {
                quotes++;
            }

            if (!(quotes & 1)) {
                if (i < remaining - 1) {
                    if (!in_star_comment && seg[i] == '/' && seg[i + 1] == '/') {
                        in_slash_comment = qtrue;
                    } else if (!in_slash_comment && seg[i] == '/' && seg[i + 1] == '*') {
                        in_star_comment = qtrue;
                    } else if (in_star_comment && seg[i] == '*' && seg[i + 1] == '/') {
                        in_star_comment = qfalse;
                        i++; // Cbuf NULs out the terminating '/'
                        break;
                    }
                }
                if (!in_slash_comment && !in_star_comment && seg[i] == ';') {
                    break;
                }
            }
            if (!in_star_comment && (seg[i] == '\n' || seg[i] == '\r')) {
                in_slash_comment = qfalse;
                break;
            }
        }

        // fail closed: a single statement that does not fit cannot be modeled safely (the engine's
        // Cbuf line is 8192; a legit server statement is tiny, so this only trips a padding attack).
        if (i >= (int)sizeof(line)) {
            return CG_DropLength();
        }

        Q_strncpyz(line, seg, i + 1); // copy the i bytes of this statement, NUL-terminate

        if (!CG_CheckOneStatement(line, depth)) {
            return qfalse;
        }

        // advance past this statement and its single delimiter, exactly like Cbuf_Execute (i++)
        if (i == remaining) {
            pos = total;
        } else {
            pos += i + 1;
        }
    }

    if (in_star_comment) {
        return CG_DropComment();
    }

    return qtrue;
}

/*
====================
CG_IsStatementAllowed

Returns whether or not the statement is filtered. Public entry (extern "C", called from the
stufftext branch in cg_servercmds.c): clear the in-walk stage table, split-and-check, then fail
closed if a crafted text overflowed the stage table (never reached by legit 1-2 statement traffic).
====================
*/
qboolean CG_IsStatementAllowed(char *cmd)
{
    qboolean ok;

    CG_StageReset();
    ok = CG_SplitAndCheck(cmd, 0);

    if (cg_stageOverflow) {
        if (CG_Covtrace()) {
            cgi.Printf("^~^~^ COVC VDROP overflow\n");
        }
        return qfalse;
    }

    return ok;
}

#ifdef SEC1_SELFTEST
// Test-only accessor (NEVER compiled into the shipped DLL - SEC1_SELFTEST is defined only by the
// self-test build). Exposes the exact value the filter models for a single write statement, so the
// self-test can compare it byte-for-byte against what the real engine tokenizer + Cmd_ArgsFrom
// store. For append it returns only the appended token (Cmd_Argv(2)), which is what the model adds
// to the old value.
void CG_Test_ModelValue(const char *stmt, char *out, int outsize)
{
    char        buf[MAX_STRING_CHARS];
    const char *verb;

    out[0] = 0;
    Q_strncpyz(buf, stmt, sizeof(buf));
    CG_Tokenize(buf);
    if (cg_tkArgc < 1) {
        return;
    }
    verb = cg_tkArgv[0];

    if (!Q_stricmp(verb, "set") || !Q_stricmp(verb, "setu") || !Q_stricmp(verb, "seta")
        || !Q_stricmp(verb, "sets")) {
        if (cg_tkArgc >= 3) {
            Q_strncpyz(out, CG_ArgsFrom(2), outsize);
        }
    } else if (!Q_stricmp(verb, "append")) {
        if (cg_tkArgc >= 3) {
            Q_strncpyz(out, CG_Argv(2), outsize);
        }
    } else {
        if (cg_tkArgc >= 2) {
            Q_strncpyz(out, CG_ArgsFrom(1), outsize);
        }
    }
}
#endif
