/*
 * iffparse_lib.c — UAOS iffparse.library implementation (v39)
 *
 * Provides the AmigaOS iffparse.library API for M68k guests: FORM/LIST/CAT/
 * PROP parsing with SCAN/STEP/RAWSTEP modes, entry/exit chunk handlers,
 * stored properties, collections, local context items, and pluggable stream
 * handlers (DOS handle or caller-supplied hook).
 *
 * All guest-visible structures live in guest RAM so pointers handed to the
 * guest are real Amiga addresses:
 *
 *   IFFHandle (100 bytes):
 *     +0   iff_Stream        stream handle (DOS BPTR or user pointer)
 *     +4   iff_Flags         IFFF_* bits
 *     +8   iff_Depth         pushed context node count (0 = default node only)
 *     +12  iff_CNHead        MinList head — top of context stack
 *     +16  iff_CNTail        0 (MinList tail slot)
 *     +20  iff_CNTailPred    bottom of context stack (default node)
 *     +24  iff_CurrentState  parser FSM state
 *     +28  iff_StreamHook    stream handler: guest Hook* or builtin tag
 *     +32  iff_ScrCmd        12-byte IFFStreamCmd scratch
 *     +44  iff_ScrBuf        12-byte header scratch
 *     +56  iff_CmdLong       4-byte handler message scratch
 *     +60  iff_DefCN         embedded default IntContextNode (40 bytes)
 *
 *   IntContextNode (40 bytes):
 *     +0  succ, +4 pred, +8 ID, +12 TYPE, +16 SIZE, +20 SCAN,
 *     +24 LCIHEAD, +28/+32 unused, +36 COMPOSITE
 *
 *   IntLocalContextItem (36 bytes):
 *     +0  succ, +4 pred, +8 ID, +12 TYPE, +16 IDENT, +20 DATASIZE,
 *     +24 PURGEHOOK, +28 USERDATA, +32 USERDATASIZE
 *
 * The context stack and per-node LCI lists are singly linked through the
 * MinNode.succ field (NULL-terminated); guests only navigate them via
 * CurrentChunk()/ParentChunk().
 */

#include "rom_modules.h"
#include "../dos/handle_table.h"
#include "../dos/vfs.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>

extern uint8_t *g_ram;
extern void dos_AllocMem_glue(uint32_t size, uint32_t reqs, uint32_t *out_addr);
extern void dos_FreeMem_glue(uint32_t addr, uint32_t size);
extern uint32_t UAOS_InvokeM68kHook(uint32_t hook_ptr, uint32_t a0,
                                  uint32_t a1, uint32_t a2);

/* =========================================================================
 * Constants (iffparse.h / iff.h)
 * ========================================================================= */

#define ID_FORM        0x464F524DUL   /* "FORM" */
#define ID_LIST        0x4C495354UL   /* "LIST" */
#define ID_CAT         0x43415420UL   /* "CAT " */
#define ID_PROP        0x50524F50UL   /* "PROP" */

/* ParseIFF modes */
#define IFFPARSE_SCAN    0
#define IFFPARSE_STEP    1
#define IFFPARSE_RAWSTEP 2

/* OpenIFF modes / iff_Flags bits */
#define IFFF_READ        0x0000
#define IFFF_WRITE       0x0001
#define IFFF_RWBITS      (IFFF_READ | IFFF_WRITE)
#define IFFF_RSEEK       0x0200
#define IFFF_OPEN        0x0400
#define IFFF_FSEEK       0x0800
#define IFFF_NEWFILE     0x1000

#define IFFSIZE_UNKNOWN  0xFFFFFFFFUL

/* Errors (negative) */
#define IFFERR_EOF         -2
#define IFFERR_EOC         -3
#define IFFERR_NOSCOPE     -4
#define IFFERR_NOMEM       -5
#define IFFERR_READ        -6
#define IFFERR_WRITE       -7
#define IFFERR_SEEK        -8
#define IFFERR_MANGLED     -9
#define IFFERR_SYNTAX     -10
#define IFFERR_NOTIFF     -11
#define IFFERR_NOHOOK     -12
#define IFFERR_CLIPBOARD  -17
#define IFFERR_BADTASK    -18
#define IFFERR_ABORT      -19
#define IFFERR_NOTAPPLIC  -20

#define IFF_RETURN2CLIENT  512

/* LCI idents */
#define IFFLCI_PROP         0x70726F70UL  /* 'prop' */
#define IFFLCI_COLLECTION   0x636F6C6CUL  /* 'coll' */
#define IFFLCI_ENTRYHANDLER 0x656E6864UL  /* 'enhd' */
#define IFFLCI_EXITHANDLER  0x65786864UL  /* 'exhd' */

/* Stream commands */
#define IFFCMD_INIT     0
#define IFFCMD_CLEANUP  1
#define IFFCMD_READ     2
#define IFFCMD_WRITE    3
#define IFFCMD_SEEK     4
#define IFFCMD_ENTRY    5
#define IFFCMD_EXIT     6
#define IFFCMD_PURGELCI 7

/* StoreLocalItem positions */
#define IFFSLI_ROOT  1
#define IFFSLI_TOP   2
#define IFFSLI_PROP  3

/* Parser FSM states */
#define IFFSTATE_COMPOSITE 0
#define IFFSTATE_PUSHCHUNK 1
#define IFFSTATE_ATOMIC    2
#define IFFSTATE_SCANEXIT  3
#define IFFSTATE_EXIT      4
#define IFFSTATE_POPCHUNK  5

/* Builtin "hook" tags stored where a guest Hook* would go.  Small values
 * can never be real guest pointers, so iff_call_hook() dispatches them to
 * native implementations instead of invoking guest code. */
#define HOOKTAG_DOSSTREAM  1
#define HOOKTAG_PROP       2
#define HOOKTAG_COLL       3
#define HOOKTAG_STOP       4
#define HOOKTAG_EXITCTX    5
#define HOOKTAG_PROPPURGE  6
#define HOOKTAG_COLLPURGE  7
#define HOOKTAG_CLIP       8

/* =========================================================================
 * Guest structure offsets
 * ========================================================================= */

/* IFFHandle */
#define IFF_STREAM     0
#define IFF_FLAGS      4
#define IFF_DEPTH      8
#define IFF_CNHEAD     12
#define IFF_CNTAILPRED 20
#define IFF_CURSTATE   24
#define IFF_STREAMHOOK 28
#define IFF_SCRCMD     32
#define IFF_SCRBUF     44
#define IFF_CMDLONG    56
#define IFF_DEFCN      60
#define IFFHANDLE_SIZE 100

/* IntContextNode */
#define CN_SUCC       0
#define CN_PRED       4
#define CN_ID         8
#define CN_TYPE       12
#define CN_SIZE       16
#define CN_SCAN       20
#define CN_LCIHEAD    24
#define CN_COMPOSITE  36
#define CNODE_SIZE    40

/* IntLocalContextItem */
#define LCI_SUCC        0
#define LCI_ID          8
#define LCI_TYPE        12
#define LCI_IDENT       16
#define LCI_DATASIZE    20
#define LCI_PURGEHOOK   24
#define LCI_USERDATA    28
#define LCI_USERDATASIZE 32
#define LCI_SIZE        36

/* StoredProperty: +0 sp_Size, +4 sp_Data        (8 bytes)  */
/* CollectionItem: +0 ci_Next, +4 ci_Size, +8 ci_Data (12)   */
/* HandlerInfo:    +0 hi_Hook, +4 hi_Object      (8 bytes)  */
/* CIPtr:          +0 FirstCI                    (4 bytes)  */

/* =========================================================================
 * Guest memory accessors (big-endian)
 * ========================================================================= */

#define GUEST_RAM_SIZE (16 * 1024 * 1024)

