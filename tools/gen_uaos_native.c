/* gen_uaos_native.c — UAOS build tool: generate a NATIVE command binary
 *
 * Usage: gen_uaos_native <name> <output_file>
 *
 * Writes a 32-byte UAOS binary header with type=NATIVE and the given
 * command name into <output_file>.  The shell reads this header, sees
 * UAOS_BIN_TYPE_NATIVE, extracts the name, and calls the matching
 * NativeCmd handler directly.
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

#define UAOS_BIN_TYPE_NATIVE 0x0001U
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
    if (argc != 3) {
        fprintf(stderr, "Usage: gen_uaos_native <name> <output_file>\n");
        return 1;
    }

    const char *name    = argv[1];
    const char *outpath = argv[2];

    if (strlen(name) > 15) {
        fprintf(stderr, "gen_uaos_native: name '%s' too long (max 15 chars)\n", name);
        return 1;
    }

    uint8_t hdr[UAOS_BIN_HEADER_SIZE];
    memset(hdr, 0, sizeof(hdr));

    /* magic */
    hdr[0] = UAOS_BIN_MAGIC_0;
    hdr[1] = UAOS_BIN_MAGIC_1;
    hdr[2] = UAOS_BIN_MAGIC_2;
    hdr[3] = UAOS_BIN_MAGIC_3;

    /* type = NATIVE (big-endian) */
    write_be16(hdr + 4, UAOS_BIN_TYPE_NATIVE);

    /* flags = 0 */
    write_be16(hdr + 6, 0);

    /* payload_size = 0 (no payload for NATIVE) */
    write_be32(hdr + 8, 0);

    /* name (NUL-padded, 16 bytes) */
    strncpy((char *)(hdr + 12), name, 15);
    hdr[27] = '\0';

    /* reserved = 0 (already zeroed) */

    FILE *f = fopen(outpath, "wb");
    if (!f) {
        perror("gen_uaos_native: fopen");
        return 1;
    }
    if (fwrite(hdr, 1, UAOS_BIN_HEADER_SIZE, f) != UAOS_BIN_HEADER_SIZE) {
        perror("gen_uaos_native: fwrite");
        fclose(f);
        return 1;
    }
    fclose(f);
    return 0;
}
