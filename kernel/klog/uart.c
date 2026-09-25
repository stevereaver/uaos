/*
 * uart.c — 16550A serial UART driver (COM1 = 0x3F8)
 *
 * The canonical UART implementation for the kernel.  Previously these
 * helpers lived statically in uaos_kernel_main.c and were duplicated
 * per-file (_ip_outb, _dh_outb, _e_outb, ...).  klog and kprint share
 * this path; output is polled, so it is safe from any context including
 * IRQ handlers and packet dispatch.
 */

#include "klog.h"

#define UART_BASE  0x3F8

static inline void uart_outb(uint16_t port, uint8_t val)
{
    __asm__ volatile ("outb %0, %1" :: "a"(val), "Nd"(port));
}

static inline uint8_t uart_inb(uint16_t port)
{
    uint8_t v;
    __asm__ volatile ("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

void uart_init(void)
{
    uart_outb(UART_BASE + 1, 0x00);  /* Disable interrupts               */
    uart_outb(UART_BASE + 3, 0x80);  /* Enable DLAB                      */
    uart_outb(UART_BASE + 0, 0x03);  /* 38400 baud (divisor lo)          */
    uart_outb(UART_BASE + 1, 0x00);  /* divisor hi                       */
    uart_outb(UART_BASE + 3, 0x03);  /* 8N1                              */
    uart_outb(UART_BASE + 2, 0xC7);  /* FIFO enable, clear, 14-byte thr. */
    uart_outb(UART_BASE + 4, 0x0B);  /* RTS/DSR                          */
}

void uart_putchar(char ch)
{
    while ((uart_inb(UART_BASE + 5) & 0x20) == 0) {}
    uart_outb(UART_BASE, (uint8_t)ch);
    if (ch == '\n') uart_putchar('\r');
}

void uart_puts(const char *s)
{
    while (*s) uart_putchar(*s++);
}

void uart_write(const char *s, size_t len)
{
    for (size_t i = 0; i < len; i++)
        uart_putchar(s[i]);
}
