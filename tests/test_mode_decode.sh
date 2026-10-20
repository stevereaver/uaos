#!/bin/sh
# test_mode_decode.sh — host-side unit test for the display-mode decoder.
#
# Extracts the real UaosDisplayMode table and uaos_gfx_mode_dims() out of
# kernel/exec/graphics_lib.c (pattern-anchored, so line numbers may drift)
# and checks nominal raster dims for every monitor family and the classic
# composite keys.  No QEMU needed.
#
# Run: tests/test_mode_decode.sh     Exit 0 = all checks passed.

set -e
REPO=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
SRC="$REPO/kernel/exec/graphics_lib.c"
WORK="${TMPDIR:-/tmp}/uaos_mode_decode.$$.c"
trap 'rm -f "$WORK" "${WORK%.c}"' EXIT

{
    echo '#include <stdint.h>'
    echo '#include <stdio.h>'
    echo '#include <string.h>'
    echo '#include <stddef.h>'
    sed -n '/^#define AGA_FLAG/,/^#define MON_UAOSRTG/p' "$SRC"
    awk '/^typedef struct/{buf=""} {buf=buf $0 "\n"} /^} UaosDisplayMode;/{printf "%s", buf; buf=""}' "$SRC"
    sed -n '/^static const UaosDisplayMode uaos_modes/,/^#define UAOS_MODE_COUNT/p' "$SRC"
    sed -n '/^int uaos_gfx_mode_dims.*[^(;]$/,/^}/p' "$SRC"
    cat <<'EOF'
typedef struct { uint32_t id; int expect_ok; int w, h; const char *label; } T;
int main(void)
{
    T tests[] = {
        { 0x00000000, 1, 320,256,  "LORES_KEY (default->PAL)" },
        { 0x00008000, 1, 640,256,  "HIRES_KEY" },
        { 0x00008004, 1, 640,512,  "HIRESLACE_KEY" },
        { 0x00000004, 1, 320,512,  "LORESLACE_KEY" },
        { 0x00008020, 1,1280,256,  "SUPER_KEY" },
        { 0x00000800, 1, 320,256,  "HAM_KEY" },
        { 0x00000400, 1, 320,256,  "LORESDPF_KEY" },
        { 0x00000440, 1, 320,256,  "LORESDPF2_KEY" },
        { 0x00000080, 1, 320,256,  "EXTRAHALFBRITE_KEY" },
        { 0x00008021, 1, 640,480,  "PRODUCTIVITY_KEY" },
        { 0x00008025, 1, 640,960,  "PRODUCTIVITYLACE_KEY" },
        { 0x00011000, 1, 320,200,  "NTSC|LORES" },
        { 0x00011004, 1, 320,400,  "NTSC|LORESLACE" },
        { 0x00019004, 1, 640,400,  "NTSC|HIRESLACE" },
        { 0x00019020, 1,1280,200,  "NTSC|SUPER" },
        { 0x00021000, 1, 320,256,  "PAL|LORES" },
        { 0x00021004, 1, 320,512,  "PAL|LORESLACE" },
        { 0x00029004, 1, 640,512,  "PAL|HIRESLACE" },
        { 0x00021804, 1, 320,512,  "PAL|HAMLACE" },
        { 0x00021800, 1, 320,256,  "PAL|HAM" },
        { 0x00021444, 1, 320,512,  "PAL|LORESLACEDPF2" },
        { 0x00029024, 1,1280,512,  "PAL|SUPERLACE" },
        { 0x00091000, 1, 320,400,  "DBLNTSC|LORES" },
        { 0x00091004, 1, 320,400,  "DBLNTSC|LORESFF" },
        { 0x00091005, 1, 320,800,  "DBLNTSC|LORESLACE" },
        { 0x00099000, 1, 640,400,  "DBLNTSC|HIRES" },
        { 0x00099005, 1, 640,800,  "DBLNTSC|HIRESLACE" },
        { 0x00091200, 1, 320,400,  "DBLNTSC|EXTRALORES" },
        { 0x000a1000, 1, 320,512,  "DBLPAL|LORES" },
        { 0x000a9005, 1, 640,1024, "DBLPAL|HIRESLACE" },
        { 0x00031004, 1, 640,480,  "VGA|EXTRALORES" },
        { 0x00039024, 1, 640,480,  "VGA|PRODUCT" },
        { 0x00039025, 1, 640,960,  "VGA|PRODUCTLACE" },
        { 0x00031804, 1, 640,480,  "VGA|HAM" },
        { 0x00061004, 1, 640,400,  "EURO72|EXTRALORES" },
        { 0x00069024, 1, 640,400,  "EURO72|PRODUCT" },
        { 0x00069025, 1, 640,800,  "EURO72|PRODUCTLACE" },
        { 0x00061804, 1, 640,400,  "EURO72|HAM" },
        { 0x00071000, 1, 320,200,  "EURO36|LORES" },
        { 0x00079024, 1,1280,400,  "EURO36|SUPERLACE" },
        { 0x00081000, 1, 400,300,  "SUPER72|LORES" },
        { 0x00089000, 1, 400,300,  "SUPER72|HIRES" },
        { 0x00089020, 1, 800,300,  "SUPER72|SUPER" },
        { 0x00089024, 1, 800,600,  "SUPER72|SUPERLACE" },
        { 0x00089028, 1, 800,600,  "SUPER72|SUPERDBL" },
        { 0x00041000, 1,1024,1024, "A2024 10Hz" },
        { 0x00049000, 1,1024,800,  "A2024 15Hz" },
        { 0x40001002, 1,1280,720,  "RTG:1280x720" },
        { 0x0000ffff, 0,   0,  0,  "nonstandard monitor 0x1000" },
        { 0x000b1000, 0,   0,  0,  "unknown monitor 0xb" },
        { 0xffffffff, 0,   0,  0,  "INVALID_ID" },
    };
    int fails = 0;
    size_t n = sizeof(tests) / sizeof(tests[0]);
    for (size_t i = 0; i < n; i++) {
        uint16_t w = 0, h = 0;
        int ok = uaos_gfx_mode_dims(tests[i].id, &w, &h);
        int pass = (ok == tests[i].expect_ok) && (!ok || (w == tests[i].w && h == tests[i].h));
        if (!pass) fails++;
        printf("[%s] %-32s id=0x%08x -> ok=%d w=%d h=%d (want %d %dx%d)\n",
               pass ? "PASS" : "FAIL", tests[i].label, tests[i].id,
               ok, w, h, tests[i].expect_ok, tests[i].w, tests[i].h);
    }
    printf("%d/%d passed\n", (int)n - fails, (int)n);
    return fails ? 1 : 0;
}
EOF
} > "$WORK"

cc -O0 -Wall -o "${WORK%.c}" "$WORK"
"${WORK%.c}"
