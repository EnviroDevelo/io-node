/* test_temp.c - the temperature maths against NIST and IEC 60751 values, and the ADS124S08 driver against
 * the frame-level chip model (sim_ads124s08.c) with sensors on the three connectors. */
#include <math.h>
#include <string.h>
#include "drivers.h"
#include "host_port.h"
#include "iobus.h"
#include "sim_ads124s08.h"
#include "sim_chips.h"
#include "temp_conv.h"
#include "test.h"
#include "tc_nist_ref.h"

#define NEAR(a, b, tol) CHECK(fabs((double)(a) - (double)(b)) <= (tol))

static void t_thermocouples(void)
{
    /* the reference functions, against the generator's own evaluation of the NIST tables */
    for (unsigned i = 0; i < sizeof tc_ref / sizeof tc_ref[0]; i++) {
        NEAR(tc_emf_mv(tc_ref[i].type, tc_ref[i].t), tc_ref[i].mv, 1e-7);   /* J at 760: the two pieces meet within 0.1 nV */
        double t;
        CHECK_EQ(tc_temp_c(tc_ref[i].type, tc_ref[i].mv, 0.0, &t), 0);
        NEAR(t, tc_ref[i].t, 1e-4);
    }
    /* and against values printed in NIST's tables (mV, 3 decimals) */
    NEAR(tc_emf_mv(TC_K, 100), 4.096, 0.0005); NEAR(tc_emf_mv(TC_K, 500), 20.644, 0.0005);
    NEAR(tc_emf_mv(TC_K, 1000), 41.276, 0.0005); NEAR(tc_emf_mv(TC_J, 100), 5.269, 0.0005);
    NEAR(tc_emf_mv(TC_T, 100), 4.279, 0.0005); NEAR(tc_emf_mv(TC_E, 100), 6.319, 0.0005);
    NEAR(tc_emf_mv(TC_S, 1000), 9.587, 0.0005); NEAR(tc_emf_mv(TC_K, -100), -3.554, 0.0005);
    /* every type, every degree: the inverse finds the temperature back, with a cold junction */
    for (int ty = 0; ty < TC_N_TYPES; ty++) {
        double worst = 0;
        for (double t = tc_min_c(ty); t <= tc_max_c(ty); t += 1.0) {
            double r, mv = tc_emf_mv(ty, t) - tc_emf_mv(ty, 23.0);
            if (tc_temp_c(ty, mv, 23.0, &r) != 0) { worst = 99; break; }
            if (fabs(r - t) > worst) worst = fabs(r - t);
        }
        CHECK(worst < 1e-4);
    }
    double t;
    CHECK_EQ(tc_temp_c(TC_K, -6.0, 0.0, &t), -1);             /* below -200 degC */
    CHECK_EQ(tc_temp_c(TC_K, 60.0, 0.0, &t), 1);              /* above 1372 degC */
    CHECK_EQ(tc_temp_c(TC_T, 25.0, 0.0, &t), 1);
}

static void t_rtd_ntc(void)
{
    /* IEC 60751 table values (PT100) */
    NEAR(rtd_ohm(100, 0), 100.0, 1e-9); NEAR(rtd_ohm(100, 100), 138.5055, 1e-4);
    NEAR(rtd_ohm(100, -100), 60.2558, 1e-4); NEAR(rtd_ohm(100, 200), 175.8560, 1e-4);
    NEAR(rtd_ohm(100, -200), 18.5201, 1e-4); NEAR(rtd_ohm(100, 850), 390.4811, 1e-4);
    double worst = 0, t;
    for (double c = -200; c <= 850; c += 0.5) {
        if (rtd_temp_c(1000, rtd_ohm(1000, c), &t) != 0) { worst = 99; break; }
        if (fabs(t - c) > worst) worst = fabs(t - c);
    }
    CHECK(worst < 1e-5);
    CHECK_EQ(rtd_temp_c(100, 10.0, &t), -1);
    CHECK_EQ(rtd_temp_c(100, 400.0, &t), 1);
    /* NTC: R25 at 25 degC, and back */
    NEAR(ntc_ohm(10000, 3435, 25), 10000, 1e-6);
    NEAR(ntc_ohm(10000, 3435, 85), 10000 * exp(3435 * (1 / 358.15 - 1 / 298.15)), 1e-6);
    CHECK_EQ(ntc_temp_c(2700, 3900, ntc_ohm(2700, 3900, -12.5), &t), 0); NEAR(t, -12.5, 1e-6);
    CHECK_EQ(ntc_temp_c(10000, 3435, 1e6, &t), -1);
    CHECK_EQ(ntc_temp_c(10000, 3435, 10, &t), 1);
}

