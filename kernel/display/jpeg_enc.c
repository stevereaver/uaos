/* jpeg_enc.c — minimal baseline JPEG encoder (freestanding, no libc)
 *
 * Produces a JFIF-compatible baseline stream: SOI, APP0, DQT (luma+chroma),
 * SOF0 (3 components, Y sampled 2x2), DHT (Annex-K tables), SOS, entropy
 * data with 0xFF byte stuffing, EOI.
 *
 * The DCT is a direct separable transform through a precomputed 8x8
 * fixed-point matrix (Q14): F = M f M^T.  1024 multiplies per block is
 * far slower than the AAN/Loeffler factorisations but is trivially
 * verifiable and plenty fast for screenshot-sized images.
 */

#include "jpeg_enc.h"
#include <string.h>

/* ------------------------------------------------------------------ */
/* Buffered byte sink                                                  */
/* ------------------------------------------------------------------ */

#define JENC_BUFSZ 1024

typedef struct {
    JpegWriteFn write;
    void       *ud;
    uint8_t     buf[JENC_BUFSZ];
    uint32_t    len;
    int         err;
} JSink;

static void sink_flush(JSink *s)
{
    if (s->len && !s->err) {
        if (s->write(s->ud, s->buf, s->len))
            s->err = 1;
    }
    s->len = 0;
}

static void sink_byte(JSink *s, uint32_t b)
{
    if (s->len == JENC_BUFSZ) sink_flush(s);
    s->buf[s->len++] = (uint8_t)b;
}

static void sink_u16(JSink *s, uint32_t v)
{
    sink_byte(s, (v >> 8) & 0xFF);
    sink_byte(s, v & 0xFF);
}

static void sink_data(JSink *s, const uint8_t *p, uint32_t n)
{
    while (n--) sink_byte(s, *p++);
}

/* ------------------------------------------------------------------ */
/* Entropy bit writer — MSB-first, 0xFF -> 0xFF 0x00 stuffing           */
/* ------------------------------------------------------------------ */

typedef struct {
    JSink   *s;
    uint32_t acc;
    int      nbits;
} JBits;

static void bits_put(JBits *b, uint32_t code, int len)
{
    if (len <= 0) return;
    b->acc = (b->acc << len) | (code & ((len >= 32) ? 0xFFFFFFFFu : ((1u << len) - 1)));
    b->nbits += len;
    while (b->nbits >= 8) {
        uint8_t byte = (uint8_t)(b->acc >> (b->nbits - 8));
        sink_byte(b->s, byte);
        if (byte == 0xFF) sink_byte(b->s, 0x00);   /* stuff */
        b->nbits -= 8;
        b->acc &= (b->nbits > 0) ? ((1u << b->nbits) - 1) : 0;
    }
}

static void bits_flush(JBits *b)
{
    if (b->nbits > 0)                       /* pad with 1-bits per spec */
        bits_put(b, 0xFF, 8 - b->nbits);
}

/* ------------------------------------------------------------------ */
/* Huffman tables — JPEG Annex K (typical)                             */
/* ------------------------------------------------------------------ */

static const uint8_t k_bits_dc_luma[16]   = { 0,1,5,1,1,1,1,1,1,0,0,0,0,0,0,0 };
static const uint8_t k_bits_dc_chroma[16] = { 0,3,1,1,1,1,1,1,1,1,1,0,0,0,0,0 };
static const uint8_t k_val_dc[12]         = { 0,1,2,3,4,5,6,7,8,9,10,11 };

