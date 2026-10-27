/* cmd_ramdump.c — C:ramdump — dump chipset state + guest chip-RAM (UAOS-366)
 *
 *   ramdump [FILE=path] [LEN=n]
 *
 * Writes a small text register header followed by LEN bytes of the
 * display RAM window (default 1 MiB — covers the takeover demo's working
 * set).  The payload is the guest chip-RAM space seen by the Amiga
 * display, so offline tools can decode the copper list and bitplanes.
 */

#include "cmd_internal.h"
#include "../chipset/chip_emu.h"

void Cmd_Ramdump(NativeCmdCtx *ctx, const char *args)
{
    char line[CMD_MAX_LINE];
    char path[CMD_MAX_PATH];
    const char *p;
    uint64_t v = 0;
    uint32_t len = 1u << 20;

    uint32_t start = 0;
    if (args && (p = cmd_kv_find(args, "start")) && cmd_parse_uint(p, &v))
        start = (uint32_t)v;
    if (args && (p = cmd_kv_find(args, "len")) && cmd_parse_uint(p, &v))
        len = (uint32_t)v;
    if (len == 0) len = 1;
    if (start + len > chip_emu_guest_ram_size())
        len = chip_emu_guest_ram_size() - start;

    if (args && (p = cmd_kv_find(args, "file"))) {
        int i = 0;
        while (p[i] && p[i] != ' ' && i < CMD_MAX_PATH - 1) { path[i] = p[i]; i++; }
        path[i] = '\0';
    } else {
        cmd_scopy(path, "RAM:ramdump.bin", sizeof(path));
    }
    cmd_make_abs(ctx->cwd, path, path, sizeof(path));

    VfsFile fh;
    if (!VFS_Open(&fh, path, VFS_WRITE | VFS_CREATE | VFS_TRUNC)) {
        PRINT("ramdump: cannot open destination");
        return;
    }

    char hdr[640];
    chip_emu_dump_report(hdr, sizeof(hdr));
    uint32_t hl = 0;
    while (hdr[hl] && hl < sizeof(hdr) - 2) hl++;
    hdr[hl++] = '\n';
    VFS_Write(&fh, (const uint8_t *)hdr, hl);

    const uint8_t *ram = chip_emu_display_ram();
    uint32_t off = start;
    while (off < len) {
        uint32_t chunk = (len - off > 65536u) ? 65536u : (len - off);
        uint32_t w = VFS_Write(&fh, ram + off, chunk);
        if (w != chunk) { PRINT("ramdump: short write"); break; }
        off += w;
        if (ctx->yield_ms) ctx->yield_ms(ctx->shell_extra, 0);
    }
    VFS_Close(&fh);

    cmd_scopy(line, "ramdump: ", sizeof(line));
    cmd_scat(line, path, sizeof(line));
    cmd_scat(line, " ", sizeof(line));
    {
        char num[16];
        cmd_uint_to_dec(off, num, sizeof(num));
        cmd_scat(line, num, sizeof(line));
    }
    cmd_scat(line, " bytes", sizeof(line));
    PRINT(line);
}
