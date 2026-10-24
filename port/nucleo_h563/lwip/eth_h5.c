/**
  ******************************************************************************
  * @file    eth_h5.c
  * @brief   Ethernet network interface driver for lwIP (STM32H5, zero-copy, NO_SYS).
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2024 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component (see LICENSE_ST_SLA0044.md
  * next to this file). If no LICENSE file comes with this software, it is
  * provided AS-IS.
  *
  * Derived from STM32H5 LwIP_UDP_Echo_Server / the CEMS platform's
  * port/stm32h5/lwip/ethernetif.c. Modified for the CEMS I/O board (bench):
  *  - no OS: received frames are polled from the main loop (eth_h5_poll), no
  *    interrupts, transmit is blocking with a short timeout;
  *  - MAC address passed in (from the chip's unique ID), not a constant;
  *  - HAL_ETH_MspInit (board pins) left to CubeMX.
  ******************************************************************************
  */
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include "stm32h5xx_hal.h"
#include "lwip/opt.h"
#include "lwip/pbuf.h"
#include "lwip/memp.h"
#include "netif/ethernet.h"
#include "netif/etharp.h"
#include "eth_h5.h"
#include "lan8742.h"

#define ETH_TX_TIMEOUT_MS     20U
#define ETH_RX_BUFFER_SIZE    1536U       /* must be the same as heth.Init.RxBuffLen */
#define ETH_RX_BUFFER_CNT     8U          /* must be greater than ETH_RX_DESC_CNT */

typedef struct {
    struct pbuf_custom pbuf_custom;
    uint8_t buff[(ETH_RX_BUFFER_SIZE + 31) & ~31] __ALIGNED(32);
} RxBuff_t;

static ETH_DMADescTypeDef DMARxDscrTab[ETH_RX_DESC_CNT];
static ETH_DMADescTypeDef DMATxDscrTab[ETH_TX_DESC_CNT];
LWIP_MEMPOOL_DECLARE(RX_POOL, ETH_RX_BUFFER_CNT, sizeof(RxBuff_t), "Zero-copy RX PBUF pool");

static bool rx_alloc_failed;
static uint8_t mac[6];
static ETH_HandleTypeDef heth;

static int32_t phy_io_init(void) { HAL_ETH_SetMDIOClockRange(&heth); return 0; }
static int32_t phy_io_deinit(void) { return 0; }
static int32_t phy_io_read(uint32_t dev, uint32_t reg, uint32_t *v) { return HAL_ETH_ReadPHYRegister(&heth, dev, reg, v) == HAL_OK ? 0 : -1; }
static int32_t phy_io_write(uint32_t dev, uint32_t reg, uint32_t v) { return HAL_ETH_WritePHYRegister(&heth, dev, reg, v) == HAL_OK ? 0 : -1; }
static int32_t phy_io_tick(void) { return (int32_t)HAL_GetTick(); }
static lan8742_Object_t phy;
static lan8742_IOCtx_t  phy_io = { phy_io_init, phy_io_deinit, phy_io_write, phy_io_read, phy_io_tick };

static void pbuf_free_custom(struct pbuf *p);

/* ---------------- link: PHY state -> MAC config + netif ---------------- */
static bool apply_phy_state(int32_t st) {
    uint32_t duplex, speed;
    switch (st) {
    case LAN8742_STATUS_100MBITS_FULLDUPLEX: duplex = ETH_FULLDUPLEX_MODE; speed = ETH_SPEED_100M; break;
    case LAN8742_STATUS_100MBITS_HALFDUPLEX: duplex = ETH_HALFDUPLEX_MODE; speed = ETH_SPEED_100M; break;
    case LAN8742_STATUS_10MBITS_FULLDUPLEX:  duplex = ETH_FULLDUPLEX_MODE; speed = ETH_SPEED_10M;  break;
    case LAN8742_STATUS_10MBITS_HALFDUPLEX:  duplex = ETH_HALFDUPLEX_MODE; speed = ETH_SPEED_10M;  break;
    default: return false;
    }
    ETH_MACConfigTypeDef c;
    memset(&c, 0, sizeof c);
    HAL_ETH_GetMACConfig(&heth, &c);
    c.DuplexMode = duplex;
    c.Speed = speed;
    HAL_ETH_SetMACConfig(&heth, &c);
    HAL_ETH_Start(&heth);
    return true;
}

void eth_h5_link_poll(struct netif *netif) {
    int32_t st = LAN8742_GetLinkState(&phy);
    if (netif_is_link_up(netif) && st <= LAN8742_STATUS_LINK_DOWN) {
        HAL_ETH_Stop(&heth);
        netif_set_link_down(netif);
    } else if (!netif_is_link_up(netif) && st > LAN8742_STATUS_LINK_DOWN) {
        if (apply_phy_state(st)) netif_set_link_up(netif);
    }
}

