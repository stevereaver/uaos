/* cmd_pciscan.c — C:pciscan — generic PCI bus enumeration (UAOS-199)
 *
 *   pciscan          every responding PCI function on bus 0-255:
 *                    bdf, class:subclass:progif, vendor:device, irq
 *                    pin/line, BARs
 *   pciscan BUS=n    restrict to bus n
 */

#include "cmd_internal.h"
#include "../irq/irq.h"

static const char *class_name(uint8_t cls)
{
    switch (cls) {
    case 0x00: return "legacy";
    case 0x01: return "storage";
    case 0x02: return "network";
    case 0x03: return "display";
    case 0x04: return "multimedia";
    case 0x05: return "memory";
    case 0x06: return "bridge";
    case 0x07: return "serial";
    case 0x08: return "generic";
    case 0x09: return "input";
    case 0x0A: return "dock";
    case 0x0B: return "cpu";
    case 0x0C: return "serialbus";
    case 0x0D: return "wireless";
    case 0x0E: return "i2o";
    case 0x0F: return "satellite";
    case 0x10: return "crypto";
    case 0x12: return "misc";
    default:   return "?";
    }
}

static void hexn(char *line, uint64_t v, int digits)
{
    char h[17]; int i;
    for (i = digits - 1; i >= 0; i--) { h[i] = "0123456789abcdef"[v & 15]; v >>= 4; }
    h[digits] = 0;
    cmd_scat(line, h, CMD_MAX_LINE);
}