static int iff_gok(uint32_t addr, uint32_t len)
{
    return addr < GUEST_RAM_SIZE && len <= GUEST_RAM_SIZE - addr;
}

static uint32_t gr32(uint32_t addr)
{
    if (!iff_gok(addr, 4)) return 0;
    return ((uint32_t)g_ram[addr]     << 24) | ((uint32_t)g_ram[addr + 1] << 16) |
           ((uint32_t)g_ram[addr + 2] <<  8) |  (uint32_t)g_ram[addr + 3];
}

static void gw32(uint32_t addr, uint32_t val)
{
    if (!iff_gok(addr, 4)) return;
    g_ram[addr]     = (uint8_t)(val >> 24);
    g_ram[addr + 1] = (uint8_t)(val >> 16);
    g_ram[addr + 2] = (uint8_t)(val >> 8);
    g_ram[addr + 3] = (uint8_t)val;
}

/* =========================================================================
 * Small helpers
 * ========================================================================= */

static int iff_GoodID(uint32_t id)
{
    /* Leading space only valid if all four bytes are spaces */
    if (((id >> 24) == 0x20) && (id != 0x20202020UL)) return 0;
    for (int i = 0; i < 4; i++) {
        uint8_t c = (uint8_t)(id >> (24 - i * 8));
        if (c < 0x20 || c > 0x7E) return 0;
    }
    return 1;
}

static int iff_GoodType(uint32_t type)
{
    if (!iff_GoodID(type)) return 0;
    for (int i = 0; i < 4; i++) {
        uint8_t c = (uint8_t)(type >> (24 - i * 8));
        if (c > 'Z') return 0;
        if (c < 'A' && !(c >= '0' && c <= '9') && c != ' ') return 0;
    }
    return 1;
}

static uint32_t iff_alloc(uint32_t size)
{
    uint32_t addr = 0;
    dos_AllocMem_glue(size, 0, &addr);
    if (addr && iff_gok(addr, size))
        memset(g_ram + addr, 0, size);
    return addr;
}

static void iff_free(uint32_t addr, uint32_t size)
{
    if (addr) dos_FreeMem_glue(addr, size);
}

static int iff_is_composite(uint32_t id)
{
    return id == ID_FORM || id == ID_LIST || id == ID_CAT || id == ID_PROP;
}

/* =========================================================================
 * Context stack / LCI helpers
 * ========================================================================= */

static uint32_t iff_TopChunk(uint32_t iff)  { return gr32(iff + IFF_CNHEAD); }
static uint32_t iff_RootChunk(uint32_t iff) { return iff + IFF_DEFCN; }

/* Link CN at head of iff's context stack (succ = next-deeper node). */
static void iff_cn_link(uint32_t iff, uint32_t cn)
{
    uint32_t old = gr32(iff + IFF_CNHEAD);
    gw32(cn + CN_SUCC, old);
    gw32(cn + CN_PRED, 0);
    if (old) gw32(old + CN_PRED, cn);
    gw32(iff + IFF_CNHEAD, cn);
    gw32(iff + IFF_CNTAILPRED, iff_RootChunk(iff));
}

static void iff_cn_unlink(uint32_t iff, uint32_t cn)
{
    uint32_t succ = gr32(cn + CN_SUCC);
    if (succ) gw32(succ + CN_PRED, 0);
    gw32(iff + IFF_CNHEAD, succ);
}

/* -----------------------------------------------------------------------
 * Hooks — builtin tags dispatch to native code; anything else is a guest
 * Hook* invoked through the m68k hook trampoline.
 * (proto: CallHookPkt — A0=hook, A1=msgptr, A2=object)
 * ----------------------------------------------------------------------- */
static int32_t iff_purge_prop(uint32_t iff, uint32_t lci);
static int32_t iff_purge_coll(uint32_t iff, uint32_t lci);
static int32_t iff_hook_prop(uint32_t iff);
static int32_t iff_hook_coll(uint32_t iff);
static void    iff_FreeLocalItem(uint32_t lci);

static int32_t iff_call_hook(uint32_t iff, uint32_t hook, uint32_t object,
                             int32_t command)
{
    switch (hook) {
    case HOOKTAG_STOP:      return IFF_RETURN2CLIENT;
    case HOOKTAG_EXITCTX:   return IFFERR_EOC;
    case HOOKTAG_PROP:      return iff_hook_prop(iff);
    case HOOKTAG_COLL:      return iff_hook_coll(iff);
    case HOOKTAG_PROPPURGE: return iff_purge_prop(iff, object);
    case HOOKTAG_COLLPURGE: return iff_purge_coll(iff, object);
    default:
        if (hook >= 0x100 && iff_gok(hook, 20)) {
            gw32(iff + IFF_CMDLONG, (uint32_t)command);
            return (int32_t)UAOS_InvokeM68kHook(hook, hook,
                                              iff + IFF_CMDLONG, object);
        }
        return IFFERR_NOHOOK;
    }
}

/* =========================================================================
 * Local context items
 * ========================================================================= */

static uint32_t iff_AllocLocalItem(uint32_t type, uint32_t id,
                                   uint32_t ident, uint32_t dataSize)
{
    uint32_t lci = iff_alloc(LCI_SIZE);
    if (!lci) return 0;
    gw32(lci + LCI_ID, id);
    gw32(lci + LCI_TYPE, type);
    gw32(lci + LCI_IDENT, ident);
    if (dataSize > 0) {
        uint32_t data = iff_alloc(dataSize);
        if (!data) { iff_free(lci, LCI_SIZE); return 0; }
        gw32(lci + LCI_USERDATA, data);
        gw32(lci + LCI_USERDATASIZE, dataSize);
        gw32(lci + LCI_DATASIZE, dataSize);
    }
    return lci;
}

static void iff_FreeLocalItem(uint32_t lci)
{
    if (!lci) return;
    iff_free(gr32(lci + LCI_USERDATA), gr32(lci + LCI_USERDATASIZE));
    iff_free(lci, LCI_SIZE);
}

/* Unlink LCI from its CN's list, then run its purge hook (or free it). */
static void iff_PurgeLCI(uint32_t iff, uint32_t cn, uint32_t lci)
{
    /* unlink from singly-linked list */
    uint32_t prev = 0, node = gr32(cn + CN_LCIHEAD);
    while (node) {
        if (node == lci) {
            if (prev) gw32(prev + LCI_SUCC, gr32(lci + LCI_SUCC));
            else      gw32(cn + CN_LCIHEAD, gr32(lci + LCI_SUCC));
            break;
        }
        prev = node;
        node = gr32(node + LCI_SUCC);
    }
    uint32_t hook = gr32(lci + LCI_PURGEHOOK);
    if (hook)
        iff_call_hook(iff, hook, lci, IFFCMD_PURGELCI);
    else
        iff_FreeLocalItem(lci);
}

static int32_t iff_purge_prop(uint32_t iff, uint32_t lci)
{
    uint32_t sp = gr32(lci + LCI_USERDATA);
    if (sp) iff_free(gr32(sp + 4), gr32(sp + 0));   /* sp_Data, sp_Size */
    iff_FreeLocalItem(lci);
    (void)iff;
    return 0;
}

static int32_t iff_purge_coll(uint32_t iff, uint32_t lci)
{
    uint32_t ciptr = gr32(lci + LCI_USERDATA);
    uint32_t ci = ciptr ? gr32(ciptr) : 0;
    while (ci) {
        uint32_t next = gr32(ci + 0);
        iff_free(gr32(ci + 8), gr32(ci + 4));       /* ci_Data, ci_Size */
        iff_free(ci, 12);
        ci = next;
    }
    iff_FreeLocalItem(lci);
    (void)iff;
    return 0;
}

