/*
 * bsdsocket_lib.c — UAOS bsdsocket.library implementation
 *
 * Provides AmigaOS bsdsocket.library LVO stubs backed by the UAOS TCP/IP stack.
 * Called from uaos_m68k_glue.c m68k_illg_instr_callback when lib_id == LIB_BSDSOCKET.
 *
 * AmiTCP/IP bsdsocket.library register calling convention:
 *   socket(domain/D0, type/D1, protocol/D2)         -> fd/D0
 *   bind(fd/D0, sockaddr_ptr/A0, addrlen/D1)         -> 0 or -1/D0
 *   connect(fd/D0, sockaddr_ptr/A0, addrlen/D1)      -> 0 or -1/D0
 *   listen(fd/D0, backlog/D1)                        -> 0 or -1/D0
 *   accept(fd/D0, sockaddr_ptr/A0, addrlen_ptr/A1)   -> fd/D0
 *   send(fd/D0, buf_ptr/A0, len/D1, flags/D2)        -> bytes/D0
 *   recv(fd/D0, buf_ptr/A0, len/D1, flags/D2)        -> bytes/D0
 *   sendto(fd/D0, buf_ptr/A0, len/D1, flags/D2, addr_ptr/A1, addrlen/D3) -> bytes/D0
 *   recvfrom(fd/D0, buf_ptr/A0, len/D1, flags/D2, addr_ptr/A1, addrlen_ptr/A2) -> bytes/D0
 *   CloseSocket(fd/D0)                               -> 0/D0
 *   gethostbyname(name_ptr/A0)                       -> hostent_ptr/D0
 *   inet_addr(str_ptr/A0)                            -> ip/D0
 *   inet_ntoa(addr/D0)                               -> str_ptr/D0  (static buffer)
 *   setsockopt(fd/D0, level/D1, optname/D2, val_ptr/A0, optlen/D3) -> 0/D0
 *   IoctlSocket(fd/D0, req/D1, arg/A0)               -> 0/D0
 */

#include "bsdsocket_lib.h"
#include "rom_modules.h"
#include "../net/stack.h"
#include "../net/tcp.h"
#include "../net/udp.h"
#include "../net/dns.h"
#include "../net/net.h"
#include <stdint.h>

/* -------------------------------------------------------------------------
 * Socket descriptor table
 * Maps AmigaOS fd (0-based) to either a TCP or UDP socket index.
 * fd 0-7: TCP sockets
 * fd 8-15: UDP sockets (mapped to udp index fd-8)
 * ------------------------------------------------------------------------- */
#define BSD_MAX_FD      16
#define BSD_FD_TCP_BASE 0
#define BSD_FD_UDP_BASE 8

typedef enum { BSD_UNUSED=0, BSD_TCP, BSD_UDP } BsdSockType;

typedef struct {
    BsdSockType type;
    int         sock_idx;   /* index into g_tcp or g_udp tables */
    uint8_t     non_blocking;
} BsdFd;

static BsdFd g_fds[BSD_MAX_FD];

static int alloc_fd(BsdSockType type, int idx)
{
    for (int i = 0; i < BSD_MAX_FD; i++) {
        if (g_fds[i].type == BSD_UNUSED) {
            g_fds[i].type      = type;
            g_fds[i].sock_idx  = idx;
            g_fds[i].non_blocking = 0;
            return i;
        }
    }
    return -1;
}

/* -------------------------------------------------------------------------
 * Fake sockaddr_in in guest RAM (used for bind/connect/accept)
 * struct sockaddr_in { uint16_t sin_family; uint16_t sin_port; uint32_t sin_addr; ... }
 * ------------------------------------------------------------------------- */
#define SOCKADDR_FAMILY_OFF  0
#define SOCKADDR_PORT_OFF    2
#define SOCKADDR_ADDR_OFF    4

/* Guest RAM accessor — provided by the glue layer (uaos_m68k_glue.c) */
extern uint8_t g_ram[];
#define GUEST_RAM_SIZE  (2 * 1024 * 1024)

