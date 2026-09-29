/* elf64_loader.c — UAOS x86-64 ELF64 user-space loader
 *
 * Loads ET_EXEC / ET_DYN x86-64 ELF binaries into a static kernel arena,
 * applies relocations from .rela.dyn, and builds the initial user stack.
 */

#include "elf64_loader.h"
#include "boot/kprint.h"
#include "exec/task.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * Static arena — boundary-tag free-list allocator
 *
 * The arena is managed as a doubly-linked list of blocks ordered by
 * address.  Each block header sits immediately before its payload:
 *
 *   [X64Blk header (48B)][payload ...]
 *
 * Because prev/next describe *physical* adjacency, free() can coalesce
 * with neighbours in O(1) and reject wild/double frees by checking the
 * magic word, the used flag, and link consistency.
 *
 * Every block records an owner: the task that will release it.  Loader
 * allocations (image, stack) start ownerless and are handed to the new
 * task via ELF64_HeapOwn() once Task_CreateX64() succeeds; sys_alloc
 * blocks are owned by the calling task.  Task_Exit() frees all blocks a
 * dead task still owns; ELF64_ReclaimHeap() resets the whole arena once
 * no X64 task remains.
 * ------------------------------------------------------------------------- */
#define X64_HEAP_SIZE   (4 * 1024 * 1024)
#define X64_STACK_SIZE  (256 * 1024)

#define X64_BLK_MAGIC   0x583634424C4B4844ULL   /* "X64BLKHD" */
#define X64_BLK_F_USED  0x1ULL
#define X64_MIN_SPLIT   (X64_BLK_HDR + 16)      /* smallest useful tail  */

typedef struct X64Blk {
    uint64_t       magic;   /* X64_BLK_MAGIC while the chain is sane   */
    uint64_t       size;    /* total block bytes, header included      */
    struct X64Blk *prev;    /* previous physical block (lower addr)    */
    struct X64Blk *next;    /* next physical block                     */
    void          *owner;   /* owning UaosTask*, NULL = kernel/loader  */
    uint64_t       flags;   /* X64_BLK_F_*                             */
} X64Blk;

#define X64_BLK_HDR     ((sizeof(X64Blk) + 15) & ~15UL)   /* 48 */

static uint8_t  g_x64_heap[X64_HEAP_SIZE] __attribute__((aligned(16)));
static X64Blk  *g_x64_first = NULL;
static uint64_t g_x64_live  = 0;      /* bytes held in used blocks */

/* -------------------------------------------------------------------------
 * Error codes
 * ------------------------------------------------------------------------- */
enum {
    ELF64_OK = 0,
    ELF64_ERR_BAD_MAGIC = -1,
    ELF64_ERR_NOT_X86_64 = -2,
    ELF64_ERR_BAD_TYPE = -3,
    ELF64_ERR_SHORT_HEADER = -4,
    ELF64_ERR_NO_HEAP = -5,
    ELF64_ERR_BAD_PHDR = -6,
    ELF64_ERR_LOAD_OOB = -7,
    ELF64_ERR_NO_STACK = -8,
    ELF64_ERR_BAD_SHDR = -9,
    ELF64_ERR_NO_RELA = -10,
};

/* -------------------------------------------------------------------------
 * Tiny freestanding helpers
 * ------------------------------------------------------------------------- */
static inline uint64_t u64_min(uint64_t a, uint64_t b)
{
    return a < b ? a : b;
}

static inline uint64_t align_up(uint64_t v, uint64_t a)
{
    if (a == 0) return v;
    return (v + a - 1) & ~(a - 1);
}

static inline uint64_t align_down(uint64_t v, uint64_t a)
{
    return v & ~(a - 1);
}

static inline int valid_ehdr(const Elf64_Ehdr *eh)
{
    return eh->e_ident[0] == ELFMAG0 &&
           eh->e_ident[1] == ELFMAG1 &&
           eh->e_ident[2] == ELFMAG2 &&
           eh->e_ident[3] == ELFMAG3 &&
           eh->e_ident[4] == ELFCLASS64 &&
           eh->e_ident[5] == ELFDATA2LSB &&
           eh->e_ident[6] == EV_CURRENT;
}

