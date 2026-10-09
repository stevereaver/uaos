/*
 * dos_lib.c — UAOS dos.library Implementation
 *
 * AmigaOS dos.library provides file system operations, process control,
 * and command-line interface functions.
 *
 * This implementation is dispatched via the ROM module system
 * (UAOS_ROM_NativeFunc) and receives M68kCPUState so it can be called
 * from any emulator backend without backend-specific APIs.
 */

#include "rom_modules.h"
#include "uaos_emu.h"
#include "dos/vfs.h"
#include "dos/handler.h"
#include "dos/handle_table.h"
#include "dos/dospacket.h"
#include "dos/amiga_dos_types.h"
#include "dos/dos_list.h"
#include "exec/task.h"
#include "exec/amiga_task.h"
#include "../dbg/diag.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "chipset/chip_emu.h"
#include "chipset/chiptrace.h"
#include "irq/rtc.h"
#include "irq/irq.h"
#include "net/ntp.h"
#include "klog/klog.h"

extern volatile uint64_t g_pit_ticks;
extern uint64_t g_m68k_cycles;
extern unsigned int m68k_cycles_run(void);

/* =========================================================================
 * Console output helpers
 * ========================================================================= */
extern void kprint(const char *s);

/* ---- Per-task cwd -------------------------------------------------------
 * The launcher sets the global g_uaos_cwd before Task_CreateM68k(), which
 * snapshots it into the task's task_cwd.  From then on a guest's
 * cd/CurrentDir only affects its own task — previously g_uaos_cwd was a
 * single global shared by every M68k task, so launching a second binary
 * (or one guest's cd) silently rewired the first task's relative paths.
 * Non-task callers (early boot, desktop launchers) fall back to the
 * global. */
const char *m68k_cur_cwd(void)
{
    extern UaosTask *Task_Current(void);
    UaosTask *cur = Task_Current();
    if (cur && cur->type == TASK_TYPE_M68K && cur->task_cwd[0])
        return cur->task_cwd;
    return g_uaos_cwd;
}

void m68k_set_cur_cwd(const char *path)
{
    extern UaosTask *Task_Current(void);
    UaosTask *cur = Task_Current();
    int is_task = (cur && cur->type == TASK_TYPE_M68K);
    char *dst = is_task ? cur->task_cwd : g_uaos_cwd;
    int   max = is_task ? (int)sizeof(cur->task_cwd) : (int)sizeof(g_uaos_cwd);
    int i = 0;
    while (i < max - 1 && path && path[i]) { dst[i] = path[i]; i++; }
    dst[i] = '\0';
}

/* =========================================================================
 * BSTR / path helpers
 * ========================================================================= */

/* Convert BSTR BPTR to native C string into dst[max].
 * Returns length or 0 if invalid. */
static int bstr_to_c(uint32_t bptr_bptr, char *dst, int max)
{
    uint32_t addr = bptr_bptr << 2;
    if (addr >= GUEST_RAM_SIZE || max < 2) return 0;
    uint8_t len = g_ram[addr];
    if (len > (uint8_t)(max - 1)) len = (uint8_t)(max - 1);
    for (int i = 0; i < (int)len; i++) dst[i] = (char)g_ram[addr + 1 + i];
    dst[len] = '\0';
    return (int)len;
}

/* Decode a dos.library string argument.  The real AmigaDOS ABI passes a
 * STRPTR (NUL-terminated C string) in D1; legacy UAOS guest callers passed
 * a BPTR to a BSTR.  Try C-string first — it is the ABI for real Amiga
 * binaries — and fall back to BSTR only when the C decode fails or the
 * bytes aren't printable. */
static int dos_arg_to_c(uint32_t a1, char *dst, int max)
{
    dst[0] = '\0';
    if (!a1 || a1 >= GUEST_RAM_SIZE || max < 2) return 0;
    int i = 0;
    while (i < max - 1 && a1 + i < GUEST_RAM_SIZE) {
        uint8_t c = g_ram[a1 + i];
        if (!c) break;
        dst[i++] = (char)c;
    }
    dst[i] = '\0';
    int ok = (i > 0);
    for (int j = 0; j < i; j++)
        if ((uint8_t)dst[j] < 0x20) { ok = 0; break; }
    if (ok) return i;
    return bstr_to_c(a1, dst, max);
}

/* Extract volume name from a path like "RAM:dir/file" into dst[max].
 * Returns length or 0 if no colon. */
static int extract_vol_name(const char *path, char *dst, int max)
{
    int i = 0;
    while (path[i] && path[i] != ':' && i < max - 1) { dst[i] = path[i]; i++; }
    dst[i] = '\0';
    return (path[i] == ':') ? i : 0;
}

/* Build an absolute path for a guest name: names without a device/assign
 * prefix are resolved against the task's cwd. */
static void dos_cwd_prefix(const char *name, char *out, int max)
{
    int has_device = 0;
    for (int i = 0; name[i]; i++) if (name[i] == ':') { has_device = 1; break; }
    int i = 0;
    if (has_device) {
        while (i < max - 1 && name[i]) { out[i] = name[i]; i++; }
    } else {
        const char *cwd = m68k_cur_cwd();
        int cwd_len = 0;
        while (cwd[cwd_len] && cwd_len < 63) cwd_len++;
        while (i < max - 1 && i < cwd_len) { out[i] = cwd[i]; i++; }
        if (cwd_len > 0 && cwd[cwd_len - 1] != ':' &&
            cwd[cwd_len - 1] != '/' && i < max - 1)
            out[i++] = '/';
        int j = 0;
        while (j < 127 && name[j] && i < max - 1) { out[i++] = name[j++]; }
    }
    out[i] = '\0';
}

/* Canonicalise a guest path for packet dispatch: cwd-prefix relative
 * names, then fully expand assigns ("LIBS:x" -> "Workbench:LIBS/x").
 * Handlers strip only the "VOL:" prefix, so dispatching an unresolved
 * "ASSIGN:dir/file" would silently lose the assign's directory part;
 * and a bare "ASSIGN:" would return the volume ROOT instead of the
 * assign's subdirectory (UAOS-248).
 *
 * "PROGDIR:" is a per-process pseudo-assign: it resolves to the current
 * task's program directory (the cwd when none was set). */
static uint32_t *program_dir_slot(void);
static uint32_t dos_program_dir_path(char *dst, int max);

static int dos_resolve_path(const char *name, char *out, int max)
{
    char full[128];
    dos_cwd_prefix(name, full, sizeof(full));

    char vol[16];
    int vl = extract_vol_name(full, vol, sizeof(vol));
    if (vl > 0 && vol[0] == 'P' &&
        (vol[1] == 'R' || vol[1] == 'r') &&
        (vol[2] == 'O' || vol[2] == 'o') &&
        (vol[3] == 'G' || vol[3] == 'g') &&
        (vol[4] == 'D' || vol[4] == 'd') &&
        (vol[5] == 'I' || vol[5] == 'i') &&
        (vol[6] == 'R' || vol[6] == 'r') && vol[7] == '\0') {
        /* "PROGDIR:rest" -> "<progdir>/rest" */
        char pdir[128];
        dos_program_dir_path(pdir, sizeof(pdir));
        int i = 0;
        while (i < max - 1 && pdir[i]) { out[i] = pdir[i]; i++; }
        const char *rest = full + vl + 1;
        if (*rest) {
            if (i < max - 1 && out[i - 1] != ':') out[i++] = '/';
            while (i < max - 1 && *rest) out[i++] = *rest++;
        }
        out[i] = '\0';
        return VFS_ResolveAssignPath(out, out, max) != NULL;
    }
    return VFS_ResolveAssignPath(full, out, max) != NULL;
}

static int guest_read_filelock(uint32_t lock_bptr,
                               uint32_t *out_handle, int32_t *out_access);
static uint32_t dos_lock_path(const char *path);
static int32_t dos_examine_pkt(MsgPort *port, int32_t action,
                               int32_t handle, uint32_t fib_guest);

/* Extract a FileLock BPTR's underlying handler lock entry.
 * Returns the HandleEntry or NULL. */
static HandleEntry *dos_lock_entry(uint32_t lock_bptr)
{
    uint32_t handle = 0;
    if (!guest_read_filelock(lock_bptr, &handle, NULL) || handle == 0)
        return NULL;
    HandleEntry *ent = HandleTable_GetLockEntry(handle, NULL);
    return (ent && ent->type == HTYPE_LOCK) ? ent : NULL;
}

/* Handler port for a lock entry (its path is canonical). */
static MsgPort *dos_lock_port(HandleEntry *ent)
{
    char vol[16];
    extract_vol_name(ent->path, vol, sizeof(vol));
    return VFS_GetHandlerPort(vol);
}

/* Dispatch an fh-based packet action.  Handler-backed files take their
 * native handle id; RAMFS files take the HandleTable id (the ram handler
 * resolves it via HandleTable_GetFile).  Returns dp_Res1. */
static int32_t dos_fh_dispatch(HandleEntry *ent, uint32_t fh,
                               int32_t action, intptr_t a2, intptr_t a3)
{
    VfsFile *vf = &ent->u.file.fh;
    MsgPort *port = vf->handler_port;
    int32_t arg1;
    if (port) {
        arg1 = (int32_t)vf->handle_id;
    } else {
        char vol[16];
        extract_vol_name(ent->path, vol, sizeof(vol));
        port = VFS_GetHandlerPort(vol);
        arg1 = (int32_t)fh;
    }
    if (!port) { SetIoErr(ERROR_DEVICE_NOT_MOUNTED); return DOSFALSE; }
    /* DoPkt() stores dp_Res2 in the global IoErr on the way back. */
    return DoPkt(port, action, arg1, a2, a3, 0, 0);
}

/* Expand "VOL:rest" through assign target `idx` of VOL — the target may
 * itself route through another assign, so the result is fully resolved.
 * Returns 1 on success with the resolved path in `out`. */
static int dos_expand_target(const char *vol, const char *rest, int idx,
                             char *out, int max)
{
    const char *target = VFS_GetAssignTarget(vol, idx);
    if (!target) return 0;
    int i = 0;
    while (i < max - 1 && target[i]) { out[i] = target[i]; i++; }
    if (*rest) {
        if (i < max - 1 && i > 0 && out[i - 1] != ':') out[i++] = '/';
        while (i < max - 1 && *rest) out[i++] = *rest++;
    }
    out[i] = '\0';
    return VFS_ResolveAssignPath(out, out, max) != NULL;
}

/* Dispatch a path-based packet action with multi-assign search semantics:
 * a name through a multi-target assign (Assign ... ADD) is tried against
 * every target in order — the same order VFS_Open and dos_Lock use — so
 * write/create requests fall through a read-only target to a writable
 * one (UAOS-248).  On success `hit`/`hit_port` (optional) receive the
 * winning resolved path and handler port. */
static int32_t dos_path_pkt(const char *name, int32_t action,
                            intptr_t a2, intptr_t a3,
                            char *hit, int hmax, MsgPort **hit_port)
{
    char full[128];
    dos_cwd_prefix(name, full, sizeof(full));

    char vol_name[16];
    int vl = extract_vol_name(full, vol_name, sizeof(vol_name));
    int targets = vl ? VFS_GetAssignTargetCount(vol_name) : 0;
    int ntry = targets > 0 ? targets : 1;
    int32_t res = DOSFALSE;
    char resolved[128];
    for (int t = 0; t < ntry; t++) {
        if (targets) {
            if (!dos_expand_target(vol_name, full + vl + 1, t,
                                   resolved, sizeof(resolved)))
                continue;
        } else if (!dos_resolve_path(name, resolved, sizeof(resolved))) {
            continue;
        }
        char rvol[16];
        if (!extract_vol_name(resolved, rvol, sizeof(rvol))) continue;
        MsgPort *port = VFS_GetHandlerPort(rvol);
        if (!port) continue;
        res = DoPkt(port, action, (intptr_t)resolved, a2, a3, 0, 0);
        if (res != DOSFALSE && res != 0) {
            if (hit) {
                int i = 0;
                while (i < hmax - 1 && resolved[i]) { hit[i] = resolved[i]; i++; }
                hit[i] = '\0';
            }
            if (hit_port) *hit_port = port;
            break;
        }
    }
    return res;
}

/* =========================================================================
 * Guest-visible FileLock helpers
 * ========================================================================= */

static void guest_write_be32(uint32_t addr, uint32_t val)
{
    if (addr > GUEST_RAM_SIZE - 4u) return;
    g_ram[addr + 0] = (uint8_t)(val >> 24);
    g_ram[addr + 1] = (uint8_t)(val >> 16);
    g_ram[addr + 2] = (uint8_t)(val >>  8);
    g_ram[addr + 3] = (uint8_t)(val      );
}

static uint32_t guest_read_be32(uint32_t addr)
{
    if (addr > GUEST_RAM_SIZE - 4u) return 0;
    return ((uint32_t)g_ram[addr + 0] << 24)
         | ((uint32_t)g_ram[addr + 1] << 16)
         | ((uint32_t)g_ram[addr + 2] <<  8)
         | ((uint32_t)g_ram[addr + 3]      );
}

/* =========================================================================
 * Guest heap — free-list allocator
 *
 * Each free block has an 8-byte header in guest RAM:
 *   [0..3] size of block (including header), big-endian uint32
 *   [4..7] BPTR to next free block (0 = end of list), big-endian uint32
 *
 * Allocated blocks have only the size word at [0..3] (magic bit 31 set).
 * The rest of the block is available to the caller.
 *
 * The free-list is built lazily: the first time we need to free we
 * initialise it to cover all RAM above the current bump pointer.
 * ========================================================================= */

/* AmigaOS exec.library AllocMem memory-type requirements */
#define MEMF_PUBLIC     0x00000001u
#define MEMF_CHIP       0x00000002u
#define MEMF_FAST       0x00000004u
#define MEMF_LOCAL      0x00000008u
#define MEMF_24BITDMA   0x00000010u
#define MEMF_DMA        0x00000010u
#define MEMF_CLEAR      0x00010000u
#define MEMF_EXPUNGE    0x80000000u

#define HEAP_HDR        8u           /* header size in bytes             */
#define HEAP_MAGIC      0x80000000u  /* marks an allocated block         */
#define HEAP_FREE_BASE  0x020000u    /* permanent start of free-list pool */

/* Guest RAM layout: 8 MB chip (0x000000–0x7FFFFF) + 8 MB fast (0x800000–0xFFFFFF).
 * The free-list pools keep a 64 KB guard at the top of each region. */
#define HEAP_CHIP_END   0x007F0000u
#define HEAP_FAST_START 0x00800000u
#define HEAP_FAST_END   0x00FF0000u
#define HEAP_ANY_END    HEAP_FAST_END

/* Address in guest RAM of the free-list heads (BPTR words).  They live in
 * the unused exception-vector band 0x10-0x1F — the previous home at
 * 0x200-0x20F collides with exec.library LVO stubs (EXEC_BASE-252/-258 =
 * 0x204/0x1FE) once every exec slot carries an ILLEGAL dispatch stub. */
#define HEAP_LIST_SLOT_CHIP 0x0010u
#define HEAP_LIST_SLOT_FAST 0x0014u
/* Init-sentinel slots: each M68k task gets its own guest RAM window that is
 * cleared on start, so readiness must live IN the window — a host-side flag
 * would leave later tasks with a zeroed head pointer and every AllocMem
 * returning NULL. */
#define HEAP_MAGIC_SLOT_CHIP 0x0018u
#define HEAP_MAGIC_SLOT_FAST 0x001Cu
#define HEAP_POOL_MAGIC      0xBEEFCAFEu

/* Host-side free-list head mirror, keyed by the bound guest RAM window.
 * The in-window head/magic words sit in the exception-vector band
 * (0x10-0x1F); a guest that pokes vectors clobbers them, and the next
 * AllocMem then re-initialized the pool as one giant free block —
 * handing out memory that was already allocated (this is how OctaMED's
 * decrunched image overwrote asl.library's jump table).  The live head
 * now lives host-side; the guest words remain only a compatibility
 * mirror. */
static struct {
    const uint8_t *win;
    uint32_t       chip_head;   /* BPTRs */
    uint32_t       fast_head;
    uint8_t        chip_init;
    uint8_t        fast_init;
} g_heap_heads[8];
static int g_heap_head_count;

static int heap_head_find(void)
{
    for (int i = 0; i < g_heap_head_count; i++)
        if (g_heap_heads[i].win == g_ram) return i;
    if (g_heap_head_count >= (int)(sizeof(g_heap_heads)/sizeof(g_heap_heads[0])))
        return -1;
    int i = g_heap_head_count++;
    g_heap_heads[i].win       = g_ram;
    g_heap_heads[i].chip_head = 0;
    g_heap_heads[i].fast_head = 0;
    g_heap_heads[i].chip_init = 0;
    g_heap_heads[i].fast_init = 0;
    return i;
}

/* Release the host-side free-list mirror for a guest RAM window when its
 * owning M68k task dies (UAOS-247).  Entries are keyed by window pointer,
 * so without this the next tenant of the slot inherits the dead task's
 * freelist head — its nodes then point into the fresh program image and
 * AllocMem hands out overlapping blocks with insane sizes. */
void UAOS_Heap_ReleaseWindow(uint8_t *ram)
{
    int w = 0;
    for (int i = 0; i < g_heap_head_count; i++)
        if (g_heap_heads[i].win != ram) g_heap_heads[w++] = g_heap_heads[i];
    g_heap_head_count = w;
}

static uint32_t heap_head_read(uint32_t list_slot)
{
    int i = heap_head_find();
    if (i >= 0) {
        int fast = (list_slot == HEAP_LIST_SLOT_FAST);
        uint8_t init = fast ? g_heap_heads[i].fast_init
                            : g_heap_heads[i].chip_init;
        if (init)
            return fast ? g_heap_heads[i].fast_head
                        : g_heap_heads[i].chip_head;
    }
    return guest_read_be32(list_slot);
}

static void heap_head_write(uint32_t list_slot, uint32_t bptr)
{
    int i = heap_head_find();
    if (i >= 0) {
        if (list_slot == HEAP_LIST_SLOT_FAST) {
            g_heap_heads[i].fast_head = bptr;
            g_heap_heads[i].fast_init = 1;
        } else {
            g_heap_heads[i].chip_head = bptr;
            g_heap_heads[i].chip_init = 1;
        }
    }
    guest_write_be32(list_slot, bptr);
}

static void heap_freelist_init_pool(uint32_t list_slot, uint32_t magic_slot,
                                    uint32_t pool_start, uint32_t pool_end)
{
    /* If this window's pool was already initialized, the host-side head is
     * authoritative — a guest write that clobbered the in-window head/magic
     * must not trigger a re-init (which would rebuild the pool as one giant
     * free block over live allocations). */
    int idx = heap_head_find();
    int fast = (list_slot == HEAP_LIST_SLOT_FAST);
    if (idx >= 0 && (fast ? g_heap_heads[idx].fast_init
                          : g_heap_heads[idx].chip_init)) {
        uint32_t head = heap_head_read(list_slot);
        if (guest_read_be32(magic_slot) != HEAP_POOL_MAGIC ||
            guest_read_be32(list_slot) != head) {
            extern void kprint(const char *);
            kprint("[heap] pool head/magic clobbered — restoring mirror\n");
            guest_write_be32(list_slot, head);
            guest_write_be32(magic_slot, HEAP_POOL_MAGIC);
        }
        return;
    }
    if (idx < 0 && guest_read_be32(magic_slot) == HEAP_POOL_MAGIC)
        return;  /* host table full — keep legacy in-window semantics */

    uint32_t start = pool_start;
    start = (start + 3u) & ~3u;
    if (start < HEAP_FREE_BASE) start = HEAP_FREE_BASE;
    if (start + HEAP_HDR >= pool_end) return; /* no room */

    uint32_t blk_size = pool_end - start;

    /* Write the single initial free block */
    guest_write_be32(start + 0, blk_size); /* size (no magic) */
    guest_write_be32(start + 4, 0);         /* next = NULL     */

    /* Store its BPTR in the head (host mirror + window), then arm magic */
    heap_head_write(list_slot, start >> 2);
    guest_write_be32(magic_slot, HEAP_POOL_MAGIC);
}

/* memcheck (UAOS-69) constants — defined early so heap_free_fl_pool can
 * poison freed blocks.  See the "memcheck" section below for details. */
#define MC_GUARD_WORD 0xC0FFEE42u   /* guard signature (stored big-endian) */
#define MC_FREE_SIG   0xFEEDFACEu   /* marks a poisoned free block         */
#define MC_POISON     0xABu         /* fill byte for freed block payloads  */
#define MC_ALLOC_FILL 0xDEu         /* fill byte for non-MEMF_CLEAR allocs */
#define MC_MAX_RECS   192

static int g_memcheck_on = 0;

static void heap_freelist_init(void)
{
    /* Chip pool covers the remaining bump-pointer space up to the 8 MB line. */
    heap_freelist_init_pool(HEAP_LIST_SLOT_CHIP, HEAP_MAGIC_SLOT_CHIP,
                            g_uaos_heap_ptr, HEAP_CHIP_END);
}

static void heap_freelist_init_fast(void)
{
    /* Fast pool covers the upper 8 MB of guest RAM. */
    heap_freelist_init_pool(HEAP_LIST_SLOT_FAST, HEAP_MAGIC_SLOT_FAST,
                            HEAP_FAST_START, HEAP_FAST_END);
}

/* Allocate 'size' bytes from a specific free list.  Returns guest addr or 0. */
static uint32_t heap_alloc_fl_pool(uint32_t size, uint32_t list_slot)
{
    size = (size + 3u) & ~3u;
    uint32_t need = size + HEAP_HDR;

    uint32_t prev_slot = list_slot;  /* address of the pointer to current */
    uint32_t cur_bptr  = heap_head_read(list_slot);

    while (cur_bptr) {
        uint32_t cur = cur_bptr << 2;
        uint32_t blk_size = guest_read_be32(cur + 0) & ~HEAP_MAGIC;
        uint32_t next_bptr = guest_read_be32(cur + 4);

        if (blk_size >= need) {
            /* Split if there's enough room for another free block */
            if (blk_size >= need + HEAP_HDR + 4u) {
                uint32_t rem = cur + need;
                uint32_t rem_size = blk_size - need;
                guest_write_be32(rem + 0, rem_size);
                guest_write_be32(rem + 4, next_bptr);
                /* Link split block in place of cur */
                if (prev_slot == list_slot)
                    heap_head_write(list_slot, rem >> 2);
                else
                    guest_write_be32(prev_slot, rem >> 2);
                /* The header must record the allocated span (need), not
                 * the original free-block size — otherwise a later free
                 * returns a block that overlaps the remainder still on
                 * the free list (heap corruption found via memcheck). */
                blk_size = need;
            } else {
                /* Use whole block — unlink cur */
                if (prev_slot == list_slot)
                    heap_head_write(list_slot, next_bptr);
                else
                    guest_write_be32(prev_slot, next_bptr);
            }

            /* Mark allocated */
            guest_write_be32(cur + 0, blk_size | HEAP_MAGIC);

            /* Zero only the allocated portion's payload, not the remainder block */
            for (uint32_t i = HEAP_HDR; i < need && cur + i < GUEST_RAM_SIZE; i++)
                g_ram[cur + i] = 0;

            return cur + HEAP_HDR;  /* return pointer past header */
        }

        prev_slot = cur + 4;
        cur_bptr  = next_bptr;
    }

    return 0; /* out of memory */
}

/* Free a block previously returned by heap_alloc_fl_pool.
 * Returns it to the front of the given free list. */
static void heap_free_fl_pool(uint32_t addr, uint32_t list_slot)
{
    if (addr < HEAP_HDR || addr >= GUEST_RAM_SIZE) return;

    uint32_t blk = addr - HEAP_HDR;
    uint32_t magic_size = guest_read_be32(blk + 0);
    if (!(magic_size & HEAP_MAGIC)) return;   /* not a valid alloc header */

    uint32_t blk_size = magic_size & ~HEAP_MAGIC;
    /* Sanity-check the size: a wild free landing mid-payload can see a
     * fake "allocated" header (e.g. memcheck poison 0xABABABAB) — without
     * a bound it would relink/poison a huge bogus block. */
    if (blk_size < HEAP_HDR + 4u || blk_size > GUEST_RAM_SIZE ||
        blk + blk_size > GUEST_RAM_SIZE) {
        KLOG(KLOG_EXEC, KLOG_WARN,
             "[heap] FreeMem(0x%x): implausible block size 0x%x — ignored\n",
             (unsigned)addr, (unsigned)blk_size);
        return;
    }
    guest_write_be32(blk + 0, blk_size);                        /* clear magic */
    guest_write_be32(blk + 4, heap_head_read(list_slot));      /* prepend     */
    heap_head_write(list_slot, blk >> 2);

    /* memcheck: sign + poison the freed payload so use-after-free writes
     * corrupt MC_POISON and are caught by the next free-list scan.  The
     * signature lets the scan distinguish poisoned blocks from blocks
     * freed while memcheck was off.  Bounded to keep free cheap. */
    if (g_memcheck_on && blk_size >= HEAP_HDR + 8u) {
        guest_write_be32(blk + HEAP_HDR, MC_FREE_SIG);
        uint32_t n = blk_size - HEAP_HDR - 4u;
        if (n > 262144u) n = 262144u;
        for (uint32_t i = 0; i < n && blk + HEAP_HDR + 4 + i < GUEST_RAM_SIZE; i++)
            g_ram[blk + HEAP_HDR + 4 + i] = MC_POISON;
    }
}

