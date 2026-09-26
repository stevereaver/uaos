/* cmd_fsck.c — C:fsck — disk/filesystem interrogation & repair tool
 *
 * AmigaDOS template:
 *   fsck DEVICE,LIST/S,INFO/S,CHECK/S,REPAIR/S,FIX/S,INTERACTIVE/S,
 *        VERBOSE/S,SURFACE/S,DUMP/K/N,ALL/S
 *
 *   fsck DH0:                check (read-only report)
 *   fsck DH0: REPAIR         check + fix automatically
 *   fsck DH0: INTERACTIVE    prompt before each fix
 *   fsck DH0: INFO           filesystem geometry/metadata dump
 *   fsck DH0: SURFACE        media surface scan (read test)
 *   fsck DH0: DUMP=0         hex dump of one sector
 *   fsck virtio0             whole disk: partition table analysis
 *   fsck LIST                list block devices
 *   fsck ALL [REPAIR]        check every detected filesystem volume
 */

#include <string.h>
#include "cmd_internal.h"
#include "../dos/fsck.h"

/* -------------------------------------------------------------------------
 * NativeCmdCtx -> FsckCtx bridges
 * ------------------------------------------------------------------------- */
static void fk_print(void *ud, const char *line)
{
    NativeCmdCtx *c = (NativeCmdCtx *)ud;
    c->print(c->shell, line);
}

static int fk_confirm(void *ud, const char *msg)
{
    return cmd_prompt_yn((NativeCmdCtx *)ud, msg);
}

static int fk_brk(void *ud)
{
    NativeCmdCtx *c = (NativeCmdCtx *)ud;
    return c->break_pending ? c->break_pending(c->shell_extra) : 0;
}

static void fk_yield(void *ud)
{
    NativeCmdCtx *c = (NativeCmdCtx *)ud;
    CMD_YIELD(c, 5);
}

/* -------------------------------------------------------------------------
 * Device resolution — display_name ("DH0:") first, then raw name.
 * ------------------------------------------------------------------------- */
static BlockDev *find_dev(const char *name)
{
    BlockDev *d = BlockDev_GetList();
    while (d) {
        if (d->display_name && cmd_seq_ci(d->display_name, name)) return d;
        d = d->next;
    }
    d = BlockDev_GetList();
    while (d) {
        if (d->name && cmd_seq_ci(d->name, name)) return d;
        d = d->next;
    }
    return NULL;
}

/* Returns 1 when the device appears to be mounted (its unit name resolves
 * to a live packet handler). */
static int dev_mounted(BlockDev *d)
{
    if (!d->display_name) return 0;
    char vol[16];
    int i = 0;
    while (i < 15 && d->display_name[i] && d->display_name[i] != ':') {
        vol[i] = d->display_name[i]; i++;
    }
    vol[i] = 0;
    return vol[0] && VFS_GetHandlerPort(vol) != NULL;
}

