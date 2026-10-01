/* acpi.c — Minimal ACPI table discovery and MADT/MCFG parsing
 *
 * RSDP discovery order:
 *   1. Multiboot2 "ACPI new RSDP" tag (type 15) — EFI boots
 *   2. Multiboot2 "ACPI old RSDP" tag (type 14) — legacy boots
 *   3. Multiboot2 EFI64 system table tag (type 12) → EFI config table
 *   4. Signature scan of 0xE0000-0xFFFFF (SeaBIOS/legacy)
 */

#include "acpi.h"
#include "../boot/kprint.h"
#include <stdint.h>
#include <stddef.h>

/* ------------------------------------------------------------------ */
/* ACPI structures                                                     */
/* ------------------------------------------------------------------ */

typedef struct __attribute__((packed)) {
    char     sig[8];
    uint8_t  checksum;
    char     oemid[6];
    uint8_t  revision;
    uint32_t rsdt_addr;
    uint32_t length;      /* rev >= 2 only */
    uint64_t xsdt_addr;
    uint8_t  xchecksum;
    uint8_t  reserved[3];
} Rsdp;

typedef struct __attribute__((packed)) {
    char     sig[4];
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    char     oemid[6];
    char     oem_table[8];
    uint32_t oem_rev;
    uint32_t creator_id;
    uint32_t creator_rev;
} SdtHeader;

/* MADT */
typedef struct __attribute__((packed)) {
    uint32_t lapic_addr;
    uint32_t flags;       /* bit0 = dual-8259 PIC installed */
} MadtBody;

typedef struct __attribute__((packed)) {
    uint8_t type;
    uint8_t length;
} MadtEntry;

/* MADT entry types */
#define MADT_LAPIC       0
#define MADT_IOAPIC      1
#define MADT_ISO         2
#define MADT_LAPIC_ADDR  5

typedef struct __attribute__((packed)) {
    uint8_t  type;        /* 1 */
    uint8_t  length;      /* 12 */
    uint8_t  id;
    uint8_t  reserved;
    uint32_t addr;
    uint32_t gsi_base;
} MadtIoApic;

typedef struct __attribute__((packed)) {
    uint8_t  type;        /* 2 */
    uint8_t  length;      /* 10 */
    uint8_t  bus;         /* 0 = ISA */
    uint8_t  source;
    uint32_t gsi;
    uint16_t flags;
} MadtIso;

typedef struct __attribute__((packed)) {
    uint8_t  type;        /* 5 */
    uint8_t  length;      /* 12 */
    uint16_t reserved;
    uint64_t lapic_addr;
} MadtLapicAddr;

/* MCFG */
typedef struct __attribute__((packed)) {
    SdtHeader hdr;
    uint64_t  reserved;
    /* Followed by 16-byte entries: base(8) seg(2) startbus(1) endbus(1) rsvd(4) */
} Mcfg;

/* Multiboot2 tag types we care about */
#define MB2_TAG_EFI64_SYSTAB 12
#define MB2_TAG_ACPI_OLD     14
#define MB2_TAG_ACPI_NEW     15

/* EFI config table GUIDs */
/* ACPI 2.0: 8868e871-e4f1-11d3-bc22-0080c73c8881 */
static const uint8_t k_guid_acpi20[16] = {
    0x71,0xe8,0x68,0x88, 0xf1,0xe4, 0xd3,0x11,
    0xbc,0x22, 0x00,0x80,0xc7,0x3c,0x88,0x81 };
/* ACPI 1.0: eb9d2d30-2d88-11d3-9a16-0090273fc14d */
static const uint8_t k_guid_acpi10[16] = {
    0x30,0x2d,0x9d,0xeb, 0x88,0x2d, 0xd3,0x11,
    0x9a,0x16, 0x00,0x90,0x27,0x3f,0xc1,0x4d };

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

