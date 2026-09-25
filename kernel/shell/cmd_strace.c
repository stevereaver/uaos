/* cmd_strace.c — C:strace — trace system calls
 *
 * Linux-style system call tracer for UAOS. Traces:
 *   - M68K thunk calls (OpenLibrary, AllocMem, etc.)
 *   - DOS packet operations
 *
 * Usage: strace <command> [args...]
 *        strace -p <taskname>     (trace running task by name)
 *        strace -e <syscall>     (trace specific syscall only)
 *        strace -o <file>        (output to file)
 *        strace -t               (show timestamps)
 *        strace -c               (count syscalls only, no output)
 *
 * Output format matches Linux strace:
 *   syscall_name(arg1, arg2, ...) = return_value
 */

#include "cmd_internal.h"
#include "../../emulation/uaos_emu.h"
#include "../exec/rom_modules.h"
#include "../klog/klog.h"
#include <stdint.h>

/* Maximum number of syscall entries to track for -c option */
#define MAX_SYSCALL_STATS 64
#define TRACE_BUF_SIZE 512

typedef struct {
    uint32_t id;
    const char *name;
    uint64_t count;
    uint64_t errors;
} SyscallStat;

static SyscallStat g_stats[MAX_SYSCALL_STATS];
static int g_stat_count = 0;

/* Tracing state */
static int g_trace_enabled = 0;
static int g_trace_count_only = 0;
static int g_trace_timestamps = 0;
static uint32_t g_start_ticks = 0;
static char g_output_path[CMD_MAX_PATH] = {0};

/* Current trace filter (-e option) - 0 means trace all */
static uint32_t g_trace_filter = 0;

/* Output buffer for file redirection */
static char g_trace_buf[TRACE_BUF_SIZE];
static int g_trace_buf_pos = 0;

/* Shell context for output */
static NativeCmdCtx *g_trace_ctx = NULL;

/* Recursion guard - prevents tracing during trace output to avoid infinite loops */
static int g_in_trace_output = 0;

/* -o output file (opened lazily on first output) */
static VfsFile g_out_file;
static int g_out_file_open = 0;   /* 0 = not tried, 1 = open, -1 = failed */

/* Forward declarations for thunk names */
static const char *thunk_name(uint32_t idx);

/* -------------------------------------------------------------------------
 * String helpers
 * ------------------------------------------------------------------------- */
static int trace_strlen(const char *s)
{
    int n = 0;
    while (s[n]) n++;
    return n;
}

static void trace_strcpy(char *d, const char *s)
{
    int i = 0;
    while (s[i]) { d[i] = s[i]; i++; }
    d[i] = '\0';
}

static void trace_strcat(char *d, const char *s)
{
    int i = 0;
    while (d[i]) i++;
    int j = 0;
    while (s[j]) { d[i++] = s[j++]; }
    d[i] = '\0';
}

static int trace_strcmp_ci(const char *a, const char *b)
{
    int i = 0;
    while (a[i] && b[i]) {
        char ac = a[i];
        char bc = b[i];
        if (ac >= 'A' && ac <= 'Z') ac += 32;
        if (bc >= 'A' && bc <= 'Z') bc += 32;
        if (ac != bc) return ac - bc;
        i++;
    }
    char ac = a[i];
    char bc = b[i];
    if (ac >= 'A' && ac <= 'Z') ac += 32;
    if (bc >= 'A' && bc <= 'Z') bc += 32;
    return ac - bc;
}

/* -------------------------------------------------------------------------
 * Output helpers — route through klog (ring buffer + UART) so trace output
 * never touches the shell/WM paint path.  That path ran inside packet
 * dispatch and caused the lockups that got tracing disabled.  With -o the
 * output goes to a file instead (like Linux strace).
 * ------------------------------------------------------------------------- */
static void trace_flush(void)
{
    g_trace_buf_pos = 0;
    klog_commit();   /* flush any partial pending klog line */
}

static void trace_output(const char *s)
{
    if (!s || !*s) return;
    if (g_in_trace_output) return;
    g_in_trace_output = 1;

    if (g_output_path[0]) {
        /* File output mode — open lazily */
        if (!g_out_file_open) {
            g_out_file_open =
                VFS_Open(&g_out_file, g_output_path,
                         VFS_WRITE | VFS_CREATE) ? 1 : -1;
        }
        if (g_out_file_open > 0) {
            int n = 0;
            while (s[n]) n++;
            VFS_Write(&g_out_file, (const uint8_t *)s, (uint32_t)n);
        } else {
            /* Fall back to klog if the file could not be opened */
            klog_puts(KLOG_STRACE, KLOG_DEBUG, s);
        }
    } else {
        klog_puts(KLOG_STRACE, KLOG_DEBUG, s);
    }

    g_in_trace_output = 0;
}

