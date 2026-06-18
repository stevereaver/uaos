/* dos_list.c — DosList global registry implementation
 *
 * Simple linked list of DosList nodes stored in a static pool.
 */

#include "dos_list.h"
#include "dospacket.h"
#include <stddef.h>
#include <stdint.h>

#define MAX_DOSLIST 32

static DosList  g_doslist_pool[MAX_DOSLIST];
static DosList *g_doslist_head = NULL;
static int      g_doslist_used = 0;

/* -------------------------------------------------------------------------
 * String helpers
 * ------------------------------------------------------------------------- */
static int dlen(const char *s)
{
    int n = 0;
    while (s[n]) n++;
    return n;
}

static void dcpy(char *d, const char *s, int max)
{
    int i = 0;
    while (i < max - 1 && s[i]) { d[i] = s[i]; i++; }
    d[i] = '\0';
}

static int dcmp_ci(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = ca - 'A' + 'a';
        if (cb >= 'A' && cb <= 'Z') cb = cb - 'A' + 'a';
        if (ca != cb) return 0;
        a++; b++;
    }
    char ca = *a, cb = *b;
    if (ca >= 'A' && ca <= 'Z') ca = ca - 'A' + 'a';
    if (cb >= 'A' && cb <= 'Z') cb = cb - 'A' + 'a';
    return ca == cb;
}

/* -------------------------------------------------------------------------
 * Internal helpers
 * ------------------------------------------------------------------------- */
static DosList *alloc_node(void)
{
    if (g_doslist_used >= MAX_DOSLIST) return NULL;
    DosList *n = &g_doslist_pool[g_doslist_used++];
    for (int i = 0; i < (int)sizeof(DosList); i++)
        ((uint8_t *)n)[i] = 0;
    return n;
}

/* -------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

void DosList_Init(void)
{
    g_doslist_used = 0;
    g_doslist_head = NULL;
    for (int i = 0; i < MAX_DOSLIST; i++) {
        DosList *n = &g_doslist_pool[i];
        for (int j = 0; j < (int)sizeof(DosList); j++)
            ((uint8_t *)n)[j] = 0;
    }
}

DosList *DosList_AddDevice(const char *name, MsgPort *handler_port,
                           uint32_t disk_type)
{
    DosList *n = alloc_node();
    if (!n) return NULL;

    dcpy(n->dol_Name, name, sizeof(n->dol_Name));
    n->dol_Type = DLT_DEVICE;
    n->u.dol_Device.dol_Handler = (struct MsgPort *)handler_port;
    n->u.dol_Device.dol_Lock = 0;
    n->u.dol_Device.dol_DiskType = disk_type;

    n->dol_Next = g_doslist_head;
    g_doslist_head = n;
    return n;
}

DosList *DosList_AddVolume(const char *name, MsgPort *handler_port,
                           uint32_t volume_date)
{
    DosList *n = alloc_node();
    if (!n) return NULL;

    dcpy(n->dol_Name, name, sizeof(n->dol_Name));
    n->dol_Type = DLT_VOLUME;
    n->u.dol_Device.dol_Handler = (struct MsgPort *)handler_port;
    n->u.dol_Device.dol_Lock = 0;
    n->u.dol_Device.dol_DiskType = volume_date;

    n->dol_Next = g_doslist_head;
    g_doslist_head = n;
    return n;
}

DosList *DosList_AddAssign(const char *name, BPTR lock)
{
    DosList *n = alloc_node();
    if (!n) return NULL;

    dcpy(n->dol_Name, name, sizeof(n->dol_Name));
    n->dol_Type = DLT_ASSIGN;
    n->u.dol_Assign.dol_Lock = lock;

    n->dol_Next = g_doslist_head;
    g_doslist_head = n;
    return n;
}

void DosList_Remove(const char *name, uint8_t type)
{
    DosList **pp = &g_doslist_head;
    while (*pp) {
        DosList *n = *pp;
        if (n->dol_Type == type && dcmp_ci(n->dol_Name, name)) {
            *pp = n->dol_Next;
            return;
        }
        pp = &n->dol_Next;
    }
}

DosList *DosList_Find(const char *name)
{
    DosList *n = g_doslist_head;
    while (n) {
        if (dcmp_ci(n->dol_Name, name)) return n;
        n = n->dol_Next;
    }
    return NULL;
}

DosList *DosList_FindByType(const char *name, uint8_t type)
{
    DosList *n = g_doslist_head;
    while (n) {
        if (n->dol_Type == type && dcmp_ci(n->dol_Name, name)) return n;
        n = n->dol_Next;
    }
    return NULL;
}

MsgPort *DosList_FindHandlerPort(const char *name)
{
    DosList *n = g_doslist_head;
    while (n) {
        if (dcmp_ci(n->dol_Name, name)) {
            if (n->dol_Type == DLT_DEVICE || n->dol_Type == DLT_VOLUME) {
                return (MsgPort *)n->u.dol_Device.dol_Handler;
            }
        }
        n = n->dol_Next;
    }
    return NULL;
}

DosList *DosList_Next(DosList *prev)
{
    if (!prev) return g_doslist_head;
    return prev->dol_Next;
}

int DosList_Count(void)
{
    int c = 0;
    DosList *n = g_doslist_head;
    while (n) { c++; n = n->dol_Next; }
    return c;
}
