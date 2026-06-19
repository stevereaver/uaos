/* gen_uaos_x64.c — UAOS build tool: wrap an x86-64 ELF64 binary
 *
 * Usage: gen_uaos_x64 <name> <elf64_binary> <output_file>
 *
 * Prepends a 32-byte UAOS binary header (type=X64) to <elf64_binary>
 * and writes the result to <output_file>.
 *
 * The shell reads the header, sees UAOS_BIN_TYPE_X64, and (once the
 * x86-64 loader is implemented) runs the ELF64 payload natively.
 *
 * Compiled and run at build time on the host (standard C, not freestanding).
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

#include "../kernel/exec/uaos_binary.h"

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
        fprintf(stderr, "Usage: gen_uaos_x64 <name> <elf64_binary> <output_file>\n");
        return 1;
    }

    const char *name    = argv[1];
    const char *elfpath = argv[2];
    const char *outpath = argv[3];

    if (strlen(name) > 15) {
        fprintf(stderr, "gen_uaos_x64: name '%s' too long (max 15 chars)\n", name);
        return 1;
    }

    /* Read the ELF64 binary */
    FILE *fin = fopen(elfpath, "rb");
    if (!fin) {
        perror("gen_uaos_x64: fopen input");
        return 1;
    }
    fseek(fin, 0, SEEK_END);
    long elf_size = ftell(fin);
    fseek(fin, 0, SEEK_SET);
    if (elf_size <= 0) {
        fprintf(stderr, "gen_uaos_x64: empty or unreadable input\n");
        fclose(fin);
        return 1;
    }
    uint8_t *payload = malloc((size_t)elf_size);
    if (!payload) {
        fprintf(stderr, "gen_uaos_x64: out of memory\n");
        fclose(fin);
        return 1;
    }
    if (fread(payload, 1, (size_t)elf_size, fin) != (size_t)elf_size) {
        perror("gen_uaos_x64: fread");
        fclose(fin);
        free(payload);
        return 1;
    }
    fclose(fin);

    /* Build UAOS header */
    uint8_t hdr[UAOS_BIN_HEADER_SIZE];
    memset(hdr, 0, sizeof(hdr));

    hdr[0] = (uint8_t)(UAOS_BIN_MAGIC >> 24);
    hdr[1] = (uint8_t)(UAOS_BIN_MAGIC >> 16);
    hdr[2] = (uint8_t)(UAOS_BIN_MAGIC >>  8);
    hdr[3] = (uint8_t)(UAOS_BIN_MAGIC      );

    write_be16(hdr + 4, UAOS_BIN_TYPE_X64);
    write_be16(hdr + 6, 0);
    write_be32(hdr + 8, (uint32_t)elf_size);
    strncpy((char *)(hdr + 12), name, 15);
    hdr[27] = '\0';

    /* Write output */
    FILE *fout = fopen(outpath, "wb");
    if (!fout) {
        perror("gen_uaos_x64: fopen output");
        free(payload);
        return 1;
    }
    if (fwrite(hdr,     1, UAOS_BIN_HEADER_SIZE, fout) != UAOS_BIN_HEADER_SIZE ||
        fwrite(payload, 1, (size_t)elf_size,     fout) != (size_t)elf_size) {
        perror("gen_uaos_x64: fwrite");
        fclose(fout);
        free(payload);
        return 1;
    }
    fclose(fout);
    free(payload);
    return 0;
}
