/* gen_library.c — UAOS build tool: generate a .library descriptor file
 *
 * Usage: gen_library <name> <version> <func_count> <output_file>
 *
 * Writes a binary UAOS .library descriptor with the given metadata.
 * The actual native implementation is compiled into the kernel; this
 * file is only a manifest that the boot scanner reads.
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

static void write_be16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xFF);
}

int main(int argc, char *argv[])
{
    if (argc != 5) {
        fprintf(stderr, "Usage: gen_library <name> <version> <func_count> <output_file>\n");
        return 1;
    }

    const char *name     = argv[1];
    uint16_t    version  = (uint16_t)atoi(argv[2]);
    uint16_t    func_cnt = (uint16_t)atoi(argv[3]);
    const char *outpath  = argv[4];

    size_t name_len_raw = strlen(name) + 1;  /* include null terminator */
    uint16_t name_len = (uint16_t)((name_len_raw + 1) & ~1u); /* pad to even */

    size_t file_size = 8 + name_len + 4;
    uint8_t *buf = calloc(1, file_size);
    if (!buf) {
        fprintf(stderr, "gen_library: out of memory\n");
        return 1;
    }

    /* Magic */
    buf[0] = 'U';
    buf[1] = 'A';
    buf[2] = 'O';
    buf[3] = 'S';

    /* Format version */
    write_be16(buf + 4, 1);

    /* Name length */
    write_be16(buf + 6, name_len);

    /* Name (null-terminated, already zero-padded by calloc) */
    memcpy(buf + 8, name, name_len_raw);

    /* Library version */
    write_be16(buf + 8 + name_len, version);

    /* Function count */
    write_be16(buf + 8 + name_len + 2, func_cnt);

    FILE *f = fopen(outpath, "wb");
    if (!f) {
        perror("gen_library: fopen");
        free(buf);
        return 1;
    }
    if (fwrite(buf, 1, file_size, f) != file_size) {
        perror("gen_library: fwrite");
        fclose(f);
        free(buf);
        return 1;
    }
    fclose(f);
    free(buf);
    return 0;
}
