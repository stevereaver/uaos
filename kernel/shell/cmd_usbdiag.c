/* cmd_usbdiag.c — C:usbdiag — UHCI interrupt-path register dump (UAOS-183)
 *
 * Follows a USB interrupt from the host controller to the CPU and shows
 * where it stops.  Read-only: no register is written.
 *
 *   usbdiag          one snapshot of every stage
 *   usbdiag <sec>    snapshot, then sample every stage for <sec> seconds
 *                    (1-60) while you use the keyboard/trackpad, then
 *                    print per-controller counts and a verdict
 *
 * Stages, per controller:
 *   HC      USBCMD/USBSTS/USBINTR, poll-path USBINT latch count, TD IOC
 *   PCI     command (INTx disable), status (bit3 interrupt status),
 *           USBLEGSUP (bit13 PIRQ enable, SMI enables, trap bits)
 *   chipset ICH PIRQA-H_ROUT, DxxIP/DxxIR, OIC.AEN
 *   IO-APIC RTE for the controller's GSI (mask, trigger, polarity,
 *           remote IRR, delivery status)
 *   LAPIC   IRR/ISR bit for the vector, TPR
 */

#include "cmd_internal.h"
#include "../drivers/usb.h"
#include "../irq/irq.h"
#include "../irq/ioapic.h"

extern volatile uint64_t g_pit_ticks;

static void ud_hexd(char *l, uint32_t v, int digits)
{
    char b[9];
    for (int i = 0; i < digits; i++)
        b[i] = "0123456789ABCDEF"[(v >> ((digits - 1 - i) * 4)) & 0xF];
    b[digits] = '\0';
    cmd_scat(l, b, CMD_MAX_LINE);
}

static void ud_hex(char *l, uint32_t v, int digits)
{
    cmd_scat(l, "0x", CMD_MAX_LINE);
    ud_hexd(l, v, digits);
}

static void ud_dec(char *l, uint32_t v)
{
    char b[12];
    cmd_uint_to_dec(v, b, sizeof(b));
    cmd_scat(l, b, CMD_MAX_LINE);
}

static void ud_s(char *l, const char *s) { cmd_scat(l, s, CMD_MAX_LINE); }

static void ud_bit(char *l, const char *name, int v)
{
    ud_s(l, name); ud_s(l, v ? "=1" : "=0");
}

static int ud_gsi(const UhciDiag *d)
{
    return (IRQ_Mode() == IRQ_MODE_IOAPIC && d->irq_vec >= 32)
           ? d->irq_vec - 32 : -1;
}

static int ud_lapic_bit(uint32_t base, int vec)
{
    if (vec < 0 || vec > 255) return 0;
    return (LAPIC_Read(base + 0x10u * (uint32_t)(vec >> 5)) >> (vec & 31)) & 1;
}

static void ud_print_ich(NativeCmdCtx *ctx)
{
    char l[CMD_MAX_LINE];
    IrqIchDiag c;
    int rc = IRQ_DiagIch(&c);
    if (!c.ich) { PRINT("chipset: not an ICH8/9/10 LPC (no RCBA routing)"); return; }

    cmd_scopy(l, "chipset: RCBA=", CMD_MAX_LINE);
    if (rc != 0) { ud_s(l, "unavailable"); PRINT(l); return; }
    ud_hex(l, c.rcba, 8);
    ud_s(l, "  OIC="); ud_hex(l, c.oic, 2);
    ud_s(l, " ["); ud_bit(l, "AEN", c.oic & 1); ud_s(l, "]");
    PRINT(l);

    cmd_scopy(l, "  PIRQA-H_ROUT:", CMD_MAX_LINE);
    for (int i = 0; i < 8; i++) { ud_s(l, " "); ud_hex(l, c.pirq[i], 2); }
    ud_s(l, "  (bit7=1: ISA route off)");
    PRINT(l);

    for (int dev = 25; dev <= 31; dev++) {
        cmd_scopy(l, "  D", CMD_MAX_LINE); ud_dec(l, (uint32_t)dev);
        ud_s(l, "IP="); ud_hex(l, c.dip[dev - 25], 8);
        ud_s(l, " D"); ud_dec(l, (uint32_t)dev);
        ud_s(l, "IR="); ud_hex(l, c.dir[dev - 25], 4);
        PRINT(l);
    }
}