/* Convert uint32 to hex string */
static void u32_to_hex(uint32_t v, char *buf, int digits)
{
    const char *h = "0123456789abcdef";
    for (int i = digits - 1; i >= 0; i--) {
        buf[i] = h[v & 0xF];
        v >>= 4;
    }
    buf[digits] = '\0';
}

/* Convert uint32 to decimal */
static void u32_to_dec(uint32_t v, char *buf)
{
    char tmp[12];
    int i = 0, j = 0;
    if (!v) { buf[j++] = '0'; buf[j] = '\0'; return; }
    while (v && i < 11) { tmp[i++] = (char)('0' + v % 10); v /= 10; }
    while (i--) buf[j++] = tmp[i];
    buf[j] = '\0';
}

/* Convert int32 to decimal (signed) */
static void i32_to_dec(int32_t v, char *buf)
{
    if (v < 0) {
        buf[0] = '-';
        u32_to_dec((uint32_t)(-v), buf + 1);
    } else {
        u32_to_dec((uint32_t)v, buf);
    }
}

/* -------------------------------------------------------------------------
 * Thunk name lookup
 * ------------------------------------------------------------------------- */
static const struct {
    uint32_t idx;
    const char *name;
} k_thunk_names[] = {
    {1,  "OpenLibrary"},
    {2,  "AllocMem"},
    {3,  "FreeMem"},
    {4,  "CloseLibrary"},
    {5,  "FindTask"},
    {6,  "AddTask"},
    {7,  "RemTask"},
    {8,  "Wait"},
    {9,  "Signal"},
    {10, "SetFunction"},
    {11, "AllocSignal"},
    {12, "FreeSignal"},
    {13, "AllocVec"},
    {14, "FreeVec"},
    {0, NULL}
};

static const char *thunk_name(uint32_t idx)
{
    for (int i = 0; k_thunk_names[i].name; i++) {
        if (k_thunk_names[i].idx == idx) {
            return k_thunk_names[i].name;
        }
    }
    return "unknown";
}

/* -------------------------------------------------------------------------
 * DOS packet action name lookup
 * ------------------------------------------------------------------------- */
static const struct {
    int32_t action;
    const char *name;
} k_action_names[] = {
    {0,      "ACTION_NIL"},
    {2,      "ACTION_GET_BLOCK"},
    {4,      "ACTION_SET_MAP"},
    {5,      "ACTION_DIE"},
    {6,      "ACTION_EVENT"},
    {7,      "ACTION_CURRENT_VOLUME"},
    {8,      "ACTION_LOCATE_OBJECT"},
    {9,      "ACTION_RENAME_DISK"},
    {15,     "ACTION_FREE_LOCK"},
    {16,     "ACTION_DELETE_OBJECT"},
    {17,     "ACTION_RENAME_OBJECT"},
    {18,     "ACTION_MORE_CACHE"},
    {19,     "ACTION_COPY_DIR"},
    {20,     "ACTION_WAIT_CHAR"},
    {21,     "ACTION_SET_PROTECT"},
    {22,     "ACTION_CREATE_DIR"},
    {23,     "ACTION_EXAMINE_OBJECT"},
    {24,     "ACTION_EXAMINE_NEXT"},
    {25,     "ACTION_DISK_INFO"},
    {26,     "ACTION_INFO"},
    {27,     "ACTION_FLUSH"},
    {28,     "ACTION_SET_COMMENT"},
    {29,     "ACTION_PARENT"},
    {30,     "ACTION_TIMER"},
    {31,     "ACTION_INHIBIT"},
    {32,     "ACTION_DISK_TYPE"},
    {33,     "ACTION_DISK_CHANGE"},
    {34,     "ACTION_SET_DATE"},
    {40,     "ACTION_SAME_LOCK"},
    {82,     "ACTION_READ"},
    {87,     "ACTION_WRITE"},
    {1004,   "ACTION_FINDUPDATE"},
    {1005,   "ACTION_FINDINPUT"},
    {1006,   "ACTION_FINDOUTPUT"},
    {1007,   "ACTION_END"},
    {1008,   "ACTION_SEEK"},
    {1023,   "ACTION_WRITE_PROTECT"},
    {1027,   "ACTION_IS_FILESYSTEM"},
    {1030,   "ACTION_SET_FILE_SIZE"},
    {1034,   "ACTION_CHANGE_MODE"},
    {1035,   "ACTION_COPY_DIR_FH"},
    {1036,   "ACTION_PARENT_FH"},
    {1037,   "ACTION_EXAMINE_ALL"},
    {1038,   "ACTION_EXAMINE_FH"},
    {0, NULL}
};

