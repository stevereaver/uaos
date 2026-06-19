/*
 * kprint.h — Kernel Console Output
 *
 * Simple console output function for kernel boot messages.
 * Outputs to both VGA text mode and UART.
 */

#ifndef UAOS_KPRINT_H
#define UAOS_KPRINT_H

#include <stdint.h>
#include <stddef.h>

/* Print a string to console */
void kprint(const char *s);

/* Print a hex number to console */
void kprinthex(uint64_t v);

/* Print a decimal number to console */
void kprintdec(uint32_t v);

/* Print a fixed-length buffer to console */
void kprintbuf(const char *s, size_t len);

#endif /* UAOS_KPRINT_H */