static const uint8_t k_bits_ac_luma[16]   = { 0,2,1,3,3,2,4,3,5,5,4,4,0,0,1,0x7d };
static const uint8_t k_val_ac_luma[162]   = {
    0x01,0x02,0x03,0x00,0x04,0x11,0x05,0x12,
    0x21,0x31,0x41,0x06,0x13,0x51,0x61,0x07,
    0x22,0x71,0x14,0x32,0x81,0x91,0xa1,0x08,
    0x23,0x42,0xb1,0xc1,0x15,0x52,0xd1,0xf0,
    0x24,0x33,0x62,0x72,0x82,0x09,0x0a,0x16,
    0x17,0x18,0x19,0x1a,0x25,0x26,0x27,0x28,
    0x29,0x2a,0x34,0x35,0x36,0x37,0x38,0x39,
    0x3a,0x43,0x44,0x45,0x46,0x47,0x48,0x49,
    0x4a,0x53,0x54,0x55,0x56,0x57,0x58,0x59,
    0x5a,0x63,0x64,0x65,0x66,0x67,0x68,0x69,
    0x6a,0x73,0x74,0x75,0x76,0x77,0x78,0x79,
    0x7a,0x83,0x84,0x85,0x86,0x87,0x88,0x89,
    0x8a,0x92,0x93,0x94,0x95,0x96,0x97,0x98,
    0x99,0x9a,0xa2,0xa3,0xa4,0xa5,0xa6,0xa7,
    0xa8,0xa9,0xaa,0xb2,0xb3,0xb4,0xb5,0xb6,
    0xb7,0xb8,0xb9,0xba,0xc2,0xc3,0xc4,0xc5,
    0xc6,0xc7,0xc8,0xc9,0xca,0xd2,0xd3,0xd4,
    0xd5,0xd6,0xd7,0xd8,0xd9,0xda,0xe1,0xe2,
    0xe3,0xe4,0xe5,0xe6,0xe7,0xe8,0xe9,0xea,
    0xf1,0xf2,0xf3,0xf4,0xf5,0xf6,0xf7,0xf8,
    0xf9,0xfa
};

static const uint8_t k_bits_ac_chroma[16] = { 0,2,1,2,4,4,3,4,7,5,4,4,0,1,2,0x77 };
static const uint8_t k_val_ac_chroma[162] = {
    0x00,0x01,0x02,0x03,0x11,0x04,0x05,0x21,
    0x31,0x06,0x12,0x41,0x51,0x07,0x61,0x71,
    0x13,0x22,0x32,0x81,0x08,0x14,0x42,0x91,
    0xa1,0xb1,0xc1,0x09,0x23,0x33,0x52,0xf0,
    0x15,0x62,0x72,0xd1,0x0a,0x16,0x24,0x34,
    0xe1,0x25,0xf1,0x17,0x18,0x19,0x1a,0x26,
    0x27,0x28,0x29,0x2a,0x35,0x36,0x37,0x38,
    0x39,0x3a,0x43,0x44,0x45,0x46,0x47,0x48,
    0x49,0x4a,0x53,0x54,0x55,0x56,0x57,0x58,
    0x59,0x5a,0x63,0x64,0x65,0x66,0x67,0x68,
    0x69,0x6a,0x73,0x74,0x75,0x76,0x77,0x78,
    0x79,0x7a,0x82,0x83,0x84,0x85,0x86,0x87,
    0x88,0x89,0x8a,0x92,0x93,0x94,0x95,0x96,
    0x97,0x98,0x99,0x9a,0xa2,0xa3,0xa4,0xa5,
    0xa6,0xa7,0xa8,0xa9,0xaa,0xb2,0xb3,0xb4,
    0xb5,0xb6,0xb7,0xb8,0xb9,0xba,0xc2,0xc3,
    0xc4,0xc5,0xc6,0xc7,0xc8,0xc9,0xca,0xd2,
    0xd3,0xd4,0xd5,0xd6,0xd7,0xd8,0xd9,0xda,
    0xe2,0xe3,0xe4,0xe5,0xe6,0xe7,0xe8,0xe9,
    0xea,0xf2,0xf3,0xf4,0xf5,0xf6,0xf7,0xf8,
    0xf9,0xfa
};

