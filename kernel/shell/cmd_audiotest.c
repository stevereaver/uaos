/* cmd_audiotest.c — C:audiotest — play a ~1 kHz test tone for ~2 s
 *
 * Exercises the active audio backend end to end and prints the backend
 * name so the routing is visible in the transcript.
 *
 * The stock audio_sine_test() arms a single 64-byte Paula buffer; the
 * chip emulator stops the channel when AUDxLEN expires (no AUDxIRQ
 * reload), which is a ~64 ms blip — it still runs here for pipeline
 * coverage, but it cannot produce a measurable sustained tone.
 * For that, the command soloes the mixer ring (audio_ring_solo) and
 * injects a synthesized 1 kHz / 48 kHz stereo sine for ~2 s, then hands
 * the ring back.  WAV-capture verification expects a dominant ~1 kHz.
 */

#include "cmd_internal.h"
#include "../audio/audio.h"
#include "../audio/audio_test.h"
#include "../chipset/chip_emu.h"
#include <stdint.h>

/* 48 kHz / 1 kHz = exactly 48 samples per cycle — one static table. */
static int16_t g_tone[48];
static int g_tone_built = 0;

static void build_tone(void)
{
    /* Quarter-wave table: q[k] = 23000 * sin(k * 7.5 deg), k = 0..12. */
    static const int16_t q[13] = {
        0, 3002, 5953, 8802, 11500, 14001, 16263,
        18247, 19919, 21249, 22216, 22776, 23000
    };
    for (int i = 0; i < 48; i++) {
        int q4 = i / 12, r = i % 12;   /* r in 0..11; q has 0..12 */
        int16_t v;
        if      (q4 == 0) v =  q[r];
        else if (q4 == 1) v =  q[12 - r];
        else if (q4 == 2) v = (int16_t)-q[r];
        else              v = (int16_t)-q[12 - r];
        g_tone[i] = v;
    }
    g_tone_built = 1;
}

void Cmd_Audiotest(NativeCmdCtx *ctx, const char *args)
{
    (void)args;

    const char *name = audio_backend_name();
    char line[CMD_MAX_LINE];
    cmd_scopy(line, "audiotest: backend=", CMD_MAX_LINE);
    cmd_scat(line, name ? name : "none", CMD_MAX_LINE);
    cmd_scat(line, " — 1 kHz sine, 2 s", CMD_MAX_LINE);
    PRINT(line);

    if (!name) {
        PRINT("audiotest: no audio backend active");
        return;
    }

    audio_sine_test();          /* stock one-shot blip, as the card specced */

    if (!g_tone_built) build_tone();

    /* ~2 s of injected 1 kHz stereo: keep ~4000 frames queued, topping up
     * by however much the backend consumed each slice. */
    audio_ring_solo(1);
    uint32_t phase = 0;
    int16_t buf[2400 * 2];
    /* Flush mixer-queued frames so the injected tone starts now. */
    while (audio_ring_ready())
        audio_ring_read(buf, 2400);
    for (int i = 0; i < 40; i++) {
        CMD_YIELD(ctx, 50);
        unsigned int want = audio_ring_ready();
        unsigned int top = want < 4000 ? 4000 - want : 0;
        if (top > 2400) top = 2400;
        for (unsigned int f = 0; f < top; f++) {
            buf[f * 2 + 0] = g_tone[phase];
            buf[f * 2 + 1] = g_tone[phase];
            phase = (phase + 1) % 48;
        }
        if (top) audio_ring_write(buf, top);
        if (CMD_BREAK(ctx)) {
            PRINT("***Break");
            break;
        }
    }
    audio_ring_solo(0);
    chip_emu_audio_set_dmacon(0x0200u);   /* master stays on, ch0 off */
    PRINT("audiotest: done");
}
