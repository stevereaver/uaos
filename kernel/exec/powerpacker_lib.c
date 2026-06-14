/*
 * powerpacker_lib.c — UAOS powerpacker.library Implementation
 *
 * Native implementation of PowerPacker decompression / decryption
 * routines for the m68k emulation layer.
 *
 * Based on pplib 1.2 by Stuart Caie (Public Domain)
 * and powerpacker.library by Nico Francois / Kjetil Hvalstrand
 *
 * All functions read their arguments from the Musashi m68k register
 * file and write return values back to D0.
 */

#include "powerpacker_lib.h"
#include "loadable_lib.h"
#include "boot/kprint.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* =========================================================================
 * Musashi register access (provided by emulation/uaos_m68k_glue.c)
 * ========================================================================= */

extern unsigned int m68k_get_reg(void *context, int reg);
extern void         m68k_set_reg(int reg, unsigned int value);

#define M68K_REG_D0  0
#define M68K_REG_D1  1
#define M68K_REG_D2  2
#define M68K_REG_D3  3
#define M68K_REG_A0  8
#define M68K_REG_A1  9
#define M68K_REG_A2  10
#define M68K_REG_A3  11
#define M68K_REG_A6  14

/* =========================================================================
 * Core algorithms (from pplib 1.2 - Public Domain)
 * ========================================================================= */

/**
 * Calculates the 16-bit checksum of a password.
 * This checksum is stored in the header of an encrypted PowerPacker file.
 */
uint32_t ppCalcChecksum(const uint8_t* password)
{
    uint32_t cksum = 0;
    uint8_t c, shift;

    /* for each byte in the password */
    while ((c = *password++)) {
        /* barrel-shift the 16 bit checksum right by [c] bits */
        shift = c & 0x0F;
        if (shift) {
            cksum = (cksum >> shift) | (cksum << (16 - shift));
        }
        /* add c to the cksum, with 16 bit wrap */
        cksum = (cksum + c) & 0xFFFF;
    }
    return cksum;
}

/**
 * Calculates a 32-bit decryption key from a password.
 * The key is used with ppDecrypt() to decrypt a file.
 */
uint32_t ppCalcPasskey(const uint8_t* password)
{
    uint32_t key = 0;
    uint8_t c;

    /* for each byte in the password */
    while ((c = *password++)) {
        /* rotate 32 bit key left by one bit */
        key = (key << 1) | (key >> (32 - 1));
        key &= 0xFFFFFFFF;
        /* add c to the key, with 32 bit wrap */
        key = (key + c) & 0xFFFFFFFF;
        /* swap lower and upper 16 bits */
        key = (key << 16) | (key >> 16);
        key &= 0xFFFFFFFF;
    }
    return key;
}

/**
 * Decrypts encrypted PowerPacker data.
 * PowerPacker encrypts data by XORing it with a 32-bit key.
 */
void ppDecrypt(uint8_t* data, uint32_t len, uint32_t key)
{
    uint8_t k0 = (key >> 24) & 0xFF;
    uint8_t k1 = (key >> 16) & 0xFF;
    uint8_t k2 = (key >> 8) & 0xFF;
    uint8_t k3 = (key) & 0xFF;

    len = ((len + 3) >> 2) - 1;

    /* XOR data with key */
    do {
        *data++ ^= k0;
        *data++ ^= k1;
        *data++ ^= k2;
        *data++ ^= k3;
    } while (len--);
}

/* Decrunch macros */
#define PP_READ_BITS(nbits, var) do { \
    bit_cnt = (nbits); (var) = 0; \
    while (bits_left < bit_cnt) { \
        if (buf < src) return 0; /* out of source bits */ \
        bit_buffer |= *--buf << bits_left; \
        bits_left += 8; \
    } \
    bits_left -= bit_cnt; \
    while (bit_cnt--) { \
        (var) = ((var) << 1) | (bit_buffer & 1); \
        bit_buffer >>= 1; \
    } \
} while (0)

#define PP_BYTE_OUT(byte) do { \
    if (out <= dest) return 0; /* output overflow */ \
    *--out = (byte); written++; \
} while (0)

/**
 * Main decrunch algorithm.
 */
