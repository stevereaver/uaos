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

    PRINT("  bdf      cls           vendor:dev     pin line  bars");
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

                /* BARs (type 0 header only, offsets 0x10-0x27) */
                for (int bi = 0; bi < 6; bi++) {
                    uint32_t bar = IRQ_PciRead32((uint8_t)bus, (uint8_t)dev,
                                                 (uint8_t)fn, (uint8_t)(0x10 + bi * 4));
                    if (!bar) continue;
                    cmd_scat(line, " ", CMD_MAX_LINE);
                    hexn(line, bar, 8);
                }
                PRINT(line);

                /* skip functions if not multi-function */
                if (fn == 0) {
                    uint8_t ht = IRQ_PciRead8((uint8_t)bus, (uint8_t)dev, 0, 0x0E);
                    if (!(ht & 0x80)) break;
                }
            }
        }
    }
}
