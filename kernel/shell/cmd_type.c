/* cmd_type.c — C:type — print file contents to the shell */

#include "cmd_internal.h"

static void type_hex(NativeCmdCtx *ctx, VfsFile *fh)
{
    uint8_t buf[16];
    uint32_t pos = 0;
    uint32_t sz = VFS_Size(fh);
    while (pos < sz) {
        int n = (int)VFS_Read(fh, buf, (sz - pos < 16) ? (sz - pos) : 16);
        if (n <= 0) break;
        char line[CMD_MAX_LINE];
        char *p = line;
        /* Address */
        p += cmd_slen(p);
        cmd_uint_to_dec(pos, p, 12);
        p = line + cmd_slen(line);
        cmd_scat(line, ": ", CMD_MAX_LINE);
        p = line + cmd_slen(line);
        /* Hex bytes */
        for (int i = 0; i < n; i++) {
            char hex[4];
            hex[0] = "0123456789ABCDEF"[buf[i] >> 4];
            hex[1] = "0123456789ABCDEF"[buf[i] & 0xF];
            hex[2] = ' ';
            hex[3] = '\0';
            cmd_scat(line, hex, CMD_MAX_LINE);
        }
        /* Pad short lines */
        for (int i = n; i < 16; i++) {
            cmd_scat(line, "   ", CMD_MAX_LINE);
        }
        cmd_scat(line, " ", CMD_MAX_LINE);
        /* ASCII */
        for (int i = 0; i < n; i++) {
            char c = (buf[i] >= 32 && buf[i] < 127) ? (char)buf[i] : '.';
            int li = cmd_slen(line);
            if (li < CMD_MAX_LINE - 1) {
                line[li] = c;
                line[li + 1] = '\0';
            }
        }
        PRINT(line);
        pos += (uint32_t)n;
    }
}

static void type_text(NativeCmdCtx *ctx, VfsFile *fh, int numbers, VfsFile *outfh)
{
    uint8_t buf[CMD_MAX_LINE];
    uint8_t chunk[256];
    uint32_t pos = 0;
    uint32_t sz = VFS_Size(fh);
    int line_no = 0;
    int chunk_pos = 0, chunk_end = 0;
    while (pos < sz) {
        int col = 0;
        while (pos < sz && col < CMD_MAX_LINE - 1) {
            uint8_t c;
            if (chunk_pos >= chunk_end) {
                uint32_t remain = sz - pos;
                int to_read = (remain < 256) ? (int)remain : 256;
                chunk_end = (int)VFS_Read(fh, chunk, to_read);
                chunk_pos = 0;
                if (chunk_end <= 0) break;
            }
            c = chunk[chunk_pos++];
            pos++;
            if (c == '\n') break;
            if (c != '\r') buf[col++] = c;
        }
        buf[col] = '\0';
        line_no++;
        char line[CMD_MAX_LINE];
        line[0] = '\0';
        if (numbers) {
            char lnum[8];
            cmd_uint_to_dec((uint32_t)line_no, lnum, 8);
            cmd_scat(line, lnum, CMD_MAX_LINE);
            cmd_scat(line, ": ", CMD_MAX_LINE);
        }
        cmd_scat(line, (char *)buf, CMD_MAX_LINE);
        if (outfh && outfh->node) {
            VFS_Write(outfh, (const uint8_t *)line, (uint32_t)cmd_slen(line));
            VFS_Write(outfh, (const uint8_t *)"\n", 1);
        } else {
            PRINT(line);
        }
    }
}

void Cmd_Type(NativeCmdCtx *ctx, const char *args)
{
    (void)args;
    int hex = 0;
    int numbers = 0;
    const char *to_file = NULL;
    const char *file_arg = NULL;

    if (ctx->template) {
        hex = CmdTemplate_GetSwitch(ctx->template, "HEX");
        numbers = CmdTemplate_GetSwitch(ctx->template, "NUMBER");
        to_file = CmdTemplate_GetString(ctx->template, "TO");
        file_arg = CmdTemplate_GetString(ctx->template, "FILE");
    }

    if (!file_arg && !ctx->pipe_file) {
        PRINT("Usage: type <file> [HEX] [NUMBER] [TO <file>]");
        return;
    }

    char path[CMD_MAX_PATH];
    if (file_arg) {
        cmd_make_abs(ctx->cwd, file_arg, path, CMD_MAX_PATH);
    } else {
        cmd_scopy(path, ctx->pipe_file, CMD_MAX_PATH);
    }

    VfsFile fh;
    if (!VFS_Open(&fh, path, VFS_READ)) {
        char msg[CMD_MAX_LINE];
        cmd_scopy(msg, "Cannot open: ", CMD_MAX_LINE);
        cmd_scat(msg, path, CMD_MAX_LINE);
        PRINT(msg);
        return;
    }

    VfsFile outfh;
    int has_out = 0;
    if (to_file) {
        char abs_to[CMD_MAX_PATH];
        cmd_make_abs(ctx->cwd, to_file, abs_to, CMD_MAX_PATH);
        if (VFS_Open(&outfh, abs_to, VFS_WRITE | VFS_CREATE)) {
            has_out = 1;
        } else {
            char msg[CMD_MAX_LINE];
            cmd_scopy(msg, "Cannot create: ", CMD_MAX_LINE);
            cmd_scat(msg, abs_to, CMD_MAX_LINE);
            PRINT(msg);
        }
    }

    if (hex) {
        type_hex(ctx, &fh);
    } else {
        type_text(ctx, &fh, numbers, has_out ? &outfh : NULL);
    }

    VFS_Close(&fh);
    if (has_out) VFS_Close(&outfh);
}