/* Annex-K quantisation tables (natural order), scaled by quality at run */
static const uint8_t k_qt_luma[64] = {
    16,11,10,16,24,40,51,61,
    12,12,14,19,26,58,60,55,
    14,13,16,24,40,57,69,56,
    14,17,22,29,51,87,80,62,
    18,22,37,56,68,109,103,77,
    24,35,55,64,81,104,113,92,
    49,64,78,87,103,121,120,101,
    72,92,95,98,112,100,103,99
};
static const uint8_t k_qt_chroma[64] = {
    17,18,24,47,99,99,99,99,
    18,21,26,66,99,99,99,99,
    24,26,56,99,99,99,99,99,
    47,66,99,99,99,99,99,99,
    99,99,99,99,99,99,99,99,
    99,99,99,99,99,99,99,99,
    99,99,99,99,99,99,99,99,
    99,99,99,99,99,99,99,99
};

/* Zigzag scan order: zigzag index -> natural (row*8+col) index */
static const uint8_t k_zigzag[64] = {
     0, 1, 8,16, 9, 2, 3,10,
    17,24,32,25,18,11, 4, 5,
    12,19,26,33,40,48,41,34,
    27,20,13, 6, 7,14,21,28,
    35,42,49,56,57,50,43,36,
    29,22,15,23,30,37,44,51,
    58,59,52,45,38,31,39,46,
    53,60,61,54,47,55,62,63
};

/* 1-D orthonormal DCT-II matrix in Q14: M[u][x] =
 * c(u)*sqrt(2/8)*cos((2x+1)*u*pi/16), c(0)=1/sqrt(2), else c(u)=1. */
#define DCT_SHIFT 14
static const int16_t k_dct[64] = {
     5793, 5793, 5793, 5793, 5793, 5793, 5793, 5793,
     8035, 6811, 4551, 1598,-1598,-4551,-6811,-8035,
     7568, 3135,-3135,-7568,-7568,-3135, 3135, 7568,
     6811,-1598,-8035,-4551, 4551, 8035, 1598,-6811,
     5793,-5793,-5793, 5793, 5793,-5793,-5793, 5793,
     4551,-8035, 1598, 6811,-6811,-1598, 8035,-4551,
     3135,-7568, 7568,-3135,-3135, 7568,-7568, 3135,
     1598,-4551, 6811,-8035, 8035,-6811, 4551,-1598
};

/* ------------------------------------------------------------------ */
/* Runtime tables                                                      */
/* ------------------------------------------------------------------ */

typedef struct {
    uint16_t code[256];
    uint8_t  len[256];
} JHuff;

static void huff_build(JHuff *t, const uint8_t *bits,
                       const uint8_t *vals, int nvals)
{
    memset(t, 0, sizeof(*t));
    uint32_t code = 0;
    int k = 0;
    for (int i = 0; i < 16 && k < nvals; i++) {
        for (int j = 0; j < bits[i]; j++) {
            uint8_t sym = vals[k++];
            t->code[sym] = (uint16_t)code;
            t->len[sym]  = (uint8_t)(i + 1);
            code++;
        }
        code <<= 1;
    }
}

static void huff_emit(JBits *b, const JHuff *t, uint8_t sym)
{
    bits_put(b, t->code[sym], t->len[sym]);
}

/* Quality-scaled quant table (libjpeg scaling), output in natural order;
 * callers zigzag-map for encoding and emit zigzag order in DQT. */
static void qt_build(uint16_t *out, const uint8_t *base, int quality)
{
    int scale = (quality < 50) ? (5000 / quality) : (200 - quality * 2);
    for (int i = 0; i < 64; i++) {
        int q = (base[i] * scale + 50) / 100;
        if (q < 1)   q = 1;
        if (q > 255) q = 255;
        out[i] = (uint16_t)q;
    }
}

/* ------------------------------------------------------------------ */
/* Block pipeline: FDCT -> quantise (output in zigzag order)           */
/* ------------------------------------------------------------------ */

/* in[64] natural order, level-shifted samples; out[64] zigzag-ordered
 * quantized coefficients.  qt must be in zigzag order too. */
