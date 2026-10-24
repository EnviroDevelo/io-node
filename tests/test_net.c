/* test_net.c - the board on Ethernet (node_net.c): Modbus TCP into the node core and back,
 * connection rules, discovery FIND / SET, and back to new over USB. The master side is the
 * platform's own codec (iob_net.c), as the supervisor uses it. */
#include <string.h>
#include "mb_rtu.h"
#include "node_net.h"
#include "sim_hw.h"
#include "test.h"

#define IP(x)  (0xC0A80100u + (x))
#define SUP    IP(50)
#define MASK24 0xFFFFFF00u

static node_t n;
static sim_hw_t hw;
static node_net_t nn;
static node_ipcfg_t saved;
static int store_rc;
static uint32_t stores;
static int store(void *ctx, const node_ipcfg_t *c) { (void)ctx; stores++; if (store_rc == NODE_HW_OK) saved = *c; return store_rc; }

static void boot(uint8_t address, const node_ipcfg_t *cfg) {
    sim_hw_init(&hw);
    node_boot_t b = { .address = address, .nv_ok = true, .hw_rev = 2,
                      .uid = { 0x1111, 0x2222, 0x3333, 0x4444, 0x5555, 0x6666 } };
    memcpy(b.serial, "CIO-26-00042", 12);
    node_init(&n, &hw.hw, &b, 1000);
    uint8_t mac[6];
    iob_mac_from_uid(b.uid, mac);
    store_rc = NODE_HW_OK; stores = 0;
    if (cfg) saved = *cfg; else memset(&saved, 0, sizeof saved);
    node_net_init(&nn, &n, cfg, mac, store, NULL);
}

/* one request as the supervisor sends it, the reply as it reads it back */
static int tcp_req(const uint8_t *rtu, size_t len, uint16_t tid, uint16_t *regs, uint16_t nregs, int *out_len) {
    uint8_t m[IOB_MBAP_MAX], tx[2 * IOB_MBAP_MAX], back[IOB_MBAP_MAX];
    size_t ml = iob_mbap_from_rtu(rtu, len, tid, m);
    int r = node_net_tcp(&nn, 1000, m, (uint16_t)ml, tx, sizeof tx);
    if (out_len) *out_len = r;
    if (r <= 0) return mbr_check_reply(rtu, len, back, 0, regs, nregs, NULL);
    uint16_t got_tid = 0;
    size_t bl = iob_rtu_from_mbap(tx, (size_t)r, &got_tid, back);
    CHECK_EQ(got_tid, tid);
    return mbr_check_reply(rtu, len, back, bl, regs, nregs, NULL);
}

static uint16_t disc(uint8_t op, uint16_t seq, uint32_t from, const node_ipcfg_t *c, uint8_t address, iob_disc_t *h, bool *changed) {
    iob_disc_t m; memset(&m, 0, sizeof m);
    m.op = op; m.seq = seq;
    memcpy(m.uid, n.uid, sizeof m.uid);
    if (c) { m.ip = c->ip; m.mask = c->mask; m.gw = c->gw; m.master_ip = c->master_ip; }
    m.address = address;
    uint8_t b[IOB_DISC_LEN], r[IOB_DISC_LEN];
    iob_disc_encode(&m, b);
    uint16_t len = node_net_disc(&nn, 1000, b, IOB_DISC_LEN, from, r, changed);
    if (len) CHECK(iob_disc_decode(r, len, h));
    return len;
}

