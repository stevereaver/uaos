/* cmd_peek.c — C:peek — physical/kernel-address read (UAOS-205)
 *
 *   peek <addr> [LEN=n] [W=8|16|32|64]
 *
 * addr: hex (0x...) or decimal — physical == virtual in our identity
 * map, so this reads RAM, MMIO BARs, PCI config shadows, anything mapped.
 * W selects access width for MMIO reads that latch/clear on size.
 * LEN dumps successive words of the chosen width (max 16).
 *
 * NO address validation beyond a NULL check — this is a debugging
 * scalpel.  Reading a bad MMIO window can fault or wedge the bus; that
 * is the point of the tool, use it deliberately.
 */

#include "cmd_internal.h"

static inline uint8_t  rd8 (uint64_t a) { return *(volatile uint8_t  *)(uintptr_t)a; }
static inline uint16_t rd16(uint64_t a) { return *(volatile uint16_t *)(uintptr_t)a; }
static inline uint32_t rd32(uint64_t a) { return *(volatile uint32_t *)(uintptr_t)a; }
static inline uint64_t rd64(uint64_t a) { return *(volatile uint64_t *)(uintptr_t)a; }

static void hexn(char *line, uint64_t v, int digits)
{
    char h[17];
    for (int i = digits - 1; i >= 0; i--) { h[i] = "0123456789abcdef"[v & 15]; v >>= 4; }
    h[digits] = 0;
    cmd_scat(line, h, CMD_MAX_LINE);
}

void Cmd_Peek(NativeCmdCtx *ctx, const char *args)
{
    char line[CMD_MAX_LINE];
    uint64_t addr = 0, v;
    uint32_t len = 1, w = 32;

    if (!args || !cmd_parse_uint(args, &addr)) {
        PRINT("usage: peek <addr> [LEN=n] [W=8|16|32|64]");
        return;
    }
    if (!addr) { PRINT("peek: refusing address 0"); return; }

    const char *p;
    if ((p = cmd_kv_find(args, "len")) && cmd_parse_uint(p, &v)) len = (uint32_t)v;
    if ((p = cmd_kv_find(args, "w"))   && cmd_parse_uint(p, &v)) w   = (uint32_t)v;
    if (len > 16) len = 16;
    if (w != 8 && w != 16 && w != 32 && w != 64) w = 32;

    int step = (int)w / 8;
    for (uint32_t i = 0; i < len; i++) {
        uint64_t a = addr + (uint64_t)i * (uint64_t)step;
        cmd_scopy(line, "  ", CMD_MAX_LINE);
        hexn(line, a, 12);
        cmd_scat(line, ": ", CMD_MAX_LINE);
        switch (w) {
        case 8:  hexn(line, rd8 (a), 2);  break;
        case 16: hexn(line, rd16(a), 4);  break;
        case 64: hexn(line, rd64(a), 16); break;
        default: hexn(line, rd32(a), 8);  break;
        }
        PRINT(line);
    }
}