static int          g_acpi_ok   = 0;
static int          g_pcat      = 0;
static uint32_t     g_lapic     = 0xFEE00000;
static uint64_t     g_ecam      = 0;
static AcpiIoApic   g_ioapics[ACPI_MAX_IOAPICS];
static int          g_nioapics  = 0;
static AcpiIso      g_isos[ACPI_MAX_ISOS];
static int          g_nisos     = 0;
static uint32_t     g_rsdt      = 0;      /* phys */
static uint64_t     g_xsdt      = 0;      /* phys */
static int          g_use_xsdt  = 0;

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static uint8_t checksum8(const void *p, uint32_t len)
{
    const uint8_t *b = (const uint8_t *)p;
    uint8_t sum = 0;
    for (uint32_t i = 0; i < len; i++) sum = (uint8_t)(sum + b[i]);
    return sum;
}

static int guid_eq(const uint8_t *a, const uint8_t *b)
{
    for (int i = 0; i < 16; i++) if (a[i] != b[i]) return 0;
    return 1;
}

static int rsdp_valid(const Rsdp *r)
{
    if (!r) return 0;
    if (r->sig[0] != 'R' || r->sig[1] != 'S' || r->sig[2] != 'D' ||
        r->sig[3] != ' ' || r->sig[4] != 'P' || r->sig[5] != 'T' ||
        r->sig[6] != 'R' || r->sig[7] != ' ') return 0;
    if (checksum8(r, 20) != 0) return 0;
    return 1;
}

/* ------------------------------------------------------------------ */
/* RSDP discovery                                                      */
/* ------------------------------------------------------------------ */

typedef struct __attribute__((packed)) {
    uint32_t type;
    uint32_t size;
} Mb2TagHdr;

static const Rsdp *scan_bios_area(void)
{
    /* EBDA segment is at 0x40:0x0E; scan it first if sane, then 0xE0000. */
    uint16_t ebda_seg = *(volatile uint16_t *)(uintptr_t)0x40E;
    uint32_t ebda = ((uint32_t)ebda_seg) << 4;
    if (ebda >= 0x80000 && ebda < 0xA0000) {
        const uint8_t *p = (const uint8_t *)(uintptr_t)ebda;
        for (uint32_t off = 0; off < 1024; off += 16)
            if (rsdp_valid((const Rsdp *)(p + off)))
                return (const Rsdp *)(p + off);
    }
    const uint8_t *p = (const uint8_t *)(uintptr_t)0xE0000;
    for (uint32_t off = 0; off < 0x20000; off += 16)
        if (rsdp_valid((const Rsdp *)(p + off)))
            return (const Rsdp *)(p + off);
    return 0;
}

static const Rsdp *rsdp_from_efi_systab(uint64_t systab_phys)
{
    /* EFI_SYSTEM_TABLE: hdr(24) fw_vendor(8) fw_rev(4) consh(4) conin(8)
     * conouth(4) conout(8) stderrh(4) stderr(8) rt(8) bs(8)
     * ncfg(8) cfgtab(8)  — 64-bit layout, config table at offset 104. */
    const uint8_t *st = (const uint8_t *)(uintptr_t)systab_phys;
    uint64_t ncfg  = *(const uint64_t *)(st + 96);
    uint64_t cfg   = *(const uint64_t *)(st + 104);
    if (!cfg || ncfg > 4096) return 0;
    const uint8_t *ct = (const uint8_t *)(uintptr_t)cfg;
    const Rsdp *rsdp10 = 0;
    for (uint64_t i = 0; i < ncfg; i++) {
        const uint8_t *e = ct + i * 24;
        if (guid_eq(e, k_guid_acpi20))
            return *(const Rsdp *const *)(e + 16);
        if (guid_eq(e, k_guid_acpi10))
            rsdp10 = *(const Rsdp *const *)(e + 16);
    }
    return rsdp10;
}

