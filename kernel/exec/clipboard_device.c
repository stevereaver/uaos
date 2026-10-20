/*
 * clipboard_device.c — UAOS clipboard.device Implementation (UAOS-358)
 *
 * AmigaOS clipboard.device provides a stream-oriented system clipboard.
 * The byte stream is opaque to the device — applications conventionally
 * write IFF FTXT (FORM/FTXT with CHRS chunks) and iffparse.library does
 * the parsing.  Here the stream lands in a host-side buffer (the "host
 * clipboard"), readable back through the same device or via
 * ClipDev_SetText()/ClipDev_GetText() which wrap/unwrap FTXT for plain
 * text producers.
 *
 * Protocol (devices/clipboard.h):
 *   CMD_WRITE — append io_Length bytes at io_Offset; a write at offset 0
 *               starts a new clip (bumps the write clip-id, which makes
 *               older read ids obsolete -> CBERR_OBSOLETEID).
 *   CMD_UPDATE — end-of-stream: the buffered clip becomes current.
 *   CMD_READ — serve bytes at io_Offset into io_Data.  An out-of-date
 *               io_ClipID fails CBERR_OBSOLETEID; a read on an empty clip
 *               queues until a write lands (device-pending semantics).
 *   CBD_POST / CBD_CURRENTREADID / CBD_CURRENTWRITEID / CBD_CHANGEHOOK.
 *
 * SatisfyMsg notification on CBD_POST is registered but not posted —
 * a posted provider simply writes on demand; queued reads complete when
 * the write stream updates.  Guest memory is big-endian — cb_r/cb_w only.
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include "rom_modules.h"
#include "task.h"
#include "../../emulation/uaos_emu.h"

/* =========================================================================
 * Guest structure offsets (big-endian) — IOClipReq = IOStdReq + fields
 * ========================================================================= */

#define IO_COMMAND       28
#define IO_FLAGS         30
#define IO_ERROR         31
#define IO_ACTUAL        32    /* IOClipReq.io_Actual  */
#define IO_LENGTH        36    /* IOClipReq.io_Length  */
#define IO_DATA          40    /* IOClipReq.io_Data    */
#define IO_OFFSET        44    /* IOClipReq.io_Offset  */
#define IO_CLIPID        48    /* IOClipReq.io_ClipID  */
#define IOCLIP_SIZE      52
#define IO_UNIT          24    /* IORequest.io_Unit    */
#define IO_DEVICE        20    /* IORequest.io_Device  */
#define MN_REPLYPORT     14    /* io_Message.mn_ReplyPort */

#define MP_MSGLIST       20    /* MsgPort.mp_MsgList (for satisfy note)   */

/* =========================================================================
 * clipboard.device commands (exec/io.h + devices/clipboard.h)
 * ========================================================================= */

#define CMD_RESET              1
#define CMD_READ               2
#define CMD_WRITE              3
#define CMD_UPDATE             4
#define CMD_CLEAR              5
#define CMD_STOP               6
#define CMD_START              7
#define CMD_FLUSH              8
#define CMD_NONSTD             9

#define CBD_POST            (CMD_NONSTD + 0)   /* 9  */
#define CBD_CURRENTREADID   (CMD_NONSTD + 1)   /* 10 */
#define CBD_CURRENTWRITEID  (CMD_NONSTD + 2)   /* 11 */
#define CBD_CHANGEHOOK      (CMD_NONSTD + 3)   /* 12 */

#define CBERR_OBSOLETEID    1

#define IOERR_OPENFAIL     (-1)
#define IOERR_ABORTED      (-2)
#define IOERR_NOCMD        (-3)
#define IOERR_BADLENGTH    (-4)
#define IOERR_BADADDRESS   (-9)

#define PRIMARY_CLIP        0

#define IOF_SATISFY         0x40  /* io_Flags: write is a satisfy response */

/* =========================================================================
 * Guest memory helpers — big-endian
 * ========================================================================= */

extern uint8_t *g_ram;

static uint16_t cb_r16(uint8_t *ram, uint32_t a)
{
    return (uint16_t)(((uint16_t)ram[a] << 8) | ram[a + 1]);
}