static const char *action_name(int32_t action)
{
    for (int i = 0; k_action_names[i].name; i++) {
        if (k_action_names[i].action == action) {
            return k_action_names[i].name;
        }
    }
    return "ACTION_UNKNOWN";
}

/* -------------------------------------------------------------------------
 * Syscall statistics
 * ------------------------------------------------------------------------- */
static SyscallStat *get_stat(uint32_t id, const char *name)
{
    for (int i = 0; i < g_stat_count; i++) {
        if (g_stats[i].id == id) {
            return &g_stats[i];
        }
    }
    if (g_stat_count < MAX_SYSCALL_STATS) {
        SyscallStat *s = &g_stats[g_stat_count++];
        s->id = id;
        s->name = name;
        s->count = 0;
        s->errors = 0;
        return s;
    }
    return NULL;
}

/* -------------------------------------------------------------------------
 * Public API for thunk_handler.c to call
 * ------------------------------------------------------------------------- */
void Strace_ThunkEntry(uint32_t thunk_idx, M68kCPUState *cpu)
{
    if (!g_trace_enabled) return;
    if (g_in_trace_output) return;  /* Prevent recursion during output */

    /* Filter check - skip if not matching filter */
    if (g_trace_filter && g_trace_filter != thunk_idx) return;

    const char *name = thunk_name(thunk_idx);

    /* Build simple output line - avoid reading guest memory strings */
    char line[128];
    char tmp[24];

    line[0] = '\0';

    /* Timestamp */
    if (g_trace_timestamps) {
        trace_strcat(line, "[------] ");
    }

    /* Syscall name and opening paren */
    trace_strcat(line, name);
    trace_strcat(line, "(");

    /* Arguments - only output register values, never dereference guest pointers */
    switch (thunk_idx) {
        case 1: /* OpenLibrary */
            /* Output a1=ptr, d0=version instead of reading string */
            trace_strcat(line, "a1=0x");
            u32_to_hex(cpu->a[1], tmp, 8);
            trace_strcat(line, tmp);
            trace_strcat(line, ", d0=");
            u32_to_dec(cpu->d[0], tmp);
            trace_strcat(line, tmp);
            break;
        case 2: /* AllocMem */
        case 13: /* AllocVec */
            u32_to_dec(cpu->d[0], tmp);
            trace_strcat(line, tmp);
            trace_strcat(line, ", 0x");
            u32_to_hex(cpu->d[1], tmp, 8);
            trace_strcat(line, tmp);
            break;
        case 3: /* FreeMem */
        case 14: /* FreeVec */
            trace_strcat(line, "0x");
            u32_to_hex(cpu->a[1], tmp, 8);
            trace_strcat(line, tmp);
            if (thunk_idx == 3) {
                trace_strcat(line, ", ");
                u32_to_dec(cpu->d[0], tmp);
                trace_strcat(line, tmp);
            }
            break;
        case 4: /* CloseLibrary */
        case 6: /* AddTask */
        case 7: /* RemTask */
        case 10: /* SetFunction */
            trace_strcat(line, "0x");
            u32_to_hex(cpu->a[1], tmp, 8);
            trace_strcat(line, tmp);
            break;
        case 5: /* FindTask */
            trace_strcat(line, "a1=0x");
            u32_to_hex(cpu->a[1], tmp, 8);
            trace_strcat(line, tmp);
            break;
        case 8: /* Wait */
            trace_strcat(line, "0x");
            u32_to_hex(cpu->d[0], tmp, 8);
            trace_strcat(line, tmp);
            break;
        case 9: /* Signal */
            trace_strcat(line, "a1=0x");
            u32_to_hex(cpu->a[1], tmp, 8);
            trace_strcat(line, tmp);
            trace_strcat(line, ", d0=0x");
            u32_to_hex(cpu->d[0], tmp, 8);
            trace_strcat(line, tmp);
            break;
        case 11: /* AllocSignal */
            i32_to_dec((int32_t)cpu->d[0], tmp);
            trace_strcat(line, tmp);
            break;
        case 12: /* FreeSignal */
            u32_to_dec(cpu->d[0], tmp);
            trace_strcat(line, tmp);
            break;
        default:
            /* Generic register dump */
            trace_strcat(line, "d0=0x");
            u32_to_hex(cpu->d[0], tmp, 8);
            trace_strcat(line, tmp);
            break;
    }

    trace_strcat(line, ")");

    trace_output(line);
}

