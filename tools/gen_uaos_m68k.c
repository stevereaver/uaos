/* gen_uaos_m68k.c — UAOS build tool: wrap an Amiga Hunk binary
 *
 * Usage: gen_uaos_m68k <name> <hunk_binary> <output_file>
 *
 * Prepends a 32-byte UAOS binary header (type=M68K) to <hunk_binary>
 * and writes the result to <output_file>.
 *
 * The shell reads the header, sees UAOS_BIN_TYPE_M68K, and passes the
 * payload bytes directly to UAOS_Emu_LoadAndRun — no 'run' prefix needed.
 *
 * Compiled and run at build time on the host (standard C, not freestanding).
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

#define UAOS_BIN_MAGIC_0 'U'
#define UAOS_BIN_MAGIC_1 'A'
#define UAOS_BIN_MAGIC_2 'O'
#define UAOS_BIN_MAGIC_3 'S'

#define UAOS_BIN_TYPE_M68K   0x0002U
#define UAOS_BIN_HEADER_SIZE 32

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
    if (argc != 4) {
        fprintf(stderr, "Usage: gen_uaos_m68k <name> <hunk_binary> <output_file>\n");
        return 1;
    }

    const char *name     = argv[1];
    const char *hunkpath = argv[2];
    const char *outpath  = argv[3];

    if (strlen(name) > 15) {
        fprintf(stderr, "gen_uaos_m68k: name '%s' too long (max 15 chars)\n", name);
        return 1;
    }

    /* Read the Hunk binary */
    FILE *fin = fopen(hunkpath, "rb");
    if (!fin) {
        perror("gen_uaos_m68k: fopen input");
        return 1;
    }
    fseek(fin, 0, SEEK_END);
    long hunk_size = ftell(fin);
    fseek(fin, 0, SEEK_SET);
    if (hunk_size <= 0) {
        fprintf(stderr, "gen_uaos_m68k: empty or unreadable input\n");
        fclose(fin);
        return 1;
    }
    uint8_t *payload = malloc((size_t)hunk_size);
    if (!payload) {
        fprintf(stderr, "gen_uaos_m68k: out of memory\n");
        fclose(fin);
        return 1;
    }
    if (fread(payload, 1, (size_t)hunk_size, fin) != (size_t)hunk_size) {
        perror("gen_uaos_m68k: fread");
        fclose(fin);
        free(payload);
        return 1;
    }
    fclose(fin);

    /* Build UAOS header */
    uint8_t hdr[UAOS_BIN_HEADER_SIZE];
    memset(hdr, 0, sizeof(hdr));

    hdr[0] = UAOS_BIN_MAGIC_0;
    hdr[1] = UAOS_BIN_MAGIC_1;
    hdr[2] = UAOS_BIN_MAGIC_2;
    hdr[3] = UAOS_BIN_MAGIC_3;

    write_be16(hdr + 4, UAOS_BIN_TYPE_M68K);
    write_be16(hdr + 6, 0);
    write_be32(hdr + 8, (uint32_t)hunk_size);
    strncpy((char *)(hdr + 12), name, 15);
    hdr[27] = '\0';

    /* Write output */
    FILE *fout = fopen(outpath, "wb");
    if (!fout) {
        perror("gen_uaos_m68k: fopen output");
        free(payload);
        return 1;
    }
    if (fwrite(hdr,     1, UAOS_BIN_HEADER_SIZE, fout) != UAOS_BIN_HEADER_SIZE ||
        fwrite(payload, 1, (size_t)hunk_size,     fout) != (size_t)hunk_size) {
        perror("gen_uaos_m68k: fwrite");
        fclose(fout);
        free(payload);
        return 1;
    }
    fclose(fout);
    free(payload);
    return 0;
}