static uint16_t ram_r16(uint32_t addr)
{
    return (uint16_t)((g_ram[addr] << 8) | g_ram[addr+1]);
}
static uint32_t ram_r32(uint32_t addr)
{
    return ((uint32_t)g_ram[addr]<<24)|((uint32_t)g_ram[addr+1]<<16)
          |((uint32_t)g_ram[addr+2]<<8)|(uint32_t)g_ram[addr+3];
}
static void ram_w16(uint32_t addr, uint16_t v)
{
    g_ram[addr]   = (uint8_t)(v>>8);
    g_ram[addr+1] = (uint8_t)(v);
}
static void ram_w32(uint32_t addr, uint32_t v)
{
    g_ram[addr]   = (uint8_t)(v>>24);
    g_ram[addr+1] = (uint8_t)(v>>16);
    g_ram[addr+2] = (uint8_t)(v>>8);
    g_ram[addr+3] = (uint8_t)(v);
}

/* Static buffer for inet_ntoa return */
static char g_ntoa_buf[20];
static uint32_t g_ntoa_guest_addr = 0;  /* set by BsdSocket_Init */

/* -------------------------------------------------------------------------
 * LVO function implementations
 * m68k register access is via extern functions provided by the glue
 * ------------------------------------------------------------------------- */
extern unsigned int m68k_get_reg(void *context, int reg);
extern void         m68k_set_reg(int reg, unsigned int value);

/* M68k register indices (from m68k.h) */
#define M68K_REG_D0  0
#define M68K_REG_D1  1
#define M68K_REG_D2  2
#define M68K_REG_D3  3
#define M68K_REG_A0  8
#define M68K_REG_A1  9
#define M68K_REG_A2  10

static void bsd_socket(void)
{
    int domain   = (int)m68k_get_reg(NULL, M68K_REG_D0);
    int type     = (int)m68k_get_reg(NULL, M68K_REG_D1);
    int protocol = (int)m68k_get_reg(NULL, M68K_REG_D2);
    (void)domain; (void)protocol;

    int fd = -1;
    if (type == SOCK_STREAM) {
        /* TCP — allocate a closed socket slot */
        for (int i = 0; i < BSD_MAX_FD; i++) {
            if (g_fds[i].type == BSD_UNUSED) {
                g_fds[i].type     = BSD_TCP;
                g_fds[i].sock_idx = -1;   /* not yet connected */
                fd = i;
                break;
            }
        }
    } else if (type == SOCK_DGRAM) {
        int idx = udp_open(0);
        if (idx >= 0) fd = alloc_fd(BSD_UDP, idx);
    }
    m68k_set_reg(M68K_REG_D0, (unsigned int)fd);
}

static void bsd_bind(void)
{
    int fd        = (int)m68k_get_reg(NULL, M68K_REG_D0);
    uint32_t sa   = m68k_get_reg(NULL, M68K_REG_A0);
    (void)m68k_get_reg(NULL, M68K_REG_D1);  /* addrlen */

    if (fd < 0 || fd >= BSD_MAX_FD || g_fds[fd].type == BSD_UNUSED) {
        m68k_set_reg(M68K_REG_D0, (unsigned int)-1); return;
    }
    uint16_t port = net_ntohs(ram_r16(sa + SOCKADDR_PORT_OFF));

    if (g_fds[fd].type == BSD_UDP) {
        udp_close(g_fds[fd].sock_idx);
        int idx = udp_open(port);
        g_fds[fd].sock_idx = idx;
    }
    /* For TCP bind just store port for use in listen/connect */
    m68k_set_reg(M68K_REG_D0, 0);
}

