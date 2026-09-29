/*
 * string.h — freestanding shim for compiling vendored BearSSL.
 *
 * Only used via -I when building system/bearssl sources; the matching
 * implementations live in system/libuaos/uaos_tls.h (one definition
 * emitted per tool that links BearSSL).
 */
#ifndef UAOS_BEARSSL_COMPAT_STRING_H
#define UAOS_BEARSSL_COMPAT_STRING_H

#include <stddef.h>

void   *memcpy(void *dst, const void *src, size_t n);
void   *memmove(void *dst, const void *src, size_t n);
void   *memset(void *dst, int c, size_t n);
int     memcmp(const void *a, const void *b, size_t n);
size_t  strlen(const char *s);
int     strcmp(const char *a, const char *b);

#endif
