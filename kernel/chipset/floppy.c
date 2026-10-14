/*
 * floppy.c — UAOS Amiga MFM floppy controller
 *
 * Encodes/decodes Amiga DD disk tracks, loads ADF images, and performs
 * realistic Paula disk DMA transfers through DSKSYNC/DSKLEN/DSKDAT.
 * Models four drives (DF0:-DF3:) behind the CIAB PRB drive-select lines.
 */

#include "chipset/floppy.h"
#include "chipset/chip_emu.h"
#include "uaos_emu.h"
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
 * Floppy state — four drives, DF0:..DF3:
 * ------------------------------------------------------------------------- */

FloppyState g_floppy[FLOPPY_UNITS];

/* Drive-control bookkeeping (CIAB PRB): previous port value for edge
 * detection, the currently selected unit, and whether any /SEL is low. */
static uint8_t g_floppy_prev_prb = 0xFFu;
static int     g_floppy_selected = 0;
static int     g_floppy_sel_active = 0;

static FloppyState *unit_state(int unit)
{
    if (unit < 0 || unit >= FLOPPY_UNITS) return NULL;
    return &g_floppy[unit];
}

static void regenerate_track(FloppyState *f)
{
    if (!f->adf_loaded) return;
    int track = f->cyl * FLOPPY_HEADS + f->head;
    if (track < 0 || track >= FLOPPY_TRACKS * FLOPPY_HEADS) return;
    const uint8_t *track_data = f->adf + track * FLOPPY_TRACK_SIZE;
    f->mfm_bits = generate_track_mfm(track_data, f->mfm_track);
}

int floppy_load_adf(int unit, const uint8_t *data, size_t size)
{
    FloppyState *f = unit_state(unit);
    if (!f) return 0;
    if (size != FLOPPY_DISK_SIZE) return 0;
    memcpy(f->adf, data, FLOPPY_DISK_SIZE);
    f->adf_loaded = 1;
    f->cyl = 0;
    f->head = 0;
    f->bit_pos = 0;
    f->dma_active = 0;
    f->dirty = 0;
    f->chng_cleared = 0;
    regenerate_track(f);
    return 1;
}

void floppy_eject(int unit)
{
    FloppyState *f = unit_state(unit);
    if (!f) return;
    f->adf_loaded = 0;
    f->dma_active = 0;
    f->dma_ram = NULL;
    f->motor = 0;
    f->mfm_bits = 0;
    f->dirty = 0;
    f->chng_cleared = 0;
}

int floppy_unit_loaded(int unit)
{
    FloppyState *f = unit_state(unit);
    return f ? f->adf_loaded : 0;
}

/* Generate a simple non-DOS test ADF: boot block at sector 0 says
 * "UAOS ADF TEST", all other sectors are blank except a magic pattern. */
void floppy_make_test_adf(int unit)
{
    FloppyState *f = unit_state(unit);
    if (!f) return;

    memset(f->adf, 0, FLOPPY_DISK_SIZE);
    f->adf_loaded = 1;

    /* Sector 0 boot block: a tiny M68k stub that does nothing but
     * leaves a recognisable signature. */
    uint8_t *boot = f->adf;
    const char *sig = "UAOS ADF TEST BOOT";
    size_t sig_len = 18;
    for (size_t i = 0; i < sig_len && i < FLOPPY_SECTOR_SIZE; i++) boot[i] = (uint8_t)sig[i];

    /* Sector 1: magic pattern for a simple DMA read test. */
    uint8_t *sec1 = f->adf + FLOPPY_SECTOR_SIZE;
    for (int i = 0; i < 256; i++) {
        sec1[i * 2 + 0] = (uint8_t)i;
        sec1[i * 2 + 1] = (uint8_t)(0xFF - i);
    }

    f->cyl = 0;
    f->head = 0;
    f->bit_pos = 0;
    f->dirty = 0;
    f->chng_cleared = 0;
    regenerate_track(f);
}

