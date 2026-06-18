/* port_handler.h — PORT: parallel port handler */

#ifndef UAOS_PORT_HANDLER_H
#define UAOS_PORT_HANDLER_H

#include "dos/handler.h"

/* Create and return the PORT: handler. */
Handler *PortHandler_Create(const char *name);

#endif
