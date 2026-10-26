/* node_net.c - see node_net.h */
#include <string.h>
#include "node_net.h"

void node_net_init(node_net_t *nn, node_t *node, const node_ipcfg_t *saved, const uint8_t mac[6],
                   int (*store)(void *ctx, const node_ipcfg_t *c), void *ctx) {
    memset(nn, 0, sizeof *nn);
    nn->node = node;
    if (saved) nn->cfg = *saved;
    memcpy(nn->mac, mac, 6);
    nn->store = store;
    nn->ctx = ctx;
    /* settings that do not hang together (a half-written store) count as none */
    if (nn->cfg.ip && !iob_net_settings_ok(nn->cfg.ip, nn->cfg.mask, nn->cfg.gw, nn->cfg.master_ip, node->address))
        memset(&nn->cfg, 0, sizeof nn->cfg);
}

bool node_net_serving(const node_net_t *nn) { return nn->cfg.ip != 0 && nn->node->address != 0; }

bool node_net_accept(node_net_t *nn, uint32_t from_ip) {
    if (!node_net_serving(nn) || (nn->cfg.master_ip && from_ip != nn->cfg.master_ip)) { nn->refused++; return false; }
    nn->have = 0;
    return true;
}

int node_net_tcp(node_net_t *nn, uint32_t now, const uint8_t *data, uint16_t len, uint8_t *tx, uint16_t cap) {
    if (!node_net_serving(nn) || len > sizeof nn->rx - nn->have) return -1;
    memcpy(nn->rx + nn->have, data, len);
    nn->have = (uint16_t)(nn->have + len);
    int out = 0, c;
    while ((c = iob_mbap_complete(nn->rx, nn->have)) > 0) {
        uint8_t rtu[IOB_MBAP_MAX], rep[NODE_FRAME_MAX];
        uint16_t tid;
        size_t rl = iob_rtu_from_mbap(nn->rx, (size_t)c, &tid, rtu);
        memmove(nn->rx, nn->rx + c, nn->have - (size_t)c);
        nn->have = (uint16_t)(nn->have - c);
        if (rl > NODE_FRAME_MAX) return -1;
        nn->tcp_frames++;
        uint16_t r = node_frame(nn->node, NODE_PORT_RS485, now, rtu, (uint16_t)rl, false, rep);
        if (!r) continue;                           /* another unit id: no answer, as on RS-485 */
        if ((size_t)out + r - 2u + 6u > cap) return -1;
        out += (int)iob_mbap_from_rtu(rep, r, tid, tx + out);
    }
    return c < 0 ? -1 : out;
}

static uint16_t here(const node_net_t *nn, uint32_t now, uint16_t seq, uint8_t *reply) {
    const node_t *n = nn->node;
    iob_disc_t h;
    memset(&h, 0, sizeof h);
    h.op = IOB_DISC_HERE;
    h.seq = seq;
    memcpy(h.uid, n->uid, sizeof h.uid);
    memcpy(h.mac, nn->mac, 6);
    for (unsigned k = 0; k < 16 && n->serial[k]; ++k) h.serial[k] = (char)n->serial[k];
    h.fw_version = n->fw_version; h.fw_build = n->fw_build; h.hw_rev = n->hw_rev;
    h.flags = (uint16_t)((nn->cfg.ip ? 0u : IOB_DISC_F_NEW) | (node_comms_ok(n, now) ? IOB_DISC_F_CONTROLLED : 0u));
    h.ip = nn->cfg.ip; h.mask = nn->cfg.mask; h.gw = nn->cfg.gw; h.master_ip = nn->cfg.master_ip;
    h.address = n->address;
    return (uint16_t)iob_disc_encode(&h, reply);
}

uint16_t node_net_disc(node_net_t *nn, uint32_t now, const uint8_t *d, uint16_t len, uint32_t from_ip,
                       uint8_t reply[IOB_DISC_LEN], bool *changed) {
    iob_disc_t m;
    *changed = false;
    if (!iob_disc_decode(d, len, &m)) return 0;
    nn->disc++;
    if (m.op == IOB_DISC_FIND) return here(nn, now, m.seq, reply);
    if (!iob_disc_set_accept(&m, nn->node->uid, nn->cfg.master_ip, from_ip)) return 0;
    node_t *n = nn->node;
    node_ipcfg_t c = { m.ip, m.mask, m.gw, m.master_ip }, old = nn->cfg;
    if (nn->store(nn->ctx, &c) == NODE_HW_OK) {
        if (m.address == n->address || n->hw->addr_store(n->hw->ctx, m.address) == NODE_HW_OK) {
            n->address = m.address;
            nn->cfg = c;
            nn->sets++;
            *changed = c.ip != old.ip || c.mask != old.mask || c.gw != old.gw;
        } else (void)nn->store(nn->ctx, &old);      /* both or neither */
    }
    return here(nn, now, m.seq, reply);             /* what is saved now: the supervisor compares */
}

bool node_net_poll(node_net_t *nn) {
    if (nn->cfg.ip == 0 || nn->node->address != 0) return false;
    node_ipcfg_t none = { 0, 0, 0, 0 };
    if (nn->store(nn->ctx, &none) != NODE_HW_OK) return false;
    nn->cfg = none;
    return true;
}
