/*
===========================================================================
HZM coop [bug-3903, user 2026-10-14] PROGRESSION FOLLOWS THE PLAYER - client half.

The player's OWN save files (coop_mod/save/<type>_<own cl_guid>.dat in this PC's homepath) are the
master copy of his progression. See fgame/g_coopsync.cpp for the server half and the whole design.

  CG_CoopSync_Init      (end of CG_Init, i.e. every map load) - when connected to a server this PC is
                        NOT hosting, send our records up: coopsync begin / d / have / end.
  CG_CoopSync_Command   (server command `coopsave`) - a record the server just saved for us: merge it
                        into our own file (bg_coopsync.h rules: progress can only be ADDED) and write it.
  CG_CoopSync_Shutdown  (CG_Shutdown: disconnect / map change) - fold in the last XP total the server
                        showed us (coop_xp_totalOwn, pushed on every award), so the last minute counts.

SECURITY: the server never names a file. It sends a record TYPE from a fixed list of seven; the path
is built HERE from our own cl_guid. Content is length-capped and printable-only, and every record that
carries progress is merged, never replaced, so a server cannot take progress away. A server that never
answers our upload (old game.dll) is never listened to at all.
===========================================================================
*/

#include "cg_local.h"
#include "../fgame/bg_coopsync.h"

static int  s_coopSyncActive; // we uploaded to this server (a remote one) this cgame session
static int  s_coopSyncAcked;  // the server merged it and will send records back
static char s_coopSyncId[COOPSYNC_MAXID + 1];
static int  s_coopSyncXpMod;  // coop_xp_totalOwn modificationCount at init

static int  s_rxType = -1;
static int  s_rxNext;
static char s_rxEnc[COOPSYNC_MAXLEN * 3 + 1];
static int  s_rxEncLen;
static char s_rxDec[COOPSYNC_MAXLEN + 1];
static char s_local[COOPSYNC_MAXLEN + 1];
static char s_out[COOPSYNC_MAXLEN + 1];
static char s_enc[COOPSYNC_MAXLEN * 3 + 1];

static const char *CoopSync_Path(int t)
{
    return va("coop_mod/save/%s_%s.dat", coopSyncTypes[t], s_coopSyncId);
}

// our own copy of record t into s_local; returns its length, or -1 when we have no (usable) file
static int CoopSync_ReadLocal(int t)
{
    void *buf = NULL;
    long  len = cgi.FS_ReadFile(CoopSync_Path(t), &buf, qtrue);
    int   ok  = -1;

    s_local[0] = 0;
    if (len >= 0 && buf) {
        if (len <= COOPSYNC_MAXLEN && CoopSync_ValidContent((const char *)buf, (int)len)) {
            memcpy(s_local, buf, len);
            s_local[len] = 0;
            ok           = (int)len;
        }
    }
    if (buf) {
        cgi.FS_FreeFile(buf);
    }
    return ok;
}

void CG_CoopSync_Init(void)
{
    cvar_t *sv, *guid, *xp;
    int     t, sent = 0;
    char    cmd[1800];

    s_coopSyncActive = 0;
    s_coopSyncAcked  = 0;
    s_rxType         = -1;

    sv = cgi.Cvar_Get("sv_running", "0", 0);
    if (sv && sv->integer) {
        // this PC hosts the game: its server already reads and writes these very files
        return;
    }
    guid = cgi.Cvar_Get("cl_guid", "", 0);
    if (!guid || !CoopSync_ValidId(guid->string)) {
        return;
    }
    Q_strncpyz(s_coopSyncId, guid->string, sizeof(s_coopSyncId));

    xp              = cgi.Cvar_Get("coop_xp_totalOwn", "", 0);
    s_coopSyncXpMod = xp ? xp->modificationCount : 0;

    cgi.SendClientCommand(va("coopsync begin %d", COOPSYNC_VERSION));
    for (t = 0; t < COOPSYNC_NTYPES; t++) {
        int len = CoopSync_ReadLocal(t);
        int elen, pos = 0;
        if (len < 0) {
            continue;
        }
        elen = CoopSync_Encode(s_local, len, s_enc);
        while (pos < elen) {
            int a, o = Com_sprintf(cmd, sizeof(cmd), "coopsync d %s", coopSyncTypes[t]);
            for (a = 0; a < COOPSYNC_ARGS && pos < elen; a++) {
                int n = elen - pos;
                if (n > COOPSYNC_ARGLEN) {
                    n = COOPSYNC_ARGLEN;
                }
                cmd[o++] = ' ';
                memcpy(cmd + o, s_enc + pos, n);
                o += n;
                pos += n;
            }
            cmd[o] = 0;
            cgi.SendClientCommand(cmd);
        }
        cgi.SendClientCommand(va("coopsync have %s %d", coopSyncTypes[t], len));
        sent++;
    }
    cgi.SendClientCommand("coopsync end");
    s_coopSyncActive = 1;
    cgi.Printf("^~^~^ COOPSYNC client sent %d records id=%.8s\n", sent, s_coopSyncId);
}