static void cb_w32(uint8_t *ram, uint32_t a, uint32_t v)
{
    ram[a] = (uint8_t)(v >> 24); ram[a + 1] = (uint8_t)(v >> 16);
    ram[a + 2] = (uint8_t)(v >> 8); ram[a + 3] = (uint8_t)v;
}

static uint32_t cb_r32(uint8_t *ram, uint32_t a)
{
    return ((uint32_t)ram[a] << 24) | ((uint32_t)ram[a + 1] << 16) |
           ((uint32_t)ram[a + 2] <<  8) |  (uint32_t)ram[a + 3];
}

static void cb_w16(uint8_t *ram, uint32_t a, uint16_t v)
{
    ram[a] = (uint8_t)(v >> 8); ram[a + 1] = (uint8_t)v;
}

/* =========================================================================
 * Host clip store + pending reads
 * ========================================================================= */

#define CLIP_BUF_SIZE (32 * 1024)

static uint8_t  g_clip_buf[CLIP_BUF_SIZE];
static uint32_t g_clip_len;        /* bytes staged by the write stream   */
static uint32_t g_clip_avail;      /* bytes published by CMD_UPDATE      */
static int32_t  g_write_id;        /* bump on every new write stream     */
static int32_t  g_read_id;         /* write-id of the published clip     */
static int      g_write_active;    /* a write stream is open             */
static uint32_t g_open_cnt;
static int      g_stopped;         /* CMD_STOP/CMD_START flow control    */

/* CBD_POST registration: provider port + the unit it posted for.  Used
 * only for bookkeeping today — queued reads complete on data, not on a
 * SatisfyMsg handshake. */
static uint32_t g_post_port;
static uint32_t g_post_unit;

typedef struct {
    uint8_t   active;
    uint32_t  io;
    uint8_t  *ram;
    UaosTask *task;
    uint32_t  data;
    uint32_t  len;
    uint32_t  off;
} ClipPendingRead;

#define CLIP_MAX_PENDING 8
static ClipPendingRead g_clip_pending[CLIP_MAX_PENDING];

/* Serve a read from the published clip.  Returns bytes written. */
static uint32_t clip_serve(uint8_t *ram, uint32_t data, uint32_t off,
                           uint32_t len)
{
    if (off >= g_clip_avail || !data || data >= GUEST_RAM_SIZE)
        return 0;
    uint32_t n = g_clip_avail - off;
    if (n > len) n = len;
    if (data + n > GUEST_RAM_SIZE) n = GUEST_RAM_SIZE - data;
    for (uint32_t i = 0; i < n; i++)
        ram[data + i] = g_clip_buf[off + i];
    return n;
}

/* Complete every queued read now that a clip is published. */
static void clip_flush_pending(void)
{
    for (int i = 0; i < CLIP_MAX_PENDING; i++) {
        ClipPendingRead *r = &g_clip_pending[i];
        if (!r->active) continue;
        uint8_t *ram = r->ram;
        uint32_t io  = r->io;
        uint32_t n   = clip_serve(ram, r->data, r->off, r->len);
        cb_w32(ram, io + IO_ACTUAL, n);
        cb_w32(ram, io + IO_CLIPID, (uint32_t)g_read_id);
        ram[io + IO_ERROR] = 0;
        r->active = 0;
        UAOS_Emu_IOReply(ram, io);
    }
}

/* Task-exit purge: never complete into a recycled window. */
void ClipDev_DropTaskRequests(UaosTask *t)
{
    if (!t) return;
    for (int i = 0; i < CLIP_MAX_PENDING; i++)
        if (g_clip_pending[i].active && g_clip_pending[i].task == t)
            g_clip_pending[i].active = 0;
}

/* =========================================================================
 * Host clipboard text API — FTXT wrap/unwrap for plain-text producers
 * =========================================================================
 * IFF FTXT: FORM <len> "FTXT" CHRS <len> <text>  — CHRS chunks are the
 * body; other chunks (FONS, CSAR, ...) are skipped on read. */

