/* kernel/net/telnetd.c — Telnet shell daemon (RFC 854)
 *
 * Architecture
 * ------------
 *   telnetd task   listens on a TCP port; each accepted socket is handed
 *                  to a remote ShellInstance (shell_win.c remote session)
 *                  and the task pumps that socket until it dies.
 *   session task   (created by ShellWin_RemoteOpen inside the shell code)
 *                  consumes the shell key queue the daemon feeds.
 *
 * Only one session is served at a time: remote shell slots are scarce
 * (MAX_REMOTE_SHELLS) and the TCP layer allows 8 sockets.  Connections
 * that arrive while a session is live — or while no shell slot is free —
 * are answered with a busy banner and closed, never left silent.
 *
 * Telnet protocol
 * ---------------
 * On connect we send the classic negotiation:
 *     WILL ECHO, WILL SUPPRESS_GO_AHEAD, DO SUPPRESS_GO_AHEAD
 * then a small NVT state machine filters IAC sequences out of the input
 * stream, responding DONT/WONT to anything the peer demands of us and
 * collapsing an escaped IAC-IAC to a literal 0xFF byte.  CR NUL and bare
 * CR are mapped to '\n'; incoming '\n' and '\r' both become '\n' for the
 * shell.  Arrow-key escape sequences (ESC [ A/B/C/D) are mapped to the
 * virtual key codes the shell editor understands.
 */

#include "telnetd.h"
#include "tcp.h"
#include "stack.h"
#include "../exec/task.h"
#include "../display/shell_win.h"
#include "../boot/kprint.h"

/* Telnet protocol bytes (RFC 854) */
#define TN_SE    240
#define TN_NOP   241
#define TN_DM    242
#define TN_BRK   243
#define TN_IP    244
#define TN_AO    245
#define TN_AYT   246
#define TN_EC    247
#define TN_EL    248
#define TN_GA    249
#define TN_SB    250
#define TN_WILL  251
#define TN_WONT  252
#define TN_DO    253
#define TN_DONT  254
#define TN_IAC   255

#define TN_OPT_ECHO     1
#define TN_OPT_SGA      3   /* suppress go ahead */

/* NVT input parser states */
enum {
    NVT_DATA = 0,       /* normal data byte */
    NVT_IAC,            /* IAC seen — expect command byte */
    NVT_NEG,            /* IAC WILL/WONT/DO/DONT seen — expect option byte */
    NVT_SB,             /* inside sub-negotiation — discard until IAC SE */
    NVT_SB_IAC,         /* inside sub-negotiation — IAC seen */
    NVT_ESC,            /* ESC seen — expect CSI leader */
    NVT_CSI,            /* ESC [ seen — expect final byte */
};

static volatile int g_running = 0;   /* daemon task is alive */
static volatile int g_stop    = 0;   /* stop requested via Telnetd_Stop() */

static int send_buf(int sock, const uint8_t *b, int len)
{
    extern volatile uint64_t g_pit_ticks;   /* 100 Hz */
    /* tcp_send allows only one in-flight segment per socket and returns
     * 0 while busy — keep retrying so negotiation bytes are not dropped,
     * but bound the wait (~250 ms) so a dead peer cannot wedge us. */
    uint64_t deadline = g_pit_ticks + 25;
    for (;;) {
        if (tcp_send(sock, b, (uint16_t)len) > 0) return 1;
        TcpState t = tcp_state(sock);
        if (t != TCP_ESTABLISHED && t != TCP_CLOSE_WAIT) return 0;
        if (g_pit_ticks >= deadline) return 0;
        net_stack_poll();
    }
}

static void send_neg(int sock, uint8_t cmd, uint8_t opt)
{
    uint8_t b[3] = { TN_IAC, cmd, opt };
    send_buf(sock, b, 3);
}

/* One-shot initial negotiation — ask the client to let us echo and to
 * suppress go-ahead (character-at-a-time mode).  All nine bytes go out
 * in a single segment: sent one WILL/DO at a time, the first send could
 * still be in flight when the next arrives and tcp_send would drop it —
 * a client that sees WILL ECHO but not WILL SGA switches off local echo
 * yet stays in line mode, so typed keys stay invisible until Enter. */
