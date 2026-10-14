/*
===========================================================================
HZM coop [bug-3903, user 2026-10-14] PROGRESSION FOLLOWS THE PLAYER - server half.

Before this, XP / challenges / unlocks / pins / pending unlocks / medals / blueprints were kept ONLY
by the machine hosting the game (coop_mod/save/<type>_<id>.dat in its homepath). A player's own PC
now holds the master copy:

  1. HANDSHAKE - while the map loads, a new cgame (cg_coopsync.c) sends `coopsync begin 2 <7 digests>`:
               one CRC per record of its own save files (bg_coopsync.h, order-independent). The server
               compares them with what IT holds for that player and answers `coopsave want <types>` for
               the records that differ - normally nothing on a map change of the same server - or, when
               everything already agrees, merges at once.
  2. UPLOAD  - wanted records come up as `coopsync d <type> <seq> <final> <len> <chunk..>`, ONE small
               chunk at a time, each acknowledged with `coopsave got <type> <seq>`; the client resends a
               chunk that was not acknowledged. Two engine facts force this (harness run r3): the client
               exe never sends the second fragment of a packet of ~1300+ bytes (connection stalls), and a
               dedicated server silently drops a client command arriving within 1 s of the previous one
               (sv_floodProtect, SV_ClientCommand) once the client is in the game.
  3. MERGE   - on `coopsync end` the records are merged (can only ADD progress) with this server's file
               AND its session cvar, and both are rewritten, so the UNCHANGED script read paths (session
               cvar first, then file) see the merged record. The player entity gets `coop_syncstate` 2;
               xp.scr::xp_identify waits (bounded) while it is 1.
  4. DOWNLOAD - every fs_write_content of one of those files (ScriptThread::FS_WriteContent calls
               G_CoopSync_OnWrite) is parked and sent by G_CoopSync_Frame to the owning client as
               `coopsave <type> <part> <nparts> <chunk..>` server commands; the cgame merges it into its own
               file. No script save site had to change.

Nothing happens for a client that never sends `begin` (old cgame, or the listen-server host, whose game
and server share one homepath), so both keep today's behaviour exactly.
===========================================================================
*/

#include "g_local.h"
#include "game.h"
#include "level.h"
#include "entity.h"
#include "scriptvariable.h"
#include "bg_coopsync.h"

enum {
    COOPSYNC_NONE      = 0,
    COOPSYNC_RECEIVING = 1,
    COOPSYNC_MERGED    = 2,
    COOPSYNC_FAILED    = 3
};

#define COOPSYNC_RECV_TIMEOUT_MS 120000

typedef struct {
    int  state;
    int  since;                 // level.inttime of `begin`
    char id[COOPSYNC_MAXID + 1];
    str  enc[COOPSYNC_NTYPES];  // encoded chunks as received
    str  rec[COOPSYNC_NTYPES];  // decoded, validated record
    int  have[COOPSYNC_NTYPES]; // the client sent this record (complete and valid)
    int  want[COOPSYNC_NTYPES]; // digests differed: we asked for it
    int  same[COOPSYNC_NTYPES]; // digests matched: nothing to transfer either way
    int  next[COOPSYNC_NTYPES]; // next expected chunk seq
} coopSyncClient_t;

static coopSyncClient_t s_coopSync[MAX_CLIENTS];
static char             s_mergeOut[COOPSYNC_MAXLEN + 1];
static char             s_decode[COOPSYNC_MAXLEN + 1];
static char             s_encode[COOPSYNC_MAXLEN * 3 + 1];
static char             s_fileBuf[COOPSYNC_MAXLEN + 1];

// DOWNLOAD COALESCING. One game action can write the same record many times in a row - joining a server
// for the first time re-grants every rank unlock (xp.scr::xp_rank_unlock -> chal_add_unlock), and each grant
// rewrites unlocks_ AND pend_. Sending every write would put hundreds of reliable commands on a client that
// is still loading (the bug-670 ring overflow). So a write only parks the newest content; G_CoopSync_Frame
// sends it, at most once per COOPSYNC_SEND_MS per client and record. The first write after a quiet spell
// goes out on the next frame, so a mission-end flush is not delayed.
#define COOPSYNC_SEND_MS 1000
static str s_dlPending[MAX_CLIENTS][COOPSYNC_NTYPES];
static int s_dlDirty[MAX_CLIENTS][COOPSYNC_NTYPES];
static int s_dlLast[MAX_CLIENTS][COOPSYNC_NTYPES];

