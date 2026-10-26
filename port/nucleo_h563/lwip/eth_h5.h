/* eth_h5.h - STM32H5 Ethernet driver for lwIP without an OS (see eth_h5.c). */
#ifndef ETH_H5_H
#define ETH_H5_H
#include <stdint.h>
#include "lwip/err.h"
#include "lwip/netif.h"
void  eth_h5_set_mac(const uint8_t mac[6]);   /* before netif_add */
err_t eth_h5_init(struct netif *netif);       /* netif_add's init function */
void  eth_h5_poll(struct netif *netif);       /* every main loop pass: received frames into lwIP */
void  eth_h5_link_poll(struct netif *netif);  /* every few hundred ms: cable in / out */
#endif
