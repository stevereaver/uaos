/*
 * powerpacker_lib.h — UAOS powerpacker.library Interface
 *
 * AmigaOS powerpacker.library provides data decompression and decryption
 * for PowerPacker-compressed files. This is a native UAOS implementation
 * backed by the loadable library system.
 *
 * Based on pplib 1.2 by Stuart Caie (Public Domain)
 * and powerpacker.library by Nico Francois / Kjetil Hvalstrand
 */

#ifndef UAOS_POWERPACKER_LIB_H
#define UAOS_POWERPACKER_LIB_H

#include <stdint.h>
#include <stdbool.h>

/* =========================================================================
 * Error codes
 * ========================================================================= */
#define PPERR_OK         0   /* no error */
#define PPERR_OPEN      -1   /* error opening file */
#define PPERR_READ      -2   /* error reading from file */
#define PPERR_NOMEMORY  -3   /* out of memory */
#define PPERR_PASSWORD  -5   /* bad or missing password */
#define PPERR_UNKNOWN   -6   /* error unknown */
#define PPERR_SEEK      (PPERR_UNKNOWN)  /* error seeking in file */
#define PPERR_DATAFORMAT (PPERR_UNKNOWN) /* error in data format */
#define PPERR_DECRUNCH  (PPERR_UNKNOWN)  /* error decrunching data */
#define PPERR_ARGS      (PPERR_UNKNOWN)  /* bad arguments to function */

/* =========================================================================
 * Data structures
 * ========================================================================= */

typedef struct {
    uint32_t tag;
    uint8_t* src;
    uint32_t src_len;
    uint8_t* dst;
    uint32_t dst_len;
} decrunch_t;

typedef struct {
    uint32_t token;
    uint32_t* ptr;
    uint32_t* ptr_max;
} write_res_t;

typedef struct {
    uint16_t w00[4];
    uint16_t w08[4];
    uint16_t w10[4];
    uint8_t  b2C[4];
    uint8_t* start;
    uint32_t fsize;
    uint8_t* src_end;
    uint8_t** addrs;
    uint32_t addrs_count;
    uint8_t* dst;
    uint32_t tmp[0x80];
    uint8_t* print_pos;
    uint16_t value;
    uint16_t bits;
    uint16_t* wnd1;
    uint16_t* wnd2;
    uint16_t wnd_max;
    uint16_t wnd_off;
    uint16_t wnd_left;
    bool (*progress_fn)(uint32_t, uint32_t, void*);
} CrunchInfo;

/* =========================================================================
 * Function indices (AmigaOS LVO convention)
 * ========================================================================= */
#define PP_LOAD_DATA          1   /* ppLoadData()        LVO -30 */
#define PP_DECRUNCH_BUFFER    2   /* ppDecrunchBuffer()  LVO -36 */
#define PP_CALC_CHECKSUM      3   /* ppCalcChecksum()     LVO -42 */
#define PP_CALC_PASSKEY       4   /* ppCalcPasskey()      LVO -48 */
#define PP_DECRYPT            5   /* ppDecrypt()          LVO -54 */
#define PP_GET_PASSWORD       6   /* ppGetPassword()      LVO -60 */
#define PP_OBSOLETE           7   /* (obsolete functions) LVO -66 */
#define PP_OVERLAY_DECR_HDR   8   /* ppOverlayDecrHdr()   LVO -72 */
#define PP_ALLOC_CRUNCH_INFO  9   /* ppAllocCrunchInfo()  LVO -78 */
#define PP_FREE_CRUNCH_INFO   10  /* ppFreeCrunchInfo()   LVO -84 */
#define PP_CRUNCH_BUFFER      11  /* ppCrunchBuffer()     LVO -90 */
#define PP_WRITE_DATA_HEADER  12  /* ppWriteDataHeader()  LVO -96 */
#define PP_ENTER_PASSWORD     13  /* ppEnterPassword()    LVO -102 */
#define PP_ERROR_MESSAGE      14  /* ppErrorMessage()     LVO -108 */
#define PP_LOAD_DATA2         15  /* ppLoadData2()        LVO -114 */
#define PP_CRUNCH_BUFFER_DEST 16  /* ppCrunchBufferDest() LVO -120 */