void Strace_ThunkExit(uint32_t thunk_idx, uint32_t result, uint32_t elapsed_us)
{
    (void)thunk_idx;
    if (!g_trace_enabled) return;
    if (g_in_trace_output) return;  /* Prevent recursion during output */
    if (g_trace_filter && g_trace_filter != thunk_idx) return;

    /* Update statistics */
    SyscallStat *stat = get_stat(thunk_idx, thunk_name(thunk_idx));
    if (stat) {
        stat->count++;
        if ((int32_t)result < 0) stat->errors++;
    }

    if (g_trace_count_only) return;

    char line[64];
    char tmp[16];

    /* Result */
    line[0] = '\0';
    trace_strcat(line, " = ");

    if ((int32_t)result < 0) {
        /* Error - show as negative */
        trace_strcat(line, "-1");
        if (result != (uint32_t)-1) {
            /* Show actual errno value */
            trace_strcat(line, " (");
            i32_to_dec((int32_t)result, tmp);
            trace_strcat(line, tmp);
            trace_strcat(line, ")");
        }
    } else {
        /* Success */
        if (result == 0 && (thunk_idx == 2 || thunk_idx == 13)) {
            /* AllocMem/AllocVec returning NULL is failure */
            trace_strcat(line, "0");
        } else {
            trace_strcat(line, "0x");
            u32_to_hex(result, tmp, 8);
            trace_strcat(line, tmp);
        }
    }

    /* Elapsed time */
    if (elapsed_us > 0) {
        trace_strcat(line, " <");
        u32_to_dec(elapsed_us, tmp);
        trace_strcat(line, tmp);
        trace_strcat(line, ">");
    }

    trace_output(line);
    trace_output("\n");
}

void Strace_DosPacket(int32_t action, int32_t arg1, int32_t arg2, int32_t result, int32_t ioerr)
{
    if (!g_trace_enabled) return;
    if (g_in_trace_output) return;  /* Prevent recursion during output */
    if (g_trace_filter) return; /* DOS packets have no single ID */

    SyscallStat *stat = get_stat(0x10000 + (uint32_t)action, action_name(action));
    if (stat) {
        stat->count++;
        if (ioerr != 0) stat->errors++;
    }

    if (g_trace_count_only) return;

    char line[256];
    char tmp[32];

    line[0] = '\0';

    if (g_trace_timestamps) {
        trace_strcat(line, "[------] ");
    }

    trace_strcat(line, action_name(action));
    trace_strcat(line, "(");

    /* Action-specific argument formatting */
    switch (action) {
        case 8:  /* LOCATE_OBJECT */
        case 19: /* COPY_DIR */
        case 29: /* PARENT */
            trace_strcat(line, "lock=");
            i32_to_dec(arg1, tmp);
            trace_strcat(line, tmp);
            break;
        case 82: /* READ */
        case 87: /* WRITE */
            trace_strcat(line, "fh=");
            i32_to_dec(arg1, tmp);
            trace_strcat(line, tmp);
            trace_strcat(line, ", buf=0x");
            u32_to_hex((uint32_t)arg2, tmp, 8);
            trace_strcat(line, tmp);
            break;
        case 1005: /* FINDINPUT */
        case 1006: /* FINDOUTPUT */
        case 1004: /* FINDUPDATE */
            trace_strcat(line, "lock=");
            i32_to_dec(arg1, tmp);
            trace_strcat(line, tmp);
            break;
        case 16: /* DELETE_OBJECT */
        case 22: /* CREATE_DIR */
        case 28: /* SET_COMMENT */
            trace_strcat(line, "lock=");
            i32_to_dec(arg1, tmp);
            trace_strcat(line, tmp);
            break;
        default:
            i32_to_dec(arg1, tmp);
            trace_strcat(line, tmp);
            trace_strcat(line, ", ");
            i32_to_dec(arg2, tmp);
            trace_strcat(line, tmp);
            break;
    }

    trace_strcat(line, ")");

    /* Result */
    trace_strcat(line, " = ");
    i32_to_dec(result, tmp);
    trace_strcat(line, tmp);

    if (ioerr != 0) {
        trace_strcat(line, " [error=");
        i32_to_dec(ioerr, tmp);
        trace_strcat(line, tmp);
        trace_strcat(line, "]");
    }

    trace_output(line);
    trace_output("\n");
}

/* -------------------------------------------------------------------------
 * M68k library-call tracing — hooked by m68k_illg_instr_callback in
 * uaos_m68k_glue.c, which is the real ILLEGAL-opcode dispatch path in the
 * kernel build.  (UAOS_HandleThunk/Strace_ThunkEntry above only serve the
 * hosted bridge path.)  Trace id is (lib << 16) | fn.
 * ------------------------------------------------------------------------- */