static void run_one(BlockDev *dev, NativeCmdCtx *ctx, int mode,
                    int verbose, int surface)
{
    FsckCtx fk;
    memset(&fk, 0, sizeof(fk));
    fk.dev     = dev;
    fk.print   = fk_print;
    fk.ud      = ctx;
    fk.mode    = mode;
    fk.verbose = verbose;
    fk.confirm = fk_confirm;
    fk.brk     = fk_brk;
    fk.yield   = fk_yield;

    char msg[CMD_MAX_LINE];
    cmd_scopy(msg, "== ", CMD_MAX_LINE);
    cmd_scat(msg, dev->display_name ? dev->display_name : dev->name, CMD_MAX_LINE);
    cmd_scat(msg, " (", CMD_MAX_LINE);
    cmd_scat(msg, dev->name, CMD_MAX_LINE);
    cmd_scat(msg, ") ==", CMD_MAX_LINE);
    PRINT(msg);

    if (mode != FSCK_MODE_CHECK && dev_mounted(dev)) {
        PRINT("WARN:  volume appears to be mounted — repairs may confuse");
        PRINT("       the live filesystem handler. Unmount first if possible.");
    }

    int rc;
    if (dev->part_offset == 0)
        rc = FSCK_DiskCheck(dev, &fk);
    else
        rc = FSCK_RunCheck(dev, &fk);

    if (surface && rc >= 0)
        FSCK_SurfaceScan(dev, &fk);

    /* ---- summary ---- */
    msg[0] = 0;
    cmd_scat(msg, "fsck: ", CMD_MAX_LINE);
    cmd_uint_to_dec(fk.errors, msg + cmd_slen(msg),
                    CMD_MAX_LINE - cmd_slen(msg));
    cmd_scat(msg, " errors, ", CMD_MAX_LINE);
    cmd_uint_to_dec(fk.warnings, msg + cmd_slen(msg),
                    CMD_MAX_LINE - cmd_slen(msg));
    cmd_scat(msg, " warnings, ", CMD_MAX_LINE);
    cmd_uint_to_dec(fk.fixed, msg + cmd_slen(msg),
                    CMD_MAX_LINE - cmd_slen(msg));
    cmd_scat(msg, " fixed", CMD_MAX_LINE);
    PRINT(msg);

    if (!ctx->set_rc) return;
    if (fk.aborted || rc < 0)      ctx->set_rc(ctx->shell_extra, 20);
    else if (fk.errors)            ctx->set_rc(ctx->shell_extra, 10);
    else if (fk.warnings)          ctx->set_rc(ctx->shell_extra, 5);
    else                           ctx->set_rc(ctx->shell_extra, 0);
}

