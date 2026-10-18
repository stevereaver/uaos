/* cmd_hdadump.c — C:hdadump — live HDA codec/widget state dump
 *
 *   hdadump          one snapshot of every configured pin->DAC path
 *
 * For each path widget prints: type (0=DAC 2=mixer 3=selector 4=pin),
 * power state, out-amp readback (bit7 = mute, low 7 bits = gain step),
 * pin ctl / EAPD / defcfg device type, and the DAC's programmed format.
 * Runs real verbs through CORB/RIRB — answers "is the codec really
 * unmuted / is the pin really in output mode" on metal.
 */

#include "cmd_internal.h"
#include "../audio/audio.h"

static void hd_emit(const char *line, void *ud)
{
    NativeCmdCtx *ctx = (NativeCmdCtx *)ud;
    PRINT(line);
    (void)ctx;
}

void Cmd_HdaDump(NativeCmdCtx *ctx, const char *args)
{
    (void)args;
    HDA_DumpState(hd_emit, ctx);
}
