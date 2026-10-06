/* kernel/net/telnetd.c — Telnet shell daemon (RFC 854)
 *
 * Architecture
 * ------------
 *   telnetd task    listens on a TCP port; each accepted socket is handed
 *                   to a remote ShellInstance (shell_win.c remote session)
 *                   and then to its own pump task, so up to
 *                   MAX_REMOTE_SHELLS sessions run concurrently.  When no
 *                   remote slot is free the connection is answered with a
 *                   busy banner and closed, never left silent.
 *   pump task       (telnetd-session, one per connection) bridges the
 *                   socket to the session: NVT-filters RX bytes into the
 *                   shell key queue until the peer or the shell goes away.
 *   session task    (created by ShellWin_RemoteOpen inside the shell code)
 *                   consumes the shell key queue the pump feeds.
 *
 * Telnet protocol
 * ---------------
 * On connect we send the classic negotiation:
 *     WILL ECHO, WILL SUPPRESS_GO_AHEAD, DO SUPPRESS_GO_AHEAD
 * then a small NVT state machine filters IAC sequences out of the input
 * stream, responding DONT/WONT to anything the peer demands of us and
 * collapsing an escaped IAC-IAC to a literal 0xFF byte.  CR, CR NUL and
 * CR LF each produce exactly one line-feed for the shell (a bare LF also
 * works).  Full ANSI CSI sequences are consumed — parameter and
 * intermediate bytes up to the final byte — with arrows, Home/End,
 * PgUp/PgDn and Delete mapped to the shell's virtual key codes.
 * Interrupt Process / Abort Output / Break (IAC IP / IAC AO / IAC BRK)
 * and a raw Ctrl-C byte all become the ETX (0x03) break byte the shell
 * treats as a command interrupt; IAC EC erases a char; IAC AYT gets a
 * "yes" reply.
 *
 * Dead peers: a session that sees no input for TELNETD_IDLE_PROBE_TICKS
 * gets an IAC AYT liveness probe (a live client answers; a vanished one
 * lets the probe's retransmits exhaust, which tcp_tick turns into
 * TCP_CLOSED).  No input at all for TELNETD_IDLE_TICKS closes the
 * session — a laptop that slept holding the connection would otherwise
 * pin a remote shell slot forever.
 */

#include "telnetd.h"
#include "tcp.h"
#include "stack.h"
#include "../exec/task.h"
#include "../irq/irq.h"
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
    NVT_ESC,            /* ESC seen — expect CSI/SS3 leader */
    NVT_CSI,            /* ESC [ seen — consume params until final byte */
    NVT_SS3,            /* ESC O seen — single final byte follows */
    NVT_CR,             /* CR delivered — swallow a following LF/NUL */
};

/* PIT runs at 100 Hz — express the watchdog budget in ticks.  Halfway
 * idle we probe the peer with IAC AYT; fully idle we close. */
#define TELNETD_IDLE_PROBE_TICKS  30000u   /* ~5 min  */
#define TELNETD_IDLE_TICKS        60000u   /* ~10 min */

extern volatile uint64_t g_pit_ticks;      /* 100 Hz */

static volatile int g_running = 0;   /* daemon task is alive */
static volatile int g_stop    = 0;   /* stop requested via Telnetd_Stop() */
static uint16_t     g_port    = 0;   /* port the listener is bound to */

/* Pump context pool — one per live connection.  g_generation is bumped
 * by every Telnetd_Start so a pump left over from a previous run notices
 * and exits instead of clinging to a stale session. */
typedef struct {
    volatile int inuse;
    int          sock;
    uint32_t     conn_gen;    /* socket's connection generation — the pump
                               * must never act on a recycled slot whose
                               * index it still holds (UAOS-263) */
    void        *sess;
    uint32_t     gen;
    void        *task;        /* the pump task itself — lets Task_Exit's
                               * cleanup hook reclaim the ctx, session and
                               * socket if the task dies without running
                               * its exit path (UAOS-263) */
    ipv4_t       peer_ip;     /* for connect/disconnect logging + STATUS */
    uint16_t     peer_port;
    uint64_t     t_connect;   /* PIT tick when the session was accepted */
} PumpCtx;

static PumpCtx           g_pump_ctx[TCP_MAX_SOCKETS];
static volatile int      g_pump_count  = 0;
static volatile uint32_t g_generation  = 0;