void floppy_set_motor(int unit, int on)
{
    FloppyState *f = unit_state(unit);
    if (!f) return;
    f->motor = on ? 1 : 0;
}

void floppy_seek(int unit, int cyl, int head)
{
    FloppyState *f = unit_state(unit);
    if (!f) return;
    if (cyl < 0) cyl = 0;
    if (cyl >= FLOPPY_TRACKS) cyl = FLOPPY_TRACKS - 1;
    if (head < 0) head = 0;
    if (head >= FLOPPY_HEADS) head = FLOPPY_HEADS - 1;
    f->cyl = cyl;
    f->head = head;
    regenerate_track(f);
    f->bit_pos = 0;
}

void floppy_step(int unit, int dir)
{
    FloppyState *f = unit_state(unit);
    if (!f) return;
    floppy_seek(unit, f->cyl + (dir ? 1 : -1), f->head);
}

/* -------------------------------------------------------------------------
 * Drive control (Amiga CIAB PRB: /STEP, DIR, /SIDE, /SEL0-3, /MTR, active low)
 * ------------------------------------------------------------------------- */

void floppy_drive_control(uint8_t prb)
{
    /* /SELx falling edge latches the motor state for that unit. */
    for (int u = 0; u < FLOPPY_UNITS; u++) {
        uint8_t sel = (uint8_t)(1u << (3 + u));
        if ((g_floppy_prev_prb & sel) && !(prb & sel))
            g_floppy[u].motor = (prb & 0x80u) ? 0 : 1;
    }

    /* Selected unit = lowest-numbered /SEL currently low.  If none is low,
     * keep the previous selection (DF0: by default). */
    g_floppy_sel_active = 0;
    for (int u = 0; u < FLOPPY_UNITS; u++) {
        if (!(prb & (1u << (3 + u)))) {
            g_floppy_selected = u;
            g_floppy_sel_active = 1;
            break;
        }
    }

    /* /STEP falling edge steps every unit whose /SEL is low and clears its
     * disk-change latch when a disk is present. */
    if ((g_floppy_prev_prb & 0x01u) && !(prb & 0x01u)) {
        int dir = (prb & 0x02u) ? 0 : 1; /* DIR: 0 = inward (cyl+1) */
        for (int u = 0; u < FLOPPY_UNITS; u++) {
            if (!(prb & (1u << (3 + u)))) {
                floppy_step(u, dir);
                if (g_floppy[u].adf_loaded) g_floppy[u].chng_cleared = 1;
            }
        }
    }

    /* /SIDE selects the head of every unit whose /SEL is low. */
    int head = (prb & 0x04u) ? 0 : 1;
    for (int u = 0; u < FLOPPY_UNITS; u++) {
        if (!(prb & (1u << (3 + u))) && g_floppy[u].head != head)
            floppy_seek(u, g_floppy[u].cyl, head);
    }

    g_floppy_prev_prb = prb;
}

int floppy_selected_unit(void)
{
    return g_floppy_selected;
}

uint8_t floppy_ciaa_pra_bits(void)
{
    if (!g_floppy_sel_active) return 0x3Cu;
    FloppyState *f = &g_floppy[g_floppy_selected];
    uint8_t bits = 0x3Cu;
    if (!f->chng_cleared)                bits &= ~0x04u; /* /CHNG */
    if (f->adf_loaded && f->write_protect) bits &= ~0x08u; /* /WPRO */
    if (f->cyl == 0)                     bits &= ~0x10u; /* /TK0 */
    if (f->motor && f->adf_loaded)       bits &= ~0x20u; /* /RDY */
    return bits;
}

/* -------------------------------------------------------------------------
 * DMA helpers
 * ------------------------------------------------------------------------- */

static void write_chip_word(uint32_t addr, uint16_t value)
{
    if (addr + 2 > 0x01000000u) return; /* sanity */
    chip_write_u16(addr, value);
}

