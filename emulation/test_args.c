/* test_args.c - Test command line argument parsing in emulator */

#include "uaos_emu.h"
#include "dos/vfs.h"
#include <stdio.h>
#include <stdint.h>

static void print_cb(void *shell, const char *s)
{
    (void)shell;
    printf("%s", s);
}

int main(void)
{
    VFS_Init();
    
    /* Get LHA binary */
    extern const uint8_t g_bin_Lha[];
    extern const uint32_t g_bin_Lha_size;
    
    printf("=== Testing LHA with '?' argument ===\n");
    const char *args1[] = {"lha", "?", NULL};
    int dummy_shell = 0;
    UAOS_Emu_SetCwd("RAM:");
    int rc = UAOS_Emu_LoadAndRun(g_bin_Lha, g_bin_Lha_size, args1, &dummy_shell, print_cb);
    printf("Exit code: %d\n", rc);
    
    return 0;
}