static void fdct_quant(const int32_t *in, const uint16_t *qt, int32_t *out)
{
    int32_t tmp[64];        /* A[u][x] = sum_y M[u][y] * in[y][x] */

    for (int u = 0; u < 8; u++) {
        for (int x = 0; x < 8; x++) {
            int32_t acc = 0;
            for (int y = 0; y < 8; y++)
                acc += (int32_t)k_dct[u * 8 + y] * in[y * 8 + x];
            /* descale with rounding */
            tmp[u * 8 + x] = (acc >= 0) ? ((acc + (1 << (DCT_SHIFT - 1))) >> DCT_SHIFT)
                                        : -(((-acc) + (1 << (DCT_SHIFT - 1))) >> DCT_SHIFT);
        }
    }
    /* F[u][v] = sum_x A[u][x] * M[v][x]; emit in zigzag order */
    for (int z = 0; z < 64; z++) {
        int n = k_zigzag[z];
        int u = n >> 3, v = n & 7;
        int32_t acc = 0;
        for (int x = 0; x < 8; x++)
            acc += tmp[u * 8 + x] * (int32_t)k_dct[v * 8 + x];
        int32_t f = (acc >= 0) ? ((acc + (1 << (DCT_SHIFT - 1))) >> DCT_SHIFT)
                               : -(((-acc) + (1 << (DCT_SHIFT - 1))) >> DCT_SHIFT);
        int32_t q = qt[z];
        out[z] = (f >= 0) ? ((f + q / 2) / q) : -(((-f) + q / 2) / q);
    }
}

static int mag_bits(int32_t v)
{
    uint32_t a = (v < 0) ? (uint32_t)(-v) : (uint32_t)v;
    int n = 0;
    while (a) { n++; a >>= 1; }
    return n;
}

/* Emit one 8x8 block (coefficients already zigzag-ordered/quantised). */
static void enc_block(JBits *b, const int32_t *q, int32_t *pred,
                      const JHuff *dc, const JHuff *ac)
{
    int32_t diff = q[0] - *pred;
    *pred = q[0];
    int s = mag_bits(diff);
    huff_emit(b, dc, (uint8_t)s);
    if (s) {
        uint32_t v = (diff < 0) ? (uint32_t)(diff + (1 << s) - 1) : (uint32_t)diff;
        bits_put(b, v, s);
    }

    int run = 0;
    for (int i = 1; i < 64; i++) {
        int32_t v = q[i];
        if (!v) { run++; continue; }
        while (run > 15) { huff_emit(b, ac, 0xF0); run -= 16; }
        s = mag_bits(v);
        huff_emit(b, ac, (uint8_t)((run << 4) | s));
        bits_put(b, (v < 0) ? (uint32_t)(v + (1 << s) - 1) : (uint32_t)v, s);
        run = 0;
    }
    if (run) huff_emit(b, ac, 0x00);    /* EOB */
}

/* ------------------------------------------------------------------ */
/* RGB -> YCbCr (JFIF full-range, integer approximations)              */
/* ------------------------------------------------------------------ */

static int32_t rgb_y(uint32_t p)
{
    int r = (int)((p >> 16) & 0xFF), g = (int)((p >> 8) & 0xFF), b = (int)(p & 0xFF);
    return (77 * r + 150 * g + 29 * b + 128) >> 8;
}
static int32_t rgb_cb(uint32_t p)
{
    int r = (int)((p >> 16) & 0xFF), g = (int)((p >> 8) & 0xFF), b = (int)(p & 0xFF);
    return ((-44 * r - 87 * g + 131 * b + 128) >> 8) + 128;
}
static int32_t rgb_cr(uint32_t p)
{
    int r = (int)((p >> 16) & 0xFF), g = (int)((p >> 8) & 0xFF), b = (int)(p & 0xFF);
    return ((131 * r - 110 * g - 21 * b + 128) >> 8) + 128;
}

/* ------------------------------------------------------------------ */
/* Header emission                                                     */
/* ------------------------------------------------------------------ */