static void iff_StoreItemInContext(uint32_t iff, uint32_t lci, uint32_t cn)
{
    /* purge existing items with same type/id/ident, then AddHead */
    uint32_t node = gr32(cn + CN_LCIHEAD);
    while (node) {
        uint32_t next = gr32(node + LCI_SUCC);
        if (gr32(node + LCI_ID)    == gr32(lci + LCI_ID)    &&
            gr32(node + LCI_TYPE)  == gr32(lci + LCI_TYPE)  &&
            gr32(node + LCI_IDENT) == gr32(lci + LCI_IDENT))
            iff_PurgeLCI(iff, cn, node);
        node = next;
    }
    gw32(lci + LCI_SUCC, gr32(cn + CN_LCIHEAD));
    gw32(cn + CN_LCIHEAD, lci);
}

static uint32_t iff_FindLocalItem(uint32_t iff, uint32_t type, uint32_t id,
                                  uint32_t ident)
{
    uint32_t cn = iff_TopChunk(iff);
    while (cn) {
        uint32_t lci = gr32(cn + CN_LCIHEAD);
        while (lci) {
            if (gr32(lci + LCI_TYPE)  == type  &&
                gr32(lci + LCI_ID)    == id    &&
                gr32(lci + LCI_IDENT) == ident)
                return lci;
            lci = gr32(lci + LCI_SUCC);
        }
        cn = gr32(cn + CN_SUCC);
    }
    return 0;
}

static uint32_t iff_FindPropContext(uint32_t iff)
{
    uint32_t cn = iff_TopChunk(iff);
    while (cn) {
        uint32_t id = gr32(cn + CN_ID);
        if (id == ID_FORM || id == ID_LIST) return cn;
        cn = gr32(cn + CN_SUCC);
    }
    return 0;
}

static int32_t iff_StoreLocalItem(uint32_t iff, uint32_t lci, int32_t position)
{
    uint32_t cn;
    switch (position) {
    case IFFSLI_ROOT: cn = iff_RootChunk(iff);       break;
    case IFFSLI_PROP: cn = iff_FindPropContext(iff); break;
    default:          cn = iff_TopChunk(iff);        break;
    }
    if (!cn) return IFFERR_NOSCOPE;
    iff_StoreItemInContext(iff, lci, cn);
    return 0;
}

/* =========================================================================
 * Context node push/pop
 * ========================================================================= */

static int32_t iff_PushContextNode(uint32_t iff, uint32_t type, uint32_t id,
                                   uint32_t size, uint32_t scan)
{
    int composite = iff_is_composite(id);
    if (!composite) {
        /* leaf chunks inherit their scope type from the containing node */
        type = gr32(iff_TopChunk(iff) + CN_TYPE);
    }
    if (!iff_GoodType(type) || !iff_GoodID(id))
        return IFFERR_MANGLED;

    uint32_t cn = iff_alloc(CNODE_SIZE);
    if (!cn) return IFFERR_NOMEM;

    gw32(cn + CN_ID, id);
    gw32(cn + CN_TYPE, type);
    gw32(cn + CN_SIZE, size);
    gw32(cn + CN_SCAN, scan);
    gw32(cn + CN_COMPOSITE, composite);
    iff_cn_link(iff, cn);
    gw32(iff + IFF_DEPTH, gr32(iff + IFF_DEPTH) + 1);
    return 0;
}

static void iff_PopContextNode(uint32_t iff)
{
    uint32_t cn = iff_TopChunk(iff);
    if (!cn || cn == iff_RootChunk(iff)) return;

    /* purge every LCI attached to this node */
    while (gr32(cn + CN_LCIHEAD))
        iff_PurgeLCI(iff, cn, gr32(cn + CN_LCIHEAD));

    iff_cn_unlink(iff, cn);
    iff_free(cn, CNODE_SIZE);
    gw32(iff + IFF_DEPTH, gr32(iff + IFF_DEPTH) - 1);
}

/* =========================================================================
 * Stream operations
 * ========================================================================= */

static int32_t iff_dos_stream(uint32_t iff, int32_t command,
                              uint32_t buf, int32_t nbytes)
{
    VfsFile *f = HandleTable_GetFile(gr32(iff + IFF_STREAM));
    if (!f) return IFFERR_NOTIFF;

    switch (command) {
    case IFFCMD_READ:
        if (buf && !iff_gok(buf, (uint32_t)nbytes)) return IFFERR_READ;
        return (int32_t)VFS_Read(f, g_ram + buf, (uint32_t)nbytes) == nbytes
               ? 0 : IFFERR_READ;
    case IFFCMD_WRITE:
        if (buf && !iff_gok(buf, (uint32_t)nbytes)) return IFFERR_WRITE;
        return (int32_t)VFS_Write(f, g_ram + buf, (uint32_t)nbytes) == nbytes
               ? 0 : IFFERR_WRITE;
    case IFFCMD_SEEK: {
        uint32_t np = (uint32_t)((int32_t)f->pos + nbytes);
        VFS_Seek(f, np);
        return 0;   /* short/clamped seeks surface on the next read */
    }
    case IFFCMD_INIT:    return 0;
    case IFFCMD_CLEANUP: VFS_Seek(f, 0); return 0;
    }
    return IFFERR_NOTAPPLIC;
}

static int32_t iff_stream_cmd(uint32_t iff, int32_t command,
                              uint32_t buf, int32_t nbytes)
{
    uint32_t hook = gr32(iff + IFF_STREAMHOOK);
    if (hook == HOOKTAG_DOSSTREAM)
        return iff_dos_stream(iff, command, buf, nbytes);
    if (hook == HOOKTAG_CLIP)
        return IFFERR_CLIPBOARD;
    if (hook >= 0x100 && iff_gok(hook, 20)) {
        uint32_t cmd = iff + IFF_SCRCMD;
        gw32(cmd + 0, (uint32_t)command);
        gw32(cmd + 4, buf);
        gw32(cmd + 8, (uint32_t)nbytes);
        return (int32_t)UAOS_InvokeM68kHook(hook, hook, cmd, iff);
    }
    return IFFERR_NOHOOK;
}

/* Read up to nbytes; returns byte count, or negative IFFERR_READ. */
static int32_t iff_ReadStream(uint32_t iff, uint32_t buf, int32_t nbytes)
{
    if (nbytes <= 0) return 0;
    return iff_stream_cmd(iff, IFFCMD_READ, buf, nbytes) ? IFFERR_READ : nbytes;
}

static int32_t iff_WriteStream(uint32_t iff, uint32_t buf, int32_t nbytes)
{
    if (nbytes <= 0) return 0;
    return iff_stream_cmd(iff, IFFCMD_WRITE, buf, nbytes) ? IFFERR_WRITE : nbytes;
}

static int32_t iff_SeekStream(uint32_t iff, int32_t offset)
{
    uint32_t flags = gr32(iff + IFF_FLAGS);
    if (offset == 0) return 0;
    if (offset > 0 && !(flags & (IFFF_RSEEK | IFFF_FSEEK))) {
        /* forward-only stream: emulate seek by reading into a scratch buf */
        static const uint32_t SEEKBUFSIZE = 1024;
        uint32_t seekbuf = iff_alloc(SEEKBUFSIZE);
        if (!seekbuf) return IFFERR_NOMEM;
        int32_t err = 0;
        while (offset > 0) {
            int32_t n = offset > (int32_t)SEEKBUFSIZE
                        ? (int32_t)SEEKBUFSIZE : offset;
            if (iff_ReadStream(iff, seekbuf, n) != n) { err = IFFERR_SEEK; break; }
            offset -= n;
        }
        iff_free(seekbuf, SEEKBUFSIZE);
        return err;
    }
    return iff_stream_cmd(iff, IFFCMD_SEEK, 0, offset)
           ? IFFERR_SEEK : 0;
}