/* Allocate 'size' bytes from the chip free list. */
static uint32_t heap_alloc_fl_chip(uint32_t size)
{
    heap_freelist_init();
    return heap_alloc_fl_pool(size, HEAP_LIST_SLOT_CHIP);
}

/* Allocate 'size' bytes from the fast free list. */
static uint32_t heap_alloc_fl_fast(uint32_t size)
{
    heap_freelist_init_fast();
    return heap_alloc_fl_pool(size, HEAP_LIST_SLOT_FAST);
}

/* Legacy single-pool allocator: defaults to fast with chip fallback.
 * Preserves old callers until they are updated to pass requirements. */
static uint32_t heap_alloc_fl(uint32_t size)
{
    uint32_t addr = heap_alloc_fl_fast(size);
    if (!addr) addr = heap_alloc_fl_chip(size);
    return addr;
}

/* Free a block previously returned by any heap_alloc_fl_* variant.
 * Returns it to the correct pool based on its guest address. */
static void heap_free_fl(uint32_t addr)
{
    if (addr >= HEAP_FAST_START)
        heap_free_fl_pool(addr, HEAP_LIST_SLOT_FAST);
    else
        heap_free_fl_pool(addr, HEAP_LIST_SLOT_CHIP);
}

/* =========================================================================
 * memcheck — Mungwall-style heap debugging (UAOS-69)
 *
 * When enabled, exec.library AllocMem adds a 4-byte guard word before and
 * after the payload, poison-fills freed blocks, and records every live
 * allocation (addr/size/allocating task) in a host-side table.
 *
 *   - FreeMem validates both guard words and reports the culprit task.
 *   - Freeing an untracked address is reported (double-free / wild free).
 *   - Memcheck_Scan() walks live allocations and both free lists,
 *     reporting corrupted guard bands and modified free blocks.
 *
 * Each M68k task has a private guest-RAM window and g_ram is rebound on
 * every context switch, so guest addresses are only meaningful relative
 * to the window they were allocated in.  Every record therefore carries
 * its RAM window, and all checks/frees run against that window rather
 * than the caller's current g_ram binding (UAOS-266: scanning from the
 * shell otherwise reads foreign memory and reports every live M68k
 * block as FRONT+TAIL corrupt).
 * ========================================================================= */

typedef struct {
    uint32_t payload;   /* guest address returned to the caller */
    uint32_t size;      /* requested payload size               */
    uint32_t blk;       /* block base: guest addr of heap header */
    void    *owner;     /* allocating task (Task_Current())      */
    uint8_t *ram;       /* guest-RAM window the block lives in   */
    char     task[20];  /* allocating task name                  */
} MemchkRec;

static MemchkRec g_mc[MC_MAX_RECS];

static const char *mc_task_name(void)
{
    extern UaosTask *Task_Current(void);
    UaosTask *t = Task_Current();
    if (t && t->ln_Name) return t->ln_Name;
    return "(boot/kernel)";
}

static void mc_record(uint32_t payload, uint32_t size, uint32_t blk)
{
    extern UaosTask *Task_Current(void);
    for (int i = 0; i < MC_MAX_RECS; i++) {
        if (!g_mc[i].payload) {
            const char *n = mc_task_name();
            g_mc[i].payload = payload;
            g_mc[i].size    = size;
            g_mc[i].blk     = blk;
            g_mc[i].owner   = Task_Current();
            g_mc[i].ram     = g_ram;
            int j = 0;
            while (n[j] && j < 19) { g_mc[i].task[j] = n[j]; j++; }
            g_mc[i].task[j] = '\0';
            return;
        }
    }
    KLOG(KLOG_EXEC, KLOG_WARN, "[memchk] tracking table full — %u-byte alloc at 0x%x not tracked\n",
         (unsigned)size, (unsigned)payload);
}

static MemchkRec *mc_find(uint32_t payload)
{
    /* Guest addresses are only unique within a RAM window — prefer a
     * record in the caller's current window so a wild free can't alias a
     * different task's block that happens to sit at the same address. */
    for (int i = 0; i < MC_MAX_RECS; i++)
        if (g_mc[i].payload == payload && g_mc[i].ram == g_ram)
            return &g_mc[i];
    /* Native teardown paths can free a pointer owned by another guest
     * window; fall back to a cross-window match so the free lands in the
     * window the block was actually allocated from. */
    for (int i = 0; i < MC_MAX_RECS; i++)
        if (g_mc[i].payload == payload)
            return &g_mc[i];
    return 0;
}

/* Guest dword read against an explicit RAM window — a record may live in
 * a different task's private g_ram than the caller's current binding. */
static uint32_t mc_rd32(const uint8_t *ram, uint32_t addr)
{
    return ((uint32_t)ram[addr + 0] << 24)
         | ((uint32_t)ram[addr + 1] << 16)
         | ((uint32_t)ram[addr + 2] <<  8)
         | ((uint32_t)ram[addr + 3]      );
}

static int mc_check_guards(const MemchkRec *r, const char *what)
{
    int bad = 0;
    const uint8_t *ram = r->ram ? r->ram : g_ram;
    if (mc_rd32(ram, r->payload - 4) != MC_GUARD_WORD) {
        KLOG(KLOG_EXEC, KLOG_ERR,
             "[memchk] %s block @0x%x (%u bytes, alloc by '%s'): FRONT guard corrupted\n",
             what, (unsigned)r->payload, (unsigned)r->size, r->task);
        bad = 1;
    }
    if (mc_rd32(ram, r->payload + r->size) != MC_GUARD_WORD) {
        KLOG(KLOG_EXEC, KLOG_ERR,
             "[memchk] %s block @0x%x (%u bytes, alloc by '%s'): TAIL guard corrupted\n",
             what, (unsigned)r->payload, (unsigned)r->size, r->task);
        bad = 1;
    }
    return bad;
}

static uint32_t mc_alloc(uint32_t size, uint32_t reqs)
{
    /* Fault injection (C:failalloc, UAOS-211): deterministic failures
     * exercise unchecked-NULL bugs.  Inert unless enabled. */
    if (Failalloc_ShouldFail(FAILALLOC_GUEST)) return 0;

    if (!g_memcheck_on) {
        if (reqs & (MEMF_CHIP | MEMF_DMA | MEMF_24BITDMA))
            return heap_alloc_fl_chip(size);
        if (reqs & MEMF_FAST)
            return heap_alloc_fl_fast(size);
        uint32_t a = heap_alloc_fl_fast(size);
        if (!a) a = heap_alloc_fl_chip(size);
        return a;
    }

    uint32_t raw = 0;
    if (reqs & (MEMF_CHIP | MEMF_DMA | MEMF_24BITDMA))
        raw = heap_alloc_fl_chip(size + 8);
    else if (reqs & MEMF_FAST)
        raw = heap_alloc_fl_fast(size + 8);
    else {
        raw = heap_alloc_fl_fast(size + 8);
        if (!raw) raw = heap_alloc_fl_chip(size + 8);
    }
    if (!raw) return 0;

    /* The pool already zeroed the block; write guards over the slack. */
    guest_write_be32(raw + 0, MC_GUARD_WORD);            /* front guard  */
    guest_write_be32(raw + 4 + size, MC_GUARD_WORD);     /* tail guard   */
    if (!(reqs & MEMF_CLEAR)) {
        for (uint32_t i = 0; i < size && raw + 4 + i < GUEST_RAM_SIZE; i++)
            g_ram[raw + 4 + i] = MC_ALLOC_FILL;
    }
    mc_record(raw + 4, size, raw - HEAP_HDR);
    return raw + 4;
}

static void mc_free(uint32_t addr)
{
    if (!g_memcheck_on) { heap_free_fl(addr); return; }

    MemchkRec *r = mc_find(addr);
    if (!r) {
        KLOG(KLOG_EXEC, KLOG_ERR,
             "[memchk] FreeMem(0x%x) by '%s': not a tracked allocation (wild/double free?)\n",
             (unsigned)addr, mc_task_name());
        /* Still attempt the free — pre-enable allocations are untracked
         * but valid.  The pool sanity check rejects bogus headers. */
        heap_free_fl(addr);
        return;
    }
    /* The block lives in its allocator's RAM window, which may not be
     * the caller's current g_ram binding (native teardown paths freeing
     * on a guest's behalf).  Rebind g_ram IRQ-off so the guard check and
     * the freelist free both target the owning window. */
    {
        uint64_t flags = irq_save();
        uint8_t *saved = g_ram;
        if (r->ram) g_ram = r->ram;
        mc_check_guards(r, "freed");
        heap_free_fl(r->payload - 4);   /* gross payload → real heap header */
        g_ram = saved;
        irq_restore(flags);
    }
    r->payload = 0;                 /* drop tracking */
}

/* Public API for C:memcheck ------------------------------------------------ */

int Memcheck_IsEnabled(void)          { return g_memcheck_on; }

/* Free every tracked allocation owned by `owner` (called from Task_Exit).
 * A task that dies or is aborted (e.g. the M68k cycle-budget kill) leaves
 * its AllocMem blocks allocated; without this sweep they'd sit in the
 * tracking table as "live" forever — and when the guest RAM slot is
 * recycled, stale records can alias a new task's allocations.  Each
 * record carries its RAM window, so the sweep frees into the right heap
 * no matter which window is currently bound to g_ram. */
uint32_t Memcheck_FreeByOwner(void *owner)
{
    if (!owner) return 0;
    uint64_t flags = irq_save();
    uint8_t *saved = g_ram;
    uint32_t freed = 0;
    for (int i = 0; i < MC_MAX_RECS; i++) {
        if (g_mc[i].payload && g_mc[i].owner == owner) {
            if (g_mc[i].ram) g_ram = g_mc[i].ram;
            mc_check_guards(&g_mc[i], "task-exit");
            heap_free_fl(g_mc[i].payload - 4);
            g_mc[i].payload = 0;
            freed++;
        }
    }
    g_ram = saved;
    irq_restore(flags);
    if (freed) {
        KLOG(KLOG_EXEC, KLOG_INFO,
             "[memchk] task exit: reclaimed %u tracked alloc(s)\n", (unsigned)freed);
    }
    return freed;
}

void Memcheck_SetEnabled(int on)
{
    g_memcheck_on = !!on;
    KLOG(KLOG_EXEC, KLOG_INFO, "[memchk] %s\n", on ? "enabled" : "disabled");
}

int Memcheck_LiveCount(void)
{
    int n = 0;
    for (int i = 0; i < MC_MAX_RECS; i++)
        if (g_mc[i].payload) n++;
    return n;
}

/* Walk one free list verifying block structure and (when enabled) poison. */
static uint32_t mc_scan_freelist(uint32_t list_slot, const char *pool_name)
{
    uint32_t bad = 0;
    uint32_t bptr = heap_head_read(list_slot);
    int guard = 8192;   /* loop bound against corrupt next pointers */
    while (bptr && guard-- > 0) {
        uint32_t cur = bptr << 2;
        if (cur + HEAP_HDR > GUEST_RAM_SIZE) {
            KLOG(KLOG_EXEC, KLOG_ERR,
                 "[memchk] %s free list: next ptr 0x%x out of range\n",
                 pool_name, (unsigned)cur);
            bad++; break;
        }
        uint32_t hdr = guest_read_be32(cur + 0);
        if (hdr & HEAP_MAGIC) {
            KLOG(KLOG_EXEC, KLOG_ERR,
                 "[memchk] %s free list: block @0x%x still marked allocated\n",
                 pool_name, (unsigned)cur);
            bad++;
        }
        uint32_t blk_size = hdr & ~HEAP_MAGIC;
        /* Only verify poison on blocks we poisoned (MC_FREE_SIG at the
         * first payload dword).  Blocks freed while memcheck was off are
         * skipped — otherwise they'd be false positives. */
        if (blk_size >= HEAP_HDR + 8u &&
            guest_read_be32(cur + HEAP_HDR) == MC_FREE_SIG) {
            uint32_t n = blk_size - HEAP_HDR - 4u;
            if (n > 64) n = 64;
            for (uint32_t i = 0; i < n && cur + HEAP_HDR + 4 + i < GUEST_RAM_SIZE; i++) {
                if (g_ram[cur + HEAP_HDR + 4 + i] != MC_POISON) {
                    KLOG(KLOG_EXEC, KLOG_ERR,
                         "[memchk] %s free block @0x%x modified while free (off +%u)\n",
                         pool_name, (unsigned)cur, (unsigned)(4 + i));
                    bad++;
                    break;
                }
            }
        }
        bptr = guest_read_be32(cur + 4);
    }
    if (guard <= 0) {
        KLOG(KLOG_EXEC, KLOG_ERR, "[memchk] %s free list: corrupt (loop)\n", pool_name);
        bad++;
    }
    return bad;
}

/* Build a pool label like "fast[2]" so multi-window scan output says
 * which guest-RAM window a violation belongs to. */
static void mc_pool_name(char *out, const char *base, int idx)
{
    int i = 0;
    while (base[i]) { out[i] = base[i]; i++; }
    out[i++] = '[';
    out[i++] = (char)('0' + idx);
    out[i++] = ']';
    out[i] = '\0';
}

/* Scan live tracked allocs + every window's free lists.  Returns
 * violation count.  Runs IRQ-off so tasks can't alloc/free mid-walk —
 * otherwise a transient split-block header looks like corruption.
 * Free lists and guard bands live inside each guest-RAM window, so the
 * walk rebinds g_ram per window; records carry their window so live
 * blocks check correctly regardless of the caller's binding (UAOS-266). */
uint32_t Memcheck_Scan(void)
{
    uint64_t flags = irq_save();
    uint8_t *saved = g_ram;

    uint32_t bad = 0;
    for (int i = 0; i < MC_MAX_RECS; i++)
        if (g_mc[i].payload)
            bad += mc_check_guards(&g_mc[i], "live");

    int scanned_current = 0;
    for (int w = 0; w < g_heap_head_count; w++) {
        char nm[16];
        if (g_heap_heads[w].win == (const uint8_t *)saved)
            scanned_current = 1;
        g_ram = (uint8_t *)g_heap_heads[w].win;
        mc_pool_name(nm, "chip", w);
        bad += mc_scan_freelist(HEAP_LIST_SLOT_CHIP, nm);
        mc_pool_name(nm, "fast", w);
        bad += mc_scan_freelist(HEAP_LIST_SLOT_FAST, nm);
    }
    /* The caller's window may have no registered pools yet — scan it too. */
    if (!scanned_current && saved) {
        g_ram = saved;
        bad += mc_scan_freelist(HEAP_LIST_SLOT_CHIP, "chip");
        bad += mc_scan_freelist(HEAP_LIST_SLOT_FAST, "fast");
    }

    g_ram = saved;
    irq_restore(flags);
    return bad;
}

/* Debug: dump one free list's chain via emit() (for C:memcheck dump). */
static void mc_hex32(uint32_t v, char *out)
{
    const char *h = "0123456789ABCDEF";
    out[0] = '0'; out[1] = 'x';
    for (int s = 28; s >= 0; s -= 4)
        out[2 + (28 - s) / 4] = h[(v >> s) & 0xF];
    out[10] = '\0';
}

void Memcheck_DumpList(uint32_t which, void (*emit)(const char *line))
{
    char buf[80], a[11], b[11], c[11];
    uint32_t list_slot = which ? HEAP_LIST_SLOT_FAST : HEAP_LIST_SLOT_CHIP;
    uint32_t bptr = guest_read_be32(list_slot);
    int guard = 32;
    while (bptr && guard-- > 0) {
        uint32_t cur = bptr << 2;
        uint32_t sz = 0, nx = 0;
        if (cur + 8 <= GUEST_RAM_SIZE) {
            sz = guest_read_be32(cur + 0);
            nx = guest_read_be32(cur + 4);
        }
        mc_hex32(cur, a); mc_hex32(sz, b); mc_hex32(nx, c);
        /* "  blk 0x........  size 0x........  next 0x........" */
        int l = 0;
        const char *p1 = "  blk ", *p2 = "  size ", *p3 = "  next ";
        while (*p1) buf[l++] = *p1++;
        for (int i = 0; a[i]; i++) buf[l++] = a[i];
        while (*p2) buf[l++] = *p2++;
        for (int i = 0; b[i]; i++) buf[l++] = b[i];
        while (*p3) buf[l++] = *p3++;
        for (int i = 0; c[i]; i++) buf[l++] = c[i];
        buf[l] = '\0';
        emit(buf);
        bptr = nx;
    }
}

/* Self-test: deliberate tail overwrite of a tracked alloc.  Returns the
 * number of violations the scan reports (>=1 when working).  The block is
 * freed afterwards; the free path reports the clobber too. */
uint32_t Memcheck_SelfTest(void)
{
    int was_on = g_memcheck_on;
    if (!was_on) g_memcheck_on = 1;

    uint32_t addr = mc_alloc(64, MEMF_FAST | MEMF_CLEAR);
    if (!addr) { g_memcheck_on = was_on; return 0; }
    guest_write_be32(addr + 64, 0x12345678);   /* clobber tail guard */

    uint32_t bad = Memcheck_Scan();
    KLOG(KLOG_EXEC, KLOG_INFO,
         "[memchk] self-test: deliberate overwrite at 0x%x+64, scan reported %u violation(s)\n",
         (unsigned)addr, (unsigned)bad);

    mc_free(addr);
    g_memcheck_on = was_on;
    return bad;
}

/* Legacy bump allocator — still used for internal structures that are never
 * freed (FileLocks, library tables, etc.) and for the initial program load.
 * Program hunks must live in chip RAM, so cap the bump pointer there. */
static uint32_t heap_alloc(uint32_t size)
{
    size = (size + 3) & ~3u;
    if (g_uaos_heap_ptr + size > HEAP_CHIP_END) return 0;
    uint32_t addr = g_uaos_heap_ptr;
    g_uaos_heap_ptr += size;
    for (uint32_t i = 0; i < size; i++) g_ram[addr + i] = 0;
    return addr;
}

/* =========================================================================
 * exec.library — AllocMem / FreeMem
 * These are the high-level dos_lib entry points called from the ROM module
 * dispatcher when M68k code calls exec.library AllocMem/FreeMem via the
 * extended ILLEGAL dispatch (lib == LIB_EXEC, fn == EXEC_ALLOC_MEM /
 * EXEC_FREE_MEM).  They are also exported to dos.library callers that need
 * to dynamically allocate guest memory from within C code.
 * ========================================================================= */

static void dos_AllocMem(M68kCPUState *cpu)
{
    /* AmigaOS: D0=byteSize, D1=requirements → D0=APTR or NULL */
    uint32_t size = cpu->d[0];
    uint32_t reqs = cpu->d[1];
    if (size == 0) { cpu->d[0] = 0; return; }

    uint32_t addr = mc_alloc(size, reqs);

    if (!addr) SetIoErr(ERROR_NO_FREE_STORE);
    cpu->d[0] = addr;
}

static void dos_FreeMem(M68kCPUState *cpu)
{
    /* AmigaOS: A1=memoryBlock, D0=byteSize */
    uint32_t addr = cpu->a[1];
    mc_free(addr);
}

/* Allocate a FileLock in guest RAM and return its BPTR */
static uint32_t guest_alloc_filelock(uint32_t handle, int32_t access)
{
    uint32_t addr = heap_alloc(16);
    if (!addr) return 0;
    guest_write_be32(addr + 0, handle);
    guest_write_be32(addr + 4, (uint32_t)access);
    guest_write_be32(addr + 8, 1);
    guest_write_be32(addr + 12, 0);
    return addr >> 2;
}

/* Read a FileLock from guest RAM.  Returns 0 if lock_bptr invalid. */
static int guest_read_filelock(uint32_t lock_bptr,
                                uint32_t *out_handle,
                                int32_t  *out_access)
{
    uint32_t addr = lock_bptr << 2;
    if (addr + 16 > GUEST_RAM_SIZE) return 0;
    if (out_handle) *out_handle = guest_read_be32(addr + 0);
    if (out_access) *out_access = (int32_t)guest_read_be32(addr + 4);
    return 1;
}

/* =========================================================================
 * Fake file handle BPTRs
 * ========================================================================= */
#define FAKE_STDOUT_ADDR   0x0400   /* below the exec stub floor (0xC1C) */
#define FAKE_STDIN_ADDR    0x0404
#define DOS_STDOUT_BPTR    (FAKE_STDOUT_ADDR >> 2)
#define DOS_STDIN_BPTR     (FAKE_STDIN_ADDR  >> 2)

/* Guest library base addresses (must match uaos_m68k_glue.c, UAOS-252) */
#define EXEC_BASE_GLUE   0x1000u
#define DOS_BASE_GLOBVEC 0x2000u
/* LVO_DOS_EXIT stub address — pushed as the return PC on a child's stack
 * so an RTS at the end of a process lands on Exit(). */
#define DOS_EXIT_STUB    (DOS_BASE_GLOBVEC + (uint32_t)(-144))

/* =========================================================================
 * dos.library function implementations
 * All functions receive M68kCPUState* so they are backend-agnostic.
 * ========================================================================= */

static void dos_Output(M68kCPUState *cpu)
{
    cpu->d[0] = DOS_STDOUT_BPTR;
}

static void dos_Input(M68kCPUState *cpu)
{
    cpu->d[0] = DOS_STDIN_BPTR;
}

static void dos_VFPrintf(M68kCPUState *cpu)
{
    uint32_t fh  = cpu->d[1];
    uint32_t fmt = cpu->a[0];
    uint32_t arr = cpu->a[1];
    (void)fh; (void)arr;
    if (fmt < GUEST_RAM_SIZE) {
        char tmp[256];
        int i = 0;
        while (i < 255 && fmt + i < GUEST_RAM_SIZE && g_ram[fmt + i]) {
            tmp[i] = (char)g_ram[fmt + i]; i++;
        }
        tmp[i] = '\0';
        if (g_print) g_print(tmp);
        else kprint(tmp);
    }
    cpu->d[0] = 0;
}

static void dos_FPuts(M68kCPUState *cpu)
{
    /* FPuts(fh=D1, str=D2): write the NUL-terminated string to the fh.
     * Console handles still print; real files get a VFS_Write. */
    uint32_t fh = cpu->d[1];
    uint32_t sp = cpu->d[2];
    cpu->d[0] = 0;
    if (sp >= GUEST_RAM_SIZE) { SetIoErr(ERROR_OBJECT_NOT_FOUND); return; }
    uint32_t len = 0;
    while (sp + len < GUEST_RAM_SIZE && g_ram[sp + len] && len < 65536) len++;
    HandleEntry *ent = HandleTable_Get(fh);
    if (ent && ent->type == HTYPE_FILE) {
        int32_t n = VFS_Write(&ent->u.file.fh, g_ram + sp, len);
        cpu->d[0] = (n == (int32_t)len) ? DOSTRUE : DOSFALSE;
        if (n != (int32_t)len) SetIoErr(ERROR_WRITE_PROTECTED);
        return;
    }
    /* stdout/stderr console */
    char tmp[256];
    uint32_t i;
    for (i = 0; i < len && i < 255; i++) tmp[i] = (char)g_ram[sp + i];
    tmp[i] = '\0';
    if (g_print) g_print(tmp);
    else kprint(tmp);
    cpu->d[0] = DOSTRUE;
}

static void dos_PutStr(M68kCPUState *cpu)
{
    uint32_t sp = cpu->d[1];
    if (sp < GUEST_RAM_SIZE) {
        char tmp[256];
        int i = 0;
        while (i < 255 && sp + i < GUEST_RAM_SIZE && g_ram[sp + i]) {
            tmp[i] = (char)g_ram[sp + i]; i++;
        }
        tmp[i] = '\0';
        if (g_print) g_print(tmp);
        else kprint(tmp);
    }
    cpu->d[0] = 0;
}

static void dos_VPrintf(M68kCPUState *cpu)
{
    uint32_t fmt = cpu->d[1];
    uint32_t arr = cpu->d[2];
    (void)arr;
    if (fmt < GUEST_RAM_SIZE) {
        char tmp[256];
        int i = 0;
        while (i < 255 && fmt + i < GUEST_RAM_SIZE && g_ram[fmt + i]) {
            tmp[i] = (char)g_ram[fmt + i]; i++;
        }
        tmp[i] = '\0';
        if (g_print) g_print(tmp);
        else kprint(tmp);
    }
    cpu->d[0] = 0;
}

static void dos_VFWritef(M68kCPUState *cpu)
{
    uint32_t fmt = cpu->d[2];
    char tmp[256];
    dos_arg_to_c(fmt, tmp, sizeof(tmp));
    kprint(tmp);
    cpu->d[0] = 0;
}

static void dos_ReadArgs(M68kCPUState *cpu)
{
    (void)cpu;
    cpu->d[0] = 1;
}

static void dos_GetArgStr(M68kCPUState *cpu)
{
    cpu->d[0] = 0;
}