/* qt_y/qt_c are zigzag-ordered, matching the DQT serialisation order */
static void emit_headers(JSink *s, int w, int h,
                         const uint16_t *qt_y, const uint16_t *qt_c)
{
    sink_u16(s, 0xFFD8);                                    /* SOI */

    sink_u16(s, 0xFFE0); sink_u16(s, 16);                   /* APP0 JFIF */
    sink_data(s, (const uint8_t *)"JFIF\0", 5);
    sink_byte(s, 1); sink_byte(s, 1);                       /* version 1.1 */
    sink_byte(s, 0);                                        /* no units */
    sink_u16(s, 1); sink_u16(s, 1);                         /* X/Y density */
    sink_byte(s, 0); sink_byte(s, 0);                       /* no thumbnail */

    /* DQT — both tables in one segment, values in zigzag order */
    sink_u16(s, 0xFFDB); sink_u16(s, 2 + 2 * (1 + 64));
    sink_byte(s, 0x00);                                     /* Pq=0 Tq=0 */
    for (int i = 0; i < 64; i++) sink_byte(s, qt_y[i]);
    sink_byte(s, 0x01);                                     /* Pq=0 Tq=1 */
    for (int i = 0; i < 64; i++) sink_byte(s, qt_c[i]);

    sink_u16(s, 0xFFC0); sink_u16(s, 8 + 3 * 3);            /* SOF0 */
    sink_byte(s, 8);                                        /* precision */
    sink_u16(s, (uint32_t)h); sink_u16(s, (uint32_t)w);
    sink_byte(s, 3);                                        /* components */
    sink_byte(s, 1); sink_byte(s, 0x22); sink_byte(s, 0);   /* Y: 2x2, qt0 */
    sink_byte(s, 2); sink_byte(s, 0x11); sink_byte(s, 1);   /* Cb: 1x1, qt1 */
    sink_byte(s, 3); sink_byte(s, 0x11); sink_byte(s, 1);   /* Cr: 1x1, qt1 */

    /* DHT — all four tables in one segment */
    sink_u16(s, 0xFFC4);
    sink_u16(s, 2 + (1 + 16 + 12) + (1 + 16 + 162) +
                  (1 + 16 + 12) + (1 + 16 + 162));
    sink_byte(s, 0x00);                                     /* DC0 */
    sink_data(s, k_bits_dc_luma, 16);   sink_data(s, k_val_dc, 12);
    sink_byte(s, 0x10);                                     /* AC0 */
    sink_data(s, k_bits_ac_luma, 16);   sink_data(s, k_val_ac_luma, 162);
    sink_byte(s, 0x01);                                     /* DC1 */
    sink_data(s, k_bits_dc_chroma, 16); sink_data(s, k_val_dc, 12);
    sink_byte(s, 0x11);                                     /* AC1 */
    sink_data(s, k_bits_ac_chroma, 16); sink_data(s, k_val_ac_chroma, 162);

    sink_u16(s, 0xFFDA); sink_u16(s, 6 + 2 * 3);            /* SOS */
    sink_byte(s, 3);
    sink_byte(s, 1); sink_byte(s, 0x00);                    /* Y  -> DC0/AC0 */
    sink_byte(s, 2); sink_byte(s, 0x11);                    /* Cb -> DC1/AC1 */
    sink_byte(s, 3); sink_byte(s, 0x11);                    /* Cr -> DC1/AC1 */
    sink_byte(s, 0); sink_byte(s, 63); sink_byte(s, 0);     /* Ss,Se,AhAl */
}

/* ------------------------------------------------------------------ */
/* Public entry                                                        */
/* ------------------------------------------------------------------ */