static void CoopSync_Reset(int cn)
{
    int i;
    s_coopSync[cn].state = COOPSYNC_NONE;
    s_coopSync[cn].since = 0;
    s_coopSync[cn].id[0] = 0;
    for (i = 0; i < COOPSYNC_NTYPES; i++) {
        s_coopSync[cn].enc[i]  = "";
        s_coopSync[cn].rec[i]  = "";
        s_coopSync[cn].have[i] = 0;
        s_coopSync[cn].want[i] = 0;
        s_coopSync[cn].same[i] = 0;
        s_coopSync[cn].next[i] = 0;
        s_dlPending[cn][i]     = "";
        s_dlDirty[cn][i]       = 0;
        s_dlLast[cn][i]        = -100000;
    }
}

void G_CoopSync_ClientConnect(int cn)
{
    if (cn >= 0 && cn < MAX_CLIENTS) {
        CoopSync_Reset(cn);
    }
}

void G_CoopSync_ClientDisconnect(int cn)
{
    if (cn >= 0 && cn < MAX_CLIENTS) {
        CoopSync_Reset(cn);
    }
}

// Publish the state to script (read as local.player.coop_syncstate). Safe to call any time.
void G_CoopSync_PushVar(gentity_t *ent)
{
    int cn;
    if (!ent || !ent->client || !ent->entity) {
        return;
    }
    cn = ent - g_entities;
    if (cn < 0 || cn >= MAX_CLIENTS || s_coopSync[cn].state == COOPSYNC_NONE) {
        return;
    }
    ent->entity->Vars()->SetVariable("coop_syncstate", s_coopSync[cn].state);
}

// Send one record to one client: coopsave <type> <part> <nparts> <arg>...
static void CoopSync_SendRecord(int cn, int type, const char *content, int len)
{
    int  elen, nparts, part, pos;
    char cmd[1800];

    elen   = CoopSync_Encode(content, len, s_encode);
    nparts = (elen + COOPSYNC_ARGLEN * COOPSYNC_ARGS - 1) / (COOPSYNC_ARGLEN * COOPSYNC_ARGS);
    if (nparts < 1) {
        nparts = 1;
    }
    pos = 0;
    for (part = 0; part < nparts; part++) {
        int a, o;
        o = Com_sprintf(cmd, sizeof(cmd), "coopsave %s %d %d", coopSyncTypes[type], part, nparts);
        for (a = 0; a < COOPSYNC_ARGS && pos < elen; a++) {
            int n = elen - pos;
            if (n > COOPSYNC_ARGLEN) {
                n = COOPSYNC_ARGLEN;
            }
            cmd[o++] = ' ';
            memcpy(cmd + o, s_encode + pos, n);
            o += n;
            pos += n;
        }
        cmd[o] = 0;
        gi.SendServerCommand(cn, "%s", cmd);
    }
}

// The server's own view of a record: its session cvar (what the scripts read first) and its file.
static void CoopSync_ReadServer(int type, const char *id, str& fileOut, str& cvarOut, int& cvarLive)
{
    void *buf = NULL;
    long  len;

    fileOut  = "";
    cvarOut  = "";
    cvarLive = 0;

    len = gi.FS_ReadFile(va("coop_mod/save/%s_%s.dat", coopSyncTypes[type], id), &buf, qtrue);
    if (len > 0 && buf) {
        if (len <= COOPSYNC_MAXLEN && CoopSync_ValidContent((const char *)buf, (int)len)) {
            memcpy(s_fileBuf, buf, len);
            s_fileBuf[len] = 0;
            fileOut        = s_fileBuf;
        }
    }
    if (buf) {
        gi.FS_FreeFile(buf);
    }

    if (coopSyncSessionCvar[type]) {
        cvar_t *cv = gi.Cvar_Get(va("%s%s", coopSyncSessionCvar[type], id), "", 0);
        if (cv && cv->string && cv->string[0]) {
            cvarOut  = cv->string;
            cvarLive = 1;
        }
    }
}