static const struct {
    uint8_t lib;
    uint8_t fn;
    const char *name;
} k_libcall_names[] = {
    /* exec.library (lib=1) */
    {1, 1, "exec.OpenLibrary"}, {1, 2, "exec.CloseLibrary"},
    {1, 3, "exec.AllocMem"},    {1, 4, "exec.FreeMem"},
    {1, 5, "exec.FindTask"},    {1, 6, "exec.Wait"},
    {1, 7, "exec.Signal"},      {1, 8, "exec.SetSignal"},
    {1, 9, "exec.AllocSignal"}, {1,10, "exec.FreeSignal"},
    {1,11, "exec.PutMsg"},      {1,12, "exec.GetMsg"},
    {1,13, "exec.ReplyMsg"},    {1,14, "exec.WaitPort"},
    /* dos.library (lib=2) */
    {2, 1, "dos.Output"},       {2, 2, "dos.Write"},
    {2, 3, "dos.Open"},         {2, 4, "dos.Close"},
    {2, 5, "dos.Read"},         {2, 6, "dos.Exit"},
    {2, 7, "dos.IoErr"},        {2, 8, "dos.Input"},
    {2,10, "dos.FPuts"},        {2,11, "dos.PutStr"},
    {2,13, "dos.Printf"},       {2,15, "dos.ReadArgs"},
    {2,16, "dos.GetArgStr"},    {2,17, "dos.IsInteractive"},
    {2,18, "dos.DeleteFile"},   {2,19, "dos.Rename"},
    {2,20, "dos.SetProtection"},{2,21, "dos.GetVar"},
    {2,22, "dos.SetVar"},       {2,23, "dos.Seek"},
    {2,24, "dos.Lock"},         {2,25, "dos.UnLock"},
    {2,26, "dos.Examine"},      {2,27, "dos.ExamineNext"},
    {2,28, "dos.CreateDir"},    {2,29, "dos.DupLock"},
    {2,30, "dos.Parent"},       {2,31, "dos.DateStamp"},
    {2,32, "dos.Delay"},        {2,33, "dos.DateToStr"},
    {2,38, "dos.LoadSeg"},      {2,39, "dos.UnLoadSeg"},
    {2,40, "dos.CreateProc"},   {2,42, "dos.RunCommand"},
    {2,50, "dos.WaitForChar"},  {2,51, "dos.NameFromLock"},
    /* bsdsocket.library (lib=3) */
    {3, 1, "socket.socket"},    {3, 5, "socket.connect"},
    {3, 7, "socket.recv"},      {3, 8, "socket.recvfrom"},
    /* graphics.library (lib=4, fn = |LVO|/6) */
    {4,10, "graphics.Text"},    {4,40, "graphics.Move"},
    {4,57, "graphics.SetAPen"}, {4,58, "graphics.SetBPen"},
    /* intuition.library (lib=5) */
    {5, 1, "intuition.OpenLibrary"},   {5, 3, "intuition.OpenWindow"},
    {5, 4, "intuition.CloseWindow"},   {5,13, "intuition.OpenWindowTags"},
    {0, 0, NULL}
};

/* -------------------------------------------------------------------------
 * x86-64 syscall names (INT 0x80 table in kernel/exec/syscall_table.h).
 * Trace id is 0x70000 | num — above the (lib<<16) space (lib <= 6).
 * Names carry a "sys." prefix so -e can disambiguate them from M68k
 * library calls of the same name (e.g. "-e sys.open" vs "-e dos.Open").
 * ------------------------------------------------------------------------- */
static const struct {
    uint32_t num;
    const char *name;
} k_syscall_names[] = {
    {0x01, "sys.write"},      {0x02, "sys.read"},
    {0x03, "sys.open"},       {0x04, "sys.close"},
    {0x05, "sys.read_file"},  {0x06, "sys.write_file"},
    {0x07, "sys.exit"},       {0x08, "sys.getargs"},
    {0x09, "sys.spawn"},      {0x0A, "sys.wait"},
    {0x0B, "sys.alloc"},      {0x0C, "sys.getcwd"},
    {0x0D, "sys.opendir"},    {0x0E, "sys.readdir"},
    {0x0F, "sys.closedir"},   {0x10, "sys.stat"},
    {0x11, "sys.gui_create_window"},  {0x12, "sys.gui_destroy_window"},
    {0x13, "sys.gui_set_scroll_info"},{0x14, "sys.gui_set_scroll"},
    {0x15, "sys.gui_draw_text"},      {0x16, "sys.gui_draw_rect"},
    {0x17, "sys.gui_present"},        {0x18, "sys.gui_get_event"},
    {0x20, "sys.mkdir"},      {0x21, "sys.delete"},
    {0x22, "sys.rename"},     {0x23, "sys.setprotection"},
    {0x24, "sys.getprotection"},{0x25, "sys.getcomment"},
    {0x26, "sys.setcomment"}, {0x27, "sys.getvolumeinfo"},
    {0x28, "sys.readkey"},    {0x29, "sys.getattrs"},
    {0x2A, "sys.setattrs"},   {0x2B, "sys.getmountcount"},
    {0x2C, "sys.getmountname"},{0x2D, "sys.meminfo"},
    {0x30, "sys.gui_draw_line"},      {0x31, "sys.gui_fill_rect"},
    {0x32, "sys.gui_draw_3dborder"},  {0x33, "sys.gui_draw_pixel"},
    {0x34, "sys.gui_draw_text_bg"},   {0x35, "sys.gui_get_winsize"},
    {0x36, "sys.gui_set_title"},      {0x37, "sys.gui_draw_ellipse"},
    {0xFF, "sys.schedule"},
    {0, NULL}
};

