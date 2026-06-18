/* aux_handler.c — AUX: serial/aux device handler
 *
 * Built-in native device handler backed by the generic DeviceHandler
 * ring-buffer implementation.  In future this can be wired to a real
 * UART or virtual serial port.
 */

#include "aux_handler.h"
#include "device_handler.h"

Handler *AuxHandler_Create(const char *name)
{
    return (Handler *)DeviceHandler_Create(name ? name : "aux-handler", DEV_TYPE_AUX);
}
