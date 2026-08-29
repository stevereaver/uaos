/*
 * prefs_lib.c — UAOS Preferences Persistence Library
 *
 * Implements AmigaOS IFF PREF file format reader/writer.
 */

#include "prefs_lib.h"
#include "../dos/vfs.h"
#include "../boot/kprint.h"
#include <stdint.h>
#include <string.h>

/* =========================================================================
 * Big-endian helpers
 * ========================================================================= */

static uint32_t read_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}

static void write_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)(v & 0xFF);
}

static int tag_eq(const char *a, const char *b)
{
    for (int i = 0; i < 4; i++)
        if (a[i] != b[i]) return 0;
    return 1;
}

static void tag_cpy(char *dst, const char *src)
{
    for (int i = 0; i < 4; i++) dst[i] = src[i];
}

/* =========================================================================
 * Prefs_Load — read IFF PREF from VFS
 * ========================================================================= */

int Prefs_Load(const char *path, PrefsFile *out)
{
    if (!path || !out) return 0;
    memset(out, 0, sizeof(PrefsFile));

    VfsFile fh;
    if (!VFS_Open(&fh, path, VFS_READ)) return 0;

    uint32_t fsize = VFS_Size(&fh);
    if (fsize < 12 || fsize > 8192) {
        VFS_Close(&fh);
        return 0;
    }

    static uint8_t buf[8192];
    uint32_t rd = VFS_Read(&fh, buf, fsize);
    VFS_Close(&fh);
    if (rd < 12) return 0;

    /* Verify IFF FORM header */
    if (read_be32(buf) != IFF_FORM) return 0;
    uint32_t form_size = read_be32(buf + 4);
    if (form_size + 8 > rd) form_size = rd - 8;

    /* Check PREF type */
    if (!tag_eq((const char *)(buf + 8), "PREF")) return 0;

    /* Parse chunks starting at offset 12 */
    uint32_t pos = 12;
    while (pos + 8 <= 12 + form_size && out->chunk_count < PREFS_MAX_CHUNKS) {
        const char *ctype = (const char *)(buf + pos);
        uint32_t csize = read_be32(buf + pos + 4);
        pos += 8;

        if (pos + csize > 12 + form_size) break;
        if (csize > PREFS_MAX_CHUNK_DATA) {
            pos += csize;
            if (csize & 1) pos++; /* IFF padding */
            continue;
        }

        PrefsChunk *ch = &out->chunks[out->chunk_count];
        tag_cpy(ch->type, ctype);
        ch->size = csize;
        memcpy(ch->data, buf + pos, csize);
        out->chunk_count++;

        /* If this is PRHD, extract prefs type */
        if (tag_eq(ctype, "PRHD") && csize >= 1) {
            out->prefs_type = ch->data[0];
        }

        pos += csize;
        if (csize & 1) pos++; /* IFF chunks are padded to even size */
    }

    return 1;
}

/* =========================================================================
 * Prefs_Save — write IFF PREF to VFS
 * ========================================================================= */

int Prefs_Save(const char *path, const PrefsFile *pf)
{
    if (!path || !pf) return 0;

    static uint8_t buf[8192];
    uint32_t pos = 0;

    /* FORM header (will fill size later) */
    write_be32(buf + 0, IFF_FORM);
    write_be32(buf + 4, 0); /* placeholder */
    memcpy(buf + 8, "PREF", 4);
    pos = 12;

    /* Ensure PRHD chunk exists */
    int has_prhd = 0;
    for (int i = 0; i < pf->chunk_count; i++) {
        if (tag_eq(pf->chunks[i].type, "PRHD")) {
            has_prhd = 1;
            break;
        }
    }

    /* Auto-add PRHD if missing */
    if (!has_prhd && pos + 8 + 4 <= sizeof(buf)) {
        memcpy(buf + pos, "PRHD", 4);
        write_be32(buf + pos + 4, 4);
        buf[pos + 8] = pf->prefs_type;
        buf[pos + 9] = 0;
        buf[pos + 10] = 0;
        buf[pos + 11] = 0;
        pos += 12;
    }

    /* Write chunks */
    for (int i = 0; i < pf->chunk_count; i++) {
        const PrefsChunk *ch = &pf->chunks[i];
        uint32_t csize = ch->size;
        if (pos + 8 + csize + (csize & 1) > sizeof(buf)) return 0;

        memcpy(buf + pos, ch->type, 4);
        write_be32(buf + pos + 4, csize);
        pos += 8;
        memcpy(buf + pos, ch->data, csize);
        pos += csize;
        if (csize & 1) buf[pos++] = 0; /* pad to even */
    }

    /* Fill FORM size */
    write_be32(buf + 4, pos - 8);

    /* Write to VFS */
    VfsFile fh;
    if (!VFS_Open(&fh, path, VFS_WRITE | VFS_CREATE | VFS_TRUNC)) return 0;
    VFS_Write(&fh, buf, pos);
    VFS_Close(&fh);
    return 1;
}

