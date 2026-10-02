/* kernel/stubs.c — freestanding symbol stubs
 *
 * Provides stub implementations for symbols the freestanding link cannot
 * otherwise resolve (printf-family used by Musashi, framebuffer globals),
 * plus minimal string/memory routines (integer-only — SSE is not context-
 * switched) and the Musashi disassembler support shims.
 *
 * Previously generated inline by scripts/build_iso.sh; now a checked-in
 * source compiled like any other kernel file.
 */

#include <stdint.h>

/* Screen size for PS/2 mouse clamp — populated by kernel before PS2Mouse_Init */
unsigned int g_fb_width_irq  = 1024;
unsigned int g_fb_height_irq = 768;

/* vfprintf / fprintf / printf / sprintf / sscanf / exit stubs for Musashi */
typedef __builtin_va_list va_list;
#define va_start(v,l) __builtin_va_start(v,l)
#define va_end(v)     __builtin_va_end(v)
#define va_arg(v,l)   __builtin_va_arg(v,l)
typedef void FILE2;
extern FILE2 *stderr;
int vfprintf(FILE2 *f, const char *fmt, va_list ap) {
    (void)f; (void)fmt; (void)ap; return 0;
}

/* Minimal vsprintf core for Musashi's disassembler.
 * Supports: %s %c %d %i %u %x %X %p %% and %-Ns / %0Nu widths. */
static int _u2a(unsigned long v, char *out, int base, int upper) {
    char tmp[32]; int n = 0, i = 0;
    const char *dig = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    if (!v) { out[0] = '0'; return 1; }
    while (v) { tmp[n++] = dig[v % (unsigned)base]; v /= (unsigned)base; }
    while (n) out[i++] = tmp[--n];
    return i;
}
static int _vsfmt(char *buf, const char *fmt, va_list ap) {
    char *o = buf;
    for (; *fmt; fmt++) {
        if (*fmt != '%') { *o++ = *fmt; continue; }
        fmt++;
        int left = 0, width = 0, pad0 = 0;
        if (*fmt == '-') { left = 1; fmt++; }
        if (*fmt == '0' && !left) { pad0 = 1; fmt++; }
        while (*fmt >= '0' && *fmt <= '9') { width = width * 10 + (*fmt - '0'); fmt++; }
        if (*fmt == 'l') fmt++;              /* swallow %l* — we always use 32-bit */
        if (*fmt == 'l') fmt++;
        if (*fmt == 'z' || *fmt == 't' || *fmt == 'j') fmt++;
        switch (*fmt) {
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            int len = 0; while (s[len]) len++;
            int pad = width > len ? width - len : 0;
            if (!left) while (pad--) *o++ = ' ';
            while (*s) *o++ = *s++;
            if (left) while (pad--) *o++ = ' ';
            break;
        }
        case 'c': *o++ = (char)va_arg(ap, int); break;
        case 'd': case 'i': {
            long v = va_arg(ap, int);
            if (v < 0) { *o++ = '-'; v = -v; }
            char tmp[32]; int len = _u2a((unsigned long)v, tmp, 10, 0);
            while (width > len) { *o++ = pad0 ? '0' : ' '; width--; }
            for (int i = 0; i < len; i++) *o++ = tmp[i];
            break;
        }
        case 'u': {
            unsigned long v = va_arg(ap, unsigned int);
            char tmp[32]; int len = _u2a(v, tmp, 10, 0);
            while (width > len) { *o++ = pad0 ? '0' : ' '; width--; }
            for (int i = 0; i < len; i++) *o++ = tmp[i];
            break;
        }
        case 'x': case 'X': {
            unsigned long v = va_arg(ap, unsigned int);
            char tmp[32]; int len = _u2a(v, tmp, 16, *fmt == 'X');
            while (width > len) { *o++ = pad0 ? '0' : ' '; width--; }
            for (int i = 0; i < len; i++) *o++ = tmp[i];
            break;
        }
        case 'p': {
            unsigned long v = (unsigned long)va_arg(ap, void *);
            char tmp[32]; int len = _u2a(v, tmp, 16, 0);
            *o++ = '0'; *o++ = 'x';
            for (int i = 0; i < len; i++) *o++ = tmp[i];
            break;
        }
        case '%': *o++ = '%'; break;
        default:  *o++ = '%'; if (*fmt) *o++ = *fmt; break;
        }
    }
    *o = '\0';
    return (int)(o - buf);
}
int sprintf(char *buf, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int n = _vsfmt(buf, fmt, ap);
    va_end(ap);
    return n;
}
int sscanf(const char *s, const char *fmt, ...) {
    (void)s; (void)fmt; return 0;
}
int __isoc99_sscanf(const char *s, const char *fmt, ...) {
    (void)s; (void)fmt; return 0;
}
void exit(int code) { (void)code; for(;;) __asm__ volatile("hlt"); }
double sin(double x)  { (void)x; return 0.0; }
double cos(double x)  { (void)x; return 1.0; }
/* setjmp/longjmp stubs — Musashi uses these for exception unwinding.
 * In a bare-metal single-threaded kernel we just halt on longjmp. */