static inline uint64_t phdr_end(const Elf64_Phdr *ph)
{
    return ph->p_vaddr + ph->p_memsz;
}

static inline uint64_t rela_type(uint64_t info)
{
    return (uint32_t)info;
}

static inline uint64_t rela_sym(uint64_t info)
{
    return info >> 32;
}

/* -------------------------------------------------------------------------
 * Arena allocator
 * ------------------------------------------------------------------------- */

/* Critical-section guard for heap list mutation — the pushfq/cli +
 * conditional sti idiom used by Memcheck_Scan(), so nested entry
 * (HeapFreeRange → x64_heap_free) and IRQ-off callers never re-enable
 * interrupts early.  Prevents a PIT preemption from observing a
 * transiently half-split block chain. */
static inline uint64_t x64_irq_save(void)
{
    uint64_t flags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
    return flags;
}

static inline void x64_irq_restore(uint64_t flags)
{
    if (flags & 0x200)
        __asm__ volatile("sti" ::: "memory");
}

/* Reset the arena to a single free block spanning the whole heap. */
static void x64_heap_reset(void)
{
    X64Blk *b = (X64Blk *)g_x64_heap;
    b->magic = X64_BLK_MAGIC;
    b->size  = X64_HEAP_SIZE;
    b->prev  = NULL;
    b->next  = NULL;
    b->owner = NULL;
    b->flags = 0;
    g_x64_first = b;
    g_x64_live  = 0;
}

static inline uintptr_t x64_blk_end(const X64Blk *b)
{
    return (uintptr_t)b + b->size;
}

/* Split a free block at nbs: [blk_start, nbs) stays free, a new free
 * block covers [nbs, blk_end).  Caller must ensure nbs - blk_start >=
 * X64_BLK_HDR.  Returns the new (still free) block at nbs. */
static X64Blk *x64_split_lead(X64Blk *b, uintptr_t nbs)
{
    uintptr_t bs = (uintptr_t)b;
    uintptr_t end = x64_blk_end(b);

    X64Blk *nb = (X64Blk *)nbs;
    nb->magic = X64_BLK_MAGIC;
    nb->size  = end - nbs;
    nb->prev  = b;
    nb->next  = b->next;
    nb->owner = NULL;
    nb->flags = 0;
    if (nb->next)
        nb->next->prev = nb;
    b->next = nb;
    b->size = nbs - bs;
    return nb;
}

/* The sliver [bs, nbs) is too small to host a block header, so it
 * cannot be split off as a free fragment.  Absorb it instead: grow the
 * preceding (necessarily used — adjacent free blocks never exist) block
 * over the sliver, or orphan it when `b` is the first block.  Returns
 * the new free block at nbs. */
static X64Blk *x64_absorb_sliver(X64Blk *b, uintptr_t nbs)
{
    uintptr_t bs = (uintptr_t)b;
    X64Blk *nb = (X64Blk *)nbs;

    nb->magic = X64_BLK_MAGIC;
    nb->size  = x64_blk_end(b) - nbs;
    nb->next  = b->next;
    nb->owner = NULL;
    nb->flags = 0;
    if (nb->next)
        nb->next->prev = nb;

    if (b->prev) {
        b->prev->size += nbs - bs;
        b->prev->next  = nb;
        nb->prev       = b->prev;
        if (b->prev->flags & X64_BLK_F_USED)
            g_x64_live += nbs - bs;         /* keep live = Σ used sizes */
    } else {
        nb->prev     = NULL;
        g_x64_first  = nb;                  /* sliver orphaned at base */
    }
    b->magic = 0;
    return nb;
}

/* Split a free tail off a block about to be marked used: [bs+need, end)
 * becomes a free block when it can still hold a useful payload. */
