/*
 * time.h — freestanding shim for compiling vendored BearSSL.
 *
 * x509_minimal.c calls time(NULL) only when the application has not
 * provided validation time via br_x509_minimal_set_time(); uaos_tls.h
 * always sets the time from SYSCALL_TIME, but the symbol must still
 * resolve.  The implementation lives in uaos_tls.h.
 */
#ifndef UAOS_BEARSSL_COMPAT_TIME_H
#define UAOS_BEARSSL_COMPAT_TIME_H

typedef long time_t;
time_t time(time_t *t);

#endif
