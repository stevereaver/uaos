/*
 * clock_win.c — UAOS Clock Application
 *
 * Workbench-style clock window.  Layout:
 *
 *   ┌─────────────────────────────────────────┐
 *   │  Clock                              [X] │  ← title bar (WM)
 *   ├─────────────────────────────────────────┤
 *   │                                         │
 *   │            13:45:22                     │  ← large time (2× font)
 *   │                                         │
 *   │       Saturday 14-Jun-2026              │  ← date line
 *   │                                         │
 *   │       AEST  (Australia/Sydney)          │  ← timezone
 *   │                                         │
 *   └─────────────────────────────────────────┘
 *
 * The window is updated once per second via ClockWin_Tick(), which is called
 * from Desktop_UpdateClock() (the RTC IRQ8 handler fires once per second).
 *
 * When ntpd has not yet run, the clock falls back to reading the CMOS RTC
 * directly (which shows UTC in that case, labelled "UTC").
 */
#include "clock_win.h"
#include "wm.h"
#include "framebuffer.h"
#include "../net/ntp.h"
#include "../net/timezone.h"
#include "../irq/rtc.h"
#include <stdint.h>
#include <stddef.h>

/* =========================================================================
 * Layout
 * ========================================================================= */

#define WIN_W   280
#define WIN_H   140

#define TITLEBAR_H  WM_TITLEBAR_H
#define BORDER      4

/* Client area origin */
#define CX   BORDER
#define CY   TITLEBAR_H

/* =========================================================================
 * State
 * ========================================================================= */

static int g_wm_handle = -1;
static int g_win_x, g_win_y, g_win_w, g_win_h;

/* =========================================================================
 * No-libc helpers
 * ========================================================================= */

static void s_copy(char *d, const char *s, int max)
{
    int i = 0;
    while (i < max - 1 && s[i]) { d[i] = s[i]; i++; }
    d[i] = '\0';
}

static void s_cat(char *d, const char *s, int max)
{
    int dl = 0; while (d[dl]) dl++;
    int i = 0;
    while (dl + i < max - 1 && s[i]) { d[dl + i] = s[i]; i++; }
    d[dl + i] = '\0';
}

static int s_len(const char *s) { int n = 0; while (s[n]) n++; return n; }

static void dig2(uint8_t v, char *out)
{
    out[0] = (char)('0' + v / 10);
    out[1] = (char)('0' + v % 10);
    out[2] = '\0';
}

static void u16_str(uint16_t v, char *buf, int max)
{
    char tmp[8]; int i = 0, j = 0;
    if (!v) { buf[0]='0'; buf[1]='\0'; return; }
    while (v && i < 7) { tmp[i++] = (char)('0' + v % 10); v /= 10; }
    while (i-- && j < max - 1) buf[j++] = tmp[i];
    buf[j] = '\0';
}

/* =========================================================================
 * Calendar helpers (day-of-week, month name)
 * ========================================================================= */