static void x64_split_tail(X64Blk *b, uint64_t need)
{
    uintptr_t bs  = (uintptr_t)b;
    uintptr_t end = bs + b->size;

    if (end - (bs + need) < X64_MIN_SPLIT)
        return;                     /* tail too small — fold into block */

    X64Blk *tail = (X64Blk *)(bs + need);
    tail->magic = X64_BLK_MAGIC;
    tail->size  = end - (bs + need);
    tail->prev  = b;
    tail->next  = b->next;
    tail->owner = NULL;
    tail->flags = 0;
    if (tail->next)
        tail->next->prev = tail;
    b->next = tail;
    b->size = need;
}

/* First-fit allocation.  `align` applies to the returned payload pointer
 * (16 minimum; 4096 is used for PIE image bases).  `owner` is stamped
 * into the block for per-task reclamation. */
static void *x64_heap_alloc(uint64_t size, uint64_t align, void *owner)
{
    uint64_t irq = x64_irq_save();

    if (!g_x64_first)
        x64_heap_reset();
    if (align < 16)
        align = 16;
    size = align_up(size ? size : 16, 16);
    const uint64_t need = size + X64_BLK_HDR;

    for (X64Blk *b = g_x64_first; b; b = b->next) {
        if (b->magic != X64_BLK_MAGIC)
            break;                          /* corrupt chain — stop */
        if (b->flags & X64_BLK_F_USED)
            continue;

        uintptr_t bs  = (uintptr_t)b;
        uintptr_t end = x64_blk_end(b);
        /* The header must sit immediately before the aligned payload. */
        uintptr_t nbs = align_up(bs + X64_BLK_HDR, align) - X64_BLK_HDR;
        if (nbs < bs || nbs + need > end)
            continue;

        if (nbs > bs) {
            if (nbs - bs >= X64_BLK_HDR)
                b = x64_split_lead(b, nbs);
            else
                b = x64_absorb_sliver(b, nbs);
        }
        bs = (uintptr_t)b;
        x64_split_tail(b, need);

        b->flags |= X64_BLK_F_USED;
        b->owner  = owner;
        g_x64_live += b->size;
        x64_irq_restore(irq);
        return (void *)(bs + X64_BLK_HDR);
    }

    kprint("[ELF64] heap exhausted: need ");
    kprintdec((uint32_t)(need >> 10));
    kprint("KB\n");
    x64_irq_restore(irq);
    return NULL;
}

/* Reserve the exact range [addr, addr+size) as an allocated block —
 * used by ET_EXEC images whose absolute vaddrs must land in the arena.
 * The block's payload covers the range (it may start slightly below
 * `addr` when the header does not fit).  Fails if any part of the range
 * is already allocated or spans multiple blocks. */
static void *x64_heap_reserve(uintptr_t addr, uint64_t size, void *owner)
{
    uint64_t irq = x64_irq_save();

    if (!g_x64_first)
        x64_heap_reset();

    uintptr_t end_req = addr + size;
    for (X64Blk *b = g_x64_first; b; b = b->next) {
        if (b->magic != X64_BLK_MAGIC)
            break;
        uintptr_t bs  = (uintptr_t)b;
        uintptr_t end = x64_blk_end(b);
        if (addr < bs || addr >= end)
            continue;                       /* range starts elsewhere */
        if ((b->flags & X64_BLK_F_USED) || end_req > end) {
            x64_irq_restore(irq);
            return NULL;                    /* overlaps live memory */
        }

        /* The header must sit entirely below addr, or the image copy
         * would clobber it.  When the free block starts too close to the
         * image there is no room — fail the reservation. */
        if (addr - bs < X64_BLK_HDR) {
            x64_irq_restore(irq);
            return NULL;
        }
        uintptr_t nbs = align_down(addr - X64_BLK_HDR, 16);  /* >= bs */
        if (nbs > bs && nbs - bs < X64_BLK_HDR)
            nbs = bs;                       /* absorb unsplittable sliver */
        if (nbs > bs)
            b = x64_split_lead(b, nbs);
        bs = (uintptr_t)b;

        x64_split_tail(b, end_req - bs);

        b->flags |= X64_BLK_F_USED;
        b->owner  = owner;
        g_x64_live += b->size;
        x64_irq_restore(irq);
        return (void *)(bs + X64_BLK_HDR);
    }
    x64_irq_restore(irq);
    return NULL;
}