static void dos_IsInteractive(M68kCPUState *cpu)
{
    uint32_t fh = cpu->d[1];
    if (fh == DOS_STDOUT_BPTR || fh == DOS_STDIN_BPTR) {
        cpu->d[0] = (uint32_t)DOSTRUE;
    } else {
        cpu->d[0] = (uint32_t)DOSFALSE;
    }
}

/* ---- Sequential child-process model ------------------------------------
 * AmigaOS CreateProc() spawns a process that SHARES the parent's address
 * space — OctaMED's startup does exactly this (the parent installs a small
 * trampoline seglist then Exit()s, and the child reads the parent's
 * Process/globals).  UAOS per-task guests each get a private 16 MB guest
 * RAM window, so spawning a separate M68k host task would run the child in
 * an empty space.  Instead CreateProc queues the child and Exit() respawns
 * THIS m68k context at the child's entry — the same sequential model the
 * host-side Musashi harness proved out against real OctaMED startup.
 */
#define MAX_PENDING_PROC 8
typedef struct {
    uint32_t entry;      /* guest PC at start */
    uint32_t stack_top;  /* initial SP (with Exit-stub return pushed) */
    uint32_t proc;       /* guest Process struct */
    uint32_t proc_port;  /* proc + PR_MSGPORT — CreateProc return value */
} PendingProc;
static PendingProc g_pending_procs[MAX_PENDING_PROC];
static int         g_pending_head = 0, g_pending_tail = 0;

extern uint32_t g_guest_proc_addr;   /* uaos_m68k_glue.c — FindTask(NULL) */

static void dos_Exit(M68kCPUState *cpu)
{
    /* If a CreateProc'd child is queued, respawn this context at its
     * entry instead of halting — the parent has finished its job. */
    if (g_pending_head != g_pending_tail) {
        PendingProc *pp = &g_pending_procs[g_pending_head];
        g_pending_head = (g_pending_head + 1) % MAX_PENDING_PROC;

        guest_write_be32(EXEC_BASE_GLUE + EXECBASE_THIS_TASK, pp->proc);
        g_guest_proc_addr = pp->proc;

        cpu->pc    = pp->entry;
        cpu->a[7]  = pp->stack_top;
        cpu->a[6]  = EXEC_BASE_GLUE;
        cpu->d[0]  = 0;
        cpu->a[0]  = 0;
        kprint("[dos] Exit: spawning queued process\n");
        /* The launcher process ends here; on real AmigaOS the shell's
         * Wait() on the launched process would return now — the queued
         * child runs detached.  The UAOS host task respawns in place, so
         * without this signal the launching shell stays parked in
         * Wait(SIGF_CHILD) forever (foreground apps look like they hung).
         * The final Task_Exit fires SIGF_CHILD again — harmless: shells
         * clear stale SIGF_CHILD before each launch wait. */
        {
            extern UaosTask *Task_Current(void);
            UaosTask *cur = Task_Current();
            if (cur && cur->parent && cur->parent->tc_State != TASK_REMOVED) {
                extern void Signal(UaosTask *t, uint32_t sigs);
                Signal(cur->parent, SIGF_CHILD);
                cur->parent = NULL;  /* detached: don't signal again at exit */
            }
        }
        return;   /* g_emu_halted stays 0 — m68k_execute continues */
    }
    (void)cpu;
    g_emu_halted = 1;
}

static void dos_IoErr(M68kCPUState *cpu)
{
    cpu->d[0] = (uint32_t)IoErr();
}

/* Fault(code=d1, header=d2, buffer=d3, len=d4): format a standard IoErr
 * message as "<header>: <text>" (or just "<text>" when header is NULL),
 * NUL-terminated and clipped to len bytes.  Returns TRUE when code != 0. */
static void dos_Fault(M68kCPUState *cpu)
{
    uint32_t code = cpu->d[1];
    uint32_t hdrp = cpu->d[2];
    uint32_t buf  = cpu->d[3];
    uint32_t len  = cpu->d[4];
    if (!buf || !len || code == 0) { cpu->d[0] = 0; return; }

    static const struct { int code; const char *text; } tab[] = {
        { 103, "not enough memory available" },
        { 105, "task table full" },
        { 114, "bad template" },
        { 115, "bad number" },
        { 116, "required argument missing" },
        { 117, "keyword needs argument" },
        { 118, "too many arguments" },
        { 119, "unmatched quotes" },
        { 120, "line too long" },
        { 121, "file is not an object" },
        { 122, "invalid resident library" },
        { 123, "invalid directory" },
        { 202, "object is in use" },
        { 203, "object already exists" },
        { 204, "directory not found" },
        { 205, "object not found" },
        { 206, "bad stream name" },
        { 207, "object too large" },
        { 209, "action not known" },
        { 210, "invalid component name" },
        { 212, "object is not of required type" },
        { 215, "device not mounted" },
        { 216, "not a DOS disk" },
        { 218, "seek error" },
        { 219, "comment too big" },
        { 220, "disk is full" },
        { 221, "disk is full" },
        { 222, "disk is write-protected" },
        { 223, "file is protected against deletion" },
        { 224, "file is protected against writing" },
        { 225, "no disk in drive" },
        { 226, "file is protected against reading" },
        { 232, "not a valid DOS file" },
        { 234, "bad hunk" },
        { 235, "not implemented" },
    };

    const char *text = NULL;
    for (unsigned i = 0; i < sizeof(tab)/sizeof(tab[0]); i++)
        if ((int)code == tab[i].code) { text = tab[i].text; break; }

    char num[32];
    if (!text) {
        char *p = num;
        const char *pfx = "error code ";
        while (*pfx) *p++ = *pfx++;
        uint32_t v = code;
        char d[10]; int nd = 0;
        do { d[nd++] = (char)('0' + v % 10); v /= 10; } while (v && nd < 10);
        while (nd) *p++ = d[--nd];
        *p = '\0';
        text = num;
    }

    uint32_t w = 0;
    if (hdrp) {
        uint8_t c;
        while (w + 1 < len && (c = g_ram[hdrp++]) != 0) g_ram[buf + w++] = c;
        if (w && w + 2 < len) { g_ram[buf + w++] = ':'; g_ram[buf + w++] = ' '; }
    }
    while (w + 1 < len && *text) g_ram[buf + w++] = (uint8_t)*text++;
    g_ram[buf + w] = 0;
    cpu->d[0] = 1;
    KLOG(KLOG_DOS, KLOG_INFO, "[dos] Fault(%ld) -> '%s'\n", (long)code, (char *)g_ram + buf);
}


/* =========================================================================
 * Packet-based file operations (the core DOS logic)
 * ========================================================================= */

static void dos_Open(M68kCPUState *cpu)
{
    uint32_t bptr = cpu->d[1];
    char name[128];
    int blen = dos_arg_to_c(bptr, name, sizeof(name));
    if (blen == 0) {
        cpu->d[0] = DOS_STDOUT_BPTR;
        return;
    }
    if (name[0] == '*' ||
        (name[0]=='C' && name[1]=='O' && name[2]=='N') ||
        (name[0]=='N' && name[1]=='I' && name[2]=='L') ||
        (name[0]=='R' && name[1]=='A' && name[2]=='W') ||
        (name[0]=='A' && name[1]=='U' && name[2]=='X')) {
        cpu->d[0] = DOS_STDOUT_BPTR;
        return;
    }

    char full_name[128];
    int has_device = 0;
    for (int i = 0; i < blen; i++) if (name[i] == ':') { has_device = 1; break; }
    if (has_device) {
        int i = 0; while (i < blen) { full_name[i] = name[i]; i++; }
        full_name[i] = '\0';
    } else {
        const char *cwd = m68k_cur_cwd();
        int cwd_len = 0; while (cwd[cwd_len] && cwd_len < 63) cwd_len++;
        int i = 0; while (i < cwd_len) { full_name[i] = cwd[i]; i++; }
        if (cwd_len > 0 && cwd[cwd_len-1] != ':' && cwd[cwd_len-1] != '/')
            full_name[i++] = '/';
        int j = 0; while (j < blen && i < 127) { full_name[i++] = name[j++]; }
        full_name[i] = '\0';
    }

    /* Route through VFS_Open so the result is a HandleTable handle —
     * dos_Read/Write/Seek/Close all use HandleTable_Get.  Calling the
     * volume handler's DoPkt directly returned handler-native handle
     * numbers, which only worked for RAM: (its handler happens to return
     * HandleTable IDs); for FAT32 volumes the namespaces diverged and
     * every Read() on the returned handle failed. */
    uint32_t mode = cpu->d[2];
    int vflags;
    if (mode == 1006)      vflags = VFS_WRITE | VFS_CREATE | VFS_TRUNC;
    else if (mode == 1004) vflags = VFS_READ | VFS_WRITE;
    else                   vflags = VFS_READ;

    /* PROGDIR: is a per-task pseudo-assign VFS_Open can't see — rewrite
     * it to the task's program directory before dispatch (UAOS-248). */
    if (full_name[0] == 'P' && full_name[1] == 'R' &&
        full_name[2] == 'O' && full_name[3] == 'G' &&
        full_name[4] == 'D' && full_name[5] == 'I' &&
        full_name[6] == 'R' && full_name[7] == ':') {
        char pd[128];
        dos_program_dir_path(pd, sizeof(pd));
        char tmp[128];
        int i = 0;
        while (i < 127 && pd[i]) { tmp[i] = pd[i]; i++; }
        const char *rest = full_name + 8;
        if (*rest) {
            if (i < 127 && tmp[i - 1] != ':') tmp[i++] = '/';
            while (i < 127 && *rest) tmp[i++] = *rest++;
        }
        tmp[i] = '\0';
        int j = 0;
        while (j <= i) { full_name[j] = tmp[j]; j++; }
    }

    VfsFile fh = {0};
    if (!VFS_Open(&fh, full_name, vflags)) {
        cpu->d[0] = 0;
        SetIoErr(IoErr() ? IoErr() : ERROR_OBJECT_NOT_FOUND);
        KLOG(KLOG_DOS, KLOG_INFO, "[dos] Open('%s') mode=%u -> 0 ioerr=%ld\n",
             full_name, (unsigned)mode, (long)IoErr());
        return;
    }
    /* Store the canonical resolved path (winning assign target) so
     * follow-up ops — DupLockFromFH, SetComment via S:, etc. — hit the
     * target that actually holds the file, not the first assign dir
     * (UAOS-248). */
    const char *entry_path =
        fh.resolved_path[0] ? fh.resolved_path : full_name;
    uint32_t handle = HandleTable_AllocFile(entry_path, &fh, vflags);
    if (!handle) {
        VFS_Close(&fh);
        cpu->d[0] = 0;
        SetIoErr(ERROR_NO_FREE_STORE);
        return;
    }
    KLOG(KLOG_DOS, KLOG_INFO, "[dos] Open('%s') mode=%u -> %lu\n",
         full_name, (unsigned)mode, (unsigned long)handle);
    cpu->d[0] = handle;
}

static void dos_Close(M68kCPUState *cpu)
{
    uint32_t fh = cpu->d[1];
    if (fh == DOS_STDOUT_BPTR || fh == DOS_STDIN_BPTR) {
        cpu->d[0] = 0;
        return;
    }
    HandleEntry *ent = HandleTable_Get(fh);
    if (ent && ent->type == HTYPE_FILE) {
        VFS_Close(&ent->u.file.fh);
    }
    HandleTable_Free(fh);
    KLOG(KLOG_DOS, KLOG_INFO, "[dos] Close(fh=%x)\n", (unsigned)fh);
    cpu->d[0] = 0;
}

static void dos_Read(M68kCPUState *cpu)
{
    uint32_t fh  = cpu->d[1];
    uint32_t buf = cpu->d[2];
    uint32_t len = cpu->d[3];

    if (fh == DOS_STDIN_BPTR) {
        cpu->d[0] = 0;
        return;
    }
    if (fh == DOS_STDOUT_BPTR) {
        cpu->d[0] = (uint32_t)-1;
        SetIoErr(ERROR_ACTION_NOT_KNOWN);
        return;
    }

    HandleEntry *ent = HandleTable_Get(fh);
    if (!ent || (ent->type != HTYPE_FILE && ent->type != HTYPE_MEMFILE)) {
        cpu->d[0] = (uint32_t)-1;
        SetIoErr(ERROR_OBJECT_NOT_FOUND);
        KLOG(KLOG_DOS, KLOG_INFO, "[dos] Read(fh=%x) bad handle\n", (unsigned)fh);
        return;
    }
    if (ent->type == HTYPE_MEMFILE) {
        /* In-memory image (overlay/SFX executable kept open by the hunk
         * loader).  Read is a straight guest-RAM copy; reads past the end
         * return short/0 like EOF. */
        uint32_t pos   = ent->u.memfile.pos;
        uint32_t avail = (pos < ent->u.memfile.size)
                       ? ent->u.memfile.size - pos : 0;
        uint32_t n = (len < avail) ? len : avail;
        uint32_t src = ent->u.memfile.ram_addr + pos;
        if (buf + n > GUEST_RAM_SIZE ||
            src + n > GUEST_RAM_SIZE) {
            cpu->d[0] = (uint32_t)-1;
            SetIoErr(ERROR_OBJECT_NOT_FOUND);
            return;
        }
        for (uint32_t i = 0; i < n; i++)
            g_ram[buf + i] = g_ram[src + i];
        ent->u.memfile.pos = pos + n;
        cpu->d[0] = n;
        return;
    }
    if (buf + len >= GUEST_RAM_SIZE) {
        cpu->d[0] = (uint32_t)-1;
        SetIoErr(ERROR_OBJECT_NOT_FOUND);
        KLOG(KLOG_DOS, KLOG_INFO, "[dos] Read(fh=%x) buf=%x+%x out of range\n", (unsigned)fh,
                (unsigned)buf, (unsigned)len);
        return;
    }
    cpu->d[0] = VFS_Read(&ent->u.file.fh, g_ram + buf, len);
    KLOG(KLOG_DOS, KLOG_INFO, "[dos] Read(fh=%x,%x,%x) -> %ld\n", (unsigned)fh,
            (unsigned)buf, (unsigned)len, (long)cpu->d[0]);
}

static void dos_Write(M68kCPUState *cpu)
{
    uint32_t fh  = cpu->d[1];
    uint32_t buf = cpu->d[2];
    uint32_t len = cpu->d[3];

    if (fh == DOS_STDOUT_BPTR || fh == DOS_STDIN_BPTR) {
        if (buf + len < GUEST_RAM_SIZE) {
            char tmp[4096];
            uint32_t i;
            for (i = 0; i < len && buf + i < GUEST_RAM_SIZE; i++)
                tmp[i] = (char)g_ram[buf + i];
            tmp[i] = '\0';
            kprint(tmp);
            cpu->d[0] = len;
        } else {
            cpu->d[0] = (uint32_t)-1;
        }
        return;
    }

    HandleEntry *ent = HandleTable_Get(fh);
    if (!ent || ent->type != HTYPE_FILE) {
        cpu->d[0] = (uint32_t)-1;
        return;
    }
    if (buf + len >= GUEST_RAM_SIZE) {
        cpu->d[0] = (uint32_t)-1;
        return;
    }
    cpu->d[0] = VFS_Write(&ent->u.file.fh, g_ram + buf, len);
}

static void dos_Seek(M68kCPUState *cpu)
{
    uint32_t fh    = cpu->d[1];
    int32_t  offset = (int32_t)cpu->d[2];
    int32_t  mode   = (int32_t)cpu->d[3];

    if (fh == DOS_STDOUT_BPTR || fh == DOS_STDIN_BPTR) {
        cpu->d[0] = (uint32_t)-1;
        SetIoErr(ERROR_ACTION_NOT_KNOWN);
        return;
    }

    HandleEntry *ent = HandleTable_Get(fh);
    if (!ent || (ent->type != HTYPE_FILE && ent->type != HTYPE_MEMFILE)) {
        cpu->d[0] = (uint32_t)-1;
        SetIoErr(ERROR_OBJECT_NOT_FOUND);
        return;
    }

    if (ent->type == HTYPE_MEMFILE) {
        /* In-memory image: OFFSET_BEGINNING(-1)/CURRENT(0)/END(+1) with
         * signed offset; seeks past the end are legal (reads hit EOF). */
        int64_t np;
        uint32_t pos = ent->u.memfile.pos;
        uint32_t size = ent->u.memfile.size;
        if (mode == OFFSET_CURRENT)        np = (int64_t)pos + offset;
        else if (mode == OFFSET_END)       np = (int64_t)size + offset;
        else                               np = offset;   /* OFFSET_BEGINNING */
        cpu->d[0] = pos;
        if (np < 0) np = 0;
        ent->u.memfile.pos = (uint32_t)np;
        KLOG(KLOG_DOS, KLOG_INFO, "[dos] Seek(memfh=%x off=%ld mode=%ld) pos %ld->%ld size=%ld\n",
             (unsigned)fh, (long)offset, (long)mode,
             (long)pos, (long)np, (long)size);
        return;
    }

    VfsFile *f = &ent->u.file.fh;
    uint32_t new_pos = 0;
    uint32_t size = VFS_Size(f);
    if (mode == OFFSET_CURRENT)      new_pos = f->pos + (uint32_t)offset;
    else if (mode == OFFSET_END)       new_pos = size + (uint32_t)offset;
    else if (mode == OFFSET_BEGINNING) new_pos = (uint32_t)offset;
    else                               new_pos = (uint32_t)offset;
    cpu->d[0] = f->pos;
    VFS_Seek(f, new_pos);
    KLOG(KLOG_DOS, KLOG_INFO, "[dos] Seek(fh=%x off=%ld mode=%ld) pos %ld->%ld size=%ld ra=%x\n",
         (unsigned)fh, (long)offset, (long)mode, (long)cpu->d[0],
         (long)new_pos, (long)size,
         (unsigned)((cpu->a[7] + 4 <= GUEST_RAM_SIZE) ? guest_read_be32(cpu->a[7]) : 0));
}

static void dos_DeleteFile(M68kCPUState *cpu)
{
    uint32_t bptr = cpu->d[1];
    char name[128];
    int blen = dos_arg_to_c(bptr, name, sizeof(name));
    if (blen == 0) {
        cpu->d[0] = (uint32_t)DOSFALSE;
        SetIoErr(ERROR_OBJECT_NOT_FOUND);
        return;
    }

    char hit[128] = "";
    int32_t res = dos_path_pkt(name, ACTION_DELETE_OBJECT, 0, 0,
                               hit, sizeof(hit), NULL);
    KLOG(KLOG_DOS, KLOG_INFO, "[dos] DeleteFile('%s' -> '%s') -> %ld\n",
         name, hit, (long)res);
    cpu->d[0] = (uint32_t)res;
    if (res == DOSFALSE && !IoErr()) SetIoErr(ERROR_OBJECT_NOT_FOUND);
}

static void dos_Rename(M68kCPUState *cpu)
{
    uint32_t old_bptr = cpu->d[1];
    uint32_t new_bptr = cpu->d[2];
    char old_name[128], new_name[128];
    dos_arg_to_c(old_bptr, old_name, sizeof(old_name));
    dos_arg_to_c(new_bptr, new_name, sizeof(new_name));

    char old_full[128], new_full[128];
    dos_cwd_prefix(old_name, old_full, sizeof(old_full));
    dos_cwd_prefix(new_name, new_full, sizeof(new_full));

    char ovol[16], nvol[16];
    int ovl = extract_vol_name(old_full, ovol, sizeof(ovol));
    int nvl = extract_vol_name(new_full, nvol, sizeof(nvol));

    /* AmigaDOS Rename is same-volume only.  When both paths share a
     * multi-assign prefix, expand both through the same target index so
     * the pair stays on one backing volume (UAOS-248). */
    int targets = 0;
    if (ovl && nvl) {
        int same = 1;
        for (int i = 0; ovol[i] || nvol[i]; i++) {
            char a = ovol[i], b = nvol[i];
            if (a >= 'A' && a <= 'Z') a += 32;
            if (b >= 'A' && b <= 'Z') b += 32;
            if (a != b) { same = 0; break; }
        }
        if (same) targets = VFS_GetAssignTargetCount(ovol);
    }
    if (!ovl || !nvl) {
        cpu->d[0] = (uint32_t)DOSFALSE;
        SetIoErr(ERROR_OBJECT_NOT_FOUND);
        return;
    }

    int ntry = targets > 0 ? targets : 1;
    int32_t res = DOSFALSE;
    for (int t = 0; t < ntry; t++) {
        char rold[128], rnew[128], rv1[16], rv2[16];
        if (targets) {
            if (!dos_expand_target(ovol, old_full + ovl + 1, t,
                                   rold, sizeof(rold)) ||
                !dos_expand_target(ovol, new_full + nvl + 1, t,
                                   rnew, sizeof(rnew)))
                continue;
        } else {
            if (!dos_resolve_path(old_name, rold, sizeof(rold)) ||
                !dos_resolve_path(new_name, rnew, sizeof(rnew)))
                break;
        }
        if (!extract_vol_name(rold, rv1, sizeof(rv1)) ||
            !extract_vol_name(rnew, rv2, sizeof(rv2))) continue;
        int eq = 1;
        for (int i = 0; rv1[i] || rv2[i]; i++) {
            char a = rv1[i], b = rv2[i];
            if (a >= 'A' && a <= 'Z') a += 32;
            if (b >= 'A' && b <= 'Z') b += 32;
            if (a != b) { eq = 0; break; }
        }
        if (!eq) break;   /* resolved to different volumes — illegal */
        MsgPort *port = VFS_GetHandlerPort(rv1);
        if (!port) continue;
        res = DoPkt(port, ACTION_RENAME_OBJECT,
                    (intptr_t)rold, (intptr_t)rnew, 0, 0, 0);
        if (res != DOSFALSE) break;
    }
    cpu->d[0] = (uint32_t)res;
    if (res == DOSFALSE && !IoErr()) SetIoErr(ERROR_OBJECT_NOT_FOUND);
}

static void dos_SetProtection(M68kCPUState *cpu)
{
    uint32_t bptr = cpu->d[1];
    int32_t mask  = (int32_t)cpu->d[2];
    char name[128];
    dos_arg_to_c(bptr, name, sizeof(name));

    int32_t res = dos_path_pkt(name, ACTION_SET_PROTECT, (intptr_t)mask,
                               0, NULL, 0, NULL);
    cpu->d[0] = (uint32_t)res;
    if (res == DOSFALSE && !IoErr()) SetIoErr(ERROR_OBJECT_NOT_FOUND);
}

static void dos_GetVar(M68kCPUState *cpu)
{
    (void)cpu;
    cpu->d[0] = (uint32_t)-1;
    SetIoErr(ERROR_ACTION_NOT_KNOWN);
}

static void dos_SetVar(M68kCPUState *cpu)
{
    (void)cpu;
    cpu->d[0] = 0;
    SetIoErr(ERROR_ACTION_NOT_KNOWN);
}


static void dos_Lock(M68kCPUState *cpu)
{
    uint32_t bptr  = cpu->d[1];
    int32_t  mode  = (int32_t)cpu->d[2];
    char name[128];
    int blen = dos_arg_to_c(bptr, name, sizeof(name));

    char full_name[128];
    dos_cwd_prefix(name, full_name, sizeof(full_name));

    /* Assigns are expanded before handler dispatch (UAOS-248).  For a
     * multi-assign name (Assign ... ADD) each target is tried in order —
     * first hit wins, mirroring AmigaDOS search semantics and the
     * VFS_Open multi-assign path. */
    char vol_name[16];
    int vl = extract_vol_name(full_name, vol_name, sizeof(vol_name));
    int targets = vl ? VFS_GetAssignTargetCount(vol_name) : 0;
    int ntry = targets > 0 ? targets : 1;
    int32_t handle = 0;
    char resolved[128];
    for (int t = 0; t < ntry; t++) {
        if (targets) {
            /* Targets may themselves route through assigns */
            if (!dos_expand_target(vol_name, full_name + vl + 1, t,
                                   resolved, sizeof(resolved)))
                continue;
        } else {
            VFS_ResolveAssignPath(full_name, resolved, sizeof(resolved));
        }

        char rvol[16];
        if (!extract_vol_name(resolved, rvol, sizeof(rvol))) continue;
        MsgPort *port = VFS_GetHandlerPort(rvol);
        if (!port) continue;
        handle = DoPkt(port, ACTION_LOCATE_OBJECT, (intptr_t)resolved,
                       mode, 0, 0, 0);
        if (handle) break;
    }
    if (handle == 0) {
        cpu->d[0] = 0;
        SetIoErr(IoErr() ? IoErr() : ERROR_OBJECT_NOT_FOUND);
        KLOG(KLOG_DOS, KLOG_INFO, "[dos] Lock('%s') failed ioerr=%ld\n",
             full_name, (long)IoErr());
        return;
    }

    KLOG(KLOG_DOS, KLOG_INFO, "[dos] Lock('%s') -> '%s'\n", full_name, resolved);
    uint32_t lock_bptr = guest_alloc_filelock((uint32_t)handle, mode);
    if (lock_bptr == 0) {
        HandleTable_Free((uint32_t)handle);
        cpu->d[0] = 0;
        SetIoErr(ERROR_NO_FREE_STORE);
        return;
    }
    cpu->d[0] = lock_bptr;
}

