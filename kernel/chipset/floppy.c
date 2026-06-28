/*
 * floppy.c — UAOS Amiga MFM floppy controller
 *
 * Encodes/decodes Amiga DD disk tracks, loads ADF images, and performs
 * realistic Paula disk DMA transfers through DSKSYNC/DSKLEN/DSKDAT.
 */

#include "chipset/floppy.h"
#include "chipset/chip_emu.h"
#include <string.h>

/* -------------------------------------------------------------------------
 * Bitstream helpers (bits packed MSB-first in bytes)
 * ------------------------------------------------------------------------- */

static inline void bits_set(uint8_t *bits, uint32_t pos, uint8_t bit)
{
    if (bit) bits[pos >> 3] |=  (uint8_t)(0x80u >> (pos & 7u));
    else     bits[pos >> 3] &= (uint8_t)~(0x80u >> (pos & 7u));
}

static inline uint8_t bits_get(const uint8_t *bits, uint32_t pos)
{
    return (bits[pos >> 3] >> (7 - (pos & 7u))) & 1u;
}

static inline uint16_t bits_read_word(const uint8_t *bits, uint32_t pos)
{
    uint16_t w = 0;
    for (int i = 0; i < 16; i++) {
        w = (uint16_t)((w << 1) | bits_get(bits, pos + i));
    }
    return w;
}

static inline void bits_write_word(uint8_t *bits, uint32_t pos, uint16_t w)
{
    for (int i = 0; i < 16; i++) {
        bits_set(bits, pos + i, (uint8_t)((w >> (15 - i)) & 1u));
    }
}

/* -------------------------------------------------------------------------
 * CRC-16-CCITT (Amiga disk CRC: seed 0xFFFF, poly 0x1021)
 * ------------------------------------------------------------------------- */

static uint16_t crc16_ccitt(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int j = 0; j < 8; j++) {
            if (crc & 0x8000u) crc = (uint16_t)((crc << 1) ^ 0x1021u);
            else               crc = (uint16_t)(crc << 1);
        }
    }
    return crc;
}

/* -------------------------------------------------------------------------
 * MFM encode / decode single bytes
 * ------------------------------------------------------------------------- */

/* Encode one data byte into 16 MFM bits.  *prev_bit is the previous data bit
 * (not clock bit).  It is updated to the last data bit of this byte. */
static uint16_t mfm_encode_byte(uint8_t data, uint8_t *prev_bit)
{
    uint16_t out = 0;
    for (int i = 7; i >= 0; i--) {
        uint8_t bit = (uint8_t)((data >> i) & 1u);
        uint8_t clock = (bit == 0 && *prev_bit == 0) ? 1u : 0u;
        out = (uint16_t)((out << 2) | ((uint16_t)(clock << 1) | bit));
        *prev_bit = bit;
    }
    return out;
}

/* Decode 16 MFM bits into one data byte.  *prev_bit is the previous data bit. */
static uint8_t mfm_decode_word(uint16_t mfm, uint8_t *prev_bit)
{
    uint8_t data = 0;
    for (int i = 15; i >= 1; i -= 2) {
        uint8_t clock = (uint8_t)((mfm >> i) & 1u);
        uint8_t bit   = (uint8_t)((mfm >> (i - 1)) & 1u);
        (void)clock;
        data = (uint8_t)((data << 1) | bit);
        *prev_bit = bit;
    }
    return data;
}

/* -------------------------------------------------------------------------
 * Track generation
 * ------------------------------------------------------------------------- */

#define SYNC_WORD 0x4489u

/* Append one data byte (MFM encoded) to the bitstream. */
static uint32_t emit_byte(uint8_t *bits, uint32_t pos, uint8_t data, uint8_t *prev_bit)
{
    uint16_t mfm = mfm_encode_byte(data, prev_bit);
    bits_write_word(bits, pos, mfm);
    return pos + 16;
}

/* Append a raw 16-bit sync word to the bitstream. */
static uint32_t emit_sync(uint8_t *bits, uint32_t pos)
{
    bits_write_word(bits, pos, SYNC_WORD);
    return pos + 16;
}

/* Append a gap of MFM-encoded 0x00 bytes (produces 0xAAAA words). */
static uint32_t emit_gap(uint8_t *bits, uint32_t pos, unsigned int n_bytes, uint8_t *prev_bit)
{
    for (unsigned int i = 0; i < n_bytes; i++) {
        pos = emit_byte(bits, pos, 0x00, prev_bit);
    }
    return pos;
}

/* Generate a raw MFM bitstream for one track.  Each of the 11 sectors is
 * laid out with AmigaDOS-style headers and data CRCs. */