static int send_buf(int sock, uint32_t gen, const uint8_t *b, int len)
{
    /* tcp_send allows only one in-flight segment per socket and returns
     * 0 while busy — keep retrying so negotiation bytes are not dropped,
     * but bound the wait (~250 ms) so a dead peer cannot wedge us.  All
     * socket ops are generation-checked: if our socket was retired and
     * the slot reissued, they fail instead of touching the new tenant. */
    uint64_t deadline = g_pit_ticks + 25;
    for (;;) {
        if (tcp_conn_send(sock, gen, b, (uint16_t)len) > 0) return 1;
        TcpState t = tcp_conn_state(sock, gen);
        if (t != TCP_ESTABLISHED && t != TCP_CLOSE_WAIT) return 0;
        if (g_pit_ticks >= deadline) return 0;
        net_stack_poll();
        Task_WaitTicks(SIGF_NET, 1);   /* ACK arrival wakes us */
    }
}

static void send_neg(int sock, uint32_t gen, uint8_t cmd, uint8_t opt)
{
    uint8_t b[3] = { TN_IAC, cmd, opt };
    send_buf(sock, gen, b, 3);
}

/* One-shot initial negotiation — ask the client to let us echo and to
 * suppress go-ahead (character-at-a-time mode).  All nine bytes go out
 * in a single segment: sent one WILL/DO at a time, the first send could
 * still be in flight when the next arrives and tcp_send would drop it —
 * a client that sees WILL ECHO but not WILL SGA switches off local echo
 * yet stays in line mode, so typed keys stay invisible until Enter. */
static void send_greeting_neg(int sock, uint32_t gen)
{
    static const uint8_t neg[] = {
        TN_IAC, TN_WILL, TN_OPT_ECHO,
        TN_IAC, TN_WILL, TN_OPT_SGA,
        TN_IAC, TN_DO,   TN_OPT_SGA
    };
    send_buf(sock, gen, neg, (int)sizeof(neg));
}

/* Feed a data byte through the NVT filter into the shell.
 * st[] carries: [0] parser state, [1] saved neg command, [2] first CSI
 * parameter digit (0 = none seen).  Returns the byte to enqueue —
 * including the negative SHELL_VKEY_* codes — or -1 to drop it. */