static void bsd_connect(void)
{
    int fd      = (int)m68k_get_reg(NULL, M68K_REG_D0);
    uint32_t sa = m68k_get_reg(NULL, M68K_REG_A0);

    if (fd < 0 || fd >= BSD_MAX_FD || g_fds[fd].type != BSD_TCP) {
        m68k_set_reg(M68K_REG_D0, (unsigned int)-1); return;
    }
    uint16_t port   = net_ntohs(ram_r16(sa + SOCKADDR_PORT_OFF));
    uint32_t ip_ne  = ram_r32(sa + SOCKADDR_ADDR_OFF);
    ipv4_t   dst_ip = net_ntohl(ip_ne);

    int idx = tcp_connect(dst_ip, port, 0);
    g_fds[fd].sock_idx = idx;
    m68k_set_reg(M68K_REG_D0, idx >= 0 ? 0 : (unsigned int)-1);
}

static void bsd_listen(void)
{
    int fd      = (int)m68k_get_reg(NULL, M68K_REG_D0);
    int backlog = (int)m68k_get_reg(NULL, M68K_REG_D1);
    (void)backlog;

    if (fd < 0 || fd >= BSD_MAX_FD || g_fds[fd].type != BSD_TCP) {
        m68k_set_reg(M68K_REG_D0, (unsigned int)-1); return;
    }
    /* Need a port — use the one set by bind or default */
    int idx = tcp_listen(g_fds[fd].sock_idx >= 0 ?
                         (uint16_t)g_fds[fd].sock_idx : 0);
    g_fds[fd].sock_idx = idx;
    m68k_set_reg(M68K_REG_D0, idx >= 0 ? 0 : (unsigned int)-1);
}

static void bsd_accept(void)
{
    int fd      = (int)m68k_get_reg(NULL, M68K_REG_D0);
    if (fd < 0 || fd >= BSD_MAX_FD || g_fds[fd].type != BSD_TCP) {
        m68k_set_reg(M68K_REG_D0, (unsigned int)-1); return;
    }
    int new_idx = tcp_accept(g_fds[fd].sock_idx);
    if (new_idx < 0) {
        m68k_set_reg(M68K_REG_D0, (unsigned int)-1); return;
    }
    int new_fd = alloc_fd(BSD_TCP, new_idx);
    m68k_set_reg(M68K_REG_D0, (unsigned int)new_fd);
}

static void bsd_send(void)
{
    int fd        = (int)m68k_get_reg(NULL, M68K_REG_D0);
    uint32_t buf  = m68k_get_reg(NULL, M68K_REG_A0);
    int len       = (int)m68k_get_reg(NULL, M68K_REG_D1);

    if (fd < 0 || fd >= BSD_MAX_FD || g_fds[fd].type != BSD_TCP || len <= 0) {
        m68k_set_reg(M68K_REG_D0, (unsigned int)-1); return;
    }
    if (len > 1460) len = 1460;
    int n = tcp_send(g_fds[fd].sock_idx, g_ram + buf, (uint16_t)len);
    m68k_set_reg(M68K_REG_D0, (unsigned int)n);
}

static void bsd_recv(void)
{
    int fd       = (int)m68k_get_reg(NULL, M68K_REG_D0);
    uint32_t buf = m68k_get_reg(NULL, M68K_REG_A0);
    int len      = (int)m68k_get_reg(NULL, M68K_REG_D1);

    if (fd < 0 || fd >= BSD_MAX_FD || g_fds[fd].type != BSD_TCP || len <= 0) {
        m68k_set_reg(M68K_REG_D0, (unsigned int)-1); return;
    }
    /* Poll network before recv to process any incoming packets */
    net_stack_poll();
    int n = tcp_recv(g_fds[fd].sock_idx, g_ram + buf, (uint16_t)len);
    m68k_set_reg(M68K_REG_D0, (unsigned int)n);
}

static void bsd_sendto(void)
{
    int fd        = (int)m68k_get_reg(NULL, M68K_REG_D0);
    uint32_t buf  = m68k_get_reg(NULL, M68K_REG_A0);
    int len       = (int)m68k_get_reg(NULL, M68K_REG_D1);
    uint32_t sa   = m68k_get_reg(NULL, M68K_REG_A1);

    if (fd < 0 || fd >= BSD_MAX_FD || g_fds[fd].type != BSD_UDP || len <= 0) {
        m68k_set_reg(M68K_REG_D0, (unsigned int)-1); return;
    }
    uint16_t port  = net_ntohs(ram_r16(sa + SOCKADDR_PORT_OFF));
    uint32_t ip_ne = ram_r32(sa + SOCKADDR_ADDR_OFF);
    ipv4_t   dst   = net_ntohl(ip_ne);
    int n = udp_send(g_fds[fd].sock_idx, dst, port, g_ram + buf, (uint16_t)len);
    m68k_set_reg(M68K_REG_D0, n ? (unsigned int)len : (unsigned int)-1);
}