int ClipDev_SetText(const char *text, uint32_t len)
{
    if (!text) len = 0;
    uint32_t body = len;
    if (body > CLIP_BUF_SIZE - 20) body = CLIP_BUF_SIZE - 20;
    /* FORM header */
    g_clip_buf[0]='F'; g_clip_buf[1]='O'; g_clip_buf[2]='R'; g_clip_buf[3]='M';
    uint32_t form_len = 4 + 8 + body + (body & 1);
    g_clip_buf[4]=(uint8_t)(form_len>>24); g_clip_buf[5]=(uint8_t)(form_len>>16);
    g_clip_buf[6]=(uint8_t)(form_len>>8);  g_clip_buf[7]=(uint8_t)form_len;
    g_clip_buf[8]='F'; g_clip_buf[9]='T'; g_clip_buf[10]='X'; g_clip_buf[11]='T';
    g_clip_buf[12]='C'; g_clip_buf[13]='H'; g_clip_buf[14]='R'; g_clip_buf[15]='S';
    g_clip_buf[16]=(uint8_t)(body>>24); g_clip_buf[17]=(uint8_t)(body>>16);
    g_clip_buf[18]=(uint8_t)(body>>8);  g_clip_buf[19]=(uint8_t)body;
    for (uint32_t i = 0; i < body; i++) g_clip_buf[20+i] = (uint8_t)text[i];
    if (body & 1) g_clip_buf[20+body] = 0;   /* IFF chunks are even-padded */
    g_clip_len   = 20 + body + (body & 1);
    g_clip_avail = g_clip_len;
    g_write_id++;
    g_read_id = g_write_id;
    g_write_active = 0;
    clip_flush_pending();
    return (int)body;
}

/* Extract text from the published clip: CHRS chunks if the buffer is a
 * FORM FTXT, raw bytes otherwise.  Returns bytes copied. */
uint32_t ClipDev_GetText(char *out, uint32_t max)
{
    if (!out || !max) return 0;
    uint32_t n = 0;
    const uint8_t *b = g_clip_buf;
    uint32_t bl = g_clip_avail;
    if (bl >= 20 && b[0]=='F' && b[1]=='O' && b[2]=='R' && b[3]=='M' &&
        b[8]=='F' && b[9]=='T' && b[10]=='X' && b[11]=='T') {
        uint32_t p = 12;
        while (p + 8 <= bl) {
            uint32_t clen = ((uint32_t)b[p+4]<<24) | ((uint32_t)b[p+5]<<16) |
                            ((uint32_t)b[p+6]<<8)  |  (uint32_t)b[p+7];
            uint32_t next = p + 8 + clen + (clen & 1);
            if (next > bl) { clen = bl - p - 8; next = bl; }
            if (b[p]=='C' && b[p+1]=='H' && b[p+2]=='R' && b[p+3]=='S') {
                for (uint32_t i = 0; i < clen && n + 1 < max; i++)
                    out[n++] = (char)b[p + 8 + i];
            }
            p = next;
        }
    } else {
        while (n < bl && n + 1 < max) out[n] = (char)b[n], n++;
    }
    out[n] = '\0';
    return n;
}

/* =========================================================================
 * Device vectors (M68kCPUState marshalled by the glue's emu_rom_call)
 * ========================================================================= */

static void clipboard_OpenDevice(M68kCPUState *cpu)
{
    /* Open (-6): A1 = IORequest, D0 = unit, D1 = flags */
    uint32_t io = cpu->a[1];
    if (cpu->d[0] != PRIMARY_CLIP) {
        if (io && io + IOCLIP_SIZE <= GUEST_RAM_SIZE)
            g_ram[io + IO_ERROR] = (uint8_t)IOERR_OPENFAIL;
        cpu->d[0] = (uint32_t)IOERR_OPENFAIL;
        return;
    }
    g_open_cnt++;
    if (io && io + IOCLIP_SIZE <= GUEST_RAM_SIZE)
        g_ram[io + IO_ERROR] = 0;
    cpu->d[0] = 0;
}

static void clipboard_CloseDevice(M68kCPUState *cpu)
{
    /* Close (-12): A1 = IORequest */
    if (g_open_cnt) g_open_cnt--;
    ClipDev_DropTaskRequests(Task_Current());
    cpu->d[0] = 0;
}