/* ---------------- the driver ---------------- */

static node_temp_t out;
static uint32_t now;

static void poll_ms(uint32_t ms)
{
    for (uint32_t i = 0; i < ms; i++) {
        now++;
        sads.now = now;
        ads124s08_poll(0, now, &out);
    }
}

static void until_rounds(unsigned k)
{
    uint16_t c0 = out.count;
    for (int i = 0; i < 20000 && (uint16_t)(out.count - c0) < k; i++) poll_ms(1);
}

static void start(const node_temp_cfg_t *c)
{
    sim_chips_reset();
    ads124s08_reset_state();
    memset(&out, 0, sizeof out);
    now = 5000;
    ads124s08_config(0, c);
}

static void t_ads_readings(void)
{
    node_temp_cfg_t c = { { IOB_TT_TC_K, IOB_TT_PT100_3W, IOB_TT_NTC }, { 1000, 1000, 1000 }, { 3435, 3435, 3435 } };
    start(&c);
    sads.die_c = 31.0;                                        /* the cold junction */
    sads.conn[0].kind = SC_TC;
    sads.conn[0].tc_mv[0] = tc_emf_mv(TC_K, 312.4) - tc_emf_mv(TC_K, 31.0);
    sads.conn[0].tc_mv[1] = tc_emf_mv(TC_K, -40.0) - tc_emf_mv(TC_K, 31.0);
    sads.conn[1].kind = SC_RTD3;
    sads.conn[1].ohm = rtd_ohm(100, 151.3);
    sads.conn[1].lead[0] = sads.conn[1].lead[1] = 2.5;        /* 3-wire: the lead resistance cancels */
    sads.conn[2].kind = SC_NTC;
    sads.conn[2].ohm = ntc_ohm(10000, 3435, 41.7);
    until_rounds(2);
    CHECK_EQ(sads.wrong_mode, 0); CHECK_EQ(sads.early_reads, 0); CHECK_EQ(sads.bad_mux, 0); CHECK_EQ(sads.bad_idac, 0);
    CHECK_EQ(out.flags, 0);
    CHECK_EQ(out.cj, 310);
    CHECK_EQ(out.st[0], IOB_TS_OK); CHECK_EQ(out.t[0], 3124);
    CHECK_EQ(out.st[1], IOB_TS_OK); CHECK_EQ(out.t[1], -400);
    CHECK_EQ(out.st[2], IOB_TS_OK); CHECK_EQ(out.t[2], 1513);
    CHECK_EQ(out.st[3], IOB_TS_OFF); CHECK_EQ(out.t[3], IOB_TEMP_INVALID);
    CHECK_EQ(out.st[4], IOB_TS_OK); CHECK_EQ(out.t[4], 417);
    CHECK_EQ(out.st[5], IOB_TS_OFF);
    /* a round is about a second: CJ, 2 x (burn-out + reading), RTD, NTC */
    uint32_t t0 = now; uint16_t c0 = out.count;
    until_rounds(1);
    CHECK(now - t0 > 500u && now - t0 < 1000u);
    CHECK_EQ((uint16_t)(out.count - c0), 1);
    CHECK_EQ(sads.starts % 7u, 0u);
    /* thermocouples have VBIAS on AINCOM (the 10M bias of AIN7/9/11) and their own minus pin where it can */
    node_temp_cfg_t c3 = { { IOB_TT_NONE, IOB_TT_NONE, IOB_TT_TC_J }, { 1000, 1000, 1000 }, { 3435, 3435, 3435 } };
    ads124s08_config(0, &c3);
    sads.conn[2].kind = SC_TC;
    sads.conn[2].tc_mv[0] = tc_emf_mv(TC_J, 650.0) - tc_emf_mv(TC_J, 31.0);
    until_rounds(2);
    CHECK_EQ(out.t[4], 6500); CHECK_EQ(out.st[0], IOB_TS_OFF);
    CHECK_EQ(sads.last_vbias, 0x40);                          /* AIN11: only AINCOM */
    CHECK_EQ(sads.last_pga, 0x0D);                            /* PGA on, x32 */
    /* type E reaches 76 mV at 1000 degC: x16 keeps it in range */
    node_temp_cfg_t ce = { { IOB_TT_TC_E, IOB_TT_NONE, IOB_TT_NONE }, { 1000, 1000, 1000 }, { 3435, 3435, 3435 } };
    ads124s08_config(0, &ce);
    sads.conn[0].tc_mv[0] = tc_emf_mv(TC_E, 990.0) - tc_emf_mv(TC_E, 31.0);
    sads.conn[0].tc_mv[1] = tc_emf_mv(TC_E, 20.0) - tc_emf_mv(TC_E, 31.0);
    until_rounds(2);
    CHECK_EQ(out.st[0], IOB_TS_OK); CHECK_EQ(out.t[0], 9900); CHECK_EQ(out.t[1], 200);
    CHECK_EQ(sads.last_vbias, 0x40 | 0x08);                   /* B- is AIN3: its own VB bit too */
}