static void bsd_recvfrom(void)
{
    int fd       = (int)m68k_get_reg(NULL, M68K_REG_D0);
    uint32_t buf = m68k_get_reg(NULL, M68K_REG_A0);
    int len      = (int)m68k_get_reg(NULL, M68K_REG_D1);
    uint32_t sa  = m68k_get_reg(NULL, M68K_REG_A1);

    if (fd < 0 || fd >= BSD_MAX_FD || g_fds[fd].type != BSD_UDP || len <= 0) {
        m68k_set_reg(M68K_REG_D0, (unsigned int)-1); return;
    }
    net_stack_poll();
    ipv4_t   src_ip   = 0;
    uint16_t src_port = 0;
    int n = udp_recv(g_fds[fd].sock_idx, g_ram + buf, (uint16_t)len,
                     &src_ip, &src_port);
    if (n > 0 && sa) {
        ram_w16(sa + SOCKADDR_FAMILY_OFF, AF_INET);
        ram_w16(sa + SOCKADDR_PORT_OFF,   net_htons(src_port));
        ram_w32(sa + SOCKADDR_ADDR_OFF,   net_htonl(src_ip));
    }
    m68k_set_reg(M68K_REG_D0, (unsigned int)n);
}

static void bsd_closesocket(void)
{
    int fd = (int)m68k_get_reg(NULL, M68K_REG_D0);
    if (fd >= 0 && fd < BSD_MAX_FD) {
        if (g_fds[fd].type == BSD_TCP) tcp_close(g_fds[fd].sock_idx);
        else if (g_fds[fd].type == BSD_UDP) udp_close(g_fds[fd].sock_idx);
        g_fds[fd].type = BSD_UNUSED;
    }
    m68k_set_reg(M68K_REG_D0, 0);
}

static void bsd_gethostbyname(void)
{
    uint32_t name_ptr = m68k_get_reg(NULL, M68K_REG_A0);
    char name[256]; int i = 0;
    while (i < 255 && g_ram[name_ptr+i]) { name[i]=g_ram[name_ptr+i]; i++; }
    name[i] = '\0';

    /* Resolve via DNS (handles dotted-decimal as fast path) */
    ipv4_t ip = 0;
    if (!dns_resolve(name, &ip, 5000, NULL, NULL)) {
        m68k_set_reg(M68K_REG_D0, 0); /* NULL = not found */
        return;
    }
    /* Write a minimal hostent into g_ntoa_buf area (re-use) */
    /* Return just the IP in D0 as a 32-bit value — programs usually dereference
     * hostent->h_addr, so store a tiny fake struct at g_ntoa_guest_addr:
     *   +0  char *h_name   (4 bytes, points to name in RAM)
     *   +4  char **h_aliases (4 bytes, 0)
     *   +8  short h_addrtype = AF_INET (2 bytes)
     *  +10  short h_length  = 4 (2 bytes)
     *  +12  char **h_addr_list (4 bytes, points to +20)
     *  +16  padding (4 bytes, 0)
     *  +20  uint32_t addr   (the IP address, network byte order)
     *  +24  NULL terminator for h_addr_list
     */
    uint32_t base = g_ntoa_guest_addr;
    if (!base) { m68k_set_reg(M68K_REG_D0, 0); return; }
    uint32_t addr_slot = base + 20;
    uint32_t list_slot = base + 12;
    /* Clear */
    for (int j = 0; j < 32; j++) g_ram[base+j] = 0;
    /* h_addrtype = AF_INET */
    ram_w16(base + 8,  AF_INET);
    /* h_length = 4 */
    ram_w16(base + 10, 4);
    /* h_addr_list pointer -> addr_slot */
    ram_w32(list_slot, addr_slot);
    /* addr */
    ram_w32(addr_slot, net_htonl(ip));
    /* NULL terminator after addr_slot pointer */
    ram_w32(addr_slot + 4, 0);

    m68k_set_reg(M68K_REG_D0, base);
}

