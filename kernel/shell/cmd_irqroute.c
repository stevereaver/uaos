/* cmd_irqroute.c — C:irqroute — PCI pin -> PIRQ -> GSI -> vector decode
 * (UAOS-199)
 *
 *   irqroute              every PCI function's interrupt chain + verdict
 *   irqroute BDF=b:d.f    decode one function in detail
 *
 * Generalises the usbdiag ICH decode to every device — the UAOS-174
 * ICH8 DxxIR hole would have shown up here as an unrouted pin.
 */

#include "cmd_internal.h"
#include "../irq/irq.h"

static const char *route_src(int s)
{
    switch (s) {
    case IRQ_ROUTE_ICH:     return "ich-dxxir";
    case IRQ_ROUTE_INTLINE: return "intline";
    default:                return "none";
    }
}

static void dump_one(NativeCmdCtx *ctx, uint8_t bus, uint8_t dev, uint8_t fn,
                     int verbose)
{
    char line[CMD_MAX_LINE], num[24];
    uint32_t id = IRQ_PciRead32(bus, dev, fn, 0);
    if ((id & 0xFFFF) == 0xFFFF || id == 0) return;

    IrqRouteInfo ri;
    IRQ_RouteInspect(bus, dev, fn, &ri);   /* 0 = decoded/no-pin */

    cmd_scopy(line, " ", CMD_MAX_LINE);
    cmd_uint_to_dec(bus, num, sizeof(num)); cmd_scat(line, num, CMD_MAX_LINE);
    cmd_scat(line, ":", CMD_MAX_LINE);
    cmd_uint_to_dec(dev, num, sizeof(num)); cmd_scat(line, num, CMD_MAX_LINE);
    cmd_scat(line, ".", CMD_MAX_LINE);
    cmd_uint_to_dec(fn, num, sizeof(num)); cmd_scat(line, num, CMD_MAX_LINE);
    cmd_scat(line, " pin=", CMD_MAX_LINE);
    cmd_uint_to_dec(ri.pin, num, sizeof(num)); cmd_scat(line, num, CMD_MAX_LINE);
    cmd_scat(line, " intline=", CMD_MAX_LINE);
    cmd_uint_to_dec(ri.intline, num, sizeof(num)); cmd_scat(line, num, CMD_MAX_LINE);
    PRINT(line);

    if (!ri.pin) { PRINT("   no interrupt pin"); return; }

    cmd_scopy(line, "   chip_pin=", CMD_MAX_LINE);
    cmd_sdec(ri.chip_pin, num, sizeof(num)); cmd_scat(line, num, CMD_MAX_LINE);
    cmd_scat(line, " pirq=", CMD_MAX_LINE);
    if (ri.pirq >= 0) { char c[2] = { (char)('A' + ri.pirq), 0 }; cmd_scat(line, c, CMD_MAX_LINE); }
    else cmd_scat(line, "unrouted", CMD_MAX_LINE);
    cmd_scat(line, " gsi=", CMD_MAX_LINE);
    cmd_sdec(ri.gsi, num, sizeof(num)); cmd_scat(line, num, CMD_MAX_LINE);
    cmd_scat(line, " via=", CMD_MAX_LINE);
    cmd_scat(line, route_src(ri.source), CMD_MAX_LINE);
    PRINT(line);

    if (verbose && ri.ich) {
        cmd_scopy(line, "   dip=", CMD_MAX_LINE);
        char h[12]; int hi = 0;
        uint32_t v = ri.dip;
        int started = 0;
        for (int s = 28; s >= 0; s -= 4) {
            int nib = (int)((v >> s) & 15);
            if (nib || started || s == 0) { h[hi++] = "0123456789abcdef"[nib]; started = 1; }
        }
        h[hi] = 0;
        cmd_scat(line, "0x", CMD_MAX_LINE); cmd_scat(line, h, CMD_MAX_LINE);
        cmd_scat(line, " dir=", CMD_MAX_LINE);
        cmd_scat(line, "0x", CMD_MAX_LINE);
        hi = 0; v = ri.dir; started = 0;
        for (int s = 12; s >= 0; s -= 4) {
            int nib = (int)((v >> s) & 15);
            if (nib || started || s == 0) { h[hi++] = "0123456789abcdef"[nib]; started = 1; }
        }
        h[hi] = 0;
        cmd_scat(line, h, CMD_MAX_LINE);
        PRINT(line);
    }

    if (ri.gsi >= 0) {
        int vec = IRQ_VecForGsi(ri.gsi);
        cmd_scopy(line, "   vec=", CMD_MAX_LINE);
        if (vec >= 0) {
            cmd_uint_to_dec((uint32_t)vec, num, sizeof(num));
            cmd_scat(line, num, CMD_MAX_LINE);
            const char *nm = IDT_VectorName(vec);
            if (nm) { cmd_scat(line, " (", CMD_MAX_LINE); cmd_scat(line, nm, CMD_MAX_LINE); cmd_scat(line, ")", CMD_MAX_LINE); }
            int kind = IRQ_VecKind(vec);
            cmd_scat(line, kind == IRQ_VEC_MSI ? " msi" : kind == IRQ_VEC_APIC ? " apic" : kind == IRQ_VEC_PIC ? " pic" : " ?", CMD_MAX_LINE);
        } else {
            cmd_scat(line, "UNASSIGNED", CMD_MAX_LINE);
        }
        PRINT(line);
    } else if (ri.pin) {
        PRINT("   * UNRESOLVED ROUTE *");
    }
}

