/* test_node.c - the node core against the requirements, one block per FW-nn, using the
 * platform's own request builders and reply checker (mb_rtu.c) as the master would. */
#include <string.h>
#include "mb_rtu.h"
#include "node.h"
#include "node_version.h"
#include "sim_hw.h"
#include "test.h"

static node_t n;
static sim_hw_t hw;
static uint32_t now;
static uint8_t tx[NODE_FRAME_MAX];
static uint16_t regs[125];
static uint8_t exc;

static void boot(uint8_t dip, node_reset_t cause, uint16_t resets)
{
    node_boot_t b = { dip, 2, cause, resets, { 0x1111, 0x2222, 0x3333, 0x4444, 0x5555, 0x6666 } };
    now = 1000;
    node_init(&n, &hw.hw, &b, now);
}

static void run(uint32_t ms) { for (uint32_t i = 0; i < ms; i++) { now++; node_poll(&n, now); } }

/* send a frame on a port; returns the MBR_* check result (registers in regs, exception in exc) */
static int xfer(node_port_t port, const uint8_t *f, size_t len, uint16_t nregs)
{
    uint16_t r = node_frame(&n, port, now, f, (uint16_t)len, false, tx);
    exc = 0;
    return mbr_check_reply(f, len, tx, r, regs, nregs, &exc);
}
static int rw(node_port_t port, uint8_t addr, uint16_t out, uint16_t relay, uint16_t seq)
{
    uint8_t f[MBR_FRAME_MAX];
    uint16_t w[3] = { out, relay, seq };
    size_t len = mbr_req_rw(f, addr, IOB_R_STATUS, IOB_R_STATUS_N, IOB_R_OUT_CMD, w, 3);
    return xfer(port, f, len, IOB_R_STATUS_N);
}
static int rd(node_port_t port, uint8_t addr, uint16_t start, uint16_t cnt)
{
    uint8_t f[MBR_FRAME_MAX];
    size_t len = mbr_req_read(f, addr, start, cnt);
    return xfer(port, f, len, cnt);
}
static int wr1(node_port_t port, uint8_t addr, uint16_t reg, uint16_t v)
{
    uint8_t f[MBR_FRAME_MAX];
    size_t len = mbr_req_write1(f, addr, reg, v);
    return xfer(port, f, len, 0);
}
static int wrn(node_port_t port, uint8_t addr, uint16_t start, const uint16_t *v, uint16_t cnt)
{
    uint8_t f[MBR_FRAME_MAX];
    size_t len = mbr_req_write(f, addr, start, v, cnt);
    return xfer(port, f, len, 0);
}
static uint16_t reg(uint16_t r) { return node_reg(&n, r); }
#define RS NODE_PORT_RS485
#define USB NODE_PORT_USB

static void t_startup(void)       /* FW-01, FW-02, FW-03, FW-04, FW-51, FW-52 */
{
    sim_hw_init(&hw);
    hw.a_out = 0xFF; hw.b_out = 0xFF; hw.relays = 3;       /* whatever the pins were */
    boot(3, NODE_RST_POWER_ON, 0);
    CHECK_EQ(hw.a_out, 0); CHECK_EQ(hw.b_out, 0); CHECK_EQ(hw.relays, 0);
    CHECK(!node_healthy(&n));
    CHECK_EQ(hw.a_configs, 1); CHECK_EQ(hw.in_configs, 1);
    CHECK_EQ(hw.a_ol_cfg, 0xFF); CHECK_EQ(hw.in_filter_cfg, 8); CHECK_EQ(hw.in_wb_cfg, 0);
    CHECK_EQ(reg(IOB_R_STATUS), IOB_ST_FIELD_A_OK | IOB_ST_FIELD_B_OK | IOB_ST_CONFIG_NEEDED);
    CHECK_EQ(reg(IOB_R_HOLD_DS), 3000); CHECK_EQ(reg(IOB_R_FILTER_MS), 8);
    CHECK_EQ(reg(IOB_R_WB_ENABLE), 0); CHECK_EQ(reg(IOB_R_OL_ENABLE), 255);
    CHECK_EQ(rd(RS, 3, IOB_R_MAGIC, IOB_R_IDENT_N), MBR_OK);
    uint16_t want[15] = { IOB_MAGIC, IOB_MAP_VERSION, 2, NODE_FW_VERSION, NODE_FW_BUILD,
                          0x1111, 0x2222, 0x3333, 0x4444, 0x5555, 0x6666, 8, 8, 2, 8 };
    for (int i = 0; i < 15; i++) CHECK_EQ(regs[i], want[i]);
    /* reserved addresses read 0 */
    CHECK_EQ(rd(RS, 3, 0, 80), MBR_OK);
    for (int r = 0; r < 80; r++)
        if ((r > 2 && r < 16) || (r > 27 && r < 32) || (r > 37 && r < 64) || r == 79) CHECK_EQ(regs[r], 0);
    /* nothing switches on, however long it runs, until the master writes register 0 */
    run(500);
    CHECK(!node_healthy(&n)); CHECK_EQ(hw.a_out | hw.b_out | hw.relays, 0);
    CHECK_EQ(hw.b_services, 0);
    /* watchdog reset and the reset count (FW-03) */
    sim_hw_init(&hw);
    boot(3, NODE_RST_IWDG, 7);
    CHECK(reg(IOB_R_STATUS) & IOB_ST_WDG_RESET);
    CHECK_EQ(reg(IOB_R_RESETS), 7);
    boot(3, NODE_RST_PIN, 8);
    CHECK(!(reg(IOB_R_STATUS) & IOB_ST_WDG_RESET));
}