static void send_greeting_neg(int sock)
{
    static const uint8_t neg[] = {
        TN_IAC, TN_WILL, TN_OPT_ECHO,
        TN_IAC, TN_WILL, TN_OPT_SGA,
        TN_IAC, TN_DO,   TN_OPT_SGA
    };
    send_buf(sock, neg, (int)sizeof(neg));
}

/* Feed a data byte through the NVT filter into the shell.
 * Returns the translated byte to enqueue, or -1 to drop it. */
static int nvt_filter(void *sess, uint8_t *st, uint8_t c, int sock)
{
    switch (*st) {
    case NVT_IAC:
        switch (c) {
        case TN_WILL:
        case TN_WONT:
            /* Option peer wants to enable itself — accept WILL, but if it
             * insists on something we do not want, WONT is its answer to
             * our DO and needs no reply either. */
            *st = NVT_NEG;
            return -1;
        case TN_DO:
        case TN_DONT:
            *st = NVT_NEG;
            return -1;
        case TN_SB:
            *st = NVT_SB;
            return -1;
        case TN_IAC:
            *st = NVT_DATA;
            return 0xFF;    /* escaped IAC = literal data byte */
        case TN_IP:
        case TN_AO:
            *st = NVT_DATA;
            ShellWin_RemoteFeed(sess, '\b');   /* best-effort interrupt */
            return -1;
        default:
            *st = NVT_DATA;
            return -1;      /* NOP, DM, BRK, EC, EL, GA ... just drop */
        }
    case NVT_NEG:
        /* Byte is the option under negotiation; the command byte was
         * saved in st[1] by the caller.  Refuse DO for anything we did
         * not offer, and refuse WILL for anything we did not ask for. */
        *st = NVT_DATA;
        {
            uint8_t cmd = st[1];
            st[1] = 0;
            if (cmd == TN_DO && c != TN_OPT_ECHO && c != TN_OPT_SGA)
                send_neg(sock, TN_WONT, c);
            if (cmd == TN_WILL && c != TN_OPT_SGA)
                send_neg(sock, TN_DONT, c);
        }
        return -1;
    case NVT_SB:
        if (c == TN_IAC) *st = NVT_SB_IAC;
        return -1;
    case NVT_SB_IAC:
        *st = (c == TN_SE) ? NVT_DATA : NVT_SB;
        return -1;
    case NVT_ESC:
        *st = (c == '[' || c == 'O') ? NVT_CSI : NVT_DATA;
        return -1;
    case NVT_CSI:
        *st = NVT_DATA;
        switch (c) {
        case 'A': return SHELL_VKEY_UP;
        case 'B': return SHELL_VKEY_DOWN;
        case 'C': return SHELL_VKEY_RIGHT;
        case 'D': return SHELL_VKEY_LEFT;
        default:  return -1;
        }
    case NVT_DATA:
    default:
        if (c == TN_IAC) { *st = NVT_IAC; return -1; }
        if (c == 0x1B)   { *st = NVT_ESC; return -1; }
        if (c == '\r' || c == '\n') return '\n';
        if (c == 0x7F) return '\b';      /* DEL = backspace */
        if (c == 0) return -1;           /* CR NUL padding */
        return c;
    }
}

/* Session pump: bridge one accepted socket to a remote shell session.
 * Runs until the peer disconnects, the shell ENDCLIs, or the socket dies.
 * While a session is live the daemon never loops back to tcp_accept(), so
 * a second client would complete its handshake yet see nothing at all —
 * drain the listener here and turn extras away with a busy banner. */