/* Validate the block header at ptr - X64_BLK_HDR and return it, or NULL
 * after logging why the free was rejected. */
static X64Blk *x64_free_target(void *ptr)
{
    uintptr_t p = (uintptr_t)ptr;
    uintptr_t heap0 = (uintptr_t)g_x64_heap;
    uintptr_t heap1 = heap0 + X64_HEAP_SIZE;

    if (p < heap0 + X64_BLK_HDR || p >= heap1) {
        kprint("[ELF64] free: pointer outside x64 heap\n");
        return NULL;
    }

    X64Blk *b = (X64Blk *)(p - X64_BLK_HDR);
    if (b->magic != X64_BLK_MAGIC) {
        kprint("[ELF64] free: not a heap block\n");
        return NULL;
    }
    if (!(b->flags & X64_BLK_F_USED)) {
        kprint("[ELF64] free: double free\n");
        return NULL;
    }
    if (b->size < X64_BLK_HDR || (b->size & 15) ||
        (uintptr_t)b + b->size > heap1) {
        kprint("[ELF64] free: corrupt block size\n");
        return NULL;
    }
    /* Link consistency — verify neighbours agree this block exists. */
    if (b->prev) {
        if ((uintptr_t)b->prev < heap0 || (uintptr_t)b->prev >= heap1 ||
            b->prev->magic != X64_BLK_MAGIC || b->prev->next != b) {
            kprint("[ELF64] free: corrupt prev link\n");
            return NULL;
        }
    } else if (b != g_x64_first) {
        kprint("[ELF64] free: orphan block\n");
        return NULL;
    }
    if (b->next) {
        if ((uintptr_t)b->next < heap0 || (uintptr_t)b->next >= heap1 ||
            b->next->magic != X64_BLK_MAGIC || b->next->prev != b) {
            kprint("[ELF64] free: corrupt next link\n");
            return NULL;
        }
    }
    return b;
}

/* Free the block whose payload starts at `ptr` and coalesce with free
 * neighbours.  Only header fields are written — payload bytes are left
 * alone (Task_Exit frees the stack the exit path itself runs on). */
static void x64_heap_free(void *ptr)
{
    uint64_t irq = x64_irq_save();

    X64Blk *b = x64_free_target(ptr);
    if (!b) {
        x64_irq_restore(irq);
        return;
    }

    b->flags &= ~X64_BLK_F_USED;
    b->owner  = NULL;
    g_x64_live -= b->size;

    /* Coalesce forward. */
    if (b->next && !(b->next->flags & X64_BLK_F_USED)) {
        X64Blk *n = b->next;
        b->size += n->size;
        b->next  = n->next;
        if (n->next)
            n->next->prev = b;
        n->magic = 0;
    }
    /* Coalesce backward. */
    if (b->prev && !(b->prev->flags & X64_BLK_F_USED)) {
        X64Blk *pv = b->prev;
        pv->size += b->size;
        pv->next  = b->next;
        if (b->next)
            b->next->prev = pv;
        b->magic = 0;
    }
    x64_irq_restore(irq);
}

/* Locate the used block whose span contains `ptr`. */
static X64Blk *x64_blk_containing(void *ptr)
{
    uintptr_t p = (uintptr_t)ptr;
    for (X64Blk *b = g_x64_first; b; b = b->next) {
        if (b->magic != X64_BLK_MAGIC)
            break;
        if (p >= (uintptr_t)b && p < x64_blk_end(b))
            return b;
    }
    return NULL;
}

uint32_t ELF64_HeapUsed(void)
{
    return (uint32_t)g_x64_live;
}

uint32_t ELF64_HeapSize(void)
{
    return X64_HEAP_SIZE;
}

void *ELF64_HeapAlloc(uint32_t size, uint32_t align)
{
    return x64_heap_alloc(size, align, Task_Current());
}

void ELF64_HeapFree(void *ptr)
{
    x64_heap_free(ptr);
}

void ELF64_HeapOwn(void *owner, void *ptr)
{
    uint64_t irq = x64_irq_save();
    X64Blk *b = x64_blk_containing(ptr);
    if (b && (b->flags & X64_BLK_F_USED))
        b->owner = owner;
    x64_irq_restore(irq);
}