// What this server holds for a record, as the scripts would see it (merged file + session cvar). 0 = overflow.
static int CoopSync_ServerView(int type, const char *id, str& out)
{
    str sFile, sCvar;
    int cvarLive;

    CoopSync_ReadServer(type, id, sFile, sCvar, cvarLive);
    if (coopSyncRule[type] == COOPSYNC_MERGE_NEWEST) {
        out = cvarLive ? sCvar : sFile;
        return 1;
    }
    if (CoopSync_Merge(coopSyncRule[type], sFile.c_str(), sCvar.c_str(), s_mergeOut) < 0) {
        return 0;
    }
    out = s_mergeOut;
    return 1;
}

static void CoopSync_MergeAll(gentity_t *ent, int cn)
{
    coopSyncClient_t *c = &s_coopSync[cn];
    int               t, changedServer = 0, sentBack = 0, same = 0;
    char              summary[256];

    summary[0] = 0;
    for (t = 0; t < COOPSYNC_NTYPES; t++) {
        str  sFile, sCvar, merged;
        int  cvarLive, ok = 1;
        const char *server;

        if (c->same[t]) {
            same++;
            continue; // this PC and this server already agree
        }

        CoopSync_ReadServer(t, c->id, sFile, sCvar, cvarLive);
        // the session cvar is the scripts' live copy during a server session; the file can lag it
        server = cvarLive ? sCvar.c_str() : sFile.c_str();

        if (coopSyncRule[t] == COOPSYNC_MERGE_NEWEST) {
            // preferences / display queue: the player's own copy wins on join; else keep the server's
            merged = c->have[t] ? c->rec[t] : str(server);
        } else {
            // client + server file + server cvar: three-way, can only add
            int n = CoopSync_Merge(coopSyncRule[t], c->have[t] ? c->rec[t].c_str() : "", sFile.c_str(), s_mergeOut);
            if (n < 0) {
                ok = 0;
            } else {
                str tmp = s_mergeOut;
                n       = CoopSync_Merge(coopSyncRule[t], tmp.c_str(), sCvar.c_str(), s_mergeOut);
                if (n < 0) {
                    ok = 0;
                } else {
                    merged = s_mergeOut;
                }
            }
        }
        if (!ok) {
            gi.Printf("^~^~^ COOPSYNC merge overflow cl=%d type=%s - server copy kept\n", cn, coopSyncTypes[t]);
            continue;
        }

        if (strcmp(merged.c_str(), sFile.c_str()) && (merged.length() || sFile.length())) {
            gi.FS_WriteFile(va("coop_mod/save/%s_%s.dat", coopSyncTypes[t], c->id), merged.c_str(), merged.length());
            changedServer++;
        }
        if (cvarLive && strcmp(merged.c_str(), sCvar.c_str())) {
            gi.cvar_set(va("%s%s", coopSyncSessionCvar[t], c->id), merged.c_str());
        }
        if (!c->have[t] || strcmp(merged.c_str(), c->rec[t].c_str())) {
            if (merged.length()) {
                CoopSync_SendRecord(cn, t, merged.c_str(), merged.length());
                sentBack++;
            }
        }
        if (t == 0) {
            Com_sprintf(
                summary,
                sizeof(summary),
                " xp=%s(client %s, server %s)",
                merged.c_str(),
                c->have[t] ? c->rec[t].c_str() : "-",
                server[0] ? server : "-"
            );
        }
    }

    c->state = COOPSYNC_MERGED;
    gi.SendServerCommand(cn, "coopsave ack %d", COOPSYNC_VERSION);
    gi.Printf(
        "^~^~^ COOPSYNC merged cl=%d id=%.8s..%s already_same=%d files_written=%d sent_back=%d after=%dms\n",
        cn,
        c->id,
        summary,
        same,
        changedServer,
        sentBack,
        level.inttime - c->since
    );
    G_CoopSync_PushVar(ent);
}