static uint32_t generate_track_mfm(const uint8_t *track_data, uint8_t *bits)
{
    uint8_t prev_bit = 0;
    uint32_t pos = 0;

    /* Leading gap. */
    pos = emit_gap(bits, pos, 60, &prev_bit);

    for (int sec = 0; sec < FLOPPY_SECTORS; sec++) {
        const uint8_t *data = track_data + sec * FLOPPY_SECTOR_SIZE;

        /* Sector header */
        pos = emit_sync(bits, pos);
        uint8_t hdr[24];
        hdr[0] = 0xFF;        /* format */
        hdr[1] = 0x00;        /* track (logical, set to 0 for simplicity) */
        hdr[2] = (uint8_t)sec; /* sector number */
        hdr[3] = 0x00;        /* distance to next sector? */
        memset(hdr + 4, 0, 16); /* label */
        uint16_t hdr_crc = crc16_ccitt(hdr, 20);
        hdr[20] = (uint8_t)(hdr_crc >> 8);
        hdr[21] = (uint8_t)(hdr_crc & 0xFFu);
        hdr[22] = 0;
        hdr[23] = 0;
        for (int i = 0; i < 24; i++) pos = emit_byte(bits, pos, hdr[i], &prev_bit);

        /* Sector data */
        pos = emit_sync(bits, pos);
        for (int i = 0; i < FLOPPY_SECTOR_SIZE; i++) {
            pos = emit_byte(bits, pos, data[i], &prev_bit);
        }
        uint16_t data_crc = crc16_ccitt(data, FLOPPY_SECTOR_SIZE);
        pos = emit_byte(bits, pos, (uint8_t)(data_crc >> 8), &prev_bit);
        pos = emit_byte(bits, pos, (uint8_t)(data_crc & 0xFFu), &prev_bit);

        /* Inter-sector gap. */
        pos = emit_gap(bits, pos, 30, &prev_bit);
    }

    /* Pad to the end of the track with gap. */
    while (pos < FLOPPY_MFM_TRACK_BITS) {
        pos = emit_byte(bits, pos, 0x00, &prev_bit);
    }

    return pos;
}

/* -------------------------------------------------------------------------
 * Track decoding
 * ------------------------------------------------------------------------- */

static int32_t find_sync(const uint8_t *bits, uint32_t bits_len, uint32_t start)
{
    for (uint32_t i = start; i + 16 <= bits_len; i++) {
        if (bits_read_word(bits, i) == SYNC_WORD) return (int32_t)i;
    }
    return -1;
}

int floppy_decode_track(const uint8_t *mfm_bits, uint32_t bits_len,
                        uint8_t sectors[FLOPPY_SECTORS][FLOPPY_SECTOR_SIZE])
{
    int found = 0;
    uint32_t pos = 0;
    memset(sectors, 0, FLOPPY_SECTORS * FLOPPY_SECTOR_SIZE);

    while (pos + 16 < bits_len) {
        int32_t sync = find_sync(mfm_bits, bits_len, pos);
        if (sync < 0) break;
        pos = (uint32_t)(sync + 16);

        if (pos + 16 * 24 > bits_len) break;

        uint8_t prev_bit = 0;
        uint8_t hdr[24];
        for (int i = 0; i < 24; i++) {
            uint16_t w = bits_read_word(mfm_bits, pos);
            pos += 16;
            hdr[i] = mfm_decode_word(w, &prev_bit);
        }

        /* Verify header CRC. */
        uint16_t hdr_crc = (uint16_t)((hdr[20] << 8) | hdr[21]);
        if (crc16_ccitt(hdr, 20) != hdr_crc) continue;

        int sec = hdr[2];
        if (sec < 0 || sec >= FLOPPY_SECTORS) continue;

        /* Look for the data sync. */
        int32_t data_sync = find_sync(mfm_bits, bits_len, pos);
        if (data_sync < 0) break;
        pos = (uint32_t)(data_sync + 16);
        if (pos + 16 * (FLOPPY_SECTOR_SIZE + 2) > bits_len) break;

        uint8_t data[FLOPPY_SECTOR_SIZE + 2];
        for (int i = 0; i < FLOPPY_SECTOR_SIZE + 2; i++) {
            uint16_t w = bits_read_word(mfm_bits, pos);
            pos += 16;
            data[i] = mfm_decode_word(w, &prev_bit);
        }

        /* Verify data CRC. */
        uint16_t data_crc = (uint16_t)((data[FLOPPY_SECTOR_SIZE] << 8) |
                                        data[FLOPPY_SECTOR_SIZE + 1]);
        if (crc16_ccitt(data, FLOPPY_SECTOR_SIZE) != data_crc) continue;

        memcpy(sectors[sec], data, FLOPPY_SECTOR_SIZE);
        found++;
    }

    return found;
}

