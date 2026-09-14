/*
===========================================================================
Copyright (C) 2026 the OpenMoHAA team

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
// cmd_filter.h -- the ONE implementation of the server-command statement rules
//
// HZM coop [SEC2] Security layer 1 (cgame, cg_servercmds_filter.cpp) checks a server stufftext at
// RECEPTION; security layer 2 (the exe, qcommon/cmd.c) checks every SERVER-origin command line at
// EXECUTION. Both call SrvFilter_CheckArgs so their allow/deny rules cannot drift apart (TRAPS T8).
// The caller supplies an environment: how to find a cvar, where to print, the listen-host flag, and
// the optional hooks only one side has (cgame's in-walk stage table and vstr value gate; the exe's
// registered-command lookup).

#pragma once

#include "q_shared.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SRVF_MAX_TOKENS         1024 // Cmd_TokenizeString2's MAX_STRING_TOKENS
#define SRVF_NAME_MAX           64   // fail closed at or past this cvar-name length
#define SRVF_VALUE_MAX          512  // fail closed at or past this value length
#define SRVF_VALIDATE_MAX_DEPTH 4    // guarded-value validation nesting (deepest real chain is 3)

typedef struct srvFilterEnv_s srvFilterEnv_t;

struct srvFilterEnv_s {
    cvar_t *(*CvarFind)(const char *name);
    void (*Printf)(const char *fmt, ...);
    const char *tag;         // machine-line tag: "COVC" = layer 1 (cgame), "COVX" = layer 2 (exe)
    qboolean    localServer; // cgame: cgs.localServer; exe: com_sv_running->integer at execution
    qboolean    covtrace;    // print the ^~^~^ <tag> VDROP sub-reasons

    // cgame only (NULL in the exe): the in-walk model of writes earlier in the same stufftext, and
    // the recursive check of what a `vstr` expands to. The exe needs neither - it sees the live cvar,
    // and a vstr expansion is itself a server-origin line that gets filtered when it runs.
    const char *(*StageGet)(const char *name);
    void (*StagePut)(const char *name, const char *value);
    qboolean (*VstrValueAllowed)(const srvFilterEnv_t *env, const char *name, int depth);

    // exe only (NULL in cgame): is `name` a registered command or alias? Cmd_ExecuteString runs a
    // command BEFORE it tries a cvar of the same name, so a bare verb admitted only because a cvar of
    // that name exists (coop_* prefix, or user-created) would run the COMMAND.
    qboolean (*IsRegisteredCommand)(const char *name);

    // set internally while validating a guarded value (never by a caller)
    qboolean validating;
};

typedef struct {
    int      argc;
    qboolean overflow; // input too long for buf: the caller must fail closed
    char    *argv[SRVF_MAX_TOKENS];
    char     buf[MAX_STRING_CHARS + SRVF_MAX_TOKENS];
} srvTokens_t;

// qcommon/cmd.c Cmd_TokenizeString2(text, qfalse), into caller storage (never touches Cmd_Argv).
void SrvFilter_Tokenize(srvTokens_t *tok, const char *text);
// qcommon/cmd.c Cmd_ArgsFrom over a srvTokens_t. Returns qfalse if the result did not fit.
qboolean SrvFilter_ArgsFrom(int argc, char **argv, int arg, char *out, size_t outSize);
// qcommon/cmd.c Cbuf_Execute's line scan: length of the statement at `text` (the break position),
// updating the comment state that persists across statements.
int SrvFilter_StatementLength(const char *text, int remaining, qboolean *inStar, qboolean *inSlash);

// The single-statement decision shared by both layers. argv[0] is the verb.
qboolean SrvFilter_CheckArgs(const srvFilterEnv_t *env, int argc, char **argv, int depth);

// Pieces the callers and the self-tests use directly.
qboolean SrvFilter_IsVariableAllowed(const srvFilterEnv_t *env, const char *name);
qboolean SrvFilter_IsNameBusTokenAllowed(const char *tok);
qboolean SrvFilter_IsNumericOrEmpty(const char *s);

#define SRVG_EXACT    0
#define SRVG_DIGITS   1
#define SRVG_NONE     0
#define SRVG_REFUSE   1
#define SRVG_VALIDATE 2
// SRVG_NONE / SRVG_REFUSE / SRVG_VALIDATE for a cvar name (docs/tools/sec2_guardlist.py derives the list).
int SrvFilter_GuardClass(const char *name);

qboolean SrvFilter_DropReason(const srvFilterEnv_t *env, const char *reason, const char *detail);

#ifdef __cplusplus
}
#endif
