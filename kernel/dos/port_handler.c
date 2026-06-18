/* port_handler.c — PORT: parallel port device handler
 *
 * Built-in native device handler backed by the generic DeviceHandler.
 */

#include "port_handler.h"
#include "device_handler.h"

Handler *PortHandler_Create(const char *name)
{
    return (Handler *)DeviceHandler_Create(name ? name : "port-handler", DEV_TYPE_PAR);
}