/* -------------------------------------------------------------------------
 * Floppy state
 * ------------------------------------------------------------------------- */

FloppyState g_floppy;

static void regenerate_track(void)
{
    if (!g_floppy.adf_loaded) return;
    int track = g_floppy.cyl * FLOPPY_HEADS + g_floppy.head;
    if (track < 0 || track >= FLOPPY_TRACKS * FLOPPY_HEADS) return;
    const uint8_t *track_data = g_floppy.adf + track * FLOPPY_TRACK_SIZE;
    g_floppy.mfm_bits = generate_track_mfm(track_data, g_floppy.mfm_track);
}

int floppy_load_adf(const uint8_t *data, size_t size)
{
    if (size != FLOPPY_DISK_SIZE) return 0;
    memcpy(g_floppy.adf, data, FLOPPY_DISK_SIZE);
    g_floppy.adf_loaded = 1;
    g_floppy.cyl = 0;
    g_floppy.head = 0;
    g_floppy.bit_pos = 0;
    regenerate_track();
    return 1;
}

/* Generate a simple non-DOS test ADF: boot block at sector 0 says
 * "UAOS ADF TEST", all other sectors are blank except a magic pattern. */
void floppy_make_test_adf(void)
{
    memset(g_floppy.adf, 0, FLOPPY_DISK_SIZE);
    g_floppy.adf_loaded = 1;

    /* Sector 0 boot block: a tiny M68k stub that does nothing but
     * leaves a recognisable signature. */
    uint8_t *boot = g_floppy.adf;
    const char *sig = "UAOS ADF TEST BOOT";
    size_t sig_len = 18;
    for (size_t i = 0; i < sig_len && i < FLOPPY_SECTOR_SIZE; i++) boot[i] = (uint8_t)sig[i];

    /* Sector 1: magic pattern for a simple DMA read test. */
    uint8_t *sec1 = g_floppy.adf + FLOPPY_SECTOR_SIZE;
    for (int i = 0; i < 256; i++) {
        sec1[i * 2 + 0] = (uint8_t)i;
        sec1[i * 2 + 1] = (uint8_t)(0xFF - i);
    }

    g_floppy.cyl = 0;
    g_floppy.head = 0;
    g_floppy.bit_pos = 0;
    regenerate_track();
}

void floppy_set_motor(int on)
{
    g_floppy.motor = on ? 1 : 0;
}

void floppy_seek(int cyl, int head)
{
    if (cyl < 0) cyl = 0;
    if (cyl >= FLOPPY_TRACKS) cyl = FLOPPY_TRACKS - 1;
    if (head < 0) head = 0;
    if (head >= FLOPPY_HEADS) head = FLOPPY_HEADS - 1;
    g_floppy.cyl = cyl;
    g_floppy.head = head;
    regenerate_track();
    g_floppy.bit_pos = 0;
}

void floppy_step(int dir)
{
    floppy_seek(g_floppy.cyl + (dir ? 1 : -1), g_floppy.head);
}

/* -------------------------------------------------------------------------
 * DMA helpers
 * ------------------------------------------------------------------------- */

static void write_chip_word(uint32_t addr, uint16_t value)
{
    if (addr + 2 > 0x01000000u) return; /* sanity */
    chip_write_u16(addr, value);
}

