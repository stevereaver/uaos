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
        /* Blank the screen — fill with black directly to VRAM */
        if (g_fb.valid) {
            FB_BeginDraw();
            FB_FillRect(0, 0, (int)g_fb.width, (int)g_fb.height, WB_BLACK);
            FB_Flip();
            g_blanker_blanked = 1;
        }
    }
}

void Blanker_OnInput(void)
{
    /* Reset idle counter on any input activity */
    g_blanker_idle = 0;

    if (g_blanker_blanked) {
        g_blanker_blanked = 0;
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