static const char *syscall_name(uint32_t num)
{
    for (int i = 0; k_syscall_names[i].name; i++) {
        if (k_syscall_names[i].num == num)
            return k_syscall_names[i].name;
    }
    return "sys.unknown";
}

/* -------------------------------------------------------------------------
 * x64 syscall tracing — hooked at the end of Syscall_Dispatch in
 * kernel/exec/syscall_dispatch.c.  Emits a single self-contained
 * "sys.name(0x.., 0x.., 0x..) = ret" line; pointer args are never
 * dereferenced (identical rule to the thunk path).
 * ------------------------------------------------------------------------- */
void Strace_Syscall(uint32_t num, uint64_t a1, uint64_t a2, uint64_t a3,
                    int64_t result)
{
    if (!g_trace_enabled) return;
    if (g_in_trace_output) return;
    if (num == 0xFF) return;   /* sys.schedule — voluntary yield; pure spam */

    uint32_t id = 0x70000 | num;
    if (g_trace_filter && g_trace_filter != id) return;

    SyscallStat *stat = get_stat(id, syscall_name(num));
    if (stat) {
        stat->count++;
        if (result < 0) stat->errors++;
    }

    if (g_trace_count_only) return;

    char line[160];
    char tmp[24];

    line[0] = '\0';
    if (g_trace_timestamps) trace_strcat(line, "[------] ");

    trace_strcat(line, syscall_name(num));
    trace_strcat(line, "(0x");
    u32_to_hex((uint32_t)a1, tmp, 8);  trace_strcat(line, tmp);
    trace_strcat(line, ", 0x");
    u32_to_hex((uint32_t)a2, tmp, 8);  trace_strcat(line, tmp);
    trace_strcat(line, ", 0x");
    u32_to_hex((uint32_t)a3, tmp, 8);  trace_strcat(line, tmp);
    trace_strcat(line, ") = ");

    if (result < 0) {
        i32_to_dec((int32_t)result, tmp);
        trace_strcat(line, tmp);
    } else {
        trace_strcat(line, "0x");
        u32_to_hex((uint32_t)result, tmp, 8);
        trace_strcat(line, tmp);
    }

    trace_output(line);
    trace_output("\n");
}

static const char *const k_lib_prefix[] = {
    "lib", "exec", "dos", "bsdsocket", "graphics", "intuition", "gadtools"
};

/* Dynamically-generated names for (lib,fn) pairs not in the table, so the
 * -c stats table and trace lines share a stable name string per id. */
#define MAX_DYN_NAMES 16
static char     g_dyn_names[MAX_DYN_NAMES][24];
static uint32_t g_dyn_ids[MAX_DYN_NAMES];
static int      g_dyn_name_count = 0;

static const char *libcall_name(uint8_t lib, uint8_t fn)
{
    for (int i = 0; k_libcall_names[i].name; i++) {
        if (k_libcall_names[i].lib == lib && k_libcall_names[i].fn == fn)
            return k_libcall_names[i].name;
    }

    uint32_t id = ((uint32_t)lib << 16) | fn;
    for (int i = 0; i < g_dyn_name_count; i++) {
        if (g_dyn_ids[i] == id) return g_dyn_names[i];
    }
    if (g_dyn_name_count >= MAX_DYN_NAMES) return "m68k.libcall";

    char *buf = g_dyn_names[g_dyn_name_count];
    g_dyn_ids[g_dyn_name_count++] = id;
    const char *p = (lib <= 6) ? k_lib_prefix[lib] : "lib";
    int i = 0;
    while (*p && i < 18) buf[i++] = *p++;
    buf[i++] = '.'; buf[i++] = 'f'; buf[i++] = 'n';
    u32_to_dec(fn, buf + i);
    return buf;
}

