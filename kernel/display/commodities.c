/* commodities.c — UAOS Commodities (CX) Framework
 *
 * Simple broker registry for commodities.  Tools register themselves
 * and the Exchange window can list/enable/disable/sleep/wake them.
 */

#include "commodities.h"
#include <stddef.h>

static CxBroker g_brokers[CX_MAX_BROKERS];

static int cx_tolower(int c)
{
    return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

static int cx_str_eq_ci(const char *a, const char *b)
{
    while (*a && *b) {
        if (cx_tolower((unsigned char)*a) != cx_tolower((unsigned char)*b))
            return 0;
        a++; b++;
    }
    return *a == *b;
}

static void cx_str_copy(char *dst, const char *src, int max)
{
    int i = 0;
    while (src[i] && i < max - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

int Cx_Register(const char *name, const char *desc, int flags,
                void *userdata,
                void (*on_enable)(void *),
                void (*on_disable)(void *),
                void (*on_sleep)(void *),
                void (*on_wake)(void *))
{
    /* Don't register duplicates */
    if (name && name[0] && Cx_FindByName(name) >= 0)
        return -1;

    for (int i = 0; i < CX_MAX_BROKERS; i++) {
        if (!g_brokers[i].active) {
            g_brokers[i].active = 1;
            cx_str_copy(g_brokers[i].name, name ? name : "", 32);
            cx_str_copy(g_brokers[i].desc, desc ? desc : "", 64);
            g_brokers[i].flags = flags;
            g_brokers[i].state = CX_STATE_ACTIVE;
            g_brokers[i].userdata = userdata;
            g_brokers[i].on_enable  = on_enable;
            g_brokers[i].on_disable = on_disable;
            g_brokers[i].on_sleep   = on_sleep;
            g_brokers[i].on_wake    = on_wake;
            return i;
        }
    }
    return -1;
}

void Cx_Unregister(int idx)
{
    if (idx < 0 || idx >= CX_MAX_BROKERS) return;
    g_brokers[idx].active = 0;
    g_brokers[idx].name[0] = '\0';
    g_brokers[idx].desc[0] = '\0';
    g_brokers[idx].userdata = NULL;
    g_brokers[idx].on_enable = NULL;
    g_brokers[idx].on_disable = NULL;
    g_brokers[idx].on_sleep = NULL;
    g_brokers[idx].on_wake = NULL;
}

int Cx_FindByName(const char *name)
{
    if (!name) return -1;
    for (int i = 0; i < CX_MAX_BROKERS; i++) {
        if (g_brokers[i].active && cx_str_eq_ci(g_brokers[i].name, name))
            return i;
    }
    return -1;
}

const CxBroker *Cx_GetBroker(int idx)
{
    if (idx < 0 || idx >= CX_MAX_BROKERS) return NULL;
    if (!g_brokers[idx].active) return NULL;
    return &g_brokers[idx];
}

int Cx_Count(void)
{
    int n = 0;
    for (int i = 0; i < CX_MAX_BROKERS; i++)
        if (g_brokers[i].active) n++;
    return n;
}

void Cx_Enable(int idx)
{
    if (idx < 0 || idx >= CX_MAX_BROKERS || !g_brokers[idx].active) return;
    g_brokers[idx].state = CX_STATE_ACTIVE;
    if (g_brokers[idx].on_enable)
        g_brokers[idx].on_enable(g_brokers[idx].userdata);
}

void Cx_Disable(int idx)
{
    if (idx < 0 || idx >= CX_MAX_BROKERS || !g_brokers[idx].active) return;
    g_brokers[idx].state = CX_STATE_DISABLED;
    if (g_brokers[idx].on_disable)
        g_brokers[idx].on_disable(g_brokers[idx].userdata);
}

void Cx_Sleep(int idx)
{
    if (idx < 0 || idx >= CX_MAX_BROKERS || !g_brokers[idx].active) return;
    g_brokers[idx].state = CX_STATE_SLEEPING;
    if (g_brokers[idx].on_sleep)
        g_brokers[idx].on_sleep(g_brokers[idx].userdata);
}

void Cx_Wake(int idx)
{
    if (idx < 0 || idx >= CX_MAX_BROKERS || !g_brokers[idx].active) return;
    g_brokers[idx].state = CX_STATE_ACTIVE;
    if (g_brokers[idx].on_wake)
        g_brokers[idx].on_wake(g_brokers[idx].userdata);
}

void Cx_CycleState(int idx)
{
    if (idx < 0 || idx >= CX_MAX_BROKERS || !g_brokers[idx].active) return;
    switch (g_brokers[idx].state) {
        case CX_STATE_ACTIVE:   Cx_Sleep(idx);   break;
        case CX_STATE_SLEEPING: Cx_Disable(idx); break;
        case CX_STATE_DISABLED: Cx_Enable(idx);  break;
    }
}
