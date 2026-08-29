/* print_handler.h — PRT: parallel port print device handler */

#ifndef UAOS_PRINT_HANDLER_H
#define UAOS_PRINT_HANDLER_H

#include "dos/handler.h"

/* Create and return the PRT: print handler.
 * Writes are sent to the host LPT1 parallel port (0x378).
 * If no physical LPT1 is present, data is buffered in a ring. */
Handler *PrintHandler_Create(const char *name);

/* Send a single byte to LPT1. Returns 1 on success, 0 if no port. */
int PrintHandler_SendByte(uint8_t c);

/* Check if LPT1 is present. */
int PrintHandler_IsPresent(void);

/* Flush pending output to LPT1. */
void PrintHandler_Flush(void);

#endif
