/*
===========================================================================
HZM coop [bug-3903, user 2026-10-14] PROGRESSION FOLLOWS THE PLAYER - client half.

The player's OWN save files (coop_mod/save/<type>_<own cl_guid>.dat in this PC's homepath) are the
master copy of his progression. See fgame/g_coopsync.cpp for the server half and the whole design.

  CG_CoopSync_Init      (end of CG_Init, i.e. every map load) - when connected to a server this PC is
                        NOT hosting, send `coopsync begin 2 <7 digests>` (one CRC per own record).
  CG_CoopSync_Command   (server command `coopsave`):
                          want <types>  queue those records as small chunks (the server's copy differs)
                          got <t> <seq> a chunk arrived: send the next one
                          ack           the server merged; it will send records back from now on
                          <type> <part> <nparts> <chunk..>  a record the server saved for us: merge it
                                        into our own file (bg_coopsync.h: progress can only be ADDED)
  CG_CoopSync_Frame     (every frame) - pace the upload: one chunk in flight, >= 1.2 s apart (a dedicated
                        server drops a command that follows the previous one within 1 s), resent if not
                        acknowledged within 3 s. Small chunks because this client exe cannot send a packet
                        that needs fragmenting (see COOPSYNC_UPARGS).
  CG_CoopSync_Shutdown  (CG_Shutdown: disconnect / map change) - fold in the last XP total the server
                        showed us (coop_xp_totalOwn, pushed on every award), so the last minute counts.

SECURITY: the server never names a file. It sends a record TYPE from a fixed list of seven; the path
is built HERE from our own cl_guid. Content is length-capped and printable-only, and every record that
carries progress is merged, never replaced, so a server cannot take progress away. A server that never
answers our handshake (old game.dll) is never listened to at all.
===========================================================================
*/

#include "cg_local.h"
#include "../fgame/bg_coopsync.h"

#define COOPSYNC_QMAX       320
#define COOPSYNC_GAP_MS     1200
#define COOPSYNC_RESEND_MS  3000
#define COOPSYNC_BEGIN_MS   6000

static int  s_coopSyncActive; // we sent a handshake to this (remote) server this cgame session
static int  s_coopSyncAcked;  // the server merged; it will send records back
static char s_coopSyncId[COOPSYNC_MAXID + 1];
static int  s_coopSyncXpMod;  // coop_xp_totalOwn modificationCount at init

// upload queue: one command in flight
typedef struct {
    char cmd[COOPSYNC_ARGLEN * COOPSYNC_UPARGS + 80];
    int  type; // -1 = the closing `end`
    int  seq;
} coopSyncQ_t;

static coopSyncQ_t s_q[COOPSYNC_QMAX];
static int         s_qHead, s_qTail;
static int         s_qSentAt;    // when the head was last sent (0 = not yet)
static int         s_lastSend;   // when ANY coopsync command was last sent
static int         s_beginTries;
static int         s_beginAt;
static char        s_beginCmd[160];

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
    } else if (len == 0) {
        ok = 0; // an empty file
    }
    if (buf) {
        cgi.FS_FreeFile(buf);
    }
    return ok;
}

static void CoopSync_Send(const char *cmd)
{
    cgi.SendClientCommand(cmd);
    s_lastSend = cgi.Milliseconds();
}

void CG_CoopSync_Init(void)
{
    cvar_t *sv, *guid, *xp;
    int     t, o;

    s_coopSyncActive = 0;
    s_coopSyncAcked  = 0;
    s_rxType         = -1;
    s_qHead = s_qTail = 0;
    s_qSentAt        = 0;
    s_beginTries     = 0;

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

    o = Com_sprintf(s_beginCmd, sizeof(s_beginCmd), "coopsync begin %d", COOPSYNC_VERSION);
    for (t = 0; t < COOPSYNC_NTYPES; t++) {
        char dig[16];
        int  len = CoopSync_ReadLocal(t);
        CoopSync_DigestHex(coopSyncRule[t], len >= 0 ? s_local : "", dig);
        o += Com_sprintf(s_beginCmd + o, sizeof(s_beginCmd) - o, " %s", dig);
    }
    CoopSync_Send(s_beginCmd);
    s_beginAt        = cgi.Milliseconds();
    s_beginTries     = 1;
    s_coopSyncActive = 1;
    cgi.Printf("^~^~^ COOPSYNC client handshake id=%.8s\n", s_coopSyncId);
}