static void bsd_inet_addr(void)
{
    uint32_t ptr = m68k_get_reg(NULL, M68K_REG_A0);
    char s[20]; int i = 0;
    while (i < 19 && g_ram[ptr+i]) { s[i]=g_ram[ptr+i]; i++; }
    s[i] = '\0';
    ipv4_t ip = 0;
    net_str_to_ip(s, &ip);
    m68k_set_reg(M68K_REG_D0, net_htonl(ip));
}

static void bsd_inet_ntoa(void)
{
    uint32_t ip_ne = m68k_get_reg(NULL, M68K_REG_D0);
    ipv4_t ip = net_ntohl(ip_ne);
    net_ip_to_str(ip, g_ntoa_buf);
    /* Copy to guest RAM */
    if (g_ntoa_guest_addr) {
        int j = 0;
        while (g_ntoa_buf[j]) { g_ram[g_ntoa_guest_addr + j] = g_ntoa_buf[j]; j++; }
        g_ram[g_ntoa_guest_addr + j] = 0;
        m68k_set_reg(M68K_REG_D0, g_ntoa_guest_addr);
    } else {
        m68k_set_reg(M68K_REG_D0, 0);
    }
}

static void bsd_setsockopt(void) { m68k_set_reg(M68K_REG_D0, 0); }
static void bsd_getsockopt(void) { m68k_set_reg(M68K_REG_D0, 0); }
static void bsd_ioctlsocket(void){ m68k_set_reg(M68K_REG_D0, 0); }

/* -------------------------------------------------------------------------
 * Function index table (matches bsdsocket.library LVO order)
 * LVOs: socket=-30, bind=-36, listen=-42, accept=-48, connect=-54,
 *       send=-60, sendto=-66, recv=-72, recvfrom=-78, closesocket=-84,
 *       setsockopt=-96, getsockopt=-102, ioctlsocket=-108,
 *       inet_addr=-132, inet_ntoa=-138, gethostbyname=-210
 * ------------------------------------------------------------------------- */
#define BSD_FN_SOCKET        1
#define BSD_FN_BIND          2
#define BSD_FN_LISTEN        3
#define BSD_FN_ACCEPT        4
#define BSD_FN_CONNECT       5
#define BSD_FN_SEND          6
#define BSD_FN_SENDTO        7
#define BSD_FN_RECV          8
#define BSD_FN_RECVFROM      9
#define BSD_FN_CLOSESOCKET   10
#define BSD_FN_SETSOCKOPT    11
#define BSD_FN_GETSOCKOPT    12
#define BSD_FN_IOCTLSOCKET   13
#define BSD_FN_INET_ADDR     14
#define BSD_FN_INET_NTOA     15
#define BSD_FN_GETHOSTBYNAME 16

void BsdSocket_Dispatch(uint32_t fn, uint32_t *regs)
{
    (void)regs;
    switch (fn) {
    case BSD_FN_SOCKET:        bsd_socket();        break;
    case BSD_FN_BIND:          bsd_bind();          break;
    case BSD_FN_LISTEN:        bsd_listen();        break;
    case BSD_FN_ACCEPT:        bsd_accept();        break;
    case BSD_FN_CONNECT:       bsd_connect();       break;
    case BSD_FN_SEND:          bsd_send();          break;
    case BSD_FN_SENDTO:        bsd_sendto();        break;
    case BSD_FN_RECV:          bsd_recv();          break;
    case BSD_FN_RECVFROM:      bsd_recvfrom();      break;
    case BSD_FN_CLOSESOCKET:   bsd_closesocket();   break;
    case BSD_FN_SETSOCKOPT:    bsd_setsockopt();    break;
    case BSD_FN_GETSOCKOPT:    bsd_getsockopt();    break;
    case BSD_FN_IOCTLSOCKET:   bsd_ioctlsocket();  break;
    case BSD_FN_INET_ADDR:     bsd_inet_addr();     break;
    case BSD_FN_INET_NTOA:     bsd_inet_ntoa();     break;
    case BSD_FN_GETHOSTBYNAME: bsd_gethostbyname(); break;
    default:
        m68k_set_reg(M68K_REG_D0, 0);
        break;
    }
}

