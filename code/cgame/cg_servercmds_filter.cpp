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
#include "../qcommon/cmd_filter.h"

// HZM coop [SEC2] The whitelists, the tokenizer port and every per-statement allow/deny rule moved to
// qcommon/cmd_filter.c, which the exe's execution-time filter (security layer 2, qcommon/cmd.c) calls
// too - one implementation, so reception and execution cannot drift apart (TRAPS T8). What stays here is
// what only the reception side needs: the Cbuf-faithful split of one whole stufftext, the in-walk stage
// table and the recursive vstr value gate, and the length / overflow / open-comment fail-closed drops.

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
// NOT closable here, and closed by layer 2 instead: a write in one stufftext and the matching vstr in a
// SEPARATE stufftext of the SAME snapshot (all statements of a snapshot are filtered and buffered before
// any Cbuf_Execute, so no in-walk table can span them), a `wait`-deferred remainder, and anything that
// only exists once a vstr or exec expands at execution time.

#define CG_VSTR_MAX_DEPTH 4  // deepest real chain is 3 (coop_loCmt -> coop_loCcur -> coop_loC* -> exec)
#define CG_STAGE_MAX      32 // legit server stufftexts carry 1-2 statements; fail-closed past this

typedef struct {
    char name[SRVF_NAME_MAX];
    char value[SRVF_VALUE_MAX];
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

static qboolean CG_SplitAndCheck(const char *text, int depth);
static qboolean CG_IsVstrValueAllowed(const srvFilterEnv_t *env, const char *name, int depth);

// The reception side of the shared rules: cgame's cvar view, the localServer allow-lists gated on
// cgs.localServer, the COVC tag, and the two hooks only reception has (stage table + vstr value gate).
static void CG_FilterEnv(srvFilterEnv_t *env)
{
    memset(env, 0, sizeof(*env));
    env->CvarFind         = cgi.Cvar_Find;
    env->Printf           = cgi.Printf;
    env->tag              = "COVC";
    env->localServer      = cgs.localServer ? qtrue : qfalse;
    env->covtrace         = CG_Covtrace();
    env->StageGet         = CG_StageGet;
    env->StagePut         = CG_StagePut;
    env->VstrValueAllowed = CG_IsVstrValueAllowed;
}

/*
====================
CG_IsVstrValueAllowed

Resolve what `vstr <name>` will expand to - the in-walk staged write if any, else the live cvar -
and validate it through the same statement filter (recursively, depth-limited). A vstr of a cvar
that does not exist, or whose value is empty/numeric, is a harmless no-op.
====================
*/
static qboolean CG_IsVstrValueAllowed(const srvFilterEnv_t *env, const char *name, int depth)
{
    const char *val;
    cvar_t     *var;
    char        buf[SRVF_VALUE_MAX];

    (void)env;

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

    if (SrvFilter_IsNumericOrEmpty(val)) {
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

Check ONE already-split statement (no unquoted ';' inside - CG_SplitAndCheck removed them): tokenize
it with the engine tokenizer port and hand it to the shared statement rules.
====================
*/
static qboolean CG_CheckOneStatement(char *line, int depth)
{
    srvTokens_t    tok;
    srvFilterEnv_t env;

    SrvFilter_Tokenize(&tok, line);
    if (tok.overflow) {
        return CG_DropLength();
    }
    if (tok.argc < 1) {
        return qtrue; // empty / comment-only statement: the engine runs nothing
    }

    CG_FilterEnv(&env);
    return SrvFilter_CheckArgs(&env, tok.argc, tok.argv, depth);
}

/*
====================
CG_SplitAndCheck

Split `text` into statements EXACTLY the way qcommon/cmd.c Cbuf_Execute does (SrvFilter_StatementLength
is its line scan) and check each resulting line as an independent statement. A statement longer than
the model buffer fails closed; so does a text that leaves a slash-star comment open.
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
        int         i;
        char        line[MAX_STRING_CHARS];

        i = SrvFilter_StatementLength(seg, remaining, &in_star_comment, &in_slash_comment);

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
    srvTokens_t tok;
    const char *verb;

    out[0] = 0;
    SrvFilter_Tokenize(&tok, stmt);
    if (tok.argc < 1) {
        return;
    }
    verb = tok.argv[0];

    if (!Q_stricmp(verb, "set") || !Q_stricmp(verb, "setu") || !Q_stricmp(verb, "seta")
        || !Q_stricmp(verb, "sets")) {
        if (tok.argc >= 3) {
            SrvFilter_ArgsFrom(tok.argc, tok.argv, 2, out, outsize);
        }
    } else if (!Q_stricmp(verb, "append")) {
        if (tok.argc >= 3) {
            Q_strncpyz(out, tok.argv[2], outsize);
        }
    } else {
        if (tok.argc >= 2) {
            SrvFilter_ArgsFrom(tok.argc, tok.argv, 1, out, outsize);
        }
    }
}
#endif
