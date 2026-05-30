/*
 * virtio_blk.h — UAOS VirtIO Block Device Driver Header
 */

#ifndef UAOS_VIRTIO_BLK_H
#define UAOS_VIRTIO_BLK_H

/* Initialize VirtIO block device driver */
int virtio_blk_init(void);

/* Read sectors from VirtIO block device */
int virtio_blk_read(uint64_t sector, void *buffer, uint32_t num_sectors);

/* Write sectors to VirtIO block device */
int virtio_blk_write(uint64_t sector, const void *buffer, uint32_t num_sectors);

/* Get device capacity in sectors */
uint64_t virtio_blk_get_capacity(void);

#endif /* UAOS_VIRTIO_BLK_H */