static int nvt_filter(uint8_t *st, uint8_t c, int sock, uint32_t gen,
                    void *sess)
{
    switch (*st) {
    case NVT_IAC:
        switch (c) {
        case TN_WILL:
        case TN_WONT:
        case TN_DO:
        case TN_DONT:
            /* st[1] is saved by the caller before we run — it survives
             * the transition so NVT_NEG knows which verb to answer. */
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
        case TN_BRK:
            /* Interrupt Process / Abort Output / Break → feed the
             * shell's break byte (ETX), which interrupts a running
             * command or cancels the current input line. */
            *st = NVT_DATA;
            return 0x03;
        case TN_EC:
            *st = NVT_DATA;
            return '\b';    /* Erase Character → backspace */
        case TN_AYT:
            /* Are You There — the classic liveness check: answer with
             * a short banner so the client sees the session is alive. */
            *st = NVT_DATA;
            send_buf(sock, gen, (const uint8_t *)"\r\n[UAOS yes]\r\n", 14);
            return -1;
        default:
            *st = NVT_DATA;
            return -1;      /* NOP, DM, EL, GA ... just drop */
        }
    case NVT_NEG:
        /* Byte is the option under negotiation; the command byte was
         * saved in st[1] by the caller.  Refuse DO for anything we did
         * not offer, and refuse WILL for anything we did not ask for. */
        if (c == TN_IAC) {
            /* Malformed sequence like "IAC WILL IAC" — do not eat the
             * IAC as an option byte, let it start a fresh command. */
            *st = NVT_IAC;
            st[1] = 0;
            return -1;
        }
        *st = NVT_DATA;
        {
            uint8_t cmd = st[1];
            st[1] = 0;
            if (cmd == TN_DO && c != TN_OPT_ECHO && c != TN_OPT_SGA)
                send_neg(sock, gen, TN_WONT, c);
            if (cmd == TN_WILL && c != TN_OPT_SGA)
                send_neg(sock, gen, TN_DONT, c);
            /* DONT ECHO — the client declined our WILL ECHO and does
             * its own echo (linemode): suppress our per-keystroke line
             * repaint or it would double every character. */
            if (cmd == TN_DONT && c == TN_OPT_ECHO && sess)
                ShellWin_RemoteSetEcho(sess, 0);
            if (cmd == TN_DO && c == TN_OPT_ECHO && sess)
                ShellWin_RemoteSetEcho(sess, 1);
        }
        return -1;
    case NVT_SB:
        if (c == TN_IAC) *st = NVT_SB_IAC;
        return -1;
    case NVT_SB_IAC:
        *st = (c == TN_SE) ? NVT_DATA : NVT_SB;
        return -1;
    case NVT_ESC:
        *st = (c == '[') ? NVT_CSI : (c == 'O') ? NVT_SS3 : NVT_DATA;
        st[2] = 0;
        return -1;
    case NVT_CSI:
        /* Consume the full sequence: parameter bytes (0x30–0x3F) and
         * intermediates (0x20–0x2F) until a final byte (0x40–0x7E).
         * Consuming only the first byte used to leak the tail of e.g.
         * "ESC [ 3 ~" (Delete) into the input line as literal text. */
        if (c >= 0x30 && c <= 0x3F) {
            if (!st[2] && c >= '0' && c <= '9') st[2] = c; /* first digit */
            return -1;
        }
        if (c >= 0x20 && c <= 0x2F) return -1;   /* intermediate bytes */
        if (c < 0x40 || c > 0x7E) {              /* malformed — abort */
            *st = NVT_DATA;
            st[2] = 0;
            return -1;
        }
        *st = NVT_DATA;
        {
            uint8_t p1 = st[2];
            st[2] = 0;
            switch (c) {
            case 'A': return SHELL_VKEY_UP;
            case 'B': return SHELL_VKEY_DOWN;
            case 'C': return SHELL_VKEY_RIGHT;
            case 'D': return SHELL_VKEY_LEFT;
            case 'H': return SHELL_VKEY_HOME;
            case 'F': return SHELL_VKEY_END;
            case '~':
                switch (p1) {
                case '1': case '7': return SHELL_VKEY_HOME;
                case '4': case '8': return SHELL_VKEY_END;
                case '2': return -1;                /* Insert — ignore */
                case '3': return SHELL_VKEY_DEL;    /* delete under cursor */
                case '5': return SHELL_VKEY_PGUP;
                case '6': return SHELL_VKEY_PGDN;
                default:  return -1;
                }
            default:  return -1;
            }
        }
    case NVT_SS3:
        /* ESC O <final> — application-mode keys (xterm arrows etc.) */
        *st = NVT_DATA;
        switch (c) {
        case 'A': return SHELL_VKEY_UP;
        case 'B': return SHELL_VKEY_DOWN;
        case 'C': return SHELL_VKEY_RIGHT;
        case 'D': return SHELL_VKEY_LEFT;
        case 'H': return SHELL_VKEY_HOME;
        case 'F': return SHELL_VKEY_END;
        default:  return -1;
        }
    case NVT_CR:
        /* CR delivered as '\n' — the RFC 854 terminator is CR LF or
         * CR NUL, so swallow exactly one following LF/NUL.  Anything
         * else means the CR stood alone: re-filter this byte as data. */
        *st = NVT_DATA;
        if (c == '\n' || c == 0) return -1;
        return nvt_filter(st, c, sock, gen, sess);
    case NVT_DATA:
    default:
        if (c == TN_IAC) { *st = NVT_IAC; return -1; }
        if (c == 0x1B)   { *st = NVT_ESC; return -1; }
        if (c == '\r')   { *st = NVT_CR;  return '\n'; }
        if (c == '\n')   return '\n';
        if (c == 0x7F) return '\b';      /* DEL = backspace */
        if (c == 0) return -1;           /* stray NUL padding */
        return c;
    }
}

/* Session pump: bridge one accepted socket to a remote shell session.
 * Runs as its own task (one per connection, so sessions are concurrent)
 * until the peer disconnects, the shell ENDCLIs, the socket dies, the
 * daemon stops, a newer daemon generation takes over, or the peer goes
 * idle past TELNETD_IDLE_TICKS. */