void ELF64_HeapFreeRange(uint64_t addr, uint64_t size)
{
    uintptr_t a0 = (uintptr_t)addr;
    uintptr_t a1 = (uintptr_t)(addr + (size ? size : 1));
    for (;;) {
        X64Blk *b = g_x64_first;
        for (; b; b = b->next) {
            if (b->magic != X64_BLK_MAGIC)
                return;
            if ((b->flags & X64_BLK_F_USED) &&
                (uintptr_t)b < a1 && x64_blk_end(b) > a0)
                break;
        }
        if (!b)
            return;
        x64_heap_free((void *)((uintptr_t)b + X64_BLK_HDR));
    }
}

void ELF64_FreeTaskBlocks(void *task)
{
    for (;;) {
        X64Blk *b = g_x64_first;
        for (; b; b = b->next) {
            if (b->magic != X64_BLK_MAGIC)
                return;
            if ((b->flags & X64_BLK_F_USED) && b->owner == task)
                break;
        }
        if (!b)
            return;
        x64_heap_free((void *)((uintptr_t)b + X64_BLK_HDR));
    }
}

/* -------------------------------------------------------------------------
 * Heap reclamation
 *
 * Per-block frees handle the common case; this is the final sweep for
 * loader-owned (NULL owner) strays left by failed spawns.  When no X64
 * userspace tasks are alive the entire arena is reset to one free block.
 *
 * Called from Task_Exit() after the current task is marked REMOVED.
 * ------------------------------------------------------------------------- */
void ELF64_ReclaimHeap(void)
{
    /* Scan the global task table for any live X64 task. */
    extern UaosTask g_tasks[];
    extern int      g_task_count;

    for (int i = 0; i < g_task_count; i++) {
        if (g_tasks[i].type == TASK_TYPE_X64 &&
            g_tasks[i].tc_State != TASK_REMOVED) {
            return;  /* at least one X64 task is still alive */
        }
    }

    /* No live X64 tasks — safe to reset the heap. */
    if (g_x64_first && g_x64_live > 0) {
        kprint("[ELF64] reclaiming x64 heap (");
        kprintdec((uint32_t)(g_x64_live / 1024));
        kprint("KB freed)\n");
    }
    if (g_x64_first)
        x64_heap_reset();
}

/* -------------------------------------------------------------------------
 * Segment loading
 * ------------------------------------------------------------------------- */
static int load_segments(const Elf64_Ehdr *eh, const uint8_t *data,
                         uint32_t size, uint64_t *image_base,
                         uint64_t *image_size, uint64_t *min_vaddr_out)
{
    uint64_t base = 0;

    /* Walk PT_LOAD once to compute the total image footprint. */
    uint64_t min_vaddr = UINT64_MAX;
    uint64_t max_end = 0;
    const Elf64_Phdr *ph = (const Elf64_Phdr *)(data + eh->e_phoff);
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD) continue;
        if (ph[i].p_memsz == 0) continue;
        if (ph[i].p_vaddr < min_vaddr) min_vaddr = ph[i].p_vaddr;
        uint64_t end = phdr_end(&ph[i]);
        if (end > max_end) max_end = end;
    }

    if (max_end == 0) {
        *image_size = 0;
        return ELF64_OK;
    }

    if (eh->e_type == ET_DYN) {
        uint64_t total = max_end - min_vaddr;
        /* Reserve the total image footprint as one page-aligned block;
         * the returned payload is where the image begins. */
        base = (uint64_t)(uintptr_t)x64_heap_alloc(total, 4096, NULL);
        if (!base) return ELF64_ERR_NO_HEAP;
        *image_size = total;
    } else {
        /* ET_EXEC: absolute vaddr; must fit inside the arena. */
        if (min_vaddr < (uint64_t)g_x64_heap ||
            max_end > (uint64_t)g_x64_heap + X64_HEAP_SIZE) {
            kprint("[ELF64] ET_EXEC segment range outside x64 heap\n");
            return ELF64_ERR_LOAD_OOB;
        }
        base = min_vaddr;
        /* Reserve the image range so later allocations cannot overlap it. */
        if (!x64_heap_reserve((uintptr_t)min_vaddr, max_end - min_vaddr,
                              NULL))
            return ELF64_ERR_NO_HEAP;
        *image_size = max_end - min_vaddr;
    }

    *image_base    = base;
    *min_vaddr_out = min_vaddr;

    /* Copy / zero each segment. */
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD) continue;
        if (ph[i].p_memsz == 0) continue;

        uint64_t dest = base + (ph[i].p_vaddr - min_vaddr);
        if (eh->e_type == ET_EXEC) {
            /* dest already absolute; verify it again. */
            if (dest < (uint64_t)g_x64_heap ||
                dest + ph[i].p_memsz > (uint64_t)g_x64_heap + X64_HEAP_SIZE) {
                return ELF64_ERR_LOAD_OOB;
            }
        }

        uint64_t file_off = ph[i].p_offset;
        uint64_t file_end = u64_min(file_off + ph[i].p_filesz, size);
        uint64_t to_copy = (file_end > file_off) ? (file_end - file_off) : 0;
        if (to_copy > ph[i].p_memsz) to_copy = ph[i].p_memsz;

        if (to_copy > 0)
            memcpy((void *)dest, data + file_off, (size_t)to_copy);
        if (ph[i].p_memsz > to_copy)
            memset((void *)(dest + to_copy), 0,
                   (size_t)(ph[i].p_memsz - to_copy));
    }

    return ELF64_OK;
}