static void CoopSync_QueueRecord(int t)
{
    int len = CoopSync_ReadLocal(t);
    int elen, pos = 0, seq = 0;

    if (len < 0) {
        // no file here: say so, so the server sends its copy down
        if (s_qTail < COOPSYNC_QMAX) {
            coopSyncQ_t *q = &s_q[s_qTail++];
            Com_sprintf(q->cmd, sizeof(q->cmd), "coopsync d %s 0 1 -1", coopSyncTypes[t]);
            q->type = t;
            q->seq  = 0;
        }
        return;
    }
    elen = CoopSync_Encode(s_local, len, s_enc);
    if (s_qTail + elen / (COOPSYNC_ARGLEN * COOPSYNC_UPARGS) + 2 >= COOPSYNC_QMAX) {
        cgi.Printf("^~^~^ COOPSYNC client skipped %s (too large to queue)\n", coopSyncTypes[t]);
        return;
    }
    do {
        coopSyncQ_t *q = &s_q[s_qTail++];
        int          a, o, fin;
        int          left = elen - pos;
        fin               = left <= COOPSYNC_ARGLEN * COOPSYNC_UPARGS;
        o = Com_sprintf(q->cmd, sizeof(q->cmd), "coopsync d %s %d %d %d", coopSyncTypes[t], seq, fin, fin ? len : 0);
        for (a = 0; a < COOPSYNC_UPARGS && pos < elen; a++) {
            int n = elen - pos;
            if (n > COOPSYNC_ARGLEN) {
                n = COOPSYNC_ARGLEN;
            }
            q->cmd[o++] = ' ';
            memcpy(q->cmd + o, s_enc + pos, n);
            o += n;
            pos += n;
        }
        q->cmd[o] = 0;
        q->type   = t;
        q->seq    = seq++;
    } while (pos < elen);
}

void CG_CoopSync_Frame(void)
{
    int now;

    if (!s_coopSyncActive || s_coopSyncAcked) {
        return;
    }
    now = cgi.Milliseconds();

    if (s_qHead >= s_qTail) {
        // waiting for the server's `want` (or `ack`): resend a lost / flood-dropped handshake twice
        if (s_beginTries > 0 && s_beginTries < 3 && now - s_beginAt > COOPSYNC_BEGIN_MS && now - s_lastSend >= COOPSYNC_GAP_MS) {
            CoopSync_Send(s_beginCmd);
            s_beginAt = now;
            s_beginTries++;
        }
        return;
    }
    if (now - s_lastSend < COOPSYNC_GAP_MS) {
        return;
    }
    if (s_qSentAt && now - s_qSentAt < COOPSYNC_RESEND_MS) {
        return; // in flight
    }
    CoopSync_Send(s_q[s_qHead].cmd);
    s_qSentAt = now;
}

static void CoopSync_Apply(int t, const char *incoming, int inLen)
{
    int localLen = CoopSync_ReadLocal(t);
    int n;

    n = CoopSync_Merge(coopSyncRule[t], s_local, incoming, s_out);
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

void CG_CoopSync_Command(void)
{
    const char *sub = cgi.Argv(1);
    int         t, part, nparts, i;

    if (!s_coopSyncActive) {
        return; // we never sent a handshake to this server - never take a record from it
    }
    if (!strcmp(sub, "ack")) {
        s_coopSyncAcked = 1;
        s_qHead = s_qTail = 0;
        cgi.Printf("^~^~^ COOPSYNC client acked by server\n");
        return;
    }
    if (!strcmp(sub, "want")) {
        if (s_qTail > 0 || s_coopSyncAcked) {
            return; // already uploading (a duplicate want)
        }
        for (i = 2; i < cgi.Argc(); i++) {
            t = CoopSync_TypeIndex(cgi.Argv(i));
            if (t >= 0) {
                CoopSync_QueueRecord(t);
            }
        }
        if (s_qTail < COOPSYNC_QMAX) {
            coopSyncQ_t *q = &s_q[s_qTail++];
            Q_strncpyz(q->cmd, "coopsync end", sizeof(q->cmd));
            q->type = -1;
            q->seq  = 0;
        }
        s_qSentAt = 0;
        cgi.Printf("^~^~^ COOPSYNC client uploading %d chunks\n", s_qTail);
        return;
    }
    if (!strcmp(sub, "got")) {
        t = CoopSync_TypeIndex(cgi.Argv(2));
        if (s_qHead < s_qTail && t >= 0 && s_q[s_qHead].type == t && s_q[s_qHead].seq == atoi(cgi.Argv(3))) {
            s_qHead++;
            s_qSentAt = 0;
        }
        return;
    }

    // a record: coopsave <type> <part> <nparts> <arg>...
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
    s_qHead = s_qTail = 0;
}