static void t_ads_rtd_wiring(void)
{
    /* PT1000 2- and 4-wire, PT100 4-wire; the 2-wire reading includes its leads */
    node_temp_cfg_t c = { { IOB_TT_PT1000_2W, IOB_TT_PT1000_4W, IOB_TT_PT100_4W }, { 1000, 1000, 1000 }, { 3435, 3435, 3435 } };
    start(&c);
    for (int k = 0; k < 3; k++) { sads.conn[k].lead[0] = sads.conn[k].lead[1] = 0.5; }
    sads.conn[0].kind = SC_RTD2; sads.conn[0].ohm = rtd_ohm(1000, 20.0);
    sads.conn[1].kind = SC_RTD4; sads.conn[1].ohm = rtd_ohm(1000, 420.0);
    sads.conn[2].kind = SC_RTD4; sads.conn[2].ohm = rtd_ohm(100, -150.0);
    until_rounds(2);
    CHECK_EQ(out.st[0], IOB_TS_OK);
    CHECK_EQ(out.t[0], 203);                                  /* 1 ohm of leads on a PT1000: +0.26 degC */
    CHECK_EQ(out.t[2], 4200);
    CHECK_EQ(out.t[4], -1500);
    CHECK_EQ(sads.bad_idac, 0);
    /* broken and shorted sensors, and out of range */
    sads.conn[0].open = true;
    sads.conn[1].ohm = 20.0;                                  /* shorted at the sensor */
    sads.conn[2].ohm = 395.0;                                 /* above 850 degC */
    until_rounds(2);
    CHECK_EQ(out.st[0], IOB_TS_OPEN); CHECK_EQ(out.t[0], IOB_TEMP_INVALID);
    CHECK_EQ(out.st[2], IOB_TS_SHORT);
    CHECK_EQ(out.st[4], IOB_TS_OVER);
    sads.conn[0].open = false;
    until_rounds(1);
    CHECK_EQ(out.st[0], IOB_TS_OK);
}