static void t_tcp(void) {
    node_ipcfg_t cfg = { IP(61), MASK24, IP(1), SUP };
    boot(1, &cfg);
    CHECK(node_net_serving(&nn));
    CHECK(!node_net_accept(&nn, IP(51)));           /* not its supervisor */
    CHECK_EQ(nn.refused, 1u);
    CHECK(node_net_accept(&nn, SUP));
    uint8_t f[MBR_FRAME_MAX]; uint16_t regs[IOB_R_IDENT_N];
    size_t len = mbr_req_read(f, 1, IOB_R_MAGIC, IOB_R_IDENT_N);
    CHECK_EQ(tcp_req(f, len, 7, regs, IOB_R_IDENT_N, NULL), MBR_OK);
    CHECK_EQ(regs[0], IOB_MAGIC);
    CHECK(node_comms_ok(&n, 1000));                 /* Modbus TCP is the master's link (FW-58) */
    /* outputs on, as over RS-485 */
    uint16_t w[3] = { 0x0005, 1, 9 }, st[IOB_R_STATUS_N];
    len = mbr_req_rw(f, 1, IOB_R_STATUS, IOB_R_STATUS_N, IOB_R_OUT_CMD, w, 3);
    CHECK_EQ(tcp_req(f, len, 8, st, IOB_R_STATUS_N, NULL), MBR_OK);
    CHECK(n.gate);
    CHECK_EQ(hw.a_out, 0x05u); CHECK_EQ(hw.relays, 1u);
    /* another unit id: no answer */
    int ol = 0;
    len = mbr_req_read(f, 2, 0, 1);
    CHECK_EQ(tcp_req(f, len, 9, regs, 1, &ol), MBR_NO_REPLY);
    CHECK_EQ(ol, 0);
    /* split across segments, and two frames in one */
    uint8_t m[IOB_MBAP_MAX * 2], tx[IOB_MBAP_MAX * 2];
    len = mbr_req_read(f, 1, 0, 2);
    size_t a = iob_mbap_from_rtu(f, len, 1, m), b = iob_mbap_from_rtu(f, len, 2, m + a);
    CHECK_EQ(node_net_tcp(&nn, 1000, m, 5, tx, sizeof tx), 0);
    int r = node_net_tcp(&nn, 1000, m + 5, (uint16_t)(a + b - 5), tx, sizeof tx);
    CHECK_EQ(r, 2 * (6 + 3 + 4));                   /* two replies of 2 registers */
    CHECK_EQ(tx[1], 1u); CHECK_EQ(tx[13 + 1], 2u);  /* transaction ids 1 and 2 */
    /* not Modbus TCP: close */
    const uint8_t junk[8] = { 0, 1, 0, 7, 0, 2, 1, 3 };
    CHECK_EQ(node_net_tcp(&nn, 1000, junk, 8, tx, sizeof tx), -1);
    CHECK(node_net_accept(&nn, SUP));               /* a new connection starts clean */
    len = mbr_req_read(f, 1, 0, 1);
    CHECK_EQ(tcp_req(f, len, 10, regs, 1, NULL), MBR_OK);
    /* a new board serves nothing */
    boot(0, NULL);
    CHECK(!node_net_serving(&nn));
    CHECK(!node_net_accept(&nn, SUP));
    CHECK_EQ(node_net_tcp(&nn, 1000, m, (uint16_t)a, tx, sizeof tx), -1);
    /* saved settings that do not hang together are none */
    node_ipcfg_t bad = { IP(61), 0xFFFF00FFu, 0, SUP };
    boot(1, &bad);
    CHECK_EQ(nn.cfg.ip, 0u);
}

