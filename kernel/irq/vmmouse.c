/* vmmouse.c — VMware backdoor absolute mouse driver
 *
 * QEMU emulates the VMware mouse backdoor when -device vmmouse is added.
 * All communication uses port 0x5658 (VMWARE_MAGIC) with a specific
 * command protocol. This gives us absolute screen coordinates without
 * any PS/2 relative delta issues.
 *
 * Protocol:
 *   Write EAX=magic, EBX=cookie/data, ECX=command, EDX=port
 *   Read  EAX=magic, EBX=status/X,   ECX=Y,        EDX=buttons
 */

#include "vmmouse.h"
#include <stdint.h>

#define VMWARE_MAGIC   0x564D5868UL   /* "VMXh" */
#define VMWARE_PORT    0x5658
#define VMWARE_PORT_HB 0x5659

/* Commands */
#define VMCMD_GETVERSION    0x0A
#define VMCMD_ABSPOINTER_DATA    0x27
#define VMCMD_ABSPOINTER_STATUS  0x28
#define VMCMD_ABSPOINTER_COMMAND 0x29

/* Absolute pointer sub-commands */
#define VMMOUSE_ENABLE   0x45414552UL
#define VMMOUSE_DISABLE  0x000000F5UL
#define VMMOUSE_REQUEST_RELATIVE  0x4C455252UL
#define VMMOUSE_REQUEST_ABSOLUTE  0x53424152UL

/* Screen size for coordinate scaling */
extern unsigned int g_fb_width_irq;
extern unsigned int g_fb_height_irq;

/* =========================================================================
 * Low-level backdoor I/O
 * ========================================================================= */

typedef struct {
    uint32_t eax, ebx, ecx, edx;
} VMCmd;

static VMCmd vm_cmd(uint32_t cmd, uint32_t arg)
{
    VMCmd r;
    r.eax = VMWARE_MAGIC;
    r.ebx = arg;
    r.ecx = cmd;
    r.edx = VMWARE_PORT;
    __asm__ volatile (
        "inl %%dx, %%eax"
        : "+a"(r.eax), "+b"(r.ebx), "+c"(r.ecx), "+d"(r.edx)
        :
        : "memory"
    );
    return r;
}

/* =========================================================================
 * Public API
 * ========================================================================= */

static int vmmouse_active = 0;

int VMMouse_Detect(void)
{
    VMCmd r = vm_cmd(VMCMD_GETVERSION, 0xFFFFFFFFUL);
    return (r.eax == VMWARE_MAGIC);
}

void VMMouse_Init(void)
{
    if (!VMMouse_Detect()) {
        vmmouse_active = 0;
        return;
    }
    /* Enable absolute pointer */
    vm_cmd(VMCMD_ABSPOINTER_COMMAND, VMMOUSE_ENABLE);
    vm_cmd(VMCMD_ABSPOINTER_COMMAND, VMMOUSE_REQUEST_ABSOLUTE);
    vmmouse_active = 1;
}

int VMMouse_Poll(int *out_x, int *out_y, int *out_buttons)
{
    if (!vmmouse_active) return 0;

    /* Check how many packets are waiting */
    VMCmd status = vm_cmd(VMCMD_ABSPOINTER_STATUS, 0);
    if ((status.eax & 0xFFFF) == 0) return 0;   /* no data */

    /* Read one packet: 4 words — buttons, X, Y, Z */
    vm_cmd(VMCMD_ABSPOINTER_DATA, 4);

    VMCmd data;
    data.eax = VMWARE_MAGIC;
    data.ebx = 4;                  /* request 4 words */
    data.ecx = VMCMD_ABSPOINTER_DATA;
    data.edx = VMWARE_PORT;
    __asm__ volatile (
        "inl %%dx, %%eax"
        : "+a"(data.eax), "+b"(data.ebx), "+c"(data.ecx), "+d"(data.edx)
        :
        : "memory"
    );

    /* EAX=flags/buttons, EBX=X (0-65535), ECX=Y (0-65535), EDX=Z */
    uint32_t buttons = data.eax & 0x7;
    uint32_t abs_x   = data.ebx;
    uint32_t abs_y   = data.ecx;

    /* Scale 0-65535 → screen pixels */
    int W = (int)g_fb_width_irq;
    int H = (int)g_fb_height_irq;
    *out_x = (int)((abs_x * (uint32_t)W) >> 16);
    *out_y = (int)((abs_y * (uint32_t)H) >> 16);
    *out_buttons = (int)buttons;

    return 1;
}