/* Read one big-endian LONG; returns 4 or a negative IFFERR. */
static int32_t iff_ReadStreamLong(uint32_t iff, uint32_t *out)
{
    int32_t n = iff_ReadStream(iff, iff + IFF_SCRBUF, 4);
    if (n < 0)  return n;
    if (n != 4) return IFFERR_EOF;
    *out = gr32(iff + IFF_SCRBUF);
    return 4;
}

static int32_t iff_WriteStreamLong(uint32_t iff, uint32_t val)
{
    gw32(iff + IFF_SCRBUF, val);
    return iff_WriteStream(iff, iff + IFF_SCRBUF, 4);
}

/* =========================================================================
 * Chunk header read (read mode)
 * ========================================================================= */

static int32_t iff_GetChunkHeader(uint32_t iff)
{
    uint32_t id = 0, size = 0, type = 0, scan = 0;
    int32_t n;

    if ((n = iff_ReadStreamLong(iff, &id))   < 0) return n;
    if ((n = iff_ReadStreamLong(iff, &size)) < 0) return n;
    if (iff_is_composite(id)) {
        if ((n = iff_ReadStreamLong(iff, &type)) < 0) return n;
        scan = 4;   /* the type longword counts into the chunk data */
    }
    return iff_PushContextNode(iff, type, id, size, scan);
}

/* =========================================================================
 * Handler invocation
 * ========================================================================= */

static int32_t iff_InvokeHandlers(uint32_t iff, int32_t mode, uint32_t ident)
{
    int32_t stepping_retval =
        (ident == IFFLCI_ENTRYHANDLER) ? IFF_RETURN2CLIENT : IFFERR_EOC;

    if (mode == IFFPARSE_RAWSTEP)
        return stepping_retval;

    uint32_t cn  = iff_TopChunk(iff);
    uint32_t lci = iff_FindLocalItem(iff, gr32(cn + CN_TYPE),
                                     gr32(cn + CN_ID), ident);
    if (lci) {
        uint32_t hi   = gr32(lci + LCI_USERDATA);
        uint32_t hook = hi ? gr32(hi + 0) : 0;
        if (!hook) return IFFERR_NOHOOK;
        int32_t err = iff_call_hook(iff, hook, gr32(hi + 4),
                                    ident == IFFLCI_ENTRYHANDLER
                                    ? IFFCMD_ENTRY : IFFCMD_EXIT);
        if (err) return err;
    }
    if (mode == IFFPARSE_STEP)
        return stepping_retval;
    return 0;
}

/* =========================================================================
 * Builtin entry-handler payloads (properties / collections)
 * ========================================================================= */

/* Entry handler behind PropChunk(): reads the whole top chunk into a
 * StoredProperty and stores its LCI in the enclosing FORM/LIST scope so
 * FindProp() sees it for the duration of that context. */
static int32_t iff_hook_prop(uint32_t iff)
{
    uint32_t cn   = iff_TopChunk(iff);
    uint32_t size = gr32(cn + CN_SIZE);
    uint32_t buf  = 0;
    int32_t  got, err;

    uint32_t lci = iff_AllocLocalItem(gr32(cn + CN_TYPE), gr32(cn + CN_ID),
                                      IFFLCI_PROP, 8 /* StoredProperty */);
    if (!lci) return IFFERR_NOMEM;
    uint32_t sp = gr32(lci + LCI_USERDATA);

    if (size) {
        buf = iff_alloc(size);
        if (!buf) { iff_FreeLocalItem(lci); return IFFERR_NOMEM; }
        /* ReadChunkBytes semantics: clamp to chunk-remaining */
        int32_t left = (int32_t)(size - gr32(cn + CN_SCAN));
        if (left < 0) left = 0;
        got = iff_ReadStream(iff, buf, left < (int32_t)size
                             ? left : (int32_t)size);
        if (got != (int32_t)size) {
            iff_free(buf, size);
            iff_FreeLocalItem(lci);
            return got < 0 ? got : IFFERR_MANGLED;
        }
        gw32(cn + CN_SCAN, gr32(cn + CN_SCAN) + (uint32_t)got);
    }
    gw32(sp + 0, size);
    gw32(sp + 4, buf);

    err = iff_StoreLocalItem(iff, lci, IFFSLI_PROP);
    if (err) {
        iff_free(buf, size);
        iff_FreeLocalItem(lci);
        return err;
    }
    gw32(lci + LCI_PURGEHOOK, HOOKTAG_PROPPURGE);
    return 0;
}

/* Entry handler behind CollectionChunk(): like PropFunc but appends a
 * CollectionItem to a shared per-scope list (newest first). */
static int32_t iff_hook_coll(uint32_t iff)
{
    uint32_t cn   = iff_TopChunk(iff);
    uint32_t type = gr32(cn + CN_TYPE), id = gr32(cn + CN_ID);
    uint32_t size = gr32(cn + CN_SIZE);
    int32_t  err;

    /* reuse the existing collection LCI for this scope if present */
    uint32_t lci = iff_FindLocalItem(iff, type, id, IFFLCI_COLLECTION);
    if (!lci) {
        lci = iff_AllocLocalItem(type, id, IFFLCI_COLLECTION,
                                 4 /* CIPtr */);
        if (!lci) return IFFERR_NOMEM;
        err = iff_StoreLocalItem(iff, lci, IFFSLI_PROP);
        if (err) { iff_FreeLocalItem(lci); return err; }
        gw32(lci + LCI_PURGEHOOK, HOOKTAG_COLLPURGE);
    }
    uint32_t ciptr = gr32(lci + LCI_USERDATA);

    uint32_t ci = iff_alloc(12);
    if (!ci) return IFFERR_NOMEM;
    uint32_t buf = 0;
    int32_t got = 0;
    if (size) {
        buf = iff_alloc(size);
        if (!buf) { iff_free(ci, 12); return IFFERR_NOMEM; }
        int32_t left = (int32_t)(size - gr32(cn + CN_SCAN));
        if (left < 0) left = 0;
        got = iff_ReadStream(iff, buf, left < (int32_t)size
                             ? left : (int32_t)size);
        if (got != (int32_t)size) {
            iff_free(buf, size);
            iff_free(ci, 12);
            return got < 0 ? got : IFFERR_MANGLED;
        }
        gw32(cn + CN_SCAN, gr32(cn + CN_SCAN) + (uint32_t)got);
    }
    /* prepend to the scope's collection list */
    gw32(ci + 0, gr32(ciptr));
    gw32(ci + 4, size);
    gw32(ci + 8, buf);
    gw32(ciptr, ci);
    return 0;
}

/* =========================================================================
 * ParseIFF FSM
 * ========================================================================= */

