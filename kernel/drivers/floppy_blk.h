/* floppy_blk.h */
#ifndef UAOS_FLOPPY_BLK_H
#define UAOS_FLOPPY_BLK_H
#include <stdint.h>
#include <stddef.h>
int FloppyBlockDev_Init(void);
int floppy_block_device_write_test(void);
#endif