static void pump_task(void *arg)
{
    PumpCtx *ctx  = (PumpCtx *)arg;
    int      sock = ctx->sock;
    uint32_t cgen = ctx->conn_gen;   /* connection identity — see below */
    void    *sess = ctx->sess;
    uint32_t gen  = ctx->gen;
    ctx->task = Task_Current();      /* for Telnetd_CleanupTask() */

    /* nvt state: [0] = state, [1] = saved neg cmd, [2] = CSI param digit */
    uint8_t st[3] = { NVT_DATA, 0, 0 };
    uint8_t buf[256];

    /* Wake on RX at interrupt time: the driver kicks armed tasks the
     * moment a frame lands instead of waiting out the 100 Hz tick. */
    net_rx_notify_arm();

    /* Idle watchdog: last_rx only advances on received input.  A peer
     * that vanishes without FIN/RST (sleep, NAT drop) keeps the socket
     * ESTABLISHED forever — the half-timeout IAC AYT probe forces the
     * dead connection to exhaust its retransmits in tcp_tick, and the
     * full timeout closes a silent-but-alive peer that ignores AYT. */
    uint64_t last_rx = g_pit_ticks;
    int      probed  = 0;

    for (;;) {
        /* Daemon asked to stop, superseded by a fresh Telnetd_Start, or
         * the net stack was shut down (netstop).  net_stack_shutdown()
         * leaves the socket table untouched, so the socket state alone
         * cannot tell us the stack is gone. */
        if (g_stop || gen != g_generation || !net_stack_is_up())
            break;

        /* Socket gone (peer closed / RST / probe retx exhaustion) — or
         * the slot was retired under us and reissued to a different
         * connection: tcp_rx/tcp_tick free sockets asynchronously, so a
         * bare index is not proof of ownership.  The generation check
         * reports CLOSED on any mismatch — without it a slow pump would
         * steal a recycled slot's input and, on exit, close a live
         * connection that isn't ours (UAOS-263). */
        TcpState t = tcp_conn_state(sock, cgen);
        if (t != TCP_ESTABLISHED && t != TCP_CLOSE_WAIT)
            break;

        int n = tcp_conn_recv(sock, cgen, buf, sizeof(buf));
        /* Half-close: peer sent FIN and RX buffer is drained */
        if (n <= 0 && t == TCP_CLOSE_WAIT)
            break;
        if (n > 0) {
            last_rx = g_pit_ticks;
            probed  = 0;
        }
        for (int i = 0; i < n; i++) {
            uint8_t c = buf[i];
            /* Remember neg command byte for NVT_NEG */
            if (st[0] == NVT_IAC &&
                (c == TN_WILL || c == TN_WONT || c == TN_DO || c == TN_DONT))
                st[1] = c;
            int k = nvt_filter(st, c, sock, cgen, sess);
            if (k != -1) {
                /* A pasted burst can fill the shell's key queue while a
                 * command runs.  Wait for the session to drain it rather
                 * than dropping the byte: a lost '\n' leaves a
                 * half-typed command that never executes and the
                 * session looks dead. */
                uint64_t deadline = g_pit_ticks + 500;  /* ~5 s */
                while (!ShellWin_RemoteFeed(sess, (char)k)) {
                    if (ShellWin_RemoteIsDead(sess) ||
                        g_stop || gen != g_generation ||
                        !net_stack_is_up() ||
                        g_pit_ticks >= deadline)
                        break;
                    net_stack_poll();
                    Task_SleepTicks(1);
                }
            }
        }

        /* Shell side ended (ENDCLI) or slot released */
        if (ShellWin_RemoteIsDead(sess))
            break;

        /* Idle watchdog — see the block above. */
        {
            uint64_t idle = g_pit_ticks - last_rx;
            if (!probed && idle >= TELNETD_IDLE_PROBE_TICKS) {
                static const uint8_t ayt[] = { TN_IAC, TN_AYT };
                send_buf(sock, cgen, ayt, (int)sizeof(ayt));
                probed = 1;
            }
            if (idle >= TELNETD_IDLE_TICKS) {
                static const char to[] =
                    "\r\n[telnetd: idle timeout — closing]\r\n";
                send_buf(sock, cgen, (const uint8_t *)to,
                         (int)sizeof(to) - 1);
                break;
            }
        }

        net_stack_poll();
        /* Sleep until the next RX interrupt kicks us, or at most one
         * tick — keeps the idle watchdog and dead-socket checks on a
         * bounded cadence without paying ~10 ms per keystroke. */
        Task_WaitTicks(SIGF_NET, 1);
    }

    /* Connection log — the disconnect side pairs with the "connect
     * from" line emitted at accept time. */
    {
        char ipbuf[20];
        net_ip_to_str(ctx->peer_ip, ipbuf);
        kprint("telnetd: disconnect ");
        kprint(ipbuf);
        kprint(":");
        kprintdec(ctx->peer_port);
        kprint(" (up ");
        kprintdec((uint32_t)((g_pit_ticks - ctx->t_connect) / 100));
        kprint("s)\n");
    }

    /* Notify the shell side and drop the connection — but only if our
     * session is still the one owning the slot; after ENDCLI the slot
     * may already have been recycled for a new session.  When the stack
     * is down a FIN could never be answered and tcp_tick no longer runs
     * to retire the socket — abort instead so the slot is freed. */
    if (!ShellWin_RemoteIsDead(sess))
        ShellWin_RemoteKill(sess);
    if (net_stack_is_up()) {
        static const char bye[] = "\r\n[session closed]\r\n";
        send_buf(sock, cgen, (const uint8_t *)bye, (int)sizeof(bye) - 1);
        tcp_conn_close(sock, cgen);
    } else {
        tcp_conn_abort(sock, cgen);
    }
    uint64_t fl = irq_save();
    g_pump_count--;
    irq_restore(fl);
    ctx->task  = NULL;
    ctx->inuse = 0;
    Task_Exit();
}