static int32_t iff_ParseIFF_run(uint32_t iff, int32_t mode)
{
    int32_t err = 0;
    for (;;) {
        uint32_t cn;
        switch (gr32(iff + IFF_CURSTATE)) {

        case IFFSTATE_COMPOSITE:
            /* Entered a FORM/LIST/CAT/PROP; expect children next. */
            gw32(iff + IFF_CURSTATE, IFFSTATE_PUSHCHUNK);
            err = iff_InvokeHandlers(iff, mode, IFFLCI_ENTRYHANDLER);
            if (err) {
                if (err == IFF_RETURN2CLIENT) err = 0;
                return err;
            }
            break;

        case IFFSTATE_PUSHCHUNK:
            err = iff_GetChunkHeader(iff);
            if (err) return err;
            cn = iff_TopChunk(iff);
            gw32(iff + IFF_CURSTATE,
                 gr32(cn + CN_COMPOSITE) ? IFFSTATE_COMPOSITE : IFFSTATE_ATOMIC);
            break;

        case IFFSTATE_ATOMIC:
            gw32(iff + IFF_CURSTATE, IFFSTATE_SCANEXIT);
            err = iff_InvokeHandlers(iff, mode, IFFLCI_ENTRYHANDLER);
            if (err) {
                if (err == IFF_RETURN2CLIENT) err = 0;
                return err;
            }
            break;

        case IFFSTATE_SCANEXIT:
            gw32(iff + IFF_CURSTATE, IFFSTATE_EXIT);
            if (mode == IFFPARSE_SCAN) {
                cn = iff_TopChunk(iff);
                int32_t toseek = (int32_t)(gr32(cn + CN_SIZE) - gr32(cn + CN_SCAN));
                if (gr32(cn + CN_SIZE) & 1) toseek++;
                if (toseek > 0) {
                    err = iff_SeekStream(iff, toseek);
                    if (err) return err;
                    gw32(cn + CN_SCAN, gr32(cn + CN_SCAN) + (uint32_t)toseek);
                }
            }
            break;

        case IFFSTATE_EXIT:
            gw32(iff + IFF_CURSTATE, IFFSTATE_POPCHUNK);
            err = iff_InvokeHandlers(iff, mode, IFFLCI_EXITHANDLER);
            if (err) {
                if (err == IFF_RETURN2CLIENT) err = 0;
                return err;
            }
            break;

        case IFFSTATE_POPCHUNK: {
            cn = iff_TopChunk(iff);
            uint32_t size = gr32(cn + CN_SIZE);
            if (size & 1) size++;
            /* In STEP/RAWSTEP the SCANEXIT skip was deferred — do it now so
             * the stream lands at the next chunk boundary. */
            if (!gr32(cn + CN_COMPOSITE)) {
                int32_t toseek = (int32_t)(size - gr32(cn + CN_SCAN));
                if (toseek > 0) {
                    err = iff_SeekStream(iff, toseek);
                    if (err) return err;
                }
            }
            iff_PopContextNode(iff);
            cn = iff_TopChunk(iff);
            /* footprint of a child within its parent: 8-byte header +
             * aligned ckData (composite ckSize already counts the type) */
            if (cn) gw32(cn + CN_SCAN, gr32(cn + CN_SCAN) + size + 8);
            if (cn && gr32(cn + CN_SCAN) < gr32(cn + CN_SIZE)) {
                gw32(iff + IFF_CURSTATE, IFFSTATE_PUSHCHUNK);
            } else {
                if (!gr32(iff + IFF_DEPTH)) return IFFERR_EOF;
                gw32(iff + IFF_CURSTATE, IFFSTATE_EXIT);
            }
            break;
        }
        }
    }
}

/* =========================================================================
 * Install a chunk handler LCI (entry or exit)
 * ========================================================================= */

static int32_t iff_InstallHandler(uint32_t iff, uint32_t type, uint32_t id,
                                  int32_t position, uint32_t hook_or_tag,
                                  uint32_t object, uint32_t ident)
{
    uint32_t lci = iff_AllocLocalItem(type, id, ident, 8);
    if (!lci) return IFFERR_NOMEM;
    uint32_t hi = gr32(lci + LCI_USERDATA);
    gw32(hi + 0, hook_or_tag);
    gw32(hi + 4, object);
    int32_t err = iff_StoreLocalItem(iff, lci, position);
    if (err) iff_FreeLocalItem(lci);
    return err;
}

/* =========================================================================
 * Exported functions — M68kCPUState stubs
 * ========================================================================= */

static int32_t iff_PopChunk_impl(uint32_t iff);

/* 1. AllocIFF() */
static void iff_fn_AllocIFF(M68kCPUState *cpu)
{
    uint32_t iff = iff_alloc(IFFHANDLE_SIZE);
    if (iff) {
        uint32_t defcn = iff + IFF_DEFCN;
        gw32(iff + IFF_FLAGS, IFFF_READ);
        gw32(iff + IFF_CNHEAD, defcn);
        gw32(iff + IFF_CNTAILPRED, defcn);
        gw32(iff + IFF_CURSTATE, IFFSTATE_PUSHCHUNK);
        gw32(iff + IFF_DEPTH, 0);
    }
    cpu->d[0] = iff;
}

/* 2. OpenIFF(iff=a0, rwMode=d0) */
static void iff_fn_OpenIFF(M68kCPUState *cpu)
{
    uint32_t iff = cpu->a[0];
    uint32_t rwmode = cpu->d[0] & IFFF_WRITE;
    int32_t err = 0;

    if (!iff) { cpu->d[0] = IFFERR_NOMEM; return; }
    if (!gr32(iff + IFF_STREAMHOOK)) { cpu->d[0] = IFFERR_NOHOOK; return; }

    err = iff_stream_cmd(iff, IFFCMD_INIT, 0, 0);
    if (!err) {
        if (rwmode == IFFF_READ && gr32(iff + IFF_STREAM)) {
            /* Validate: first chunk must be a composite IFF form */
            err = iff_GetChunkHeader(iff);
            if (!err) {
                gw32(iff + IFF_CURSTATE, IFFSTATE_COMPOSITE);
                if (gr32(iff_TopChunk(iff) + CN_COMPOSITE)) {
                    uint32_t f = gr32(iff + IFF_FLAGS);
                    f = (f & ~IFFF_RWBITS) | rwmode | IFFF_OPEN | IFFF_NEWFILE;
                    gw32(iff + IFF_FLAGS, f);
                } else {
                    err = IFFERR_NOTIFF;
                    iff_PopContextNode(iff);
                }
            } else {
                if (err == IFFERR_MANGLED) err = IFFERR_NOTIFF;
                iff_stream_cmd(iff, IFFCMD_CLEANUP, 0, 0);
            }
        } else {
            uint32_t f = gr32(iff + IFF_FLAGS);
            f = (f & ~IFFF_RWBITS) | rwmode | IFFF_OPEN | IFFF_NEWFILE;
            gw32(iff + IFF_FLAGS, f);
        }
    }
    cpu->d[0] = (uint32_t)err;
}

/* 3. ParseIFF(iff=a0, mode=d0) */
static void iff_fn_ParseIFF(M68kCPUState *cpu)
{
    uint32_t iff = cpu->a[0];
    int32_t mode = (int32_t)cpu->d[0];
    if (!iff || !(gr32(iff + IFF_FLAGS) & IFFF_OPEN)) {
        cpu->d[0] = (uint32_t)IFFERR_NOTIFF;
        return;
    }
    cpu->d[0] = (uint32_t)iff_ParseIFF_run(iff, mode);
}

/* 4. CloseIFF(iff=a0) */
static void iff_fn_CloseIFF(M68kCPUState *cpu)
{
    uint32_t iff = cpu->a[0];
    if (!iff || !(gr32(iff + IFF_FLAGS) & IFFF_OPEN)) return;
    gw32(iff + IFF_FLAGS, gr32(iff + IFF_FLAGS) & ~IFFF_OPEN);

    /* pop every pushed context node (PopChunk also finalizes writes) */
    while (gr32(iff + IFF_DEPTH))
        if (iff_PopChunk_impl(iff)) break;

    iff_stream_cmd(iff, IFFCMD_CLEANUP, 0, 0);
}

/* 5. FreeIFF(iff=a0) */
static void iff_fn_FreeIFF(M68kCPUState *cpu)
{
    uint32_t iff = cpu->a[0];
    if (!iff) return;
    /* safety: pop any remaining context nodes */
    while (gr32(iff + IFF_DEPTH))
        iff_PopContextNode(iff);
    /* purge LCIs on the default context node (handlers, props, ...) */
    uint32_t defcn = iff + IFF_DEFCN;
    while (gr32(defcn + CN_LCIHEAD))
        iff_PurgeLCI(iff, defcn, gr32(defcn + CN_LCIHEAD));
    iff_free(iff, IFFHANDLE_SIZE);
}

