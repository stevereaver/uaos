/* uaos_libc.h — minimal freestanding C helpers for UAOS userspace programs
 *
 * Header-only static inline implementations of the tiny subset of the
 * standard C library that native UAOS programs need.  No standard library
 * is linked, so these functions are intentionally small and self-contained.
 */

#ifndef UAOS_LIBC_H
#define UAOS_LIBC_H

#include <stdint.h>
#include <stddef.h>

static inline size_t uaos_strlen(const char *s)
{
    size_t n = 0;
    while (s[n])
        n++;
    return n;
}

static inline int uaos_strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

static inline int uaos_strncmp(const char *a, const char *b, size_t n)
{
    while (n && *a && *a == *b) {
        a++;
        b++;
        n--;
    }
    if (n == 0)
        return 0;
    return (unsigned char)*a - (unsigned char)*b;
}

static inline char *uaos_strcpy(char *dst, const char *src)
{
    size_t i = 0;
    while (src[i]) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
    return dst;
}

static inline char *uaos_strncpy(char *dst, const char *src, size_t n)
{
    size_t i = 0;
    while (i < n && src[i]) {
        dst[i] = src[i];
        i++;
    }
    while (i < n) {
        dst[i] = '\0';
        i++;
    }
    return dst;
}

static inline void *uaos_memcpy(void *dst, const void *src, size_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n--)
        *d++ = *s++;
    return dst;
}

static inline void *uaos_memset(void *dst, int c, size_t n)
{
    uint8_t *d = (uint8_t *)dst;
    while (n--)
        *d++ = (uint8_t)c;
    return dst;
}

static inline int uaos_memcmp(const void *a, const void *b, size_t n)
{
    const uint8_t *pa = (const uint8_t *)a;
    const uint8_t *pb = (const uint8_t *)b;
    while (n--) {
        if (*pa != *pb)
            return (int)*pa - (int)*pb;
        pa++;
        pb++;
    }
    return 0;
}

static inline const char *uaos_strchr(const char *s, int c)
{
    while (*s) {
        if (*s == (char)c)
            return s;
        s++;
    }
    return NULL;
}

static inline int uaos_isdigit(int c)
{
    return (unsigned char)c >= '0' && (unsigned char)c <= '9';
}

static inline int uaos_isprint(int c)
{
    unsigned char uc = (unsigned char)c;
    return uc >= 32 && uc < 127;
}

static inline int uaos_isspace(int c)
{
    unsigned char uc = (unsigned char)c;
    return uc == ' ' || uc == '\t' || uc == '\n' || uc == '\r' || uc == '\f' || uc == '\v';
}

static inline int uaos_toupper(int c)
{
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 'A';
    return c;
}

static inline int uaos_tolower(int c)
{
    if (c >= 'A' && c <= 'Z')
        return c - 'A' + 'a';
    return c;
}

/* Append src to dst, bounded by max total bytes (including NUL). */
static inline void uaos_strlcat(char *dst, const char *src, size_t max)
{
    size_t dst_len = uaos_strlen(dst);
    size_t i = 0;
    while (dst_len + i + 1 < max && src[i]) {
        dst[dst_len + i] = src[i];
        i++;
    }
    dst[dst_len + i] = '\0';
}

#endif /* UAOS_LIBC_H */