static int ppDecrunchBuffer_main(const uint8_t* eff, const uint8_t* src,
                                  uint8_t* dest, uint32_t src_len,
                                  uint32_t dest_len, const uint32_t litbit)
{
    const uint8_t* buf = &src[src_len];
    uint8_t* out = &dest[dest_len];
    uint8_t* dest_end = out;
    uint32_t bit_buffer = 0, x, todo, offbits, offset = 0, written = 0;
    uint8_t bits_left = 0, bit_cnt;

    if (src == NULL || dest == NULL) {
        return 0;
    }

    /* skip the first few bits */
    PP_READ_BITS(src[src_len + 3], x);

    /* while we still have output to unpack */
    while (written < dest_len) {
        PP_READ_BITS(1, x);
        if (x == litbit) {
            todo = 1;
            do {
                PP_READ_BITS(2, x);
                todo += x;
            } while (x == 3);
            while (todo--) {
                PP_READ_BITS(8, x);
                PP_BYTE_OUT(x);
            }
            /* should we end decoding on a literal, break out */
            if (written == dest_len) break;
        }

        /* match */
        PP_READ_BITS(2, x);
        offbits = eff[x];
        todo = x + 2;
        if (x == 3) {
            PP_READ_BITS(1, x);
            if (x == 0) offbits = 7;
            PP_READ_BITS(offbits, offset);
            do {
                PP_READ_BITS(3, x);
                todo += x;
            } while (x == 7);
        } else {
            PP_READ_BITS(offbits, offset);
        }

        if (&out[offset] >= dest_end) {
            return 0; /* match overflow */
        }

        while (todo--) {
            x = out[offset];
            PP_BYTE_OUT(x);
        }
    }

    /* all output bytes written without error */
    return 1;
}

/**
 * Decrunches PowerPacked data.
 */
int ppDecrunchBuffer(const uint8_t* eff, const uint8_t* src,
                     uint8_t* dest, uint32_t src_len, uint32_t dest_len)
{
    return ppDecrunchBuffer_main(eff, src, dest, src_len, dest_len, 0);
}

/**
 * Decrunches 'master mode' PowerPacked data.
 */
int ppDecrunchBuffer_m(const uint8_t* eff, const uint8_t* src,
                       uint8_t* dest, uint32_t src_len, uint32_t dest_len)
{
    return ppDecrunchBuffer_main(eff, src, dest, src_len, dest_len, 1);
}

/**
 * Returns error message string for an error code.
 */
const char* ppErrorMessage(int32_t errorcode)
{
    switch (errorcode) {
        case PPERR_OK:         return "No error";
        case PPERR_OPEN:       return "Error opening file";
        case PPERR_READ:       return "Error reading from file";
        case PPERR_NOMEMORY:   return "Out of memory";
        case PPERR_PASSWORD:   return "Bad or missing password";
        case PPERR_UNKNOWN:    return "Unknown error";
        default:               return "Unknown error code";
    }
}

/* =========================================================================
 * File format detection and loading
 * ========================================================================= */

/* PowerPacker file format identifiers */
#define PP20_MAGIC  0x50503230  /* "PP20" */
#define PP2O_MAGIC  0x5050324F  /* "PP2O" */
#define PACK_MAGIC  0x5041434B  /* "PACK" */
#define MLDC_MAGIC  0x4D4C4443  /* "MLDC" */
#define DENB_MAGIC  0x44454E21  /* "DEN!" */
#define MD12_MAGIC  0x4D443132  /* "MD12" */
#define GAZB_MAGIC  0x47415A21  /* "GAZ!" */
#define XX50_MAGIC  0x58583530  /* "XX50" */
#define LR88_MAGIC  0x4C523838  /* "LR88" */
#define PPLS_MAGIC  0x50504C53  /* "PPLS" */
#define PX20_MAGIC  0x50583230  /* "PX20" */

/**
 * Internal function to load and decrunch PowerPacked data from a file.
 * This is a simplified version - full implementation would use UAOS filesystem.
 */
static int32_t __ppLoadData(const char* filename, uint8_t** bufptr,
                            uint32_t* buflenptr,
                            bool (*password_func)(uint8_t*, uint32_t))
{
    /* For now, return error - full implementation needs filesystem integration */
    (void)filename;
    (void)bufptr;
    (void)buflenptr;
    (void)password_func;
    return PPERR_OPEN;
}