static void ud_print_hc(NativeCmdCtx *ctx, int i, const UhciDiag *d)
{
    char l[CMD_MAX_LINE];
    int gsi = ud_gsi(d);

    cmd_scopy(l, "uhci", CMD_MAX_LINE); ud_dec(l, (uint32_t)i);
    ud_s(l, " "); ud_hexd(l, d->bus, 2); ud_s(l, ":"); ud_hexd(l, d->dev, 2);
    ud_s(l, "."); ud_dec(l, d->fn);
    ud_s(l, " io="); ud_hex(l, d->io, 4);
    ud_s(l, " vec=");
    if (d->irq_vec >= 0) ud_dec(l, (uint32_t)d->irq_vec); else ud_s(l, "none");
    if (gsi >= 0) { ud_s(l, " gsi="); ud_dec(l, (uint32_t)gsi); }
    ud_s(l, " irq_hits="); ud_dec(l, d->irq_hits);
    ud_s(l, " poll_usbint="); ud_dec(l, d->poll_usbint);
    PRINT(l);

    cmd_scopy(l, "  HC   USBCMD=", CMD_MAX_LINE); ud_hex(l, d->usbcmd, 4);
    ud_s(l, " USBSTS="); ud_hex(l, d->usbsts, 4);
    ud_s(l, " USBINTR="); ud_hex(l, d->usbintr, 4);
    ud_s(l, " ["); ud_bit(l, "IOC", (d->usbintr >> 2) & 1);
    ud_s(l, " "); ud_bit(l, "SPI", (d->usbintr >> 3) & 1); ud_s(l, "]");
    ud_s(l, " FRNUM="); ud_hex(l, d->frnum, 4);
    PRINT(l);

    cmd_scopy(l, "       PORTSC=", CMD_MAX_LINE); ud_hex(l, d->portsc[0], 4);
    ud_s(l, "/"); ud_hex(l, d->portsc[1], 4);
    ud_s(l, " pipes="); ud_dec(l, (uint32_t)d->npipes);
    ud_s(l, " with_IOC="); ud_dec(l, (uint32_t)d->pipes_ioc);
    PRINT(l);

    cmd_scopy(l, "  PCI  cmd=", CMD_MAX_LINE); ud_hex(l, d->pci_cmd, 4);
    ud_s(l, " ["); ud_bit(l, "IO", d->pci_cmd & 1);
    ud_s(l, " "); ud_bit(l, "BM", (d->pci_cmd >> 2) & 1);
    ud_s(l, " "); ud_bit(l, "INTxDis", (d->pci_cmd >> 10) & 1); ud_s(l, "]");
    ud_s(l, " sts="); ud_hex(l, d->pci_sts, 4);
    ud_s(l, " ["); ud_bit(l, "IntSts", (d->pci_sts >> 3) & 1); ud_s(l, "]");
    ud_s(l, " line="); ud_dec(l, d->int_line);
    ud_s(l, " pin=");
    if (d->int_pin >= 1 && d->int_pin <= 4) {
        char p[2] = { (char)('A' + d->int_pin - 1), 0 };
        ud_s(l, p);
    } else ud_hex(l, d->int_pin, 2);
    PRINT(l);

    cmd_scopy(l, "       LEGSUP=", CMD_MAX_LINE); ud_hex(l, d->legsup, 4);
    ud_s(l, " ["); ud_bit(l, "PIRQEN", (d->legsup >> 13) & 1);
    ud_s(l, " "); ud_bit(l, "SMIEN", (d->legsup >> 4) & 1);
    ud_s(l, " "); ud_bit(l, "SMIbyUSB", (d->legsup >> 12) & 1);
    ud_s(l, " traps="); ud_hex(l, d->legsup & 0x0F0Fu, 4); ud_s(l, "]");
    PRINT(l);

    if (gsi >= 0) {
        uint32_t lo = 0, hi = 0;
        cmd_scopy(l, "  APIC RTE", CMD_MAX_LINE); ud_dec(l, (uint32_t)gsi);
        if (IOAPIC_ReadRTE((uint32_t)gsi, &lo, &hi) != 0) {
            ud_s(l, " no IO-APIC covers this GSI");
        } else {
            ud_s(l, " lo="); ud_hex(l, lo, 8);
            ud_s(l, " hi="); ud_hex(l, hi, 8);
            ud_s(l, " [vec="); ud_dec(l, lo & 0xFF);
            ud_s(l, (lo & (1u << 15)) ? " level" : " edge");
            ud_s(l, (lo & (1u << 13)) ? " low" : " high");
            ud_s(l, (lo & (1u << 16)) ? " MASKED" : " unmasked");
            ud_s(l, " "); ud_bit(l, "rIRR", (lo & RTE_BIT_RIRR) != 0);
            ud_s(l, " "); ud_bit(l, "dlv", (lo & RTE_BIT_DELIVS) != 0);
            ud_s(l, "]");
        }
        PRINT(l);

        cmd_scopy(l, "  LAPIC IRR=", CMD_MAX_LINE);
        ud_dec(l, (uint32_t)ud_lapic_bit(0x200, d->irq_vec));
        ud_s(l, " ISR="); ud_dec(l, (uint32_t)ud_lapic_bit(0x100, d->irq_vec));
        ud_s(l, " (vector "); ud_dec(l, (uint32_t)d->irq_vec); ud_s(l, ")");
        PRINT(l);
    }
}

#define UD_MAX 8

typedef struct {
    uint32_t hits0, usbint0;      /* counters at sample start */
    uint32_t samples, sts_int, pci_int, rirr, dlv, irr;
} UdSample;