void Cmd_Pciscan(NativeCmdCtx *ctx, const char *args)
{
    char line[CMD_MAX_LINE], num[24];
    int only_bus = -1;

    const char *b = cmd_kv_find(args, "bus");
    if (b) { uint64_t v; if (cmd_parse_uint(b, &v)) only_bus = (int)v; }

    PRINT("  bdf      cls           vendor:dev     pin line  bars / bridge");
    for (int bus = 0; bus < 256; bus++) {
        if (only_bus >= 0 && bus != only_bus) continue;
        for (int dev = 0; dev < 32; dev++) {
            for (int fn = 0; fn < 8; fn++) {
                uint32_t id = IRQ_PciRead32((uint8_t)bus, (uint8_t)dev, (uint8_t)fn, 0);
                if ((id & 0xFFFF) == 0xFFFF || id == 0) { if (!fn) break; continue; }

                uint32_t cls = IRQ_PciRead32((uint8_t)bus, (uint8_t)dev, (uint8_t)fn, 0x08);
                uint8_t pin  = IRQ_PciRead8((uint8_t)bus, (uint8_t)dev, (uint8_t)fn, 0x3D);
                uint8_t iline= IRQ_PciRead8((uint8_t)bus, (uint8_t)dev, (uint8_t)fn, 0x3C);

                cmd_scopy(line, "  ", CMD_MAX_LINE);
                cmd_uint_to_dec(bus, num, sizeof(num)); cmd_scat(line, num, CMD_MAX_LINE);
                cmd_scat(line, ":", CMD_MAX_LINE);
                cmd_uint_to_dec(dev, num, sizeof(num)); cmd_scat(line, num, CMD_MAX_LINE);
                cmd_scat(line, ".", CMD_MAX_LINE);
                cmd_uint_to_dec(fn, num, sizeof(num)); cmd_scat(line, num, CMD_MAX_LINE);
                int sl = cmd_slen(line); while (sl++ < 9) cmd_scat(line, " ", CMD_MAX_LINE);

                cmd_scat(line, class_name((uint8_t)(cls >> 24)), CMD_MAX_LINE);
                sl = cmd_slen(line); while (sl++ < 21) cmd_scat(line, " ", CMD_MAX_LINE);

                hexn(line, id & 0xFFFF, 4);
                cmd_scat(line, ":", CMD_MAX_LINE);
                hexn(line, id >> 16, 4);
                sl = cmd_slen(line); while (sl++ < 31) cmd_scat(line, " ", CMD_MAX_LINE);

                cmd_uint_to_dec(pin, num, sizeof(num)); cmd_scat(line, num, CMD_MAX_LINE);
                sl = cmd_slen(line); while (sl++ < 36) cmd_scat(line, " ", CMD_MAX_LINE);
                cmd_uint_to_dec(iline, num, sizeof(num)); cmd_scat(line, num, CMD_MAX_LINE);
                sl = cmd_slen(line); while (sl++ < 41) cmd_scat(line, " ", CMD_MAX_LINE);

                /* BAR count and layout depend on the header type:
                 * type 0 = endpoint (6 BARs), type 1 = PCI-PCI bridge
                 * (2 BARs + bus/window regs), type 2 = CardBus (1 BAR). */
                uint8_t htr = IRQ_PciRead8((uint8_t)bus, (uint8_t)dev,
                                           (uint8_t)fn, 0x0E);
                uint8_t ht = htr & 0x7F;
                int nbar = (ht == 0) ? 6 : (ht == 1) ? 2 : (ht == 2) ? 1 : 0;
                for (int bi = 0; bi < nbar; bi++) {
                    uint32_t bar = IRQ_PciRead32((uint8_t)bus, (uint8_t)dev,
                                                 (uint8_t)fn, (uint8_t)(0x10 + bi * 4));
                    if (!bar) continue;
                    cmd_scat(line, " ", CMD_MAX_LINE);
                    /* 64-bit memory BAR: the next dword is the high half */
                    if (!(bar & 1) && (bar & 6) == 4 && bi + 1 < nbar) {
                        uint32_t hi = IRQ_PciRead32((uint8_t)bus, (uint8_t)dev,
                                                    (uint8_t)fn, (uint8_t)(0x10 + (bi + 1) * 4));
                        hexn(line, ((uint64_t)hi << 32) | (bar & ~0xFULL), 16);
                        bi++;
                    } else {
                        hexn(line, bar, 8);
                    }
                }

                if (ht == 1) {
                    /* Type-1 header: 0x18/0x1C/0x20/0x24 are secondary/
                     * subordinate bus numbers and the IO/mem/prefetch
                     * forwarding windows — not BARs.  Decode them on a
                     * continuation line. */
                    uint32_t bn = IRQ_PciRead32((uint8_t)bus, (uint8_t)dev,
                                                (uint8_t)fn, 0x18);
                    cmd_scat(line, " sec=", CMD_MAX_LINE);
                    cmd_uint_to_dec((bn >> 8) & 0xFF, num, sizeof(num));
                    cmd_scat(line, num, CMD_MAX_LINE);
                    cmd_scat(line, " sub=", CMD_MAX_LINE);
                    cmd_uint_to_dec((bn >> 16) & 0xFF, num, sizeof(num));
                    cmd_scat(line, num, CMD_MAX_LINE);
                    PRINT(line);

                    cmd_scopy(line, "    ", CMD_MAX_LINE);
                    uint32_t io = IRQ_PciRead32((uint8_t)bus, (uint8_t)dev,
                                                (uint8_t)fn, 0x1C);
                    cmd_scat(line, "io=", CMD_MAX_LINE);
                    if (io & 0xF0F0) {   /* any base/limit nibble set */
                        hexn(line, (io & 0xF0) << 8, 4);
                        cmd_scat(line, "-", CMD_MAX_LINE);
                        hexn(line, (((io >> 8) & 0xF0) << 8) | 0xFFF, 4);
                    } else cmd_scat(line, "-", CMD_MAX_LINE);
                    uint32_t mw = IRQ_PciRead32((uint8_t)bus, (uint8_t)dev,
                                                (uint8_t)fn, 0x20);
                    cmd_scat(line, " mem=", CMD_MAX_LINE);
                    if (mw & 0xFFF0FFF0) {
                        hexn(line, (mw & 0xFFF0) << 16, 8);
                        cmd_scat(line, "-", CMD_MAX_LINE);
                        hexn(line, (((mw >> 16) & 0xFFF0) << 16) | 0xFFFFF, 8);
                    } else cmd_scat(line, "-", CMD_MAX_LINE);
                    uint32_t pw = IRQ_PciRead32((uint8_t)bus, (uint8_t)dev,
                                                (uint8_t)fn, 0x24);
                    cmd_scat(line, " pmem=", CMD_MAX_LINE);
                    if (pw & 0xFFF0FFF0) {
                        hexn(line, (pw & 0xFFF0) << 16, 8);
                        cmd_scat(line, "-", CMD_MAX_LINE);
                        hexn(line, (((pw >> 16) & 0xFFF0) << 16) | 0xFFFFF, 8);
                    } else cmd_scat(line, "-", CMD_MAX_LINE);
                    PRINT(line);
                } else {
                    if (ht > 2) {
                        cmd_scat(line, " hdr=0x", CMD_MAX_LINE);
                        hexn(line, ht, 2);
                    }
                    PRINT(line);
                }

                /* skip functions if not multi-function */
                if (fn == 0 && !(htr & 0x80)) break;
            }
        }
    }
}