void Strace_M68kEntry(uint8_t lib, uint8_t fn, M68kCPUState *cpu)
{
    if (!g_trace_enabled) return;
    if (g_in_trace_output) return;

    uint32_t id = ((uint32_t)lib << 16) | (uint32_t)fn;
    if (g_trace_filter && g_trace_filter != id) return;
    if (g_trace_count_only) return;   /* stats only, updated at exit */

    char line[128];
    char tmp[24];

    line[0] = '\0';
    if (g_trace_timestamps) trace_strcat(line, "[------] ");

    trace_strcat(line, libcall_name(lib, fn));
    trace_strcat(line, "(d0=0x");
    u32_to_hex(cpu->d[0], tmp, 8);
    trace_strcat(line, tmp);
    trace_strcat(line, ", a0=0x");
    u32_to_hex(cpu->a[0], tmp, 8);
    trace_strcat(line, tmp);
    trace_strcat(line, ", a1=0x");
    u32_to_hex(cpu->a[1], tmp, 8);
    trace_strcat(line, tmp);
    trace_strcat(line, ")");

    trace_output(line);
}

void Strace_M68kExit(uint8_t lib, uint8_t fn, int32_t result)
{
    if (!g_trace_enabled) return;
    if (g_in_trace_output) return;

    uint32_t id = ((uint32_t)lib << 16) | (uint32_t)fn;
    if (g_trace_filter && g_trace_filter != id) return;

    SyscallStat *stat = get_stat(id, libcall_name(lib, fn));
    if (stat) {
        stat->count++;
        if (result < 0) stat->errors++;
    }

    if (g_trace_count_only) return;

    char line[64];
    char tmp[16];

    line[0] = '\0';
    trace_strcat(line, " = ");
    if (result < 0) {
        trace_strcat(line, "-1");
    } else {
        trace_strcat(line, "0x");
        u32_to_hex((uint32_t)result, tmp, 8);
        trace_strcat(line, tmp);
    }

    trace_output(line);
    trace_output("\n");
}

/* -------------------------------------------------------------------------
 * Control API
 * ------------------------------------------------------------------------- */
void Strace_Enable(void)
{
    g_trace_enabled = 1;
    g_start_ticks = 0;
    g_out_file_open = 0;
    g_stat_count = 0;
    for (int i = 0; i < MAX_SYSCALL_STATS; i++) {
        g_stats[i].id = 0;
        g_stats[i].name = NULL;
        g_stats[i].count = 0;
        g_stats[i].errors = 0;
    }
}

void Strace_Disable(void)
{
    if (g_trace_enabled && g_trace_count_only) {
        /* Print statistics */
        trace_output("\n");
        trace_output("syscall                count    errors\n");
        trace_output("-------------------- ------- -------\n");

        char line[128];
        char tmp[32];

        for (int i = 0; i < g_stat_count; i++) {
            line[0] = '\0';

            /* Name (padded to 20) */
            trace_strcat(line, g_stats[i].name);
            int pad = 20 - trace_strlen(g_stats[i].name);
            while (pad-- > 0) trace_strcat(line, " ");

            /* Count */
            trace_strcat(line, " ");
            u32_to_dec((uint32_t)g_stats[i].count, tmp);
            trace_strcat(line, tmp);
            pad = 7 - trace_strlen(tmp);
            while (pad-- > 0) trace_strcat(line, " ");

            /* Errors */
            trace_strcat(line, " ");
            u32_to_dec((uint32_t)g_stats[i].errors, tmp);
            trace_strcat(line, tmp);

            trace_output(line);
            trace_output("\n");
        }

        trace_flush();
    }

    if (g_out_file_open > 0) {
        VFS_Close(&g_out_file);
        g_out_file_open = 0;
    }

    g_trace_enabled = 0;
    trace_flush();
}

int Strace_IsEnabled(void)
{
    return g_trace_enabled;
}

void Strace_SetFilter(uint32_t filter)
{
    g_trace_filter = filter;
}

void Strace_SetOutput(const char *path)
{
    if (path && path[0]) {
        trace_strcpy(g_output_path, path);
    } else {
        g_output_path[0] = '\0';
    }
}

void Strace_SetCountOnly(int enable)
{
    g_trace_count_only = enable;
}

void Strace_SetTimestamps(int enable)
{
    g_trace_timestamps = enable;
}

/* -------------------------------------------------------------------------
 * Command implementation
 * ------------------------------------------------------------------------- */
