/*
 * mtrr.c — MTRR helpers (see mtrr.h)
 *
 * Bare-metal single-CPU context: the cache-disable sequence runs early
 * in boot before the scheduler starts, so no cross-CPU synchronisation
 * is needed.  Interrupts are still masked for the CD window: an ISR
 * running with caches off is technically survivable but brutally slow,
 * and any fault here is a triple fault (IDT is not installed yet).
 */

#include "mtrr.h"
#include "../boot/kprint.h"
#include "../irq/irq.h"

#define MSR_MTRRCAP       0xFE
#define MSR_MTRR_DEFTYPE  0x2FF
#define MSR_MTRR_PHYSBASE 0x200    /* +2n: base, +2n+1: mask */

#define MTRRCAP_WC        (1u << 10)
#define MTRRDEF_E         (1u << 11)
#define MTRRMASK_VALID    (1u << 11)
#define MTRR_TYPE_WC      0x01     /* UC=0 WC=1 WT=4 WP=5 WB=6 */

#define CR0_CD            (1u << 30)
#define CR0_NW            (1u << 29)
#define CR4_PGE           (1u << 7)

static inline uint64_t rdmsr64(uint32_t msr)
{
    uint32_t lo, hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline void wrmsr64(uint32_t msr, uint64_t val)
{
    __asm__ volatile ("wrmsr" :: "a"((uint32_t)val),
                                "d"((uint32_t)(val >> 32)), "c"(msr));
}

static inline uint64_t read_cr0(void)
{
    uint64_t v;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(v));
    return v;
}

static inline void write_cr0(uint64_t v)
{
    __asm__ volatile ("mov %0, %%cr0" :: "r"(v) : "memory");
}

static inline uint64_t read_cr3(void)
{
    uint64_t v;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(v));
    return v;
}

static inline void write_cr3(uint64_t v)
{
    __asm__ volatile ("mov %0, %%cr3" :: "r"(v) : "memory");
}

static inline uint64_t read_cr4(void)
{
    uint64_t v;
    __asm__ volatile ("mov %%cr4, %0" : "=r"(v));
    return v;
}

static inline void write_cr4(uint64_t v)
{
    __asm__ volatile ("mov %0, %%cr4" :: "r"(v) : "memory");
}

static uint32_t phys_addr_bits(void)
{
    /* CPUID 0x80000008 EAX[7:0] = MAXPHYADDR */
    uint32_t eax;
    __asm__ volatile ("cpuid" : "=a"(eax)
                     : "a"(0x80000008u) : "ebx", "ecx", "edx");
    /* Only addresses <4 GB are ever marked — clamp to 36 bits so a bogus
     * or unexpected report can't set reserved mask bits and #GP on wrmsr. */
    if (eax < 32 || eax > 36) eax = 36;
    return eax;
}

