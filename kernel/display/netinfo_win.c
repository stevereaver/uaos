/*
 * netinfo_win.c — UAOS NetInfo Application
 *
 * Workbench-style network information window.
 * Shows interface name, MAC, IP, netmask, gateway, broadcast,
 * DNS, DHCP status, and link state.
 */

#include "netinfo_win.h"
#include "wm.h"
#include "framebuffer.h"
#include "../net/stack.h"
#include "../net/ip.h"
#include "../net/net_device.h"
#include <stdint.h>
#include <stddef.h>

/* =========================================================================
 * Layout
 * ========================================================================= */

#define WIN_W   340
#define WIN_H   220

#define TITLEBAR_H  WM_TITLEBAR_H
#define BORDER      4

/* Client area origin */
#define CX   BORDER
#define CY   TITLEBAR_H

/* Line height for text rows */
#define LINE_H  16
#define PAD_X   12
#define PAD_Y   8

/* =========================================================================
 * State
 * ========================================================================= */

static int g_wm_handle = -1;
static int g_win_x, g_win_y, g_win_w, g_win_h;

/* =========================================================================
 * No-libc helpers
 * ========================================================================= */

static int s_len(const char *s) { int n = 0; while (s[n]) n++; return n; }

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

static void dig2(uint8_t v, char *out)
{
    out[0] = (char)('0' + v / 10);
    out[1] = (char)('0' + v % 10);
    out[2] = '\0';
}

/* Convert IPv4 to dotted string */
static void ipv4_to_str(ipv4_t ip, char *buf, int max)
{
    uint8_t a = (uint8_t)(ip >> 24);
    uint8_t b = (uint8_t)(ip >> 16);
    uint8_t c = (uint8_t)(ip >> 8);
    uint8_t d = (uint8_t)(ip);

    char tmp[20];
    tmp[0] = '\0';

    char oct[4];
    int i = 0;
    oct[i++] = (char)('0' + a / 100);
    if (a >= 100) a %= 100;
    oct[i++] = (char)('0' + a / 10);
    oct[i++] = (char)('0' + a % 10);
    oct[i] = '\0';
    s_cat(tmp, oct, sizeof(tmp));
    s_cat(tmp, ".", sizeof(tmp));

    i = 0;
    oct[i++] = (char)('0' + b / 100);
    if (b >= 100) b %= 100;
    oct[i++] = (char)('0' + b / 10);
    oct[i++] = (char)('0' + b % 10);
    oct[i] = '\0';
    s_cat(tmp, oct, sizeof(tmp));
    s_cat(tmp, ".", sizeof(tmp));

    i = 0;
    oct[i++] = (char)('0' + c / 100);
    if (c >= 100) c %= 100;
    oct[i++] = (char)('0' + c / 10);
    oct[i++] = (char)('0' + c % 10);
    oct[i] = '\0';
    s_cat(tmp, oct, sizeof(tmp));
    s_cat(tmp, ".", sizeof(tmp));

    i = 0;
    oct[i++] = (char)('0' + d / 100);
    if (d >= 100) d %= 100;
    oct[i++] = (char)('0' + d / 10);
    oct[i++] = (char)('0' + d % 10);
    oct[i] = '\0';
    s_cat(tmp, oct, sizeof(tmp));

    s_copy(buf, tmp, max);
}

/* =========================================================================
 * Draw
 * ========================================================================= */

