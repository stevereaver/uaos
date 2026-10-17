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
#include "../irq/acpi_prt.h"

static const char *route_src(int s)
{
    switch (s) {
    case IRQ_ROUTE_ICH:     return "ich-dxxir";
    case IRQ_ROUTE_INTLINE: return "intline";
    case IRQ_ROUTE_ACPI:    return "acpi-prt";
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
    if (ri.swizzled) {
        cmd_scat(line, " via swizzle at ", CMD_MAX_LINE);
        cmd_uint_to_dec(ri.bridge_bus, num, sizeof(num)); cmd_scat(line, num, CMD_MAX_LINE);
        cmd_scat(line, ":", CMD_MAX_LINE);
        cmd_uint_to_dec(ri.bridge_dev, num, sizeof(num)); cmd_scat(line, num, CMD_MAX_LINE);
        cmd_scat(line, ".", CMD_MAX_LINE);
        cmd_uint_to_dec(ri.bridge_fn, num, sizeof(num)); cmd_scat(line, num, CMD_MAX_LINE);
    }
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

/* irqroute PRT — dump the parsed ACPI _PRT sets and, when an ICH is
 * present, cross-check the bus-0 chipset decode against ACPI for the
 * devices 25-31 that ich_route_gsi owns (they must agree). */
static void dump_prt(NativeCmdCtx *ctx)
{
    char line[CMD_MAX_LINE], num[24];
    int ns = AcpiPrt_NumSets();

    cmd_scopy(line, "acpi-prt: ", CMD_MAX_LINE);
    cmd_uint_to_dec((uint32_t)ns, num, sizeof(num));
    cmd_scat(line, num, CMD_MAX_LINE);
    cmd_scat(line, " set(s)", CMD_MAX_LINE);
    PRINT(line);

    for (int s = 0; s < ns; s++) {
        uint8_t bus, bb, bd, bf;
        int has_b;
        AcpiPrt_SetInfo(s, &bus, &has_b, &bb, &bd, &bf);
        cmd_scopy(line, "set ", CMD_MAX_LINE);
        cmd_uint_to_dec((uint32_t)s, num, sizeof(num)); cmd_scat(line, num, CMD_MAX_LINE);
        cmd_scat(line, ": routes bus ", CMD_MAX_LINE);
        cmd_uint_to_dec(bus, num, sizeof(num)); cmd_scat(line, num, CMD_MAX_LINE);
        cmd_scat(line, " (", CMD_MAX_LINE);
        if (has_b) {
            cmd_scat(line, "bridge ", CMD_MAX_LINE);
            cmd_uint_to_dec(bb, num, sizeof(num)); cmd_scat(line, num, CMD_MAX_LINE);
            cmd_scat(line, ":", CMD_MAX_LINE);
            cmd_uint_to_dec(bd, num, sizeof(num)); cmd_scat(line, num, CMD_MAX_LINE);
            cmd_scat(line, ".", CMD_MAX_LINE);
            cmd_uint_to_dec(bf, num, sizeof(num)); cmd_scat(line, num, CMD_MAX_LINE);
        } else {
            cmd_scat(line, "host bridge", CMD_MAX_LINE);
        }
        cmd_scat(line, "), ", CMD_MAX_LINE);
        cmd_uint_to_dec((uint32_t)AcpiPrt_SetNEntries(s), num, sizeof(num));
        cmd_scat(line, num, CMD_MAX_LINE);
        cmd_scat(line, " entries", CMD_MAX_LINE);
        PRINT(line);

        /* Compact per-device pin map: "  dev NN: A=16 B=17 C=18 D=19" */
        for (uint8_t dev = 0; dev < 32; dev++) {
            int gsi4[4] = { -1, -1, -1, -1 };
            int any = 0;
            for (int e = 0; e < AcpiPrt_SetNEntries(s); e++) {
                uint8_t ed, ep; int eg;
                if (AcpiPrt_SetEntry(s, e, &ed, &ep, &eg) != 0) continue;
                if (ed == dev) { gsi4[ep - 1] = eg; any = 1; }
            }
            if (!any) continue;
            cmd_scopy(line, "  dev ", CMD_MAX_LINE);
            cmd_uint_to_dec(dev, num, sizeof(num)); cmd_scat(line, num, CMD_MAX_LINE);
            cmd_scat(line, ":", CMD_MAX_LINE);
            for (int p = 0; p < 4; p++) {
                if (gsi4[p] < 0) continue;
                cmd_scat(line, " ", CMD_MAX_LINE);
                { char c[2] = { (char)('A' + p), 0 }; cmd_scat(line, c, CMD_MAX_LINE); }
                cmd_scat(line, "=", CMD_MAX_LINE);
                cmd_sdec(gsi4[p], num, sizeof(num)); cmd_scat(line, num, CMD_MAX_LINE);
            }
            PRINT(line);
        }
    }

    /* Cross-check: on an ICH machine, devices 25-31 of bus 0 route
     * through DxxIP/DxxIR — the ACPI answer must match. */
    IrqIchDiag d;
    int have_ich = (IRQ_DiagIch(&d) == 0) && d.ich;
    int bus0 = -1;
    for (int s = 0; s < ns; s++) {
        uint8_t bus, bb, bd, bf; int hb;
        AcpiPrt_SetInfo(s, &bus, &hb, &bb, &bd, &bf);
        if (bus == 0) { bus0 = s; break; }
    }
    if (!have_ich || bus0 < 0) return;

    PRINT("ich-vs-acpi, bus 0 dev 25-31:");
    for (uint8_t dev = 25; dev <= 31; dev++) {
        for (uint8_t fn = 0; fn < 8; fn++) {
            uint32_t id = IRQ_PciRead32(0, dev, fn, 0);
            if ((id & 0xFFFF) == 0xFFFF || id == 0) { if (!fn) break; continue; }
            IrqRouteInfo ri;
            IRQ_RouteInspect(0, dev, fn, &ri);
            if (ri.pin < 1 || ri.pin > 4) continue;
            int agsi = AcpiPrt_Route(0, dev, fn, ri.pin, 0);
            cmd_scopy(line, "  0:", CMD_MAX_LINE);
            cmd_uint_to_dec(dev, num, sizeof(num)); cmd_scat(line, num, CMD_MAX_LINE);
            cmd_scat(line, ".", CMD_MAX_LINE);
            cmd_uint_to_dec(fn, num, sizeof(num)); cmd_scat(line, num, CMD_MAX_LINE);
            cmd_scat(line, " pin=", CMD_MAX_LINE);
            { char c[2] = { (char)('A' + ri.pin - 1), 0 }; cmd_scat(line, c, CMD_MAX_LINE); }
            cmd_scat(line, " ich=", CMD_MAX_LINE);
            cmd_sdec(ri.gsi, num, sizeof(num)); cmd_scat(line, num, CMD_MAX_LINE);
            cmd_scat(line, " acpi=", CMD_MAX_LINE);
            cmd_sdec(agsi, num, sizeof(num)); cmd_scat(line, num, CMD_MAX_LINE);
            cmd_scat(line, (ri.gsi == agsi) ? " ok" : " MISMATCH", CMD_MAX_LINE);
            PRINT(line);
            if (!fn && !(IRQ_PciRead8(0, dev, 0, 0x0E) & 0x80)) break;
        }
    }
}

void Cmd_Irqroute(NativeCmdCtx *ctx, const char *args)
{
    if (cmd_kw_find(args, "prt")) { dump_prt(ctx); return; }

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
