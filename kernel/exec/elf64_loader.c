/* elf64_loader.c — UAOS x86-64 ELF64 user-space loader
 *
 * Loads ET_EXEC / ET_DYN x86-64 ELF binaries into a static kernel arena,
 * applies relocations from .rela.dyn, and builds the initial user stack.
 */

#include "elf64_loader.h"
#include "boot/kprint.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * Static arena
 * ------------------------------------------------------------------------- */
#define X64_HEAP_SIZE   (4 * 1024 * 1024)
#define X64_STACK_SIZE  (64 * 1024)

static uint8_t  g_x64_heap[X64_HEAP_SIZE];
static uint32_t g_x64_heap_used = 0;

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
static void *x64_heap_alloc(uint64_t size, uint64_t align)
{
    uintptr_t base = (uintptr_t)g_x64_heap + g_x64_heap_used;
    uintptr_t aligned = align_up(base, align);
    uintptr_t end = aligned + size;
    if (end > (uintptr_t)g_x64_heap + X64_HEAP_SIZE) {
        kprint("[ELF64] heap exhausted: need ");
        kprintdec((uint32_t)(size >> 10));
        kprint("KB, free ");
        kprintdec((uint32_t)((X64_HEAP_SIZE - g_x64_heap_used) >> 10));
        kprint("KB\n");
        return NULL;
    }
    g_x64_heap_used = (uint32_t)(end - (uintptr_t)g_x64_heap);
    return (void *)aligned;
}

uint32_t ELF64_HeapUsed(void)
{
    return g_x64_heap_used;
}

void *ELF64_HeapAlloc(uint32_t size, uint32_t align)
{
    return x64_heap_alloc(size, align);
}

/* -------------------------------------------------------------------------
 * Segment loading
 * ------------------------------------------------------------------------- */
static int load_segments(const Elf64_Ehdr *eh, const uint8_t *data,
                         uint32_t size, uint64_t *image_base,
                         uint64_t *image_size)
{
    uint64_t base = 0;

    if (eh->e_type == ET_DYN) {
        /* For PIE binaries, choose a load base at the current bump. */
        base = (uint64_t)x64_heap_alloc(0, 4096);
        if (!base) return ELF64_ERR_NO_HEAP;
    }

    *image_base = base;

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
        /* Reserve the total image size from the arena. */
        if (!x64_heap_alloc(total, 1)) return ELF64_ERR_NO_HEAP;
        *image_size = total;
    } else {
        /* ET_EXEC: absolute vaddr; must fit inside the arena. */
        if (min_vaddr < (uint64_t)g_x64_heap ||
            max_end > (uint64_t)g_x64_heap + X64_HEAP_SIZE) {
            kprint("[ELF64] ET_EXEC segment range outside x64 heap\n");
            return ELF64_ERR_LOAD_OOB;
        }
        *image_size = max_end - (uint64_t)g_x64_heap;
        if (g_x64_heap_used < (*image_size))
            g_x64_heap_used = (uint32_t)*image_size;
    }

    /* Copy / zero each segment. */
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD) continue;
        if (ph[i].p_memsz == 0) continue;

        uint64_t dest = base + ph[i].p_vaddr;
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
                             uint64_t image_base)
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
            uint64_t addr = image_base + rela[r].r_offset;
            uint64_t *slot = (uint64_t *)addr;

            uint64_t value = 0;
            switch (type) {
            case R_X86_64_RELATIVE:
                value = image_base + (uint64_t)rela[r].r_addend;
                break;
            case R_X86_64_64:
            case R_X86_64_GLOB_DAT:
                if (symidx >= sym_sh->sh_size / sizeof(Elf64_Sym)) continue;
                value = image_base + sym[symidx].st_value +
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

    uint8_t *stack = x64_heap_alloc(X64_STACK_SIZE, 16);
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

    kprint("[ELF64] loading ");
    kprint((eh->e_type == ET_DYN) ? "PIE" : "ET_EXEC");
    kprint(" x86-64 binary, ");
    kprintdec((uint32_t)size);
    kprint(" bytes\n");

    uint64_t image_base = 0, image_size = 0;
    int rc = load_segments(eh, data, size, &image_base, &image_size);
    if (rc != ELF64_OK) {
        out->error = rc;
        return rc;
    }

    rc = apply_relocations(eh, data, image_base);
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
    out->entry_rip = (eh->e_type == ET_DYN) ? (image_base + eh->e_entry)
                                            : eh->e_entry;
    out->initial_rsp = rsp;
    out->error = ELF64_OK;

    kprint("[ELF64] entry=0x");
    kprinthex((uint32_t)(out->entry_rip >> 32));
    kprinthex((uint32_t)out->entry_rip);
    kprint(" rsp=0x");
    kprinthex((uint32_t)(out->initial_rsp >> 32));
    kprinthex((uint32_t)out->initial_rsp);
    kprint(" heap=");
    kprintdec(g_x64_heap_used / 1024);
    kprint("KB\n");

    kprint("[ELF64] bytes @ entry: ");
    const uint8_t *ep = (const uint8_t *)out->entry_rip;
    for (int i = 0; i < 16; i++) {
        static const char hx[] = "0123456789ABCDEF";
        char buf[3];
        buf[0] = hx[ep[i] >> 4];
        buf[1] = hx[ep[i] & 0xF];
        buf[2] = 0;
        kprint(buf);
        kprint(" ");
    }
    kprint("\n");

    return ELF64_OK;
}