static void clipboard_BeginIO(M68kCPUState *cpu)
{
    /* BeginIO (-30): A1 = IOClipReq */
    uint32_t io = cpu->a[1];
    if (!io || io + IOCLIP_SIZE > GUEST_RAM_SIZE) {
        cpu->d[0] = (uint32_t)-1;
        return;
    }

    uint16_t cmd = cb_r16(g_ram, io + IO_COMMAND);

    switch (cmd) {
    case CMD_WRITE: {
        /* Stream write: append at io_Offset.  Offset 0 opens a new clip —
         * the write id bumps immediately so a reader using the old id
         * gets CBERR_OBSOLETEID like on real hardware. */
        uint32_t data = cb_r32(g_ram, io + IO_DATA);
        uint32_t len  = cb_r32(g_ram, io + IO_LENGTH);
        uint32_t off  = cb_r32(g_ram, io + IO_OFFSET);
        if (off == 0) {
            g_clip_len = 0;
            g_write_active = 1;
            g_write_id++;
        } else if (!g_write_active) {
            g_ram[io + IO_ERROR] = (uint8_t)CBERR_OBSOLETEID;
            cb_w32(g_ram, io + IO_ACTUAL, 0);
            cpu->d[0] = 0;
            return;
        }
        uint32_t n = 0;
        if (off < CLIP_BUF_SIZE && data && data < GUEST_RAM_SIZE) {
            n = CLIP_BUF_SIZE - off;
            if (n > len) n = len;
            if (data + n > GUEST_RAM_SIZE) n = GUEST_RAM_SIZE - data;
            for (uint32_t i = 0; i < n; i++)
                g_clip_buf[off + i] = g_ram[data + i];
            if (off + n > g_clip_len) g_clip_len = off + n;
        }
        cb_w32(g_ram, io + IO_ACTUAL, n);
        cb_w32(g_ram, io + IO_CLIPID, (uint32_t)g_write_id);
        g_ram[io + IO_ERROR] = 0;
        cpu->d[0] = 0;
        break;
    }

    case CMD_UPDATE: {
        /* End-of-write-stream: publish the staged clip. */
        g_clip_avail = g_clip_len;
        g_read_id    = g_write_id;
        g_write_active = 0;
        g_ram[io + IO_ERROR] = 0;
        cpu->d[0] = 0;
        clip_flush_pending();
        break;
    }

    case CMD_CLEAR:
        /* Abort the staged write; the published clip survives. */
        g_clip_len = 0;
        g_write_active = 0;
        g_ram[io + IO_ERROR] = 0;
        cpu->d[0] = 0;
        break;

    case CMD_READ: {
        uint32_t data   = cb_r32(g_ram, io + IO_DATA);
        uint32_t len    = cb_r32(g_ram, io + IO_LENGTH);
        uint32_t off    = cb_r32(g_ram, io + IO_OFFSET);
        int32_t  clipid = (int32_t)cb_r32(g_ram, io + IO_CLIPID);
        if (clipid && clipid != g_read_id) {
            g_ram[io + IO_ERROR] = (uint8_t)CBERR_OBSOLETEID;
            cb_w32(g_ram, io + IO_ACTUAL, 0);
            cpu->d[0] = 0;
            return;
        }
        if (g_stopped || (off >= g_clip_avail && !g_clip_avail)) {
            /* Nothing to serve — queue until a write publishes (or
             * AbortIO/CloseDevice drops us). */
            for (int i = 0; i < CLIP_MAX_PENDING; i++) {
                if (g_clip_pending[i].active) continue;
                g_clip_pending[i].active = 1;
                g_clip_pending[i].io   = io;
                g_clip_pending[i].ram  = g_ram;
                g_clip_pending[i].task = Task_Current();
                g_clip_pending[i].data = data;
                g_clip_pending[i].len  = len;
                g_clip_pending[i].off  = off;
                g_ram[io + IO_FLAGS] = (uint8_t)((g_ram[io + IO_FLAGS] |
                                                  UIOF_QUEUED) & ~UIOF_QUICK);
                g_ram[io + IO_ERROR] = 0;
                cpu->d[0] = 0;
                return;
            }
            g_ram[io + IO_ERROR] = (uint8_t)IOERR_NOCMD;
            cpu->d[0] = (uint32_t)IOERR_NOCMD;
            return;
        }
        uint32_t n = clip_serve(g_ram, data, off, len);
        cb_w32(g_ram, io + IO_ACTUAL, n);
        cb_w32(g_ram, io + IO_CLIPID, (uint32_t)g_read_id);
        g_ram[io + IO_ERROR] = 0;
        cpu->d[0] = 0;
        break;
    }

    case CBD_POST:
        /* Provider posts a port for satisfy notifications — record it.
         * io_Data is the MsgPort (real device posts a SatisfyMsg when a
         * reader wants data it doesn't have; our queued reads complete
         * on publish instead). */
        g_post_port = cb_r32(g_ram, io + IO_DATA);
        g_post_unit = cb_r32(g_ram, io + IO_UNIT);
        g_ram[io + IO_ERROR] = 0;
        cpu->d[0] = 0;
        break;

    case CBD_CURRENTREADID:
        cb_w32(g_ram, io + IO_CLIPID, (uint32_t)g_read_id);
        cb_w32(g_ram, io + IO_ACTUAL, (uint32_t)g_read_id);
        g_ram[io + IO_ERROR] = 0;
        cpu->d[0] = 0;
        break;

    case CBD_CURRENTWRITEID:
        cb_w32(g_ram, io + IO_CLIPID, (uint32_t)g_write_id);
        cb_w32(g_ram, io + IO_ACTUAL, (uint32_t)g_write_id);
        g_ram[io + IO_ERROR] = 0;
        cpu->d[0] = 0;
        break;

    case CBD_CHANGEHOOK:
        /* Register-and-ignore: no change notifications are generated,
         * but accepting the command lets clients that probe it run. */
        g_ram[io + IO_ERROR] = 0;
        cpu->d[0] = 0;
        break;

    case CMD_STOP:
        g_stopped = 1;
        g_ram[io + IO_ERROR] = 0;
        cpu->d[0] = 0;
        break;

    case CMD_START:
        g_stopped = 0;
        g_ram[io + IO_ERROR] = 0;
        cpu->d[0] = 0;
        break;

    case CMD_FLUSH:
        ClipDev_DropTaskRequests(Task_Current());
        g_ram[io + IO_ERROR] = 0;
        cpu->d[0] = 0;
        break;

    case CMD_RESET:
        g_clip_len = g_clip_avail = 0;
        g_write_active = 0;
        g_stopped = 0;
        g_ram[io + IO_ERROR] = 0;
        cpu->d[0] = 0;
        break;

    default:
        g_ram[io + IO_ERROR] = (uint8_t)IOERR_NOCMD;
        cpu->d[0] = (uint32_t)IOERR_NOCMD;
        break;
    }
}

