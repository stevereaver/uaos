/* floppy_blk.h */
#ifndef UAOS_FLOPPY_BLK_H
#define UAOS_FLOPPY_BLK_H
#include <stdint.h>
#include <stddef.h>
int  FloppyBlockDev_Init(void);
int  FloppyBlk_Attach(int unit);
void FloppyBlk_Detach(int unit);
void FloppyBlk_DiagDump(void *ctx, void (*emit)(void *, const char *));
int  floppy_block_device_write_test(void);
int  floppy_multi_unit_test(void);
#endif
