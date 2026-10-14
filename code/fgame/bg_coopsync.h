/*
===========================================================================
HZM coop [bug-3903, user 2026-10-14] PROGRESSION FOLLOWS THE PLAYER - shared helpers.

Included by BOTH fgame/g_coopsync.cpp (server side) and cgame/cg_coopsync.c (client side), so the
record list, the wire encoding and the merge rules can never drift apart between the two halves.
Everything is static: each module gets its own copy, no link-time coupling, plain C that also
compiles as C++.

The records are the per-player save files the mod's scripts already write with fs_write_content:
    coop_mod/save/<type>_<id>.dat      id = the player's cl_guid (coop_mod/xp.scr::xp_identify)

Merge rules (README: docs/proposals/progression_sync_2026-10-14/README.md):
    xp                 the higher integer
    chal               "cid:progress|..." - per cid the higher progress, union of cids
    unlocks, medals    "a|b|c" - union
    bpfound            "m_n|m_n|" (trailing '|') - union
    pins, pend         no merge: the newest copy wins (caller decides which one that is)
A merge can only ever ADD progress, so a stale or hostile copy cannot take anything away.
===========================================================================
*/
#ifndef BG_COOPSYNC_H
#define BG_COOPSYNC_H

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define COOPSYNC_VERSION 1
#define COOPSYNC_NTYPES  7
#define COOPSYNC_MAXLEN  32768 // per record, decoded; real records are a few KB
#define COOPSYNC_ARGLEN  240   // one wire argument (the server truncates client args at 255)
#define COOPSYNC_ARGS    6     // wire arguments per command (well inside MAX_STRING_CHARS 2048)
#define COOPSYNC_MAXID   64

enum {
    COOPSYNC_MERGE_MAX,
    COOPSYNC_MERGE_CHAL,
    COOPSYNC_MERGE_UNION,
    COOPSYNC_MERGE_UNION_TRAIL,
    COOPSYNC_MERGE_NEWEST
};

static const char *const coopSyncTypes[COOPSYNC_NTYPES] = {"xp", "chal", "unlocks", "pins", "pend", "medals", "bpfound"};

static const int coopSyncRule[COOPSYNC_NTYPES] = {
    COOPSYNC_MERGE_MAX,
    COOPSYNC_MERGE_CHAL,
    COOPSYNC_MERGE_UNION,
    COOPSYNC_MERGE_NEWEST,
    COOPSYNC_MERGE_NEWEST,
    COOPSYNC_MERGE_UNION,
    COOPSYNC_MERGE_UNION_TRAIL
};

// The server-side session cache cvar each script keeps beside the file (read FIRST by the scripts).
// bpfound has none.
static const char *const coopSyncSessionCvar[COOPSYNC_NTYPES] = {
    "coop_xp_", "coop_chal_", "coop_unlocks_", "coop_pins_", "coop_pend_", "coop_medals_", NULL
};

static int CoopSync_TypeIndex(const char *t)
{
    int i;
    if (!t) {
        return -1;
    }
    for (i = 0; i < COOPSYNC_NTYPES; i++) {
        if (!strcmp(t, coopSyncTypes[i])) {
            return i;
        }
    }
    return -1;
}

// A save-file id: cl_guid is 32 hex chars. Anything that could form a path is refused.
static int CoopSync_ValidId(const char *id)
{
    int n = 0;
    if (!id || !id[0]) {
        return 0;
    }
    for (; *id; id++, n++) {
        char c = *id;
        if (n >= COOPSYNC_MAXID) {
            return 0;
        }
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '-')) {
            return 0;
        }
    }
    return 1;
}

// Record content: printable ASCII only (what every script writer produces), bounded.
static int CoopSync_ValidContent(const char *s, int len)
{
    int i;
    if (len < 0 || len > COOPSYNC_MAXLEN) {
        return 0;
    }
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x20 || c > 0x7e) {
            return 0;
        }
    }
    return 1;
}

// Wire encoding: everything outside a small safe set becomes %XX, so a token can never carry a
// space, quote, ';', '/', '$' or line break through Cmd_TokenizeString / Cmd_Args_Sanitize.
static int CoopSync_SafeChar(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '|'
        || c == ':' || c == '.' || c == '~' || c == ',' || c == '-';
}

// returns the encoded length; out must hold 3*len+1
static int CoopSync_Encode(const char *in, int len, char *out)
{
    static const char hex[] = "0123456789ABCDEF";
    int               i, o = 0;
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)in[i];
        if (CoopSync_SafeChar((char)c)) {
            out[o++] = (char)c;
        } else {
            out[o++] = '%';
            out[o++] = hex[c >> 4];
            out[o++] = hex[c & 15];
        }
    }
    out[o] = 0;
    return o;
}

static int CoopSync_HexVal(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1;
}