void BsdSocket_Init(void)
{
    /* Zero all socket descriptors */
    for (int i = 0; i < BSD_MAX_FD; i++) g_fds[i].type = BSD_UNUSED;
    /* Reserve 32 bytes in guest RAM near 0x580 for inet_ntoa/gethostbyname */
    g_ntoa_guest_addr = 0x0580;
}

/* -------------------------------------------------------------------------
 * ROM module registration — makes bsdsocket.library appear in 'libs'
 * Native function table mirrors the BSD_FN_* indices (1-based).
 * The rom_modules registry is used only for display/discovery; the actual
 * dispatch still goes through BsdSocket_Dispatch() via the ILLEGAL trap.
 * ------------------------------------------------------------------------- */

static void bsd_stub_socket(void)        { bsd_socket();        }
static void bsd_stub_bind(void)          { bsd_bind();          }
static void bsd_stub_listen(void)        { bsd_listen();        }
static void bsd_stub_accept(void)        { bsd_accept();        }
static void bsd_stub_connect(void)       { bsd_connect();       }
static void bsd_stub_send(void)          { bsd_send();          }
static void bsd_stub_sendto(void)        { bsd_sendto();        }
static void bsd_stub_recv(void)          { bsd_recv();          }
static void bsd_stub_recvfrom(void)      { bsd_recvfrom();      }
static void bsd_stub_closesocket(void)   { bsd_closesocket();   }
static void bsd_stub_setsockopt(void)    { bsd_setsockopt();    }
static void bsd_stub_getsockopt(void)    { bsd_getsockopt();    }
static void bsd_stub_ioctlsocket(void)   { bsd_ioctlsocket();  }
static void bsd_stub_inet_addr(void)     { bsd_inet_addr();     }
static void bsd_stub_inet_ntoa(void)     { bsd_inet_ntoa();     }
static void bsd_stub_gethostbyname(void) { bsd_gethostbyname(); }

static void *bsd_funcs[] = {
    bsd_stub_socket,        /* index 1  — socket()        */
    bsd_stub_bind,          /* index 2  — bind()          */
    bsd_stub_listen,        /* index 3  — listen()        */
    bsd_stub_accept,        /* index 4  — accept()        */
    bsd_stub_connect,       /* index 5  — connect()       */
    bsd_stub_send,          /* index 6  — send()          */
    bsd_stub_sendto,        /* index 7  — sendto()        */
    bsd_stub_recv,          /* index 8  — recv()          */
    bsd_stub_recvfrom,      /* index 9  — recvfrom()      */
    bsd_stub_closesocket,   /* index 10 — CloseSocket()   */
    bsd_stub_setsockopt,    /* index 11 — setsockopt()    */
    bsd_stub_getsockopt,    /* index 12 — getsockopt()    */
    bsd_stub_ioctlsocket,   /* index 13 — IoctlSocket()   */
    bsd_stub_inet_addr,     /* index 14 — inet_addr()     */
    bsd_stub_inet_ntoa,     /* index 15 — inet_ntoa()     */
    bsd_stub_gethostbyname, /* index 16 — gethostbyname() */
};

void UAOS_BSDSOCKET_Register(void)
{
    /* BSD_BASE = 0x3000 (guest RAM address of bsdsocket.library base) */
    UAOS_ROM_Register("bsdsocket.library", 4, 0x00003000,
                      (uint16_t)(sizeof(bsd_funcs) / sizeof(bsd_funcs[0])),
                      bsd_funcs);
}