static void dos_Unlock(M68kCPUState *cpu)
{
    uint32_t lock = cpu->d[1];
    if (lock == 0) { cpu->d[0] = (uint32_t)DOSTRUE; return; }

    uint32_t handle = 0;
    guest_read_filelock(lock, &handle, NULL);
    /* ACTION_FREE_LOCK round-trip — a bare HandleTable_Free would leak the
     * handler-side lock node (UAOS-247). */
    if (handle) VFS_FreeLock(handle);
    cpu->d[0] = (uint32_t)DOSTRUE;
}

static void dos_DupLock(M68kCPUState *cpu)
{
    uint32_t lock = cpu->d[1];
    if (lock == 0) { cpu->d[0] = 0; return; }

    uint32_t handle = 0;
    if (!guest_read_filelock(lock, &handle, NULL) || handle == 0) {
        cpu->d[0] = 0;
        SetIoErr(ERROR_OBJECT_NOT_FOUND);
        return;
    }

    HandleEntry *ent = HandleTable_GetLockEntry(handle, NULL);
    if (!ent || ent->type != HTYPE_LOCK) {
        cpu->d[0] = 0;
        SetIoErr(ERROR_OBJECT_NOT_FOUND);
        return;
    }

    char vol_name[16];
    extract_vol_name(ent->path, vol_name, sizeof(vol_name));
    MsgPort *port = VFS_GetHandlerPort(vol_name);
    if (!port) {
        cpu->d[0] = 0;
        SetIoErr(ERROR_DEVICE_NOT_MOUNTED);
        return;
    }

    int32_t dup = DoPkt(port, ACTION_COPY_DIR, (int32_t)handle, 0, 0, 0, 0);
    if (dup == 0) {
        cpu->d[0] = 0;
        SetIoErr(IoErr());
        return;
    }

    uint32_t dup_bptr = guest_alloc_filelock((uint32_t)dup, -2);
    if (dup_bptr == 0) {
        HandleTable_Free((uint32_t)dup);
        cpu->d[0] = 0;
        SetIoErr(ERROR_NO_FREE_STORE);
        return;
    }
    cpu->d[0] = dup_bptr;
}

static void dos_Parent(M68kCPUState *cpu)
{
    uint32_t lock = cpu->d[1];
    if (lock == 0) { cpu->d[0] = 0; return; }

    uint32_t handle = 0;
    if (!guest_read_filelock(lock, &handle, NULL) || handle == 0) {
        cpu->d[0] = 0;
        SetIoErr(ERROR_OBJECT_NOT_FOUND);
        return;
    }

    HandleEntry *ent = HandleTable_GetLockEntry(handle, NULL);
    if (!ent || ent->type != HTYPE_LOCK) {
        cpu->d[0] = 0;
        SetIoErr(ERROR_OBJECT_NOT_FOUND);
        return;
    }

    char vol_name[16];
    extract_vol_name(ent->path, vol_name, sizeof(vol_name));
    MsgPort *port = VFS_GetHandlerPort(vol_name);
    if (!port) {
        cpu->d[0] = 0;
        SetIoErr(ERROR_DEVICE_NOT_MOUNTED);
        return;
    }

    int32_t ph = DoPkt(port, ACTION_PARENT, (int32_t)handle, 0, 0, 0, 0);
    if (ph == 0) {
        cpu->d[0] = 0;
        SetIoErr(IoErr());
        return;
    }

    uint32_t parent_bptr = guest_alloc_filelock((uint32_t)ph, -2);
    if (parent_bptr == 0) {
        HandleTable_Free((uint32_t)ph);
        cpu->d[0] = 0;
        SetIoErr(ERROR_NO_FREE_STORE);
        return;
    }
    cpu->d[0] = parent_bptr;
}

static void dos_Examine(M68kCPUState *cpu)
{
    uint32_t lock = cpu->d[1];
    uint32_t fib_ptr = cpu->d[2];

    if (fib_ptr >= GUEST_RAM_SIZE) {
        cpu->d[0] = 0;
        SetIoErr(ERROR_OBJECT_NOT_FOUND);
        return;
    }

    uint32_t handle = 0;
    if (!guest_read_filelock(lock, &handle, NULL) || handle == 0) {
        cpu->d[0] = 0;
        SetIoErr(ERROR_OBJECT_NOT_FOUND);
        return;
    }

    HandleEntry *ent = HandleTable_GetLockEntry(handle, NULL);
    if (!ent || ent->type != HTYPE_LOCK) {
        cpu->d[0] = 0;
        SetIoErr(ERROR_OBJECT_NOT_FOUND);
        return;
    }

    char vol_name[16];
    extract_vol_name(ent->path, vol_name, sizeof(vol_name));
    MsgPort *port = VFS_GetHandlerPort(vol_name);
    if (!port) {
        cpu->d[0] = 0;
        SetIoErr(ERROR_DEVICE_NOT_MOUNTED);
        return;
    }

    cpu->d[0] = (uint32_t)dos_examine_pkt(port, ACTION_EXAMINE_OBJECT,
                                          (int32_t)handle, fib_ptr);
}

static void dos_ExamineNext(M68kCPUState *cpu)
{
    uint32_t lock = cpu->d[1];
    uint32_t fib_ptr = cpu->d[2];

    if (fib_ptr >= GUEST_RAM_SIZE) {
        cpu->d[0] = 0;
        SetIoErr(ERROR_OBJECT_NOT_FOUND);
        return;
    }

    uint32_t handle = 0;
    if (!guest_read_filelock(lock, &handle, NULL) || handle == 0) {
        cpu->d[0] = 0;
        SetIoErr(ERROR_OBJECT_NOT_FOUND);
        return;
    }

    HandleEntry *ent = HandleTable_GetLockEntry(handle, NULL);
    if (!ent || ent->type != HTYPE_LOCK) {
        cpu->d[0] = 0;
        SetIoErr(ERROR_OBJECT_NOT_FOUND);
        return;
    }

    char vol_name[16];
    extract_vol_name(ent->path, vol_name, sizeof(vol_name));
    MsgPort *port = VFS_GetHandlerPort(vol_name);
    if (!port) {
        cpu->d[0] = 0;
        SetIoErr(ERROR_DEVICE_NOT_MOUNTED);
        return;
    }

    cpu->d[0] = (uint32_t)dos_examine_pkt(port, ACTION_EXAMINE_NEXT,
                                          (int32_t)handle, fib_ptr);
}

static void dos_CreateDir(M68kCPUState *cpu)
{
    uint32_t bptr = cpu->d[1];
    char name[128];
    int blen = dos_arg_to_c(bptr, name, sizeof(name));
    (void)blen;

    char hit[128] = "";
    MsgPort *port = NULL;
    int32_t res = dos_path_pkt(name, ACTION_CREATE_DIR, 0, 0,
                               hit, sizeof(hit), &port);
    if (res == 0 || res == DOSFALSE) {
        cpu->d[0] = 0;
        if (!IoErr()) SetIoErr(ERROR_OBJECT_NOT_FOUND);
        return;
    }
    /* Handlers disagree on the return: some give a lock handle, others
     * just DOSTRUE.  Release any handler-side lock and take a fresh one
     * so the caller always gets a real FileLock BPTR (AmigaDOS semantics).
     * The lock must be taken on the resolved winning target, not the
     * unresolved assign name (UAOS-248). */
    if (res != DOSTRUE && port)
        DoPkt(port, ACTION_FREE_LOCK, res, 0, 0, 0, 0);
    cpu->d[0] = dos_lock_path(hit[0] ? hit : name);
    if (!cpu->d[0]) SetIoErr(ERROR_OBJECT_NOT_FOUND);
}

/* =========================================================================
 * Date / time helpers
 * ========================================================================= */

static int is_leap_year(int y)
{
    return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
}

static int32_t days_since_1978(uint16_t year, uint8_t month, uint8_t day)
{
    int32_t days = 0;
    for (int y = 1978; y < year; y++) {
        days += is_leap_year(y) ? 366 : 365;
    }
    static const int mdays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    for (int m = 1; m < month; m++) {
        days += mdays[m - 1];
        if (m == 2 && is_leap_year(year)) days++;
    }
    days += day - 1;
    return days;
}

static void uint_to_str(uint32_t v, char *buf, int max)
{
    if (max <= 1) { if (max == 1) buf[0] = '\0'; return; }
    if (v == 0) { buf[0] = '0'; buf[1] = '\0'; return; }

    char tmp[12];
    int i = 0;
    while (v > 0 && i < 11) {
        tmp[i++] = (char)('0' + (v % 10));
        v /= 10;
    }
    int j = 0;
    while (j < i && j < max - 1) {
        buf[j] = tmp[i - 1 - j];
        j++;
    }
    buf[j] = '\0';
}

static void uint_to_str_2d(uint32_t v, char *buf)
{
    buf[0] = (char)('0' + (v / 10));
    buf[1] = (char)('0' + (v % 10));
    buf[2] = '\0';
}

/* =========================================================================
 * Pattern matching helpers — shared by ParsePattern / MatchPattern
 * ========================================================================= */

/* AmigaDOS wildcard matcher: '?', '%', '*', '#x' repetition (incl. '#?'),
 * "'c" quoting, '(a|b|c)' alternation groups, '~' complement.
 * NFA over name positions — each set bit i means "i chars of n consumed";
 * groups and '#' backtrack correctly without exponential recursion. */
#define PM_MAXLEN 256

static int pm_ci_eq(char a, char b)
{
    if (a >= 'A' && a <= 'Z') a += 32;
    if (b >= 'A' && b <= 'Z') b += 32;
    return a == b;
}

/* pointer past one pattern unit starting at p (< pend) */
static const char *pm_unit_end(const char *p, const char *pend)
{
    if (*p == '\'') return (p + 2 < pend) ? p + 2 : pend;
    if (*p == '(') {
        int d = 1;
        p++;
        while (p < pend && d) {
            if (*p == '(') d++;
            else if (*p == ')') d--;
            p++;
        }
        return p;
    }
    return p + 1;
}

static void pm_run(const char *n, const char *p, const char *pend,
                   uint8_t st[]);

/* Apply one unit u..ue: st positions -> positions after the unit. */
static void pm_unit_step(const char *n, const char *u, const char *ue,
                         uint8_t st[])
{
    uint8_t out[PM_MAXLEN + 1];
    memset(out, 0, sizeof(out));
    if (*u == '%') return;                  /* epsilon: positions unchanged */
    if (*u == '(' && ue > u + 1 && ue[-1] == ')') {
        /* alternatives u+1..ue-1 split by top-level '|' */
        const char *ge = ue - 1, *s = u + 1;
        int depth = 0;
        for (const char *q = u + 1; q <= ge; q++) {
            if (q != ge) {
                if (*q == '(') { depth++; continue; }
                if (*q == ')') { depth--; continue; }
                if (*q != '|' || depth) continue;
            }
            /* alternative s..q, run from the same start positions */
            uint8_t sub[PM_MAXLEN + 1];
            memcpy(sub, st, sizeof(sub));
            pm_run(n, s, q, sub);
            for (int i = 0; i <= PM_MAXLEN; i++)
                if (sub[i]) out[i] = 1;
            s = q + 1;
        }
    } else {
        for (int i = 0; n[i] && i < PM_MAXLEN; i++) {
            if (!st[i]) continue;
            if (*u == '?' ||
                (*u == '\'' && u + 1 < ue && pm_ci_eq(n[i], u[1])) ||
                (*u != '\'' && pm_ci_eq(n[i], *u)))
                out[i + 1] = 1;
        }
    }
    memcpy(st, out, sizeof(out));
}

/* Run segment p..pend as a prefix match over the position set st. */
static void pm_run(const char *n, const char *p, const char *pend,
                   uint8_t st[])
{
    int nl = (int)strlen(n);
    if (nl > PM_MAXLEN) nl = PM_MAXLEN;
    while (p < pend) {
        if (*p == '~') {
            /* complement: positions whose remaining name does NOT match
             * the rest of this level advance to end-of-string */
            uint8_t out[PM_MAXLEN + 1];
            memset(out, 0, sizeof(out));
            for (int i = 0; i <= nl; i++) {
                if (!st[i]) continue;
                uint8_t sub[PM_MAXLEN + 1];
                memset(sub, 0, sizeof(sub));
                sub[i] = 1;
                pm_run(n, p + 1, pend, sub);
                if (!sub[nl]) out[nl] = 1;
            }
            memcpy(st, out, sizeof(out));
            return;
        }
        if (*p == '#') {
            const char *u  = ++p;
            const char *ue = pm_unit_end(u, pend);
            /* >=0 repetitions: fixed-point closure */
            uint8_t cur[PM_MAXLEN + 1], prev[PM_MAXLEN + 1];
            memcpy(cur, st, sizeof(cur));
            for (;;) {
                uint8_t step[PM_MAXLEN + 1];
                memcpy(prev, cur, sizeof(cur));
                memcpy(step, cur, sizeof(step));
                pm_unit_step(n, u, ue, step);
                int same = 1;
                for (int i = 0; i <= nl; i++) {
                    cur[i] |= step[i];
                    if (cur[i] != prev[i]) same = 0;
                }
                if (same) break;
            }
            memcpy(st, cur, sizeof(cur));
            p = ue;
            continue;
        }
        if (*p == '*') {
            /* '*' == '#?' — all positions from the earliest set bit on */
            for (int i = 0; i <= nl; i++) {
                if (st[i]) {
                    for (int j = i; j <= nl; j++) st[j] = 1;
                    break;
                }
            }
            p++;
            continue;
        }
        const char *ue = pm_unit_end(p, pend);
        pm_unit_step(n, p, ue, st);
        p = ue;
    }
}

static int pattern_match(const char *name, const char *pat)
{
    int nl = (int)strlen(name);
    if (nl > PM_MAXLEN) return 0;
    uint8_t st[PM_MAXLEN + 1];
    memset(st, 0, sizeof(st));
    st[0] = 1;
    pm_run(name, pat, pat + strlen(pat), st);
    return st[nl] != 0;
}

/* =========================================================================
 * DateStamp — fill guest DateStamp with current date/time
 * ========================================================================= */

static void dos_DateStamp(M68kCPUState *cpu)
{
    uint32_t addr = cpu->a[0];
    if (addr && addr + 12 <= GUEST_RAM_SIZE) {
        RtcDateTime dt = RTC_ReadDateTime();
        int32_t days = days_since_1978(dt.year, dt.month, dt.day);
        int32_t minutes = (int32_t)dt.hour * 60 + (int32_t)dt.min;
        int32_t ticks = (int32_t)dt.sec * 50;  /* PAL: 50 ticks/sec */

        guest_write_be32(addr + 0, (uint32_t)days);
        guest_write_be32(addr + 4, (uint32_t)minutes);
        guest_write_be32(addr + 8, (uint32_t)ticks);
    }
    cpu->d[0] = cpu->a[0];
}

/* =========================================================================
 * Delay — busy-wait using a spin loop
 * ========================================================================= */

static void dos_Delay(M68kCPUState *cpu)
{
    uint32_t ticks = cpu->d[0];
    if (ticks == 0) return;

    /* Amiga ticks are 1/50 s = 2 PIT ticks at 100 Hz.  Block on the
     * wait queue instead of busy-waiting so a Delay()ing guest task
     * does not stay runnable and starve lower priorities.  Sleep in
     * 1-tick slices and pump interrupt delivery so handlers (CIAB
     * player tick etc.) still fire while the guest is blocked. */
    extern void UAOS_M68k_DeliverInterrupts(void);
    UaosTask *self = Task_Current();
    uint64_t remaining = (uint64_t)ticks * 2;
    while (remaining-- > 0) {
        /* External halt (window-close quit, UAOS-247): abandon the rest of
         * the delay so the wrapper's teardown isn't held up. */
        if (self && self->type == TASK_TYPE_M68K && self->m68k_halted)
            break;
        Task_SleepTicks(1);
        UAOS_M68k_DeliverInterrupts();
    }
}

/* =========================================================================
 * DateToStr — convert DateStamp to formatted strings
 * ========================================================================= */

static void dos_DateToStr(M68kCPUState *cpu)
{
    uint32_t dt_addr = cpu->a[0];
    if (!dt_addr || dt_addr + 28 > GUEST_RAM_SIZE) {
        cpu->d[0] = (uint32_t)DOSFALSE;
        return;
    }

    /* Read DateTime struct from guest RAM (big-endian) */
    int32_t ds_days    = (int32_t)guest_read_be32(dt_addr + 0);
    int32_t ds_minute  = (int32_t)guest_read_be32(dt_addr + 4);
    int32_t ds_tick    = (int32_t)guest_read_be32(dt_addr + 8);
    uint8_t format     = g_ram[dt_addr + 12];
    uint32_t str_day   = guest_read_be32(dt_addr + 16);
    uint32_t str_date  = guest_read_be32(dt_addr + 20);
    uint32_t str_time  = guest_read_be32(dt_addr + 24);

    /* Convert DateStamp to calendar fields.
     * Amiga epoch (1978-01-01) to Unix epoch (1970-01-01) = 2922 days. */
    uint32_t unix_ts = (uint32_t)((ds_days + 2922) * 86400LL +
                                   ds_minute * 60LL + ds_tick / 50);
    uint16_t year; uint8_t month, day, hour, min, sec;
    ntp_unix_to_datetime(unix_ts, &year, &month, &day, &hour, &min, &sec);

    static const char *mon_name[] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
    };

    /* Write day string (empty for now) */
    if (str_day && str_day < GUEST_RAM_SIZE) {
        g_ram[str_day] = '\0';
    }

    /* Write date string */
    if (str_date && str_date + 16 < GUEST_RAM_SIZE) {
        char buf[16];
        uint32_t yr = year % 100;
        switch (format) {
            case 2: /* FORMAT_USA */
                uint_to_str_2d(month, buf);
                buf[2] = '-';
                uint_to_str_2d(day, buf + 3);
                buf[5] = '-';
                uint_to_str_2d(yr, buf + 6);
                buf[8] = '\0';
                break;
            case 3: /* FORMAT_CDN */
                uint_to_str_2d(yr, buf);
                buf[2] = '-';
                uint_to_str_2d(month, buf + 3);
                buf[5] = '-';
                uint_to_str_2d(day, buf + 6);
                buf[8] = '\0';
                break;
            case 1: /* FORMAT_INTL */
                uint_to_str_2d(day, buf);
                buf[2] = '-';
                buf[3] = mon_name[month - 1][0];
                buf[4] = mon_name[month - 1][1];
                buf[5] = mon_name[month - 1][2];
                buf[6] = '-';
                uint_to_str(year, buf + 7, 6);
                break;
            default: /* FORMAT_DOS, FORMAT_DEF */
                uint_to_str_2d(day, buf);
                buf[2] = '-';
                buf[3] = mon_name[month - 1][0];
                buf[4] = mon_name[month - 1][1];
                buf[5] = mon_name[month - 1][2];
                buf[6] = '-';
                uint_to_str_2d(yr, buf + 7);
                buf[9] = '\0';
                break;
        }
        int i = 0;
        while (buf[i] && str_date + i < GUEST_RAM_SIZE) {
            g_ram[str_date + i] = (uint8_t)buf[i];
            i++;
        }
        g_ram[str_date + i] = '\0';
    }

    /* Write time string */
    if (str_time && str_time + 10 < GUEST_RAM_SIZE) {
        char buf[10];
        uint_to_str_2d(hour, buf);
        buf[2] = ':';
        uint_to_str_2d(min, buf + 3);
        buf[5] = ':';
        uint_to_str_2d(sec, buf + 6);
        buf[8] = '\0';
        int i = 0;
        while (buf[i] && str_time + i < GUEST_RAM_SIZE) {
            g_ram[str_time + i] = (uint8_t)buf[i];
            i++;
        }
        g_ram[str_time + i] = '\0';
    }

    cpu->d[0] = (uint32_t)DOSTRUE;
}

/* =========================================================================
 * ParsePattern / ParsePatternNoCase — copy pattern and detect wildcards
 *
 * Recognised wildcards: ?, *, #? (and convenience alias *), plus % (matches
 * the empty string, as in AmigaDOS).  ParsePattern returns 1 if the
 * pattern contains wildcards, 0 if it is a plain literal, and -1 on error.
 * ========================================================================= */

static void dos_ParsePattern(M68kCPUState *cpu)
{
    uint32_t src = cpu->d[1];
    uint32_t dst = cpu->a[0];
    int32_t dst_len = (int32_t)cpu->d[2];

    if (dst_len < 1 || src >= GUEST_RAM_SIZE || dst >= GUEST_RAM_SIZE) {
        cpu->d[0] = (uint32_t)-1;
        return;
    }

    int i = 0;
    int has_wild = 0;

    while (i < dst_len - 1 && src + i < GUEST_RAM_SIZE && g_ram[src + i]) {
        char c = (char)g_ram[src + i];
        g_ram[dst + i] = (uint8_t)c;
        if (c == '?' || c == '*' || c == '#' || c == '%') has_wild = 1;
        i++;
    }

    if (src + i < GUEST_RAM_SIZE && g_ram[src + i]) {
        cpu->d[0] = (uint32_t)-1;
        return;
    }

    g_ram[dst + i] = 0;
    cpu->d[0] = has_wild ? 1 : 0;
}

static void dos_ParsePatternNoCase(M68kCPUState *cpu)
{
    /* Same behaviour as ParsePattern — our MatchPattern is case-insensitive */
    dos_ParsePattern(cpu);
}

/* =========================================================================
 * MatchPattern / MatchPatternNoCase — match string against parsed pattern
 * ========================================================================= */

static void dos_MatchPattern(M68kCPUState *cpu)
{
    uint32_t pat = cpu->d[1];
    uint32_t str = cpu->a[0];

    if (pat >= GUEST_RAM_SIZE || str >= GUEST_RAM_SIZE) {
        cpu->d[0] = (uint32_t)DOSFALSE;
        return;
    }

    char pat_buf[128];
    char str_buf[128];
    int i = 0;
    while (i < 127 && pat + i < GUEST_RAM_SIZE && g_ram[pat + i]) {
        pat_buf[i] = (char)g_ram[pat + i]; i++;
    }
    pat_buf[i] = '\0';

    i = 0;
    while (i < 127 && str + i < GUEST_RAM_SIZE && g_ram[str + i]) {
        str_buf[i] = (char)g_ram[str + i]; i++;
    }
    str_buf[i] = '\0';

    cpu->d[0] = pattern_match(str_buf, pat_buf) ? (uint32_t)DOSTRUE : (uint32_t)DOSFALSE;
}

static void dos_MatchPatternNoCase(M68kCPUState *cpu)
{
    /* Same behaviour as MatchPattern — our pattern_match is already case-insensitive */
    dos_MatchPattern(cpu);
}

/* =========================================================================
 * Segment loading (LoadSeg / UnLoadSeg)
 * ========================================================================= */

#define LS_HUNK_HEADER   0x3F3
#define LS_HUNK_CODE     0x3E9
#define LS_HUNK_DATA     0x3EA
#define LS_HUNK_BSS      0x3EB
#define LS_HUNK_RELOC32  0x3EC
#define LS_HUNK_END      0x3F2
#define LS_HUNK_SYMBOL   0x3F0
#define LS_HUNK_DEBUG    0x3F1

#define MAX_LOADSEG_HUNKS  32

static uint8_t g_loadseg_buf[524288]; /* 512KB temp for hunk loading */

/* Forward declarations for seglist tracking */
static void seglist_track(uint32_t bptr, uint32_t addr);
static uint32_t seglist_untrack(uint32_t bptr);

static uint32_t ls_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] <<  8) | ((uint32_t)p[3]);
}

