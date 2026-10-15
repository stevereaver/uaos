/* adf_picker.c — double-click .adf → "Insert Disk" drive picker (UAOS-158) */

#include "adf_picker.h"
#include "requester.h"
#include "wm.h"
#include "framebuffer.h"
#include "../dos/adf_mount.h"
#include "../chipset/floppy.h"
#include <stddef.h>

/* The requester callback runs later, after the caller's path buffer may
 * be gone — keep our own copy. */
static char g_adf_path[128];

static void str_cp(char *dst, const char *src, int max)
{
    int i = 0;
    while (src && src[i] && i < max - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static const char *adf_basename(const char *path)
{
    const char *base = path;
    for (const char *p = path; p && *p; p++)
        if (*p == '/' || *p == ':') base = p + 1;
    return base;
}

int AdfPicker_IsAdfPath(const char *path)
{
    if (!path) return 0;
    int len = 0;
    while (path[len]) len++;
    if (len < 4) return 0;
    const char *s = path + len - 4;
    if (s[0] != '.') return 0;
    char a = s[1], d = s[2], f = s[3];
    if (a >= 'A' && a <= 'Z') a += 'a' - 'A';
    if (d >= 'A' && d <= 'Z') d += 'a' - 'A';
    if (f >= 'A' && f <= 'Z') f += 'a' - 'A';
    return a == 'a' && d == 'd' && f == 'f';
}

static void picker_cb(int button, const char *text, void *user_data)
{
    (void)text; (void)user_data;
    if (button < 0 || button > 3) return;   /* Cancel / REQ_BTN_NONE */
    int err = AdfMount_Insert(button, g_adf_path);
    if (err == ADF_OK) {
        /* Repaint the desktop now so the new disk icon shows at once. */
        WM_InvalidateDesktopRect(0, 0, (int)g_fb.width, (int)g_fb.height);
        return;
    }

    /* "Could not insert <basename>" — keep the whole line ≤63 chars. */
    char l0[64];
    const char *pre = "Could not insert ";
    int i = 0;
    while (pre[i] && i < 62) { l0[i] = pre[i]; i++; }
    const char *base = adf_basename(g_adf_path);
    for (int k = 0; base[k] && i < 63; k++) l0[i++] = base[k];
    l0[i] = '\0';

    const char *lines[] = { l0, AdfMount_ErrorString(err), NULL };
    Requester_Info("Insert Disk", lines, NULL, NULL);
}

void AdfPicker_Open(const char *path)
{
    str_cp(g_adf_path, path, sizeof(g_adf_path));

    /* enabled[u] = drive u is empty; Cancel is always enabled. */
    int enabled[5];
    int any_free = 0;
    for (int u = 0; u < 4; u++) {
        enabled[u] = !floppy_unit_loaded(u);
        if (enabled[u]) any_free = 1;
    }
    enabled[4] = 1;

    /* Body: Insert "<basename>" into which drive?  — the requester
     * copies each line into a 64-char buffer (REQ_MAX_LABEL); the fixed
     * text is 27 chars, so cap the basename at 36. */
    char body[128];
    const char *pre = "Insert \"";
    int i = 0;
    while (pre[i]) { body[i] = pre[i]; i++; }
    const char *base = adf_basename(g_adf_path);
    for (int k = 0; base[k] && k < 36 && i < 60; k++) body[i++] = base[k];
    const char *post = "\" into which drive?";
    for (int k = 0; post[k]; k++) body[i++] = post[k];
    if (!any_free) {
        const char *full = "\nAll drives are in use.";
        for (int k = 0; full[k]; k++) body[i++] = full[k];
    }
    body[i] = '\0';

    static const char *const labels[5] = {
        "DF0:", "DF1:", "DF2:", "DF3:", "Cancel"
    };
    Requester_Choice("Insert Disk", body, labels, enabled, 5,
                     picker_cb, NULL);
}