static uint16_t read_chip_word(uint32_t addr)
{
    if (addr + 2 > 0x01000000u) return 0;
    return chip_read_u16(addr);
}

/* Transfer words from the MFM bitstream to chip RAM. */
static void dma_transfer_words(FloppyState *f, uint32_t bits_per_tick)
{
    if (!f->dma_sync_found) {
        /* Search for the next sync word in the next tick's worth of bits. */
        uint32_t search_end = f->bit_pos + bits_per_tick;
        if (search_end > f->mfm_bits) search_end = f->mfm_bits;

        /* AmigaDOS sectors have a header sync followed by a 24-byte header
         * and then a data sync.  DMA reads should return the sector data,
         * so locate the header sync first, skip the header, and then sync
         * on the data sync. */
        uint32_t header_sync = 0;
        int found_header = 0;
        for (uint32_t p = f->bit_pos; p + 16 <= search_end; p++) {
            if (bits_read_word(f->mfm_track, p) == f->dma_sync) {
                header_sync = p;
                found_header = 1;
                break;
            }
        }
        if (!found_header) return;

        uint32_t data_search_start = header_sync + 16 + 24 * 8;
        if (data_search_start > f->mfm_bits) data_search_start = f->mfm_bits;
        int found_data_sync = 0;
        for (uint32_t p = data_search_start; p + 16 <= search_end; p++) {
            if (bits_read_word(f->mfm_track, p) == f->dma_sync) {
                f->dma_sync_found = 1;
                f->bit_pos = p + 16;
                found_data_sync = 1;
                break;
            }
        }
        if (!found_data_sync) return;
    }

    /* Transfer as many whole words as fit in the bits consumed this tick.
     * Each decoded word is two MFM-encoded bytes = 32 raw bits. */
    uint32_t bits_to_consume = f->bit_pos + bits_per_tick;
    if (bits_to_consume > f->mfm_bits) bits_to_consume = f->mfm_bits;
    while (f->dma_words > 0 && f->bit_pos + 32 <= bits_to_consume) {
        uint8_t prev_bit = 0;
        uint16_t mfm0 = bits_read_word(f->mfm_track, f->bit_pos);
        uint16_t mfm1 = bits_read_word(f->mfm_track, f->bit_pos + 16);
        uint8_t b0 = mfm_decode_word(mfm0, &prev_bit);
        uint8_t b1 = mfm_decode_word(mfm1, &prev_bit);
        uint16_t w = (uint16_t)((b0 << 8) | b1);
        write_chip_word(f->dma_ptr, w);
        f->dma_ptr += 2;
        f->bit_pos += 32;
        f->dma_words--;
    }

    if (f->dma_words == 0) {
        f->dma_active = 0;
        chip_emu_raise_intreq(FLOPPY_INTREQ_BIT);
    }
}