/* =========================================================================
 * Public API for native code (non-m68k)
 * ========================================================================= */

/* Register powerpacker.library with the loadable library system.
 * Call after UAOS_LoadableLib_Init(). */
void UAOS_POWERPACKER_Register(void);

/* Dispatch function — called by the m68k ILLEGAL handler. */
void UAOS_POWERPACKER_Dispatch(uint32_t fn_idx);

/* -------------------------------------------------------------------------
 * Core algorithms (native implementation)
 * ------------------------------------------------------------------------- */

/**
 * Calculates the 16-bit checksum of a password.
 * This checksum is stored in the header of an encrypted PowerPacker file.
 * @param password a null terminated string
 * @return the 16-bit checksum of the password
 */
uint32_t ppCalcChecksum(const uint8_t* password);

/**
 * Calculates a 32-bit decryption key from a password.
 * The key is used with ppDecrypt() to decrypt a file.
 * @param password a null terminated string
 * @return a 32-bit decryption key
 */
uint32_t ppCalcPasskey(const uint8_t* password);

/**
 * Decrypts encrypted PowerPacker data.
 * PowerPacker encrypts data by XORing it with a 32-bit key.
 * @param data a buffer with encrypted PowerPacker data
 * @param len the length of the buffer in bytes (rounded up to nearest 4)
 * @param key the key to perform the decryption with
 */
void ppDecrypt(uint8_t* data, uint32_t len, uint32_t key);

/**
 * Decrunches PowerPacked data.
 *
 * PowerPacker data files have the following format:
 *   1 longword identifier           'PP20', 'PX20' or 'PPLS'
 *  [1 longword length (if 'PPLS')   0xLLLLLLLL]
 *  [1 word checksum (if 'PX20')     0xSSSS]
 *   1 longword efficiency           0xEEEEEEEE
 *   X longwords crunched data       0xCCCCCCCC, 0xCCCCCCCC, ...
 *   1 longword decrunch info        'decrlen' << 8 | '8 bits other info'
 *
 * @param eff a pointer to the 4-byte efficiency header
 * @param src a pointer to the start of the crunched data
 * @param dest a pointer to the start of the decrunched data buffer
 * @param src_len length of source data (not including efficiency header or trailer)
 * @param dest_len length of the decrunched data buffer
 * @return 0 for failure, 1 for success
 */
int ppDecrunchBuffer(const uint8_t* eff, const uint8_t* src,
                     uint8_t* dest, uint32_t src_len, uint32_t dest_len);

/**
 * Does the same as ppDecrunchBuffer(), only it expects 'master mode' data.
 * This is a secret mode used in PowerPacker 2.0 - 3.0 packed executables,
 * where the bit used to identify literals is swapped from 0 to 1
 */
int ppDecrunchBuffer_m(const uint8_t* eff, const uint8_t* src,
                       uint8_t* dest, uint32_t src_len, uint32_t dest_len);

/**
 * Loads PowerPacked data from a file.
 * @param filename the file to load
 * @param bufferptr pointer to where the decrunched data buffer pointer will be stored
 * @param buflenptr pointer to where the length will be stored
 * @return error code (PPERR_OK = 0 on success)
 */
int32_t ppLoadData(const char* filename, uint8_t** bufferptr,
                   uint32_t* buflenptr, bool (*password_func)(uint8_t*, uint32_t));

/**
 * Returns error message string for an error code.
 * @param errorcode the error code
 * @return pointer to error message string
 */
const char* ppErrorMessage(int32_t errorcode);

#endif /* UAOS_POWERPACKER_LIB_H */
