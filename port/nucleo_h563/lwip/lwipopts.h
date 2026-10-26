/* lwipopts.h - lwIP for the bench I/O board (NUCLEO-H563ZI): NO_SYS, raw API, from the node's main
 * loop. One Modbus TCP connection (the supervisor), one listening port, the discovery port. */
#ifndef LWIPOPTS_H
#define LWIPOPTS_H

#define NO_SYS                          1
#define SYS_LIGHTWEIGHT_PROT            0             /* lwIP is only ever called from the main loop */
#define LWIP_NETCONN                    0
#define LWIP_SOCKET                     0

/* ---- memory ---- */
#define MEM_ALIGNMENT                   4
#define MEM_SIZE                        (8 * 1024)
#define MEMP_NUM_PBUF                   8
#define PBUF_POOL_SIZE                  8
#define PBUF_POOL_BUFSIZE               1536
#define LWIP_SUPPORT_CUSTOM_PBUF        1             /* zero-copy RX in eth_h5.c */
#define MEMP_NUM_TCP_PCB                3             /* the supervisor's connection, one being replaced, spare */
#define MEMP_NUM_TCP_PCB_LISTEN         1
#define MEMP_NUM_UDP_PCB                2
#define MEMP_NUM_TCP_SEG                16

/* ---- protocols ---- */
#define LWIP_ARP                        1
#define LWIP_ETHERNET                   1
#define LWIP_IPV4                       1
#define LWIP_IPV6                       0
#define LWIP_ICMP                       1             /* ping */
#define LWIP_DHCP                       0             /* static: the supervisor gives the IP (discovery SET) */
#define LWIP_AUTOIP                     0
#define LWIP_ACD                        0
#define LWIP_UDP                        1
#define LWIP_TCP                        1
#define LWIP_DNS                        0
#define LWIP_IGMP                       0
#define TCP_MSS                         536
#define TCP_WND                         (2 * TCP_MSS)
#define TCP_SND_BUF                     (2 * TCP_MSS)
#define TCP_SND_QUEUELEN                8
#define TCP_LISTEN_BACKLOG              0
#define LWIP_TCP_KEEPALIVE              1
#define TCP_KEEPIDLE_DEFAULT            10000         /* a supervisor that vanished: its connection goes in ~25 s */
#define TCP_KEEPINTVL_DEFAULT           5000
#define TCP_KEEPCNT_DEFAULT             3
/* discovery answers before the board has an IP (a new board), as DHCP does */
#define LWIP_IP_ACCEPT_UDP_PORT(port)   ((port) == PP_NTOHS(50223u))   /* IOB_DISC_PORT */

/* ---- interface ---- */
#define LWIP_NETIF_LINK_CALLBACK        0
#define LWIP_NETIF_STATUS_CALLBACK      0
#define LWIP_SINGLE_NETIF               1

/* ---- checksums: the H5 Ethernet MAC computes them (eth_h5.c sets TX insertion) ---- */
#define CHECKSUM_GEN_IP                 0
#define CHECKSUM_GEN_UDP                0
#define CHECKSUM_GEN_TCP                0
#define CHECKSUM_GEN_ICMP               1
#define CHECKSUM_CHECK_IP               0
#define CHECKSUM_CHECK_UDP              0
#define CHECKSUM_CHECK_TCP              0
#define CHECKSUM_CHECK_ICMP             0

#define LWIP_STATS                      0
#define LWIP_DEBUG                      0

#endif