/* 6. ReadChunkBytes(iff=a0, buf=a1, numBytes=d0) */
static void iff_fn_ReadChunkBytes(M68kCPUState *cpu)
{
    uint32_t iff = cpu->a[0];
    uint32_t buf = cpu->a[1];
    int32_t  n   = (int32_t)cpu->d[0];
    uint32_t cn  = iff_TopChunk(iff);
    int32_t  left = (int32_t)(gr32(cn + CN_SIZE) - gr32(cn + CN_SCAN));
    int32_t  got;

    if (n > left) n = left;
    got = iff_ReadStream(iff, buf, n);
    if (got > 0) gw32(cn + CN_SCAN, gr32(cn + CN_SCAN) + (uint32_t)got);
    cpu->d[0] = (uint32_t)got;
}

/* 7. WriteChunkBytes(iff=a0, buf=a1, numBytes=d0) */
static void iff_fn_WriteChunkBytes(M68kCPUState *cpu)
{
    uint32_t iff = cpu->a[0];
    uint32_t buf = cpu->a[1];
    int32_t  n   = (int32_t)cpu->d[0];
    uint32_t cn  = iff_TopChunk(iff);
    int32_t  wrote;

    if (gr32(cn + CN_SIZE) != IFFSIZE_UNKNOWN) {
        int32_t left = (int32_t)(gr32(cn + CN_SIZE) - gr32(cn + CN_SCAN));
        if (n > left) n = left;
    }
    wrote = iff_WriteStream(iff, buf, n);
    if (wrote > 0) gw32(cn + CN_SCAN, gr32(cn + CN_SCAN) + (uint32_t)wrote);
    cpu->d[0] = (uint32_t)wrote;
}

/* 8. ReadChunkRecords(iff=a0, buf=a1, recSize=d0, numRecs=d1) */
static void iff_fn_ReadChunkRecords(M68kCPUState *cpu)
{
    uint32_t iff = cpu->a[0];
    uint32_t buf = cpu->a[1];
    uint32_t recsize = cpu->d[0];
    uint32_t numrec  = cpu->d[1];
    uint32_t cn  = iff_TopChunk(iff);
    int32_t  left = (int32_t)(gr32(cn + CN_SIZE) - gr32(cn + CN_SCAN));
    int32_t  total, got;

    if (!recsize) { cpu->d[0] = 0; return; }
    total = (int32_t)(recsize * numrec);
    if (total > left)
        total = (int32_t)((uint32_t)left - ((uint32_t)left % recsize));
    got = iff_ReadStream(iff, buf, total);
    if (got > 0) {
        gw32(cn + CN_SCAN, gr32(cn + CN_SCAN) + (uint32_t)got);
        cpu->d[0] = (uint32_t)got / recsize;
    } else {
        cpu->d[0] = (uint32_t)got;
    }
}

/* 9. WriteChunkRecords(iff=a0, buf=a1, recSize=d0, numRecs=d1) */
static void iff_fn_WriteChunkRecords(M68kCPUState *cpu)
{
    uint32_t iff = cpu->a[0];
    uint32_t buf = cpu->a[1];
    uint32_t recsize = cpu->d[0];
    uint32_t numrec  = cpu->d[1];
    uint32_t cn  = iff_TopChunk(iff);
    int32_t  total, wrote;

    if (!recsize) { cpu->d[0] = 0; return; }
    total = (int32_t)(recsize * numrec);
    if (gr32(cn + CN_SIZE) != IFFSIZE_UNKNOWN) {
        int32_t left = (int32_t)(gr32(cn + CN_SIZE) - gr32(cn + CN_SCAN));
        if (total > left)
            total = (int32_t)((uint32_t)left - ((uint32_t)left % recsize));
    }
    wrote = iff_WriteStream(iff, buf, total);
    if (wrote > 0) {
        gw32(cn + CN_SCAN, gr32(cn + CN_SCAN) + (uint32_t)wrote);
        cpu->d[0] = (uint32_t)wrote / recsize;
    } else {
        cpu->d[0] = (uint32_t)wrote;
    }
}

/* 10. PushChunk(iff=a0, type=d0, id=d1, size=d2) */
static void iff_fn_PushChunk(M68kCPUState *cpu)
{
    uint32_t iff = cpu->a[0];
    uint32_t type = cpu->d[0], id = cpu->d[1], size = cpu->d[2];
    int32_t err = 0;

    if (gr32(iff + IFF_FLAGS) & IFFF_WRITE) {
        uint32_t pcn = gr32(iff + IFF_DEPTH) ? iff_TopChunk(iff) : 0;
        uint32_t scan = 0;

        if (!pcn) {
            if (gr32(iff + IFF_FLAGS) & IFFF_NEWFILE)
                gw32(iff + IFF_FLAGS, gr32(iff + IFF_FLAGS) & ~IFFF_NEWFILE);
            else { cpu->d[0] = (uint32_t)IFFERR_EOF; return; }
        }
        if (!iff_GoodID(id)) { cpu->d[0] = (uint32_t)IFFERR_SYNTAX; return; }
        else if (!pcn) {
            if (id != ID_FORM && id != ID_LIST && id != ID_CAT) {
                cpu->d[0] = (uint32_t)IFFERR_NOTIFF; return;
            }
        } else if (id == ID_PROP) {
            if (gr32(pcn + CN_ID) != ID_LIST) {
                cpu->d[0] = (uint32_t)IFFERR_SYNTAX; return;
            }
        } else if (iff_is_composite(id)) {
            if (!iff_GoodType(type)) {
                cpu->d[0] = (uint32_t)IFFERR_NOTIFF; return;
            }
        } else {
            uint32_t pcnid = gr32(pcn + CN_ID);
            if (pcnid != ID_FORM && pcnid != ID_PROP) {
                cpu->d[0] = (uint32_t)IFFERR_SYNTAX; return;
            }
        }

        if (size == IFFSIZE_UNKNOWN && !(gr32(iff + IFF_FLAGS) & IFFF_RSEEK)) {
            cpu->d[0] = (uint32_t)IFFERR_SEEK; return; /* no buffered writer */
        }
        if ((err = iff_WriteStreamLong(iff, id))   < 0) { cpu->d[0] = err; return; }
        if ((err = iff_WriteStreamLong(iff, size)) < 0) { cpu->d[0] = err; return; }
        if (iff_is_composite(id)) {
            if ((err = iff_WriteStreamLong(iff, type)) < 0) {
                cpu->d[0] = err; return;
            }
            scan = 4;
        }
        err = iff_PushContextNode(iff, type, id, size, scan);
    } else {
        /* read mode: interpret the next chunk header */
        err = iff_GetChunkHeader(iff);
    }
    cpu->d[0] = (uint32_t)err;
}

/* PopChunk body shared by the PopChunk LVO and CloseIFF. */
static int32_t iff_PopChunk_impl(uint32_t iff)
{
    uint32_t cn = iff_TopChunk(iff);
    uint32_t size = 0;
    int32_t err;

    if (gr32(iff + IFF_FLAGS) & IFFF_WRITE) {
        if (gr32(cn + CN_SIZE) == IFFSIZE_UNKNOWN) {
            /* patch the real size in: seek back over data + size field,
             * write it, then seek forward to the end of the chunk again */
            err = iff_SeekStream(iff, -(int32_t)(gr32(cn + CN_SCAN) + 4));
            if (err) return err;
            size = gr32(cn + CN_SCAN);
            if ((err = iff_WriteStreamLong(iff, size)) < 0) return err;
            err = iff_SeekStream(iff, (int32_t)size);
            if (err) return err;
        } else {
            size = gr32(cn + CN_SIZE);
        }
        if (size & 1) {
            gw32(iff + IFF_SCRBUF, 0);
            err = iff_WriteStream(iff, iff + IFF_SCRBUF, 1);
            if (err < 0) return err;
            size++;
        }
    }
    iff_PopContextNode(iff);
    if (gr32(iff + IFF_FLAGS) & IFFF_WRITE) {
        cn = iff_TopChunk(iff);
        if (cn && gr32(cn + CN_SUCC))      /* real parent context exists */
            gw32(cn + CN_SCAN, gr32(cn + CN_SCAN) + size + 8);
    }
    return 0;
}