static void t_outputs(void)       /* FW-20, FW-25, write-before-read, CMD_ECHO */
{
    sim_hw_init(&hw);
    boot(1, NODE_RST_POWER_ON, 0);
    CHECK_EQ(rw(RS, 1, 0x8001, 2, 41), MBR_OK);
    /* the read part already shows this write: applied outputs and the echo */
    CHECK(regs[0] & IOB_ST_OUT_ENABLED);
    CHECK_EQ(regs[IOB_R_OUT_APPLIED - IOB_R_STATUS], 0x8001);
    CHECK_EQ(regs[IOB_R_RELAY_APPLIED - IOB_R_STATUS], 2);
    CHECK_EQ(regs[IOB_R_CMD_ECHO - IOB_R_STATUS], 41);
    CHECK_EQ(hw.a_out, 0x01); CHECK_EQ(hw.b_out, 0x80); CHECK_EQ(hw.relays, 2);
    CHECK(node_healthy(&n));
    /* request and reply sizes as in the timing budget: 19 and 29 bytes */
    uint8_t f[MBR_FRAME_MAX];
    uint16_t w[3] = { 0, 0, 1 };
    CHECK_EQ(mbr_req_rw(f, 1, IOB_R_STATUS, 12, 0, w, 3), 19);
    CHECK_EQ(node_frame(&n, RS, now, f, 19, false, tx), 29);
    CHECK_EQ(hw.a_out | hw.b_out | hw.relays, 0);
    /* outputs and relays from one request change together */
    CHECK_EQ(rw(RS, 1, 0xFFFF, 3, 2), MBR_OK);
    CHECK_EQ(hw.a_out, 0xFF); CHECK_EQ(hw.b_out, 0xFF); CHECK_EQ(hw.relays, 3);
    run(100);
    CHECK(hw.b_services >= 100);                    /* FW-24: serviced while the gate is open */
}

static void t_hold(void)          /* FW-21, FW-26, FW-44 */
{
    sim_hw_init(&hw);
    boot(1, NODE_RST_POWER_ON, 0);
    CHECK_EQ(wr1(RS, 1, IOB_R_HOLD_DS, 100), MBR_OK);           /* 10 s */
    CHECK(!(reg(IOB_R_STATUS) & IOB_ST_CONFIG_NEEDED));
    CHECK_EQ(rw(RS, 1, 0x00FF, 1, 1), MBR_OK);
    uint32_t t0 = now;
    /* reads, settings and the relay register alone do not restart the hold timer */
    run(5000);
    CHECK_EQ(rd(RS, 1, 0, 80), MBR_OK);
    CHECK_EQ(wr1(RS, 1, IOB_R_FILTER_MS, 10), MBR_OK);
    CHECK_EQ(wr1(RS, 1, IOB_R_RELAY_CMD, 1), MBR_OK);
    while (now - t0 < 9999) { now++; node_poll(&n, now); }
    CHECK(node_healthy(&n)); CHECK_EQ(hw.a_out, 0xFF);
    now++; node_poll(&n, now);                                   /* 10.000 s */
    CHECK(!node_healthy(&n));
    CHECK_EQ(hw.a_out | hw.b_out | hw.relays, 0);
    CHECK_EQ(reg(IOB_R_OUT_APPLIED), 0); CHECK_EQ(reg(IOB_R_RELAY_APPLIED), 0);
    CHECK(reg(IOB_R_STATUS) & IOB_ST_TIMEOUT);
    CHECK(!(reg(IOB_R_STATUS) & IOB_ST_OUT_ENABLED));
    uint32_t serv = hw.b_services;
    run(50);
    CHECK_EQ(hw.b_services, serv);                               /* FW-24: not while closed */
    /* FW-26: none of these switch anything on */
    CHECK_EQ(wr1(RS, 1, IOB_R_RELAY_CMD, 3), MBR_OK);
    uint16_t cfg[4] = { 100, 8, 0, 255 };
    CHECK_EQ(wrn(RS, 1, IOB_R_HOLD_DS, cfg, 4), MBR_OK);
    CHECK_EQ(wr1(RS, 1, IOB_R_CMD_SEQ, 9), MBR_OK);
    CHECK_EQ(rd(RS, 1, 0, 80), MBR_OK);
    run(10);
    CHECK_EQ(hw.a_out | hw.b_out | hw.relays, 0);
    /* FAULT_CLEAR clears TIMEOUT (and WDG_RESET) only */
    CHECK_EQ(wr1(RS, 1, IOB_R_FAULT_CLEAR, 1), MBR_OK);
    CHECK(!(reg(IOB_R_STATUS) & IOB_ST_TIMEOUT));
    CHECK_EQ(hw.a_out | hw.b_out | hw.relays, 0);
    CHECK_EQ(reg(IOB_R_FAULT_CLEAR), 0);
    /* the next valid write of register 0 re-opens the gate */
    CHECK_EQ(wr1(RS, 1, IOB_R_OUT_CMD, 0x0300), MBR_OK);
    CHECK(node_healthy(&n));
    CHECK_EQ(hw.b_out, 0x03); CHECK_EQ(hw.relays, 3);
    /* a frame arriving after the hold ran out, before the next loop pass, cannot act on the old hold */
    CHECK_EQ(wr1(RS, 1, IOB_R_HOLD_DS, 10), MBR_OK);
    CHECK_EQ(wr1(RS, 1, IOB_R_OUT_CMD, 0x00F0), MBR_OK);
    now += 1000;                                                  /* no node_poll in between */
    CHECK_EQ(wr1(RS, 1, IOB_R_RELAY_CMD, 3), MBR_OK);
    CHECK_EQ(hw.a_out | hw.b_out | hw.relays, 0);
    CHECK(reg(IOB_R_STATUS) & IOB_ST_TIMEOUT);
    CHECK_EQ(wr1(RS, 1, IOB_R_FAULT_CLEAR, 1), MBR_OK);
    /* the longest hold, 10 min */
    CHECK_EQ(wr1(RS, 1, IOB_R_HOLD_DS, 6000), MBR_OK);
    CHECK_EQ(wr1(RS, 1, IOB_R_OUT_CMD, 1), MBR_OK);
    run(599999); CHECK(node_healthy(&n));
    run(1);      CHECK(!node_healthy(&n));
    /* the clock wrapping past 2^32 ms does not trip or stall the hold */
    sim_hw_init(&hw);
    node_boot_t b = { 1, 1, NODE_RST_POWER_ON, 0, { 0 } };
    now = 0xFFFFFF00u;
    node_init(&n, &hw.hw, &b, now);
    CHECK_EQ(wr1(RS, 1, IOB_R_HOLD_DS, 10), MBR_OK);
    CHECK_EQ(wr1(RS, 1, IOB_R_OUT_CMD, 1), MBR_OK);
    run(999); CHECK(node_healthy(&n));
    run(1);   CHECK(!node_healthy(&n));
}

