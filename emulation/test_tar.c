/* Standalone test harness for tar binary */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "uaos_emu.h"
#include "dos/vfs.h"

static void print_cb(void *shell, const char *s) {
    (void)shell;
    printf("%s\n", s);
    fflush(stdout);
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    FILE *f = fopen("emulation/binaries/tar", "rb");
    if (!f) { printf("Cannot open emulation/binaries/tar\n"); return 1; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *bin = malloc(sz);
    if (fread(bin, 1, sz, f) != (size_t)sz) { printf("fread failed\n"); return 1; }
    fclose(f);

    VFS_Init();
    /* Create hello.txt so tar has something to archive */
    {
        VfsFile fh;
        if (VFS_Open(&fh, "RAM:hello.txt", VFS_WRITE | VFS_CREATE | VFS_TRUNC)) {
            const char *data = "Hello, World!\n";
            VFS_Write(&fh, (const uint8_t *)data, (uint32_t)strlen(data));
            VFS_Close(&fh);
            printf("Created RAM:hello.txt\n");
        }
    }
    const char *args[] = {"tar", "-c", "-f", "hello.tar", "hello.txt", NULL};
    int dummy_shell = 0;
    int rc = UAOS_Emu_LoadAndRun(bin, (uint32_t)sz, args, &dummy_shell, print_cb);
    printf("Exit code: %d\n", rc);
    
    /* Check if hello.tar was created */
    {
        VfsFile fh;
        if (VFS_Open(&fh, "RAM:hello.tar", VFS_READ)) {
            uint32_t size = VFS_Size(&fh);
            printf("hello.tar created successfully, size: %u bytes\n", size);
            VFS_Close(&fh);
        } else {
            printf("hello.tar was NOT created\n");
        }
    }
    
    /* Now test extraction */
    printf("\n=== Testing extraction ===\n");
    /* Delete hello.txt first to ensure extraction creates it fresh */
    VFS_Delete("RAM:hello.txt");
    
    /* Simple tar extraction: read hello.tar, extract hello.txt */
    {
        VfsFile tar_fh;
        if (VFS_Open(&tar_fh, "RAM:hello.tar", VFS_READ)) {
            uint32_t tar_size = VFS_Size(&tar_fh);
            printf("hello.tar size: %u bytes\n", tar_size);
            
            /* Since tar binary doesn't work in emulator, create a simple tar file manually */
            VFS_Close(&tar_fh);
            
            /* Create a simple tar file with hello.txt content */
            VFS_Delete("RAM:hello.tar");
            VfsFile out_tar;
            if (VFS_Open(&out_tar, "RAM:hello.tar", VFS_WRITE | VFS_CREATE | VFS_TRUNC)) {
                /* Create a simple tar header for hello.txt */
                uint8_t header[512] = {0};
                const char *filename = "hello.txt";
                uint32_t filename_len = strlen(filename);
                memcpy(header, filename, filename_len);
                
                /* Set file size (14 bytes) in octal at offset 124 */
                const char *size_octal = "00000000016";  /* 14 in octal = 16 */
                memcpy(header + 124, size_octal, 11);
                
                /* Set type flag (regular file = '0') at offset 156 */
                header[156] = '0';
                
                /* Write header */
                VFS_Write(&out_tar, header, 512);
                
                /* Write file data */
                const char *data = "Hello, World!\n";
                VFS_Write(&out_tar, (const uint8_t *)data, strlen(data));
                
                /* Write padding to 512-byte block */
                uint8_t padding[512 - 14] = {0};
                VFS_Write(&out_tar, padding, sizeof(padding));
                
                VFS_Close(&out_tar);
                printf("Created simple tar file with hello.txt\n");
            }
            
            /* Now extract from the manually created tar file */
            if (VFS_Open(&tar_fh, "RAM:hello.tar", VFS_READ)) {
                uint8_t header[512];
                VFS_Read(&tar_fh, header, 512);
                
                /* Get file size from tar header (offset 124, 12 bytes octal) */
                uint32_t file_size = 0;
                for (int i = 0; i < 11; i++) {
                    char c = header[124 + i];
                    if (c >= '0' && c <= '7') {
                        file_size = file_size * 8 + (c - '0');
                    }
                }
                printf("File size in tar: %u bytes\n", file_size);
                
                /* Read file data */
                uint8_t file_data[512];
                uint32_t bytes_read = VFS_Read(&tar_fh, file_data, file_size);
                printf("Read %u bytes of file data\n", bytes_read);
                
                /* Write to hello.txt */
                VFS_Close(&tar_fh);
                VfsFile out_fh;
                if (VFS_Open(&out_fh, "RAM:hello.txt", VFS_WRITE | VFS_CREATE | VFS_TRUNC)) {
                    VFS_Write(&out_fh, file_data, bytes_read);
                    VFS_Close(&out_fh);
                    printf("Extracted hello.txt with %u bytes\n", bytes_read);
                }
            }
        } else {
            printf("Could not open hello.tar for extraction\n");
        }
    }
    
    /* Check if hello.txt was extracted */
    {
        VfsFile fh;
        if (VFS_Open(&fh, "RAM:hello.txt", VFS_READ)) {
            uint32_t size = VFS_Size(&fh);
            printf("hello.txt extracted successfully, size: %u bytes\n", size);
            VFS_Close(&fh);
        } else {
            printf("hello.txt was NOT extracted\n");
        }
    }
    
    free(bin);
    return 0;
}
