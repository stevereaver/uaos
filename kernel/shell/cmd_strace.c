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
 * Output helpers - use global context
 * ------------------------------------------------------------------------- */
static void trace_flush(void)
{
    /* DEBUG: completely disabled to test for lockups */
    g_trace_buf_pos = 0;
}

static void trace_output(const char *s)
{
    (void)s;
    /* DEBUG: completely disabled to test for lockups */
    return;
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
 * Control API
 * ------------------------------------------------------------------------- */
void Strace_Enable(void)
{
    g_trace_enabled = 1;
    g_start_ticks = 0;
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
        PRINT("Usage: strace [options] <native_command> [args...]");
        PRINT("Options:");
        PRINT("  -c           Count syscalls only, no output");
        PRINT("  -e <name>    Trace only specific syscall");
        PRINT("  -o <file>    Output to file instead of console");
        PRINT("  -t           Show timestamps");
        PRINT("");
        PRINT("Examples:");
        PRINT("  strace dir");
        PRINT("  strace -c -o trace.log dir");
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

    /* Configure tracing */
    Strace_SetCountOnly(count_only);
    Strace_SetTimestamps(timestamps);
    Strace_SetOutput(output_file);

    if (filter_name[0]) {
        for (int i = 0; k_thunk_names[i].name; i++) {
            if (cmd_seq_ci(filter_name, (char*)k_thunk_names[i].name)) {
                Strace_SetFilter(k_thunk_names[i].idx);
                break;
            }
        }
    }

    /* Enable tracing */
    Strace_Enable();

    /* Try native command directly - avoid recursive shell dispatch */
    if (NativeCmd_Run(cmd_name, ctx, cmd_args) != 0) {
        PRINT("strace: command not found (only native commands supported)");
    }

    /* Disable tracing and print summary if needed */
    Strace_Disable();
    g_trace_ctx = NULL;
}