/* ---------------- receive ---------------- */
void eth_h5_poll(struct netif *netif) {
    for (int k = 0; k < 8 && !rx_alloc_failed; ++k) {    /* a few frames per pass: the loop stays fast */
        struct pbuf *p = NULL;
        if (HAL_ETH_ReadData(&heth, (void **)&p) != HAL_OK || p == NULL) break;
        if (netif->input(p, netif) != ERR_OK) pbuf_free(p);
    }
}

/* ---------------- transmit ---------------- */
static err_t low_level_output(struct netif *netif, struct pbuf *p) {
    (void)netif;
    uint32_t i = 0U;
    ETH_BufferTypeDef tb[ETH_TX_DESC_CNT];
    ETH_TxPacketConfigTypeDef tc;
    memset(tb, 0, sizeof tb);
    memset(&tc, 0, sizeof tc);
    tc.Attributes = ETH_TX_PACKETS_FEATURES_CSUM | ETH_TX_PACKETS_FEATURES_CRCPAD;
    tc.ChecksumCtrl = ETH_CHECKSUM_IPHDR_PAYLOAD_INSERT_PHDR_CALC;
    tc.CRCPadCtrl = ETH_CRC_PAD_INSERT;
    for (struct pbuf *q = p; q != NULL; q = q->next) {
        if (i >= ETH_TX_DESC_CNT) return ERR_IF;
        tb[i].buffer = q->payload;
        tb[i].len = q->len;
        if (i > 0) tb[i - 1].next = &tb[i];
        tb[i].next = NULL;
        i++;
    }
    tc.Length = p->tot_len;
    tc.TxBuffer = tb;
    tc.pData = p;
    pbuf_ref(p);                                         /* freed by HAL_ETH_TxFreeCallback */
    HAL_StatusTypeDef st = HAL_ETH_Transmit(&heth, &tc, ETH_TX_TIMEOUT_MS);
    HAL_ETH_ReleaseTxPacket(&heth);
    if (st != HAL_OK) { pbuf_free(p); return ERR_IF; }
    return ERR_OK;
}

/* ---------------- init ---------------- */
void eth_h5_set_mac(const uint8_t m[6]) { memcpy(mac, m, 6); }

err_t eth_h5_init(struct netif *netif) {
    netif->name[0] = 'e';
    netif->name[1] = 'n';
    netif->output = etharp_output;
    netif->linkoutput = low_level_output;
    heth.Instance = ETH;
    heth.Init.MACAddr = mac;
    heth.Init.MediaInterface = HAL_ETH_RMII_MODE;
    heth.Init.RxDesc = DMARxDscrTab;
    heth.Init.TxDesc = DMATxDscrTab;
    heth.Init.RxBuffLen = ETH_RX_BUFFER_SIZE;
    HAL_ETH_Init(&heth);                                 /* pins and clocks: HAL_ETH_MspInit from CubeMX */
    netif->hwaddr_len = ETH_HWADDR_LEN;
    memcpy(netif->hwaddr, mac, 6);
    netif->mtu = ETH_MAX_PAYLOAD;
    netif->flags |= NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP;
    LWIP_MEMPOOL_INIT(RX_POOL);
    LAN8742_RegisterBusIO(&phy, &phy_io);
    if (LAN8742_Init(&phy) == LAN8742_STATUS_OK && apply_phy_state(LAN8742_GetLinkState(&phy))) netif_set_link_up(netif);
    else netif_set_link_down(netif);
    netif_set_up(netif);
    return ERR_OK;
}

/* ---------------- zero-copy RX buffers ---------------- */
static void pbuf_free_custom(struct pbuf *p) {
    LWIP_MEMPOOL_FREE(RX_POOL, (struct pbuf_custom *)p);
    rx_alloc_failed = false;
}

void HAL_ETH_RxAllocateCallback(uint8_t **buff) {
    struct pbuf_custom *p = LWIP_MEMPOOL_ALLOC(RX_POOL);
    if (p) {
        *buff = (uint8_t *)p + offsetof(RxBuff_t, buff);
        p->custom_free_function = pbuf_free_custom;
        pbuf_alloced_custom(PBUF_RAW, 0, PBUF_REF, p, *buff, ETH_RX_BUFFER_SIZE);
    } else {
        rx_alloc_failed = true;
        *buff = NULL;
    }
}

void HAL_ETH_RxLinkCallback(void **pStart, void **pEnd, uint8_t *buff, uint16_t Length) {
    struct pbuf **ppStart = (struct pbuf **)pStart, **ppEnd = (struct pbuf **)pEnd;
    struct pbuf *p = (struct pbuf *)(void *)(buff - offsetof(RxBuff_t, buff));
    p->next = NULL;
    p->tot_len = 0;
    p->len = Length;
    if (!*ppStart) *ppStart = p; else (*ppEnd)->next = p;
    *ppEnd = p;
    for (p = *ppStart; p != NULL; p = p->next) p->tot_len = (u16_t)(p->tot_len + Length);
}

void HAL_ETH_TxFreeCallback(uint32_t *buff) { pbuf_free((struct pbuf *)buff); }