void Cmd_Usbdiag(NativeCmdCtx *ctx, const char *args)
{
    uint32_t secs = 0;
    if (args && *args) {
        const char *p = args;
        while (*p >= '0' && *p <= '9') { secs = secs * 10 + (uint32_t)(*p - '0'); p++; }
        if (secs < 1 || secs > 60 || (*p && *p != ' ')) {
            PRINT("usage: usbdiag [<sec>]   (sample 1-60 s while using the device)");
            return;
        }
    }

    char l[CMD_MAX_LINE];
    int n = UHCI_DiagCount();
    if (n > UD_MAX) n = UD_MAX;

    cmd_scopy(l, "irq mode: ", CMD_MAX_LINE);
    ud_s(l, IRQ_Mode() == IRQ_MODE_IOAPIC ? "IO-APIC" : "PIC");
    ud_s(l, "  LAPIC TPR="); ud_hex(l, LAPIC_Read(0x80) & 0xFF, 2);
    ud_s(l, "  uhci controllers="); ud_dec(l, (uint32_t)n);
    PRINT(l);
    ud_print_ich(ctx);

    UhciDiag d[UD_MAX];
    for (int i = 0; i < n; i++) {
        UHCI_DiagRead(i, &d[i]);
        ud_print_hc(ctx, i, &d[i]);
    }
    if (!secs || !n) return;

    static UdSample s[UD_MAX];
    for (int i = 0; i < n; i++) {
        s[i] = (UdSample){0};
        s[i].hits0 = d[i].irq_hits;
        s[i].usbint0 = d[i].poll_usbint;
    }

    cmd_scopy(l, "sampling ", CMD_MAX_LINE); ud_dec(l, secs);
    ud_s(l, " s - use the keyboard/trackpad now...");
    PRINT(l);

    uint64_t end = g_pit_ticks + (uint64_t)secs * 100;
    while (g_pit_ticks < end) {
        for (int i = 0; i < n; i++) {
            UhciDiag x;
            UHCI_DiagRead(i, &x);
            s[i].samples++;
            if (x.usbsts & 1)            s[i].sts_int++;
            if (x.pci_sts & (1u << 3))   s[i].pci_int++;
            int gsi = ud_gsi(&x);
            uint32_t lo, hi;
            if (gsi >= 0 && IOAPIC_ReadRTE((uint32_t)gsi, &lo, &hi) == 0) {
                if (lo & RTE_BIT_RIRR)   s[i].rirr++;
                if (lo & RTE_BIT_DELIVS) s[i].dlv++;
            }
            if (ud_lapic_bit(0x200, x.irq_vec)) s[i].irr++;
        }
        CMD_YIELD(ctx, 10);
    }

    PRINT("results (per controller; counts are samples where the bit was set):");
    for (int i = 0; i < n; i++) {
        UhciDiag x;
        UHCI_DiagRead(i, &x);
        uint32_t dh = x.irq_hits - s[i].hits0;
        uint32_t du = x.poll_usbint - s[i].usbint0;

        cmd_scopy(l, "uhci", CMD_MAX_LINE); ud_dec(l, (uint32_t)i);
        ud_s(l, " samples="); ud_dec(l, s[i].samples);
        ud_s(l, " dispatches="); ud_dec(l, dh);
        ud_s(l, " poll_usbint="); ud_dec(l, du);
        PRINT(l);
        cmd_scopy(l, "  USBSTS.INT=", CMD_MAX_LINE); ud_dec(l, s[i].sts_int);
        ud_s(l, " PCI.IntSts="); ud_dec(l, s[i].pci_int);
        ud_s(l, " RTE.rIRR="); ud_dec(l, s[i].rirr);
        ud_s(l, " RTE.dlv="); ud_dec(l, s[i].dlv);
        ud_s(l, " LAPIC.IRR="); ud_dec(l, s[i].irr);
        PRINT(l);

        const char *v;
        if (x.irq_vec < 0)
            v = "no vector attached (IRQ_AttachPCI failed at boot)";
        else if (dh)
            v = "OK - INTx delivered and dispatched";
        else if (!du && !s[i].sts_int)
            v = "HC never latched USBINT - idle port or no IOC completions";
        else if (!s[i].pci_int && !((x.legsup >> 13) & 1))
            v = "USBINT latches, LEGSUP.PIRQEN=0 - INTx not routed to PIRQ";
        else if (!s[i].pci_int)
            v = "USBINT latches, PCI IntSts never set - HC not asserting INTx?";
        else if (!s[i].rirr && !s[i].irr)
            v = "INTx asserted, IO-APIC pin never saw it - PIRQ->GSI mismatch";
        else
            v = "IO-APIC/LAPIC saw it but no dispatch - vector/LAPIC problem";
        cmd_scopy(l, "  verdict: ", CMD_MAX_LINE); ud_s(l, v);
        PRINT(l);
        if (!dh && !((x.legsup >> 13) & 1))
            PRINT("  note: LEGSUP.PIRQEN=0 - HC interrupts cannot reach PIRQ");
    }
}