/*
Client command `coopsync ...`. Returns qtrue when it was ours (handled, whatever the outcome).
Runs BEFORE G_ProcessClientCommand, because the handshake arrives before the Player entity exists.
*/
qboolean G_CoopSync_Command(gentity_t *ent)
{
    const char       *sub;
    int               cn, t, i;
    coopSyncClient_t *c;
    char              userinfo[MAX_INFO_STRING];
    const char       *guid;

    if (Q_stricmp(gi.Argv(0), "coopsync")) {
        return qfalse;
    }
    if (!ent || !ent->client) {
        return qtrue;
    }
    cn = ent - g_entities;
    if (cn < 0 || cn >= game.maxclients || cn >= MAX_CLIENTS) {
        return qtrue;
    }
    c   = &s_coopSync[cn];
    sub = gi.Argv(1);

    if (!Q_stricmp(sub, "begin")) {
        char want[128];
        int  nwant = 0, wo = 0;

        if (c->state == COOPSYNC_MERGED) {
            // a cgame reload (vid_restart) on a live connection, or a resent begin: already merged
            gi.SendServerCommand(cn, "coopsave ack %d", COOPSYNC_VERSION);
            return qtrue;
        }
        if (c->state == COOPSYNC_RECEIVING) {
            return qtrue; // a resent begin while we wait for chunks: the `want` is already on its way
        }
        CoopSync_Reset(cn);
        gi.GetUserinfo(cn, userinfo, sizeof(userinfo));
        if (!strcmp(Info_ValueForKey(userinfo, "ip"), "localhost")) {
            // the listen-server host: his game and this server share one set of files
            return qtrue;
        }
        guid = Info_ValueForKey(userinfo, "cl_guid");
        if (!CoopSync_ValidId(guid) || atoi(gi.Argv(2)) < 2 || gi.Argc() < 3 + COOPSYNC_NTYPES) {
            gi.Printf("^~^~^ COOPSYNC refused cl=%d (no usable cl_guid or old handshake)\n", cn);
            return qtrue;
        }
        Q_strncpyz(c->id, guid, sizeof(c->id));
        c->since = level.inttime;

        want[0] = 0;
        for (t = 0; t < COOPSYNC_NTYPES; t++) {
            str  view;
            char dig[16];
            if (!CoopSync_ServerView(t, c->id, view)) {
                strcpy(dig, "x");
            } else {
                CoopSync_DigestHex(coopSyncRule[t], view.c_str(), dig);
            }
            if (strcmp(dig, "x") && !Q_stricmp(dig, gi.Argv(3 + t))) {
                c->same[t] = 1;
            } else {
                c->want[t] = 1;
                nwant++;
                wo += Com_sprintf(want + wo, sizeof(want) - wo, " %s", coopSyncTypes[t]);
            }
        }
        c->state = COOPSYNC_RECEIVING;
        if (!nwant) {
            CoopSync_MergeAll(ent, cn); // everything already agrees: nothing to upload
            return qtrue;
        }
        G_CoopSync_PushVar(ent);
        gi.SendServerCommand(cn, "coopsave want%s", want);
        gi.Printf("^~^~^ COOPSYNC want cl=%d id=%.8s..%s\n", cn, c->id, want);
        return qtrue;
    }

    if (c->state == COOPSYNC_MERGED && !Q_stricmp(sub, "end")) {
        gi.SendServerCommand(cn, "coopsave ack %d", COOPSYNC_VERSION); // a resent end: the ack was lost
        return qtrue;
    }
    if (c->state != COOPSYNC_RECEIVING) {
        return qtrue;
    }

    if (!Q_stricmp(sub, "d")) {
        int seq, fin, want;
        t   = CoopSync_TypeIndex(gi.Argv(2));
        seq = atoi(gi.Argv(3));
        fin = atoi(gi.Argv(4));
        want = atoi(gi.Argv(5));
        if (t < 0 || !c->want[t]) {
            return qtrue;
        }
        if (seq < c->next[t]) {
            gi.SendServerCommand(cn, "coopsave got %s %d", coopSyncTypes[t], seq); // a duplicate: re-ack
            return qtrue;
        }
        if (seq > c->next[t]) {
            return qtrue; // a gap (a chunk the flood protection dropped): wait for the resend
        }
        for (i = 6; i < gi.Argc(); i++) {
            c->enc[t] += gi.Argv(i);
        }
        c->next[t]++;
        if (c->enc[t].length() > COOPSYNC_MAXLEN * 3) {
            gi.Printf("^~^~^ COOPSYNC dropped cl=%d type=%s (too large)\n", cn, coopSyncTypes[t]);
            c->enc[t]  = "";
            c->want[t] = 0;
        } else if (fin) {
            int n     = (want < 0) ? 0 : CoopSync_Decode(c->enc[t].c_str(), s_decode, sizeof(s_decode));
            c->enc[t] = "";
            if (want < 0) {
                c->have[t] = 0; // the client has no such file
            } else if (n != want || !CoopSync_ValidContent(s_decode, n)) {
                gi.Printf("^~^~^ COOPSYNC dropped cl=%d type=%s (got %d of %d)\n", cn, coopSyncTypes[t], n, want);
            } else {
                c->rec[t]  = s_decode;
                c->have[t] = 1;
            }
        }
        gi.SendServerCommand(cn, "coopsave got %s %d", coopSyncTypes[t], seq);
        return qtrue;
    }

    if (!Q_stricmp(sub, "end")) {
        CoopSync_MergeAll(ent, cn);
        return qtrue;
    }

    return qtrue;
}