/* Transfer raw MFM words from chip RAM back into the MFM bitstream. */
static void dma_write_words(FloppyState *f, uint32_t bits_per_tick)
{
    if (!f->dma_sync_found) {
        /* For a write, we wait until the current bit position reaches the
         * next sync word.  In a real Amiga the write gate is enabled after
         * DSKSYNC; we approximate by starting the write at the next sync. */
        uint32_t search_end = f->bit_pos + bits_per_tick;
        if (search_end > f->mfm_bits) search_end = f->mfm_bits;
        for (uint32_t p = f->bit_pos; p + 16 <= search_end; p++) {
            if (bits_read_word(f->mfm_track, p) == f->dma_sync) {
                f->dma_sync_found = 1;
                f->bit_pos = p + 16;
                break;
            }
        }
        if (!f->dma_sync_found) return;
    }

    uint32_t bits_to_consume = f->bit_pos + bits_per_tick;
    if (bits_to_consume > f->mfm_bits) bits_to_consume = f->mfm_bits;
    while (f->dma_words > 0 && f->bit_pos + 16 <= bits_to_consume) {
        uint16_t w = read_chip_word(f->dma_ptr);
        bits_write_word(f->mfm_track, f->bit_pos, w);
        f->dma_ptr += 2;
        f->bit_pos += 16;
        f->dma_words--;
    }

    if (f->dma_words == 0) {
        f->dma_active = 0;
        /* Decode the modified track and update the ADF buffer. */
        uint8_t sectors[FLOPPY_SECTORS][FLOPPY_SECTOR_SIZE];
        int decoded = floppy_decode_track(f->mfm_track, f->mfm_bits, sectors);
        if (decoded > 0) {
            int track = f->cyl * FLOPPY_HEADS + f->head;
            for (int s = 0; s < FLOPPY_SECTORS; s++) {
                /* Only update sectors that were successfully decoded and differ
                 * from the current ADF contents, to avoid corrupting unwritten
                 * sectors due to alignment issues. */
                uint8_t *cur = f->adf + track * FLOPPY_TRACK_SIZE + s * FLOPPY_SECTOR_SIZE;
                if (memcmp(cur, sectors[s], FLOPPY_SECTOR_SIZE) != 0) {
                    memcpy(cur, sectors[s], FLOPPY_SECTOR_SIZE);
                    f->dirty = 1;
                }
            }
        }
        chip_emu_raise_intreq(FLOPPY_INTREQ_BIT);
    }
}

/* -------------------------------------------------------------------------
 * Rotation / tick
 * ------------------------------------------------------------------------- */

void floppy_tick(void)
{
    for (int u = 0; u < FLOPPY_UNITS; u++) {
        FloppyState *f = &g_floppy[u];
        if (!f->motor || !f->adf_loaded) continue;
        if (f->mfm_bits == 0) continue;

        uint32_t bits_per_tick = (f->mfm_bits + (FLOPPY_TICKS_PER_TRACK / 2))
                                 / FLOPPY_TICKS_PER_TRACK;
        if (bits_per_tick == 0) bits_per_tick = 1;

        /* Disk DMA streams into/out of the RAM window that was bound when
         * the transfer was launched — floppy_tick() runs in timer-ISR
         * context where g_ram may point at another task's window (or the
         * shared one). */
        if (f->dma_active) {
            uint8_t *saved_ram = g_ram;
            if (f->dma_ram) g_ram = f->dma_ram;
            if (f->dma_write) {
                dma_write_words(f, bits_per_tick);
            } else {
                dma_transfer_words(f, bits_per_tick);
            }
            g_ram = saved_ram;
        }

        /* Advance rotation. */
        f->bit_pos += bits_per_tick;
        if (f->bit_pos >= f->mfm_bits) {
            f->bit_pos -= f->mfm_bits;
        }
    }
}

/* -------------------------------------------------------------------------
 * Register-level interface used by chip_emu.c
 * ------------------------------------------------------------------------- */

int floppy_dma_read(int unit, uint32_t dskpt, uint16_t dsklen, uint16_t dsk_sync)
{
    FloppyState *f = unit_state(unit);
    if (!f || !f->adf_loaded) return 0;
    uint16_t words = dsklen & 0x3FFFu;
    if (words == 0) words = 0x8000u;
    f->dma_ptr = dskpt;
    f->dma_words = words;
    f->dma_sync = dsk_sync ? dsk_sync : SYNC_WORD;
    f->dma_sync_found = 0;
    f->dma_active = 1;
    f->dma_write = 0;
    f->dma_ram = g_ram;
    return 1;
}

int floppy_dma_write(int unit, uint32_t dskpt, uint16_t dsklen)
{
    FloppyState *f = unit_state(unit);
    if (!f || !f->adf_loaded) return 0;
    if (f->write_protect) return 0;
    uint16_t words = dsklen & 0x3FFFu;
    if (words == 0) words = 0x8000u;
    f->dma_ptr = dskpt;
    f->dma_words = words;
    f->dma_sync = SYNC_WORD;
    f->dma_sync_found = 0;
    f->dma_active = 1;
    f->dma_write = 1;
    f->dma_ram = g_ram;
    return 1;
}

