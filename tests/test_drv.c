/* test_drv.c - the chip drivers against the frame-level chip models (sim_chips.c), and the whole
 * node on the real drivers. */
#include <string.h>
#include "drivers.h"
#include "host_port.h"
#include "mb_rtu.h"
#include "node.h"
#include "sim_chips.h"
#include "test.h"


static void t_crc5(void)
{
    /* the receiver runs the CRC over all 24 bits and must get 0 */
    unsigned bad = 0;
    for (unsigned v = 0; v < (1u << 19); v += 37u) {
        uint8_t f[3] = { (uint8_t)(v >> 11), (uint8_t)(v >> 3), (uint8_t)((v & 7u) << 5) };
        f[2] = (uint8_t)(f[2] | isoface_crc5(f, 19));
        if (isoface_crc5(f, 24) != 0) bad++;
    }
    CHECK_EQ(bad, 0);
    CHECK_EQ(isoface_crc5((const uint8_t *)"\0\0\0", 0), 0x1F);
    /* a missing chip (MISO stuck high or low) never passes */
    const uint8_t hi[3] = { 0xFF, 0xFF, 0xFF }, lo[3] = { 0, 0, 0 };
    CHECK(isoface_crc5(hi, 19) != (hi[2] & 0x1Fu));
    CHECK(isoface_crc5(lo, 19) != (lo[2] & 0x1Fu));
}

static void t_iso2h823(void)
{
    node_a_diag_t d;
    sim_chips_reset();
    /* configuration: version checked, power-up flags cleared, LEDG off, OL on, all diagnostics on */
    CHECK_EQ(iso2h823_config(0, 0xFF), NODE_HW_OK);
    CHECK_EQ(sa.glcfg, 0x04);
    CHECK_EQ(sa.diagcfg, 0xFF);
    CHECK_EQ(sa.wrong_mode + sa.wrong_len + sa.bad_crc_in, 0);
    CHECK(hp.delay_us >= 2u * sa.frames);                        /* /CS high between frames */
    CHECK_EQ(iso2h823_config(0, 0x00), NODE_HW_OK);             /* no channel checked: OLOFF */
    CHECK_EQ(sa.glcfg, 0x06);
    CHECK_EQ(iso2h823_config(0, 0x01), NODE_HW_OK);
    /* healthy after configuration (power-up UV, TE, W4P were read away) */
    CHECK_EQ(iso2h823_diag(0, &d), NODE_HW_OK);
    CHECK(d.supply_ok); CHECK(!d.chip_fault);
    CHECK_EQ(d.open | d.overload | d.overtemp, 0);
    /* DRIVE */
    CHECK_EQ(iso2h823_write(0, 0xA5), NODE_HW_OK);
    CHECK_EQ(sa.drive, 0xA5);
    /* diagnostics, mapped as the driver header says */
    sa.cond[2] = SA_OLA;                       /* unplugged while on */
    sa.cond[1] = SA_OLI;                       /* unplugged while off */
    sa.cond[3] = SA_OCL;                       /* current limit */
    sa.cond[4] = SA_SCV;                       /* short to VBB */
    sa.cond[5] = SA_SCV | SA_OLI;              /* capacitor discharge with an open valve */
    sa.cond[6] = SA_OT;
    CHECK_EQ(iso2h823_diag(0, &d), NODE_HW_OK);
    CHECK_EQ(d.open, 0x26);
    CHECK_EQ(d.overload, 0x18);
    CHECK_EQ(d.overtemp, 0x40);
    /* present state: once the conditions are gone, the next read after one more is clear */
    memset(sa.cond, 0, sizeof sa.cond);
    CHECK_EQ(iso2h823_diag(0, &d), NODE_HW_OK);
    CHECK_EQ(iso2h823_diag(0, &d), NODE_HW_OK);
    CHECK_EQ(d.open | d.overload | d.overtemp, 0);
    /* a brief fault between reads is still seen once (sticky) */
    sa.cond[0] = SA_OCL;
    iso2h823_write(0, 0xA5);                   /* any frame lets the chip see it */
    sa.cond[0] = 0;
    CHECK_EQ(iso2h823_diag(0, &d), NODE_HW_OK);
    CHECK_EQ(d.overload, 0x01);
    CHECK_EQ(iso2h823_diag(0, &d), NODE_HW_OK);
    CHECK_EQ(d.overload, 0x00);
    /* field supply A: UV, then the power chip not ready */
    sa.uv = true;
    CHECK_EQ(iso2h823_diag(0, &d), NODE_HW_OK); CHECK(!d.supply_ok);
    sa.uv = false;                             /* sticky: shown once more, then clear */
    CHECK_EQ(iso2h823_diag(0, &d), NODE_HW_OK); CHECK(!d.supply_ok);
    CHECK_EQ(iso2h823_diag(0, &d), NODE_HW_OK); CHECK(d.supply_ok);
    sa.w4p = true;
    CHECK_EQ(iso2h823_diag(0, &d), NODE_HW_OK); CHECK(!d.supply_ok);
    sa.w4p = false;
    CHECK_EQ(iso2h823_diag(0, &d), NODE_HW_OK);
    CHECK_EQ(iso2h823_diag(0, &d), NODE_HW_OK); CHECK(d.supply_ok);
    /* chip fault: one transmission error is tolerated, persistent ones are a fault; OTP at once */
    sa.te_events = 1;
    CHECK_EQ(iso2h823_diag(0, &d), NODE_HW_OK); CHECK(!d.chip_fault);
    CHECK_EQ(iso2h823_diag(0, &d), NODE_HW_OK); CHECK(!d.chip_fault);
    sa.te_events = 1000;
    CHECK_EQ(iso2h823_diag(0, &d), NODE_HW_OK);
    CHECK_EQ(iso2h823_diag(0, &d), NODE_HW_OK); CHECK(d.chip_fault);
    sa.te_events = 0;
    CHECK_EQ(iso2h823_diag(0, &d), NODE_HW_OK);
    CHECK_EQ(iso2h823_diag(0, &d), NODE_HW_OK); CHECK(!d.chip_fault);
    sa.otp = true;
    CHECK_EQ(iso2h823_diag(0, &d), NODE_HW_OK); CHECK(d.chip_fault);
    sa.otp = false;
    /* a damaged reply, a missing chip, another chip */
    sa.corrupt_replies = 1;
    CHECK_EQ(iso2h823_write(0, 1), NODE_HW_NO_ANSWER);
    sa.dead = true;
    CHECK_EQ(iso2h823_config(0, 0xFF), NODE_HW_NO_ANSWER);
    CHECK_EQ(iso2h823_write(0, 1), NODE_HW_NO_ANSWER);
    CHECK_EQ(iso2h823_diag(0, &d), NODE_HW_NO_ANSWER);
    sa.dead = false;
    sa.version = 0x50;
    CHECK_EQ(iso2h823_config(0, 0xFF), NODE_HW_READBACK);
    CHECK_EQ(iso2h823_diag(0, &d), NODE_HW_NO_ANSWER);
    CHECK_EQ(sa.bad_crc_in + sa.wrong_mode + sa.wrong_len, 0);   /* every frame the driver sent was valid */
}