static void clipboard_AbortIO(M68kCPUState *cpu)
{
    /* AbortIO (-36): A1 = IOClipReq — drop a queued read. */
    uint32_t io = cpu->a[1];
    for (int i = 0; i < CLIP_MAX_PENDING; i++) {
        ClipPendingRead *r = &g_clip_pending[i];
        if (r->active && r->io == io && r->ram == g_ram) {
            r->active = 0;
            g_ram[io + IO_ERROR] = (uint8_t)IOERR_ABORTED;
            cpu->d[0] = 0;
            return;
        }
    }
    cpu->d[0] = (uint32_t)-1;   /* not pending */
}

/* =========================================================================
 * Function table + registration
 * ========================================================================= */

static void *clipboard_funcs[] = {
    clipboard_OpenDevice,   /* index 1  */
    clipboard_CloseDevice,  /* index 2  */
    clipboard_BeginIO,      /* index 3  */
    clipboard_AbortIO,      /* index 4  */
};

/* Device vectors -> clipboard_funcs[] indices (canonical exec/io.h
 * device vectors; -42/-48 aliases kept for the old UAOS map). */
static const UaosRomLvo clipboard_lvo_map[] = {
    {  -6, 1 },   /* Open     */
    { -12, 2 },   /* Close    */
    { -30, 3 },   /* BeginIO  */
    { -36, 4 },   /* AbortIO  */
    { -42, 3 },   /* legacy BeginIO alias */
    { -48, 4 },   /* legacy AbortIO alias */
};