void Cmd_Fsck(NativeCmdCtx *ctx, const char *args)
{
    CmdTemplateResult *t = ctx->template;
    (void)args;
    if (!t) { PRINT("fsck: internal error (no template)"); return; }

    int list      = CmdTemplate_GetSwitch(t, "LIST");
    int info      = CmdTemplate_GetSwitch(t, "INFO");
    int repair    = CmdTemplate_GetSwitch(t, "REPAIR") ||
                    CmdTemplate_GetSwitch(t, "FIX");
    int inter     = CmdTemplate_GetSwitch(t, "INTERACTIVE");
    int verbose   = CmdTemplate_GetSwitch(t, "VERBOSE");
    int surface   = CmdTemplate_GetSwitch(t, "SURFACE");
    int all       = CmdTemplate_GetSwitch(t, "ALL");
    const char *devname = CmdTemplate_GetString(t, "DEVICE");
    int dump_sec = -1;
    CmdTemplate_GetInt(t, "DUMP", &dump_sec);

    if (!devname && !list && !all) {
        PRINT("Usage: fsck DEVICE|LIST|ALL [switches]");
        PRINT("  DEVICE        e.g. DH0:, WB:, virtio01, virtio0 (whole disk)");
        PRINT("  LIST          list block devices");
        PRINT("  ALL           check every block device");
        PRINT("  INFO          dump filesystem metadata");
        PRINT("  CHECK         read-only check (default)");
        PRINT("  REPAIR/FIX    fix problems automatically");
        PRINT("  INTERACTIVE   prompt before each fix");
        PRINT("  SURFACE       media surface scan (read test)");
        PRINT("  DUMP=<sec>    hex dump of a sector");
        PRINT("  VERBOSE       detailed output");
        PRINT("");
        PRINT("Examples:");
        PRINT("  fsck DH0:                report only");
        PRINT("  fsck WB: REPAIR          check and fix");
        PRINT("  fsck virtio0             partition table analysis");
        PRINT("  fsck DH0: INFO SURFACE");
        if (ctx->set_rc) ctx->set_rc(ctx->shell_extra, 10);
        return;
    }

    /* ---- LIST ---- */
    if (list) {
        PRINT("Block devices:");
        int n = 0;
        for (BlockDev *d = BlockDev_GetList(); d; d = d->next) {
            char msg[CMD_MAX_LINE];
            cmd_scopy(msg, "  ", CMD_MAX_LINE);
            cmd_scat(msg, d->name, CMD_MAX_LINE);
            if (d->display_name) {
                cmd_scat(msg, " (", CMD_MAX_LINE);
                cmd_scat(msg, d->display_name, CMD_MAX_LINE);
                cmd_scat(msg, ")", CMD_MAX_LINE);
            }
            cmd_scat(msg, "  ", CMD_MAX_LINE);
            cmd_uint_to_dec((uint32_t)(d->num_sectors >> 11),
                            msg + cmd_slen(msg),
                            CMD_MAX_LINE - cmd_slen(msg));
            cmd_scat(msg, " MiB  ", CMD_MAX_LINE);
            cmd_scat(msg, FSCK_ProbeName(d), CMD_MAX_LINE);
            if (d->part_offset)
                cmd_scat(msg, "  partition", CMD_MAX_LINE);
            else
                cmd_scat(msg, "  disk", CMD_MAX_LINE);
            PRINT(msg);
            n++;
        }
        if (!n) PRINT("  (none)");
        return;
    }

    /* ---- device ops ---- */
    if (devname) {
        BlockDev *dev = find_dev(devname);
        if (!dev) {
            char msg[CMD_MAX_LINE];
            cmd_scopy(msg, "fsck: device not found: ", CMD_MAX_LINE);
            cmd_scat(msg, devname, CMD_MAX_LINE);
            PRINT(msg);
            if (ctx->set_rc) ctx->set_rc(ctx->shell_extra, 20);
            return;
        }

        if (dump_sec >= 0) {
            FsckCtx fk;
            memset(&fk, 0, sizeof(fk));
            fk.dev = dev; fk.print = fk_print; fk.ud = ctx;
            FSCK_DumpSector(dev, (uint64_t)dump_sec, &fk);
            return;
        }

        if (info) {
            FsckCtx fk;
            memset(&fk, 0, sizeof(fk));
            fk.dev = dev; fk.print = fk_print; fk.ud = ctx;
            fk.verbose = verbose;
            char msg[CMD_MAX_LINE];
            cmd_scopy(msg, "== ", CMD_MAX_LINE);
            cmd_scat(msg, dev->display_name ? dev->display_name : dev->name,
                     CMD_MAX_LINE);
            cmd_scat(msg, " ==", CMD_MAX_LINE);
            PRINT(msg);
            if (dev->part_offset == 0) {
                /* whole disk: show partition table then FS probe */
                if (FSCK_DiskCheck(dev, &fk) != 0 && fk.errors)
                    { /* errors already reported */ }
            } else {
                FSCK_RunInfo(dev, &fk);
            }
            if (surface)
                FSCK_SurfaceScan(dev, &fk);
            return;
        }

        int mode = inter ? FSCK_MODE_INTERACTIVE
                 : repair ? FSCK_MODE_REPAIR : FSCK_MODE_CHECK;
        run_one(dev, ctx, mode, verbose, surface);
        return;
    }

    /* ---- ALL ---- */
    if (all) {
        int mode = inter ? FSCK_MODE_INTERACTIVE
                 : repair ? FSCK_MODE_REPAIR : FSCK_MODE_CHECK;
        int any = 0;
        for (BlockDev *d = BlockDev_GetList(); d; d = d->next) {
            if (d->part_offset == 0) continue;   /* partitions only */
            PRINT("");
            run_one(d, ctx, mode, verbose, surface);
            any++;
            if (ctx->break_pending && ctx->break_pending(ctx->shell_extra)) {
                PRINT("^C — stopped");
                break;
            }
        }
        if (!any) PRINT("fsck: no partitions to check");
        return;
    }
}
