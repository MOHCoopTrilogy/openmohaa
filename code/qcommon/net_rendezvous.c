/*
 * HZM coop [238] - NAT hole-punch rendezvous client. See net_rendezvous.h and
 * _research/nat_holepunch.md. Message set (OOB text lines):
 *
 *   host  -> daemon : hzm_rdv_reg <code> <protover>        every RDV_KEEPALIVE sec
 *   daemon-> host   : hzm_rdv_regok <pubip> <pubport> <nonce>
 *   client-> daemon : hzm_rdv_join <code>
 *   daemon-> client : hzm_rdv_peer <hostip> <hostport>
 *   daemon-> host   : hzm_rdv_punchreq <clientip> <clientport> <nonce>
 *   host  -> client : hzm_rdv_punch <nonce>                x RDV_PUNCH_COUNT, opens the mapping
 *
 * Cvars: net_rdv (host: 1 = register + accept punches), net_rdv_code (memorable host-chosen
 * word), net_rdv_host (daemon ip[:port], default the project instance). Client command:
 * coop_join <code>. Everything degrades silently to the normal direct-connect paths.
 */

#include "net_rendezvous.h"

#define RDV_DEFAULT_HOST  ""            /* set per release once the project daemon is up */
#define RDV_DEFAULT_PORT  12301
#define RDV_KEEPALIVE     20000         /* ms between host registrations */
#define RDV_PUNCH_COUNT   5
#define RDV_PUNCH_GAP     250           /* ms between punch packets */
#define RDV_JOIN_RESEND   1000          /* ms between join requests */
#define RDV_JOIN_TRIES    8

static cvar_t *net_rdv;
static cvar_t *net_rdv_code;
static cvar_t *net_rdv_host;

/* ---- host state ---- */
static netadr_t     rdv_daemonAdr;
static qboolean     rdv_daemonResolved;
static char         rdv_daemonStr[128];
static int          rdv_nextRegTime;
static char         rdv_nonce[32];
static qboolean     rdv_announced;

/* one pending punch burst (a second joiner mid-burst just restarts it) */
static netadr_t     rdv_punchTarget;
static int          rdv_punchLeft;
static int          rdv_punchNextTime;

/* ---- client join state ---- */
static qboolean     rdv_joinActive;
static char         rdv_joinCode[64];
static int          rdv_joinNextSend;
static int          rdv_joinTriesLeft;

static qboolean RDV_ResolveDaemon(void)
{
    const char *want = (net_rdv_host && net_rdv_host->string[0]) ? net_rdv_host->string : RDV_DEFAULT_HOST;

    if (!want[0]) {
        return qfalse;
    }
    if (rdv_daemonResolved && !Q_stricmp(rdv_daemonStr, want)) {
        return qtrue;
    }
    if (!NET_StringToAdr(want, &rdv_daemonAdr, NA_IP)) {
        Com_Printf("rendezvous: cannot resolve '%s'\n", want);
        rdv_daemonResolved = qfalse;
        return qfalse;
    }
    if (rdv_daemonAdr.port == 0) {
        rdv_daemonAdr.port = BigShort(RDV_DEFAULT_PORT);
    }
    Q_strncpyz(rdv_daemonStr, want, sizeof(rdv_daemonStr));
    rdv_daemonResolved = qtrue;
    return qtrue;
}

/* =========================== HOST SIDE =========================== */

void RDV_ServerFrame(void)
{
    int now = Sys_Milliseconds();

    if (!net_rdv || !net_rdv->integer) {
        rdv_announced = qfalse;
        return;
    }
    if (!com_sv_running || !com_sv_running->integer) {
        return;
    }
    if (!net_rdv_code->string[0]) {
        return;
    }
    if (!RDV_ResolveDaemon()) {
        return;
    }

    if (now >= rdv_nextRegTime) {
        rdv_nextRegTime = now + RDV_KEEPALIVE;
        NET_OutOfBandPrint(NS_SERVER, rdv_daemonAdr, "hzm_rdv_reg %s %d", net_rdv_code->string, com_protocol->integer);
    }

    /* pending punch burst toward a joiner */
    if (rdv_punchLeft > 0 && now >= rdv_punchNextTime) {
        rdv_punchNextTime = now + RDV_PUNCH_GAP;
        rdv_punchLeft--;
        NET_OutOfBandPrint(NS_SERVER, rdv_punchTarget, "hzm_rdv_punch %s", rdv_nonce);
    }
}