static uint32_t loadseg_hunk_load(const uint8_t *bin, uint32_t bin_size)
{
    if (bin_size < 8) return 0;
    const uint8_t *p = bin;
    const uint8_t *end = bin + bin_size;

    if (ls_be32(p) != LS_HUNK_HEADER) return 0;
    p += 4;

    while (p + 4 <= end) {
        uint32_t cnt = ls_be32(p); p += 4;
        if (!cnt) break;
        p += cnt * 4;
    }

    if (p + 12 > end) return 0;
    uint32_t table_size = ls_be32(p); p += 4;
    uint32_t first_hunk = ls_be32(p); p += 4;
    uint32_t last_hunk  = ls_be32(p); p += 4;
    (void)table_size;

    uint32_t n_hunks = last_hunk - first_hunk + 1;
    if (n_hunks > MAX_LOADSEG_HUNKS) return 0;

    uint32_t hunk_base[MAX_LOADSEG_HUNKS];
    uint32_t allocated[MAX_LOADSEG_HUNKS];

    for (uint32_t i = 0; i < n_hunks; i++) {
        if (p + 4 > end) return 0;
        uint32_t spec  = ls_be32(p); p += 4;
        uint32_t flags = spec & 0xC0000000u;
        uint32_t words = spec & ~0xC0000000u;
        /* Both flag bits set means a full MEMF_* requirements longword
         * follows the size word (dos/doshunks.h HUNKF_…). */
        uint32_t mem_req = 0;
        if (flags == 0xC0000000u) {
            if (p + 4 > end) return 0;
            mem_req = ls_be32(p); p += 4;
        } else if (flags == 0x80000000u) {
            mem_req = MEMF_FAST;
        } else if (flags == 0x40000000u) {
            mem_req = MEMF_CHIP;
        }
        uint32_t bytes = words * 4;
        /* Real LoadSeg layout: [total_size_bytes][next_seg_BPTR][data].
         * The seglist BPTR points at the link field, so
         *   link_addr = allocated+4, data = allocated+8. */
        uint32_t seg_size = (8 + (bytes ? bytes : 4) + 3u) & ~3u;
        allocated[i] = (mem_req & MEMF_CHIP) ? heap_alloc_fl_chip(seg_size)
                     : (mem_req & MEMF_FAST) ? heap_alloc_fl_fast(seg_size)
                     : heap_alloc_fl(seg_size);
        if (!allocated[i]) return 0;
        hunk_base[i] = allocated[i] + 8;
        guest_write_be32(allocated[i] + 0, bytes + 8);
    }

    /* Write SegList next pointers (BPTR to next segment's link field) */
    for (uint32_t i = 0; i < n_hunks; i++) {
        guest_write_be32(allocated[i] + 4,
                         (i + 1 < n_hunks) ? ((allocated[i + 1] + 4) >> 2) : 0);
    }

    int cur = 0;
    while (p + 4 <= end && cur < (int)n_hunks) {
        uint32_t type = ls_be32(p) & 0x3FFFFFFF; p += 4;

        if (type == LS_HUNK_CODE || type == LS_HUNK_DATA) {
            if (p + 4 > end) break;
            uint32_t words = ls_be32(p); p += 4;
            uint32_t bytes = words * 4;
            if (p + bytes > end) return 0;
            for (uint32_t i = 0; i < bytes; i++)
                g_ram[hunk_base[cur] + i] = p[i];
            p += bytes;
        } else if (type == LS_HUNK_BSS) {
            if (p + 4 > end) break;
            p += 4;
        } else if (type == LS_HUNK_RELOC32) {
            while (p + 4 <= end) {
                uint32_t n_offsets = ls_be32(p); p += 4;
                if (!n_offsets) break;
                if (p + 4 > end) break;
                uint32_t ref_hunk = ls_be32(p); p += 4;
                if (ref_hunk >= n_hunks) { p += n_offsets * 4; continue; }
                uint32_t base = hunk_base[ref_hunk];
                for (uint32_t r = 0; r < n_offsets; r++) {
                    if (p + 4 > end) break;
                    uint32_t offset = ls_be32(p); p += 4;
                    uint32_t patch_addr = hunk_base[cur] + offset;
                    if (patch_addr + 4 <= GUEST_RAM_SIZE) {
                        uint32_t old_val = guest_read_be32(patch_addr);
                        guest_write_be32(patch_addr, old_val + base);
                    }
                }
            }
            continue;
        } else if (type == LS_HUNK_SYMBOL || type == LS_HUNK_DEBUG) {
            while (p + 4 <= end) {
                uint32_t len = ls_be32(p); p += 4;
                if (!len) break;
                p += len * 4 + 4;
            }
            continue;
        } else if (type == LS_HUNK_END) {
            cur++;
            continue;
        } else {
            break;
        }
    }

    /* Seglist head = address of the first segment's link field;
     * its BPTR = ret>>2, entry point = ret+4 (hunk data). */
    return allocated[0] + 4;
}

static void dos_LoadSeg(M68kCPUState *cpu)
{
    uint32_t bptr = cpu->d[1];
    char name[128];
    int blen = dos_arg_to_c(bptr, name, sizeof(name));
    if (blen == 0) {
        cpu->d[0] = 0;
        SetIoErr(ERROR_OBJECT_NOT_FOUND);
        return;
    }

    char full_name[128];
    int has_device = 0;
    for (int i = 0; i < blen; i++) if (name[i] == ':') { has_device = 1; break; }
    if (has_device) {
        int i = 0; while (i < blen) { full_name[i] = name[i]; i++; }
        full_name[i] = '\0';
    } else {
        const char *cwd = m68k_cur_cwd();
        int cwd_len = 0; while (cwd[cwd_len] && cwd_len < 63) cwd_len++;
        int i = 0; while (i < cwd_len) { full_name[i] = cwd[i]; i++; }
        if (cwd_len > 0 && cwd[cwd_len - 1] != ':' && cwd[cwd_len - 1] != '/')
            full_name[i++] = '/';
        int j = 0; while (j < blen && i < 127) { full_name[i++] = name[j++]; }
        full_name[i] = '\0';
    }

    VfsFile fh;
    if (!VFS_Open(&fh, full_name, VFS_READ)) {
        cpu->d[0] = 0;
        SetIoErr(ERROR_OBJECT_NOT_FOUND);
        return;
    }

    uint32_t size = VFS_Size(&fh);
    if (size == 0 || size > sizeof(g_loadseg_buf)) {
        VFS_Close(&fh);
        cpu->d[0] = 0;
        SetIoErr(ERROR_OBJECT_NOT_FOUND);
        return;
    }

    uint32_t read = VFS_Read(&fh, g_loadseg_buf, size);
    VFS_Close(&fh);
    if (read != size) {
        cpu->d[0] = 0;
        SetIoErr(ERROR_OBJECT_NOT_FOUND);
        return;
    }

    uint32_t seglist = loadseg_hunk_load(g_loadseg_buf, size);
    if (seglist == 0) {
        cpu->d[0] = 0;
        SetIoErr(ERROR_OBJECT_NOT_FOUND);
        return;
    }

    /* Track the seglist so UnLoadSeg can free it */
    seglist_track(seglist >> 2, seglist);

    cpu->d[0] = seglist >> 2;
}

/* =========================================================================
 * Seglist tracking — map BPTR → allocation address so UnLoadSeg can free
 * ========================================================================= */

#define MAX_SEGLISTS 32

typedef struct {
    uint32_t bptr;   /* BPTR returned to caller (allocated[0] >> 2)        */
    uint32_t addr;   /* raw guest address of first segment (allocated[0])   */
} SeglistEntry;

static SeglistEntry g_seglists[MAX_SEGLISTS];
static int          g_seglist_count = 0;

static void seglist_track(uint32_t bptr, uint32_t addr)
{
    if (g_seglist_count >= MAX_SEGLISTS) return;
    g_seglists[g_seglist_count].bptr = bptr;
    g_seglists[g_seglist_count].addr = addr;
    g_seglist_count++;
}

static uint32_t seglist_untrack(uint32_t bptr)
{
    for (int i = 0; i < g_seglist_count; i++) {
        if (g_seglists[i].bptr == bptr) {
            uint32_t addr = g_seglists[i].addr;
            g_seglists[i] = g_seglists[--g_seglist_count];
            return addr;
        }
    }
    return 0;
}

static void dos_UnLoadSeg(M68kCPUState *cpu)
{
    /* AmigaOS: D1=BPTR seglist */
    uint32_t seg_bptr = cpu->d[1];
    if (!seg_bptr) return;

    /* Walk the seglist chain and free each segment via the free-list
     * allocator.  Segment layout (set up by loadseg_hunk_load):
     *   [seglist+0] BPTR to next segment's link field (0 = last)
     *   [seglist-4] total block size in bytes
     *   [seglist+4] segment data
     * seglist addresses here are the link-field addresses (allocated+4);
     * the heap payload to free is allocated = link_field - 4. */

    uint32_t tracked_addr = seglist_untrack(seg_bptr);
    if (tracked_addr) {
        uint32_t cur = tracked_addr;
        while (cur && cur < GUEST_RAM_SIZE) {
            uint32_t next_bptr = guest_read_be32(cur + 0);
            heap_free_fl(cur - 4);
            cur = next_bptr ? (next_bptr << 2) : 0;
        }
    }
    /* If not tracked, silently no-op (should not happen with properly loaded seglists) */
}

/* =========================================================================
 * Process control
 * ========================================================================= */

/* =========================================================================
 * Process helpers — forward declarations
 * ========================================================================= */

/* From exec_task.c */
extern UaosTask *Task_CreateM68k(const char *name, int8_t pri,
                                  const uint8_t *binary, uint32_t bin_size,
                                  const char **argv,
                                  void (*print_fn)(const char *));
extern UaosTask *Task_Current(void);

/* From uaos_m68k_glue.c */
extern int m68k_execute(int num_cycles);
extern void m68k_end_timeslice(void);
extern void m68k_set_reg(int reg, unsigned int val);
extern unsigned int m68k_get_reg(void *context, int reg);

/* M68k register IDs (Musashi) */
#define M68K_REG_D0  0
#define M68K_REG_D1  1
#define M68K_REG_D2  2
#define M68K_REG_D3  3
#define M68K_REG_A0  8
#define M68K_REG_A6 14
#define M68K_REG_A7 15
#define M68K_REG_PC 16

/* =========================================================================
 * Helpers to build a minimal in-RAM M68k binary that jumps to a loaded
 * seglist entry point.  RunCommand uses this to call a seglist without
 * having a real Hunk file on disk.
 *
 * Layout in the temp buffer (big-endian M68k instructions):
 *   JSR  <entry_addr>.L   (4EBx absolute long)
 *   MOVEQ #rc, D0
 *   ILLEGAL (triggers dos_Exit via glue)
 *
 * We can't use the normal "binary" path because that requires a Hunk header.
 * Instead we call the entry directly via m68k_execute after manually setting
 * up the registers and PC.
 * ========================================================================= */

/* Allocate & build a minimal Process struct in guest RAM.
 * Returns the guest address or 0.
 *
 * Layout follows AmigaOS dos/dosextens.h (see amiga_task.h):
 *   Task node at +0, embedded pr_MsgPort at +0x5C, process fields to +0xE4.
 * pr_CLI stays 0 for detached (CreateProc-style) children — the parent
 * writes its own trampoline which reads ThisTask/pr_SegList itself. */
static uint32_t build_process_struct(const char *name, uint8_t pri,
                                     uint32_t seg_bptr, uint32_t stack_top,
                                     uint32_t stack_low)
{
    uint32_t name_len = 0;
    if (name) while (name[name_len] && name_len < 63) name_len++;
    uint32_t proc_addr = heap_alloc_fl(PROCESS_SIZE + CLI_SIZE + 32 + name_len + 2);
    if (!proc_addr) return 0;

    uint32_t cli_addr  = proc_addr + PROCESS_SIZE;
    uint32_t name_addr = proc_addr + PROCESS_SIZE + CLI_SIZE + 32;

    /* Zero the whole block */
    for (uint32_t i = 0; i < PROCESS_SIZE + CLI_SIZE + 32 + name_len + 2; i++)
        g_ram[proc_addr + i] = 0;

    /* Task node header */
    g_ram[proc_addr + TASK_LN_TYPE]  = NT_PROCESS;
    g_ram[proc_addr + TASK_LN_PRI]   = (uint8_t)pri;
    g_ram[proc_addr + TASK_TC_STATE] = TS_RUN;
    guest_write_be32(proc_addr + TASK_LN_NAME, name_addr);
    for (uint32_t i = 0; i <= name_len; i++)
        g_ram[name_addr + i] = (uint8_t)name[i];

    /* tc_SigAlloc: all bits free */
    guest_write_be32(proc_addr + TASK_TC_SIGALLOC, 0xFFFFFFFFu);
    /* tc_SP bounds (stack checking / runtime diagnostics) */
    guest_write_be32(proc_addr + TASK_TC_SPLOWER, stack_low);
    guest_write_be32(proc_addr + TASK_TC_SPUPPER, stack_top);
    guest_write_be32(proc_addr + TASK_TC_SPREG,   stack_top);

    /* pr_MsgPort at +0x5C: node type + owning task so PutMsg/ReplyMsg
     * signal correctly.  mp_SigBit stays 0 — processes in UAOS's
     * sequential model never sleep on the process port. */
    {
        uint32_t mp = proc_addr + PR_MSGPORT;
        g_ram[mp + LN_TYPE] = NT_MSGPORT;
        g_ram[mp + MP_FLAGS] = PA_SIGNAL;
        g_ram[mp + MP_SIGBIT] = 0;
        guest_write_be32(mp + MP_SIGTASK, proc_addr);
        /* init mp_MsgList as empty List: head->tail, tailpred->head */
        guest_write_be32(mp + MP_MSGLIST + LH_HEAD,     mp + MP_MSGLIST + LH_TAIL);
        guest_write_be32(mp + MP_MSGLIST + LH_TAIL,     0);
        guest_write_be32(mp + MP_MSGLIST + LH_TAILPRED, mp + MP_MSGLIST + LH_HEAD);
        g_ram[mp + MP_MSGLIST + LH_TYPE] = 0;
    }

    guest_write_be32(proc_addr + PR_SEGLIST,   seg_bptr);
    guest_write_be32(proc_addr + PR_STACKSIZE, stack_top - stack_low);
    guest_write_be32(proc_addr + PR_GLOBVEC,   DOS_BASE_GLOBVEC);
    guest_write_be32(proc_addr + PR_STACKBASE, stack_low >> 2);
    guest_write_be32(proc_addr + PR_CIS,       DOS_STDIN_BPTR);
    guest_write_be32(proc_addr + PR_COS,       DOS_STDOUT_BPTR);
    guest_write_be32(proc_addr + PR_WINDOWPTR, 0xFFFFFFFFu); /* no req window */

    /* pr_CLI stays 0 — this is a detached process, not a CLI command. */
    (void)cli_addr;
    return proc_addr;
}

static void dos_CreateProc(M68kCPUState *cpu)
{
    /* AmigaOS: D1=BSTR name, D2=LONG pri, D3=BPTR seglist, D4=ULONG stackSize
     * → D0=struct MsgPort * (the new process's pr_MsgPort) or NULL.
     *
     * Sequential model (see dos_Exit): we build the child Process + stack in
     * THIS guest window, queue it, and return.  The child runs when the
     * parent calls Exit() — dos_Exit respawns this m68k context at the
     * child's entry point. */
    uint32_t name_bptr = cpu->d[1];
    int8_t   pri       = (int8_t)(cpu->d[2] & 0xFF);
    uint32_t seg_bptr  = cpu->d[3];
    uint32_t stacksize = cpu->d[4];
    if (stacksize < 0x800) stacksize = 0x800;      /* sanity floor */
    if (stacksize > 0x40000) stacksize = 0x40000;

    char proc_name[64];
    dos_arg_to_c(name_bptr, proc_name, sizeof(proc_name));
    if (!proc_name[0]) {
        int i = 0;
        const char *dflt = "NewProc";
        while (dflt[i]) { proc_name[i] = dflt[i]; i++; }
        proc_name[i] = '\0';
    }

    if (!seg_bptr) {
        SetIoErr(ERROR_OBJECT_NOT_FOUND);
        cpu->d[0] = 0;
        return;
    }
    uint32_t seg_addr = seg_bptr << 2;
    if (seg_addr + 8 > GUEST_RAM_SIZE) {
        SetIoErr(ERROR_OBJECT_NOT_FOUND);
        cpu->d[0] = 0;
        return;
    }
    /* Segment layout: [BPTR next][data...] — entry is first code byte. */
    uint32_t entry = seg_addr + 4;

    if ((g_pending_tail + 1) % MAX_PENDING_PROC == g_pending_head) {
        SetIoErr(ERROR_TASK_TABLE_FULL);
        cpu->d[0] = 0;
        return;
    }

    /* Child stack: allocated in the shared window, DOS_Exit stub on top so
     * an RTS at the end of the child lands on Exit(). */
    uint32_t stack_low = heap_alloc_fl(stacksize);
    if (!stack_low) {
        SetIoErr(ERROR_NO_FREE_STORE);
        cpu->d[0] = 0;
        return;
    }
    uint32_t stack_top = stack_low + stacksize - 4;
    guest_write_be32(stack_top, DOS_EXIT_STUB);

    uint32_t proc_addr = build_process_struct(proc_name, (uint8_t)pri,
                                              seg_bptr, stack_top, stack_low);
    if (!proc_addr) {
        heap_free_fl(stack_low);
        SetIoErr(ERROR_NO_FREE_STORE);
        cpu->d[0] = 0;
        return;
    }

    PendingProc *pp = &g_pending_procs[g_pending_tail];
    pp->entry     = entry;
    pp->stack_top = stack_top;
    pp->proc      = proc_addr;
    pp->proc_port = proc_addr + PR_MSGPORT;
    g_pending_tail = (g_pending_tail + 1) % MAX_PENDING_PROC;

    kprint("[dos] CreateProc '");
    kprint(proc_name);
    kprint("' queued\n");

    cpu->d[0] = proc_addr + PR_MSGPORT;   /* struct MsgPort * (APTR) */
}

static void dos_RunCommand(M68kCPUState *cpu)
{
    /* AmigaOS: D1=BPTR seglist, D2=ULONG stackSize, A0=STRPTR argPtr,
     *          D3=ULONG argSize → D0=LONG returnCode
     *
     * RunCommand executes a loaded seglist synchronously in the context of
     * the current process, then returns the result code.
     *
     * Implementation:
     *   1. Resolve the entry point from the seglist.
     *   2. Push the argument string + length onto the M68k stack.
     *   3. JSR to the entry — we stay inside the current m68k_execute loop
     *      because we just adjust PC and SP then continue execution.
     *      The segment's startup code will eventually call dos.library Exit()
     *      which sets g_emu_halted=1; we detect that and return the RC.
     *
     * Caveat: because we redirect execution we cannot truly "return" from
     * RunCommand until the sub-program calls Exit().  This is exactly what
     * real AmigaOS does — RunCommand is a call/return via the stack.
     */
    uint32_t seg_bptr  = cpu->d[1];
    uint32_t arg_ptr   = cpu->a[0];
    uint32_t arg_size  = cpu->d[3];
    (void)cpu->d[2];   /* stackSize — we use the current stack */

    if (!seg_bptr) {
        SetIoErr(ERROR_OBJECT_NOT_FOUND);
        cpu->d[0] = (uint32_t)-1;
        return;
    }

    uint32_t seg_addr = seg_bptr << 2;
    if (seg_addr + 8 > GUEST_RAM_SIZE) {
        SetIoErr(ERROR_OBJECT_NOT_FOUND);
        cpu->d[0] = (uint32_t)-1;
        return;
    }

    /* Entry point is 4 bytes past the seglist next-pointer */
    uint32_t entry = seg_addr + 4;

    /* Set up the M68k state to call the entry point.
     *   D0 = argument size (number of characters)
     *   A0 = argument pointer (C string or BSTR depending on startup)
     * We do a virtual JSR by pushing the current PC on the M68k stack so
     * RTS at the end of the program returns here.  Then we set PC = entry.
     *
     * However, in practice most programs call dos.library Exit() rather than
     * RTS, which sets g_emu_halted.  We handle both paths:
     *   - If the program does RTS, PC returns to a small trap stub we install.
     *   - If the program calls Exit(), g_emu_halted stops execution.
     *
     * We install a 4-byte "return stub" just below the current SP:
     *   ILLEGAL 0x4AFC + dispatch word (DOS_EXIT via lib 2, fn 6)
     */
    uint32_t sp = m68k_get_reg(NULL, M68K_REG_A7);

    /* Place return stub at a known location in upper RAM */
    const uint32_t ret_stub = 0x1EF800u;
    if (ret_stub + 4 <= GUEST_RAM_SIZE) {
        g_ram[ret_stub + 0] = 0x4A; g_ram[ret_stub + 1] = 0xFC; /* ILLEGAL */
        g_ram[ret_stub + 2] = 0x02; g_ram[ret_stub + 3] = 0x06; /* LIB_DOS, DOS_EXIT */
    }

    /* Push return address (ret_stub) onto M68k stack */
    sp -= 4;
    if (sp + 4 <= GUEST_RAM_SIZE) {
        guest_write_be32(sp, ret_stub);
    }
    m68k_set_reg(M68K_REG_A7, sp);

    /* Set entry arguments per AmigaOS calling convention */
    m68k_set_reg(M68K_REG_D0, arg_size);
    m68k_set_reg(M68K_REG_A0, arg_ptr);

    /* Jump to entry */
    m68k_set_reg(M68K_REG_PC, entry);

    /* Signal the glue that we don't want to stop yet */
    g_emu_halted = 0;

    /* Execute until Exit() is called (or until something else terminates) */
    while (!g_emu_halted) {
        m68k_execute(10000);
        Chiptrace_PcSample();
        g_m68k_cycles += (uint64_t)m68k_cycles_run();
        chip_emu_run_to_cycle(g_m68k_cycles);
    }

    /* Retrieve the return code that Exit() was called with.
     * dos_Exit stores it in D1 (Amiga convention); after Exit sets
     * g_emu_halted the last D1 value is still in the Musashi register set. */
    uint32_t rc = m68k_get_reg(NULL, M68K_REG_D1);

    /* Reset halted flag — the outer loop can continue */
    g_emu_halted = 0;

    cpu->d[0] = rc;
}

/* Tag constants for SystemTagList (from dos/dosextens.h) */
#define NP_Seglist      (0x80000000 + 2)   /* BPTR seglist for new process */
#define NP_FreeSeglist  (0x80000000 + 3)   /* BOOL: free seglist on exit */
#define NP_CopyArgs     (0x80000000 + 4)   /* BOOL: copy args */
#define NP_ArgPtr       (0x80000000 + 5)   /* STRPTR: argument pointer */
#define NP_ArgLen       (0x80000000 + 6)   /* ULONG: argument length */
#define NP_Priority     (0x80000000 + 7)   /* BYTE: initial priority */
#define NP_Name         (0x80000000 + 9)   /* STRPTR: process name */
#define NP_Cwd          (0x80000000 + 12)  /* BPTR: current working directory */
#define NP_StackSize    (0x80000000 + 13)  /* ULONG: stack size */

/* Tag constants for SystemTagList (SYS_*) */
#define SYS_Input       (0x80000000 + 100) /* BPTR: input filehandle */
#define SYS_Output      (0x80000000 + 101) /* BPTR: output filehandle */
#define SYS_Asynch      (0x80000000 + 102) /* BOOL: run asynchronously */

/* Tag list control tags */
#define TAG_DONE        0
#define TAG_IGNORE      1
#define TAG_MORE        2
#define TAG_SKIP        3

typedef struct TagItem {
    uint32_t ti_Tag;
    uint32_t ti_Data;
} TagItem_t;

static uint32_t parse_tag_item(uint32_t *tag_ptr, uint32_t tag_to_find)
{
    int guard = 0;
    if (!tag_ptr) return 0;

    while (guard++ < 16384) {
        uint32_t tag = guest_read_be32((uint32_t)(uintptr_t)tag_ptr);
        uint32_t data = guest_read_be32((uint32_t)(uintptr_t)tag_ptr + 4);

        if (tag == TAG_DONE) {
            break;
        }

        if (tag == TAG_IGNORE) {
            tag_ptr += 2;
            continue;
        }

        if (tag == TAG_MORE) {
            tag_ptr = (uint32_t *)(uintptr_t)data;
            continue;
        }

        if (tag == TAG_SKIP) {
            tag_ptr += 2 * (data + 1);
            continue;
        }

        if (tag == tag_to_find) {
            return data;
        }

        tag_ptr += 2;  /* Each tag item is 8 bytes */
    }

    return 0;  /* Tag not found */
}

