/* blanker.c — UAOS Screen Blanker
 *
 * A commodity that blanks the screen after a configurable period of
 * mouse/keyboard inactivity.  Registers with the Commodities framework
 * so it can be controlled from Exchange.
 */

#include "blanker.h"
#include "commodities.h"
#include "framebuffer.h"
#include "wm.h"
#include <stdint.h>
#include <stddef.h>

static int  g_blanker_timeout = 60;   /* seconds of inactivity before blank */
static int  g_blanker_idle    = 0;    /* current idle counter */
static int  g_blanker_blanked = 0;    /* screen is currently blanked */
static int  g_blanker_broker  = -1;   /* CX broker index */
static volatile int g_blanker_pending = 0; /* IRQ requested a blank frame */
static int  g_blanker_saved_pct = -1; /* brightness before blank (-1 none) */

/* UAOS-139: on bare metal with the NV50 PWM driver a blank also drops
 * the panel backlight to 0 — real backlight-off instead of just black
 * pixels.  No-op where the driver found no GPU. */
static void blanker_backlight(int on)
{
    extern int NV50BL_Present(void);
    extern int NV50BL_GetBrightness(void);
    extern int NV50BL_SetBrightness(int);
    if (!NV50BL_Present()) return;
    if (!on) {
        int cur = NV50BL_GetBrightness();
        if (cur > 0) {
            g_blanker_saved_pct = cur;
            NV50BL_SetBrightness(0);
        }
    } else if (g_blanker_saved_pct >= 0) {
        NV50BL_SetBrightness(g_blanker_saved_pct);
        g_blanker_saved_pct = -1;
    }
}

static void blanker_on_enable(void *ud)
{
    (void)ud;
    /* Reset idle counter when enabled */
    g_blanker_idle = 0;
}

static void blanker_on_disable(void *ud)
{
    (void)ud;
    /* Unblank if currently blanked */
    if (g_blanker_blanked) {
        g_blanker_blanked = 0;
        blanker_backlight(1);
        WM_Redraw();
    }
    g_blanker_idle = 0;
}

static void blanker_on_sleep(void *ud)
{
    (void)ud;
    /* Unblank when going to sleep (pause blanking) */
    if (g_blanker_blanked) {
        g_blanker_blanked = 0;
        blanker_backlight(1);
        WM_Redraw();
    }
}

static void blanker_on_wake(void *ud)
{
    (void)ud;
    g_blanker_idle = 0;
}

void Blanker_Init(void)
{
    g_blanker_broker = Cx_Register(
        "Blanker",
        "Screen blanker — blanks after idle timeout",
        0,  /* no input handler flag */
        NULL,
        blanker_on_enable,
        blanker_on_disable,
        blanker_on_sleep,
        blanker_on_wake);
}

void Blanker_Tick(void)
{
    if (g_blanker_broker < 0) return;

    const CxBroker *b = Cx_GetBroker(g_blanker_broker);
    if (!b) return;

    /* Only count idle time when active */
    if (b->state != CX_STATE_ACTIVE) return;

    g_blanker_idle++;

    if (g_blanker_idle >= g_blanker_timeout && !g_blanker_blanked) {
        /* UAOS-191: Blanker_Tick runs inside the RTC IRQ handler (via
         * Desktop_UpdateClock) — it must not open a framebuffer frame
         * here.  Flag the request; the event pump paints the black frame
         * from task context in Blanker_Flush().  The RTC path wakes the
         * pump on every tick, so the delay is at most one pump pass. */
        g_blanker_pending = 1;
    }
}

/* Called once per event-pump iteration (task context).  Performs the blank
 * requested by Blanker_Tick: BeginDraw/black fill/Flip are framebuffer
 * frame operations and belong on the pump like every other paint. */
void Blanker_Flush(void)
{
    if (!g_blanker_pending) return;
    g_blanker_pending = 0;
    if (g_blanker_blanked || !g_fb.valid) return;

    FB_BeginDraw();
    FB_FillRect(0, 0, (int)g_fb.width, (int)g_fb.height, WB_BLACK);
    FB_Flip();
    g_blanker_blanked = 1;
    blanker_backlight(0);
}

void Blanker_OnInput(void)
{
    /* Reset idle counter on any input activity */
    g_blanker_idle = 0;

    if (g_blanker_blanked) {
        g_blanker_blanked = 0;
        blanker_backlight(1);
        WM_Redraw();
    }
}

int Blanker_IsBlanked(void)
{
    return g_blanker_blanked;
}

void Blanker_SetTimeout(int seconds)
{
    if (seconds < 10) seconds = 10;
    if (seconds > 600) seconds = 600;
    g_blanker_timeout = seconds;
}

int Blanker_GetTimeout(void)
{
    return g_blanker_timeout;
}
