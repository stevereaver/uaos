/*
 * bsdsocket_lib.h — UAOS bsdsocket.library (AmigaOS BSD socket API)
 *
 * Provides the subset of bsdsocket.library LVOs used by AmigaOS networking
 * programs, mapped onto the UAOS native TCP/IP stack.
 *
 * LVO offsets (from AmiTCP/IP SDK, negative from library base):
 *   socket          -30
 *   bind            -36
 *   listen          -42
 *   accept          -48
 *   connect         -54
 *   send            -60
 *   sendto          -66
 *   recv            -72
 *   recvfrom        -78
 *   closesocket     -84
 *   gethostbyname   -210
 *   inet_addr       -132
 *   inet_ntoa       -138
 *   setsockopt      -96
 *   getsockopt      -102
 *   IoctlSocket     -108
 *   CloseSocket     -84  (alias)
 */

#ifndef UAOS_BSDSOCKET_LIB_H
#define UAOS_BSDSOCKET_LIB_H

#include <stdint.h>

/* Initialise bsdsocket.library — registers LVO stubs in Musashi M68k space */
void BsdSocket_Init(void);

/* Called from M68k ILLEGAL dispatch when bsdsocket LVO fires */
void BsdSocket_Dispatch(uint32_t lvo_offset, uint32_t *m68k_regs);

/* AF/SOCK/IPPROTO constants (same as POSIX) */
#define AF_INET     2
#define SOCK_STREAM 1
#define SOCK_DGRAM  2
#define IPPROTO_TCP 6
#define IPPROTO_UDP 17

#endif /* UAOS_BSDSOCKET_LIB_H */
