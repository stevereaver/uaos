/* cmd_status.c — C:status — full process/task status listing
 *
 * Syntax: status [FULL] [TCB] [CLI]
 *
 *   (no flags)  List all tasks with name, state, type and priority.
 *   FULL        Include additional fields: stack sizes, signal masks.
 *   TCB         Dump raw TCB fields for each task (detailed debugging view).
 *   CLI         List only shell CLI processes (same filter as the ps command
 *               but with extended columns).
 *
 * This is the AmigaDOS STATUS command re-implemented for UAOS.  Unlike the
 * simpler C:ps command (which just enumerates via the ctx->enum_tasks
 * callback), status reads directly from the global task table so it can
 * expose priority, state, type and signal masks.
 *
 * Return codes: always 0.
 */

#include "cmd_internal.h"
#include "../exec/task.h"

/* Render a 32-bit value as 8 hex digits into buf[9]. */
static void status_hex(uint32_t v, char *buf)
{
    const char *h = "0123456789ABCDEF";
    for (int i = 7; i >= 0; i--) {
        buf[i] = h[v & 0xF];
        v >>= 4;
    }
    buf[8] = '\0';
}

/* Render tc_State as a short string. */
static const char *status_state(uint8_t st)
{
    switch (st) {
    case TASK_RUNNING:  return "Run ";
    case TASK_READY:    return "Rdy ";
    case TASK_WAITING:  return "Wait";
    case TASK_REMOVED:  return "Dead";
    default:            return "?   ";
    }
}

/* Render task type as a short string. */
static const char *status_type(TaskType t)
{
    if (t == TASK_TYPE_M68K) return "M68K";
    if (t == TASK_TYPE_X64)  return "X64 ";
    return "Natv";
}

/* Left-pad name to width w (truncate if longer). */
static void status_name(const char *name, int w, char *out)
{
    int i = 0;
    if (!name) name = "(unnamed)";
    while (name[i] && i < w) { out[i] = name[i]; i++; }
    while (i < w) out[i++] = ' ';
    out[w] = '\0';
}

