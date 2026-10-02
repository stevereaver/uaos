/* cmd_diskdiag.c — C:diskdiag — storage-path stage dump (UAOS-204)
 *
 *   diskdiag           blockdev registry + every driver stage dump
 *   diskdiag TEST=dev  timed read of first sector of <dev> (e.g. DF0:)
 *
 * Same "dump every stage, then a verdict" pattern as usbdiag.
 */

#include "cmd_internal.h"
#include "../dbg/diag.h"
#include "../dos/blockdev.h"
#include "../irq/irq.h"

extern void IDE_DiagDump(void *ctx, void (*emit)(void *, const char *));
extern void AHCI_DiagDump(void *ctx, void (*emit)(void *, const char *));
extern void Vblk_DiagDump(void *ctx, void (*emit)(void *, const char *));
extern void Vscsi_DiagDump(void *ctx, void (*emit)(void *, const char *));
extern void FloppyBlk_DiagDump(void *ctx, void (*emit)(void *, const char *));

static void dd_emit(void *ctx, const char *line)
{
    CMD_PRINT((NativeCmdCtx *)ctx, line);
}

/* expose kernel ms-precision tsc for the timed read */
extern uint64_t Tickmon_TscHz(void);

static inline uint64_t rdtsc_(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

void Cmd_Diskdiag(NativeCmdCtx *ctx, const char *args)
{
    char line[CMD_MAX_LINE], num[24];

    const char *test = cmd_kv_find(args, "test");
    if (test) {
        /* TEST=<dev>: timed 1-sector read; "stuck read" detector */
        char dev[32];
        int i = 0;
        while (test[i] && test[i] != ' ' && i < 31) { dev[i] = test[i]; i++; }
        dev[i] = 0;
        BlockDev *b = BlockDev_Find(dev);
        if (!b) {
            cmd_scopy(line, "diskdiag: no blockdev '", CMD_MAX_LINE);
            cmd_scat(line, dev, CMD_MAX_LINE);
            cmd_scat(line, "'", CMD_MAX_LINE);
            PRINT(line);
            return;
        }
        static uint8_t buf[512];
        cmd_scopy(line, "diskdiag: reading sector 0 of '", CMD_MAX_LINE);
        cmd_scat(line, dev, CMD_MAX_LINE);
        cmd_scat(line, "' ...", CMD_MAX_LINE);
        PRINT(line);
        uint64_t t0 = rdtsc_();
        int rc = BlockDev_Read(b, 0, buf, 1);
        uint64_t dt = rdtsc_() - t0;
        uint64_t hz = Tickmon_TscHz();
        uint64_t ms = hz ? dt * 1000 / hz : dt;
        cmd_scopy(line, "diskdiag: read ", CMD_MAX_LINE);
        cmd_scat(line, rc == 0 ? "OK" : "FAILED", CMD_MAX_LINE);
        cmd_scat(line, " rc=", CMD_MAX_LINE);
        cmd_sdec(rc, num, sizeof(num));
        cmd_scat(line, num, CMD_MAX_LINE);
        cmd_scat(line, " in ", CMD_MAX_LINE);
        cmd_uint_to_dec((uint32_t)ms, num, sizeof(num));
        cmd_scat(line, num, CMD_MAX_LINE);
        cmd_scat(line, " ms", CMD_MAX_LINE);
        PRINT(line);
        return;
    }

    PRINT("== block devices ==");
    for (BlockDev *b = BlockDev_GetList(); b; b = b->next) {
        cmd_scopy(line, " '", CMD_MAX_LINE);
        cmd_scat(line, b->name, CMD_MAX_LINE);
        cmd_scat(line, "' \"", CMD_MAX_LINE);
        cmd_scat(line, b->display_name ? b->display_name : "", CMD_MAX_LINE);
        cmd_scat(line, "\" cap=", CMD_MAX_LINE);
        cmd_uint_to_dec((uint32_t)b->num_sectors, num, sizeof(num));
        cmd_scat(line, num, CMD_MAX_LINE);
        cmd_scat(line, " sect x ", CMD_MAX_LINE);
        cmd_uint_to_dec(b->sector_size, num, sizeof(num));
        cmd_scat(line, num, CMD_MAX_LINE);
        if (b->part_offset) {
            cmd_scat(line, " part_off=", CMD_MAX_LINE);
            cmd_uint_to_dec((uint32_t)b->part_offset, num, sizeof(num));
            cmd_scat(line, num, CMD_MAX_LINE);
        }
        if (b->formatted) cmd_scat(line, " fmt", CMD_MAX_LINE);
        PRINT(line);
    }

    PRINT("== drivers ==");
    IDE_DiagDump(ctx, dd_emit);
    AHCI_DiagDump(ctx, dd_emit);
    Vblk_DiagDump(ctx, dd_emit);
    Vscsi_DiagDump(ctx, dd_emit);
    FloppyBlk_DiagDump(ctx, dd_emit);
}