static void t_rules(void)         /* section 5 exceptions, FW-12, FW-02 */
{
    sim_hw_init(&hw);
    boot(5, NODE_RST_POWER_ON, 0);
    uint8_t f[MBR_FRAME_MAX];
    /* read-only and reserved registers: 02 */
    CHECK_EQ(wr1(RS, 5, IOB_R_STATUS, 1), MBR_EXCEPTION); CHECK_EQ(exc, 2);
    CHECK_EQ(wr1(RS, 5, 10, 1), MBR_EXCEPTION); CHECK_EQ(exc, 2);
    CHECK_EQ(wr1(RS, 5, IOB_R_MAGIC, 1), MBR_EXCEPTION); CHECK_EQ(exc, 2);
    CHECK_EQ(wr1(RS, 5, IOB_R_BOOTLOADER, IOB_BOOT_KEY), MBR_EXCEPTION); CHECK_EQ(exc, 2);   /* FW-57 */
    /* out of range: 03, and nothing in the request is written */
    uint16_t cfg[4] = { 50, 101, 1, 1 };
    CHECK_EQ(wrn(RS, 5, IOB_R_HOLD_DS, cfg, 4), MBR_EXCEPTION); CHECK_EQ(exc, 3);
    CHECK_EQ(reg(IOB_R_HOLD_DS), 3000); CHECK_EQ(reg(IOB_R_WB_ENABLE), 0);
    CHECK(reg(IOB_R_STATUS) & IOB_ST_CONFIG_NEEDED);
    CHECK_EQ(wr1(RS, 5, IOB_R_HOLD_DS, 0), MBR_EXCEPTION); CHECK_EQ(exc, 3);
    CHECK_EQ(wr1(RS, 5, IOB_R_HOLD_DS, 6001), MBR_EXCEPTION); CHECK_EQ(exc, 3);
    CHECK_EQ(wr1(RS, 5, IOB_R_RELAY_CMD, 4), MBR_EXCEPTION); CHECK_EQ(exc, 3);
    CHECK_EQ(wr1(RS, 5, IOB_R_FAULT_CLEAR, 4), MBR_EXCEPTION); CHECK_EQ(exc, 3);
    CHECK_EQ(wr1(RS, 5, IOB_R_OL_ENABLE, 256), MBR_EXCEPTION); CHECK_EQ(exc, 3);
    /* FC23 whose write part is bad changes nothing and does not open the gate */
    uint16_t w[3] = { 0xFFFF, 7, 3 };
    size_t len = mbr_req_rw(f, 5, IOB_R_STATUS, 12, 0, w, 3);
    CHECK_EQ(xfer(RS, f, len, 12), MBR_EXCEPTION); CHECK_EQ(exc, 3);
    CHECK(!node_healthy(&n)); CHECK_EQ(reg(IOB_R_OUT_CMD), 0); CHECK_EQ(reg(IOB_R_CMD_SEQ), 0);
    /* a write spanning a writable and a read-only register */
    uint16_t two[2] = { 1, 1 };
    CHECK_EQ(wrn(RS, 5, IOB_R_CMD_SEQ, two, 2), MBR_EXCEPTION); CHECK_EQ(exc, 2);
    CHECK_EQ(reg(IOB_R_CMD_SEQ), 0);
    /* addresses past 79: 02; counts of 0 or too many: 03 */
    CHECK_EQ(rd(RS, 5, 79, 2), MBR_EXCEPTION); CHECK_EQ(exc, 2);
    CHECK_EQ(rd(RS, 5, 79, 1), MBR_OK);
    f[0] = 5; f[1] = 3; f[2] = 0; f[3] = 0; f[4] = 0; f[5] = 0;
    uint16_t c = mbr_crc16(f, 6); f[6] = (uint8_t)c; f[7] = (uint8_t)(c >> 8);
    CHECK_EQ(xfer(RS, f, 8, 0), MBR_EXCEPTION); CHECK_EQ(exc, 3);
    f[5] = 126; c = mbr_crc16(f, 6); f[6] = (uint8_t)c; f[7] = (uint8_t)(c >> 8);
    CHECK_EQ(xfer(RS, f, 8, 0), MBR_EXCEPTION); CHECK_EQ(exc, 3);
    /* function 04 reads the same space */
    f[1] = 4; f[2] = 0; f[3] = 64; f[4] = 0; f[5] = 1; c = mbr_crc16(f, 6); f[6] = (uint8_t)c; f[7] = (uint8_t)(c >> 8);
    CHECK_EQ(node_frame(&n, RS, now, f, 8, false, tx), 7);
    CHECK_EQ(tx[1], 4); CHECK_EQ(tx[3] << 8 | tx[4], IOB_MAGIC);
    /* unknown function: 01 */
    f[1] = 5; c = mbr_crc16(f, 6); f[6] = (uint8_t)c; f[7] = (uint8_t)(c >> 8);
    CHECK_EQ(node_frame(&n, RS, now, f, 8, false, tx), 5); CHECK_EQ(tx[1], 0x85); CHECK_EQ(tx[2], 1);
    /* FW-12: bad CRC, a framing error, a runt: no reply, counted */
    uint16_t e0 = reg(IOB_R_RX_ERRORS);
    len = mbr_req_read(f, 5, 0, 1); f[len - 1] ^= 1;
    CHECK_EQ(node_frame(&n, RS, now, f, (uint16_t)len, false, tx), 0);
    f[len - 1] ^= 1;
    CHECK_EQ(node_frame(&n, RS, now, f, (uint16_t)len, true, tx), 0);
    CHECK_EQ(node_frame(&n, RS, now, f, 3, false, tx), 0);
    CHECK_EQ(reg(IOB_R_RX_ERRORS), e0 + 3u);
    /* another address, and a broadcast: no reply, not errors */
    len = mbr_req_read(f, 6, 0, 1);
    CHECK_EQ(node_frame(&n, RS, now, f, (uint16_t)len, false, tx), 0);
    f[0] = 0; c = mbr_crc16(f, 6); f[6] = (uint8_t)c; f[7] = (uint8_t)(c >> 8);
    CHECK_EQ(node_frame(&n, RS, now, f, 8, false, tx), 0);
    CHECK_EQ(reg(IOB_R_RX_ERRORS), e0 + 3u);
    /* RX_ERRORS wraps */
    n.rx_errors = 0xFFFF;
    len = mbr_req_read(f, 5, 0, 1); f[2] ^= 0x10;
    node_frame(&n, RS, now, f, (uint16_t)len, false, tx);
    CHECK_EQ(reg(IOB_R_RX_ERRORS), 0);
    /* FW-02: DIP 0 never answers on RS-485; its comms LED is steady on */
    boot(0, NODE_RST_POWER_ON, 0);
    for (uint8_t a = 0; a < 248; a++) {
        if (a == 0) continue;
        len = mbr_req_read(f, a, 0, 1);
        if (node_frame(&n, RS, now, f, (uint16_t)len, false, tx)) { CHECK(0); break; }
    }
    bool steady = true;                                       /* comms LED steady on */
    for (uint32_t t = 0; t < 2000; t++) steady &= node_comms_led(&n, t);
    CHECK(steady);
    boot(1, NODE_RST_POWER_ON, 0);
    CHECK(!node_comms_led(&n, now));
}