/* =========================================================================
 * Chunk manipulation
 * ========================================================================= */

const PrefsChunk *Prefs_FindChunk(const PrefsFile *pf, const char *type)
{
    if (!pf || !type) return NULL;
    for (int i = 0; i < pf->chunk_count; i++) {
        if (tag_eq(pf->chunks[i].type, type))
            return &pf->chunks[i];
    }
    return NULL;
}

int Prefs_SetChunk(PrefsFile *pf, const char *type,
                   const uint8_t *data, uint32_t size)
{
    if (!pf || !type || (!data && size > 0)) return 0;
    if (size > PREFS_MAX_CHUNK_DATA) return 0;

    /* Find existing chunk */
    for (int i = 0; i < pf->chunk_count; i++) {
        if (tag_eq(pf->chunks[i].type, type)) {
            pf->chunks[i].size = size;
            if (size > 0)
                memcpy(pf->chunks[i].data, data, size);
            return 1;
        }
    }

    /* Add new chunk */
    if (pf->chunk_count >= PREFS_MAX_CHUNKS) return 0;

    PrefsChunk *ch = &pf->chunks[pf->chunk_count];
    tag_cpy(ch->type, type);
    ch->size = size;
    if (size > 0)
        memcpy(ch->data, data, size);
    pf->chunk_count++;
    return 1;
}

int Prefs_RemoveChunk(PrefsFile *pf, const char *type)
{
    if (!pf || !type) return 0;
    for (int i = 0; i < pf->chunk_count; i++) {
        if (tag_eq(pf->chunks[i].type, type)) {
            for (int j = i; j < pf->chunk_count - 1; j++)
                pf->chunks[j] = pf->chunks[j + 1];
            pf->chunk_count--;
            return 1;
        }
    }
    return 0;
}

void Prefs_SetType(PrefsFile *pf, uint8_t type)
{
    if (!pf) return;
    pf->prefs_type = type;
    /* Update or add PRHD chunk */
    uint8_t prhd[4] = { type, 0, 0, 0 };
    Prefs_SetChunk(pf, "PRHD", prhd, 4);
}

/* =========================================================================
 * ENV: / ENVARC: helpers
 * ========================================================================= */

int Prefs_LoadToEnv(const char *name)
{
    if (!name) return 0;

    char src[64];
    char dst[64];

    /* Build ENVARC:<name> and ENV:<name> paths */
    int si = 0;
    const char *p = "ENVARC:";
    while (*p && si < 60) src[si++] = *p++;
    p = name;
    while (*p && si < 62) src[si++] = *p++;
    src[si] = '\0';

    int di = 0;
    p = "ENV:";
    while (*p && di < 60) dst[di++] = *p++;
    p = name;
    while (*p && di < 62) dst[di++] = *p++;
    dst[di] = '\0';

    /* Load from ENVARC: */
    PrefsFile pf;
    if (!Prefs_Load(src, &pf)) return 0;

    /* Save to ENV: */
    return Prefs_Save(dst, &pf);
}

int Prefs_SaveToEnvarc(const char *name)
{
    if (!name) return 0;

    char src[64];
    char dst[64];

    /* Build ENV:<name> and ENVARC:<name> paths */
    int si = 0;
    const char *p = "ENV:";
    while (*p && si < 60) src[si++] = *p++;
    p = name;
    while (*p && si < 62) src[si++] = *p++;
    src[si] = '\0';

    int di = 0;
    p = "ENVARC:";
    while (*p && di < 60) dst[di++] = *p++;
    p = name;
    while (*p && di < 62) dst[di++] = *p++;
    dst[di] = '\0';

    /* Load from ENV: */
    PrefsFile pf;
    if (!Prefs_Load(src, &pf)) return 0;

    /* Save to ENVARC: */
    return Prefs_Save(dst, &pf);
}

/* =========================================================================
 * Prefs change notification
 * ========================================================================= */

/* Callback registered by subsystems that care about prefs changes */
typedef void (*PrefsNotifyCb)(uint8_t prefs_type);

#define MAX_NOTIFY_CBS 8
static PrefsNotifyCb g_notify_cbs[MAX_NOTIFY_CBS];
static int g_notify_cb_count = 0;

void Prefs_RegisterNotify(void (*cb)(uint8_t))
{
    if (g_notify_cb_count < MAX_NOTIFY_CBS)
        g_notify_cbs[g_notify_cb_count++] = cb;
}

void Prefs_NotifyChange(uint8_t prefs_type)
{
    for (int i = 0; i < g_notify_cb_count; i++)
        g_notify_cbs[i](prefs_type);
}
