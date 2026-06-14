/* gen_m68k_library.c — UAOS build tool: generate a real M68k binary .library
 *
 * Usage: gen_m68k_library <name> <version> <func_count> <output_file>
 *
 * Generates a .library file containing actual m68k machine code.
 * The binary is loaded into guest RAM at boot and executed by the
 * Musashi m68k CPU emulator — no native C stubs.
 *
 * Binary layout (big-endian where multi-byte):
 *   0-3    Magic "UAOS"
 *   4      Format version (2)
 *   5      Reserved
 *   6-7    Function count
 *   8-9    Library version
 *   10-11  Code size in bytes
 *   12-15  Library base offset from load address (= 30 + (N-1)*6)
 *   16-63  Name (null-terminated, padded)
 *   64+    M68k code — LVO table with real m68k instructions
 *
 * M68k code layout (each function = 6 bytes):
 *   funcN at lowest address  (LVO = -(30 + (N-1)*6))
 *   ...
 *   func1 at highest address (LVO = -30)
 *
 * Each LVO entry:
 *   MOVEQ #0, D0   (0x70 0x00) — return FALSE / 0
 *   NOP            (0x4E 0x71) — padding
 *   RTS            (0x4E 0x75) — return
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#define UAOS_LIB_MAGIC_0 'U'
#define UAOS_LIB_MAGIC_1 'A'
#define UAOS_LIB_MAGIC_2 'O'
#define UAOS_LIB_MAGIC_3 'S'

#define HEADER_SIZE   64
#define LVO_ENTRY_SZ  6

static void write_be16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xFF);
}

static void write_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)((v >> 16) & 0xFF);
    p[2] = (uint8_t)((v >>  8) & 0xFF);
    p[3] = (uint8_t)(v & 0xFF);
}

int main(int argc, char *argv[])
{
    if (argc != 5) {
        fprintf(stderr, "Usage: gen_m68k_library <name> <version> <func_count> <output_file>\n");
        return 1;
    }

    const char *name     = argv[1];
    uint16_t    version  = (uint16_t)atoi(argv[2]);
    uint16_t    func_cnt = (uint16_t)atoi(argv[3]);
    const char *outpath  = argv[4];

    if (func_cnt < 1 || func_cnt > 64) {
        fprintf(stderr, "gen_m68k_library: func_count must be 1..64\n");
        return 1;
    }
    if (strlen(name) > 47) {
        fprintf(stderr, "gen_m68k_library: name too long (max 47)\n");
        return 1;
    }

    uint32_t code_size = (uint32_t)func_cnt * LVO_ENTRY_SZ;
    uint32_t base_offset = 30 + ((uint32_t)func_cnt - 1) * LVO_ENTRY_SZ;
    uint32_t file_size = HEADER_SIZE + code_size;

    uint8_t *buf = calloc(1, file_size);
    if (!buf) {
        fprintf(stderr, "gen_m68k_library: out of memory\n");
        return 1;
    }

    /* Magic */
    buf[0] = UAOS_LIB_MAGIC_0;
    buf[1] = UAOS_LIB_MAGIC_1;
    buf[2] = UAOS_LIB_MAGIC_2;
    buf[3] = UAOS_LIB_MAGIC_3;

    /* Format version */
    buf[4] = 2;
    buf[5] = 0;

    /* Function count */
    write_be16(buf + 6, func_cnt);

    /* Library version */
    write_be16(buf + 8, version);

    /* Code size */
    write_be16(buf + 10, (uint16_t)code_size);

    /* Library base offset */
    write_be32(buf + 12, base_offset);

    /* Name */
    memcpy(buf + 16, name, strlen(name) + 1);

    /* M68k code — LVO entries in reverse order:
     * funcN first (lowest address), func1 last (highest address)
     */
    for (int i = 0; i < (int)func_cnt; i++) {
        uint32_t off = HEADER_SIZE + (uint32_t)i * LVO_ENTRY_SZ;
        /* MOVEQ #0, D0 */
        buf[off + 0] = 0x70;
        buf[off + 1] = 0x00;
        /* NOP */
        buf[off + 2] = 0x4E;
        buf[off + 3] = 0x71;
        /* RTS */
        buf[off + 4] = 0x4E;
        buf[off + 5] = 0x75;
    }

    FILE *f = fopen(outpath, "wb");
    if (!f) {
        perror("gen_m68k_library: fopen");
        free(buf);
        return 1;
    }
    if (fwrite(buf, 1, file_size, f) != file_size) {
        perror("gen_m68k_library: fwrite");
        fclose(f);
        free(buf);
        return 1;
    }
    fclose(f);
    free(buf);

    fprintf(stderr, "gen_m68k_library: %s (%u funcs, %u bytes)\n",
            outpath, (unsigned)func_cnt, (unsigned)file_size);
    return 0;
}