/* -------------------------------------------------------------------------
 * Relocations
 * ------------------------------------------------------------------------- */
static int apply_relocations(const Elf64_Ehdr *eh, const uint8_t *data,
                             uint64_t bias)
{
    /* Only ET_DYN normally needs relocations; ET_EXEC may already be fixed. */
    if (eh->e_type != ET_DYN) return ELF64_OK;

    if (eh->e_shentsize < sizeof(Elf64_Shdr) || eh->e_shnum == 0)
        return ELF64_OK; /* no section table, nothing to apply */

    const Elf64_Shdr *sh = (const Elf64_Shdr *)(data + eh->e_shoff);

    for (int i = 0; i < eh->e_shnum; i++) {
        if (sh[i].sh_type != SHT_RELA) continue;
        if (sh[i].sh_entsize < sizeof(Elf64_Rela)) continue;
        if (sh[i].sh_link >= eh->e_shnum) continue;

        const Elf64_Shdr *sym_sh = &sh[sh[i].sh_link];
        if (sym_sh->sh_type != SHT_SYMTAB && sym_sh->sh_type != SHT_DYNSYM)
            continue;

        const Elf64_Rela *rela = (const Elf64_Rela *)(data + sh[i].sh_offset);
        const Elf64_Sym  *sym  = (const Elf64_Sym  *)(data + sym_sh->sh_offset);
        uint64_t count = sh[i].sh_size / sizeof(Elf64_Rela);

        for (uint64_t r = 0; r < count; r++) {
            uint64_t type = rela_type(rela[r].r_info);
            uint64_t symidx = rela_sym(rela[r].r_info);
            uint64_t addr = bias + rela[r].r_offset;
            uint64_t *slot = (uint64_t *)addr;

            uint64_t value = 0;
            switch (type) {
            case R_X86_64_RELATIVE:
                value = bias + (uint64_t)rela[r].r_addend;
                break;
            case R_X86_64_64:
            case R_X86_64_GLOB_DAT:
                if (symidx >= sym_sh->sh_size / sizeof(Elf64_Sym)) continue;
                value = bias + sym[symidx].st_value +
                        (uint64_t)rela[r].r_addend;
                break;
            case R_X86_64_NONE:
            default:
                continue;
            }
            *slot = value;
        }
    }

    return ELF64_OK;
}

/* -------------------------------------------------------------------------
 * Initial stack
 * ------------------------------------------------------------------------- */