static void CoopSync_Apply(int t, const char *incoming, int inLen)
{
    int localLen = CoopSync_ReadLocal(t);
    int n;

    if (coopSyncRule[t] == COOPSYNC_MERGE_NEWEST) {
        n = CoopSync_Merge(COOPSYNC_MERGE_NEWEST, s_local, incoming, s_out);
    } else {
        n = CoopSync_Merge(coopSyncRule[t], s_local, incoming, s_out);
    }
    if (n < 0) {
        cgi.Printf("^~^~^ COOPSYNC client kept %s (merge overflow)\n", coopSyncTypes[t]);
        return;
    }
    if (localLen >= 0 && !strcmp(s_out, s_local)) {
        return; // nothing new
    }
    cgi.FS_WriteFile(CoopSync_Path(t), s_out, n);
    cgi.Printf("^~^~^ COOPSYNC client wrote %s (%d bytes, was %d)\n", coopSyncTypes[t], n, localLen);
}

// server command: coopsave ack <ver> | coopsave <type> <part> <nparts> <arg>...
void CG_CoopSync_Command(void)
{
    const char *sub = cgi.Argv(1);
    int         t, part, nparts, i;

    if (!s_coopSyncActive) {
        return; // we never uploaded to this server - never take a record from it
    }
    if (!strcmp(sub, "ack")) {
        s_coopSyncAcked = 1;
        cgi.Printf("^~^~^ COOPSYNC client acked by server\n");
        return;
    }
    t = CoopSync_TypeIndex(sub);
    if (t < 0) {
        return;
    }
    part   = atoi(cgi.Argv(2));
    nparts = atoi(cgi.Argv(3));
    if (nparts < 1 || part < 0 || part >= nparts) {
        s_rxType = -1;
        return;
    }
    if (part == 0) {
        s_rxType   = t;
        s_rxNext   = 0;
        s_rxEncLen = 0;
        s_rxEnc[0] = 0;
    }
    if (s_rxType != t || part != s_rxNext) {
        s_rxType = -1; // out of order: drop this record, the next save resends it whole
        return;
    }
    for (i = 4; i < cgi.Argc(); i++) {
        const char *a = cgi.Argv(i);
        int         n = (int)strlen(a);
        if (s_rxEncLen + n >= (int)sizeof(s_rxEnc)) {
            s_rxType = -1;
            return;
        }
        memcpy(s_rxEnc + s_rxEncLen, a, n);
        s_rxEncLen += n;
        s_rxEnc[s_rxEncLen] = 0;
    }
    s_rxNext = part + 1;
    if (part == nparts - 1) {
        int n    = CoopSync_Decode(s_rxEnc, s_rxDec, sizeof(s_rxDec));
        s_rxType = -1;
        if (n < 0 || !CoopSync_ValidContent(s_rxDec, n)) {
            return;
        }
        CoopSync_Apply(t, s_rxDec, n);
    }
}

void CG_CoopSync_Shutdown(void)
{
    cvar_t *xp;

    if (s_coopSyncActive && s_coopSyncAcked) {
        xp = cgi.Cvar_Get("coop_xp_totalOwn", "", 0);
        // only a value this server pushed during this session (not one archived from an older one)
        if (xp && xp->modificationCount != s_coopSyncXpMod && xp->string[0] && atol(xp->string) > 0) {
            char v[32];
            Com_sprintf(v, sizeof(v), "%ld", atol(xp->string));
            CoopSync_Apply(0, v, (int)strlen(v));
        }
    }
    s_coopSyncActive = 0;
    s_coopSyncAcked  = 0;
    s_rxType         = -1;
}
