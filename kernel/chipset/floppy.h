/*
 * floppy.h — UAOS Amiga MFM floppy controller
 *
 * Exposes up to four ADF-backed floppy drives (DF0:-DF3:) that can be read
 * through the Paula disk DMA registers (DSKSYNC, DSKLEN, DSKDAT, DSKPT).
 * Tracks are encoded to MFM on demand and streamed to chip RAM as the
 * virtual disk rotates.  Drive select/motor/step/head control follows the
 * Amiga CIAB PRB / CIAA PRA semantics.
 */

#ifndef UAOS_FLOPPY_H
#define UAOS_FLOPPY_H

#include <stdint.h>
#include <stddef.h>

#define FLOPPY_UNITS        4

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
    int      dirty;         /* ADF contents modified since load */

    /* Current physical position. */
    int      cyl;
    int      head;
    int      motor;
    int      write_protect;
    int      chng_cleared;  /* 0 = /CHNG asserted (reset/eject/load); set by a step with disk in */

    /* Rotation state: current bit position in the current MFM track. */
    uint32_t bit_pos;

    /* MFM bitstream for the current track, packed MSB-first. */
    uint8_t  mfm_track[FLOPPY_MFM_TRACK_BYTES];
    uint32_t mfm_bits;       /* number of valid bits in mfm_track */

    /* DMA transfer state. */
    int      dma_active;
    int      dma_write;      /* 1 if this is a write DMA, 0 if read */
    uint32_t dma_ptr;        /* chip RAM destination */
    uint16_t dma_words;      /* words remaining to transfer */
    uint16_t dma_sync;       /* sync word expected before transfer */
    int      dma_sync_found; /* 1 after sync has been seen */
    uint8_t *dma_ram;        /* RAM window the DMA was launched against */
} FloppyState;

extern FloppyState g_floppy[FLOPPY_UNITS];

/* Load an 880 KiB ADF image into a drive (unit 0..3). */
int  floppy_load_adf(int unit, const uint8_t *data, size_t size);

/* Remove the disk from a drive. */
void floppy_eject(int unit);

/* Returns 1 if the unit has a disk loaded. */
int  floppy_unit_loaded(int unit);

/* Generate a simple non-DOS test ADF in the unit's internal buffer. */
void floppy_make_test_adf(int unit);

/* Motor / head / cylinder control. */
void floppy_set_motor(int unit, int on);
void floppy_seek(int unit, int cyl, int head);
void floppy_step(int unit, int dir);

/* Per-PIT-tick rotation and DMA advance for all units. */
void floppy_tick(void);

/* Start a disk DMA read.  Returns 1 if accepted, 0 if no disk loaded. */
int  floppy_dma_read(int unit, uint32_t dskpt, uint16_t dsklen, uint16_t dsk_sync);

/* Start a disk DMA write (supported only for the synthetic buffer path). */
int  floppy_dma_write(int unit, uint32_t dskpt, uint16_t dsklen);

/* Single-word DSKDAT access. */
uint16_t floppy_dskdat_read(int unit);
void     floppy_dskdat_write(int unit, uint16_t value);

/* Drive control port (Amiga CIAB PRB): /STEP, DIR, /SIDE, /SEL0-3, /MTR. */
void     floppy_drive_control(uint8_t prb);

/* Unit currently selected by the drive-control port (last /SEL low, else 0). */
int      floppy_selected_unit(void);

/* CIAA PRA bits 2-5 for the selected drive: /CHNG, /WPRO, /TK0, /RDY
 * (active low).  Returns 0x3C (all inactive) when no drive is selected. */
uint8_t  floppy_ciaa_pra_bits(void);

/* Called when an M68k task's RAM window is being released.  If a disk DMA
 * was launched against that window it is disarmed so floppy_tick() cannot
 * write into the next tenant of the slot.  Returns 1 if a binding was
 * cleared (caller may then drop the DSKEN bit in DMACON). */
int  floppy_unbind_ram(uint8_t *ram);

/* Decode an entire MFM track into the 11 sector data buffers.  Returns the
 * number of sectors successfully decoded. */
int floppy_decode_track(const uint8_t *mfm_bits, uint32_t bits_len,
                        uint8_t sectors[FLOPPY_SECTORS][FLOPPY_SECTOR_SIZE]);

/* Direct decoded sector read, useful for DOS handlers and diagnostics. */
int floppy_read_sector(int unit, int track, int sector, uint8_t *out);

/* Direct decoded sector write. */
int floppy_write_sector(int unit, int track, int sector, const uint8_t *data);

/* Dirty tracking: set by sector/DMA writes, cleared by load/eject and
 * floppy_clear_dirty.  floppy_adf_data returns the unit's ADF buffer. */
int          floppy_is_dirty(int unit);
void         floppy_clear_dirty(int unit);
const uint8_t *floppy_adf_data(int unit);

/* Write-protect control and diagnostics. */
void floppy_set_write_protect(int unit, int wp);
int floppy_write_protect_test(void);
int floppy_dma_write_test(void);

#endif /* UAOS_FLOPPY_H */
