/* jpeg_enc.h — minimal baseline JPEG encoder (freestanding, no libc)
 *
 * Encodes an RGB source into a baseline sequential-DCT JFIF stream:
 * 8-bit samples, 3-component YCbCr, 4:2:0 subsampling (2x2 chroma),
 * standard Annex-K Huffman tables.  Output is streamed through a
 * caller-supplied write callback so no image-size buffer is needed.
 */

#ifndef UAOS_JPEG_ENC_H
#define UAOS_JPEG_ENC_H

#include <stdint.h>

/* Fetch one source pixel as 0x00RRGGBB.  The encoder clamps sampling
 * coordinates to the image bounds (edge replication), so the callback
 * is only ever invoked with 0 <= x < width and 0 <= y < height. */
typedef uint32_t (*JpegPixelFn)(void *user, int x, int y);

/* Sink for encoded bytes.  Must return 0 on success, non-zero on error
 * (encoding aborts and Jpeg_Encode reports the failure). */
typedef int (*JpegWriteFn)(void *user, const uint8_t *data, uint32_t len);

/* Encode width×height RGB pixels to baseline JPEG.
 *   quality : 1..100 (75–90 is the sane range; clamped)
 * Returns 0 on success, -1 on bad parameters, -2 on sink write error. */
int Jpeg_Encode(JpegWriteFn write, void *write_ud,
                JpegPixelFn px,   void *px_ud,
                int width, int height, int quality);

#endif /* UAOS_JPEG_ENC_H */