static void telnetd_task(void *arg)
{
    uint16_t port = (uint16_t)(uintptr_t)arg;
    net_rx_notify_arm();
    int lsock = tcp_listen(port);
    if (lsock < 0) {
        kprint("telnetd: tcp_listen failed\n");
        g_running = 0;
        Task_Exit();
    }
    g_port = port;
    kprint("telnetd: listening on port ");
    kprintdec(port);
    kprint("\n");

    /* Run until STOP is requested or the stack goes down; the latter
     * lets a fresh telnetd start cleanly after the next netstart.  Every
     * accepted socket gets a remote shell slot and its own pump task;
     * when no slot (or pump context) is free the connection is answered
     * with a busy banner and closed rather than left silent. */
    while (!g_stop && net_stack_is_up()) {
        uint32_t cgen = 0;
        int csock = tcp_accept(lsock, &cgen);
        if (csock >= 0) {
            /* Log every accepted connection (peer ip:port) — an
             * unauthenticated service should at least leave a trace. */
            ipv4_t peer_ip = 0;
            uint16_t peer_port = 0;
            if (tcp_peer(csock, &peer_ip, &peer_port) == 0) {
                char ipbuf[20];
                net_ip_to_str(peer_ip, ipbuf);
                kprint("telnetd: connect ");
                kprint(ipbuf);
                kprint(":");
                kprintdec(peer_port);
                kprint("\n");
            }
            send_greeting_neg(csock, cgen);
            void *sess = ShellWin_RemoteOpen(csock, cgen);
            PumpCtx *ctx = NULL;
            if (sess) {
                for (int i = 0; i < TCP_MAX_SOCKETS; i++) {
                    if (!g_pump_ctx[i].inuse) { ctx = &g_pump_ctx[i]; break; }
                }
            }
            if (!sess || !ctx) {
                static const char busy[] =
                    "\r\nUAOS: no remote shell slots free, try later\r\n";
                send_buf(csock, cgen,
                         (const uint8_t *)busy, (int)sizeof(busy) - 1);
                tcp_conn_close(csock, cgen);
                if (sess) ShellWin_RemoteKill(sess);
            } else {
                ctx->inuse     = 1;
                ctx->sock      = csock;
                ctx->conn_gen  = cgen;
                ctx->sess      = sess;
                ctx->gen       = g_generation;
                ctx->peer_ip   = peer_ip;
                ctx->peer_port = peer_port;
                ctx->t_connect = g_pit_ticks;
                uint64_t fl = irq_save();
                g_pump_count++;
                irq_restore(fl);
                if (!Task_CreateNative("telnetd-session", 0,
                                       pump_task, ctx)) {
                    ctx->inuse = 0;
                    fl = irq_save();
                    g_pump_count--;
                    irq_restore(fl);
                    ShellWin_RemoteKill(sess);
                    tcp_conn_close(csock, cgen);
                }
            }
        }
        net_stack_poll();
        Task_WaitTicks(SIGF_NET, 1);   /* wake on inbound SYN, not tick */
    }
    tcp_close(lsock);
    g_port = 0;
    g_running = 0;
    kprint("telnetd: stopped\n");
    Task_Exit();
}