static const char *const k_months[] = {
    "", "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};

static const char *const k_days[] = {
    "Sunday","Monday","Tuesday","Wednesday","Thursday","Friday","Saturday"
};

/* Tomohiko Sakamoto's day-of-week (0=Sun…6=Sat) */
static int dow(uint16_t y, uint8_t m, uint8_t d)
{
    static const int t[] = {0,3,2,5,0,3,5,1,4,6,2,4};
    if (m < 3) y--;
    return (int)((y + y/4 - y/100 + y/400 + t[m-1] + d) % 7);
}

/* =========================================================================
 * Draw
 * ========================================================================= */

static void clock_draw_content(int wx, int wy, int ww, int wh)
{
    (void)ww; (void)wh;

    g_win_x = wx; g_win_y = wy; g_win_w = ww; g_win_h = wh;

    int cx = wx + CX;
    int cy = wy + CY;
    int cw = ww - CX * 2;
    int ch = wh - CY - BORDER;

    /* Background */
    FB_FillRect(cx, cy, cw, ch, WB_GREY);
    FB_DrawRect(cx, cy, cw, ch, WB_DARK_GREY);

    /* ── Get current local time ──────────────────────────────────────────── */
    uint16_t year; uint8_t month, day, hour, min, sec;
    const TzInfo *tz = tz_get_current();
    uint32_t epoch   = ntp_get_epoch();
    const char *abbr;

    if (epoch) {
        /* NTP epoch is live: convert UTC → local */
        int32_t  off_min   = tz_offset_min(tz, epoch);
        uint32_t local_ts  = (uint32_t)((int64_t)epoch + (int64_t)off_min * 60);
        ntp_unix_to_datetime(local_ts, &year, &month, &day, &hour, &min, &sec);
        abbr = tz_abbr(tz, epoch);
    } else {
        /* Fallback: read CMOS directly (UTC, no timezone applied) */
        RtcDateTime dt = RTC_ReadDateTime();
        year  = dt.year; month = dt.month; day = dt.day;
        hour  = dt.hour; min   = dt.min;   sec = dt.sec;
        abbr  = "UTC";
        tz    = NULL;
    }

    /* ── Time: "HH:MM:SS" — drawn with a 2× scale font emulation ────────── */
    /* We don't have a 2× font, so draw the characters twice as wide/tall   */
    /* by drawing each glyph twice offset by 1 pixel (bold effect).         */
    char time_s[12];
    {
        char h2[3], m2[3], s2[3];
        dig2(hour, h2); dig2(min, m2); dig2(sec, s2);
        time_s[0]=h2[0]; time_s[1]=h2[1];
        time_s[2]=':';
        time_s[3]=m2[0]; time_s[4]=m2[1];
        time_s[5]=':';
        time_s[6]=s2[0]; time_s[7]=s2[1];
        time_s[8]='\0';
    }

    /* Centre the time string — each char is 8px wide, we have 8 chars */
    int time_w = s_len(time_s) * 8;
    int time_x = cx + (cw - time_w) / 2;
    int time_y = cy + 12;

    /* Bold effect: draw twice, shifted 1px right */
    FB_PutStr(time_x,     time_y, time_s, WB_BLACK, WB_GREY);
    FB_PutStr(time_x + 1, time_y, time_s, WB_BLACK, WB_GREY);
    /* And once shifted 1px down for a slight 3D feel */
    FB_PutStr(time_x, time_y + 1, time_s, WB_DARK_GREY, WB_GREY);

    /* ── Date: "Saturday 14-Jun-2026" ────────────────────────────────────── */
    char date_s[32];
    if (year >= 2000 && month >= 1 && month <= 12 && day >= 1 && day <= 31) {
        int wd = dow(year, month, day);
        if (wd < 0 || wd > 6) wd = 0;

        char day_s[3], yr_s[8];
        dig2(day, day_s);
        u16_str(year, yr_s, sizeof(yr_s));

        s_copy(date_s, k_days[wd],         sizeof(date_s));
        s_cat (date_s, " ",                sizeof(date_s));
        s_cat (date_s, day_s,              sizeof(date_s));
        s_cat (date_s, "-",                sizeof(date_s));
        s_cat (date_s, k_months[month],    sizeof(date_s));
        s_cat (date_s, "-",                sizeof(date_s));
        s_cat (date_s, yr_s,               sizeof(date_s));
    } else {
        s_copy(date_s, "Date not set", sizeof(date_s));
    }

    int date_x = cx + (cw - s_len(date_s) * 8) / 2;
    int date_y = time_y + 22;
    FB_PutStr(date_x, date_y, date_s, WB_BLACK, WB_GREY);

    /* ── Separator line ──────────────────────────────────────────────────── */
    int sep_y = date_y + 18;
    FB_DrawHLine(cx + 16, sep_y, cw - 32, WB_DARK_GREY);

    /* ── Timezone: "AEST  (Australia/Sydney)" ────────────────────────────── */
    char tz_s[80];
    s_copy(tz_s, abbr, sizeof(tz_s));
    if (tz && tz->name) {
        s_cat(tz_s, "  (", sizeof(tz_s));
        s_cat(tz_s, tz->name, sizeof(tz_s));
        s_cat(tz_s, ")", sizeof(tz_s));
    }

    int tz_x = cx + (cw - s_len(tz_s) * 8) / 2;
    int tz_y = sep_y + 6;
    FB_PutStr(tz_x, tz_y, tz_s, WB_BLUE, WB_GREY);
}

/* =========================================================================
 * WM callbacks
 * ========================================================================= */

static void clock_draw(int wx, int wy, int ww, int wh)
{
    clock_draw_content(wx, wy, ww, wh);
}

static void clock_key(char c) { (void)c; }

/* =========================================================================
 * Public API
 * ========================================================================= */

void ClockWin_Tick(void)
{
    if (g_wm_handle < 0) return;
    if (!WM_IsWindowActive(g_wm_handle)) { g_wm_handle = -1; return; }
    /* Redraw just our window content without a full WM_Redraw() */
    clock_draw_content(g_win_x, g_win_y, g_win_w, g_win_h);
}

void ClockWin_Open(void)
{
    if (g_wm_handle >= 0 && WM_IsWindowActive(g_wm_handle)) {
        WM_RaiseWindow(g_wm_handle);
        WM_Redraw();
        return;
    }

    g_wm_handle = -1;

    /* Centre on screen */
    extern FbState g_fb;
    int W = (int)g_fb.width;
    int H = (int)g_fb.height;
    int wx = (W - WIN_W) / 2;
    int wy = (H - WIN_H) / 2;

    g_wm_handle = WM_AddWindow(wx, wy, WIN_W, WIN_H,
                               "Clock", clock_draw, clock_key);
    if (g_wm_handle < 0) return;

    WM_Redraw();
}