static void netinfo_draw_content(int wx, int wy, int ww, int wh)
{
    g_win_x = wx; g_win_y = wy; g_win_w = ww; g_win_h = wh;

    int cx = wx + CX;
    int cy = wy + CY;
    int cw = ww - CX * 2;
    int ch = wh - CY - BORDER;

    /* Background */
    FB_FillRect(cx, cy, cw, ch, WB_GREY);
    FB_DrawRect(cx, cy, cw, ch, WB_DARK_GREY);

    /* Title inside window */
    FB_PutStr(cx + PAD_X, cy + PAD_Y, "Network Information", WB_BLACK, WB_GREY);

    /* Separator */
    int y = cy + PAD_Y + LINE_H + 2;
    FB_DrawHLine(cx + 8, y, cw - 16, WB_DARK_GREY);
    y += 6;

    if (!net_stack_is_up()) {
        FB_PutStr(cx + PAD_X, y, "No network device found.", WB_RED, WB_GREY);
        return;
    }

    char line[80];
    char val[32];
    const char *devname = netdev_name();

    /* Interface */
    s_copy(line, "Interface:  ", sizeof(line));
    s_cat(line, devname[0] ? devname : "unknown", sizeof(line));
    FB_PutStr(cx + PAD_X, y, line, WB_BLACK, WB_GREY);
    y += LINE_H;

    /* Link status */
    s_copy(line, "Status:     ", sizeof(line));
    s_cat(line, netdev_is_up() ? "Up" : "Down", sizeof(line));
    FB_PutStr(cx + PAD_X, y, line, netdev_is_up() ? WB_GREEN : WB_RED, WB_GREY);
    y += LINE_H;

    /* MAC */
    uint8_t mac[ETH_ALEN];
    netdev_get_mac(mac);
    static const char hex[] = "0123456789ABCDEF";
    s_copy(val, "", sizeof(val));
    int pos = 0;
    for (int b = 0; b < ETH_ALEN; b++) {
        val[pos++] = hex[(mac[b] >> 4) & 0xF];
        val[pos++] = hex[mac[b] & 0xF];
        if (b < ETH_ALEN - 1) val[pos++] = ':';
    }
    val[pos] = '\0';
    s_copy(line, "MAC:        ", sizeof(line));
    s_cat(line, val, sizeof(line));
    FB_PutStr(cx + PAD_X, y, line, WB_BLACK, WB_GREY);
    y += LINE_H;

    /* IP */
    ipv4_to_str(ip_get_local(), val, sizeof(val));
    s_copy(line, "IP:         ", sizeof(line));
    s_cat(line, val, sizeof(line));
    FB_PutStr(cx + PAD_X, y, line, WB_BLACK, WB_GREY);
    y += LINE_H;

    /* Netmask */
    ipv4_to_str(ip_get_netmask(), val, sizeof(val));
    s_copy(line, "Netmask:    ", sizeof(line));
    s_cat(line, val, sizeof(line));
    FB_PutStr(cx + PAD_X, y, line, WB_BLACK, WB_GREY);
    y += LINE_H;

    /* Gateway */
    ipv4_to_str(ip_get_gateway(), val, sizeof(val));
    s_copy(line, "Gateway:    ", sizeof(line));
    s_cat(line, val, sizeof(line));
    FB_PutStr(cx + PAD_X, y, line, WB_BLACK, WB_GREY);
    y += LINE_H;

    /* Broadcast */
    ipv4_t bc = ip_get_local() | ~ip_get_netmask();
    ipv4_to_str(bc, val, sizeof(val));
    s_copy(line, "Broadcast:  ", sizeof(line));
    s_cat(line, val, sizeof(line));
    FB_PutStr(cx + PAD_X, y, line, WB_BLACK, WB_GREY);
    y += LINE_H;

    /* DNS */
    ipv4_t dns = net_stack_get_dns();
    if (dns) {
        ipv4_to_str(dns, val, sizeof(val));
    } else {
        s_copy(val, "Not set", sizeof(val));
    }
    s_copy(line, "DNS:        ", sizeof(line));
    s_cat(line, val, sizeof(line));
    FB_PutStr(cx + PAD_X, y, line, WB_BLACK, WB_GREY);
    y += LINE_H;

    /* DHCP / Static */
    s_copy(line, "Config:     ", sizeof(line));
    s_cat(line, net_stack_dhcp_used() ? "DHCP" : "Static", sizeof(line));
    FB_PutStr(cx + PAD_X, y, line, net_stack_dhcp_used() ? WB_BLUE : WB_ORANGE, WB_GREY);
}

/* =========================================================================
 * WM callbacks
 * ========================================================================= */

static void netinfo_draw(int wx, int wy, int ww, int wh)
{
    netinfo_draw_content(wx, wy, ww, wh);
}

static void netinfo_key(char c) { (void)c; }

/* =========================================================================
 * Public API
 * ========================================================================= */

void NetInfoWin_Open(void)
{
    if (g_wm_handle >= 0 && WM_GetDrawFn(g_wm_handle) == netinfo_draw) {
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
                               "NetInfo", netinfo_draw, netinfo_key);
    if (g_wm_handle < 0) return;

    WM_Redraw();
}