static void pump_session(int lsock, int sock)
{
    void *sess = ShellWin_RemoteOpen(sock);
    if (!sess) {
        static const char busy[] =
            "\r\nUAOS: no remote shell slots free, try later\r\n";
        tcp_send(sock, (const uint8_t *)busy, (uint16_t)sizeof(busy) - 1);
        tcp_close(sock);
        return;
    }

    /* nvt state: [0] = state, [1] = saved neg command */
    uint8_t st[2] = { NVT_DATA, 0 };
    uint8_t buf[256];

    for (;;) {
        /* Daemon asked to stop, or the net stack was shut down (netstop).
         * net_stack_shutdown() leaves the socket table untouched, so the
         * socket state alone cannot tell us the stack is gone. */
        if (g_stop || !net_stack_is_up())
            break;

        /* Socket gone (peer closed / RST) */
        TcpState t = tcp_state(sock);
        if (t != TCP_ESTABLISHED && t != TCP_CLOSE_WAIT)
            break;

        /* One session at a time — refuse extra connections politely. */
        int extra = tcp_accept(lsock);
        if (extra >= 0) {
            static const char busy[] =
                "\r\nUAOS: another telnet session is active, try later\r\n";
            send_buf(extra, (const uint8_t *)busy, (int)sizeof(busy) - 1);
            tcp_close(extra);
        }

        int n = tcp_recv(sock, buf, sizeof(buf));
        /* Half-close: peer sent FIN and RX buffer is drained */
        if (n <= 0 && t == TCP_CLOSE_WAIT)
            break;
        for (int i = 0; i < n; i++) {
            uint8_t c = buf[i];
            /* Remember neg command byte for NVT_NEG */
            if (st[0] == NVT_IAC &&
                (c == TN_WILL || c == TN_WONT || c == TN_DO || c == TN_DONT))
                st[1] = c;
            int k = nvt_filter(sess, st, c, sock);
            if (k >= 0)
                ShellWin_RemoteFeed(sess, (char)k);
        }

        /* Shell side ended (ENDCLI) or slot released */
        if (ShellWin_RemoteIsDead(sess))
            break;

        net_stack_poll();
        Task_Yield();
    }

    /* Notify the shell side and drop the connection.  When the stack is
     * already down a FIN could never be answered and tcp_tick no longer
     * runs to retire the socket — abort instead so the slot is freed. */
    ShellWin_RemoteKill(sess);
    if (net_stack_is_up()) {
        static const char bye[] = "\r\n[session closed]\r\n";
        tcp_send(sock, (const uint8_t *)bye, (uint16_t)sizeof(bye) - 1);
        tcp_close(sock);
    } else {
        tcp_abort(sock);
    }
}

static void telnetd_task(void *arg)
{
    uint16_t port = (uint16_t)(uintptr_t)arg;
    int lsock = tcp_listen(port);
    if (lsock < 0) {
        kprint("telnetd: tcp_listen failed\n");
        g_running = 0;
        Task_Exit();
    }
    kprint("telnetd: listening on port ");
    kprintdec(port);
    kprint("\n");

    /* Run until STOP is requested or the stack goes down; the latter
     * lets a fresh telnetd start cleanly after the next netstart. */
    while (!g_stop && net_stack_is_up()) {
        int csock = tcp_accept(lsock);
        if (csock >= 0) {
            send_greeting_neg(csock);
            pump_session(lsock, csock);
        }
        net_stack_poll();
        Task_Yield();
    }
    tcp_close(lsock);
    g_running = 0;
    kprint("telnetd: stopped\n");
    Task_Exit();
}

int Telnetd_Start(uint16_t port)
{
    if (g_running) return 0;
    if (port == 0) port = TELNETD_DEFAULT_PORT;
    if (!net_stack_is_up()) return 0;
    /* Set the flags before spawning: if the new task runs first and
     * tcp_listen fails it clears g_running itself. */
    g_stop    = 0;
    g_running = 1;
    if (!Task_CreateNative("telnetd", -128, telnetd_task,
                           (void *)(uintptr_t)port)) {
        g_running = 0;
        return 0;
    }
    return 1;
}

void Telnetd_Stop(void)
{
    extern volatile uint64_t g_pit_ticks;   /* 100 Hz */
    if (!g_running) return;
    g_stop = 1;
    /* Wait for the daemon task to notice g_stop, close its sockets and
     * clear g_running.  Task_Yield() does not reschedule (a bare pause)
     * and Wait() has no timeout, so poll on the PIT tick with a ~1 s
     * deadline — the timer ISR preempts us and lets the daemon run.
     * hlt keeps the CPU idle between checks. */
    uint64_t deadline = g_pit_ticks + 100;
    while (g_running && g_pit_ticks < deadline)
        __asm__ volatile ("sti; hlt" ::: "memory");
}

int Telnetd_IsRunning(void)
{
    return g_running;
}