// returns the decoded length, or -1 on malformed input / overflow; out must hold outSize
static int CoopSync_Decode(const char *in, char *out, int outSize)
{
    int o = 0;
    while (*in) {
        int c;
        if (*in == '%') {
            int h = CoopSync_HexVal(in[1]);
            int l = (h >= 0) ? CoopSync_HexVal(in[2]) : -1;
            if (h < 0 || l < 0) {
                return -1;
            }
            c = (h << 4) | l;
            in += 3;
        } else {
            c = (unsigned char)*in;
            in++;
        }
        if (o >= outSize - 1) {
            return -1;
        }
        out[o++] = (char)c;
    }
    out[o] = 0;
    return o;
}

// ---- merge ----------------------------------------------------------------------------------

#define COOPSYNC_MAXTOK 2048

typedef struct {
    int  n;
    char tok[COOPSYNC_MAXTOK][160];
} coopSyncTokens_t;

// split on sep into the token list, appending. Returns 0 if a token or the count does not fit - the
// caller must then NOT merge (a dropped token would be lost progress).
static int CoopSync_Split(const char *s, char sep, coopSyncTokens_t *t)
{
    while (s && *s) {
        const char *e = strchr(s, sep);
        int         len = e ? (int)(e - s) : (int)strlen(s);
        if (len > 0) {
            if (len >= (int)sizeof(t->tok[0]) || t->n >= COOPSYNC_MAXTOK) {
                return 0;
            }
            memcpy(t->tok[t->n], s, len);
            t->tok[t->n][len] = 0;
            t->n++;
        }
        if (!e) {
            break;
        }
        s = e + 1;
    }
    return 1;
}

static int CoopSync_FindTok(const coopSyncTokens_t *t, const char *tok, int keyLen)
{
    int i;
    for (i = 0; i < t->n; i++) {
        if (keyLen >= 0) {
            if (!strncmp(t->tok[i], tok, keyLen) && t->tok[i][keyLen] == ':') {
                return i;
            }
        } else if (!strcmp(t->tok[i], tok)) {
            return i;
        }
    }
    return -1;
}

// length of the "cid" part of "cid:progress", or -1 when the token has no ':'
static int CoopSync_ChalKey(const char *tok)
{
    const char *c = strchr(tok, ':');
    return c ? (int)(c - tok) : -1;
}

/*
Merge record b INTO a (a's order is kept, b's new entries are appended). out must hold
COOPSYNC_MAXLEN + 1. For COOPSYNC_MERGE_NEWEST the caller picks; this returns b when b is non-NULL.
Returns the output length, or -1 if the result would not fit (the caller then keeps a unchanged).
*/
static int CoopSync_Merge(int rule, const char *a, const char *b, char *out)
{
    static coopSyncTokens_t ta;
    static coopSyncTokens_t tb;
    int                     i, o = 0;

    if (!a) {
        a = "";
    }
    if (!b) {
        b = "";
    }
    out[0] = 0;

    if (rule == COOPSYNC_MERGE_NEWEST) {
        if ((int)strlen(b) > COOPSYNC_MAXLEN) {
            return -1;
        }
        strcpy(out, b);
        return (int)strlen(out);
    }

    if (rule == COOPSYNC_MERGE_MAX) {
        long va = atol(a), vb = atol(b);
        if (!a[0] && !b[0]) {
            return 0;
        }
        if (!b[0] || (a[0] && va >= vb)) {
            vb = va;
        }
        if (vb < 0) {
            vb = 0;
        }
        sprintf(out, "%ld", vb);
        return (int)strlen(out);
    }

    ta.n = 0;
    tb.n = 0;
    if (!CoopSync_Split(a, '|', &ta) || !CoopSync_Split(b, '|', &tb)) {
        return -1;
    }

    for (i = 0; i < tb.n; i++) {
        int k = (rule == COOPSYNC_MERGE_CHAL) ? CoopSync_ChalKey(tb.tok[i]) : -1;
        int j = CoopSync_FindTok(&ta, tb.tok[i], k);
        if (j < 0) {
            if (ta.n >= COOPSYNC_MAXTOK) {
                return -1;
            }
            strcpy(ta.tok[ta.n++], tb.tok[i]);
        } else if (k >= 0) {
            long pa = atol(ta.tok[j] + k + 1);
            long pb = atol(tb.tok[i] + k + 1);
            if (pb > pa) {
                strcpy(ta.tok[j], tb.tok[i]);
            }
        }
    }

    for (i = 0; i < ta.n; i++) {
        int len = (int)strlen(ta.tok[i]);
        if (o + len + 1 > COOPSYNC_MAXLEN) {
            return -1;
        }
        if (rule != COOPSYNC_MERGE_UNION_TRAIL && i > 0) {
            out[o++] = '|';
        }
        memcpy(out + o, ta.tok[i], len);
        o += len;
        if (rule == COOPSYNC_MERGE_UNION_TRAIL) {
            out[o++] = '|';
        }
    }
    out[o] = 0;
    return o;
}

#endif
