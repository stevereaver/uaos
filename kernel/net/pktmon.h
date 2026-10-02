/* pktmon.h — in-guest pcap packet capture (UAOS-214) */

#ifndef UAOS_PKTMON_H
#define UAOS_PKTMON_H

#include <stdint.h>

int  Pktmon_Start(const char *path, uint32_t max_packets);
void Pktmon_Stop(void);
void Pktmon_Record(int dir, const uint8_t *frame, uint16_t len); /* 0=rx 1=tx */
void Pktmon_Status(int *out_open, uint32_t *out_pkts,
                   uint32_t *out_dropped, const char **out_path);

#endif