static void t_usb(void)           /* FW-56, FW-57 */
{
    sim_hw_init(&hw);
    boot(2, NODE_RST_POWER_ON, 0);
    /* answers at its DIP address and at 247, never at 247 on RS-485 */
    CHECK_EQ(rd(USB, 247, IOB_R_MAGIC, 1), MBR_OK); CHECK_EQ(regs[0], IOB_MAGIC);
    CHECK_EQ(rd(USB, 2, IOB_R_MAGIC, 1), MBR_OK);
    CHECK_EQ(rd(RS, 247, IOB_R_MAGIC, 1), MBR_NO_REPLY);
    CHECK_EQ(rd(USB, 3, IOB_R_MAGIC, 1), MBR_NO_REPLY);
    /* no master yet: the USB port may drive the outputs */
    CHECK_EQ(wr1(USB, 247, IOB_R_OUT_CMD, 0x0003), MBR_OK);
    CHECK_EQ(hw.a_out, 3);
    /* the RS-485 master takes control: USB writes to 0-2 refused (04), settings still allowed */
    CHECK_EQ(wr1(RS, 2, IOB_R_HOLD_DS, 20), MBR_OK);       /* 2 s */
    CHECK_EQ(rw(RS, 2, 0x0100, 0, 5), MBR_OK);
    CHECK_EQ(wr1(USB, 247, IOB_R_OUT_CMD, 0x00FF), MBR_EXCEPTION); CHECK_EQ(exc, 4);
    CHECK_EQ(wr1(USB, 247, IOB_R_CMD_SEQ, 1), MBR_EXCEPTION); CHECK_EQ(exc, 4);
    uint16_t w[3] = { 0xFF, 3, 1 };
    uint8_t f[MBR_FRAME_MAX];
    size_t len = mbr_req_rw(f, 247, IOB_R_STATUS, 12, 0, w, 3);
    CHECK_EQ(xfer(USB, f, len, 12), MBR_EXCEPTION); CHECK_EQ(exc, 4);
    CHECK_EQ(hw.a_out, 0); CHECK_EQ(hw.b_out, 1); CHECK_EQ(reg(IOB_R_CMD_SEQ), 5);
    CHECK_EQ(wr1(USB, 247, IOB_R_WB_ENABLE, 0x0F), MBR_OK);
    CHECK_EQ(rd(USB, 247, IOB_R_STATUS, 12), MBR_OK);
    /* the master's hold time passes: USB may write again */
    run(2000);
    CHECK(!node_master_in_control(&n, now));
    CHECK_EQ(wr1(USB, 247, IOB_R_OUT_CMD, 0x0001), MBR_OK);
    /* bootloader: wrong key 03, right key accepted; outputs off and HEALTHY stopped first */
    CHECK_EQ(wr1(USB, 247, IOB_R_BOOTLOADER, 0x1234), MBR_EXCEPTION); CHECK_EQ(exc, 3);
    CHECK(node_healthy(&n));
    CHECK_EQ(wr1(USB, 247, IOB_R_BOOTLOADER, IOB_BOOT_KEY), MBR_OK);
    CHECK(!node_healthy(&n));
    CHECK_EQ(hw.a_out | hw.b_out | hw.relays, 0);
    CHECK_EQ(reg(IOB_R_BOOTLOADER), 0);
    run(99); CHECK(!node_boot_due(&n, now));
    run(1);  CHECK(node_boot_due(&n, now));
    CHECK_EQ(hw.a_out | hw.b_out | hw.relays, 0);
    /* USB errors are not RS-485 errors */
    len = mbr_req_read(f, 247, 0, 1); f[3] ^= 1;
    CHECK_EQ(node_frame(&n, USB, now, f, (uint16_t)len, false, tx), 0);
    CHECK_EQ(reg(IOB_R_RX_ERRORS), 0);
}