int Jpeg_Encode(JpegWriteFn write, void *write_ud,
                JpegPixelFn px, void *px_ud,
                int width, int height, int quality)
{
    if (!write || !px || width < 1 || height < 1 ||
        width > 16384 || height > 16384)
        return -1;
    if (quality < 1)   quality = 1;
    if (quality > 100) quality = 100;

    JSink s;
    s.write = write; s.ud = write_ud; s.len = 0; s.err = 0;

    /* Quant tables in zigzag order (used for both DQT and encoding) */
    uint16_t qt_y[64], qt_c[64];
    {
        uint16_t nat[64];
        qt_build(nat, k_qt_luma, quality);
        for (int i = 0; i < 64; i++) qt_y[i] = nat[k_zigzag[i]];
        qt_build(nat, k_qt_chroma, quality);
        for (int i = 0; i < 64; i++) qt_c[i] = nat[k_zigzag[i]];
    }

    JHuff hdc_y, hac_y, hdc_c, hac_c;
    huff_build(&hdc_y, k_bits_dc_luma,   k_val_dc,        12);
    huff_build(&hac_y, k_bits_ac_luma,   k_val_ac_luma,   162);
    huff_build(&hdc_c, k_bits_dc_chroma, k_val_dc,        12);
    huff_build(&hac_c, k_bits_ac_chroma, k_val_ac_chroma, 162);

    emit_headers(&s, width, height, qt_y, qt_c);

    JBits bw;
    bw.s = &s; bw.acc = 0; bw.nbits = 0;

    int32_t pred[3] = { 0, 0, 0 };
    int32_t yblk[64], cbblk[64], crblk[64], coef[64];

    for (int my = 0; my < height; my += 16) {
        for (int mx = 0; mx < width; mx += 16) {
            /* Fetch the 16x16 MCU pixels once, edges replicated. */
            uint32_t pix[256];
            for (int y = 0; y < 16; y++) {
                int sy = my + y; if (sy >= height) sy = height - 1;
                for (int x = 0; x < 16; x++) {
                    int sx = mx + x; if (sx >= width) sx = width - 1;
                    pix[y * 16 + x] = px(px_ud, sx, sy);
                }
            }

            /* Four luma blocks, MCU order: TL, TR, BL, BR */
            for (int sub = 0; sub < 4; sub++) {
                int ox = (sub & 1) * 8, oy = (sub >> 1) * 8;
                for (int y = 0; y < 8; y++)
                    for (int x = 0; x < 8; x++)
                        yblk[y * 8 + x] = rgb_y(pix[(oy + y) * 16 + (ox + x)]) - 128;
                fdct_quant(yblk, qt_y, coef);
                enc_block(&bw, coef, &pred[0], &hdc_y, &hac_y);
            }

            /* Chroma blocks: 2x2 average, then convert */
            for (int y = 0; y < 8; y++) {
                for (int x = 0; x < 8; x++) {
                    uint32_t p00 = pix[(y * 2) * 16 + x * 2];
                    uint32_t p01 = pix[(y * 2) * 16 + x * 2 + 1];
                    uint32_t p10 = pix[(y * 2 + 1) * 16 + x * 2];
                    uint32_t p11 = pix[(y * 2 + 1) * 16 + x * 2 + 1];
                    uint32_t r = (((p00 >> 16) & 0xFF) + ((p01 >> 16) & 0xFF) +
                                  ((p10 >> 16) & 0xFF) + ((p11 >> 16) & 0xFF)) >> 2;
                    uint32_t g = (((p00 >> 8) & 0xFF) + ((p01 >> 8) & 0xFF) +
                                  ((p10 >> 8) & 0xFF) + ((p11 >> 8) & 0xFF)) >> 2;
                    uint32_t b = ((p00 & 0xFF) + (p01 & 0xFF) +
                                  (p10 & 0xFF) + (p11 & 0xFF)) >> 2;
                    uint32_t avg = (r << 16) | (g << 8) | b;
                    cbblk[y * 8 + x] = rgb_cb(avg) - 128;
                    crblk[y * 8 + x] = rgb_cr(avg) - 128;
                }
            }
            fdct_quant(cbblk, qt_c, coef);
            enc_block(&bw, coef, &pred[1], &hdc_c, &hac_c);
            fdct_quant(crblk, qt_c, coef);
            enc_block(&bw, coef, &pred[2], &hdc_c, &hac_c);
        }
    }

    bits_flush(&bw);
    sink_u16(&s, 0xFFD9);                                   /* EOI */
    sink_flush(&s);

    return s.err ? -2 : 0;
}