qboolean RDV_HandleServerOOB(netadr_t from, const char *cmd)
{
    if (!Q_stricmp(cmd, "hzm_rdv_regok")) {
        Q_strncpyz(rdv_nonce, Cmd_Argv(3), sizeof(rdv_nonce));
        if (!rdv_announced) {
            rdv_announced = qtrue;
            Com_Printf("^~^~^ rendezvous: ONLINE - join code '%s' (public %s:%s)\n",
                       net_rdv_code ? net_rdv_code->string : "?", Cmd_Argv(1), Cmd_Argv(2));
        }
        return qtrue;
    }

    if (!Q_stricmp(cmd, "hzm_rdv_punchreq")) {
        /* only honor requests carrying our registration nonce (anti-spray) */
        if (!rdv_nonce[0] || Q_stricmp(Cmd_Argv(3), rdv_nonce)) {
            return qtrue;
        }
        if (!NET_StringToAdr(va("%s:%s", Cmd_Argv(1), Cmd_Argv(2)), &rdv_punchTarget, NA_IP)) {
            return qtrue;
        }
        rdv_punchLeft     = RDV_PUNCH_COUNT;
        rdv_punchNextTime = 0;
        Com_Printf("rendezvous: punching toward %s\n", NET_AdrToString(rdv_punchTarget));
        return qtrue;
    }

    if (!Q_stricmp(cmd, "hzm_rdv_err")) {
        Com_Printf("rendezvous: daemon error '%s'\n", Cmd_Argv(1));
        return qtrue;
    }

    return qfalse;
}

/* =========================== CLIENT SIDE =========================== */

#ifndef DEDICATED
static void RDV_Join_f(void)
{
    if (Cmd_Argc() != 2) {
        Com_Printf("usage: coop_join <code>\n");
        return;
    }
    if (!RDV_ResolveDaemon()) {
        Com_Printf("coop_join: no rendezvous server configured (net_rdv_host)\n");
        return;
    }
    Q_strncpyz(rdv_joinCode, Cmd_Argv(1), sizeof(rdv_joinCode));
    rdv_joinActive    = qtrue;
    rdv_joinTriesLeft = RDV_JOIN_TRIES;
    rdv_joinNextSend  = 0;
    Com_Printf("coop_join: looking up '%s'...\n", rdv_joinCode);
}
#endif

void RDV_ClientFrame(void)
{
#ifndef DEDICATED
    int now;

    if (!rdv_joinActive) {
        return;
    }
    now = Sys_Milliseconds();
    if (now < rdv_joinNextSend) {
        return;
    }
    if (rdv_joinTriesLeft <= 0) {
        rdv_joinActive = qfalse;
        Com_Printf("coop_join: no answer for '%s' - is the host online? (fallback: connect <ip>)\n", rdv_joinCode);
        return;
    }
    rdv_joinTriesLeft--;
    rdv_joinNextSend = now + RDV_JOIN_RESEND;
    NET_OutOfBandPrint(NS_CLIENT, rdv_daemonAdr, "hzm_rdv_join %s", rdv_joinCode);
#endif
}

qboolean RDV_HandleClientOOB(netadr_t from, const char *cmd)
{
#ifndef DEDICATED
    if (!Q_stricmp(cmd, "hzm_rdv_peer")) {
        if (!rdv_joinActive) {
            return qtrue;
        }
        rdv_joinActive = qfalse;
        Com_Printf("coop_join: host found at %s:%s - connecting\n", Cmd_Argv(1), Cmd_Argv(2));
        /* the normal connect flow IS the client-side punch: getchallenge retries open our
         * mapping while the host's punch burst opens theirs */
        Cbuf_AddText(va("connect %s:%s\n", Cmd_Argv(1), Cmd_Argv(2)));
        return qtrue;
    }

    if (!Q_stricmp(cmd, "hzm_rdv_punch")) {
        /* hole-opener from the host - nothing to do, receiving it did the work */
        return qtrue;
    }

    if (!Q_stricmp(cmd, "hzm_rdv_err")) {
        if (rdv_joinActive) {
            rdv_joinActive = qfalse;
            Com_Printf("coop_join: %s\n", Cmd_Argv(1));
        }
        return qtrue;
    }
#endif
    return qfalse;
}

/* =========================== SHARED =========================== */

void RDV_Init(void)
{
    net_rdv      = Cvar_Get("net_rdv", "0", CVAR_ARCHIVE);
    net_rdv_code = Cvar_Get("net_rdv_code", "", CVAR_ARCHIVE);
    net_rdv_host = Cvar_Get("net_rdv_host", RDV_DEFAULT_HOST, CVAR_ARCHIVE);

#ifndef DEDICATED
    Cmd_AddCommand("coop_join", RDV_Join_f);
#endif
}
