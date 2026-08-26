/* avail.c — UAOS x86-64 userspace 'avail' command
 *
 * AmigaDOS C:Avail — show available system memory.
 *
 * Queries the kernel-exported memory API (SYSCALL_MEMINFO) and prints a
 * point-in-time snapshot of the live memory arenas:
 *   - the x86-64 userspace heap (ELF64 loader / sys_alloc arena)
 *   - the emulated M68k guest RAM slots
 *   - the scheduler task table
 *
 * Options:
 *   BYTES  report raw byte counts instead of KB/MB
 *   K      report kilobytes (default)
 */

#include "uaos_cmd.h"

/* Format a byte count into a human-readable string ("NNN", "NNN KB",
 * "NNN MB") into out[max].  When bytes_mode is non-zero, the raw byte
 * count is emitted instead. */
static void fmt_bytes(uint32_t bytes, int bytes_mode, char *out, int max)
{
    if (bytes_mode) {
        uint_to_dec(bytes, out, max);
        return;
    }
    if (bytes >= (1024u * 1024u)) {
        uint32_t mb = bytes / (1024u * 1024u);
        uint_to_dec(mb, out, max);
        uaos_strlcat(out, " MB", max);
    } else {
        uint32_t kb = bytes / 1024u;
        uint_to_dec(kb, out, max);
        uaos_strlcat(out, " KB", max);
    }
}

/* Pad s with trailing spaces up to at least width columns in-place. */
static void pad_to(char *s, int max, int width)
{
    int n = (int)uaos_strlen(s);
    while (n < width && n + 1 < max)
        s[n++] = ' ';
    s[n] = '\0';
}

int main(int argc, const char **argv)
{
    char args[UAOS_CMD_LINE_MAX];
    cmd_build_args(argc, argv, args, sizeof(args));

    int bytes_mode = cmd_kw_find(args, "BYTES");

    struct uaos_meminfo m;
    if (uaos_meminfo(&m) != 0) {
        put_line("avail: memory query failed");
        return 20;
    }

    put_line("Type       Total        Used        Free");
    put_line("----------------------------------------");

    char line[UAOS_CMD_LINE_MAX];
    char num[24];

    /* x86-64 userspace heap */
    uaos_strcpy(line, "X64");
    pad_to(line, sizeof(line), 12);
    fmt_bytes(m.x64_total, bytes_mode, num, sizeof(num));
    uaos_strlcat(line, num, sizeof(line));
    pad_to(line, sizeof(line), 25);
    fmt_bytes(m.x64_used, bytes_mode, num, sizeof(num));
    uaos_strlcat(line, num, sizeof(line));
    pad_to(line, sizeof(line), 37);
    fmt_bytes(m.x64_free, bytes_mode, num, sizeof(num));
    uaos_strlcat(line, num, sizeof(line));
    put_line(line);

    /* Emulated M68k guest RAM slots */
    uaos_strcpy(line, "M68K");
    pad_to(line, sizeof(line), 12);
    fmt_bytes(m.m68k_ram_total, bytes_mode, num, sizeof(num));
    uaos_strlcat(line, num, sizeof(line));
    uaos_strlcat(line, "/slot", sizeof(line));
    pad_to(line, sizeof(line), 25);
    uint_to_dec(m.m68k_slots_used, num, sizeof(num));
    uaos_strlcat(line, num, sizeof(line));
    uaos_strlcat(line, "/", sizeof(line));
    uint_to_dec(m.m68k_slots_total, num, sizeof(num));
    uaos_strlcat(line, " slots", sizeof(line));
    put_line(line);

    put_line("");

    /* Scheduler task summary */
    uaos_strcpy(line, "Tasks  ");
    uint_to_dec(m.tasks_total, num, sizeof(num));
    uaos_strlcat(line, num, sizeof(line));
    uaos_strlcat(line, " total, ", sizeof(line));
    uint_to_dec(m.tasks_running, num, sizeof(num));
    uaos_strlcat(line, num, sizeof(line));
    uaos_strlcat(line, " running, ", sizeof(line));
    uint_to_dec(m.tasks_waiting, num, sizeof(num));
    uaos_strlcat(line, num, sizeof(line));
    uaos_strlcat(line, " waiting", sizeof(line));
    put_line(line);

    return 0;
}
