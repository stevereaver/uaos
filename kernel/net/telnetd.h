/* kernel/net/telnetd.h — Telnet shell daemon
 *
 * Unauthenticated remote shell service: each accepted TCP connection is
 * dropped straight into a UAOS shell session (no login).  Intended as a
 * debugging interface — anyone who can reach the port gets a shell.
 */
#ifndef UAOS_TELNETD_H
#define UAOS_TELNETD_H

#include <stdint.h>

#define TELNETD_DEFAULT_PORT 23
#define TELNETD_PORT_MAX     4095   /* a bit below TCP_BASE_PORT range cap */

/* Start the telnet daemon listening on the given TCP port.
 * Returns 1 on success, 0 on failure (listener unavailable, net stack down,
 * or daemon already running).  The daemon runs as its own native task. */
int Telnetd_Start(uint16_t port);

/* Ask the daemon to stop: it closes the listener, drops any active
 * session, and exits its task.  Returns once the task has wound down.
 * No-op if the daemon is not running. */
void Telnetd_Stop(void);

/* Report whether the daemon task is currently running. */
int Telnetd_IsRunning(void);

#endif /* UAOS_TELNETD_H */
