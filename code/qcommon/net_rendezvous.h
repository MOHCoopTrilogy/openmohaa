/*
 * HZM coop [238] - NAT hole-punch rendezvous client (design: _research/nat_holepunch.md).
 * Host side registers a memorable code with the rendezvous daemon and punches toward
 * joiners on request; client side resolves a code to the host's public endpoint and
 * hands off to the normal connect flow. All I/O is standard OOB text on ip_socket.
 */

#ifndef NET_RENDEZVOUS_H
#define NET_RENDEZVOUS_H

#include "q_shared.h"
#include "qcommon.h"

#ifdef __cplusplus
extern "C" {
#endif

void RDV_Init(void);            /* register cvars + the coop_join command (client exe only) */
void RDV_ServerFrame(void);     /* host keepalive/registration + pending punch bursts */
void RDV_ClientFrame(void);     /* join-request resends + timeout */

/* returns qtrue when the message was consumed */
qboolean RDV_HandleServerOOB(netadr_t from, const char *cmd);
qboolean RDV_HandleClientOOB(netadr_t from, const char *cmd);

#ifdef __cplusplus
}
#endif

#endif