void Cmd_Irqroute(NativeCmdCtx *ctx, const char *args)
{
    const char *bdf = cmd_kv_find(args, "bdf");
    if (bdf) {
        uint64_t bus, dev, fn;
        if (cmd_parse_uint(bdf, &bus)) {
            const char *p = bdf;
            while (*p && *p != ':' && *p != ' ') p++;
            if (*p == ':' && cmd_parse_uint(p + 1, &dev)) {
                while (*p && *p != '.' && *p != ' ') p++;
                if (*p == '.' && cmd_parse_uint(p + 1, &fn)) {
                    dump_one(ctx, (uint8_t)bus, (uint8_t)dev, (uint8_t)fn, 1);
                    return;
                }
            }
        }
        PRINT("usage: irqroute BDF=b:d.f");
        return;
    }

    IrqIchDiag d;
    if (IRQ_DiagIch(&d) && d.ich) {
        char line[CMD_MAX_LINE], num[24];
        cmd_scopy(line, "ICH: rcba=", CMD_MAX_LINE);
        cmd_scat(line, d.rcba ? "yes" : "no", CMD_MAX_LINE);
        cmd_scat(line, " oic=", CMD_MAX_LINE);
        cmd_scat(line, (d.oic & 1) ? "ioxapic-en" : "0", CMD_MAX_LINE);
        cmd_scat(line, " pirq:", CMD_MAX_LINE);
        for (int i = 0; i < 8; i++) {
            cmd_scat(line, " ", CMD_MAX_LINE);
            cmd_uint_to_dec(d.pirq[i] & 0x7F, num, sizeof(num));
            if (d.pirq[i] & 0x80) cmd_scat(line, "-", CMD_MAX_LINE); /* disabled */
            cmd_scat(line, num, CMD_MAX_LINE);
        }
        PRINT(line);
    }

    for (int bus = 0; bus < 256; bus++)
        for (int dev = 0; dev < 32; dev++)
            for (int fn = 0; fn < 8; fn++) {
                uint32_t id = IRQ_PciRead32((uint8_t)bus, (uint8_t)dev, (uint8_t)fn, 0);
                if ((id & 0xFFFF) == 0xFFFF || id == 0) { if (!fn) break; continue; }
                dump_one(ctx, (uint8_t)bus, (uint8_t)dev, (uint8_t)fn, 0);
                if (fn == 0) {
                    uint8_t ht = IRQ_PciRead8((uint8_t)bus, (uint8_t)dev, 0, 0x0E);
                    if (!(ht & 0x80)) break;
                }
            }
}