void Cmd_Status(NativeCmdCtx *ctx, const char *args)
{
    int full = 0, tcb = 0, cli_only = 0;

    if (args) {
        full     = cmd_kw_find(args, "FULL");
        tcb      = cmd_kw_find(args, "TCB");
        cli_only = cmd_kw_find(args, "CLI");
    }

    /* ------------------------------------------------------------------
     * Normal listing
     * ------------------------------------------------------------------ */
    if (!tcb) {
        if (full) {
            PRINT("Name             Type State Pri  SigAlloc SigWait  SigRecvd");
            PRINT("------------------------------------------------------------");
        } else {
            PRINT("Name             Type State Pri");
            PRINT("--------------------------------");
        }

        int shown = 0;
        for (int i = 0; i < g_task_count; i++) {
            UaosTask *t = &g_tasks[i];
            if (t->tc_State == TASK_REMOVED) continue;

            /* CLI-only filter */
            if (cli_only) {
                /* Heuristic: native tasks whose name starts with "Shell" or
                 * "CLI" are shell instances; M68K and X64 tasks are processes. */
                const char *n = t->ln_Name ? t->ln_Name : "";
                int is_cli = (t->type == TASK_TYPE_M68K) || (t->type == TASK_TYPE_X64);
                if (!is_cli) {
                    is_cli = (n[0]=='S'&&n[1]=='h'&&n[2]=='e'&&n[3]=='l'&&n[4]=='l') ||
                             (n[0]=='C'&&n[1]=='L'&&n[2]=='I');
                }
                if (!is_cli) continue;
            }

            char line[CMD_MAX_LINE];
            char name[17];
            status_name(t->ln_Name, 16, name);

            /* Priority as signed decimal */
            char pristr[6];
            int8_t p8 = t->ln_Pri;
            int pi = 0;
            if (p8 < 0) { pristr[pi++] = '-'; p8 = (int8_t)(-p8); }
            char tmp[4]; int ti = 0;
            uint8_t pv = (uint8_t)p8;
            do { tmp[ti++] = '0' + (pv % 10); pv /= 10; } while (pv);
            while (ti-- > 0) pristr[pi++] = tmp[ti];
            pristr[pi] = '\0';

            cmd_scopy(line, name, CMD_MAX_LINE);
            cmd_scat(line, " ", CMD_MAX_LINE);
            cmd_scat(line, status_type(t->type), CMD_MAX_LINE);
            cmd_scat(line, " ", CMD_MAX_LINE);
            cmd_scat(line, status_state(t->tc_State), CMD_MAX_LINE);
            cmd_scat(line, " ", CMD_MAX_LINE);
            /* Pad priority to 4 chars */
            {
                int pl = cmd_slen(pristr);
                while (pl++ < 4 && cmd_slen(line) < CMD_MAX_LINE - 1)
                    cmd_scat(line, " ", CMD_MAX_LINE);
            }
            cmd_scat(line, pristr, CMD_MAX_LINE);

            if (full) {
                char hex[9];
                cmd_scat(line, "  ", CMD_MAX_LINE);
                status_hex(t->tc_SigAlloc, hex); cmd_scat(line, hex, CMD_MAX_LINE);
                cmd_scat(line, " ", CMD_MAX_LINE);
                status_hex(t->tc_SigWait,  hex); cmd_scat(line, hex, CMD_MAX_LINE);
                cmd_scat(line, " ", CMD_MAX_LINE);
                status_hex(t->tc_SigRecvd, hex); cmd_scat(line, hex, CMD_MAX_LINE);
            }

            PRINT(line);
            shown++;
        }

        if (shown == 0) PRINT("(no tasks)");
        return;
    }

    /* ------------------------------------------------------------------
     * TCB dump
     * ------------------------------------------------------------------ */
    PRINT("=== Task Control Block Dump ===");
    for (int i = 0; i < g_task_count; i++) {
        UaosTask *t = &g_tasks[i];
        if (t->tc_State == TASK_REMOVED) continue;

        char line[CMD_MAX_LINE];
        char name[17];
        status_name(t->ln_Name, 16, name);

        PRINT("---");
        cmd_scopy(line, "Name    : ", CMD_MAX_LINE); cmd_scat(line, name, CMD_MAX_LINE);
        PRINT(line);
        cmd_scopy(line, "Type    : ", CMD_MAX_LINE);
        cmd_scat(line, status_type(t->type), CMD_MAX_LINE); PRINT(line);
        cmd_scopy(line, "State   : ", CMD_MAX_LINE);
        cmd_scat(line, status_state(t->tc_State), CMD_MAX_LINE); PRINT(line);

        {
            char hex[9];
            cmd_scopy(line, "SigAlloc: 0x", CMD_MAX_LINE);
            status_hex(t->tc_SigAlloc, hex); cmd_scat(line, hex, CMD_MAX_LINE); PRINT(line);
            cmd_scopy(line, "SigWait : 0x", CMD_MAX_LINE);
            status_hex(t->tc_SigWait, hex);  cmd_scat(line, hex, CMD_MAX_LINE); PRINT(line);
            cmd_scopy(line, "SigRecvd: 0x", CMD_MAX_LINE);
            status_hex(t->tc_SigRecvd, hex); cmd_scat(line, hex, CMD_MAX_LINE); PRINT(line);
        }

        if (t->type == TASK_TYPE_NATIVE || t->type == TASK_TYPE_X64) {
            char hex[17]; hex[16] = '\0';
            const char *h = "0123456789ABCDEF";
            /* Print native_rsp as 16-digit hex */
            uint64_t rsp = t->native_rsp;
            for (int j = 15; j >= 0; j--) {
                hex[j] = h[rsp & 0xF]; rsp >>= 4;
            }
            cmd_scopy(line, "SP(x86) : 0x", CMD_MAX_LINE);
            cmd_scat(line, hex, CMD_MAX_LINE); PRINT(line);
            if (t->type == TASK_TYPE_X64) {
                uint64_t urs = t->native_initial_rsp;
                for (int j = 15; j >= 0; j--) {
                    hex[j] = h[urs & 0xF]; urs >>= 4;
                }
                cmd_scopy(line, "SP(usr) : 0x", CMD_MAX_LINE);
                cmd_scat(line, hex, CMD_MAX_LINE); PRINT(line);
            }
        } else {
            char hex[9];
            cmd_scopy(line, "M68K SP : 0x", CMD_MAX_LINE);
            status_hex(t->m68k_stack_top, hex); cmd_scat(line, hex, CMD_MAX_LINE); PRINT(line);
            cmd_scopy(line, "M68K PC : 0x", CMD_MAX_LINE);
            status_hex(t->m68k_entry,     hex); cmd_scat(line, hex, CMD_MAX_LINE); PRINT(line);
        }
    }
}