void UAOS_CLIPBOARD_Register(void)
{
    UAOS_ROM_Register("clipboard.device", 40, 0,
                      (uint16_t)(sizeof(clipboard_funcs) / sizeof(clipboard_funcs[0])),
                      clipboard_funcs);
    UAOS_ROM_BindLvoMap("clipboard.device", clipboard_lvo_map,
                        (uint16_t)(sizeof(clipboard_lvo_map) / sizeof(clipboard_lvo_map[0])));
}

/* =========================================================================
 * Self-test — run from the boot battery (selftest cmdline)
 * ========================================================================= */

extern void kprint(const char *s);
extern void dos_AllocMem_glue(uint32_t size, uint32_t reqs, uint32_t *out_addr);

#define ALLOCF (0x00000001u | 0x00010000u)   /* MEMF_PUBLIC | CLEAR */

int uaos_clipdev_test(void)
{
    int errors = 0;
    M68kCPUState cpu;
    memset(&cpu, 0, sizeof(cpu));

    uint32_t io = 0, buf = 0, rd = 0, port = 0;
    dos_AllocMem_glue(IOCLIP_SIZE, ALLOCF, &io);
    dos_AllocMem_glue(64, ALLOCF, &buf);
    dos_AllocMem_glue(64, ALLOCF, &rd);
    dos_AllocMem_glue(64, ALLOCF, &port);
    if (!io || !buf || !rd || !port) {
        kprint("[clipdev] FAIL: alloc\n");
        return 0;
    }

    /* Fake reply port: empty msglist, no sigtask (IOReply appends the
     * node and early-outs on the signal). */
    cb_w32(g_ram, port + MP_MSGLIST + 0, port + MP_MSGLIST + 4);
    cb_w32(g_ram, port + MP_MSGLIST + 4, 0);
    cb_w32(g_ram, port + MP_MSGLIST + 8, port + MP_MSGLIST + 0);
    cb_w32(g_ram, io + MN_REPLYPORT, port);

    /* Open: unit 0 ok, unit 1 refused. */
    cpu.d[0] = 0; cpu.a[1] = io;
    clipboard_OpenDevice(&cpu);
    if (cpu.d[0] != 0 || g_ram[io + IO_ERROR] != 0) {
        errors++; kprint("[clipdev] FAIL: open unit0\n");
    }
    cpu.d[0] = 1;
    clipboard_OpenDevice(&cpu);
    if ((int32_t)cpu.d[0] != IOERR_OPENFAIL) {
        errors++; kprint("[clipdev] FAIL: open unit1\n");
    }
    cpu.d[0] = 0;

    /* Write "hello" at offset 0 — starts a new clip stream. */
    memcpy(g_ram + buf, "hello", 5);
    g_ram[io + IO_FLAGS] = UIOF_QUICK;
    cb_w16(g_ram, io + IO_COMMAND, CMD_WRITE);
    cb_w32(g_ram, io + IO_DATA, buf);
    cb_w32(g_ram, io + IO_LENGTH, 5);
    cb_w32(g_ram, io + IO_OFFSET, 0);
    cb_w32(g_ram, io + IO_CLIPID, 0);
    clipboard_BeginIO(&cpu);
    if (g_ram[io + IO_ERROR] != 0 ||
        cb_r32(g_ram, io + IO_ACTUAL) != 5) {
        errors++; kprint("[clipdev] FAIL: write\n");
    }
    uint32_t wid = cb_r32(g_ram, io + IO_CLIPID);
    if (!wid) { errors++; kprint("[clipdev] FAIL: no write id\n"); }

    /* Publish the staged clip. */
    cb_w16(g_ram, io + IO_COMMAND, CMD_UPDATE);
    clipboard_BeginIO(&cpu);
    if (g_ram[io + IO_ERROR] != 0) {
        errors++; kprint("[clipdev] FAIL: update\n");
    }

    /* CBD_CURRENTREADID reports the published clip id. */
    cb_w16(g_ram, io + IO_COMMAND, CBD_CURRENTREADID);
    cb_w32(g_ram, io + IO_CLIPID, 0);
    clipboard_BeginIO(&cpu);
    if (cb_r32(g_ram, io + IO_CLIPID) != wid) {
        errors++; kprint("[clipdev] FAIL: readid\n");
    }

    /* Read the clip back. */
    memset(g_ram + rd, 0, 16);
    cb_w16(g_ram, io + IO_COMMAND, CMD_READ);
    cb_w32(g_ram, io + IO_DATA, rd);
    cb_w32(g_ram, io + IO_LENGTH, 5);
    cb_w32(g_ram, io + IO_OFFSET, 0);
    cb_w32(g_ram, io + IO_CLIPID, 0);
    clipboard_BeginIO(&cpu);
    if (g_ram[io + IO_ERROR] != 0 ||
        cb_r32(g_ram, io + IO_ACTUAL) != 5 ||
        memcmp(g_ram + rd, "hello", 5) != 0 ||
        cb_r32(g_ram, io + IO_CLIPID) != wid) {
        errors++; kprint("[clipdev] FAIL: readback\n");
    }

    /* A stale clip id reads CBERR_OBSOLETEID. */
    cb_w32(g_ram, io + IO_CLIPID, wid + 9);
    clipboard_BeginIO(&cpu);
    if (g_ram[io + IO_ERROR] != (uint8_t)CBERR_OBSOLETEID) {
        errors++; kprint("[clipdev] FAIL: obsolete id\n");
    }

    /* Empty clip: an async read queues, then a write+update publishes
     * and completes it through IOReply onto the reply port. */
    cb_w16(g_ram, io + IO_COMMAND, CMD_RESET);
    clipboard_BeginIO(&cpu);
    g_ram[io + IO_FLAGS] = 0;                    /* async — real reply path */
    cb_w16(g_ram, io + IO_COMMAND, CMD_READ);
    cb_w32(g_ram, io + IO_DATA, rd);
    cb_w32(g_ram, io + IO_LENGTH, 4);
    cb_w32(g_ram, io + IO_OFFSET, 0);
    cb_w32(g_ram, io + IO_CLIPID, 0);
    clipboard_BeginIO(&cpu);
    if (!(g_ram[io + IO_FLAGS] & UIOF_QUEUED)) {
        errors++; kprint("[clipdev] FAIL: read did not queue\n");
    }
    memcpy(g_ram + buf, "xy", 2);
    cb_w16(g_ram, io + IO_COMMAND, CMD_WRITE);   /* reuse same req block */
    cb_w32(g_ram, io + IO_DATA, buf);
    cb_w32(g_ram, io + IO_LENGTH, 2);
    cb_w32(g_ram, io + IO_OFFSET, 0);
    clipboard_BeginIO(&cpu);                      /* (completed, not queued) */
    cb_w16(g_ram, io + IO_COMMAND, CMD_UPDATE);
    clipboard_BeginIO(&cpu);                      /* publishes -> flush   */
    if (!(g_ram[io + IO_FLAGS] & UIOF_DONE) ||
        cb_r32(g_ram, io + IO_ACTUAL) != 2 ||
        memcmp(g_ram + rd, "xy", 2) != 0) {
        errors++; kprint("[clipdev] FAIL: queued read flush\n");
    }
    if (cb_r32(g_ram, port + MP_MSGLIST) != io) {
        errors++; kprint("[clipdev] FAIL: reply not on port\n");
    }

    /* AbortIO on a completed request reports not-found. */
    clipboard_AbortIO(&cpu);
    if ((int32_t)cpu.d[0] != -1) {
        errors++; kprint("[clipdev] FAIL: abort idle\n");
    }

    /* Host text API: FTXT wrap/unwrap. */
    ClipDev_SetText("hi", 2);
    char out[8];
    uint32_t got = ClipDev_GetText(out, sizeof(out));
    if (got != 2 || out[0] != 'h' || out[1] != 'i') {
        errors++; kprint("[clipdev] FAIL: host text api\n");
    }

    return errors == 0;
}