static void t_chips(void)         /* FW-04, FW-22, FW-30..32, FW-40..43 */
{
    /* bank A missing at start: OUT_A_CHIP, retried every second; bank B and the relays still run
     * (a bank A fault is only reported, Gary 22 Oct); bank A is not reported as driven */
    sim_hw_init(&hw);
    hw.a_dead = true;
    boot(1, NODE_RST_POWER_ON, 0);
    CHECK(reg(IOB_R_STATUS) & IOB_ST_OUT_A_CHIP);
    CHECK_EQ(rw(RS, 1, 0xFFFF, 3, 1), MBR_OK);
    CHECK(regs[0] & IOB_ST_OUT_ENABLED);
    CHECK(regs[0] & IOB_ST_OUT_A_CHIP);
    CHECK(node_healthy(&n));
    CHECK_EQ(regs[IOB_R_OUT_APPLIED - IOB_R_STATUS], 0xFF00);
    CHECK_EQ(regs[IOB_R_RELAY_APPLIED - IOB_R_STATUS], 3);
    CHECK_EQ(hw.b_out, 0xFF); CHECK_EQ(hw.relays, 3);
    uint32_t c0 = hw.a_configs;
    run(999);  CHECK_EQ(hw.a_configs, c0);
    run(1);    CHECK_EQ(hw.a_configs, c0 + 1);
    run(1000); CHECK_EQ(hw.a_configs, c0 + 2);
    hw.a_dead = false;
    run(1000);
    CHECK(!(reg(IOB_R_STATUS) & IOB_ST_OUT_A_CHIP));
    CHECK(node_healthy(&n));
    CHECK_EQ(hw.a_out, 0xFF); CHECK_EQ(hw.b_out, 0xFF); CHECK_EQ(hw.relays, 3);
    /* one failed diagnostic read is tolerated, two in a row are a fault */
    hw.a_fail_reads = 1;
    run(20);
    CHECK(!(reg(IOB_R_STATUS) & IOB_ST_OUT_A_CHIP));
    hw.a_fail_reads = 2;
    run(20);
    CHECK(reg(IOB_R_STATUS) & IOB_ST_OUT_A_CHIP);
    CHECK(node_healthy(&n));
    CHECK_EQ(hw.b_out, 0xFF); CHECK_EQ(hw.relays, 3);
    CHECK_EQ(reg(IOB_R_OUT_APPLIED), 0xFF00);
    CHECK(node_fault_led(&n));
    run(1000);
    CHECK(!(reg(IOB_R_STATUS) & IOB_ST_OUT_A_CHIP));
    CHECK(node_healthy(&n));
    /* the chip reports an internal fault */
    hw.a_internal_fault = true;
    run(10);
    CHECK(reg(IOB_R_STATUS) & IOB_ST_OUT_A_CHIP);
    hw.a_internal_fault = false;
    run(10);
    CHECK(!(reg(IOB_R_STATUS) & IOB_ST_OUT_A_CHIP));
    /* configuration that reads back wrong counts as a failure (FW-04) */
    hw.a_readback_bad = true;
    CHECK_EQ(wr1(RS, 1, IOB_R_OL_ENABLE, 0x0F), MBR_OK);
    run(2);
    CHECK(reg(IOB_R_STATUS) & IOB_ST_OUT_A_CHIP);
    hw.a_readback_bad = false;
    run(1000);
    CHECK(!(reg(IOB_R_STATUS) & IOB_ST_OUT_A_CHIP));
    CHECK_EQ(hw.a_ol_cfg, 0x0F);

    /* bank A diagnostics, open circuit masked by OL_ENABLE, present state not latched (FW-40) */
    hw.a_open_raw = 0xF1; hw.a_overload = 0x02; hw.a_overtemp = 0x04;
    run(10);
    CHECK_EQ(reg(IOB_R_A_OPEN), 0x01); CHECK_EQ(reg(IOB_R_A_OVERLOAD), 0x02); CHECK_EQ(reg(IOB_R_A_OVERTEMP), 0x04);
    hw.a_open_raw = 0; hw.a_overload = 0; hw.a_overtemp = 0;
    run(10);
    CHECK_EQ(reg(IOB_R_A_OPEN) | reg(IOB_R_A_OVERLOAD) | reg(IOB_R_A_OVERTEMP), 0);

    /* inputs: read at least every 5 ms, 0 without field supply A, wire break masked */
    uint32_t r0 = hw.in_reads;
    run(100);
    CHECK(hw.in_reads - r0 >= 20);
    hw.inputs = 0xA5; hw.wirebreak_raw = 0xFF;
    run(5);
    CHECK_EQ(reg(IOB_R_INPUTS), 0xA5);
    CHECK_EQ(reg(IOB_R_WIREBREAK), 0);
    CHECK_EQ(wr1(RS, 1, IOB_R_WB_ENABLE, 0x30), MBR_OK);
    CHECK_EQ(wr1(RS, 1, IOB_R_FILTER_MS, 20), MBR_OK);
    run(5);
    CHECK_EQ(hw.in_wb_cfg, 0x30); CHECK_EQ(hw.in_filter_cfg, 20);
    CHECK_EQ(reg(IOB_R_WIREBREAK), 0x30);
    hw.field_a = false;
    run(10);
    CHECK_EQ(reg(IOB_R_INPUTS), 0);
    CHECK(!(reg(IOB_R_STATUS) & IOB_ST_FIELD_A_OK));
    CHECK(node_fault_led(&n));
    hw.field_a = true;
    run(10);
    CHECK(reg(IOB_R_STATUS) & IOB_ST_FIELD_A_OK);
    CHECK_EQ(reg(IOB_R_INPUTS), 0xA5);
    /* input chip faults: two failed reads, then IN_CHIP, inputs 0; outputs unaffected */
    hw.in_fail_reads = 2;
    run(10);
    CHECK(reg(IOB_R_STATUS) & IOB_ST_IN_CHIP);
    CHECK_EQ(reg(IOB_R_INPUTS), 0);
    CHECK(node_healthy(&n));
    run(1000);
    CHECK(!(reg(IOB_R_STATUS) & IOB_ST_IN_CHIP));
    CHECK_EQ(reg(IOB_R_INPUTS), 0xA5);

    /* bank B: FAULT debounced 1 ms, its watchdog, its supply (FW-41, FW-42) */
    hw.b_fault_pin = true;
    now++; node_poll(&n, now);
    CHECK_EQ(reg(IOB_R_B_FAULT), 0);
    now++; node_poll(&n, now);
    CHECK_EQ(reg(IOB_R_B_FAULT), IOB_BF_FAULT);
    hw.b_fault_pin = false;
    run(2);
    CHECK_EQ(reg(IOB_R_B_FAULT), 0);
    hw.b_wdg_tripped = true;
    run(1);
    CHECK_EQ(reg(IOB_R_B_FAULT), IOB_BF_WDG);
    CHECK_EQ(reg(IOB_R_OUT_APPLIED), 0x00FF);      /* bank B not driven */
    hw.b_wdg_tripped = false;
    run(1);
    CHECK_EQ(reg(IOB_R_OUT_APPLIED), 0xFFFF);
    hw.field_b = false;
    run(1);
    CHECK(!(reg(IOB_R_STATUS) & IOB_ST_FIELD_B_OK));
}

