/* iob_node_real.c - a drop-in replacement for the platform's iob_node_sim.c, with the same API
 * (iob_node_sim.h), backed by the real node firmware core (node/node.c) and simulated chips
 * (tests/sim_hw.c). Linked into the platform's own host test suite by `make master-test`, so
 * the master is tested against the node firmware itself, not against a model of it.
 *
 * Each iob_node_t the tests use is shadowed by a node_t. Before every call the tests' "outside
 * world" (inputs, faults, supplies, identity) is pushed into the shadow; afterwards the node's
 * registers and state are copied back into the iob_node_t fields the tests read. */
#include <string.h>
#include "bsp.h"
#include "iob_node_sim.h"
#include "mb_rtu.h"
#include "node.h"
#include "node_net.h"
#include "sim_hw.h"

typedef struct { iob_node_t *key; node_t me; sim_hw_t hw; node_net_t net; } shadow_t;
static shadow_t sh[16];

static shadow_t *find(const iob_node_t *n)
{
    for (unsigned i = 0; i < sizeof sh / sizeof sh[0]; i++) if (sh[i].key == n) return &sh[i];
    for (unsigned i = 0; i < sizeof sh / sizeof sh[0]; i++)
        if (!sh[i].key) { sh[i].key = (iob_node_t *)n; return &sh[i]; }
    return &sh[0];                                   /* more than 16 nodes: never in the tests */
}

/* the tests' world -> the simulated chips and the node's identity */
static void push(iob_node_t *n, shadow_t *s)
{
    sim_hw_t *h = &s->hw;
    h->inputs = n->inputs;
    h->a_open_raw = n->a_open;
    h->a_overload = n->a_overload;
    h->a_overtemp = n->a_overtemp;
    h->wirebreak_raw = n->wirebreak;
    h->field_a = n->field_a;
    h->field_b = n->field_b;
    h->b_fault_pin = (n->b_fault & IOB_BF_FAULT) != 0;
    h->b_wdg_tripped = (n->b_fault & IOB_BF_WDG) != 0;
    h->in_internal_fault = n->in_chip_fault;
    h->a_internal_fault = n->out_a_chip_fault;
    node_t *m = &s->me;
    m->address = n->address;
    memcpy(m->serial, n->serial, sizeof m->serial);
    m->magic = n->magic; m->map_version = n->map_version;
    m->n_out_a = n->n_out_a; m->n_out_b = n->n_out_b; m->n_relays = n->n_relays; m->n_inputs = n->n_inputs;
    m->fw_version = n->fw_version; m->hw_rev = n->hw_rev; m->fw_build = 1;
    memcpy(m->uid, n->uid, sizeof m->uid);
    s->net.cfg = (node_ipcfg_t){ n->ip, n->mask, n->gw, n->master_ip };
    memcpy(s->net.mac, n->mac, 6);
}

/* the node's registers and state -> the fields the tests read */
static void pull(iob_node_t *n, const shadow_t *s)
{
    const node_t *m = &s->me;
    n->out_cmd = m->out_cmd; n->relay_cmd = m->relay_cmd; n->seq = m->seq; n->echo = m->echo;
    n->hold_ds = m->hold_ds; n->filter_ms = m->filter_ms; n->wb_en = m->wb_en; n->ol_en = m->ol_en;
    n->config_needed = m->config_needed; n->timeout_latched = m->timeout_latched; n->wdg_latched = m->wdg_latched;
    n->gate = node_healthy(m);
    n->last_write_ms = m->last_write_ms;
    n->comms_seen = m->comms_seen; n->comms_lost = m->comms_lost; n->last_comms_ms = m->last_comms_ms;
    n->in_rep = m->in_rep; n->in_pend = m->in_pend;
    n->address = m->address;                         /* an address change may have moved it */
    n->rx_errors = m->rx_errors; n->resets = m->resets;
    n->frames = m->frames; n->replies = m->replies; n->writes0 = m->writes0;
    n->ip = s->net.cfg.ip; n->mask = s->net.cfg.mask; n->gw = s->net.cfg.gw; n->master_ip = s->net.cfg.master_ip;
}