static uint64_t build_initial_stack(const char **argv)
{
    int argc = 0;
    if (argv) {
        while (argv[argc]) argc++;
    }

    size_t str_size = 0;
    for (int i = 0; i < argc; i++)
        str_size += strlen(argv[i]) + 1;

    /* argc + argv[0..argc] + envp[0]=NULL */
    size_t ptr_words = (size_t)argc + 3;
    size_t ptr_size = ptr_words * sizeof(uint64_t);
    size_t total = str_size + ptr_size + 16; /* slack for alignment */

    if (total > X64_STACK_SIZE) {
        kprint("[ELF64] argv too large for initial stack\n");
        return 0;
    }

    uint8_t *stack = x64_heap_alloc(X64_STACK_SIZE, 16, NULL);
    if (!stack) return 0;

    uint8_t *top = stack + X64_STACK_SIZE;
    uint8_t *str_base = top - str_size;

    /* Copy argument strings to the top of the stack. */
    size_t off = 0;
    const char *str_addrs[64];
    for (int i = 0; i < argc; i++) {
        size_t len = strlen(argv[i]) + 1;
        if (off + len > str_size) break;
        memcpy(str_base + off, argv[i], len);
        str_addrs[i] = (const char *)(str_base + off);
        off += len;
    }

    /* Headers sit just below the strings, aligned to 16 bytes. */
    uint8_t *hdr = (uint8_t *)(((uintptr_t)(str_base - ptr_size)) & ~15UL);
    uint64_t *words = (uint64_t *)hdr;

    words[0] = (uint64_t)argc;
    for (int i = 0; i < argc; i++)
        words[i + 1] = (uint64_t)str_addrs[i];
    words[argc + 1] = 0;          /* argv[argc] = NULL sentinel */
    words[argc + 2] = 0;          /* envp[0] = NULL */

    return (uint64_t)hdr;
}

/* -------------------------------------------------------------------------
 * Public loader
 * ------------------------------------------------------------------------- */
int ELF64_Load(const uint8_t *data, uint32_t size,
               const char **argv, ELF64Result *out)
{
    if (!out) return ELF64_ERR_SHORT_HEADER;
    memset(out, 0, sizeof(*out));

    if (size < sizeof(Elf64_Ehdr)) {
        out->error = ELF64_ERR_SHORT_HEADER;
        return out->error;
    }

    const Elf64_Ehdr *eh = (const Elf64_Ehdr *)data;
    if (!valid_ehdr(eh)) {
        out->error = ELF64_ERR_BAD_MAGIC;
        return out->error;
    }

    if (eh->e_machine != EM_X86_64) {
        out->error = ELF64_ERR_NOT_X86_64;
        return out->error;
    }

    if (eh->e_type != ET_EXEC && eh->e_type != ET_DYN) {
        out->error = ELF64_ERR_BAD_TYPE;
        return out->error;
    }

    if (eh->e_phnum == 0 || eh->e_phentsize < sizeof(Elf64_Phdr) ||
        eh->e_phoff + (uint64_t)eh->e_phnum * eh->e_phentsize > size) {
        out->error = ELF64_ERR_BAD_PHDR;
        return out->error;
    }

    uint64_t image_base = 0, image_size = 0, min_vaddr = 0;
    int rc = load_segments(eh, data, size, &image_base, &image_size,
                           &min_vaddr);
    if (rc != ELF64_OK) {
        out->error = rc;
        return rc;
    }

    /* Load bias: vaddr + bias = runtime address.  For ET_DYN the image
     * memory starts at image_base, so bias = image_base - min_vaddr
     * (min_vaddr is 0 for typical -pie output, making bias == image_base). */
    uint64_t bias = image_base - min_vaddr;

    rc = apply_relocations(eh, data, bias);
    if (rc != ELF64_OK) {
        out->error = rc;
        return rc;
    }

    uint64_t rsp = build_initial_stack(argv);
    if (rsp == 0) {
        out->error = ELF64_ERR_NO_STACK;
        return out->error;
    }

    out->image_base = image_base;
    out->image_size = image_size;
    out->entry_rip = (eh->e_type == ET_DYN) ? (bias + eh->e_entry)
                                            : eh->e_entry;
    out->initial_rsp = rsp;
    out->error = ELF64_OK;

    return ELF64_OK;
}