int Telnetd_Start(uint16_t port)
{
    if (port == 0) port = TELNETD_DEFAULT_PORT;
    if (!net_stack_is_up()) return 0;
    /* Atomic check-and-set — a double-start (User-Startup + manual, or
     * two callers racing the gap between the test and the store) used
     * to spawn two listener tasks on the same port. */
    uint64_t fl = irq_save();
    if (g_running) {
        irq_restore(fl);
        return 0;
    }
    /* Set the flags before spawning: if the new task runs first and
     * tcp_listen fails it clears g_running itself.  Bumping the
     * generation tells any pump left over from a previous run to exit. */
    g_generation++;
    g_stop    = 0;
    g_running = 1;
    irq_restore(fl);
    if (!Task_CreateNative("telnetd", 0, telnetd_task,
                           (void *)(uintptr_t)port)) {
        g_running = 0;
        return 0;
    }
    return 1;
}

void Telnetd_Stop(void)
{
    if (!g_running) return;
    g_stop = 1;
    /* Wait for the daemon task — and the per-connection pump tasks — to
     * notice g_stop, close their sockets and wind down.  Task_Yield()
     * performs a real reschedule (UAOS-169), so the daemon runs
     * immediately; when it can't switch (scheduler not started, or
     * Forbid held — e.g. `&` jobs under the idle task) hlt still sleeps
     * until the next IRQ.  The ~1 s deadline bounds the shutdown. */
    uint64_t deadline = g_pit_ticks + 100;
    while ((g_running || g_pump_count > 0) && g_pit_ticks < deadline) {
        if (!Task_Yield()) {
            /* irq_save/irq_restore around sti;hlt: the sti is needed for
             * the hlt to wake on IRQs, but a bare sti would leak IF=1 to
             * a caller that entered with interrupts off (UAOS-176). */
            uint64_t fl = irq_save();
            __asm__ volatile ("sti; hlt" ::: "memory");
            irq_restore(fl);
        }
    }
}

int Telnetd_IsRunning(void)
{
    return g_running;
}

uint16_t Telnetd_Port(void)
{
    return g_port;
}

/* Task_Exit hook — a pump task that dies without reaching its exit path
 * (RemTask-style removal skips Task_Exit cleanup) would otherwise leak
 * its socket — ESTABLISHED has no stack-side bound — plus the remote
 * shell slot and this pump context (UAOS-263). */
void Telnetd_CleanupTask(void *task)
{
    if (!task) return;
    for (int i = 0; i < TCP_MAX_SOCKETS; i++) {
        PumpCtx *ctx = &g_pump_ctx[i];
        if (!ctx->inuse || ctx->task != task) continue;
        if (!ShellWin_RemoteIsDead(ctx->sess))
            ShellWin_RemoteKill(ctx->sess);
        if (net_stack_is_up())
            tcp_conn_close(ctx->sock, ctx->conn_gen);
        else
            tcp_conn_abort(ctx->sock, ctx->conn_gen);
        uint64_t fl = irq_save();
        g_pump_count--;
        irq_restore(fl);
        ctx->task  = NULL;
        ctx->inuse = 0;
    }
}

int Telnetd_SessionInfo(int idx, ipv4_t *ip, uint16_t *port,
                        uint32_t *up_secs)
{
    int n = 0;
    for (int i = 0; i < TCP_MAX_SOCKETS; i++) {
        if (!g_pump_ctx[i].inuse) continue;
        if (n++ == idx) {
            if (ip)   *ip      = g_pump_ctx[i].peer_ip;
            if (port) *port    = g_pump_ctx[i].peer_port;
            if (up_secs)
                *up_secs = (uint32_t)((g_pit_ticks -
                                       g_pump_ctx[i].t_connect) / 100);
            return 1;
        }
    }
    return 0;
}