static int net_store(void *ctx, const node_ipcfg_t *c) { (void)ctx; (void)c; return NODE_HW_OK; }

/* a power-up or a restart of the board: the chips settle as the world says */
static void boot(iob_node_t *n, shadow_t *s, node_reset_t cause, uint16_t resets, uint32_t now)
{
    sim_hw_init(&s->hw);
    node_boot_t b = { .address = n->address, .nv_ok = true, .hw_rev = (uint8_t)n->hw_rev, .reset_cause = cause, .resets = resets };
    memcpy(b.uid, n->uid, sizeof b.uid);
    push(n, s);
    node_init(&s->me, &s->hw.hw, &b, now);
    node_net_init(&s->net, &s->me, NULL, n->mac, net_store, NULL);
    push(n, s);
    node_poll(&s->me, now + 1u);
    node_poll(&s->me, now + 2u);                     /* the bank B fault pin's 1 ms debounce */
}

void iob_node_init(iob_node_t *n, uint8_t address)
{
    memset(n, 0, sizeof *n);
    n->address = address;
    n->powered = true;
    n->magic = IOB_MAGIC; n->map_version = IOB_MAP_VERSION;
    n->n_out_a = 8; n->n_out_b = 8; n->n_relays = 2; n->n_inputs = 8;
    n->fw_version = 0x0100; n->hw_rev = 1;
    for (unsigned i = 0; i < 6; ++i) n->uid[i] = (uint16_t)(0x1000u * address + i);
    n->field_a = n->field_b = true;
    if (address >= 1 && address <= 3) { n->ip = 0xC0A8013Cu + address; n->mask = 0xFFFFFF00u; }
    iob_mac_from_uid(n->uid, n->mac);
    shadow_t *s = find(n);
    boot(n, s, NODE_RST_POWER_ON, 0, 0);
    pull(n, s);
}

void iob_node_restart(iob_node_t *n, bool wdg, uint32_t now)
{
    shadow_t *s = find(n);
    boot(n, s, wdg ? NODE_RST_IWDG : NODE_RST_SOFTWARE, (uint16_t)(n->resets + 1u), now);
    pull(n, s);
}

void iob_node_tick(iob_node_t *n, uint32_t now)
{
    shadow_t *s = find(n);
    push(n, s);
    node_poll(&s->me, now);
    pull(n, s);
}

uint16_t iob_node_reg(const iob_node_t *n, uint16_t r)
{
    shadow_t *s = find(n);
    push((iob_node_t *)n, s);
    return node_reg(&s->me, r);
}

uint16_t iob_node_frame(iob_node_t *n, uint32_t now, const uint8_t *rx, uint16_t len, uint8_t *tx, uint16_t cap)
{
    if (!n->powered) return 0;
    shadow_t *s = find(n);
    push(n, s);
    node_poll(&s->me, now);
    static uint8_t buf[NODE_FRAME_MAX];
    uint16_t r = node_frame(&s->me, NODE_PORT_RS485, now, rx, len, false, buf);
    if (r > cap) r = 0;
    memcpy(tx, buf, r);
    pull(n, s);
    return r;
}

/* ---------------- the bus (as in iob_node_sim.c) ---------------- */
static uint16_t bus_xfer(iob_port_t *p, const uint8_t *tx, uint16_t n, uint8_t *rx, uint16_t cap, uint32_t timeout)
{
    (void)timeout;
    iob_simbus_t *b = (iob_simbus_t *)p->ctx;
    b->requests++;
    uint32_t now = b->now ? b->now() : bsp_ms();
    uint16_t got = 0;
    for (unsigned i = 0; i < b->n; ++i) {
        uint16_t r = iob_node_frame(b->nodes[i], now, tx, n, rx, cap);
        if (r) got = r;
    }
    if (b->drop_next) { b->drop_next--; return 0; }
    if (got && b->corrupt_next) { b->corrupt_next--; rx[got - 1] ^= 0x5Au; }
    return got;
}