uint8_t iso1i813t_filter_code(uint8_t filter_ms);

static void t_iso1i813t(void)
{
    node_in_t d;
    /* FILTER_MS -> the nearest filter at or above (20 ms is the longest) */
    CHECK_EQ(iso1i813t_filter_code(0), 0x08);
    CHECK_EQ(iso1i813t_filter_code(1), 0x04);
    CHECK_EQ(iso1i813t_filter_code(2), 0x05);
    CHECK_EQ(iso1i813t_filter_code(3), 0x05);
    CHECK_EQ(iso1i813t_filter_code(4), 0x06);
    CHECK_EQ(iso1i813t_filter_code(8), 0x06);       /* the default 8 ms: 10 ms */
    CHECK_EQ(iso1i813t_filter_code(10), 0x06);
    CHECK_EQ(iso1i813t_filter_code(11), 0x07);
    CHECK_EQ(iso1i813t_filter_code(100), 0x07);
    sim_chips_reset();
    CHECK_EQ(iso1i813t_config(0, 8, 0x05), NODE_HW_OK);
    CHECK_EQ(si.coefil[0], 0x16); CHECK_EQ(si.coefil[1], 0x06); CHECK_EQ(si.coefil[2], 0x16);
    CHECK_EQ(si.coefil[7], 0x06);
    CHECK_EQ(si.bad_crc_in + si.wrong_mode + si.wrong_len, 0);
    /* power-up flags cleared by the configuration */
    CHECK_EQ(iso1i813t_read(0, &d), NODE_HW_OK);
    CHECK(d.supply_ok); CHECK(!d.chip_fault);
    si.states = 0xA5;
    CHECK_EQ(iso1i813t_read(0, &d), NODE_HW_OK);
    CHECK_EQ(d.states, 0xA5);
    /* wire break only where MWB is set, sticky until read with the break gone */
    si.wb = 0x07;
    CHECK_EQ(iso1i813t_read(0, &d), NODE_HW_OK);
    CHECK_EQ(d.wirebreak, 0x05);
    si.wb = 0;
    CHECK_EQ(iso1i813t_read(0, &d), NODE_HW_OK);
    CHECK_EQ(iso1i813t_read(0, &d), NODE_HW_OK);
    CHECK_EQ(d.wirebreak, 0);
    /* supply: UV, then the sense side not ready */
    si.uv = true;
    CHECK_EQ(iso1i813t_read(0, &d), NODE_HW_OK); CHECK(!d.supply_ok);
    si.uv = false;
    CHECK_EQ(iso1i813t_read(0, &d), NODE_HW_OK);
    CHECK_EQ(iso1i813t_read(0, &d), NODE_HW_OK); CHECK(d.supply_ok);
    si.w4s = true;
    CHECK_EQ(iso1i813t_read(0, &d), NODE_HW_OK); CHECK(!d.supply_ok);
    si.w4s = false;
    CHECK_EQ(iso1i813t_read(0, &d), NODE_HW_OK);
    CHECK_EQ(iso1i813t_read(0, &d), NODE_HW_OK); CHECK(d.supply_ok);
    /* transmission errors: one tolerated, persistent ones a fault */
    si.te_events = 2;
    CHECK_EQ(iso1i813t_read(0, &d), NODE_HW_OK); CHECK(!d.chip_fault);
    si.te_events = 1000;
    CHECK_EQ(iso1i813t_read(0, &d), NODE_HW_OK);
    CHECK_EQ(iso1i813t_read(0, &d), NODE_HW_OK); CHECK(d.chip_fault);
    si.te_events = 0;
    CHECK_EQ(iso1i813t_read(0, &d), NODE_HW_OK);
    CHECK_EQ(iso1i813t_read(0, &d), NODE_HW_OK); CHECK(!d.chip_fault);
    /* damaged reply, missing chip */
    si.corrupt_replies = 1;
    CHECK_EQ(iso1i813t_read(0, &d), NODE_HW_NO_ANSWER);
    si.dead = true;
    CHECK_EQ(iso1i813t_config(0, 8, 0), NODE_HW_NO_ANSWER);
    CHECK_EQ(iso1i813t_read(0, &d), NODE_HW_NO_ANSWER);
    si.dead = false;
    CHECK_EQ(si.bad_crc_in + si.wrong_mode + si.wrong_len, 0);
}