/* FW-55, FW-58, FW-59: the communications watchdog, its latch and the LEDs */
static uint32_t led_on_ms(bool (*f)(const node_t *, uint32_t), uint32_t from, uint32_t ms)
{
    uint32_t on = 0;
    for (uint32_t t = from; t < from + ms; t++) on += f(&n, t);
    return on;
}
static int led_flashes(uint32_t from, uint32_t ms)             /* comms LED: off -> on changes */
{
    int k = 0;
    bool last = node_comms_led(&n, from - 1u);
    for (uint32_t t = from; t < from + ms; t++) { bool v = node_comms_led(&n, t); k += v && !last; last = v; }
    return k;
}
static uint16_t status(void) { return reg(IOB_R_STATUS); }
static void align(void) { run(1000u - now % 1000u); }    /* to a whole second */

static void t_comms(void)
{
    sim_hw_init(&hw);
    boot(3, NODE_RST_POWER_ON, 0);
    uint8_t f[MBR_FRAME_MAX];
    size_t len;
    /* heartbeat: 1 Hz, 50 %, whatever the link does */
    bool hb = true;
    for (uint32_t t = 0; t < 3000; t++) hb &= node_heartbeat_led(t) == ((t / 500u) % 2u == 0);
    CHECK(hb);
    /* before the master's first request: link down but nothing latched, comms LED off */
    CHECK(!node_comms_ok(&n, now));
    CHECK_EQ(led_on_ms(node_comms_led, now, 2000), 0);
    run(5000);
    CHECK(!(status() & IOB_ST_COMMS_LOST));
    /* USB, another address, a broadcast, a bad CRC and a framing error do not count */
    len = mbr_req_read(f, NODE_USB_ADDRESS, IOB_R_MAGIC, 1);
    CHECK_EQ(xfer(NODE_PORT_USB, f, len, 1), MBR_OK);
    len = mbr_req_read(f, 3, IOB_R_MAGIC, 1);
    CHECK_EQ(xfer(NODE_PORT_USB, f, len, 1), MBR_OK);
    len = mbr_req_read(f, 4, IOB_R_MAGIC, 1);
    CHECK_EQ(node_frame(&n, RS, now, f, (uint16_t)len, false, tx), 0);
    uint16_t w = 50;
    len = mbr_req_write(f, 0, IOB_R_HOLD_DS, &w, 1);
    CHECK_EQ(node_frame(&n, RS, now, f, (uint16_t)len, false, tx), 0);
    len = mbr_req_read(f, 3, IOB_R_MAGIC, 1); f[len - 1] ^= 1u;
    CHECK_EQ(node_frame(&n, RS, now, f, (uint16_t)len, false, tx), 0);
    len = mbr_req_read(f, 3, IOB_R_MAGIC, 1);
    CHECK_EQ(node_frame(&n, RS, now, f, (uint16_t)len, true, tx), 0);
    CHECK(!node_comms_ok(&n, now));
    /* a request on RS-485 brings the link up: one 100 ms flash every 0.5 s */
    align();
    CHECK_EQ(rd(RS, 3, IOB_R_MAGIC, 1), MBR_OK);
    CHECK(node_comms_ok(&n, now));
    CHECK_EQ(led_flashes(now, 1000), 2);
    CHECK_EQ(led_on_ms(node_comms_led, now, 1000), 200);
    run(NODE_COMMS_TIMEOUT_MS - 1u);
    CHECK(node_comms_ok(&n, now));
    CHECK(!(status() & IOB_ST_COMMS_LOST));
    run(1);
    /* 1 s without a request: down, latched, comms LED fast 4 Hz */
    CHECK(!node_comms_ok(&n, now));
    CHECK(status() & IOB_ST_COMMS_LOST);
    CHECK_EQ(led_flashes(now, 1000), 4);
    /* the link comes back (an exception reply counts too): still latched, double flash */
    align();
    f[0] = 3; f[1] = 0x2B; f[2] = 0x0E; f[3] = 0x01; f[4] = 0;
    uint16_t c = mbr_crc16(f, 5); f[5] = (uint8_t)c; f[6] = (uint8_t)(c >> 8);
    CHECK(node_frame(&n, RS, now, f, 7, false, tx) == 5);
    CHECK(node_comms_ok(&n, now));
    CHECK(status() & IOB_ST_COMMS_LOST);
    CHECK_EQ(led_flashes(now, 1000), 2);
    CHECK_EQ(led_on_ms(node_comms_led, now, 1000), 200);
    bool dbl = node_comms_led(&n, now + 50u) && !node_comms_led(&n, now + 150u) &&
               node_comms_led(&n, now + 300u) && !node_comms_led(&n, now + 600u);   /* both in the first half */
    CHECK(dbl);
    /* healthy traffic does not clear it, nor does FAULT_CLEAR bit 0 */
    for (int i = 0; i < 50; i++) { run(20); CHECK_EQ(rw(RS, 3, 0, 0, (uint16_t)i), MBR_OK); }
    CHECK(status() & IOB_ST_COMMS_LOST);
    CHECK_EQ(wr1(RS, 3, IOB_R_FAULT_CLEAR, IOB_FC_EVENTS), MBR_OK);
    CHECK(status() & IOB_ST_COMMS_LOST);
    /* FAULT_CLEAR 4 is out of range and changes nothing */
    CHECK_EQ(wr1(RS, 3, IOB_R_FAULT_CLEAR, 4), MBR_EXCEPTION);
    CHECK_EQ(exc, 3);
    CHECK(status() & IOB_ST_COMMS_LOST);
    /* bit 1 clears it (the main board, when IOB_LOST is acknowledged); also over USB */
    align();
    CHECK_EQ(wr1(RS, 3, IOB_R_FAULT_CLEAR, IOB_FC_COMMS), MBR_OK);
    CHECK(!(status() & IOB_ST_COMMS_LOST));
    CHECK_EQ(led_flashes(now, 1000), 2);
    CHECK(!node_comms_led(&n, now + 300u));                  /* single flash again */
    run(NODE_COMMS_TIMEOUT_MS + 5u);
    CHECK(status() & IOB_ST_COMMS_LOST);
    CHECK_EQ(wr1(NODE_PORT_USB, NODE_USB_ADDRESS, IOB_R_FAULT_CLEAR, 3), MBR_OK);
    CHECK(!(status() & IOB_ST_COMMS_LOST));
    /* the outputs do not follow the link: they hold until the hold time (FW-21) */
    CHECK_EQ(rw(RS, 3, 0x0101, 0, 1), MBR_OK);
    run(NODE_COMMS_TIMEOUT_MS + 10u);
    CHECK(!node_comms_ok(&n, now));
    CHECK_EQ(hw.a_out, 0x01);
    CHECK_EQ(hw.b_out, 0x01);
    CHECK(node_healthy(&n));
    /* a restart forgets the latch (the main board sees the restart itself) */
    boot(3, NODE_RST_POWER_ON, 0);
    CHECK(!(status() & IOB_ST_COMMS_LOST));
    /* DIP 0 never answers, so its link never comes up or latches */
    boot(0, NODE_RST_POWER_ON, 0);
    len = mbr_req_read(f, 0, IOB_R_MAGIC, 1);
    node_frame(&n, RS, now, f, (uint16_t)len, false, tx);
    run(3000);
    CHECK(!node_comms_ok(&n, now));
    CHECK(!(status() & IOB_ST_COMMS_LOST));
}