static const Rsdp *find_rsdp(uint32_t mb2_phys)
{
    if (mb2_phys) {
        uint32_t total = *(const uint32_t *)(uintptr_t)mb2_phys;
        const uint8_t *p   = (const uint8_t *)(uintptr_t)(mb2_phys + 8);
        const uint8_t *end = (const uint8_t *)(uintptr_t)(mb2_phys + total);
        const Rsdp *v1 = 0;
        uint64_t systab = 0;
        while (p + 8 <= end) {
            const Mb2TagHdr *t = (const Mb2TagHdr *)p;
            if (t->type == 0) break;
            if (t->type == MB2_TAG_ACPI_NEW && t->size >= 44) {
                const Rsdp *r = (const Rsdp *)(p + 8);
                if (rsdp_valid(r)) return r;
            } else if (t->type == MB2_TAG_ACPI_OLD && t->size >= 28) {
                const Rsdp *r = (const Rsdp *)(p + 8);
                if (rsdp_valid(r)) v1 = r;
            } else if (t->type == MB2_TAG_EFI64_SYSTAB && t->size >= 16) {
                systab = *(const uint64_t *)(p + 8);
            }
            p += (t->size + 7) & ~7u;
        }
        if (systab) {
            const Rsdp *r = rsdp_from_efi_systab(systab);
            if (r) return r;
        }
        if (v1) return v1;
    }
    return scan_bios_area();
}

/* ------------------------------------------------------------------ */
/* Table walking                                                       */
/* ------------------------------------------------------------------ */

const void *ACPI_FindTable(const char sig[4])
{
    if (!g_acpi_ok) return 0;
    if (g_use_xsdt) {
        const SdtHeader *xs = (const SdtHeader *)(uintptr_t)g_xsdt;
        uint32_t n = (xs->length - sizeof(SdtHeader)) / 8;
        const uint64_t *ents = (const uint64_t *)((const uint8_t *)xs + sizeof(SdtHeader));
        for (uint32_t i = 0; i < n; i++) {
            const SdtHeader *h = (const SdtHeader *)(uintptr_t)ents[i];
            if (h->sig[0] == sig[0] && h->sig[1] == sig[1] &&
                h->sig[2] == sig[2] && h->sig[3] == sig[3])
                return h;
        }
    } else {
        const SdtHeader *rs = (const SdtHeader *)(uintptr_t)g_rsdt;
        uint32_t n = (rs->length - sizeof(SdtHeader)) / 4;
        const uint32_t *ents = (const uint32_t *)((const uint8_t *)rs + sizeof(SdtHeader));
        for (uint32_t i = 0; i < n; i++) {
            const SdtHeader *h = (const SdtHeader *)(uintptr_t)ents[i];
            if (h->sig[0] == sig[0] && h->sig[1] == sig[1] &&
                h->sig[2] == sig[2] && h->sig[3] == sig[3])
                return h;
        }
    }
    return 0;
}

static void parse_madt(const SdtHeader *madt)
{
    const MadtBody *body = (const MadtBody *)(madt + 1);
    g_lapic = body->lapic_addr;
    g_pcat  = (body->flags & 1) != 0;

    const uint8_t *p   = (const uint8_t *)(body + 1);
    const uint8_t *end = (const uint8_t *)madt + madt->length;
    while (p + 2 <= end) {
        const MadtEntry *e = (const MadtEntry *)p;
        if (e->length < 2 || p + e->length > end) break;
        if (e->type == MADT_IOAPIC && e->length >= sizeof(MadtIoApic)
            && g_nioapics < ACPI_MAX_IOAPICS) {
            const MadtIoApic *io = (const MadtIoApic *)p;
            g_ioapics[g_nioapics].id        = io->id;
            g_ioapics[g_nioapics].mmio_base = io->addr;
            g_ioapics[g_nioapics].gsi_base  = io->gsi_base;
            g_nioapics++;
            kprint("[ACPI] IO-APIC id="); kprintdec(io->id);
            kprint(" base="); kprinthex(io->addr);
            kprint(" gsi_base="); kprintdec(io->gsi_base); kprint("\n");
        } else if (e->type == MADT_ISO && e->length >= sizeof(MadtIso)
                   && g_nisos < ACPI_MAX_ISOS) {
            const MadtIso *iso = (const MadtIso *)p;
            g_isos[g_nisos].bus    = iso->bus;
            g_isos[g_nisos].source = iso->source;
            g_isos[g_nisos].gsi    = iso->gsi;
            g_isos[g_nisos].flags  = iso->flags;
            g_nisos++;
            kprint("[ACPI] ISO irq"); kprintdec(iso->source);
            kprint("->gsi"); kprintdec(iso->gsi);
            kprint(" flags="); kprinthex(iso->flags); kprint("\n");
        } else if (e->type == MADT_LAPIC_ADDR && e->length >= sizeof(MadtLapicAddr)) {
            const MadtLapicAddr *la = (const MadtLapicAddr *)p;
            g_lapic = (uint32_t)la->lapic_addr;
        }
        p += e->length;
    }
}