/* Transfer words from the MFM bitstream to chip RAM. */
static void dma_transfer_words(uint32_t bits_per_tick)
{
    if (!g_floppy.dma_sync_found) {
        /* Search for the next sync word in the next tick's worth of bits. */
        uint32_t search_end = g_floppy.bit_pos + bits_per_tick;
        if (search_end > g_floppy.mfm_bits) search_end = g_floppy.mfm_bits;

        /* AmigaDOS sectors have a header sync followed by a 24-byte header
         * and then a data sync.  DMA reads should return the sector data,
         * so locate the header sync first, skip the header, and then sync
         * on the data sync. */
        uint32_t header_sync = 0;
        int found_header = 0;
        for (uint32_t p = g_floppy.bit_pos; p + 16 <= search_end; p++) {
            if (bits_read_word(g_floppy.mfm_track, p) == g_floppy.dma_sync) {
                header_sync = p;
                found_header = 1;
                break;
            }
        }
        if (!found_header) return;

        uint32_t data_search_start = header_sync + 16 + 24 * 8;
        if (data_search_start > g_floppy.mfm_bits) data_search_start = g_floppy.mfm_bits;
        int found_data_sync = 0;
        for (uint32_t p = data_search_start; p + 16 <= search_end; p++) {
            if (bits_read_word(g_floppy.mfm_track, p) == g_floppy.dma_sync) {
                g_floppy.dma_sync_found = 1;
                g_floppy.bit_pos = p + 16;
                found_data_sync = 1;
                break;
            }
        }
        if (!found_data_sync) return;
    }

    /* Transfer as many whole words as fit in the bits consumed this tick.
     * Each decoded word is two MFM-encoded bytes = 32 raw bits. */
    uint32_t bits_to_consume = g_floppy.bit_pos + bits_per_tick;
    if (bits_to_consume > g_floppy.mfm_bits) bits_to_consume = g_floppy.mfm_bits;
    while (g_floppy.dma_words > 0 && g_floppy.bit_pos + 32 <= bits_to_consume) {
        uint8_t prev_bit = 0;
        uint16_t mfm0 = bits_read_word(g_floppy.mfm_track, g_floppy.bit_pos);
        uint16_t mfm1 = bits_read_word(g_floppy.mfm_track, g_floppy.bit_pos + 16);
        uint8_t b0 = mfm_decode_word(mfm0, &prev_bit);
        uint8_t b1 = mfm_decode_word(mfm1, &prev_bit);
        uint16_t w = (uint16_t)((b0 << 8) | b1);
        write_chip_word(g_floppy.dma_ptr, w);
        g_floppy.dma_ptr += 2;
        g_floppy.bit_pos += 32;
        g_floppy.dma_words--;
    }

    if (g_floppy.dma_words == 0) {
        g_floppy.dma_active = 0;
        chip_emu_raise_intreq(FLOPPY_INTREQ_BIT);
    }
}

/* -------------------------------------------------------------------------
 * Rotation / tick
 * ------------------------------------------------------------------------- */

void floppy_tick(void)
{
    if (!g_floppy.motor || !g_floppy.adf_loaded) return;

    if (g_floppy.mfm_bits == 0) return;
    uint32_t bits_per_tick = (g_floppy.mfm_bits + (FLOPPY_TICKS_PER_TRACK / 2))
                             / FLOPPY_TICKS_PER_TRACK;
    if (bits_per_tick == 0) bits_per_tick = 1;

    if (g_floppy.dma_active) {
        dma_transfer_words(bits_per_tick);
    }

    /* Advance rotation. */
    g_floppy.bit_pos += bits_per_tick;
    if (g_floppy.bit_pos >= g_floppy.mfm_bits) {
        g_floppy.bit_pos -= g_floppy.mfm_bits;
    }
}

/* -------------------------------------------------------------------------
 * Register-level interface used by chip_emu.c
 * ------------------------------------------------------------------------- */

int floppy_dma_read(uint32_t dskpt, uint16_t dsklen, uint16_t dsk_sync)
{
    if (!g_floppy.adf_loaded) return 0;
    uint16_t words = dsklen & 0x3FFFu;
    if (words == 0) words = 0x8000u;
    g_floppy.dma_ptr = dskpt;
    g_floppy.dma_words = words;
    g_floppy.dma_sync = dsk_sync ? dsk_sync : SYNC_WORD;
    g_floppy.dma_sync_found = 0;
    g_floppy.dma_active = 1;
    return 1;
}

int floppy_dma_write(uint32_t dskpt, uint16_t dsklen)
{
    (void)dskpt;
    (void)dsklen;
    return 0; /* not implemented for real ADF */
}

uint16_t floppy_dskdat_read(void)
{
    if (!g_floppy.adf_loaded || !g_floppy.dma_active) return 0;
    if (g_floppy.bit_pos + 32 > g_floppy.mfm_bits) return 0;
    uint8_t prev_bit = 0;
    uint16_t mfm0 = bits_read_word(g_floppy.mfm_track, g_floppy.bit_pos);
    uint16_t mfm1 = bits_read_word(g_floppy.mfm_track, g_floppy.bit_pos + 16);
    uint8_t b0 = mfm_decode_word(mfm0, &prev_bit);
    uint8_t b1 = mfm_decode_word(mfm1, &prev_bit);
    g_floppy.bit_pos += 32;
    return (uint16_t)((b0 << 8) | b1);
}

void floppy_dskdat_write(uint16_t value)
{
    (void)value;
    /* Write path not implemented for real ADF. */
}

/* Direct decoded sector read, useful for DOS handlers and diagnostics. */
int floppy_read_sector(int track, int sector, uint8_t *out)
{
    if (!g_floppy.adf_loaded) return 0;
    if (track < 0 || track >= FLOPPY_TRACKS * FLOPPY_HEADS) return 0;
    if (sector < 0 || sector >= FLOPPY_SECTORS) return 0;
    memcpy(out, g_floppy.adf + track * FLOPPY_TRACK_SIZE + sector * FLOPPY_SECTOR_SIZE,
           FLOPPY_SECTOR_SIZE);
    return 1;
}