/* 11. PopChunk(iff=a0) */
static void iff_fn_PopChunk(M68kCPUState *cpu)
{
    uint32_t iff = cpu->a[0];
    if (!gr32(iff + IFF_DEPTH)) { cpu->d[0] = (uint32_t)IFFERR_EOC; return; }
    cpu->d[0] = (uint32_t)iff_PopChunk_impl(iff);
}

/* 12. EntryHandler(iff,type,id,pos,hook,object) */
static void iff_fn_EntryHandler(M68kCPUState *cpu)
{
    cpu->d[0] = (uint32_t)iff_InstallHandler(
        cpu->a[0], cpu->d[0], cpu->d[1], (int32_t)cpu->d[2],
        cpu->a[1], cpu->a[2], IFFLCI_ENTRYHANDLER);
}

/* 13. ExitHandler(iff,type,id,pos,hook,object) */
static void iff_fn_ExitHandler(M68kCPUState *cpu)
{
    cpu->d[0] = (uint32_t)iff_InstallHandler(
        cpu->a[0], cpu->d[0], cpu->d[1], (int32_t)cpu->d[2],
        cpu->a[1], cpu->a[2], IFFLCI_EXITHANDLER);
}

/* 14. PropChunk(iff,type,id) */
static void iff_fn_PropChunk(M68kCPUState *cpu)
{
    cpu->d[0] = (uint32_t)iff_InstallHandler(
        cpu->a[0], cpu->d[0], cpu->d[1], IFFSLI_TOP,
        HOOKTAG_PROP, cpu->a[0], IFFLCI_ENTRYHANDLER);
}

/* 15. PropChunks(iff,list=a1,n=d0) */
static void iff_fn_PropChunks(M68kCPUState *cpu)
{
    uint32_t iff = cpu->a[0], list = cpu->a[1];
    uint32_t n = cpu->d[0];
    int32_t err = 0;
    for (uint32_t i = 0; i < n && !err; i++)
        err = iff_InstallHandler(iff, gr32(list + i * 8), gr32(list + i * 8 + 4),
                                 IFFSLI_TOP, HOOKTAG_PROP, iff,
                                 IFFLCI_ENTRYHANDLER);
    cpu->d[0] = (uint32_t)err;
}

/* 16. StopChunk(iff,type,id) */
static void iff_fn_StopChunk(M68kCPUState *cpu)
{
    cpu->d[0] = (uint32_t)iff_InstallHandler(
        cpu->a[0], cpu->d[0], cpu->d[1], IFFSLI_TOP,
        HOOKTAG_STOP, cpu->a[0], IFFLCI_ENTRYHANDLER);
}

/* 17. StopChunks(iff,list=a1,n=d0) */
static void iff_fn_StopChunks(M68kCPUState *cpu)
{
    uint32_t iff = cpu->a[0], list = cpu->a[1];
    uint32_t n = cpu->d[0];
    int32_t err = 0;
    for (uint32_t i = 0; i < n && !err; i++)
        err = iff_InstallHandler(iff, gr32(list + i * 8), gr32(list + i * 8 + 4),
                                 IFFSLI_TOP, HOOKTAG_STOP, iff,
                                 IFFLCI_ENTRYHANDLER);
    cpu->d[0] = (uint32_t)err;
}

/* 18. CollectionChunk(iff,type,id) */
static void iff_fn_CollectionChunk(M68kCPUState *cpu)
{
    cpu->d[0] = (uint32_t)iff_InstallHandler(
        cpu->a[0], cpu->d[0], cpu->d[1], IFFSLI_TOP,
        HOOKTAG_COLL, cpu->a[0], IFFLCI_ENTRYHANDLER);
}

/* 19. CollectionChunks(iff,list=a1,n=d0) */
static void iff_fn_CollectionChunks(M68kCPUState *cpu)
{
    uint32_t iff = cpu->a[0], list = cpu->a[1];
    uint32_t n = cpu->d[0];
    int32_t err = 0;
    for (uint32_t i = 0; i < n && !err; i++)
        err = iff_InstallHandler(iff, gr32(list + i * 8), gr32(list + i * 8 + 4),
                                 IFFSLI_TOP, HOOKTAG_COLL, iff,
                                 IFFLCI_ENTRYHANDLER);
    cpu->d[0] = (uint32_t)err;
}

/* 20. StopOnExit(iff,type,id) */
static void iff_fn_StopOnExit(M68kCPUState *cpu)
{
    cpu->d[0] = (uint32_t)iff_InstallHandler(
        cpu->a[0], cpu->d[0], cpu->d[1], IFFSLI_TOP,
        HOOKTAG_EXITCTX, cpu->a[0], IFFLCI_EXITHANDLER);
}

/* 21. FindProp(iff,type,id) -> StoredProperty* */
static void iff_fn_FindProp(M68kCPUState *cpu)
{
    uint32_t lci = iff_FindLocalItem(cpu->a[0], cpu->d[0], cpu->d[1],
                                     IFFLCI_PROP);
    cpu->d[0] = lci ? gr32(lci + LCI_USERDATA) : 0;
}

/* 22. FindCollection(iff,type,id) -> CollectionItem* */
static void iff_fn_FindCollection(M68kCPUState *cpu)
{
    uint32_t lci = iff_FindLocalItem(cpu->a[0], cpu->d[0], cpu->d[1],
                                     IFFLCI_COLLECTION);
    uint32_t ciptr = lci ? gr32(lci + LCI_USERDATA) : 0;
    cpu->d[0] = ciptr ? gr32(ciptr) : 0;
}

/* 23. FindPropContext(iff) -> ContextNode* */
static void iff_fn_FindPropContext(M68kCPUState *cpu)
{
    cpu->d[0] = iff_FindPropContext(cpu->a[0]);
}

/* 24. CurrentChunk(iff) -> ContextNode* */
static void iff_fn_CurrentChunk(M68kCPUState *cpu)
{
    uint32_t iff = cpu->a[0];
    cpu->d[0] = gr32(iff + IFF_DEPTH) ? iff_TopChunk(iff) : 0;
}

/* 25. ParentChunk(cn=a0) -> ContextNode* */
static void iff_fn_ParentChunk(M68kCPUState *cpu)
{
    cpu->d[0] = cpu->a[0] ? gr32(cpu->a[0] + CN_SUCC) : 0;
}

/* 26. AllocLocalItem(type=d0,id=d1,ident=d2,dataSize=d3) */
static void iff_fn_AllocLocalItem(M68kCPUState *cpu)
{
    cpu->d[0] = iff_AllocLocalItem(cpu->d[0], cpu->d[1], cpu->d[2], cpu->d[3]);
}

/* 27. LocalItemData(lci=a0) */
static void iff_fn_LocalItemData(M68kCPUState *cpu)
{
    cpu->d[0] = cpu->a[0] ? gr32(cpu->a[0] + LCI_USERDATA) : 0;
}

/* 28. SetLocalItemPurge(lci=a0, hook=a1) */
static void iff_fn_SetLocalItemPurge(M68kCPUState *cpu)
{
    if (cpu->a[0]) gw32(cpu->a[0] + LCI_PURGEHOOK, cpu->a[1]);
}

/* 29. FreeLocalItem(lci=a0) */
static void iff_fn_FreeLocalItem(M68kCPUState *cpu)
{
    iff_FreeLocalItem(cpu->a[0]);
}

