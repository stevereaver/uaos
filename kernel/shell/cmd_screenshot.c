/* cmd_screenshot.c — C:screenshot — capture the whole screen to a JPEG
 *
 *   screenshot                  -> RAM:<YYYYMMDDHHMMSS>.jpg (auto serial)
 *   screenshot FILE=path        -> explicit destination
 *   screenshot Q=n              -> JPEG quality 1..100 (default 85)
 *
 * The default filename is a serial number built from the capture
 * date/time (NTP-synced local time when available, CMOS RTC otherwise).
 * A same-second second shot gets a _N suffix so captures never overwrite.
 *
 * Pixels come from FB_GetPixel — the composited back buffer once the WM
 * has made it coherent, VRAM before that.  The mouse pointer sprite is
 * drawn straight to VRAM at flip time and is not part of the capture.
 */

#include "cmd_internal.h"
#include "../display/framebuffer.h"
#include "../display/jpeg_enc.h"
#include "../irq/rtc.h"
#include "../net/ntp.h"
#include "../net/timezone.h"

/* ------------------------------------------------------------------ */

typedef struct {
    VfsFile  *fh;
    uint32_t  bytes;
    int       err;
} ShotSink;

static int shot_write(void *ud, const uint8_t *data, uint32_t len)
{
    ShotSink *s = (ShotSink *)ud;
    uint32_t w = VFS_Write(s->fh, data, len);
    s->bytes += w;
    if (w != len) { s->err = 1; return 1; }
    return 0;
}

static uint32_t shot_pixel(void *ud, int x, int y)
{
    (void)ud;
    return FB_GetPixel(x, y);
}

/* ------------------------------------------------------------------ */

static void dig2(char **p, uint32_t v)
{
    (*p)[0] = (char)('0' + (v / 10) % 10);
    (*p)[1] = (char)('0' +  v % 10);
    *p += 2;
}

/* Build "YYYYMMDDHHMMSS" (14 chars + NUL) from current date/time.
 * Returns 1 when the clock was sane, 0 when unset — caller falls back
 * to a plain counter serial. */
static int shot_serial(char *out)
{
    uint16_t year; uint8_t month, day, hour, min, sec;

    uint32_t epoch = ntp_get_epoch();
    if (epoch) {
        const TzInfo *tz  = tz_get_current();
        uint32_t local_ts = (uint32_t)((int64_t)epoch +
                                       (int64_t)tz_offset_min(tz, epoch) * 60);
        ntp_unix_to_datetime(local_ts, &year, &month, &day, &hour, &min, &sec);
    } else {
        RtcDateTime dt = RTC_ReadDateTime();
        year = dt.year; month = dt.month; day = dt.day;
        hour = dt.hour; min   = dt.min;   sec = dt.sec;
        if (year < 2000 || year > 2099 ||
            month < 1 || month > 12 || day < 1 || day > 31)
            return 0;
    }

    char *p = out;
    *p++ = (char)('0' + (year / 1000) % 10);
    *p++ = (char)('0' + (year / 100)  % 10);
    *p++ = (char)('0' + (year / 10)   % 10);
    *p++ = (char)('0' +  year         % 10);
    dig2(&p, month); dig2(&p, day);
    dig2(&p, hour);  dig2(&p, min); dig2(&p, sec);
    *p = '\0';
    return 1;
}

static int file_exists(const char *path)
{
    VfsFile t;
    if (VFS_Open(&t, path, VFS_READ)) { VFS_Close(&t); return 1; }
    return 0;
}

/* ------------------------------------------------------------------ */

void Cmd_Screenshot(NativeCmdCtx *ctx, const char *args)
{
    if (!g_fb.valid) {
        PRINT("screenshot: no framebuffer");
        return;
    }

    int quality = 85;
    const char *p;
    uint64_t v;
    if ((p = cmd_kv_find(args, "q")) && cmd_parse_uint(p, &v))
        quality = (int)v;
    if (quality < 1 || quality > 100) {
        PRINT("screenshot: Q must be 1..100");
        return;
    }

    /* Destination: FILE= override, else RAM:<serial>.jpg */
    char path[CMD_MAX_PATH];
    if ((p = cmd_kv_find(args, "file"))) {
        int i = 0;
        while (p[i] && p[i] != ' ' && i < CMD_MAX_PATH - 1) {
            path[i] = p[i]; i++;
        }
        path[i] = '\0';
    } else {
        char serial[16];
        cmd_scopy(path, "RAM:", CMD_MAX_PATH);
        if (shot_serial(serial)) {
            cmd_scat(path, serial, CMD_MAX_PATH);
            cmd_scat(path, ".jpg", CMD_MAX_PATH);
            /* Same-second collision -> append _N */
            for (int n = 1; file_exists(path) && n < 100; n++) {
                cmd_scopy(path, "RAM:", CMD_MAX_PATH);
                cmd_scat(path, serial, CMD_MAX_PATH);
                cmd_scat(path, "_", CMD_MAX_PATH);
                char num[8];
                cmd_uint_to_dec((uint32_t)n, num, sizeof(num));
                cmd_scat(path, num, CMD_MAX_PATH);
                cmd_scat(path, ".jpg", CMD_MAX_PATH);
            }
        } else {
            /* Clock unset — plain monotonic serial instead */
            static uint32_t s_nos_clock = 0;
            char num[12];
            cmd_uint_to_dec(++s_nos_clock, num, sizeof(num));
            cmd_scat(path, "shot", CMD_MAX_PATH);
            cmd_scat(path, num, CMD_MAX_PATH);
            cmd_scat(path, ".jpg", CMD_MAX_PATH);
        }
    }

    VfsFile fh;
    if (!VFS_Open(&fh, path, VFS_WRITE | VFS_CREATE | VFS_TRUNC)) {
        char msg[CMD_MAX_LINE];
        cmd_scopy(msg, "screenshot: cannot create ", CMD_MAX_LINE);
        cmd_scat(msg, path, CMD_MAX_LINE);
        PRINT(msg);
        return;
    }

    ShotSink sink;
    sink.fh = &fh; sink.bytes = 0; sink.err = 0;

    int rc = Jpeg_Encode(shot_write, &sink, shot_pixel, NULL,
                         (int)g_fb.width, (int)g_fb.height, quality);
    VFS_Close(&fh);

    if (rc || sink.err) {
        VFS_Delete(path);
        PRINT("screenshot: write failed (out of space?)");
        return;
    }

    char msg[CMD_MAX_LINE], num[12];
    cmd_scopy(msg, "screenshot: ", CMD_MAX_LINE);
    cmd_scat(msg, path, CMD_MAX_LINE);
    cmd_scat(msg, " (", CMD_MAX_LINE);
    cmd_uint_to_dec(sink.bytes, num, sizeof(num));
    cmd_scat(msg, num, CMD_MAX_LINE);
    cmd_scat(msg, " bytes)", CMD_MAX_LINE);
    PRINT(msg);
}