int MTRR_MarkWC(uint64_t base, uint64_t size)
{
    if (!base || !size) return -1;

    uint64_t cap = rdmsr64(MSR_MTRRCAP);
    if (!(cap & MTRRCAP_WC)) return -1;
    uint32_t vcnt = (uint32_t)(cap & 0xFF);
    if (!vcnt) return -1;

    uint64_t phys_mask = (1ULL << phys_addr_bits()) - 1;

    /* Smallest power-of-two region that covers [base, base+size) */
    uint64_t end = base + size - 1;
    uint64_t p;
    for (p = 4096; p < (1ULL << 32); p <<= 1) {
        uint64_t rbase = base & ~(p - 1);
        if (rbase + p - 1 >= end) break;
    }
    if (p > (1ULL << 32)) return -1;
    uint64_t rbase = base & ~(p - 1);

    /* Find a free variable-range slot, and check the occupied ones:
     * two VALID ranges that overlap with different types produce
     * UNDEFINED memory ordering (Intel SDM) — on real firmware that is
     * a hang, not a glitch.  Dump what EFI left so the overlap, if any,
     * is visible instead of a dead screen. */
    int slot = -1;
    for (uint32_t n = 0; n < vcnt; n++) {
        uint64_t m = rdmsr64(MSR_MTRR_PHYSBASE + 2 * n + 1);
        uint64_t b = rdmsr64(MSR_MTRR_PHYSBASE + 2 * n);
        if (!(m & MTRRMASK_VALID)) {
            if (slot < 0) slot = (int)n;
            continue;
        }
        uint64_t mfield = m & phys_mask & ~(uint64_t)MTRRMASK_VALID;
        uint64_t msize  = (~mfield & phys_mask) + 1;
        uint64_t mbase  = b & ~(msize - 1);
        kprint("[MTRR] slot "); kprinthex(n);
        kprint(" base="); kprinthex(mbase);
        kprint(" size="); kprinthex(msize);
        kprint(" type="); kprinthex(b & 0xFF);
        kprint("\n");
        /* Overlap with our intended region? */
        if (mbase < rbase + p && mbase + msize > rbase) {
            if ((b & 0xFF) == MTRR_TYPE_WC) {
                kprint("[MTRR] region already WC via slot ");
                kprinthex(n); kprint("\n");
                return 0;
            }
            kprint("[MTRR] SKIP: overlaps slot "); kprinthex(n);
            kprint(" (type "); kprinthex(b & 0xFF);
            kprint(") — overlapping ranges are undefined\n");
            return -1;
        }
    }
    if (slot < 0) return -1;

    /* Full Intel SDM Vol. 3A §11.11.8 sequence:
     *   cli — an IRQ in the CD window runs uncached (and faults
     *         triple-fault: the IDT isn't up yet at this call site)
     *   PGE off + TLB flush — a stale global translation for the FB
     *         range keeps pre-MTRR behaviour and is the classic hang;
     *   CR0.CD=1 NW=0, wbinvd; MTRRs off, wbinvd; program; wbinvd;
     *   MTRRs on, wbinvd; restore CR0; flush TLB; restore CR4. */
    uint64_t cr0 = read_cr0();
    uint64_t cr4 = read_cr4();
    kprint("[MTRR] base "); kprinthex(rbase); kprint(" size ");
    kprinthex(p); kprint(" slot "); kprinthex((uint64_t)slot);
    kprint(" cr4="); kprinthex(cr4); kprint("\n");

    /* Save/restore IF — cli for the CD window, then put flags back
     * exactly (the caller may legitimately run with IRQs off). */
    uint64_t rflags = irq_save();

    if (cr4 & CR4_PGE)
        write_cr4(cr4 & ~(uint64_t)CR4_PGE);
    write_cr3(read_cr3());            /* flush non-global TLB too */

    write_cr0((cr0 | CR0_CD) & ~(uint64_t)CR0_NW);
    __asm__ volatile ("wbinvd" ::: "memory");

    uint64_t deftype = rdmsr64(MSR_MTRR_DEFTYPE);
    wrmsr64(MSR_MTRR_DEFTYPE, deftype & ~(uint64_t)MTRRDEF_E);
    __asm__ volatile ("wbinvd" ::: "memory");

    wrmsr64(MSR_MTRR_PHYSBASE + 2 * slot,
            (rbase & phys_mask) | MTRR_TYPE_WC);
    wrmsr64(MSR_MTRR_PHYSBASE + 2 * slot + 1,
            ((~(p - 1)) & phys_mask) | MTRRMASK_VALID);
    __asm__ volatile ("wbinvd" ::: "memory");

    wrmsr64(MSR_MTRR_DEFTYPE, deftype | MTRRDEF_E);
    __asm__ volatile ("wbinvd" ::: "memory");
    write_cr0(cr0);

    if (cr4 & CR4_PGE)
        write_cr4(cr4);               /* re-enables PGE + flushes TLB */
    else
        write_cr3(read_cr3());

    irq_restore(rflags);

    kprint("[MTRR] WC region @ ");
    kprinthex(rbase);
    kprint(" size ");
    kprinthex(p);
    kprint(" slot ");
    kprinthex((uint64_t)slot);
    kprint("\n");
    return 0;
}
