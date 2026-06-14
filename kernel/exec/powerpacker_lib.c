/*
 * powerpacker_lib.c — UAOS powerpacker.library Implementation
 *
 * Native implementation of PowerPacker decompression / decryption
 * routines for the m68k emulation layer.
 *
 * All functions read their arguments from the Musashi m68k register
 * file and write return values back to D0.
 */

#include "powerpacker_lib.h"
#include "loadable_lib.h"
#include "boot/kprint.h"
#include <stdint.h>
#include <stddef.h>

/* =========================================================================
 * Musashi register access (provided by emulation/uaos_m68k_glue.c)
 * ========================================================================= */

extern unsigned int m68k_get_reg(void *context, int reg);
extern void         m68k_set_reg(int reg, unsigned int value);

#define M68K_REG_D0  0
#define M68K_REG_D1  1
#define M68K_REG_A0  8
#define M68K_REG_A1  9

/* =========================================================================
 * powerpacker.library function indices (AmigaOS LVO convention)
 * ========================================================================= */

#define PP_LOAD_DATA          1   /* ppLoadData()       LVO -30 */
#define PP_DECRYPT            2   /* ppDecrypt()        LVO -36 */
#define PP_GET_PASSWORD_INFO  3   /* ppGetPasswordInfo() LVO -42 */
#define PP_CALC_CHECKSUM      4   /* ppCalcChecksum()    LVO -48 */

/* =========================================================================
 * Stubs
 * ========================================================================= */

static void pp_LoadData(void)
{
    /* D0 = source ptr, D1 = dest ptr, A0 = buffer size
     * Returns: success BOOL in D0 */
    kprint("[PP] ppLoadData() stub\n");
    m68k_set_reg(M68K_REG_D0, 0);  /* FALSE — not implemented yet */
}

static void pp_Decrypt(void)
{
    /* D0 = data ptr, D1 = length, A0 = password ptr
     * Returns: success BOOL in D0 */
    kprint("[PP] ppDecrypt() stub\n");
    m68k_set_reg(M68K_REG_D0, 0);
}

static void pp_GetPasswordInfo(void)
{
    /* D0 = data ptr
     * Returns: password info ptr in D0 */
    kprint("[PP] ppGetPasswordInfo() stub\n");
    m68k_set_reg(M68K_REG_D0, 0);
}

static void pp_CalcChecksum(void)
{
    /* D0 = data ptr, D1 = length
     * Returns: checksum in D0 */
    kprint("[PP] ppCalcChecksum() stub\n");
    m68k_set_reg(M68K_REG_D0, 0);
}

/* =========================================================================
 * Dispatch
 * ========================================================================= */

void UAOS_POWERPACKER_Dispatch(uint32_t fn_idx)
{
    switch (fn_idx) {
        case PP_LOAD_DATA:          pp_LoadData();          break;
        case PP_DECRYPT:            pp_Decrypt();            break;
        case PP_GET_PASSWORD_INFO:  pp_GetPasswordInfo();    break;
        case PP_CALC_CHECKSUM:      pp_CalcChecksum();       break;
        default:
            kprint("[PP] Unknown function index ");
            kprintdec(fn_idx);
            kprint("\n");
            break;
    }
}

/* =========================================================================
 * Registration
 * ========================================================================= */

void UAOS_POWERPACKER_Register(void)
{
    int rc = UAOS_LoadableLib_BindDispatch("powerpacker.library",
                                           UAOS_POWERPACKER_Dispatch);
    if (rc != 0) {
        kprint("[PP] powerpacker.library not found in LIBS: (no .library file)\n");
    }
}
