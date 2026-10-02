/* cmd_poke.c — C:poke — physical/MMIO write (UAOS-205)
 *
 *   poke <addr> <value> [W=8|16|32|64] [FORCE]
 *
 * Without FORCE, the write happens but the old value is only read back
 * and reported — the write still goes through (there is no safe "check"
 * for a poke; FORCE exists purely as a conscious-acknowledgement flag so
 * nobody writes to MMIO by fat-fingering a peek line).
 */

#include "cmd_internal.h"

static inline uint8_t  rd8 (uint64_t a) { return *(volatile uint8_t  *)(uintptr_t)a; }
static inline uint16_t rd16(uint64_t a) { return *(volatile uint16_t *)(uintptr_t)a; }
static inline uint32_t rd32(uint64_t a) { return *(volatile uint32_t *)(uintptr_t)a; }
static inline uint64_t rd64(uint64_t a) { return *(volatile uint64_t *)(uintptr_t)a; }
static inline void wr8 (uint64_t a, uint8_t  v) { *(volatile uint8_t  *)(uintptr_t)a = v; }
static inline void wr16(uint64_t a, uint16_t v) { *(volatile uint16_t *)(uintptr_t)a = v; }
static inline void wr32(uint64_t a, uint32_t v) { *(volatile uint32_t *)(uintptr_t)a = v; }
static inline void wr64(uint64_t a, uint64_t v) { *(volatile uint64_t *)(uintptr_t)a = v; }

static void hexn(char *line, uint64_t v, int digits)
{
    char h[17];
    for (int i = digits - 1; i >= 0; i--) { h[i] = "0123456789abcdef"[v & 15]; v >>= 4; }
    h[digits] = 0;
    cmd_scat(line, h, CMD_MAX_LINE);
}

void Cmd_Poke(NativeCmdCtx *ctx, const char *args)
{
    char line[CMD_MAX_LINE];
    uint64_t addr = 0, val = 0;
    uint32_t w = 32;

    if (!args || !cmd_parse_uint(args, &addr)) {
        PRINT("usage: poke <addr> <value> [W=8|16|32|64] [FORCE]");
        return;
    }
    const char *p = args;
    while (*p && *p != ' ') p++;
    while (*p == ' ') p++;
    if (!cmd_parse_uint(p, &val)) {
        PRINT("usage: poke <addr> <value> [W=8|16|32|64] [FORCE]");
        return;
    }
    if (!addr) { PRINT("poke: refusing address 0"); return; }
    if (!cmd_kw_find(args, "force")) {
        PRINT("poke: add FORCE to confirm an MMIO write");
        return;
    }

    const char *pw;
    if ((pw = cmd_kv_find(args, "w"))) { uint64_t wv; if (cmd_parse_uint(pw, &wv)) w = (uint32_t)wv; }
    if (w != 8 && w != 16 && w != 32 && w != 64) w = 32;

    uint64_t before;
    switch (w) {
    case 8:  before = rd8 (addr); break;
    case 16: before = rd16(addr); break;
    case 64: before = rd64(addr); break;
    default: before = rd32(addr); break;
    }

    switch (w) {
    case 8:  wr8 (addr, (uint8_t)val);  break;
    case 16: wr16(addr, (uint16_t)val); break;
    case 64: wr64(addr, val);           break;
    default: wr32(addr, (uint32_t)val); break;
    }

    uint64_t after;
    switch (w) {
    case 8:  after = rd8 (addr); break;
    case 16: after = rd16(addr); break;
    case 64: after = rd64(addr); break;
    default: after = rd32(addr); break;
    }

    cmd_scopy(line, "poke ", CMD_MAX_LINE);
    hexn(line, addr, 12);
    cmd_scat(line, ": ", CMD_MAX_LINE);
    hexn(line, before, (int)w / 4);
    cmd_scat(line, " -> ", CMD_MAX_LINE);
    hexn(line, val, (int)w / 4);
    cmd_scat(line, "  readback ", CMD_MAX_LINE);
    hexn(line, after, (int)w / 4);
    if (after != (val & ((w == 64) ? ~0ULL : ((1ULL << w) - 1))))
        cmd_scat(line, "  (differs — read-only or side-effect reg)", CMD_MAX_LINE);
    PRINT(line);
}