static void dos_SystemTagList(M68kCPUState *cpu)
{
    /* AmigaOS: D1=STRPTR command, A1=struct TagItem *tags → D0=LONG result
     *
     * Create and run a new process with configuration from tag list.
     * Tags extracted:
     *   NP_Name       - process name
     *   NP_Seglist    - BPTR seglist to run
     *   NP_StackSize  - stack size for process
     *   NP_Priority   - task priority (-128 to 127)
     *   NP_ArgPtr     - pointer to argument string
     *   NP_ArgLen     - length of argument string
     *   NP_Cwd        - current working directory
     *   SYS_Input     - input filehandle
     *   SYS_Output    - output filehandle
     *   SYS_Asynch    - if true, run asynchronously
     */
    uint32_t cmd_ptr = cpu->d[1];
    uint32_t tags_ptr = cpu->a[1];

    /* Extract tag values */
    uint32_t np_name = parse_tag_item((uint32_t *)(uintptr_t)tags_ptr, NP_Name);
    uint32_t np_seglist = parse_tag_item((uint32_t *)(uintptr_t)tags_ptr, NP_Seglist);
    /* A caller-supplied NP_Seglist is a BPTR (seglist addr >> 2); the
     * loadseg path below produces a raw address instead — track which we
     * have so the entry point and pr_SegList come out right either way. */
    int seglist_is_bptr = np_seglist != 0;
    uint32_t np_stacksize = parse_tag_item((uint32_t *)(uintptr_t)tags_ptr, NP_StackSize);
    uint32_t np_priority = parse_tag_item((uint32_t *)(uintptr_t)tags_ptr, NP_Priority);
    uint32_t np_argptr = parse_tag_item((uint32_t *)(uintptr_t)tags_ptr, NP_ArgPtr);
    uint32_t np_arglen = parse_tag_item((uint32_t *)(uintptr_t)tags_ptr, NP_ArgLen);
    uint32_t np_cwd = parse_tag_item((uint32_t *)(uintptr_t)tags_ptr, NP_Cwd);
    uint32_t sys_asynch = parse_tag_item((uint32_t *)(uintptr_t)tags_ptr, SYS_Asynch);

    /* If no seglist provided, try to load from command string */
    if (!np_seglist && cmd_ptr && cmd_ptr < GUEST_RAM_SIZE) {
        /* Read the command string */
        char cmd[256];
        int ci = 0;
        while (ci < 255 && cmd_ptr + ci < GUEST_RAM_SIZE && g_ram[cmd_ptr + ci]) {
            cmd[ci] = (char)g_ram[cmd_ptr + ci]; ci++;
        }
        cmd[ci] = '\0';

        /* Split into command name + arguments */
        char cmd_name[128];
        int ni = 0;
        while (cmd[ni] && cmd[ni] != ' ' && ni < 127) {
            cmd_name[ni] = cmd[ni]; ni++;
        }
        cmd_name[ni] = '\0';

        /* Look for binary and load seglist */
        static const char * const search_paths[] = {
            "SYS:C/",
            "RAM:",
            NULL
        };

        for (int pi = 0; search_paths[pi]; pi++) {
            char full_path[128];
            int fi = 0;
            const char *pfx = search_paths[pi];
            while (pfx[fi]) { full_path[fi] = pfx[fi]; fi++; }
            int ni2 = 0;
            while (cmd_name[ni2] && fi < 127) { full_path[fi++] = cmd_name[ni2++]; }
            full_path[fi] = '\0';

            VfsFile fh;
            if (!VFS_Open(&fh, full_path, VFS_READ)) continue;

            uint32_t bin_size = VFS_Size(&fh);
            if (!bin_size || bin_size > sizeof(g_loadseg_buf)) {
                VFS_Close(&fh);
                continue;
            }

            uint32_t rd = VFS_Read(&fh, g_loadseg_buf, bin_size);
            VFS_Close(&fh);
            if (rd != bin_size) continue;

            np_seglist = loadseg_hunk_load(g_loadseg_buf, bin_size);
            if (np_seglist) {
                seglist_track(np_seglist >> 2, np_seglist);
                seglist_is_bptr = 0;
                break;
            }
        }
    }

    if (!np_seglist) {
        cpu->d[0] = (uint32_t)DOSFALSE;
        SetIoErr(ERROR_OBJECT_NOT_FOUND);
        return;
    }

    /* Use defaults for optional parameters */
    if (!np_stacksize) np_stacksize = 8192;  /* Default 8KB stack */
    if (!np_priority) np_priority = 0;       /* Normal priority */

    /* Read process name */
    char proc_name[64] = "NewProc";
    if (np_name) {
        int ni = 0;
        while (ni < 63 && np_name + ni < GUEST_RAM_SIZE && g_ram[np_name + ni]) {
            proc_name[ni] = (char)g_ram[np_name + ni];
            ni++;
        }
        proc_name[ni] = '\0';
    }

    /* Build child stack + process struct, then queue for sequential run —
     * same model as dos_CreateProc (the child shares this address space). */
    if ((g_pending_tail + 1) % MAX_PENDING_PROC == g_pending_head) {
        cpu->d[0] = (uint32_t)DOSFALSE;
        SetIoErr(ERROR_TASK_TABLE_FULL);
        return;
    }

    uint32_t stack_low = heap_alloc_fl(np_stacksize);
    if (!stack_low) {
        cpu->d[0] = (uint32_t)DOSFALSE;
        SetIoErr(ERROR_NO_FREE_STORE);
        return;
    }
    uint32_t stack_top = stack_low + np_stacksize - 4;
    guest_write_be32(stack_top, DOS_EXIT_STUB);

    /* pr_SegList is stored as a BPTR; a caller-supplied seglist already is
     * one, while our loadseg result is a raw address needing >> 2. */
    uint32_t pr_seglist = seglist_is_bptr ? np_seglist : (np_seglist >> 2);
    uint32_t proc_addr = build_process_struct(proc_name, (int8_t)np_priority,
                                              pr_seglist,
                                              stack_top, stack_low);
    if (!proc_addr) {
        heap_free_fl(stack_low);
        cpu->d[0] = (uint32_t)DOSFALSE;
        SetIoErr(ERROR_NO_FREE_STORE);
        return;
    }

    /* The seglist's first longword is the next-BPTR link; code begins +4
     * past it.  A caller-supplied BPTR must be shifted back to an address. */
    uint32_t seg_addr = seglist_is_bptr ? (np_seglist << 2) : np_seglist;
    PendingProc *pp = &g_pending_procs[g_pending_tail];
    pp->entry     = seg_addr + 4;
    pp->stack_top = stack_top;
    pp->proc      = proc_addr;
    pp->proc_port = proc_addr + PR_MSGPORT;
    g_pending_tail = (g_pending_tail + 1) % MAX_PENDING_PROC;

    /* Stash arguments on the child's stack frame if provided */
    if (np_argptr && np_arglen) {
        uint32_t arg_buf = heap_alloc_fl(np_arglen + 1);
        if (arg_buf) {
            for (uint32_t i = 0; i < np_arglen && (np_argptr + i) < GUEST_RAM_SIZE; i++)
                g_ram[arg_buf + i] = g_ram[np_argptr + i];
            g_ram[arg_buf + np_arglen] = '\0';
            guest_write_be32(proc_addr + PR_ARGUMENTS, arg_buf);
        }
    }

    kprint("[dos] SystemTagList: queued process '");
    kprint(proc_name);
    kprint("'\n");
    /* Return process message port APTR */
    cpu->a[0] = proc_addr + PR_MSGPORT;
    /* V36+ System() returns 0 when the command was started (or an error
     * code).  Leaving D0 stale makes callers that test the result read
     * whatever was in D0 on entry. */
    cpu->d[0] = 0;
    (void)sys_asynch;  /* Synchronous execution for now */
    (void)np_cwd;      /* CWD handling not yet implemented */
}

/* =========================================================================
 * Packets (SendPkt / WaitPkt / ReplyPkt)
 * ========================================================================= */

static void dos_SendPkt(M68kCPUState *cpu)
{
    /* Amiga: A0=DosPacket*, A1=MsgPort* port, D1=MsgPort* replyport → D0=res */
    DosPacket *dp = (DosPacket *)(uintptr_t)cpu->a[0];
    MsgPort *port = (MsgPort *)(uintptr_t)cpu->a[1];
    (void)cpu->d[1];
    if (!dp || !port) {
        cpu->d[0] = 0;
        SetIoErr(ERROR_ACTION_NOT_KNOWN);
        return;
    }
    /* Synchronous dispatch */
    int32_t res = DoPkt(port, dp->dp_Type,
                        dp->dp_Arg1, dp->dp_Arg2, dp->dp_Arg3,
                        dp->dp_Arg4, dp->dp_Arg5);
    dp->dp_Res1 = res;
    dp->dp_Res2 = IoErr();
    cpu->d[0] = (uint32_t)res;
}

static void dos_WaitPkt(M68kCPUState *cpu)
{
    /* Amiga: → D0=DosPacket*  (async queue empty in single-threaded mode) */
    cpu->d[0] = 0;
}

static void dos_ReplyPkt(M68kCPUState *cpu)
{
    /* Amiga: A0=DosPacket*, D0=res1, D1=res2 */
    DosPacket *dp = (DosPacket *)(uintptr_t)cpu->a[0];
    if (dp) {
        dp->dp_Res1 = (int32_t)cpu->d[0];
        dp->dp_Res2 = (int32_t)cpu->d[1];
    }
}

/* =========================================================================
 * Path handling (AddPart / CompareNames)
 * ========================================================================= */

static void dos_AddPart(M68kCPUState *cpu)
{
    /* Amiga: D1=STRPTR dirname, D2=STRPTR filename, D3=ULONG size → D0=BOOL
     *
     * If the filename contains ':' it replaces the directory part: a
     * leading ':' writes over dirname's first ':' (keeping its volume
     * prefix), any other ':' restarts at the buffer start.  Otherwise the
     * filename is appended, with '/' inserted when dirname doesn't end in
     * '/' or ':'.  The whole result is validated against the buffer size
     * before anything is written; overflow fails with IoErr=120. */
    uint32_t dir_ptr = cpu->d[1];
    uint32_t file_ptr = cpu->d[2];
    uint32_t max_size = cpu->d[3];
    uint32_t file_len = 0, dest;
    int has_colon = 0, separator = 0;

    if (dir_ptr >= GUEST_RAM_SIZE || file_ptr >= GUEST_RAM_SIZE) {
        cpu->d[0] = (uint32_t)DOSFALSE;
        return;
    }

    while (file_ptr + file_len < GUEST_RAM_SIZE && g_ram[file_ptr + file_len]) {
        if (g_ram[file_ptr + file_len] == ':') has_colon = 1;
        ++file_len;
    }

    if (has_colon) {
        if (g_ram[file_ptr] == ':') {
            /* leading ':' keeps dirname's volume prefix */
            uint32_t c = dir_ptr;
            while (c < GUEST_RAM_SIZE && g_ram[c] && g_ram[c] != ':') ++c;
            dest = (c < GUEST_RAM_SIZE && g_ram[c] == ':') ? c : dir_ptr;
        } else {
            dest = dir_ptr;
        }
    } else {
        uint32_t end = dir_ptr;
        while (end < GUEST_RAM_SIZE && g_ram[end]) ++end;
        dest = end;
        if (end > dir_ptr && g_ram[end - 1] != ':' && g_ram[end - 1] != '/')
            separator = 1;
    }

    if (dest - dir_ptr + (uint32_t)separator + file_len + 1 > max_size) {
        SetIoErr(120);  /* ERROR_LINE_TOO_LONG */
        cpu->d[0] = (uint32_t)DOSFALSE;
        return;
    }

    if (separator)
        g_ram[dest++] = '/';
    for (uint32_t i = 0; i <= file_len; i++)
        g_ram[dest + i] = g_ram[file_ptr + i];
    cpu->d[0] = (uint32_t)DOSTRUE;
}

/* PathPart core: the parent path of `p` ends at the last '/' — except
 * when that slash is at the start or follows '/' or ':', in which case it
 * is part of the parent — or, with no '/', just after the first ':'. */
static uint32_t dos_path_part(uint32_t p)
{
    uint32_t last_slash = 0, s;
    if (p >= GUEST_RAM_SIZE) return p;
    for (s = p; s < GUEST_RAM_SIZE && g_ram[s]; s++)
        if (g_ram[s] == '/') last_slash = s;
    if (!last_slash) {
        for (s = p; s < GUEST_RAM_SIZE && g_ram[s]; s++)
            if (g_ram[s] == ':') return s + 1;
        return p;
    }
    if (last_slash == p || g_ram[last_slash - 1] == '/' ||
        g_ram[last_slash - 1] == ':')
        ++last_slash;
    return last_slash;
}

static void dos_FilePart(M68kCPUState *cpu)
{
    /* Amiga: D1=STRPTR path → D0 = ptr to filename component */
    uint32_t part = dos_path_part(cpu->d[1]);
    if (part < GUEST_RAM_SIZE && g_ram[part] == '/')
        ++part;
    cpu->d[0] = part;
}

static void dos_PathPart(M68kCPUState *cpu)
{
    /* Amiga: D1=STRPTR path → D0 = ptr just past the parent directory */
    cpu->d[0] = dos_path_part(cpu->d[1]);
}

static void dos_StrToLong(M68kCPUState *cpu)
{
    /* Amiga: D1=STRPTR string, D2=LONG *value → D0 = characters consumed
     * (including skipped whitespace), or -1 when no digits converted.
     * Accepts '-' only; accumulation stops cleanly at 32-bit overflow
     * boundaries. */
    uint32_t str = cpu->d[1], val_ptr = cpu->d[2], cursor = str;
    uint32_t acc = 0;
    int negative = 0, converted = 0;

    while (cursor < GUEST_RAM_SIZE &&
           (g_ram[cursor] == ' ' || g_ram[cursor] == '\t'))
        ++cursor;
    negative = (cursor < GUEST_RAM_SIZE && g_ram[cursor] == '-');
    if (negative)
        ++cursor;
    while (cursor < GUEST_RAM_SIZE &&
           g_ram[cursor] >= '0' && g_ram[cursor] <= '9') {
        uint32_t prev = acc, before;
        int32_t signed_prev = (int32_t)prev;
        acc <<= 3;
        if (signed_prev > 268435455 || signed_prev < -268435456)
            break;
        before = acc;
        acc += prev;
        if ((before ^ acc) & (prev ^ acc) & 0x80000000u)
            break;
        before = acc;
        acc += prev;
        if ((before ^ acc) & (prev ^ acc) & 0x80000000u)
            break;
        acc += (uint32_t)(g_ram[cursor] - '0');
        converted = 1;
        ++cursor;
    }
    guest_write_be32(val_ptr, negative ? 0u - acc : acc);
    cpu->d[0] = converted ? cursor - str : (uint32_t)-1;
}

static void dos_CompareNames(M68kCPUState *cpu)
{
    /* Amiga: D1=LONG type, D2=STRPTR name1, D3=STRPTR name2 → D0=LONG */
    int32_t type = (int32_t)cpu->d[1];
    uint32_t n1_ptr = cpu->d[2];
    uint32_t n2_ptr = cpu->d[3];

    char name1[128], name2[128];
    int i = 0;
    while (i < 127 && n1_ptr + i < GUEST_RAM_SIZE && g_ram[n1_ptr + i]) {
        name1[i] = (char)g_ram[n1_ptr + i]; i++;
    }
    name1[i] = '\0';

    i = 0;
    while (i < 127 && n2_ptr + i < GUEST_RAM_SIZE && g_ram[n2_ptr + i]) {
        name2[i] = (char)g_ram[n2_ptr + i]; i++;
    }
    name2[i] = '\0';

    int ci = (type != 0);
    int j = 0;
    while (name1[j] && name2[j]) {
        char c1 = name1[j];
        char c2 = name2[j];
        if (ci) {
            if (c1 >= 'A' && c1 <= 'Z') c1 += 32;
            if (c2 >= 'A' && c2 <= 'Z') c2 += 32;
        }
        if (c1 != c2) {
            cpu->d[0] = (c1 < c2) ? (uint32_t)-1 : 1;
            return;
        }
        j++;
    }
    if (name1[j] == name2[j]) {
        cpu->d[0] = 0;
    } else if (name1[j] == '\0') {
        cpu->d[0] = (uint32_t)-1;
    } else {
        cpu->d[0] = 1;
    }
}

/* =========================================================================
 * Date/Time (StrToDate)
 * ========================================================================= */

static void dos_StrToDate(M68kCPUState *cpu)
{
    /* Amiga: A0=struct DateTime *datetime → D0=BOOL
     *
     * DateTime layout:
     *  0 : DateStamp  (12 bytes)
     * 12 : ULONG dat_Format
     * 16 : ULONG dat_Flags
     * 20 : APTR dat_StrDay
     * 24 : APTR dat_StrDate
     * 28 : APTR dat_StrTime
     */
    uint32_t dt_ptr = cpu->a[0];
    if (dt_ptr + 32 > GUEST_RAM_SIZE) {
        cpu->d[0] = (uint32_t)DOSFALSE;
        return;
    }

    uint32_t str_date_ptr = guest_read_be32(dt_ptr + 24);
    uint32_t str_time_ptr = guest_read_be32(dt_ptr + 28);
    uint32_t format = guest_read_be32(dt_ptr + 12);

    int day = 1, month = 1, year = 78;
    int hour = 0, min = 0, sec = 0;

    /* Parse date string */
    if (str_date_ptr && str_date_ptr + 12 < GUEST_RAM_SIZE) {
        char ds[32];
        int k = 0;
        while (k < 31 && str_date_ptr + k < GUEST_RAM_SIZE && g_ram[str_date_ptr + k]) {
            ds[k] = (char)g_ram[str_date_ptr + k]; k++;
        }
        ds[k] = '\0';

        if (format == 1) { /* FORMAT_INTL / USA: MM-DD-YY */
            int v[3] = {0,0,0};
            int vi = 0, val = 0;
            for (int j = 0; ds[j] && vi < 3; j++) {
                char ch = ds[j];
                if (ch >= '0' && ch <= '9') {
                    val = val * 10 + (ch - '0');
                } else if (ch == '-' || ch == '/' || ch == '.') {
                    v[vi++] = val; val = 0;
                }
            }
            if (vi < 3) v[vi] = val;
            month = v[0]; day = v[1]; year = v[2];
        } else {
            /* FORMAT_DOS: DD-MMM-YY */
            day = (ds[0] - '0') * 10 + (ds[1] - '0');
            year = (ds[7] - '0') * 10 + (ds[8] - '0');
            char mon[4] = {0,0,0,0};
            mon[0] = ds[3]; mon[1] = ds[4]; mon[2] = ds[5];
            const char *mns[] = {"jan","feb","mar","apr","may","jun","jul","aug","sep","oct","nov","dec"};
            for (int m = 0; m < 12; m++) {
                char a = mon[0]; if (a >= 'A' && a <= 'Z') a += 32;
                char b = mns[m][0];
                if (a == b && mon[1] == mns[m][1] && mon[2] == mns[m][2]) {
                    month = m + 1; break;
                }
            }
        }
    }

    /* Parse time string */
    if (str_time_ptr && str_time_ptr + 10 < GUEST_RAM_SIZE) {
        char ts[16];
        int k = 0;
        while (k < 15 && str_time_ptr + k < GUEST_RAM_SIZE && g_ram[str_time_ptr + k]) {
            ts[k] = (char)g_ram[str_time_ptr + k]; k++;
        }
        ts[k] = '\0';
        hour = (ts[0] - '0') * 10 + (ts[1] - '0');
        min  = (ts[3] - '0') * 10 + (ts[4] - '0');
        sec  = (ts[6] - '0') * 10 + (ts[7] - '0');
    }

    /* Validate */
    if (day < 1 || day > 31 || month < 1 || month > 12 || year < 0 || year > 99 ||
        hour < 0 || hour > 23 || min < 0 || min > 59 || sec < 0 || sec > 59) {
        cpu->d[0] = (uint32_t)DOSFALSE;
        return;
    }

    /* Convert to DateStamp (days since 1-Jan-1978, minutes, ticks) */
    static const int16_t mdays[12] = {0,31,59,90,120,151,181,212,243,273,304,334};
    int y = year + 1900;
    if (y < 1978) y += 100;
    int leap = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)) ? 1 : 0;
    int days = (y - 1978) * 365;
    for (int ly = 1978; ly < y; ly++) {
        if ((ly % 4 == 0 && (ly % 100 != 0 || ly % 400 == 0))) days++;
    }
    days += mdays[month - 1];
    if (month > 2) days += leap;
    days += (day - 1);

    int total_ticks = ((hour * 60 + min) * 60 + sec) * 50;
    int minutes = total_ticks / 3000;
    int ticks = total_ticks % 3000;

    guest_write_be32(dt_ptr + 0, (uint32_t)days);
    guest_write_be32(dt_ptr + 4, (uint32_t)minutes);
    guest_write_be32(dt_ptr + 8, (uint32_t)ticks);

    cpu->d[0] = (uint32_t)DOSTRUE;
}

/* =========================================================================
 * Signals (CheckSignal / WaitForChar)
 * ========================================================================= */

static void dos_CheckSignal(M68kCPUState *cpu)
{
    /* AmigaOS: D0=ULONG mask → D0=ULONG received (and clears matched bits)
     *
     * Returns the subset of pending signals that intersect with mask, then
     * clears those bits from tc_SigRecvd so they are only reported once.
     * This is the non-blocking variant of exec.library Wait().
     */
    uint32_t mask = cpu->d[0];
    UaosTask *cur = Task_Current();
    if (!cur) { cpu->d[0] = 0; return; }

    /* Atomically read and clear matching bits */
    uint64_t fl = irq_save();
    uint32_t received = cur->tc_SigRecvd & mask;
    cur->tc_SigRecvd &= ~received;  /* Clear only the bits that were matched */
    irq_restore(fl);

    cpu->d[0] = received;
}

static void dos_WaitForChar(M68kCPUState *cpu)
{
    /* Amiga: D1=BPTR file, D2=ULONG timeout → D0=BOOL (0=timeout, -1=available) */
    uint32_t fh = cpu->d[1];
    (void)cpu->d[2];
    if (fh == DOS_STDIN_BPTR) {
        cpu->d[0] = (uint32_t)DOSTRUE;
    } else {
        cpu->d[0] = (uint32_t)DOSFALSE;
    }
}

/* =========================================================================
 * Advanced locks (NameFromLock / LockRecord / UnLockRecord)
 * ========================================================================= */

static void dos_NameFromLock(M68kCPUState *cpu)
{
    /* Amiga: D1=BPTR lock, D2=STRPTR buffer, D3=LONG len → D0=BOOL */
    uint32_t lock = cpu->d[1];
    uint32_t buf = cpu->d[2];
    int32_t len = (int32_t)cpu->d[3];

    if (buf >= GUEST_RAM_SIZE || len < 2) {
        cpu->d[0] = (uint32_t)DOSFALSE;
        return;
    }

    uint32_t handle = 0;
    if (!guest_read_filelock(lock, &handle, NULL) || handle == 0) {
        cpu->d[0] = (uint32_t)DOSFALSE;
        SetIoErr(ERROR_OBJECT_NOT_FOUND);
        return;
    }

    HandleEntry *ent = HandleTable_GetLockEntry(handle, NULL);
    if (!ent || ent->type != HTYPE_LOCK) {
        cpu->d[0] = (uint32_t)DOSFALSE;
        SetIoErr(ERROR_OBJECT_NOT_FOUND);
        return;
    }

    const char *path = ent->path;
    int i = 0;
    while (path[i] && i < len - 1 && buf + i < GUEST_RAM_SIZE) {
        g_ram[buf + i] = (uint8_t)path[i];
        i++;
    }
    g_ram[buf + i] = '\0';
    cpu->d[0] = (uint32_t)DOSTRUE;
}

static void dos_LockRecord(M68kCPUState *cpu)
{
    /* Amiga: D1=BPTR fh, D2=offset, D3=length, D4=mode, D5=timeout → D0=BOOL */
    /* Synchronous single-threaded system — records are never contested */
    (void)cpu;
    cpu->d[0] = (uint32_t)DOSTRUE;
}

static void dos_UnLockRecord(M68kCPUState *cpu)
{
    /* Amiga: D1=BPTR fh, D2=offset, D3=length */
    (void)cpu;
}

/* =========================================================================
 * CLI (GetConsoleTask / SetConsoleTask)
 * ========================================================================= */

static uint32_t g_console_task = 0;

static void dos_GetConsoleTask(M68kCPUState *cpu)
{
    /* Amiga: → D0=struct MsgPort*.  The canonical store is the running
     * process's pr_ConsoleTask (+0xA4) so each M68k task sees the console
     * port materialised in its own guest window (UAOS-237); the static is
     * only a fallback for pre-process contexts. */
    uint32_t proc = guest_read_be32(EXEC_BASE_GLUE + EXECBASE_THIS_TASK);
    cpu->d[0] = proc ? guest_read_be32(proc + PR_CONSOLETASK) : g_console_task;
}

static void dos_SetConsoleTask(M68kCPUState *cpu)
{
    /* Amiga: D1=struct MsgPort* */
    g_console_task = cpu->d[1];
    uint32_t proc = guest_read_be32(EXEC_BASE_GLUE + EXECBASE_THIS_TASK);
    if (proc) guest_write_be32(proc + PR_CONSOLETASK, cpu->d[1]);
}

/* =========================================================================
 * CurrentDir / ProgramDir / misc 2.x DOS calls (OctaMED bring-up set)
 * ========================================================================= */

/* Lock a host path and return a guest FileLock BPTR (0 on failure).
 * Path may still carry an assign prefix — resolve before dispatch. */
static uint32_t dos_lock_path(const char *path)
{
    char resolved[128];
    const char *p = path;
    if (VFS_ResolveAssignPath(path, resolved, sizeof(resolved)))
        p = resolved;
    char vol_name[16];
    extract_vol_name(p, vol_name, sizeof(vol_name));
    MsgPort *port = VFS_GetHandlerPort(vol_name);
    if (!port) return 0;
    int32_t h = DoPkt(port, ACTION_LOCATE_OBJECT, (intptr_t)p,
                      -2 /* SHARED_LOCK */, 0, 0, 0);
    if (!h) return 0;
    uint32_t bptr = guest_alloc_filelock((uint32_t)h, -2);
    if (!bptr) HandleTable_Free((uint32_t)h);
    return bptr;
}

/* PROGDIR: lock — lazily bound to the launch directory.  For now the
 * launch directory IS the cwd (the shell CDs or passes full paths). */
/* Per-task pr program dir lock — the BPTR addresses memory in the owning
 * task's guest RAM window, so it must not be shared across M68k tasks.
 * The static below only serves non-task contexts (pre-scheduler use). */
static uint32_t g_program_dir_boot = 0;

static uint32_t *program_dir_slot(void)
{
    extern UaosTask *Task_Current(void);
    UaosTask *cur = Task_Current();
    if (cur && cur->type == TASK_TYPE_M68K)
        return &cur->m68k_program_dir;
    return &g_program_dir_boot;
}