/**
 * Loads PowerPacked data from a file.
 */
int32_t ppLoadData(const char* filename, uint8_t** bufferptr,
                   uint32_t* buflenptr,
                   bool (*password_func)(uint8_t*, uint32_t))
{
    return __ppLoadData(filename, bufferptr, buflenptr, password_func);
}

/* =========================================================================
 * m68k Interface Stubs
 * ========================================================================= */

/**
 * ppLoadData() - Load and decrunch PowerPacked file
 * D0 = filename ptr, D1 = buffer ptr ptr, D2 = length ptr
 * A0 = password function (optional), A1 = memtype
 * Returns: error code in D0 (0 = success)
 */
static void pp_LoadData(void)
{
    uint32_t filename_ptr = m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t memtype      = m68k_get_reg(NULL, M68K_REG_D0);
    uint32_t bufferpptr   = m68k_get_reg(NULL, M68K_REG_A1);
    uint32_t lenptr       = m68k_get_reg(NULL, M68K_REG_A2);
    uint32_t passfunc     = m68k_get_reg(NULL, M68K_REG_A3);

    (void)memtype; (void)passfunc;

    kprint("[PP] ppLoadData() called\n");

    /* For m68k: we need to access memory through the emulator
     * For now, return not implemented */
    if (filename_ptr == 0 || bufferpptr == 0 || lenptr == 0) {
        m68k_set_reg(M68K_REG_D0, PPERR_ARGS);
        return;
    }

    /* Return PPERR_OPEN - file loading requires filesystem integration */
    m68k_set_reg(M68K_REG_D0, PPERR_OPEN);
}

/**
 * ppDecrunchBuffer() - Decrunch PowerPacked data in memory
 * A0 = eff ptr, A1 = src ptr, A2 = dest ptr
 * D0 = src_len, D1 = dest_len
 * Returns: success BOOL in D0 (0 = fail, 1 = success)
 */
static void pp_DecrunchBuffer(void)
{
    uint32_t eff_ptr   = m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t src_ptr   = m68k_get_reg(NULL, M68K_REG_A1);
    uint32_t dest_ptr  = m68k_get_reg(NULL, M68K_REG_A2);
    uint32_t src_len   = m68k_get_reg(NULL, M68K_REG_D0);
    uint32_t dest_len  = m68k_get_reg(NULL, M68K_REG_D1);

    int result;

    /* Validate pointers */
    if (eff_ptr == 0 || src_ptr == 0 || dest_ptr == 0) {
        m68k_set_reg(M68K_REG_D0, 0);
        return;
    }

    /* For m68k emulation, we need to translate addresses
     * The actual implementation would map m68k addresses to host memory */
    kprint("[PP] ppDecrunchBuffer() - m68k addr translation needed\n");

    /* For now, return failure - needs proper m68k memory mapping */
    result = 0;
    m68k_set_reg(M68K_REG_D0, result);
}

/**
 * ppCalcChecksum() - Calculate password checksum
 * A0 = password string ptr
 * Returns: checksum in D0
 */
static void pp_CalcChecksum(void)
{
    uint32_t pass_ptr = m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t result = 0;

    if (pass_ptr != 0) {
        /* Need to read from m68k memory - for now compute on host side
         * if we can access it (during testing) */
        const uint8_t* password = (const uint8_t*)(uintptr_t)pass_ptr;
        result = ppCalcChecksum(password);
    }

    m68k_set_reg(M68K_REG_D0, result);
}

/**
 * ppCalcPasskey() - Calculate decryption key from password
 * A0 = password string ptr
 * Returns: passkey in D0
 */
static void pp_CalcPasskey(void)
{
    uint32_t pass_ptr = m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t result = 0;

    if (pass_ptr != 0) {
        const uint8_t* password = (const uint8_t*)(uintptr_t)pass_ptr;
        result = ppCalcPasskey(password);
    }

    m68k_set_reg(M68K_REG_D0, result);
}

/**
 * ppDecrypt() - Decrypt PowerPacked data
 * A0 = data ptr, D0 = length, D1 = key
 */