void iob_simbus_init(iob_simbus_t *b)
{
    memset(b, 0, sizeof *b);
    b->port.xfer = bus_xfer;
    b->port.ctx = b;
}

void iob_simbus_add(iob_simbus_t *b, iob_node_t *n)
{
    if (b->n < 4) b->nodes[b->n++] = n;
}

/* ---------------- the network (as in iob_node_sim.c), through the node's own node_net.c ---------------- */
static uint16_t net_xfer(iob_port_t *p, const uint8_t *tx, uint16_t n, uint8_t *rx, uint16_t cap, uint32_t timeout)
{
    (void)timeout;
    iob_simnet_t *s = (iob_simnet_t *)p->ctx;
    s->requests++;
    uint32_t now = s->now ? s->now() : bsp_ms();
    uint8_t m[IOB_MBAP_MAX], rep[2u * IOB_MBAP_MAX], out[IOB_MBAP_MAX];
    size_t ml = iob_mbap_from_rtu(tx, n, (uint16_t)s->requests, m);
    if (!ml) return 0;
    for (unsigned i = 0; i < s->n; ++i) {
        iob_node_t *nd = s->nodes[i];
        if (!nd->powered || nd->ip == 0 || nd->ip != s->target_ip) continue;
        shadow_t *x = find(nd);
        push(nd, x);
        node_poll(&x->me, now);
        int r = node_net_accept(&x->net, s->master_ip) ? node_net_tcp(&x->net, now, m, (uint16_t)ml, rep, sizeof rep) : -1;
        pull(nd, x);
        if (r <= 0 || s->drop_next) { if (s->drop_next) s->drop_next--; return 0; }
        size_t ol = iob_rtu_from_mbap(rep, (size_t)r, NULL, out);
        if (!ol || ol > cap) return 0;
        memcpy(rx, out, ol);
        return (uint16_t)ol;
    }
    return 0;
}

static void net_target(iob_port_t *p, uint32_t ip) { ((iob_simnet_t *)p->ctx)->target_ip = ip; }

static bool net_send(iob_dgram_t *d, const uint8_t *buf, uint16_t len)
{
    iob_simnet_t *s = (iob_simnet_t *)d->ctx;
    s->datagrams++;
    s->qn = s->qi = 0;
    uint32_t now = s->now ? s->now() : bsp_ms();
    for (unsigned i = 0; i < s->n && s->qn < 8; ++i) {
        iob_node_t *nd = s->nodes[i];
        if (!nd->powered) continue;
        shadow_t *x = find(nd);
        push(nd, x);
        bool changed;
        if (node_net_disc(&x->net, now, buf, len, s->master_ip, s->q[s->qn], &changed)) s->qn++;
        pull(nd, x);
    }
    return true;
}

static uint16_t net_recv(iob_dgram_t *d, uint8_t *buf, uint16_t cap, uint32_t timeout)
{
    (void)timeout;
    iob_simnet_t *s = (iob_simnet_t *)d->ctx;
    if (s->qi >= s->qn || cap < IOB_DISC_LEN) return 0;
    memcpy(buf, s->q[s->qi++], IOB_DISC_LEN);
    return IOB_DISC_LEN;
}

void iob_simnet_init(iob_simnet_t *s, uint32_t master_ip)
{
    memset(s, 0, sizeof *s);
    s->port.xfer = net_xfer;
    s->port.target = net_target;
    s->port.ctx = s;
    s->disc.send = net_send;
    s->disc.recv = net_recv;
    s->disc.ctx = s;
    s->master_ip = master_ip;
}

void iob_simnet_add(iob_simnet_t *s, iob_node_t *n)
{
    if (s->n < 4) s->nodes[s->n++] = n;
}