int floppy_unbind_ram(uint8_t *ram)
{
    if (!ram) return 0;
    int cleared = 0;
    for (int u = 0; u < FLOPPY_UNITS; u++) {
        if (g_floppy[u].dma_ram == ram) {
            g_floppy[u].dma_ram = NULL;
            g_floppy[u].dma_active = 0;
            cleared = 1;
        }
    }
    return cleared;
}

uint16_t floppy_dskdat_read(int unit)
{
    FloppyState *f = unit_state(unit);
    if (!f || !f->adf_loaded || !f->dma_active) return 0;
    if (f->bit_pos + 32 > f->mfm_bits) return 0;
    uint8_t prev_bit = 0;
    uint16_t mfm0 = bits_read_word(f->mfm_track, f->bit_pos);
    uint16_t mfm1 = bits_read_word(f->mfm_track, f->bit_pos + 16);
    uint8_t b0 = mfm_decode_word(mfm0, &prev_bit);
    uint8_t b1 = mfm_decode_word(mfm1, &prev_bit);
    f->bit_pos += 32;
    return (uint16_t)((b0 << 8) | b1);
}

void floppy_dskdat_write(int unit, uint16_t value)
{
    FloppyState *f = unit_state(unit);
    if (!f || !f->adf_loaded || !f->dma_active || !f->dma_write) return;
    if (f->write_protect) return;
    if (f->bit_pos + 16 > f->mfm_bits) return;

    bits_write_word(f->mfm_track, f->bit_pos, value);
    f->dma_ptr += 2;
    f->bit_pos += 16;
    f->dma_words--;

    if (f->dma_words == 0) {
        f->dma_active = 0;
        uint8_t sectors[FLOPPY_SECTORS][FLOPPY_SECTOR_SIZE];
        int decoded = floppy_decode_track(f->mfm_track, f->mfm_bits, sectors);
        if (decoded > 0) {
            int track = f->cyl * FLOPPY_HEADS + f->head;
            for (int s = 0; s < FLOPPY_SECTORS; s++) {
                uint8_t *cur = f->adf + track * FLOPPY_TRACK_SIZE + s * FLOPPY_SECTOR_SIZE;
                if (memcmp(cur, sectors[s], FLOPPY_SECTOR_SIZE) != 0) {
                    memcpy(cur, sectors[s], FLOPPY_SECTOR_SIZE);
                    f->dirty = 1;
                }
            }
        }
        chip_emu_raise_intreq(FLOPPY_INTREQ_BIT);
    }
}

/* Direct decoded sector read, useful for DOS handlers and diagnostics. */
int floppy_read_sector(int unit, int track, int sector, uint8_t *out)
{
    FloppyState *f = unit_state(unit);
    if (!f || !f->adf_loaded) return 0;
    if (track < 0 || track >= FLOPPY_TRACKS * FLOPPY_HEADS) return 0;
    if (sector < 0 || sector >= FLOPPY_SECTORS) return 0;
    memcpy(out, f->adf + track * FLOPPY_TRACK_SIZE + sector * FLOPPY_SECTOR_SIZE,
           FLOPPY_SECTOR_SIZE);
    return 1;
}

/* Direct decoded sector write.  Returns 1 on success, 0 on failure. */
int floppy_write_sector(int unit, int track, int sector, const uint8_t *data)
{
    FloppyState *f = unit_state(unit);
    if (!f || !f->adf_loaded) return 0;
    if (f->write_protect) return 0;
    if (track < 0 || track >= FLOPPY_TRACKS * FLOPPY_HEADS) return 0;
    if (sector < 0 || sector >= FLOPPY_SECTORS) return 0;
    uint8_t *sec = f->adf + track * FLOPPY_TRACK_SIZE + sector * FLOPPY_SECTOR_SIZE;
    memcpy(sec, data, FLOPPY_SECTOR_SIZE);
    f->dirty = 1;
    if (f->cyl * FLOPPY_HEADS + f->head == track) {
        regenerate_track(f);
    }
    return 1;
}