static void dos_CurrentDir(M68kCPUState *cpu)
{
    /* AmigaOS: D1=BPTR lock → D0=old lock; pr_CurrentDir at Process+0x98. */
    uint32_t lock = cpu->d[1];
    uint32_t proc = guest_read_be32(EXEC_BASE_GLUE + EXECBASE_THIS_TASK);
    uint32_t old  = proc ? guest_read_be32(proc + PR_CURRENTDIR) : 0;
    if (proc) guest_write_be32(proc + PR_CURRENTDIR, lock);

    /* Keep the host-side cwd string in sync so relative Opens follow. */
    if (lock) {
        uint32_t handle = 0;
        if (guest_read_filelock(lock, &handle, NULL) && handle) {
            HandleEntry *ent = HandleTable_GetLockEntry(handle, NULL);
            if (ent && ent->path[0]) {
                m68k_set_cur_cwd(ent->path);
            }
        }
    }
    cpu->d[0] = old;
}

static void dos_SetProgramDir(M68kCPUState *cpu)
{
    /* D1=BPTR lock → D0=old */
    uint32_t *slot = program_dir_slot();
    uint32_t old = *slot;
    *slot = cpu->d[1];
    cpu->d[0] = old;
}

static void dos_GetProgramDir(M68kCPUState *cpu)
{
    uint32_t *slot = program_dir_slot();
    if (!*slot)
        *slot = dos_lock_path(m68k_cur_cwd());
    cpu->d[0] = *slot;
}

static void dos_SetIoErr(M68kCPUState *cpu)
{
    /* D1=new result code → D0=old; also pr_Result2 of the current proc. */
    cpu->d[0] = (uint32_t)IoErr();
    SetIoErr((int32_t)cpu->d[1]);
    uint32_t proc = guest_read_be32(EXEC_BASE_GLUE + EXECBASE_THIS_TASK);
    if (proc) guest_write_be32(proc + PR_RESULT2, cpu->d[1]);
}

static void dos_Cli(M68kCPUState *cpu)
{
    /* → D0=1 if the process has a CLI struct */
    uint32_t proc = guest_read_be32(EXEC_BASE_GLUE + EXECBASE_THIS_TASK);
    cpu->d[0] = (proc && guest_read_be32(proc + PR_CLI)) ? DOSTRUE : DOSFALSE;
}

static void dos_FindCliProc(M68kCPUState *cpu)
{
    /* D1=process number → D0=struct Process* — return ThisTask for slot 1 */
    cpu->d[0] = (cpu->d[1] == 1)
        ? guest_read_be32(EXEC_BASE_GLUE + EXECBASE_THIS_TASK) : 0;
}

static void dos_WriteChars(M68kCPUState *cpu)
{
    /* D1=STRPTR buffer, D2=ULONG length → D0=chars written (console out) */
    uint32_t buf = cpu->d[1], len = cpu->d[2];
    if (buf + len >= GUEST_RAM_SIZE || len > 4096) { cpu->d[0] = 0; return; }
    char tmp[4097];
    uint32_t i;
    for (i = 0; i < len; i++) tmp[i] = (char)g_ram[buf + i];
    tmp[i] = '\0';
    kprint(tmp);
    cpu->d[0] = len;
}

static void dos_SelectInput(M68kCPUState *cpu)
{
    /* D1=fh → D0=old input fh (updates pr_CIS) */
    uint32_t proc = guest_read_be32(EXEC_BASE_GLUE + EXECBASE_THIS_TASK);
    uint32_t old  = proc ? guest_read_be32(proc + PR_CIS) : DOS_STDIN_BPTR;
    if (proc) guest_write_be32(proc + PR_CIS, cpu->d[1]);
    cpu->d[0] = old;
}

static void dos_SelectOutput(M68kCPUState *cpu)
{
    uint32_t proc = guest_read_be32(EXEC_BASE_GLUE + EXECBASE_THIS_TASK);
    uint32_t old  = proc ? guest_read_be32(proc + PR_COS) : DOS_STDOUT_BPTR;
    if (proc) guest_write_be32(proc + PR_COS, cpu->d[1]);
    cpu->d[0] = old;
}

static void dos_FreeArgs(M68kCPUState *cpu)
{
    /* D1=RDArgs — our ReadArgs used a fixed guest buffer; nothing to free */
    cpu->d[0] = DOSTRUE;
}

static void dos_Flush(M68kCPUState *cpu)
{
    cpu->d[0] = DOSTRUE;
}

static void dos_Execute(M68kCPUState *cpu)
{
    /* D1=command string, D2=input fh, D3=output fh — script/command
     * execution isn't routed through the shell yet. */
    (void)cpu;
    kprint("[dos] Execute() not implemented\n");
    cpu->d[0] = DOSFALSE;
    SetIoErr(ERROR_ACTION_NOT_KNOWN);
}

static void dos_DeviceProc(M68kCPUState *cpu)
{
    /* D1=BSTR device/volume name → D0=APTR MsgPort of the handler.
     * Handler ports are host-side objects; expose a sentinel so callers
     * can distinguish "mounted" from "not mounted".  The name may be an
     * assign ("S:") — resolve to the backing volume first (UAOS-248). */
    char name[64], resolved[128], vol[16];
    dos_arg_to_c(cpu->d[1], name, sizeof(name));
    MsgPort *port = NULL;
    if (dos_resolve_path(name, resolved, sizeof(resolved)) &&
        extract_vol_name(resolved, vol, sizeof(vol)))
        port = VFS_GetHandlerPort(vol);
    cpu->d[0] = port ? 0xFFFFFFFFu : 0;   /* guest can test for NULL */
    if (!port) SetIoErr(ERROR_DEVICE_NOT_MOUNTED);
}

/* =========================================================================
 * CreateSegList stub
 * ========================================================================= */

static void dos_CreateSegList(M68kCPUState *cpu)
{
    /* AmigaOS (3.9+): A0=APTR codeStart, D0=ULONG codeSize → D0=BPTR seglist
     *
     * Wraps a raw M68k code buffer in a single-segment seglist so it can be
     * passed to CreateProc() or RunCommand().  The seglist header (4 bytes)
     * holds the next-BPTR (0 = last) followed by the code.
     *
     * This is not part of the original AmigaOS 3.1 API (it was added later)
     * but several third-party tools try to call it.  We allocate a block from
     * the free-list allocator, copy the code, and return a BPTR.
     */
    uint32_t code_ptr  = cpu->a[0];
    uint32_t code_size = cpu->d[0];

    if (!code_ptr || !code_size || code_ptr + code_size > GUEST_RAM_SIZE) {
        cpu->d[0] = 0;
        SetIoErr(ERROR_OBJECT_NOT_FOUND);
        return;
    }

    /* Allocate: 4-byte header + code */
    uint32_t total = 4 + code_size;
    uint32_t blk = heap_alloc_fl(total);
    if (!blk) {
        cpu->d[0] = 0;
        SetIoErr(ERROR_NO_FREE_STORE);
        return;
    }

    /* next-BPTR = 0 (single segment) */
    guest_write_be32(blk + 0, 0);

    /* Copy code into the block */
    for (uint32_t i = 0; i < code_size && blk + 4 + i < GUEST_RAM_SIZE; i++)
        g_ram[blk + 4 + i] = g_ram[code_ptr + i];

    /* Track so UnLoadSeg can free it */
    seglist_track(blk >> 2, blk);

    cpu->d[0] = blk >> 2;  /* return BPTR */
}

/* =========================================================================
 * UAOS-248 — OctaMED gap-fill
 * Lock/fh metadata, assigns, DosList, and buffered I/O
 * ========================================================================= */

/* Native path for the task's program-dir lock (cwd when unset). */
static uint32_t dos_program_dir_path(char *dst, int max)
{
    uint32_t lock = *program_dir_slot();
    HandleEntry *ent = lock ? dos_lock_entry(lock) : NULL;
    const char *src = (ent && ent->path[0]) ? ent->path : m68k_cur_cwd();
    int i = 0;
    while (i < max - 1 && src && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
    return (uint32_t)i;
}

/* Marshal a native (little-endian) FileInfoBlock into guest RAM
 * big-endian.  Handlers write C structs — the guest expects Amiga layout:
 *   DiskKey 0, DirEntryType 4, FileName 8..115, Protection 116,
 *   EntryType 120, Size 124, NumBlocks 128, DateStamp 132/136/140,
 *   Comment 144..223, OwnerUID 224, OwnerGID 226, Reserved 228..259. */
static void dos_fib_to_guest(uint32_t dst, const FileInfoBlock *f)
{
    for (int i = 0; i < 260; i++) g_ram[dst + i] = 0;
    guest_write_be32(dst + 0,   (uint32_t)f->fib_DiskKey);
    guest_write_be32(dst + 4,   (uint32_t)f->fib_DirEntryType);
    for (int i = 0; i < 108; i++) g_ram[dst + 8 + i] = (uint8_t)f->fib_FileName[i];
    guest_write_be32(dst + 116, (uint32_t)f->fib_Protection);
    guest_write_be32(dst + 120, (uint32_t)f->fib_EntryType);
    guest_write_be32(dst + 124, (uint32_t)f->fib_Size);
    guest_write_be32(dst + 128, (uint32_t)f->fib_NumBlocks);
    guest_write_be32(dst + 132, (uint32_t)f->fib_Date.ds_Days);
    guest_write_be32(dst + 136, (uint32_t)f->fib_Date.ds_Minute);
    guest_write_be32(dst + 140, (uint32_t)f->fib_Date.ds_Tick);
    for (int i = 0; i < 80; i++) g_ram[dst + 144 + i] = (uint8_t)f->fib_Comment[i];
    g_ram[dst + 224] = (uint8_t)(f->fib_OwnerUID >> 8);
    g_ram[dst + 225] = (uint8_t)f->fib_OwnerUID;
    g_ram[dst + 226] = (uint8_t)(f->fib_OwnerGID >> 8);
    g_ram[dst + 227] = (uint8_t)f->fib_OwnerGID;
}

/* Examine-like packet call: handler fills a native fib, marshal to guest. */
static int32_t dos_examine_pkt(MsgPort *port, int32_t action,
                               int32_t handle, uint32_t fib_guest)
{
    if (!fib_guest || fib_guest + 260 > GUEST_RAM_SIZE) {
        SetIoErr(ERROR_OBJECT_NOT_FOUND);
        return DOSFALSE;
    }
    FileInfoBlock fib;
    memset(&fib, 0, sizeof(fib));
    int32_t res = DoPkt(port, action, handle, (intptr_t)&fib, 0, 0, 0);
    if (res == DOSTRUE) dos_fib_to_guest(fib_guest, &fib);
    return res;
}

/* --- Info (LVO -114): D1=lock BPTR, D2=InfoData* ----------------------- */
static void dos_Info(M68kCPUState *cpu)
{
    cpu->d[0] = DOSFALSE;
    uint32_t idp = cpu->d[2];
    if (!idp || idp + 36 > GUEST_RAM_SIZE) {
        SetIoErr(ERROR_OBJECT_NOT_FOUND);
        return;
    }
    uint32_t handle = 0;
    if (!guest_read_filelock(cpu->d[1], &handle, NULL) || !handle) {
        SetIoErr(ERROR_OBJECT_NOT_FOUND);
        return;
    }
    HandleEntry *ent = HandleTable_GetLockEntry(handle, NULL);
    if (!ent) { SetIoErr(ERROR_OBJECT_NOT_FOUND); return; }
    MsgPort *port = dos_lock_port(ent);
    if (!port) { SetIoErr(ERROR_DEVICE_NOT_MOUNTED); return; }
    cpu->d[0] = (uint32_t)DoPkt(port, ACTION_INFO, (int32_t)handle,
                                (intptr_t)(g_ram + idp), 0, 0, 0);
}

/* --- IsFileSystem (LVO -708): D1=name → D0 ---------------------------- */
static void dos_IsFileSystem(M68kCPUState *cpu)
{
    char name[64], resolved[128], vol[16];
    dos_arg_to_c(cpu->d[1], name, sizeof(name));
    cpu->d[0] = DOSFALSE;
    if (!dos_resolve_path(name, resolved, sizeof(resolved)) ||
        !extract_vol_name(resolved, vol, sizeof(vol)))
        return;
    MsgPort *port = VFS_GetHandlerPort(vol);
    if (!port) return;
    cpu->d[0] = (uint32_t)DoPkt(port, ACTION_IS_FILESYSTEM,
                                (intptr_t)resolved, 0, 0, 0, 0);
}

/* --- Inhibit (LVO -726): D1=name D2=onoff → D0 ------------------------ */
static void dos_Inhibit(M68kCPUState *cpu)
{
    char name[64], resolved[128], vol[16];
    dos_arg_to_c(cpu->d[1], name, sizeof(name));
    cpu->d[0] = DOSFALSE;
    if (!dos_resolve_path(name, resolved, sizeof(resolved)) ||
        !extract_vol_name(resolved, vol, sizeof(vol))) {
        SetIoErr(ERROR_DEVICE_NOT_MOUNTED);
        return;
    }
    MsgPort *port = VFS_GetHandlerPort(vol);
    if (!port) { SetIoErr(ERROR_DEVICE_NOT_MOUNTED); return; }
    cpu->d[0] = (uint32_t)DoPkt(port, ACTION_INHIBIT,
                                (intptr_t)resolved, (intptr_t)cpu->d[2], 0, 0, 0);
}

/* --- SetComment (LVO -180): D1=name D2=comment → D0 ------------------- */
static void dos_SetComment(M68kCPUState *cpu)
{
    char name[128], comment[80];
    dos_arg_to_c(cpu->d[1], name, sizeof(name));
    dos_arg_to_c(cpu->d[2], comment, sizeof(comment));
    int32_t res = dos_path_pkt(name, ACTION_SET_COMMENT,
                               (intptr_t)comment, 0, NULL, 0, NULL);
    cpu->d[0] = (uint32_t)res;
    if (res == DOSFALSE && !IoErr()) SetIoErr(ERROR_OBJECT_NOT_FOUND);
}

/* --- SameLock (LVO -420): D1,D2=lock BPTRs → D0 ------------------------ */
static void dos_SameLock(M68kCPUState *cpu)
{
    HandleEntry *e1 = dos_lock_entry(cpu->d[1]);
    HandleEntry *e2 = dos_lock_entry(cpu->d[2]);
    cpu->d[0] = DOSFALSE;
    if (!e1 || !e2) { SetIoErr(ERROR_OBJECT_NOT_FOUND); return; }
    /* Same backing node is the strongest identity (covers DupLock'd
     * locks whose stored path may differ from the original's). */
    if (e1->u.lock.node && e1->u.lock.node == e2->u.lock.node) {
        cpu->d[0] = DOSTRUE;
        return;
    }
    if (!e1->path[0] || !e2->path[0]) return;
    /* Lock entries carry canonical paths — case-fold compare is enough. */
    int i = 0;
    for (;;) {
        char a = e1->path[i], b = e2->path[i];
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) { cpu->d[0] = DOSFALSE; return; }
        if (!a) { cpu->d[0] = DOSTRUE; return; }
        i++;
    }
}

/* --- SetFileSize (LVO -456): D1=fh D2=pos D3=mode → D0=new size ------- */
static void dos_SetFileSize(M68kCPUState *cpu)
{
    uint32_t fh = cpu->d[1];
    cpu->d[0] = (uint32_t)-1;
    HandleEntry *ent = HandleTable_Get(fh);
    if (!ent || ent->type != HTYPE_FILE) {
        SetIoErr(ERROR_OBJECT_NOT_FOUND);
        return;
    }
    int32_t res = dos_fh_dispatch(ent, fh, ACTION_SET_FILE_SIZE,
                                  (intptr_t)(int32_t)cpu->d[2],
                                  (intptr_t)(int32_t)cpu->d[3]);
    cpu->d[0] = (uint32_t)res;
    KLOG(KLOG_DOS, KLOG_INFO,
         "[dos] SetFileSize(fh=%x pos=%ld mode=%ld) -> %ld ioerr=%ld\n",
         (unsigned)fh, (long)(int32_t)cpu->d[2], (long)(int32_t)cpu->d[3],
         (long)res, (long)IoErr());
}

/* --- ExamineFH (LVO -390): D1=fh D2=fib → D0 -------------------------- */
static void dos_ExamineFH(M68kCPUState *cpu)
{
    uint32_t fh = cpu->d[1], fib = cpu->d[2];
    cpu->d[0] = DOSFALSE;
    HandleEntry *ent = HandleTable_Get(fh);
    if (!ent || ent->type != HTYPE_FILE) {
        SetIoErr(ERROR_OBJECT_NOT_FOUND);
        return;
    }
    VfsFile *vf = &ent->u.file.fh;
    MsgPort *port = vf->handler_port;
    int32_t h = (int32_t)vf->handle_id;
    if (!port) {
        char vol[16];
        extract_vol_name(ent->path, vol, sizeof(vol));
        port = VFS_GetHandlerPort(vol);
        h = (int32_t)fh;
    }
    if (!port) { SetIoErr(ERROR_DEVICE_NOT_MOUNTED); return; }
    cpu->d[0] = (uint32_t)dos_examine_pkt(port, ACTION_EXAMINE_FH, h, fib);
}

/* --- ParentOfFH (LVO -384): D1=fh → D0=lock BPTR ----------------------- */
static void dos_ParentOfFH(M68kCPUState *cpu)
{
    uint32_t fh = cpu->d[1];
    cpu->d[0] = 0;
    HandleEntry *ent = HandleTable_Get(fh);
    if (!ent || ent->type != HTYPE_FILE) {
        SetIoErr(ERROR_OBJECT_NOT_FOUND);
        return;
    }
    int32_t ph = dos_fh_dispatch(ent, fh, ACTION_PARENT_FH, 0, 0);
    if (ph <= 0) {
        SetIoErr(IoErr() ? IoErr() : ERROR_OBJECT_NOT_FOUND);
        return;
    }
    uint32_t bptr = guest_alloc_filelock((uint32_t)ph, -2);
    if (!bptr) { HandleTable_Free((uint32_t)ph); SetIoErr(ERROR_NO_FREE_STORE); return; }
    cpu->d[0] = bptr;
}

/* --- DupLockFromFH (LVO -372): D1=fh → D0=lock BPTR -------------------- */
static void dos_DupLockFromFH(M68kCPUState *cpu)
{
    uint32_t fh = cpu->d[1];
    cpu->d[0] = 0;
    HandleEntry *ent = HandleTable_Get(fh);
    if (!ent || ent->type != HTYPE_FILE || !ent->path[0]) {
        SetIoErr(ERROR_OBJECT_NOT_FOUND);
        return;
    }
    char resolved[128];
    VFS_ResolveAssignPath(ent->path, resolved, sizeof(resolved));
    cpu->d[0] = dos_lock_path(resolved);
    if (!cpu->d[0]) SetIoErr(ERROR_OBJECT_NOT_FOUND);
}

/* --- OpenFromLock (LVO -378): D1=lock → D0=fh ------------------------- */
static void dos_OpenFromLock(M68kCPUState *cpu)
{
    uint32_t lock = cpu->d[1];
    cpu->d[0] = 0;
    HandleEntry *ent = dos_lock_entry(lock);
    if (!ent || !ent->path[0]) {
        SetIoErr(ERROR_OBJECT_NOT_FOUND);
        return;
    }
    VfsFile vf = {0};
    if (!VFS_Open(&vf, ent->path, VFS_READ | VFS_WRITE)) {
        /* Objects that only exist for reads still open MODE_OLDFILE. */
        if (!VFS_Open(&vf, ent->path, VFS_READ)) {
            SetIoErr(IoErr() ? IoErr() : ERROR_OBJECT_NOT_FOUND);
            return;
        }
    }
    const char *fpath = vf.resolved_path[0] ? vf.resolved_path : ent->path;
    uint32_t handle = HandleTable_AllocFile(fpath, &vf,
                                            VFS_READ | VFS_WRITE);
    if (!handle) { VFS_Close(&vf); SetIoErr(ERROR_NO_FREE_STORE); return; }
    cpu->d[0] = handle;
}

/* --- ExAll (LVO -432) / ExAllEnd (LVO -990) -----------------------------
 * ExAllData sizes per data type: ED_NAME 8, ED_TYPE 12, ED_SIZE 16,
 * ED_PROTECTION 20, ED_DATE 32, ED_COMMENT 36, ED_OWNER 40.
 * ExAllControl: eac_Entries 0, eac_LastKey 4, eac_MatchString 8,
 * eac_MatchFunc 12.  Name/comment strings are BSTRs carved from the
 * free-list heap; ExAllEnd (or unlock) releases them. */
static const uint8_t exall_sizes[8] = { 0, 8, 12, 16, 20, 32, 36, 40 };

static uint32_t g_exall_strs[256];
static int      g_exall_str_count;

static uint32_t exall_bstr(const char *s)
{
    int len = 0;
    while (s[len] && len < 255) len++;
    uint32_t addr = heap_alloc_fl(4 + ((uint32_t)len + 1 + 3) / 4 * 4);
    if (!addr) return 0;
    g_ram[addr] = (uint8_t)len;
    for (int i = 0; i < len; i++) g_ram[addr + 1 + i] = (uint8_t)s[i];
    if (g_exall_str_count < 256)
        g_exall_strs[g_exall_str_count++] = addr;
    return addr >> 2;
}

static void exall_free_strs(void)
{
    for (int i = 0; i < g_exall_str_count; i++)
        heap_free_fl(g_exall_strs[i]);
    g_exall_str_count = 0;
}

static void dos_ExAll(M68kCPUState *cpu)
{
    uint32_t lock = cpu->d[1];
    uint32_t buf  = cpu->d[2];
    uint32_t size = cpu->d[3];
    int32_t  type = cpu->d[4];
    uint32_t ctrl = cpu->d[5];   /* ExAllControl* is D5 per the .fd */
    cpu->d[0] = DOSFALSE;
    if (type < 1 || type > 7 || !buf || !ctrl ||
        ctrl + 16 > GUEST_RAM_SIZE || buf >= GUEST_RAM_SIZE) {
        SetIoErr(ERROR_BAD_NUMBER);
        return;
    }
    uint32_t esz = exall_sizes[type];

    uint32_t handle = 0;
    if (!guest_read_filelock(lock, &handle, NULL) || !handle) {
        SetIoErr(ERROR_OBJECT_NOT_FOUND);
        return;
    }
    HandleEntry *ent = HandleTable_GetLockEntry(handle, NULL);
    if (!ent) { SetIoErr(ERROR_OBJECT_NOT_FOUND); return; }
    MsgPort *port = dos_lock_port(ent);
    if (!port) { SetIoErr(ERROR_DEVICE_NOT_MOUNTED); return; }

    uint32_t p = buf;
    uint32_t n = 0;
    while (p + esz <= buf + size && p + esz <= GUEST_RAM_SIZE) {
        FileInfoBlock fib;
        memset(&fib, 0, sizeof(fib));
        int32_t res = DoPkt(port, ACTION_EXAMINE_ALL, (int32_t)handle,
                            (intptr_t)&fib, 0, 0, 0);
        if (res != DOSTRUE) break;
        uint32_t nb = exall_bstr(fib.fib_FileName);
        guest_write_be32(p + 4, nb);
        if (type >= 2) guest_write_be32(p + 8, (uint32_t)fib.fib_DirEntryType);
        if (type >= 3) guest_write_be32(p + 12, (uint32_t)fib.fib_Size);
        if (type >= 4) guest_write_be32(p + 16, (uint32_t)fib.fib_Protection);
        if (type >= 5) {
            guest_write_be32(p + 20, (uint32_t)fib.fib_Date.ds_Days);
            guest_write_be32(p + 24, (uint32_t)fib.fib_Date.ds_Minute);
            guest_write_be32(p + 28, (uint32_t)fib.fib_Date.ds_Tick);
        }
        if (type >= 6) guest_write_be32(p + 32, exall_bstr(fib.fib_Comment));
        if (type >= 7) {
            g_ram[p + 36] = 0; g_ram[p + 37] = 0;
            g_ram[p + 38] = 0; g_ram[p + 39] = 0;
        }
        guest_write_be32(p + 0, p + esz);   /* ed_Next (APTR) */
        p += esz;
        n++;
    }
    if (n)
        guest_write_be32(p - esz, 0);       /* terminate list */
    guest_write_be32(ctrl + 0, guest_read_be32(ctrl + 0) + n); /* eac_Entries */
    guest_write_be32(ctrl + 4, n ? p : guest_read_be32(ctrl + 4)); /* eac_LastKey */
    cpu->d[0] = n ? DOSTRUE : DOSFALSE;
    if (!n) SetIoErr(ERROR_NO_MORE_ENTRIES);
    KLOG(KLOG_DOS, KLOG_INFO, "[dos] ExAll(lock=%u buf=%x type=%ld) -> %ld\n",
         lock, (unsigned)buf, (long)type, (long)n);
}

static void dos_ExAllEnd(M68kCPUState *cpu)
{
    /* D1=lock D2=buf D3=size D4=data D5=control → free entry strings. */
    (void)cpu;
    exall_free_strs();
    cpu->d[0] = DOSTRUE;
}

/* --- Assigns (LVO -612/-618/-624/-630) ---------------------------------
 * AssignLock steals the caller's lock; AssignAdd appends the locked dir
 * as a new target; AssignLate is deferred; AssignPath resolves now. */
