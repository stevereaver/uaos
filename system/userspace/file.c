/* file.c — UAOS x86-64 userspace 'file' command
 *
 * Inspects files and reports their type from magic numbers, focusing on the
 * executable formats used by UAOS: UAOS-wrapped x86-64 ELF64, UAOS-wrapped
 * Amiga Hunk (M68k), raw ELF files, and raw Hunk binaries.
 */

#include "uaos_syscall.h"
#include "uaos_libc.h"

#define UAOS_MAGIC_0 'U'
#define UAOS_MAGIC_1 'A'
#define UAOS_MAGIC_2 'O'
#define UAOS_MAGIC_3 'S'
#define UAOS_TYPE_X64   0x0003
#define UAOS_TYPE_M68K  0x0002
#define UAOS_TYPE_NATIVE 0x0001

#define ELFMAG0 0x7F
#define ELFMAG1 'E'
#define ELFMAG2 'L'
#define ELFMAG3 'F'
#define ELFCLASS32 1
#define ELFCLASS64 2
#define EM_68K     4
#define EM_X86_64  62

#define HUNK_HEADER 0x000003F3

static void put_s(const char *s)
{
    uaos_write(1, s, (long)uaos_strlen(s));
}

static uint16_t be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static int is_ascii(const uint8_t *data, long len)
{
    if (len <= 0)
        return 0;
    long printable = 0;
    for (long i = 0; i < len; i++) {
        unsigned char c = data[i];
        if (uaos_isprint(c) || uaos_isspace(c))
            printable++;
    }
    return printable * 10 >= len * 9;
}

static void identify(const char *path, const uint8_t *data, long len)
{
    put_s(path);
    put_s(": ");

    if (len < 4) {
        if (len == 0)
            put_s("empty");
        else
            put_s("data");
        put_s("\n");
        return;
    }

    /* UAOS wrapped binary? */
    if (data[0] == UAOS_MAGIC_0 && data[1] == UAOS_MAGIC_1 &&
        data[2] == UAOS_MAGIC_2 && data[3] == UAOS_MAGIC_3) {
        if (len >= 6) {
            uint16_t type = be16(data + 4);
            if (type == UAOS_TYPE_X64) {
                put_s("UAOS x86-64 ELF64 binary");
            } else if (type == UAOS_TYPE_M68K) {
                put_s("UAOS Amiga Hunk (M68k) binary");
            } else if (type == UAOS_TYPE_NATIVE) {
                put_s("UAOS native shell command");
            } else {
                put_s("UAOS binary (unknown type)");
            }
        } else {
            put_s("UAOS binary");
        }
        put_s("\n");
        return;
    }

    /* ELF? */
    if (data[0] == ELFMAG0 && data[1] == ELFMAG1 &&
        data[2] == ELFMAG2 && data[3] == ELFMAG3) {
        int elfclass = (len >= 5) ? data[4] : 0;
        int elfmach = 0;
        if (elfclass == ELFCLASS64 && len >= 20) {
            elfmach = le16(data + 18);
        } else if (elfclass == ELFCLASS32 && len >= 20) {
            elfmach = le16(data + 18);
        }

        put_s("ELF");
        if (elfclass == ELFCLASS64)
            put_s("64");
        else if (elfclass == ELFCLASS32)
            put_s("32");
        else
            put_s(" (unknown class)");

        if (elfmach == EM_X86_64)
            put_s(" x86-64");
        else if (elfmach == EM_68K)
            put_s(" Motorola 68000");
        else if (elfmach)
            put_s(" (unknown machine)");
        put_s(" executable");
        put_s("\n");
        return;
    }

    /* Raw Amiga Hunk? */
    if (len >= 4 && be32(data) == HUNK_HEADER) {
        put_s("raw Amiga Hunk (M68k) binary");
        put_s("\n");
        return;
    }

    if (is_ascii(data, len))
        put_s("ASCII text");
    else
        put_s("data");
    put_s("\n");
}

static int file_path(const char *path)
{
    struct uaos_stat st;
    if (uaos_stat(path, &st) == 0) {
        if (st.is_dir) {
            put_s(path);
            put_s(": directory\n");
            return 0;
        }
    }

    long fd = uaos_open(path, UAOS_O_RDONLY);
    if (fd < 0) {
        put_s(path);
        put_s(": cannot open\n");
        return 1;
    }

    uint8_t buf[128];
    long n = uaos_read_file((int)fd, buf, sizeof(buf));
    uaos_close((int)fd);

    identify(path, buf, n);
    return 0;
}

int main(int argc, const char **argv)
{
    if (argc < 2) {
        put_s("usage: file file...\n");
        return 1;
    }

    int rc = 0;
    for (int i = 1; i < argc; i++) {
        if (file_path(argv[i]) != 0)
            rc = 1;
    }
    return rc;
}