void Cmd_Strace(NativeCmdCtx *ctx, const char *args)
{
    if (!args || !*args) {
        PRINT("Usage: strace [options] <command> [args...]");
        PRINT("Options:");
        PRINT("  -c           Count syscalls only, no output");
        PRINT("  -e <name>    Trace only specific call");
        PRINT("  -o <file>    Output to file instead of klog");
        PRINT("  -t           Show timestamps");
        PRINT("");
        PRINT("Traces x64 syscalls (sys.*), M68k libcalls (exec.*, dos.*, ...)");
        PRINT("and DOS packets.  Output goes to klog (dmesg/serial), not the console.");
        PRINT("Examples:");
        PRINT("  strace dir                 then 'dmesg strace'");
        PRINT("  strace -c mem              stats table only");
        PRINT("  strace -e sys.open dir     one syscall only");
        PRINT("  strace -o RAM:trace.txt run <m68k binary>");
        return;
    }

    /* Store context for output */
    g_trace_ctx = ctx;

    /* Parse options */
    const char *p = args;
    int count_only = 0;
    int timestamps = 0;
    char output_file[CMD_MAX_PATH] = {0};
    char filter_name[32] = {0};

    /* Skip leading whitespace */
    while (*p == ' ' || *p == '\t') p++;

    /* Parse flags */
    while (*p == '-') {
        char flag = p[1];
        if (flag == '\0' || flag == ' ') break;

        if (flag == 'c') {
            count_only = 1;
            p += 2;
        } else if (flag == 't') {
            timestamps = 1;
            p += 2;
        } else if (flag == 'e' || flag == 'o') {
            /* Option with argument */
            p += 2;
            while (*p == ' ') p++;

            char arg[64];
            int i = 0;
            while (*p && *p != ' ' && i < 63) {
                arg[i++] = *p++;
            }
            arg[i] = '\0';

            if (flag == 'o') {
                cmd_make_abs(ctx->cwd, arg, output_file, CMD_MAX_PATH);
            } else if (flag == 'e') {
                cmd_scopy(filter_name, arg, 32);
            }
        } else {
            p += 2;
        }

        while (*p == ' ') p++;
    }

    /* Need a command to run */
    if (!*p) {
        PRINT("strace: no command specified");
        return;
    }

    /* Extract command name */
    char cmd_name[32];
    int i = 0;
    while (*p && *p != ' ' && i < 31) {
        cmd_name[i++] = *p++;
    }
    cmd_name[i] = '\0';

    /* Get command arguments */
    const char *cmd_args = p;
    while (*cmd_args == ' ') cmd_args++;

    /* Configure tracing — the filter persists in a global, so clear it
     * explicitly or a previous "-e" run keeps filtering this one. */
    Strace_SetCountOnly(count_only);
    Strace_SetTimestamps(timestamps);
    Strace_SetOutput(output_file);
    Strace_SetFilter(0);

    if (filter_name[0]) {
        /* Match M68k libcall names first ("exec.Wait" or bare "Wait") */
        for (int i = 0; k_libcall_names[i].name; i++) {
            const char *n = k_libcall_names[i].name;
            const char *short_n = n;
            for (const char *q = n; *q; q++)
                if (*q == '.') short_n = q + 1;
            if (cmd_seq_ci(filter_name, (char*)n) ||
                cmd_seq_ci(filter_name, (char*)short_n)) {
                Strace_SetFilter(((uint32_t)k_libcall_names[i].lib << 16) |
                                 k_libcall_names[i].fn);
                break;
            }
        }
        if (!g_trace_filter) {
            for (int i = 0; k_thunk_names[i].name; i++) {
                if (cmd_seq_ci(filter_name, (char*)k_thunk_names[i].name)) {
                    Strace_SetFilter(k_thunk_names[i].idx);
                    break;
                }
            }
        }
        if (!g_trace_filter) {
            /* x64 syscall names — match "sys.open" or bare "sys_open" */
            for (int i = 0; k_syscall_names[i].name; i++) {
                const char *n = k_syscall_names[i].name;
                const char *short_n = n;
                for (const char *q = n; *q; q++)
                    if (*q == '.') short_n = q + 1;
                if (cmd_seq_ci(filter_name, (char*)n) ||
                    cmd_seq_ci(filter_name, (char*)short_n)) {
                    Strace_SetFilter(0x70000 | k_syscall_names[i].num);
                    break;
                }
            }
        }
    }

    /* Enable tracing */
    Strace_Enable();

    /* Run the command: native table first (no shell re-entry), then fall
     * back to the full shell dispatcher so userspace ELF binaries (dir,
     * echo, ...) and external commands are traced too — their x64 INT 0x80
     * calls go through Strace_Syscall. */
    if (NativeCmd_Run(cmd_name, ctx, cmd_args) != 0) {
        if (ctx->dispatch_line && ctx->shell_extra) {
            char line[CMD_MAX_PATH + 96];
            int li = 0;
            for (const char *q = cmd_name; *q && li < (int)sizeof(line) - 2; q++)
                line[li++] = *q;
            if (*cmd_args) {
                line[li++] = ' ';
                for (const char *q = cmd_args; *q && li < (int)sizeof(line) - 1; q++)
                    line[li++] = *q;
            }
            line[li] = '\0';
            ctx->dispatch_line(ctx->shell_extra, line);
        } else {
            PRINT("strace: command not found");
        }
    }

    /* Disable tracing and print summary if needed */
    Strace_Disable();
    g_trace_ctx = NULL;
}