/* FW-33: every input change reaches the RS-485 master at least once */
static uint16_t rs_inputs(void) { CHECK_EQ(rd(RS, 3, IOB_R_INPUTS, 1), MBR_OK); return regs[0]; }
static void t_inputs_hold(void)
{
    sim_hw_init(&hw);
    boot(3, NODE_RST_POWER_ON, 0);
    run(10);
    hw.inputs = 0x81;                                           /* two healthy contacts closed */
    run(4);
    CHECK_EQ(rs_inputs(), 0x81);
    CHECK_EQ(rs_inputs(), 0x81);
    /* input 1 opens for 6 ms between two reads: the next read shows it open, the one after closed */
    hw.inputs = 0x80; run(6); hw.inputs = 0x81; run(6);
    CHECK_EQ(reg(IOB_R_INPUTS), 0x80);                          /* reg() does not consume */
    CHECK_EQ(reg(IOB_R_INPUTS), 0x80);
    CHECK_EQ(rs_inputs(), 0x80);
    CHECK_EQ(rs_inputs(), 0x81);
    CHECK_EQ(rs_inputs(), 0x81);
    /* the same through the master's function 23 status read */
    hw.inputs = 0x01; run(4); hw.inputs = 0x81; run(4);
    CHECK_EQ(rw(RS, 3, 0, 0, 7), MBR_OK);
    CHECK_EQ(regs[IOB_R_INPUTS - IOB_R_STATUS], 0x01);
    CHECK_EQ(rw(RS, 3, 0, 0, 8), MBR_OK);
    CHECK_EQ(regs[IOB_R_INPUTS - IOB_R_STATUS], 0x81);
    /* a short closing of an open input is shown too */
    hw.inputs = 0x80; run(4); CHECK_EQ(rs_inputs(), 0x80);
    hw.inputs = 0x82; run(4); hw.inputs = 0x80; run(4);
    CHECK_EQ(rs_inputs(), 0x82);
    CHECK_EQ(rs_inputs(), 0x80);
    /* a read that does not include register 17, an exception, or a USB read consumes nothing */
    hw.inputs = 0x00; run(4); hw.inputs = 0x80; run(4);
    CHECK_EQ(rd(RS, 3, IOB_R_STATUS, 1), MBR_OK);
    CHECK_EQ(rd(RS, 3, IOB_R_OUT_APPLIED, 2), MBR_OK);
    CHECK_EQ(rd(RS, 3, IOB_R_INPUTS, 70), MBR_EXCEPTION);
    CHECK_EQ(rd(NODE_PORT_USB, NODE_USB_ADDRESS, IOB_R_INPUTS, 1), MBR_OK);
    CHECK_EQ(regs[0], 0x80);                                    /* USB: as they are now */
    CHECK_EQ(rs_inputs(), 0x00);
    CHECK_EQ(rs_inputs(), 0x80);
    /* with no master on the bus, USB keeps following the inputs (nothing piles up) */
    for (int k = 0; k < 4; k++) {
        hw.inputs = (uint8_t)(k & 1 ? 0x80 : 0x01); run(4);
        CHECK_EQ(rd(NODE_PORT_USB, NODE_USB_ADDRESS, IOB_R_INPUTS, 1), MBR_OK);
        CHECK_EQ(regs[0], k & 1 ? 0x80 : 0x01);
    }
    hw.inputs = 0x80; run(4);
    CHECK_EQ(rs_inputs(), 0x01);                                /* the master still gets every change */
    CHECK_EQ(rs_inputs(), 0x80);
    /* field supply A lost for a moment between reads: inputs shown 0 once (FW-32), then back */
    hw.field_a = false; run(20); hw.field_a = true; run(20);
    CHECK_EQ(rs_inputs(), 0x00);
    CHECK_EQ(rs_inputs(), 0x80);
    /* input chip failed: 0 for as long as it is faulty */
    hw.in_dead = true; run(20);
    CHECK(status() & IOB_ST_IN_CHIP);
    CHECK_EQ(rs_inputs(), 0x00);
    CHECK_EQ(rs_inputs(), 0x00);
}

void test_node(void)
{
    t_startup();
    t_outputs();
    t_hold();
    t_rules();
    t_usb();
    t_chips();
    t_comms();
    t_inputs_hold();
}