static void t_iso808a(void)
{
    sim_chips_reset();
    uint32_t d0 = hp.delay_us;
    iso808_write(0, 0x81);
    CHECK_EQ(sb.out, 0x81);
    CHECK_EQ(sb.frames, 1);
    CHECK_EQ(sb.wrong_mode + sb.wrong_len, 0);
    CHECK(hp.delay_us - d0 >= 20u);                              /* SS high 20 us between frames */
    hp.b_fault = true;  CHECK(iso808_fault(0));
    hp.b_fault = false; CHECK(!iso808_fault(0));
    hp.b_supply = false; CHECK(!iso808_supply_ok(0));
    hp.b_supply = true;  CHECK(iso808_supply_ok(0));
    CHECK(!iso808_wdg_tripped(0));                               /* not observable on this part */
}

/* The whole node on the real drivers: outputs reach both chips, open circuit on bank A shows up,
 * a dead bank A is only reported, inputs and wire breaks come through, the hold time ends it all. */
static void t_board(void)
{
    sim_chips_reset();
    node_t n;
    node_boot_t b = { 1, 1, NODE_RST_POWER_ON, 0, { 0 } };
    node_init(&n, drivers_hw(), &b, 0);
    CHECK(!(node_reg(&n, IOB_R_STATUS) & IOB_ST_OUT_A_CHIP));
    CHECK_EQ(sa.drive, 0); CHECK_EQ(sb.out, 0); CHECK_EQ(hp.relays, 0);
    uint8_t f[64], tx[NODE_FRAME_MAX];
    uint16_t w[3] = { 0x3C0F, 2, 1 };
    size_t len = mbr_req_rw(f, 1, IOB_R_STATUS, 12, 0, w, 3);
    CHECK(node_frame(&n, NODE_PORT_RS485, 1, f, (uint16_t)len, false, tx) > 0);
    CHECK_EQ(sa.drive, 0x0F); CHECK_EQ(sb.out, 0x3C); CHECK_EQ(hp.relays, 2);
    sa.cond[1] = SA_OLA;
    for (uint32_t t = 2; t < 20; t++) node_poll(&n, t);
    CHECK_EQ(node_reg(&n, IOB_R_A_OPEN), 0x02);
    CHECK_EQ(node_reg(&n, IOB_R_OUT_APPLIED), 0x3C0F);
    CHECK(node_reg(&n, IOB_R_STATUS) & IOB_ST_FIELD_B_OK);
    hp.b_supply = false;
    node_poll(&n, 21);
    CHECK(!(node_reg(&n, IOB_R_STATUS) & IOB_ST_FIELD_B_OK));
    hp.b_supply = true;
    /* bank A dies: reported, bank B and the relays carry on */
    sa.dead = true;
    for (uint32_t t = 22; t < 60; t++) node_poll(&n, t);
    uint16_t st = node_reg(&n, IOB_R_STATUS);
    CHECK(st & IOB_ST_OUT_A_CHIP);
    CHECK(st & IOB_ST_OUT_ENABLED);
    CHECK_EQ(sb.out, 0x3C); CHECK_EQ(hp.relays, 2);
    CHECK_EQ(node_reg(&n, IOB_R_OUT_APPLIED), 0x3C00);
    sa.dead = false;
    for (uint32_t t = 60; t < 1200; t++) node_poll(&n, t);
    CHECK(!(node_reg(&n, IOB_R_STATUS) & IOB_ST_OUT_A_CHIP));
    CHECK_EQ(sa.drive, 0x0F);                                   /* rewritten after the recovery */
    /* inputs, wire break where checked, the settings reaching the chip */
    si.states = 0x81;
    si.wb = 0x03;
    CHECK_EQ(node_reg(&n, IOB_R_STATUS) & IOB_ST_IN_CHIP, 0);
    len = mbr_req_write1(f, 1, IOB_R_WB_ENABLE, 0x01);
    CHECK(node_frame(&n, NODE_PORT_RS485, 1200, f, (uint16_t)len, false, tx) > 0);
    len = mbr_req_write1(f, 1, IOB_R_OUT_CMD, 0x3C0F);           /* the master keeps writing */
    CHECK(node_frame(&n, NODE_PORT_RS485, 1200, f, (uint16_t)len, false, tx) > 0);
    for (uint32_t t = 1200; t < 1220; t++) node_poll(&n, t);
    CHECK_EQ(node_reg(&n, IOB_R_INPUTS), 0x81);
    CHECK_EQ(node_reg(&n, IOB_R_WIREBREAK), 0x01);
    CHECK_EQ(si.coefil[0], 0x16); CHECK_EQ(si.coefil[1], 0x06);
    si.uv = true;
    for (uint32_t t = 1220; t < 1230; t++) node_poll(&n, t);
    CHECK_EQ(node_reg(&n, IOB_R_INPUTS), 0);                    /* FW-32 */
    CHECK(!(node_reg(&n, IOB_R_STATUS) & IOB_ST_FIELD_A_OK));
    si.uv = false;
    for (uint32_t t = 1230; t < 1240; t++) node_poll(&n, t);
    CHECK_EQ(node_reg(&n, IOB_R_INPUTS), 0x81);
    /* the hold time runs out: both banks driven off */
    for (uint32_t t = 1240; t <= 301201; t += 1) node_poll(&n, t);
    CHECK_EQ(sa.drive, 0); CHECK_EQ(sb.out, 0); CHECK_EQ(hp.relays, 0);
    CHECK_EQ(sa.bad_crc_in + sa.wrong_mode + sa.wrong_len + sb.wrong_mode + sb.wrong_len, 0);
    CHECK_EQ(si.bad_crc_in + si.wrong_mode + si.wrong_len, 0);
}

void test_drv(void)
{
    t_crc5();
    t_iso2h823();
    t_iso1i813t();
    t_iso808a();
    t_board();
}