static void dos_assign_common(M68kCPUState *cpu, int add, int defer)
{
    char name[32], path[128];
    dos_arg_to_c(cpu->d[1], name, sizeof(name));
    cpu->d[0] = DOSFALSE;
    if (!name[0]) { SetIoErr(ERROR_OBJECT_WRONG_TYPE); return; }

    if (defer >= 0) {
        /* Late/Path: D2 is a path STRPTR/BSTR. */
        dos_arg_to_c(cpu->d[2], path, sizeof(path));
        if (!path[0]) { SetIoErr(ERROR_OBJECT_NOT_FOUND); return; }
    } else {
        /* Lock/Add: D2 is a FileLock BPTR whose path we adopt. */
        uint32_t lock = cpu->d[2];
        if (!lock) {
            cpu->d[0] = (VFS_RemoveAssign(name) == 0) ? DOSTRUE : DOSFALSE;
            return;
        }
        HandleEntry *ent = dos_lock_entry(lock);
        if (!ent || !ent->path[0]) {
            SetIoErr(ERROR_OBJECT_NOT_FOUND);
            return;
        }
        int i = 0;
        while (i < 127 && ent->path[i]) { path[i] = ent->path[i]; i++; }
        path[i] = '\0';
    }

    char resolved[128];
    const char *tgt = path;
    if (VFS_ResolveAssignPath(path, resolved, sizeof(resolved)))
        tgt = resolved;
    cpu->d[0] = (VFS_AddAssign(name, tgt, add, defer > 0) == 0)
                ? DOSTRUE : DOSFALSE;
    KLOG(KLOG_DOS, KLOG_INFO, "[dos] Assign(%s -> %s) add=%d defer=%d -> %ld\n",
         name, tgt, add, defer, (long)cpu->d[0]);
}

static void dos_AssignLock(M68kCPUState *cpu)
{
    dos_assign_common(cpu, 0, -1);
    if ((int32_t)cpu->d[0] == DOSTRUE) {
        /* The assign consumed the lock — drop the handle like the real
         * AssignLock does (it takes ownership). */
        uint32_t handle = 0;
        if (guest_read_filelock(cpu->d[2], &handle, NULL) && handle)
            VFS_FreeLock(handle);
    }
}
static void dos_AssignLate(M68kCPUState *cpu) { dos_assign_common(cpu, 0, 1); }
static void dos_AssignPath(M68kCPUState *cpu) { dos_assign_common(cpu, 0, 0); }
static void dos_AssignAdd(M68kCPUState *cpu)  { dos_assign_common(cpu, 1, -1); }

/* --- GetDeviceProc / FreeDeviceProc (LVO -642/-648) --------------------
 * DevProc { dvp_Port 0, dvp_Lock 4, dvp_Flags 8, dvp_DevNode 12 }.  We
 * hand out a 16-byte guest struct; dvp_Port is a sentinel (host ports are
 * not guest objects) and dvp_Lock is a real lock on the resolved dir. */
static uint32_t g_devprocs[32];
static int      g_devproc_count;

static void dos_GetDeviceProc(M68kCPUState *cpu)
{
    char name[64], resolved[128], vol[16];
    dos_arg_to_c(cpu->d[1], name, sizeof(name));
    uint32_t dvp = cpu->d[2];
    int alloc = (dvp == 0);
    cpu->d[0] = 0;

    if (alloc) {
        dvp = heap_alloc_fl(16);
        if (!dvp) { SetIoErr(ERROR_NO_FREE_STORE); return; }
    } else if (dvp + 16 > GUEST_RAM_SIZE) {
        SetIoErr(ERROR_OBJECT_NOT_FOUND);
        return;
    }

    MsgPort *port = NULL;
    uint32_t lock = 0;
    if (dos_resolve_path(name, resolved, sizeof(resolved)) &&
        extract_vol_name(resolved, vol, sizeof(vol))) {
        port = VFS_GetHandlerPort(vol);
        if (port) lock = dos_lock_path(resolved);
    }
    if (!port) {
        if (alloc) heap_free_fl(dvp);
        SetIoErr(ERROR_DEVICE_NOT_MOUNTED);
        return;
    }
    guest_write_be32(dvp + 0, 0xFFFFFFFFu);   /* sentinel port */
    guest_write_be32(dvp + 4, lock);
    guest_write_be32(dvp + 8, 0);
    guest_write_be32(dvp + 12, 0);
    if (alloc && g_devproc_count < 32)
        g_devprocs[g_devproc_count++] = dvp;
    cpu->d[0] = dvp;
}

static void dos_FreeDeviceProc(M68kCPUState *cpu)
{
    uint32_t dvp = cpu->d[1];
    cpu->d[0] = DOSFALSE;
    if (!dvp || dvp + 16 > GUEST_RAM_SIZE) return;
    uint32_t lock = guest_read_be32(dvp + 4);
    if (lock) {
        uint32_t handle = 0;
        if (guest_read_filelock(lock, &handle, NULL) && handle)
            VFS_FreeLock(handle);
    }
    for (int i = 0; i < g_devproc_count; i++) {
        if (g_devprocs[i] == dvp) {
            heap_free_fl(dvp);
            g_devprocs[i] = g_devprocs[--g_devproc_count];
            break;
        }
    }
    cpu->d[0] = DOSTRUE;
}

/* --- DosList iteration (LVO -654/-660/-666/-690) ------------------------
 * Guest-visible DosList node (44 bytes):
 *   dol_Next 0 (BPTR), dol_Type 4, dol_Task 8, dol_Lock 12,
 *   union 16..39 (zeros), dol_Name 40 (BSTR BPTR).
 * LockDosList builds a snapshot chain in the free-list heap; nodes live
 * until UnlockDosList frees them. */
#define GDOL_NODE_SIZE 48

static uint32_t g_dol_nodes[64];
static int      g_dol_count;

static uint32_t doslist_bstr(const char *s)
{
    int len = 0;
    while (s[len] && len < 30) len++;
    uint32_t addr = heap_alloc_fl(4 + ((uint32_t)len + 1 + 3) / 4 * 4);
    if (!addr) return 0;
    g_ram[addr] = (uint8_t)len;
    for (int i = 0; i < len; i++) g_ram[addr + 1 + i] = (uint8_t)s[i];
    return addr >> 2;   /* BSTR BPTR */
}

static void dos_LockDosList(M68kCPUState *cpu)
{
    uint32_t flags = cpu->d[1];
    /* LDF_DEVICES 1, LDF_VOLUMES 2, LDF_ASSIGNS 4.  0 = all. */
    uint32_t head = 0, tail = 0;
    DosList *n = NULL;
    while ((n = DosList_Next(n)) != NULL) {
        uint32_t gtype;
        if (n->dol_Type == DLT_DEVICE) {
            if (flags && !(flags & 1)) continue;
            gtype = 1;
        } else if (n->dol_Type == DLT_VOLUME) {
            if (flags && !(flags & 2)) continue;
            gtype = 2;
        } else if (n->dol_Type == DLT_ASSIGN) {
            if (flags && !(flags & 4)) continue;
            /* bound assign = directory link, unbound = non-binding */
            gtype = n->u.dol_Assign.dol_Lock ? 0 : 4;
        } else {
            continue;
        }
        if (g_dol_count >= 64) break;
        uint32_t node = heap_alloc_fl(GDOL_NODE_SIZE);
        if (!node) break;
        for (int i = 0; i < GDOL_NODE_SIZE; i++) g_ram[node + i] = 0;
        guest_write_be32(node + 4, gtype);
        guest_write_be32(node + 40, doslist_bstr(n->dol_Name));
        g_dol_nodes[g_dol_count++] = node;
        if (!head) head = node;
        if (tail) guest_write_be32(tail + 0, node >> 2);
        tail = node;
    }
    cpu->d[0] = head;
}

static void dos_UnlockDosList(M68kCPUState *cpu)
{
    (void)cpu;
    for (int i = 0; i < g_dol_count; i++) {
        uint32_t node = g_dol_nodes[i];
        uint32_t nb = guest_read_be32(node + 40);
        if (nb) heap_free_fl(nb << 2);
        heap_free_fl(node);
    }
    g_dol_count = 0;
}

static void dos_AttemptLockDosList(M68kCPUState *cpu) { dos_LockDosList(cpu); }

static void dos_NextDosEntry(M68kCPUState *cpu)
{
    /* D1=APTR node → D0=APTR next */
    uint32_t node = cpu->d[1];
    cpu->d[0] = 0;
    if (!node || node + GDOL_NODE_SIZE > GUEST_RAM_SIZE) return;
    uint32_t nb = guest_read_be32(node + 0);
    cpu->d[0] = nb ? (nb << 2) : 0;
}

/* --- Buffered I/O (LVO -306..-366) --------------------------------------
 * UAOS files are unbuffered at the VFS layer; these are thin wrappers so
 * guests calling the BCPL-style API still work. */
static void dos_FGetC(M68kCPUState *cpu)
{
    uint32_t fh = cpu->d[1];
    cpu->d[0] = (uint32_t)-1;
    HandleEntry *ent = HandleTable_Get(fh);
    if (!ent || ent->type != HTYPE_FILE) return;
    uint8_t ch;
    if (VFS_Read(&ent->u.file.fh, &ch, 1) == 1) cpu->d[0] = ch;
}

static void dos_FPutC(M68kCPUState *cpu)
{
    uint32_t fh = cpu->d[1];
    uint8_t ch = (uint8_t)cpu->d[2];
    cpu->d[0] = (uint32_t)-1;
    HandleEntry *ent = HandleTable_Get(fh);
    if (!ent || ent->type != HTYPE_FILE) {
        if (fh == DOS_STDOUT_BPTR) {
            char s[2] = { (char)ch, 0 };
            kprint(s);
            cpu->d[0] = ch;
        }
        return;
    }
    if (VFS_Write(&ent->u.file.fh, &ch, 1) == 1) cpu->d[0] = ch;
}

static void dos_UnGetC(M68kCPUState *cpu)
{
    uint32_t fh = cpu->d[1];
    int32_t ch = (int32_t)cpu->d[2];
    cpu->d[0] = (uint32_t)-1;
    HandleEntry *ent = HandleTable_Get(fh);
    if (!ent || ent->type != HTYPE_FILE) return;
    VfsFile *f = &ent->u.file.fh;
    if (f->pos > 0) {
        VFS_Seek(f, f->pos - 1);
        cpu->d[0] = (uint32_t)ch;
    } else {
        SetIoErr(ERROR_OBJECT_NOT_FOUND);
    }
}

static void dos_FRead(M68kCPUState *cpu)
{
    uint32_t fh = cpu->d[1], block = cpu->d[2];
    uint32_t blen = cpu->d[3], num = cpu->d[4];
    cpu->d[0] = 0;
    HandleEntry *ent = HandleTable_Get(fh);
    if (!ent || ent->type != HTYPE_FILE || !blen || !num) return;
    uint64_t total = (uint64_t)blen * num;
    if (block + total > GUEST_RAM_SIZE) { SetIoErr(ERROR_BAD_NUMBER); return; }
    int32_t n = VFS_Read(&ent->u.file.fh, g_ram + block, (uint32_t)total);
    if (n > 0) cpu->d[0] = (uint32_t)n / blen;
}

static void dos_FWrite(M68kCPUState *cpu)
{
    uint32_t fh = cpu->d[1], block = cpu->d[2];
    uint32_t blen = cpu->d[3], num = cpu->d[4];
    cpu->d[0] = 0;
    HandleEntry *ent = HandleTable_Get(fh);
    if (!ent || ent->type != HTYPE_FILE || !blen || !num) return;
    uint64_t total = (uint64_t)blen * num;
    if (block + total > GUEST_RAM_SIZE) { SetIoErr(ERROR_BAD_NUMBER); return; }
    int32_t n = VFS_Write(&ent->u.file.fh, g_ram + block, (uint32_t)total);
    if (n > 0) cpu->d[0] = (uint32_t)n / blen;
}

static void dos_FGets(M68kCPUState *cpu)
{
    uint32_t fh = cpu->d[1], buf = cpu->d[2], buflen = cpu->d[3];
    cpu->d[0] = 0;
    HandleEntry *ent = HandleTable_Get(fh);
    if (!ent || ent->type != HTYPE_FILE || !buflen ||
        buf + buflen > GUEST_RAM_SIZE) {
        SetIoErr(ERROR_OBJECT_NOT_FOUND);
        return;
    }
    uint32_t i = 0;
    while (i < buflen - 1) {
        uint8_t ch;
        if (VFS_Read(&ent->u.file.fh, &ch, 1) != 1) break;
        g_ram[buf + i++] = ch;
        if (ch == '\n') break;
    }
    if (i) {
        g_ram[buf + i] = 0;
        cpu->d[0] = buf;
    }
}

static void dos_SetVBuf(M68kCPUState *cpu)
{
    /* Unbuffered: acknowledge SetVBuf/SetMode so callers proceed. */
    cpu->d[0] = DOSTRUE;
}

static void dos_SetMode(M68kCPUState *cpu)
{
    cpu->d[0] = DOSTRUE;
}

/* =========================================================================
 * Function table — indices must match the ILLEGAL handler's DOS_* constants
 * ========================================================================= */

static void *dos_funcs[] = {
    dos_Output,        /* index 1  */
    dos_Write,         /* index 2  */
    dos_Open,          /* index 3  */
    dos_Close,         /* index 4  */
    dos_Read,          /* index 5  */
    dos_Exit,          /* index 6  */
    dos_IoErr,         /* index 7  */
    dos_Input,         /* index 8  */
    dos_VFPrintf,      /* index 9  */
    dos_FPuts,         /* index 10 */
    dos_PutStr,        /* index 11 */
    dos_VPrintf,       /* index 12 */
    dos_VPrintf,       /* index 13 — alias */
    dos_VFWritef,      /* index 14 */
    dos_ReadArgs,      /* index 15 */
    dos_GetArgStr,     /* index 16 */
    dos_IsInteractive, /* index 17 */
    dos_DeleteFile,    /* index 18 */
    dos_Rename,        /* index 19 */
    dos_SetProtection, /* index 20 */
    dos_GetVar,        /* index 21 */
    dos_SetVar,        /* index 22 */
    dos_Seek,          /* index 23 */
    dos_Lock,          /* index 24 */
    dos_Unlock,        /* index 25 */
    dos_Examine,       /* index 26 */
    dos_ExamineNext,   /* index 27 */
    dos_CreateDir,     /* index 28 */
    dos_DupLock,       /* index 29 */
    dos_Parent,        /* index 30 */
    dos_DateStamp,     /* index 31 */
    dos_Delay,         /* index 32 */
    dos_DateToStr,     /* index 33 */
    dos_ParsePattern,       /* index 34 */
    dos_MatchPattern,       /* index 35 */
    dos_ParsePatternNoCase, /* index 36 */
    dos_MatchPatternNoCase, /* index 37 */
    dos_LoadSeg,            /* index 38 */
    dos_UnLoadSeg,          /* index 39 */
    dos_CreateProc,         /* index 40 */
    dos_SystemTagList,      /* index 41 */
    dos_RunCommand,         /* index 42 */
    dos_SendPkt,            /* index 43 */
    dos_WaitPkt,            /* index 44 */
    dos_ReplyPkt,           /* index 45 */
    dos_AddPart,            /* index 46 */
    dos_CompareNames,       /* index 47 */
    dos_StrToDate,          /* index 48 */
    dos_CheckSignal,        /* index 49 */
    dos_WaitForChar,        /* index 50 */
    dos_NameFromLock,       /* index 51 */
    dos_LockRecord,         /* index 52 */
    dos_UnLockRecord,       /* index 53 */
    dos_GetConsoleTask,     /* index 54 */
    dos_SetConsoleTask,     /* index 55 */
    dos_CreateSegList,      /* index 56 */
    dos_CurrentDir,         /* index 57 */
    dos_SetProgramDir,      /* index 58 */
    dos_GetProgramDir,      /* index 59 */
    dos_SetIoErr,           /* index 60 */
    dos_Cli,                /* index 61 */
    dos_FindCliProc,        /* index 62 */
    dos_WriteChars,         /* index 63 */
    dos_FreeArgs,           /* index 64 */
    dos_Flush,              /* index 65 */
    dos_SelectInput,        /* index 66 */
    dos_SelectOutput,       /* index 67 */
    dos_Execute,            /* index 68 */
    dos_DeviceProc,         /* index 69 */
    dos_Fault,              /* index 70 */
    dos_FilePart,           /* index 71 */
    dos_PathPart,           /* index 72 */
    /* UAOS-248 — OctaMED gap-fill */
    dos_Info,               /* index 73 */
    dos_ExamineFH,          /* index 74 */
    dos_ParentOfFH,         /* index 75 */
    dos_DupLockFromFH,      /* index 76 */
    dos_OpenFromLock,       /* index 77 */
    dos_SameLock,           /* index 78 */
    dos_SetFileSize,        /* index 79 */
    dos_ExAll,              /* index 80 */
    dos_ExAllEnd,           /* index 81 */
    dos_IsFileSystem,       /* index 82 */
    dos_Inhibit,            /* index 83 */
    dos_SetComment,         /* index 84 */
    dos_AssignLock,         /* index 85 */
    dos_AssignLate,         /* index 86 */
    dos_AssignPath,         /* index 87 */
    dos_AssignAdd,          /* index 88 */
    dos_GetDeviceProc,      /* index 89 */
    dos_FreeDeviceProc,     /* index 90 */
    dos_LockDosList,        /* index 91 */
    dos_UnlockDosList,      /* index 92 */
    dos_AttemptLockDosList, /* index 93 */
    dos_NextDosEntry,       /* index 94 */
    dos_FGetC,              /* index 95 */
    dos_FPutC,              /* index 96 */
    dos_UnGetC,             /* index 97 */
    dos_FRead,              /* index 98 */
    dos_FWrite,             /* index 99 */
    dos_FGets,              /* index 100 */
    dos_SetVBuf,            /* index 101 */
    dos_SetMode,            /* index 102 */
    dos_StrToLong,          /* index 103 */
};

/* =========================================================================
 * Registration function
 * ========================================================================= */

void UAOS_DOS_Register(void)
{
    UAOS_ROM_Register("dos.library", 40, 0x000000D0,
                      (uint16_t)(sizeof(dos_funcs) / sizeof(dos_funcs[0])),
                      dos_funcs);
}

/* =========================================================================
 * Glue entry points called from uaos_m68k_glue.c for exec.library
 * AllocMem and FreeMem.  These bridge the two translation units.
 * ========================================================================= */

void dos_AllocMem_glue(uint32_t size, uint32_t reqs, uint32_t *out_addr)
{
    if (size == 0) { *out_addr = 0; return; }

    uint32_t addr = mc_alloc(size, reqs);
    if (!addr) SetIoErr(ERROR_NO_FREE_STORE);
    *out_addr = addr;
}

void dos_FreeMem_glue(uint32_t addr, uint32_t size)
{
    (void)size; /* our free-list tracks block sizes internally */
    mc_free(addr);
}

/* AllocAbs: carve exactly [addr, addr+size) out of the owning pool's free
 * list (UAOS-239).  The payload must begin at `addr`, so the heap header
 * lands at addr-HEAP_HDR — the free block containing the range must cover
 * [addr-HEAP_HDR, addr+size).  Any leading/trailing slop is re-linked as
 * free blocks.  Returns 0 on failure (range not wholly free, or the split
 * edges can't hold a free node).
 *
 * Not tracked by memcheck: the exact-address contract can't honour the
 * +8 guard-band layout.  A later FreeMem on an AllocAbs block still works —
 * the mc_find miss path falls back to the pool free. */
void dos_AllocAbs_glue(uint32_t addr, uint32_t size, uint32_t *out_addr)
{
    *out_addr = 0;
    if (!size || addr < HEAP_HDR || addr >= GUEST_RAM_SIZE) return;

    uint32_t list_slot = (addr >= HEAP_FAST_START) ? HEAP_LIST_SLOT_FAST
                                                   : HEAP_LIST_SLOT_CHIP;
    if (addr >= HEAP_FAST_START) heap_freelist_init_fast();
    else                         heap_freelist_init();

    uint32_t want_lo = addr - HEAP_HDR;                  /* header base   */
    uint32_t want_hi = addr + ((size + 3u) & ~3u);       /* exclusive end */

    uint32_t prev_slot = list_slot;   /* address of the link to current */
    uint32_t cur_bptr  = heap_head_read(list_slot);

    while (cur_bptr) {
        uint32_t cur      = cur_bptr << 2;
        uint32_t blk_size = guest_read_be32(cur + 0) & ~HEAP_MAGIC;
        uint32_t next_bptr= guest_read_be32(cur + 4);
        uint32_t blk_end  = cur + blk_size;

        if (want_lo >= cur && want_hi <= blk_end) {
            uint32_t lead = want_lo - cur;          /* leading free slop */
            uint32_t tail = blk_end - want_hi;      /* trailing free slop */

            /* A split edge must be zero or big enough for a free node. */
            if ((lead && lead < HEAP_HDR) || (tail && tail < HEAP_HDR))
                return;

            if (tail) {
                guest_write_be32(want_hi + 0, tail);
                guest_write_be32(want_hi + 4, next_bptr);
            }
            if (lead) {
                guest_write_be32(cur + 0, lead);
                guest_write_be32(cur + 4, tail ? (want_hi >> 2) : next_bptr);
                if (prev_slot == list_slot) heap_head_write(list_slot, cur_bptr);
                else                        guest_write_be32(prev_slot, cur_bptr);
            } else if (tail) {
                if (prev_slot == list_slot) heap_head_write(list_slot, want_hi >> 2);
                else                        guest_write_be32(prev_slot, want_hi >> 2);
            } else {
                /* exact fit — unlink the whole block */
                if (prev_slot == list_slot) heap_head_write(list_slot, next_bptr);
                else                        guest_write_be32(prev_slot, next_bptr);
            }

            guest_write_be32(want_lo + 0, (want_hi - want_lo) | HEAP_MAGIC);
            guest_write_be32(want_lo + 4, 0);
            *out_addr = addr;
            return;
        }

        prev_slot = cur + 4;
        cur_bptr  = next_bptr;
    }
}

/* AvailMem: sum free blocks in the pool(s) selected by 'attrs'.
 * MEMF_CHIP only → chip pool; MEMF_FAST only → fast pool; anything else
 * (plain PUBLIC / EXECUTABLE etc.) counts both pools. */
static void availmem_walk(uint32_t list_slot, uint32_t *total, uint32_t *largest)
{
    uint32_t bptr = heap_head_read(list_slot);
    int guard = 0;
    while (bptr && guard++ < 4096) {
        uint32_t cur = bptr << 2;
        uint32_t sz  = guest_read_be32(cur + 0) & ~HEAP_MAGIC;
        if (sz > HEAP_HDR) {
            uint32_t usable = sz - HEAP_HDR;
            *total += usable;
            if (usable > *largest) *largest = usable;
        }
        bptr = guest_read_be32(cur + 4);
    }
}

void dos_AvailMem_glue(uint32_t attrs, uint32_t *total, uint32_t *largest)
{
    *total = *largest = 0;
    if (!(attrs & MEMF_FAST)) { heap_freelist_init();      availmem_walk(HEAP_LIST_SLOT_CHIP, total, largest); }
    if (!(attrs & MEMF_CHIP)) { heap_freelist_init_fast(); availmem_walk(HEAP_LIST_SLOT_FAST, total, largest); }
}

/* =========================================================================
 * Host-side helpers for the asl.library file requester (UAOS-242)
 * ========================================================================= */

/* Lock() for a native C path — same ACTION_LOCATE_OBJECT plumbing as
 * dos_Lock but without an M68kCPUState.  Returns a FileLock BPTR or 0.
 * The 16-byte guest FileLock rides the never-freed bump heap like every
 * other lock; the caller frees the handler-side node via
 * dos_UnLockBPTR_glue. */
uint32_t dos_LockPath_glue(const char *path)
{
    char resolved[128];
    const char *p = path;
    if (VFS_ResolveAssignPath(path, resolved, sizeof(resolved)))
        p = resolved;
    char vol_name[16];
    extract_vol_name(p, vol_name, sizeof(vol_name));
    MsgPort *port = VFS_GetHandlerPort(vol_name);
    if (!port) return 0;
    int32_t handle = DoPkt(port, ACTION_LOCATE_OBJECT, (intptr_t)p,
                           SHARED_LOCK, 0, 0, 0);
    if (!handle) return 0;
    uint32_t bptr = guest_alloc_filelock((uint32_t)handle, SHARED_LOCK);
    if (!bptr) HandleTable_Free((uint32_t)handle);
    return bptr;
}

/* UnLock() for a FileLock BPTR — releases the handler-side lock node via
 * ACTION_FREE_LOCK.  The guest struct itself is bump-heap, never freed. */
void dos_UnLockBPTR_glue(uint32_t lock_bptr)
{
    uint32_t handle = 0;
    if (guest_read_filelock(lock_bptr, &handle, NULL) && handle)
        VFS_FreeLock(handle);
}

/* Host-callable wrapper around the ParsePattern wildcard matcher — used by
 * the asl file requester to filter its listview (UAOS-242). */
int dos_pattern_match_glue(const char *name, const char *pat)
{
    return pattern_match(name, pat);
}
