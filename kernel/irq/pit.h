/* pit.h — 8253/8254 PIT channel-0 timebase
 *
 * The kernel programs the PIT once at boot; every timing consumer
 * (g_pit_ticks, Task_SleepTicks, network pacing, watchdog) derives from
 * the rate below.  Anything reporting the tick rate must use
 * UAOS_PIT_HZ — never a literal — so the report cannot drift from what
 * was actually programmed.
 */

#ifndef UAOS_PIT_H
#define UAOS_PIT_H

#include <stdint.h>

#define UAOS_PIT_HZ  100          /* channel-0 interrupt rate */

/* IDT handler — defined in kernel/boot/uaos_kernel_main.c */
void PIT_IRQHandler(uint64_t vector, uint64_t error_code);

#endif /* UAOS_PIT_H */