typedef long long jmp_buf[8];
int  _setjmp(jmp_buf *e)          { (void)e; return 0; }
void longjmp(jmp_buf *e, int v)   { (void)e; (void)v;
    for(;;) __asm__ volatile("hlt"); }

/* String / memory stubs — dword string ops for the bulk then byte tail.
 * rep movsl/stosl move 4 bytes per iteration instead of 1 (the framebuffer
 * flip memcpys MBs per repaint), and stay integer-only: no SSE, which the
 * scheduler does not context-switch. */
void *memset(void *d, int c, unsigned long n) {
    unsigned char *p = (unsigned char *)d;
    unsigned long cc = (unsigned char)c;
    cc |= cc << 8; cc |= cc << 16;
    unsigned long nq = n >> 2, nb = n & 3;
    __asm__ volatile("rep stosl" : "+D"(p), "+c"(nq) : "a"(cc) : "memory");
    __asm__ volatile("rep stosb" : "+D"(p), "+c"(nb) : "a"(cc) : "memory");
    return d;
}
void *memcpy(void *d, const void *s, unsigned long n) {
    unsigned char *dp = (unsigned char *)d;
    const unsigned char *sp = (const unsigned char *)s;
    unsigned long nq = n >> 2, nb = n & 3;
    __asm__ volatile("rep movsl" : "+D"(dp), "+S"(sp), "+c"(nq) :: "memory");
    __asm__ volatile("rep movsb" : "+D"(dp), "+S"(sp), "+c"(nb) :: "memory");
    return d;
}
int memcmp(const void *a, const void *b, unsigned long n) {
    const unsigned char *ap = (const unsigned char *)a;
    const unsigned char *bp = (const unsigned char *)b;
    while (n--) {
        if (*ap != *bp) return (int)*ap - (int)*bp;
        ap++; bp++;
    }
    return 0;
}
int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}
unsigned long strlen(const char *s) {
    unsigned long n = 0;
    while (*s++) n++;
    return n;
}
char *strcpy(char *d, const char *s) {
    char *r = d;
    while ((*d++ = *s++)) {}
    return r;
}
char *strcat(char *d, const char *s) {
    char *r = d;
    while (*d) d++;
    while ((*d++ = *s++)) {}
    return r;
}
/* Simple insertion-sort qsort for Musashi's disassembler opcode table. */
void qsort(void *base, unsigned long nmemb, unsigned long size,
           int (*compar)(const void *, const void *)) {
    unsigned char *b = (unsigned char *)base;
    unsigned char tmp[64];
    if (size > sizeof(tmp)) return;   /* disasm entries are small */
    for (unsigned long i = 1; i < nmemb; i++) {
        memcpy(tmp, b + i * size, size);
        long j = (long)i - 1;
        while (j >= 0 && compar(b + (unsigned long)j * size, tmp) > 0) {
            memcpy(b + ((unsigned long)j + 1) * size,
                   b + (unsigned long)j * size, size);
            j--;
        }
        memcpy(b + ((unsigned long)j + 1) * size, tmp, size);
    }
}

/* Serial UART output used by the kernel in place of fprintf */
static inline void _uart_putc(char c) {
    /* Wait for transmit-hold-empty on COM1 */
    __asm__ volatile (
        "   movw $0x3FD, %%dx\n"
        "1: inb %%dx, %%al\n"
        "   testb $0x20, %%al\n"
        "   jz 1b\n"
        "   movb %0, %%al\n"
        "   movw $0x3F8, %%dx\n"
        "   outb %%al, %%dx\n"
        :: "r"((unsigned char)c) : "eax", "edx"
    );
}
static void _uart_puts(const char *s) {
    while (*s) { if (*s == '\n') _uart_putc('\r'); _uart_putc(*s++); }
}

/* fprintf stub — formats via _vsfmt then emits through klog so the text
 * lands in the ring buffer, UART, and fbcon with the [kern] tag.
 * Previously this printed the raw format string (literal "%s"/"%u"). */
typedef void FILE;
extern FILE *stderr;
FILE *stderr = (FILE*)0;
extern void klog_emit(int subsys, int level, const char *fmt, ...);

int fprintf(FILE *f, const char *fmt, ...) {
    (void)f;
    static char fbuf[512];
    va_list ap; va_start(ap, fmt);
    int n = _vsfmt(fbuf, fmt, ap);
    va_end(ap);
    fbuf[n] = '\0';
    klog_emit(0, 3, "%s", fbuf);     /* KLOG_KERN / KLOG_INFO */
    return n;
}
int printf(const char *fmt, ...) {
    static char pbuf[512];
    va_list ap; va_start(ap, fmt);
    int n = _vsfmt(pbuf, fmt, ap);
    va_end(ap);
    pbuf[n] = '\0';
    klog_emit(0, 3, "%s", pbuf);
    return n;
}
