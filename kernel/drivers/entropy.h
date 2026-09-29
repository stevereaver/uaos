/*
 * entropy.h — kernel entropy source for getrandom(2)-style syscall
 *
 * Feeds BearSSL seeding in userspace.  Prefers hardware RDRAND/RDSEED
 * when CPUID reports them; otherwise falls back to an RDTSC-jitter
 * pool (weak — documented limitation; QEMU should be run with
 * -cpu qemu64,+rdrand or better for real entropy).
 */
#ifndef UAOS_KERNEL_DRIVERS_ENTROPY_H
#define UAOS_KERNEL_DRIVERS_ENTROPY_H

#include <stdint.h>

/* Detect CPU entropy features (RDRAND/RDSEED).  Called once at boot. */
void     entropy_init(void);

/* 1 if a hardware RNG instruction is available, else 0. */
int      entropy_hw_available(void);

/* Fill buf with len bytes of entropy.  Returns number of bytes
 * written (always len unless buf is NULL / len == 0). */
uint32_t entropy_fill(void *buf, uint32_t len);

#endif
