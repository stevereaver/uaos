/* aux_handler.h — AUX: device handler */

#ifndef UAOS_AUX_HANDLER_H
#define UAOS_AUX_HANDLER_H

#include "dos/handler.h"

/* Create and return the AUX: handler. */
Handler *AuxHandler_Create(const char *name);

#endif