static void pp_Decrypt(void)
{
    uint32_t data_ptr = m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t len      = m68k_get_reg(NULL, M68K_REG_D0);
    uint32_t key      = m68k_get_reg(NULL, M68K_REG_D1);

    if (data_ptr != 0 && len > 0) {
        uint8_t* data = (uint8_t*)(uintptr_t)data_ptr;
        ppDecrypt(data, len, key);
    }

    m68k_set_reg(M68K_REG_D0, 0);
}

/**
 * ppGetPassword() - Get password from user (stub)
 */
static void pp_GetPassword(void)
{
    kprint("[PP] ppGetPassword() stub - not implemented\n");
    m68k_set_reg(M68K_REG_D0, 0);
}

/**
 * ppErrorMessage() - Get error message string
 * A0 = error code
 * Returns: message string ptr in D0
 */
static void pp_ErrorMessage(void)
{
    int32_t errorcode = (int32_t)m68k_get_reg(NULL, M68K_REG_D0);
    const char* msg = ppErrorMessage(errorcode);

    /* Return host address - m68k can't use this directly
     * A full implementation would need to copy to m68k-accessible memory */
    m68k_set_reg(M68K_REG_D0, (uint32_t)(uintptr_t)msg);
}

/* Stub functions for unimplemented features */
static void pp_Obsolete(void)
{
    kprint("[PP] Obsolete function called\n");
    m68k_set_reg(M68K_REG_D0, 0);
}

static void pp_OverlayDecrHdr(void)
{
    kprint("[PP] ppOverlayDecrHdr() stub\n");
    m68k_set_reg(M68K_REG_D0, 0);
}

static void pp_AllocCrunchInfo(void)
{
    kprint("[PP] ppAllocCrunchInfo() stub - compression not supported\n");
    m68k_set_reg(M68K_REG_D0, 0);
}

static void pp_FreeCrunchInfo(void)
{
    kprint("[PP] ppFreeCrunchInfo() stub\n");
}

static void pp_CrunchBuffer(void)
{
    kprint("[PP] ppCrunchBuffer() stub - compression not supported\n");
    m68k_set_reg(M68K_REG_D0, 0);
}

static void pp_WriteDataHeader(void)
{
    kprint("[PP] ppWriteDataHeader() stub\n");
    m68k_set_reg(M68K_REG_D0, 0);
}

static void pp_EnterPassword(void)
{
    kprint("[PP] ppEnterPassword() stub\n");
    m68k_set_reg(M68K_REG_D0, 0);
}

static void pp_LoadData2(void)
{
    kprint("[PP] ppLoadData2() stub\n");
    m68k_set_reg(M68K_REG_D0, PPERR_ARGS);
}

static void pp_CrunchBufferDest(void)
{
    kprint("[PP] ppCrunchBufferDest() stub - compression not supported\n");
    m68k_set_reg(M68K_REG_D0, 0);
}

/* =========================================================================
 * Dispatch
 * ========================================================================= */

void UAOS_POWERPACKER_Dispatch(uint32_t fn_idx)
{
    switch (fn_idx) {
        case PP_LOAD_DATA:          pp_LoadData();          break;
        case PP_DECRUNCH_BUFFER:    pp_DecrunchBuffer();    break;
        case PP_CALC_CHECKSUM:      pp_CalcChecksum();      break;
        case PP_CALC_PASSKEY:       pp_CalcPasskey();       break;
        case PP_DECRYPT:            pp_Decrypt();           break;
        case PP_GET_PASSWORD:       pp_GetPassword();       break;
        case PP_OBSOLETE:           pp_Obsolete();          break;
        case PP_OVERLAY_DECR_HDR:   pp_OverlayDecrHdr();    break;
        case PP_ALLOC_CRUNCH_INFO:  pp_AllocCrunchInfo();   break;
        case PP_FREE_CRUNCH_INFO:   pp_FreeCrunchInfo();    break;
        case PP_CRUNCH_BUFFER:      pp_CrunchBuffer();      break;
        case PP_WRITE_DATA_HEADER:  pp_WriteDataHeader();   break;
        case PP_ENTER_PASSWORD:     pp_EnterPassword();    break;
        case PP_ERROR_MESSAGE:      pp_ErrorMessage();      break;
        case PP_LOAD_DATA2:         pp_LoadData2();         break;
        case PP_CRUNCH_BUFFER_DEST: pp_CrunchBufferDest();  break;
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
    } else {
        kprint("[PP] powerpacker.library registered successfully\n");
    }
}
