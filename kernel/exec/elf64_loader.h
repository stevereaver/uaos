/* elf64_loader.h — UAOS x86-64 ELF64 user-space loader
 *
 * Phase 3 ABI: loads ET_EXEC / ET_DYN x86-64 binaries into a static
 * in-kernel arena, applies .rela.dyn relocations, and builds the initial
 * user stack in the SysV AMD64 ABI layout.
 */

#ifndef UAOS_ELF64_LOADER_H
#define UAOS_ELF64_LOADER_H

#include <stdint.h>
#include <stddef.h>

/* -------------------------------------------------------------------------
 * ELF64 identification
 * ------------------------------------------------------------------------- */
#define ELFMAG0         0x7f
#define ELFMAG1         'E'
#define ELFMAG2         'L'
#define ELFMAG3         'F'
#define ELFCLASS64      2
#define ELFDATA2LSB     1
#define EV_CURRENT      1
#define ET_EXEC         2
#define ET_DYN          3
#define EM_X86_64       62

/* -------------------------------------------------------------------------
 * ELF64 program header types
 * ------------------------------------------------------------------------- */
#define PT_LOAD         1
#define PT_DYNAMIC      2

/* -------------------------------------------------------------------------
 * ELF64 section header types
 * ------------------------------------------------------------------------- */
#define SHT_NULL        0
#define SHT_PROGBITS    1
#define SHT_SYMTAB      2
#define SHT_STRTAB      3
#define SHT_RELA        4
#define SHT_DYNSYM      11

/* -------------------------------------------------------------------------
 * Dynamic entry tags
 * ------------------------------------------------------------------------- */
#define DT_NULL         0
#define DT_RELA         7
#define DT_RELASZ       8
#define DT_RELAENT      9
#define DT_SYMTAB       6
#define DT_STRTAB       5
#define DT_STRSZ        10

/* -------------------------------------------------------------------------
 * x86-64 relocation types
 * ------------------------------------------------------------------------- */
#define R_X86_64_NONE     0
#define R_X86_64_64       1
#define R_X86_64_GLOB_DAT 6
#define R_X86_64_RELATIVE 8

/* -------------------------------------------------------------------------
 * ELF64 data structures
 * ------------------------------------------------------------------------- */
typedef struct {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} __attribute__((packed)) Elf64_Ehdr;

typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} __attribute__((packed)) Elf64_Phdr;

typedef struct {
    uint32_t sh_name;
    uint32_t sh_type;
    uint64_t sh_flags;
    uint64_t sh_addr;
    uint64_t sh_offset;
    uint64_t sh_size;
    uint32_t sh_link;
    uint32_t sh_info;
    uint64_t sh_addralign;
    uint64_t sh_entsize;
} __attribute__((packed)) Elf64_Shdr;

typedef struct {
    uint32_t st_name;
    uint8_t  st_info;
    uint8_t  st_other;
    uint16_t st_shndx;
    uint64_t st_value;
    uint64_t st_size;
} __attribute__((packed)) Elf64_Sym;

typedef struct {
    uint64_t r_offset;
    uint64_t r_info;
    int64_t  r_addend;
} __attribute__((packed)) Elf64_Rela;

typedef struct {
    int64_t  d_tag;
    uint64_t d_un;
} __attribute__((packed)) Elf64_Dyn;

/* -------------------------------------------------------------------------
 * Loader result
 * ------------------------------------------------------------------------- */
typedef struct {
    uint64_t entry_rip;      /* Initial instruction pointer */
    uint64_t initial_rsp;    /* User stack pointer (argc at [rsp]) */
    uint64_t image_base;     /* Load base used for relocations */
    uint64_t image_size;     /* Bytes consumed from the x64 heap */
    int      error;          /* 0 = success, negative = error code */
} ELF64Result;

/* -------------------------------------------------------------------------
 * Loader API
 * ------------------------------------------------------------------------- */

/* Load an ELF64 binary from memory into the x64 heap, apply relocations,
 * build the initial stack from argv, and return entry / RSP.
 * argv[0] is the program name; argv must be NULL-terminated.
 * Returns 0 on success, negative error code on failure. */
int ELF64_Load(const uint8_t *data, uint32_t size,
               const char **argv, ELF64Result *out);

/* Return the amount of heap currently in use (bytes). */
uint32_t ELF64_HeapUsed(void);

/* Return the total size of the x64 heap arena (bytes). */
uint32_t ELF64_HeapSize(void);

/* Allocate size bytes from the x64 heap with the given alignment.
 * Returns a valid kernel pointer or NULL on exhaustion.
 * The heap is a single bump arena; allocations are never freed. */
void *ELF64_HeapAlloc(uint32_t size, uint32_t align);

/* Reclaim the x64 heap when no X64 tasks are alive.
 * Called from Task_Exit() after marking the current task REMOVED. */
void ELF64_ReclaimHeap(void);

#endif /* UAOS_ELF64_LOADER_H */