/*
Called by ScriptThread::FS_WriteContent after every script file write. If the path is one of the
synced records of a connected client whose upload was merged, the newest content is parked for it.
*/
void G_CoopSync_OnWrite(const char *path, const char *content, int len)
{
    static const char prefix[] = "coop_mod/save/";
    const char       *rest;
    const char       *dot;
    char              id[COOPSYNC_MAXID + 1];
    int               t, cn, idLen;

    if (!path || Q_stricmpn(path, prefix, sizeof(prefix) - 1)) {
        return;
    }
    rest = path + sizeof(prefix) - 1;
    for (t = 0; t < COOPSYNC_NTYPES; t++) {
        int tl = strlen(coopSyncTypes[t]);
        if (!strncmp(rest, coopSyncTypes[t], tl) && rest[tl] == '_') {
            break;
        }
    }
    if (t >= COOPSYNC_NTYPES) {
        return;
    }
    rest += strlen(coopSyncTypes[t]) + 1;
    dot = strrchr(rest, '.');
    if (!dot || Q_stricmp(dot, ".dat")) {
        return;
    }
    idLen = dot - rest;
    if (idLen <= 0 || idLen > COOPSYNC_MAXID) {
        return;
    }
    memcpy(id, rest, idLen);
    id[idLen] = 0;

    if (!CoopSync_ValidContent(content, len)) {
        return;
    }

    for (cn = 0; cn < game.maxclients && cn < MAX_CLIENTS; cn++) {
        if (s_coopSync[cn].state != COOPSYNC_MERGED || strcmp(s_coopSync[cn].id, id)) {
            continue;
        }
        if (!g_entities[cn].inuse || !g_entities[cn].client) {
            continue;
        }
        s_dlPending[cn][t] = content;
        s_dlDirty[cn][t]   = 1;
    }
}

// Every server frame (G_RunFrame): send each client's parked records, rate-limited per record; give up on
// an upload that never finished (the scripts stopped waiting long ago; no downloads for a FAILED client).
void G_CoopSync_Frame(void)
{
    int cn, t;
    int now = level.inttime;

    for (cn = 0; cn < game.maxclients && cn < MAX_CLIENTS; cn++) {
        if (s_coopSync[cn].state == COOPSYNC_RECEIVING && now - s_coopSync[cn].since > COOPSYNC_RECV_TIMEOUT_MS) {
            s_coopSync[cn].state = COOPSYNC_FAILED;
            gi.Printf("^~^~^ COOPSYNC upload timed out cl=%d - this session uses the server's copy only\n", cn);
            G_CoopSync_PushVar(&g_entities[cn]);
            continue;
        }
        if (s_coopSync[cn].state != COOPSYNC_MERGED) {
            continue;
        }
        for (t = 0; t < COOPSYNC_NTYPES; t++) {
            if (!s_dlDirty[cn][t]) {
                continue;
            }
            if (now - s_dlLast[cn][t] < COOPSYNC_SEND_MS && now >= s_dlLast[cn][t]) {
                continue;
            }
            if (!g_entities[cn].inuse || !g_entities[cn].client) {
                s_dlDirty[cn][t] = 0;
                continue;
            }
            CoopSync_SendRecord(cn, t, s_dlPending[cn][t].c_str(), s_dlPending[cn][t].length());
            s_dlDirty[cn][t] = 0;
            s_dlLast[cn][t]  = now;
        }
    }
}