static void t_disc(void) {
    boot(0, NULL);                                  /* a new board */
    iob_disc_t h; bool ch;
    CHECK_EQ(disc(IOB_DISC_FIND, 3, SUP, NULL, 0, &h, &ch), IOB_DISC_LEN);
    CHECK_EQ(h.op, IOB_DISC_HERE); CHECK_EQ(h.seq, 3u);
    CHECK(h.flags & IOB_DISC_F_NEW);
    CHECK(!strcmp(h.serial, "CIO-26-00042"));
    CHECK_EQ(h.uid[0], 0x1111u); CHECK_EQ(h.mac[0], 0x02u);
    CHECK_EQ(h.address, 0u); CHECK_EQ(h.ip, 0u);
    CHECK(!ch);

    node_ipcfg_t c = { IP(62), MASK24, IP(1), SUP };
    CHECK_EQ(disc(IOB_DISC_SET, 4, SUP, &c, 2, &h, &ch), IOB_DISC_LEN);
    CHECK(ch);
    CHECK_EQ(h.ip, IP(62)); CHECK_EQ(h.address, 2u); CHECK_EQ(h.master_ip, SUP); CHECK(!(h.flags & IOB_DISC_F_NEW));
    CHECK_EQ(saved.ip, IP(62)); CHECK_EQ(hw.stored_address, 2u); CHECK_EQ(n.address, 2u);
    CHECK(node_net_serving(&nn));

    /* another supervisor: ignored */
    node_ipcfg_t o = { IP(70), MASK24, 0, IP(51) };
    CHECK_EQ(disc(IOB_DISC_SET, 5, IP(51), &o, 3, &h, &ch), 0u);
    CHECK_EQ(nn.cfg.ip, IP(62));
    /* another board's UID: ignored */
    iob_disc_t m; memset(&m, 0, sizeof m); m.op = IOB_DISC_SET; m.uid[0] = 9;
    m.ip = IP(63); m.mask = MASK24; m.master_ip = SUP; m.address = 3;
    uint8_t b[IOB_DISC_LEN], r[IOB_DISC_LEN];
    iob_disc_encode(&m, b);
    CHECK_EQ(node_net_disc(&nn, 1000, b, IOB_DISC_LEN, SUP, r, &ch), 0u);
    CHECK_EQ(node_net_disc(&nn, 1000, b, 10, SUP, r, &ch), 0u);   /* not a discovery message */
    /* its supervisor moves it; same IP settings, new address: no IP change */
    c.ip = IP(72);
    CHECK_EQ(disc(IOB_DISC_SET, 6, SUP, &c, 2, &h, &ch), IOB_DISC_LEN);
    CHECK(ch); CHECK_EQ(h.ip, IP(72));
    CHECK_EQ(disc(IOB_DISC_SET, 7, SUP, &c, 5, &h, &ch), IOB_DISC_LEN);
    CHECK(!ch); CHECK_EQ(n.address, 5u);
    /* a store that fails: nothing changes, the answer shows what is saved */
    store_rc = NODE_HW_NO_ANSWER;
    c.ip = IP(80);
    CHECK_EQ(disc(IOB_DISC_SET, 8, SUP, &c, 5, &h, &ch), IOB_DISC_LEN);
    CHECK(!ch); CHECK_EQ(h.ip, IP(72)); CHECK_EQ(nn.cfg.ip, IP(72));
    store_rc = NODE_HW_OK;
    hw.addr_store_rc = NODE_HW_NO_ANSWER;           /* the address cannot be saved: the IP is put back */
    CHECK_EQ(disc(IOB_DISC_SET, 9, SUP, &c, 6, &h, &ch), IOB_DISC_LEN);
    CHECK(!ch); CHECK_EQ(h.ip, IP(72)); CHECK_EQ(saved.ip, IP(72)); CHECK_EQ(n.address, 5u);
    hw.addr_store_rc = NODE_HW_OK;

    /* controlled flag while the master talks to it */
    CHECK(node_net_accept(&nn, SUP));
    uint8_t f[MBR_FRAME_MAX]; uint16_t regs[1];
    size_t len = mbr_req_read(f, 5, 0, 1);
    CHECK_EQ(tcp_req(f, len, 1, regs, 1, NULL), MBR_OK);
    CHECK_EQ(disc(IOB_DISC_FIND, 10, IP(99), NULL, 0, &h, &ch), IOB_DISC_LEN);   /* anyone may look */
    CHECK(h.flags & IOB_DISC_F_CONTROLLED);

    /* back to new over USB: address 0 clears the IP settings */
    CHECK(!node_net_poll(&nn));
    n.address = 0;
    CHECK(node_net_poll(&nn));
    CHECK_EQ(saved.ip, 0u); CHECK_EQ(nn.cfg.master_ip, 0u);
    CHECK(!node_net_serving(&nn));
    CHECK(!node_net_poll(&nn));
}

void test_net(void) {
    t_tcp();
    t_disc();
}