static void t_ads_faults(void)
{
    node_temp_cfg_t c = { { IOB_TT_TC_K, IOB_TT_NTC, IOB_TT_NONE }, { 1000, 270, 1000 }, { 3435, 3900, 3435 } };
    start(&c);
    sads.die_c = 24.0;
    sads.conn[0].kind = SC_TC;
    sads.conn[0].tc_mv[0] = tc_emf_mv(TC_K, 100.0) - tc_emf_mv(TC_K, 24.0);
    sads.conn[0].tc_mv[1] = tc_emf_mv(TC_K, -210.0) - tc_emf_mv(TC_K, 24.0);   /* below the range */
    sads.conn[1].kind = SC_NTC;
    sads.conn[1].ohm = ntc_ohm(2700, 3900, 5.0);              /* the legacy 2.7k: 50 uA */
    until_rounds(2);
    CHECK_EQ(out.t[0], 1000); CHECK_EQ(out.st[1], IOB_TS_UNDER);
    CHECK_EQ(out.t[2], 50); CHECK_EQ(sads.last_idacmag, 0x02);
    /* an open thermocouple: the burn-out check finds it, the reading without it is not trusted */
    sads.conn[0].tc_open[0] = true;
    until_rounds(1);
    CHECK_EQ(out.st[0], IOB_TS_OPEN); CHECK_EQ(out.t[0], IOB_TEMP_INVALID);
    sads.conn[0].tc_open[0] = false;
    /* the NTC shorted, then open */
    sads.conn[1].ohm = 1.0;
    until_rounds(1);
    CHECK_EQ(out.st[2], IOB_TS_SHORT);
    sads.conn[1].open = true;
    until_rounds(1);
    CHECK_EQ(out.st[2], IOB_TS_OPEN);
    sads.conn[1].open = false; sads.conn[1].ohm = ntc_ohm(2700, 3900, 5.0);
    /* a damaged reply is read again; nothing is lost */
    until_rounds(1);
    sads.corrupt_replies = 1;
    until_rounds(1);
    CHECK_EQ(out.flags, 0); CHECK_EQ(out.st[0], IOB_TS_OK);
    /* a dead chip: every configured channel "converter fault", tried again every second */
    sads.dead = true;
    poll_ms(1500);
    CHECK(out.flags & IOB_TF_ADC);
    CHECK_EQ(out.st[0], IOB_TS_ADC); CHECK_EQ(out.st[2], IOB_TS_ADC); CHECK_EQ(out.st[4], IOB_TS_OFF);
    CHECK_EQ(out.t[0], IOB_TEMP_INVALID);
    uint32_t r0 = sads.resets;
    sads.dead = false;
    until_rounds(1);
    CHECK_EQ(out.flags, 0); CHECK_EQ(out.st[0], IOB_TS_OK);
    CHECK(sads.resets > r0);
    /* replies always damaged: a fault, not a wrong reading */
    sads.corrupt_replies = 1000000;
    poll_ms(1500);
    CHECK(out.flags & IOB_TF_ADC);
    sads.corrupt_replies = 0;
    until_rounds(1);
    CHECK_EQ(out.st[0], IOB_TS_OK);
    /* the isolated supply fails: switched off for a moment, then started again */
    hp.temp_supply_bad = true;
    poll_ms(50);
    CHECK(out.flags & IOB_TF_SUPPLY); CHECK_EQ(out.st[0], IOB_TS_ADC);
    CHECK_EQ(hp.temp_cycles, 1);
    CHECK(!hp.temp_off);
    hp.temp_supply_bad = false;
    until_rounds(1);
    CHECK_EQ(out.flags, 0); CHECK_EQ(out.st[0], IOB_TS_OK);
    /* the cold junction out of its range: thermocouples have no reading, the NTC still does */
    sads.die_c = 140.0;
    until_rounds(2);
    CHECK(out.flags & IOB_TF_CJ);
    CHECK_EQ(out.st[0], IOB_TS_CJ); CHECK_EQ(out.cj, IOB_TEMP_INVALID);
    CHECK_EQ(out.st[2], IOB_TS_OK);
}

static void t_ads_crc(void)
{
    /* CRC-8 x^8 + x^2 + x + 1, seed 0xFF: a single flipped bit is always caught */
    uint8_t d[4] = { 0x00, 0x12, 0x34, 0x56 };
    uint8_t c = ads124s08_crc8(d, 4);
    CHECK_EQ(c, sim_ads_crc8(d, 4));
    unsigned missed = 0;
    for (unsigned b = 0; b < 32; b++) { d[b / 8] ^= (uint8_t)(1u << (b % 8)); if (ads124s08_crc8(d, 4) == c) missed++; d[b / 8] ^= (uint8_t)(1u << (b % 8)); }
    CHECK_EQ(missed, 0);
    uint8_t ff[4] = { 0xFF, 0xFF, 0xFF, 0xFF };
    CHECK(ads124s08_crc8(ff, 4) != 0xFF);                     /* a bus stuck high fails the check */
}

void test_temp(void)
{
    t_thermocouples();
    t_rtd_ntc();
    t_ads_crc();
    t_ads_readings();
    t_ads_rtd_wiring();
    t_ads_faults();
}
