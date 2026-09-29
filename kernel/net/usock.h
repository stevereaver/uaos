/*
 * usock.h — userspace socket layer for native x86-64 tasks
 *
 * Bridges the INT 0x80 syscall surface (kernel/exec/syscall_dispatch.c)
 * to the kernel TCP/IP stack.  Provides a small blocking socket API with
 * per-task ownership so sockets are reclaimed when a task exits.
 *
 * The exported functions are called only from syscall handlers, i.e.
 * always in the context of the calling task, so they may poll the stack
 * and sleep via Task_SleepTicks().
 */
#ifndef UAOS_USOCK_H
#define UAOS_USOCK_H

#include <stdint.h>

/* Socket types (mirrored in system/libuaos/uaos_socket.h) */
#define USOCK_STREAM   1    /* TCP */

/* Options for usock_setopt (values are milliseconds; 0 = wait forever) */
#define USOCK_OPT_CONNECT_TIMEOUT   1
#define USOCK_OPT_RECV_TIMEOUT      2
#define USOCK_OPT_SEND_TIMEOUT      3

/* Return codes (negative errno-style; mirrored in uaos_socket.h) */
#define USOCK_OK         0
#define USOCK_EBADF     -1   /* bad/foreign/closed socket handle */
#define USOCK_ETIMEDOUT -2   /* operation deadline expired */
#define USOCK_ECONN     -3   /* refused / reset / not connected */
#define USOCK_ENETDOWN  -4   /* network stack is not running */
#define USOCK_EINVAL    -5   /* bad args / unsupported type */
#define USOCK_EMFILE    -6   /* no free socket slots */

int  usock_socket(int type);
int  usock_connect(int us, uint32_t ip, uint16_t port);
int  usock_send(int us, const void *buf, int len);
int  usock_recv(int us, void *buf, int len);
int  usock_close(int us);
int  usock_state(int us);
int  usock_setopt(int us, int opt, uint32_t val);
int  usock_resolve(const char *hostname, uint32_t *ip_out, uint32_t timeout_ms);

/* Close all sockets owned by an exiting task (called from Task_Exit). */
void usock_cleanup_task(void *task);

#endif /* UAOS_USOCK_H */
