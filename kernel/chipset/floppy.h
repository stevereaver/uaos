/*
 * floppy.h — UAOS Amiga MFM floppy controller
 *
 * Exposes an ADF-backed floppy that can be read through the Paula disk DMA
 * registers (DSKSYNC, DSKLEN, DSKDAT, DSKPT).  Tracks are encoded to MFM on
 * demand and streamed to chip RAM as the virtual disk rotates.
 */

#ifndef UAOS_FLOPPY_H
#define UAOS_FLOPPY_H

#include <stdint.h>
#include <stddef.h>

#define FLOPPY_TRACKS       80
#define FLOPPY_HEADS        2
#define FLOPPY_SECTORS      11
#define FLOPPY_SECTOR_SIZE  512
#define FLOPPY_TRACK_SIZE   (FLOPPY_SECTORS * FLOPPY_SECTOR_SIZE)
#define FLOPPY_DISK_SIZE    (FLOPPY_TRACKS * FLOPPY_HEADS * FLOPPY_TRACK_SIZE)

/* Raw MFM track capacity: 12800 bytes = 102400 bits. */
#define FLOPPY_MFM_TRACK_BYTES 12800
#define FLOPPY_MFM_TRACK_BITS  (FLOPPY_MFM_TRACK_BYTES * 8)

/* Disk rotation speed: one track per 1/11 second ≈ 9.09 ticks at 100 Hz. */
#define FLOPPY_TICKS_PER_TRACK 9

/* Disk interrupt request bit (INTREQ bit 1, DSKBLK). */
#define FLOPPY_INTREQ_BIT 0x0002u

/* State of a single floppy drive. */
typedef struct {
    /* ADF data, 880 KiB. */
    uint8_t  adf[FLOPPY_DISK_SIZE];
    int      adf_loaded;

    /* Current physical position. */
    int      cyl;
    int      head;
    int      motor;
    int      write_protect;

    /* Rotation state: current bit position in the current MFM track. */
    uint32_t bit_pos;

    /* MFM bitstream for the current track, packed MSB-first. */
    uint8_t  mfm_track[FLOPPY_MFM_TRACK_BYTES];
    uint32_t mfm_bits;       /* number of valid bits in mfm_track */

    /* DMA transfer state. */
    int      dma_active;
    uint32_t dma_ptr;        /* chip RAM destination */
    uint16_t dma_words;      /* words remaining to transfer */
    uint16_t dma_sync;       /* sync word expected before transfer */
    int      dma_sync_found; /* 1 after sync has been seen */
} FloppyState;

extern FloppyState g_floppy;

/* Load an 880 KiB ADF image into the floppy. */
int  floppy_load_adf(const uint8_t *data, size_t size);

/* Generate a simple non-DOS test ADF in the internal buffer. */
void floppy_make_test_adf(void);

/* Motor / head / cylinder control. */
void floppy_set_motor(int on);
void floppy_seek(int cyl, int head);
void floppy_step(int dir);

/* Per-PIT-tick rotation and DMA advance. */
void floppy_tick(void);

/* Start a disk DMA read.  Returns 1 if accepted, 0 if no disk loaded. */
int  floppy_dma_read(uint32_t dskpt, uint16_t dsklen, uint16_t dsk_sync);

/* Start a disk DMA write (supported only for the synthetic buffer path). */
int  floppy_dma_write(uint32_t dskpt, uint16_t dsklen);

/* Single-word DSKDAT access. */
uint16_t floppy_dskdat_read(void);
void     floppy_dskdat_write(uint16_t value);

/* Decode an entire MFM track into the 11 sector data buffers.  Returns the
 * number of sectors successfully decoded. */
int floppy_decode_track(const uint8_t *mfm_bits, uint32_t bits_len,
                        uint8_t sectors[FLOPPY_SECTORS][FLOPPY_SECTOR_SIZE]);

/* Direct decoded sector read, useful for DOS handlers and diagnostics. */
int floppy_read_sector(int track, int sector, uint8_t *out);

#endif /* UAOS_FLOPPY_H */
