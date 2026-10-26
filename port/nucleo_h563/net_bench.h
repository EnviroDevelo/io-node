/* net_bench.h - the bench board on Ethernet: lwIP (NO_SYS, raw API) under node_net.c, from the main
 * loop. Modbus TCP on IOB_TCP_PORT, discovery on IOB_DISC_PORT (node_net.h has the rules). The IP
 * settings live in the TAMP backup registers like the address (kept over a reset, lost at power-off). */
#ifndef NET_BENCH_H
#define NET_BENCH_H
#include <stdint.h>
#include "node.h"

void net_bench_init(node_t *node, const uint16_t uid[6]);   /* after node_init */
void net_bench_poll(uint32_t now_ms);                        /* every main loop pass */

#endif
