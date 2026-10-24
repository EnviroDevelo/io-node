/* net_bench.c - see net_bench.h */
#include <string.h>
#include "main.h"
#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/tcp.h"
#include "lwip/udp.h"
#include "lwip/timeouts.h"
#include "eth_h5.h"
#include "net_bench.h"
#include "node_net.h"

#define IP_MAGIC 0x1B0E0000u                 /* BKP10R = magic | 16-bit sum of BKP6R..BKP9R */

static node_net_t      nn;
static struct netif    nif;
static struct tcp_pcb *conn;
static struct udp_pcb *disc;
static uint32_t        link_ms;

uint32_t sys_now(void) { return HAL_GetTick(); }
uint32_t net_bench_rand(void) { return HAL_GetTick() * 2654435761u ^ ((const volatile uint32_t *)UID_BASE)[0]; }

/* ---------------- the IP settings in the backup registers (BKP6R..BKP10R) ---------------- */
static uint32_t sum16(const node_ipcfg_t *c) {
    uint32_t w[4] = { c->ip, c->mask, c->gw, c->master_ip }, s = 0;
    for (unsigned k = 0; k < 4; ++k) s += (w[k] >> 16) + (w[k] & 0xFFFFu);
    return s & 0xFFFFu;
}
static void load(node_ipcfg_t *c) {
    c->ip = TAMP->BKP6R; c->mask = TAMP->BKP7R; c->gw = TAMP->BKP8R; c->master_ip = TAMP->BKP9R;
    if (TAMP->BKP10R != (IP_MAGIC | sum16(c))) memset(c, 0, sizeof *c);   /* never set, or power-off */
}
static int store(void *ctx, const node_ipcfg_t *c) {
    (void)ctx;
    PWR->DBPCR |= PWR_DBPCR_DBP;
    TAMP->BKP6R = c->ip; TAMP->BKP7R = c->mask; TAMP->BKP8R = c->gw; TAMP->BKP9R = c->master_ip;
    TAMP->BKP10R = IP_MAGIC | sum16(c);
    node_ipcfg_t back;
    load(&back);
    return memcmp(&back, c, sizeof back) == 0 ? NODE_HW_OK : NODE_HW_NO_ANSWER;
}

/* ---------------- the netif follows the saved settings ---------------- */
static void apply(void) {
    ip4_addr_t ip, mask, gw;
    ip4_addr_set_u32(&ip, lwip_htonl(nn.cfg.ip));
    ip4_addr_set_u32(&mask, lwip_htonl(nn.cfg.mask));
    ip4_addr_set_u32(&gw, lwip_htonl(nn.cfg.gw));
    if (conn) { tcp_abort(conn); conn = NULL; }       /* the board changes over to its new IP */
    netif_set_addr(&nif, &ip, &mask, &gw);
}

/* ---------------- Modbus TCP ---------------- */
static void conn_err(void *arg, err_t err) { (void)arg; (void)err; conn = NULL; }

static err_t conn_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
    (void)arg; (void)err;
    if (p == NULL) {                                   /* the supervisor closed it */
        if (pcb == conn) conn = NULL;
        tcp_close(pcb);
        return ERR_OK;
    }
    static uint8_t tx[2u * IOB_MBAP_MAX];
    bool bad = false;
    for (struct pbuf *q = p; q != NULL && !bad; q = q->next) {
        int out = node_net_tcp(&nn, HAL_GetTick(), (const uint8_t *)q->payload, q->len, tx, sizeof tx);
        if (out < 0) bad = true;
        else if (out > 0) bad = tcp_write(pcb, tx, (u16_t)out, TCP_WRITE_FLAG_COPY) != ERR_OK;
    }
    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);
    if (bad) {
        if (pcb == conn) conn = NULL;
        tcp_abort(pcb);
        return ERR_ABRT;
    }
    tcp_output(pcb);
    return ERR_OK;
}

static err_t on_accept(void *arg, struct tcp_pcb *pcb, err_t err) {
    (void)arg;
    if (err != ERR_OK || pcb == NULL) return ERR_VAL;
    if (!node_net_accept(&nn, lwip_ntohl(ip4_addr_get_u32(ip_2_ip4(&pcb->remote_ip))))) { tcp_abort(pcb); return ERR_ABRT; }
    if (conn) tcp_abort(conn);                         /* one at a time: the new one wins */
    conn = pcb;
    tcp_nagle_disable(pcb);
    ip_set_option(pcb, SOF_KEEPALIVE);
    tcp_recv(pcb, conn_recv);
    tcp_err(pcb, conn_err);
    return ERR_OK;
}

/* ---------------- discovery ---------------- */
static void disc_recv(void *arg, struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *addr, u16_t port) {
    (void)arg;
    uint8_t d[IOB_DISC_LEN + 1], r[IOB_DISC_LEN];
    u16_t len = pbuf_copy_partial(p, d, sizeof d, 0);
    pbuf_free(p);
    bool changed = false;
    uint16_t rl = node_net_disc(&nn, HAL_GetTick(), d, len, lwip_ntohl(ip4_addr_get_u32(ip_2_ip4(addr))), r, &changed);
    if (rl) {
        struct pbuf *o = pbuf_alloc(PBUF_TRANSPORT, rl, PBUF_RAM);
        if (o) {
            pbuf_take(o, r, rl);
            udp_sendto_if_src(pcb, o, IP_ADDR_BROADCAST, port, &nif, &nif.ip_addr);   /* any subnet hears it */
            pbuf_free(o);
        }
    }
    if (changed) apply();
}

void net_bench_init(node_t *node, const uint16_t uid[6]) {
    node_ipcfg_t c;
    PWR->DBPCR |= PWR_DBPCR_DBP;
    load(&c);
    uint8_t mac[6];
    iob_mac_from_uid(uid, mac);
    node_net_init(&nn, node, &c, mac, store, NULL);
    lwip_init();
    eth_h5_set_mac(mac);
    netif_add(&nif, IP4_ADDR_ANY4, IP4_ADDR_ANY4, IP4_ADDR_ANY4, NULL, eth_h5_init, netif_input);
    netif_set_default(&nif);
    apply();
    struct tcp_pcb *l = tcp_new();
    if (l && tcp_bind(l, IP_ADDR_ANY, IOB_TCP_PORT) == ERR_OK && (l = tcp_listen(l)) != NULL) tcp_accept(l, on_accept);
    disc = udp_new();
    if (disc) {
        ip_set_option(disc, SOF_BROADCAST);
        udp_bind(disc, IP_ADDR_ANY, IOB_DISC_PORT);
        udp_recv(disc, disc_recv, NULL);
    }
}

void net_bench_poll(uint32_t now) {
    eth_h5_poll(&nif);
    sys_check_timeouts();
    if (node_net_poll(&nn)) apply();                   /* address cleared over USB: a new board again */
    if (now - link_ms >= 250u) { link_ms = now; eth_h5_link_poll(&nif); }
}