/* 30. FindLocalItem(iff,type,id,ident) */
static void iff_fn_FindLocalItem(M68kCPUState *cpu)
{
    cpu->d[0] = iff_FindLocalItem(cpu->a[0], cpu->d[0], cpu->d[1], cpu->d[2]);
}

/* 31. StoreLocalItem(iff=a0, lci=a1, position=d0) */
static void iff_fn_StoreLocalItem(M68kCPUState *cpu)
{
    if (!cpu->a[1]) { cpu->d[0] = (uint32_t)IFFERR_NOSCOPE; return; }
    cpu->d[0] = (uint32_t)iff_StoreLocalItem(cpu->a[0], cpu->a[1],
                                           (int32_t)cpu->d[0]);
}

/* 32. StoreItemInContext(iff=a0, lci=a1, cn=a2) */
static void iff_fn_StoreItemInContext(M68kCPUState *cpu)
{
    if (cpu->a[1] && cpu->a[2])
        iff_StoreItemInContext(cpu->a[0], cpu->a[1], cpu->a[2]);
}

/* 33. InitIFF(iff=a0, flags=d0, streamHook=a1) */
static void iff_fn_InitIFF(M68kCPUState *cpu)
{
    uint32_t iff = cpu->a[0];
    if (!iff) return;
    gw32(iff + IFF_FLAGS, gr32(iff + IFF_FLAGS) | cpu->d[0]);
    gw32(iff + IFF_STREAMHOOK, cpu->a[1]);
}

/* 34. InitIFFasDOS(iff=a0) */
static void iff_fn_InitIFFasDOS(M68kCPUState *cpu)
{
    uint32_t iff = cpu->a[0];
    if (!iff) return;
    gw32(iff + IFF_FLAGS, gr32(iff + IFF_FLAGS) | IFFF_RSEEK);
    gw32(iff + IFF_STREAMHOOK, HOOKTAG_DOSSTREAM);
}

/* 35. InitIFFasClip(iff=a0) — clipboard.device not present; record the tag
 *       so OpenIFF fails cleanly with IFFERR_CLIPBOARD. */
static void iff_fn_InitIFFasClip(M68kCPUState *cpu)
{
    uint32_t iff = cpu->a[0];
    if (!iff) return;
    gw32(iff + IFF_FLAGS, gr32(iff + IFF_FLAGS) | IFFF_RSEEK);
    gw32(iff + IFF_STREAMHOOK, HOOKTAG_CLIP);
}

/* 36. OpenClipboard(unit=d0) */
static void iff_fn_OpenClipboard(M68kCPUState *cpu)
{
    (void)cpu->d[0];
    cpu->d[0] = 0;   /* no clipboard.device */
}

/* 37. CloseClipboard(cb=a0) */
static void iff_fn_CloseClipboard(M68kCPUState *cpu)
{
    (void)cpu;
}

/* 38. GoodID(id=d0) */
static void iff_fn_GoodID(M68kCPUState *cpu)
{
    cpu->d[0] = iff_GoodID(cpu->d[0]);
}

/* 39. GoodType(id=d0) */
static void iff_fn_GoodType(M68kCPUState *cpu)
{
    cpu->d[0] = iff_GoodType(cpu->d[0]);
}

/* 40. IDtoStr(id=d0, buf=a0) — write 4-char ID + NUL; returns buf */
static void iff_fn_IDtoStr(M68kCPUState *cpu)
{
    uint32_t buf = cpu->a[0];
    if (buf && iff_gok(buf, 5)) {
        uint32_t id = cpu->d[0];
        g_ram[buf + 0] = (uint8_t)(id >> 24);
        g_ram[buf + 1] = (uint8_t)(id >> 16);
        g_ram[buf + 2] = (uint8_t)(id >> 8);
        g_ram[buf + 3] = (uint8_t)id;
        g_ram[buf + 4] = 0;
        cpu->d[0] = buf;
    } else {
        cpu->d[0] = 0;
    }
}

/* =========================================================================
 * Function table & registration
 *
 * Index order matches the LVO map in emulation/uaos_m68k_glue.c.
 * ========================================================================= */

static void *iffparse_funcs[] = {
    iff_fn_AllocIFF,            /*  1 -30 AllocIFF           */
    iff_fn_OpenIFF,             /*  2 -36 OpenIFF            */
    iff_fn_ParseIFF,            /*  3 -42 ParseIFF           */
    iff_fn_CloseIFF,            /*  4 -48 CloseIFF           */
    iff_fn_FreeIFF,             /*  5 -54 FreeIFF            */
    iff_fn_ReadChunkBytes,      /*  6 -60 ReadChunkBytes     */
    iff_fn_WriteChunkBytes,     /*  7 -66 WriteChunkBytes    */
    iff_fn_ReadChunkRecords,    /*  8 -72 ReadChunkRecords   */
    iff_fn_WriteChunkRecords,   /*  9 -78 WriteChunkRecords  */
    iff_fn_PushChunk,           /* 10 -84 PushChunk          */
    iff_fn_PopChunk,            /* 11 -90 PopChunk           */
    iff_fn_EntryHandler,        /* 12 -102 EntryHandler      */
    iff_fn_ExitHandler,         /* 13 -108 ExitHandler       */
    iff_fn_PropChunk,           /* 14 -114 PropChunk         */
    iff_fn_PropChunks,          /* 15 -120 PropChunks        */
    iff_fn_StopChunk,           /* 16 -126 StopChunk         */
    iff_fn_StopChunks,          /* 17 -132 StopChunks        */
    iff_fn_CollectionChunk,     /* 18 -138 CollectionChunk   */
    iff_fn_CollectionChunks,    /* 19 -144 CollectionChunks  */
    iff_fn_StopOnExit,          /* 20 -150 StopOnExit        */
    iff_fn_FindProp,            /* 21 -156 FindProp          */
    iff_fn_FindCollection,      /* 22 -162 FindCollection    */
    iff_fn_FindPropContext,     /* 23 -168 FindPropContext   */
    iff_fn_CurrentChunk,        /* 24 -174 CurrentChunk      */
    iff_fn_ParentChunk,         /* 25 -180 ParentChunk       */
    iff_fn_AllocLocalItem,      /* 26 -186 AllocLocalItem    */
    iff_fn_LocalItemData,       /* 27 -192 LocalItemData     */
    iff_fn_SetLocalItemPurge,   /* 28 -198 SetLocalItemPurge */
    iff_fn_FreeLocalItem,       /* 29 -204 FreeLocalItem     */
    iff_fn_FindLocalItem,       /* 30 -210 FindLocalItem     */
    iff_fn_StoreLocalItem,      /* 31 -216 StoreLocalItem    */
    iff_fn_StoreItemInContext,  /* 32 -222 StoreItemInContext*/
    iff_fn_InitIFF,             /* 33 -228 InitIFF           */
    iff_fn_InitIFFasDOS,        /* 34 -234 InitIFFasDOS      */
    iff_fn_InitIFFasClip,       /* 35 -240 InitIFFasClip     */
    iff_fn_OpenClipboard,       /* 36 -246 OpenClipboard     */
    iff_fn_CloseClipboard,      /* 37 -252 CloseClipboard    */
    iff_fn_GoodID,              /* 38 -258 GoodID            */
    iff_fn_GoodType,            /* 39 -264 GoodType          */
    iff_fn_IDtoStr,             /* 40 -270 IDtoStr           */
};

void UAOS_IFFPARSE_Register(void)
{
    UAOS_ROM_Register("iffparse.library", 39, 0x00000060,
                      (uint16_t)(sizeof(iffparse_funcs) / sizeof(iffparse_funcs[0])),
                      iffparse_funcs);
}