static void parse_mcfg(const SdtHeader *mcfg)
{
    if (mcfg->length < sizeof(SdtHeader) + 8 + 16) return;
    const uint8_t *e = (const uint8_t *)mcfg + sizeof(SdtHeader) + 8;
    g_ecam = *(const uint64_t *)e;    /* first segment's base */
    kprint("[ACPI] ECAM base="); kprinthex(g_ecam); kprint("\n");
}

void ACPI_Init(uint32_t mb2_phys)
{
    g_acpi_ok  = 0;
    g_nioapics = 0;
    g_nisos    = 0;
    g_ecam     = 0;
    g_lapic    = 0xFEE00000;

    const Rsdp *r = find_rsdp(mb2_phys);
    if (!r) {
        kprint("[ACPI] RSDP not found\n");
        return;
    }
    kprint("[ACPI] RSDP @"); kprinthex((uint64_t)(uintptr_t)r);
    kprint(" rev="); kprintdec(r->revision); kprint("\n");

    if (r->revision >= 2 && r->xsdt_addr) {
        g_xsdt = r->xsdt_addr;
        g_use_xsdt = 1;
    } else {
        g_rsdt = r->rsdt_addr;
        g_use_xsdt = 0;
    }

    g_acpi_ok = 1;

    const SdtHeader *madt = (const SdtHeader *)ACPI_FindTable("APIC");
    if (madt) parse_madt(madt);
    const SdtHeader *mcfg = (const SdtHeader *)ACPI_FindTable("MCFG");
    if (mcfg) parse_mcfg(mcfg);

    kprint("[ACPI] LAPIC="); kprinthex(g_lapic);
    kprint(" ioapics="); kprintdec(g_nioapics);
    kprint(" isos="); kprintdec(g_nisos); kprint("\n");
}

int      ACPI_Present(void)        { return g_acpi_ok; }
int      ACPI_PcatCompat(void)     { return g_pcat; }
uint32_t ACPI_LapicBase(void)      { return g_lapic; }
int      ACPI_NumIoApics(void)     { return g_nioapics; }
const AcpiIoApic *ACPI_IoApic(int i) { return (i >= 0 && i < g_nioapics) ? &g_ioapics[i] : 0; }
int      ACPI_NumIsos(void)        { return g_nisos; }
const AcpiIso *ACPI_Iso(int i)     { return (i >= 0 && i < g_nisos) ? &g_isos[i] : 0; }
uint64_t ACPI_EcamBase(void)       { return g_ecam; }

int ACPI_IsaToGsi(int isa_irq, uint16_t *flags_out)
{
    for (int i = 0; i < g_nisos; i++) {
        if (g_isos[i].bus == 0 && g_isos[i].source == isa_irq) {
            if (flags_out) *flags_out = g_isos[i].flags;
            return (int)g_isos[i].gsi;
        }
    }
    if (flags_out) *flags_out = 0;
    return isa_irq;
}