int floppy_is_dirty(int unit)
{
    FloppyState *f = unit_state(unit);
    return f ? f->dirty : 0;
}

void floppy_clear_dirty(int unit)
{
    FloppyState *f = unit_state(unit);
    if (f) f->dirty = 0;
}

const uint8_t *floppy_adf_data(int unit)
{
    FloppyState *f = unit_state(unit);
    if (!f || !f->adf_loaded) return NULL;
    return f->adf;
}

/* Enable or disable the virtual write-protect tab. */
void floppy_set_write_protect(int unit, int wp)
{
    FloppyState *f = unit_state(unit);
    if (!f) return;
    f->write_protect = wp ? 1 : 0;
}

/* Verify that write-protected disks reject direct sector writes. */
int floppy_write_protect_test(void)
{
    if (!floppy_unit_loaded(0)) return 0;
    floppy_set_write_protect(0, 1);
    uint8_t buf[512];
    memset(buf, 0x77, 512);
    int ok = !floppy_write_sector(0, 0, 0, buf);
    floppy_set_write_protect(0, 0);
    return ok;
}

/* Verify the DMA write path by writing an MFM-encoded sector into chip RAM
 * and streaming it back onto the virtual track. */
int floppy_dma_write_test(void)
{
    FloppyState *f = &g_floppy[0];
    if (!f->adf_loaded) return 0;
    if (g_ram == NULL) return 0;

    /* Use the current track (cylinder 0, head 0) and sector 0. */
    floppy_set_motor(0, 1);
    floppy_seek(0, 0, 0);

    /* Build the payload and its CRC. */
    uint8_t data[512];
    for (int i = 0; i < 512; i++) data[i] = (uint8_t)(i ^ 0x5A);
    uint16_t crc = crc16_ccitt(data, 512);
    uint8_t crc_hi = (uint8_t)(crc >> 8);
    uint8_t crc_lo = (uint8_t)(crc & 0xFFu);

    /* Encode the data + CRC into chip RAM as MFM words.  The write will start
     * at the sector's data sync, so the sync itself is left in place and the
     * first chip-RAM word is the MFM encoding of data[0]. */
    uint32_t addr = 0x20000u;
    if (addr + 2 * 514 > GUEST_RAM_SIZE) return 0;
    uint32_t pos = 0;
    uint8_t prev_bit = 1;
    for (int i = 0; i < 512; i++) {
        uint16_t mfm = mfm_encode_byte(data[i], &prev_bit);
        bits_write_word(g_ram + addr, pos, mfm);
        pos += 16;
    }
    bits_write_word(g_ram + addr, pos, mfm_encode_byte(crc_hi, &prev_bit));
    pos += 16;
    bits_write_word(g_ram + addr, pos, mfm_encode_byte(crc_lo, &prev_bit));
    pos += 16;

    /* Position the write at the data sync for sector 0. */
    int32_t header_sync = find_sync(f->mfm_track, f->mfm_bits, 0);
    if (header_sync < 0) return 0;
    int32_t data_sync = find_sync(f->mfm_track, f->mfm_bits,
                                  (uint32_t)(header_sync + 16 + 24 * 16));
    if (data_sync < 0) return 0;
    f->bit_pos = (uint32_t)data_sync;

    /* Start the DMA write of 512 data bytes + 2 CRC bytes. */
    floppy_dma_write(0, addr, 514);

    /* Run the tick loop until the DMA completes. */
    for (int i = 0; i < 100 && f->dma_active; i++) {
        floppy_tick();
    }
    if (f->dma_active) return 0;

    /* Read back the sector and verify it. */
    uint8_t out[512];
    if (!floppy_read_sector(0, 0, 0, out)) return 0;
    for (int i = 0; i < 512; i++) {
        if (out[i] != (uint8_t)(i ^ 0x5A)) return 0;
    }
    return 1;
}
