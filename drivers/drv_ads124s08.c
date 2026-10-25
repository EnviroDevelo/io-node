/* drv_ads124s08.c - the temperature inputs: TI ADS124S08 (24-bit delta-sigma ADC, 12 inputs, PGA, IDACs,
 * burn-out current sources) behind an ISOW7741 isolator, SPI mode 1 at most 4 MHz (H563 board rev A,
 * schematic "temperature" sheet).
 * Sources: register layout and command codes cross-checked against the Zephyr ads1x4s0x driver
 * (drivers/adc/adc_ads1x4s0x.c, Apache-2.0); the SYS register (SYS_MON, CRC, SENDSTAT), the data CRC
 * (CRC-8, x^8 + x^2 + x + 1, seed 0xFF, over STATUS and the three data bytes) and the internal
 * temperature sensor (129 mV at 25 degC, 403 uV/degC) are from the ADS124S08 datasheet (SBAS660) as
 * recalled, not yet checked against a copy: ADS124S08_CONFIRMED stays 0 until they are (drivers.h).
 *
 * Board wiring: three connectors TEMP 1-3, connector c on AIN4c..AIN4c+3 = A+, A-, B+, B-; the RTN pins
 * through 100R to REFP0, and REFP0 through 5.11k 0.1 % to GND_T (REFN0), so an IDAC's current sets the
 * reference for a ratiometric RTD reading. AIN7/9/11 have 10M to AINCOM (VBIAS has no VB bit for them),
 * so thermocouple readings drive VBIAS on AINCOM as well as on the minus pin where it has a VB bit.
 * DRDY is not wired and START is tied low: each conversion is started by command in single-shot mode
 * and read with RDATA once its conversion time (internal oscillator) has passed.
 *
 * A round of conversions, about one second:
 *   - the cold junction: the ADC's internal temperature sensor (SYS_MON 010), 20 SPS;
 *   - per thermocouple: a burn-out check (1 uA burn-out sources, 50 SPS: an open thermocouple drives
 *     the input to full scale) and then the reading without them (10 SPS, 50 and 60 Hz rejected);
 *   - per RTD: ratiometric against the 5.11k (PT100 250 uA, gain 8; PT1000 100 uA, PGA bypassed),
 *     2-, 3- (both IDACs, lead resistance cancelled) or 4-wire; an open RTD collapses the reference,
 *     caught by the reference monitor (FL_REF_L0, below 0.3 V) or a full-scale reading;
 *   - per NTC: IDAC 10 uA (R25 >= 5k) or 50 uA, against the internal 2.5 V reference, PGA bypassed
 *     (the IDAC's tolerance is taken out by the per-channel calibration on the supervisor).
 * A failed transfer (CRC, register read-back, ID) marks every configured channel IOB_TS_ADC and the
 * chip is reset and tried again every second. The front end's isolated supply fault (TEMP_EN_FLT low)
 * does the same, after switching the supply off for 20 ms. */
#include <math.h>
#include <string.h>
#include "drivers.h"
#include "iobus.h"
#include "node_port.h"
#include "temp_conv.h"

#define SPI_MODE 1u

/* commands and registers */
#define CMD_RESET 0x06u
#define CMD_START 0x08u
#define CMD_RDATA 0x12u
#define CMD_RREG  0x20u
#define CMD_WREG  0x40u
#define R_ID      0x00u
#define R_STATUS  0x01u
#define R_INPMUX  0x02u          /* INPMUX..SYS, 8 registers, written for each conversion */
#define ST_POR    0x80u
#define ST_REF_L0 0x01u
#define AINCOM    12u
#define VB_AINC   0x40u
#define IDAC_OFF  0xFu

/* PGA register: DELAY 000, PGA_EN (01 = on), GAIN = log2 */
#define PGA_ON(g) (uint8_t)(0x08u | (g))
#define PGA_BYPASS 0x00u
/* DATARATE: single-shot, low-latency filter, internal clock */
#define DR_10  0x32u
#define DR_20  0x34u
#define DR_50  0x35u
/* REF: internal 2.5 V (always on); REFP0/REFN0 with the positive buffer on, monitor L0 */
#define REF_INT 0x1Au
#define REF_P0  0x52u
/* SYS: SYS_MON, CAL_SAMP 10 (default), CRC on, SENDSTAT on */
#define SYS(mon) (uint8_t)(((mon) << 5) | 0x13u)
#define MON_OFF   0u
#define MON_TEMP  2u
#define MON_BO_1UA 6u
/* IDACMAG codes */
#define I_10UA  0x1u
#define I_50UA  0x2u
#define I_100UA 0x3u
#define I_250UA 0x4u

#define FS      8388608.0      /* 2^23 */
#define VREF_INT 2.5
#define R_REF   5110.0
#define RETRY_MS 1000u

enum { PH_START = 0, PH_RESET, PH_IDLE, PH_CONV, PH_DOWN };
enum { K_CJ = 0, K_BURN, K_MEAS };
typedef struct { uint8_t kind, conn, ch; } step_t;

static struct {
    node_temp_cfg_t cfg;
    bool     cfg_new;
    uint8_t  ph;
    uint32_t at;
    step_t   plan[1 + 3 * 4];
    uint8_t  n, k, reads;
    uint8_t  reg[8];
    bool     open[IOB_TEMPS];
    double   cj;
    bool     cj_ok;
    node_temp_t out;
} fe;

static bool due(uint32_t now, uint32_t at) { return (int32_t)(now - at) >= 0; }

uint8_t ads124s08_crc8(const uint8_t *d, unsigned n)
{
    uint8_t c = 0xFFu;
    for (unsigned i = 0; i < n; i++) {
        c ^= d[i];
        for (int b = 0; b < 8; b++) c = (uint8_t)((c & 0x80u) ? ((unsigned)c << 1) ^ 0x07u : (unsigned)c << 1);
    }
    return c;
}

static bool is_tc(uint8_t t) { return t >= IOB_TT_TC_K && t <= IOB_TT_TC_S; }
static bool is_rtd(uint8_t t) { return t >= IOB_TT_PT100_2W && t <= IOB_TT_PT1000_4W; }
static bool is_pt1000(uint8_t t) { return t >= IOB_TT_PT1000_2W && t <= IOB_TT_PT1000_4W; }
static bool is_3w(uint8_t t) { return t == IOB_TT_PT100_3W || t == IOB_TT_PT1000_3W; }
static bool known(uint8_t t) { return t == IOB_TT_NONE || is_tc(t) || is_rtd(t) || t == IOB_TT_NTC; }

/* channel states before the first reading of a round, and with no sensor */
static void reset_out(void)
{
    for (unsigned ch = 0; ch < IOB_TEMPS; ch++) {
        uint8_t t = fe.cfg.type[ch / 2u];
        bool used = known(t) && t != IOB_TT_NONE && (is_tc(t) || ch % 2u == 0);
        fe.out.t[ch] = IOB_TEMP_INVALID;
        fe.out.st[ch] = used ? IOB_TS_WAIT : IOB_TS_OFF;
    }
    fe.out.cj = IOB_TEMP_INVALID;
}

static void plan(void)
{
    fe.n = 0;
    bool any_tc = false;
    for (unsigned c = 0; c < IOB_TEMP_CONNS; c++) any_tc = any_tc || is_tc(fe.cfg.type[c]);
    if (any_tc) fe.plan[fe.n++] = (step_t){ K_CJ, 0, 0 };
    for (uint8_t c = 0; c < IOB_TEMP_CONNS; c++) {
        uint8_t t = fe.cfg.type[c];
        if (is_tc(t))
            for (uint8_t j = 0; j < 2; j++) {
                fe.plan[fe.n++] = (step_t){ K_BURN, c, (uint8_t)(2u * c + j) };
                fe.plan[fe.n++] = (step_t){ K_MEAS, c, (uint8_t)(2u * c + j) };
            }
        else if (is_rtd(t) || t == IOB_TT_NTC)
            fe.plan[fe.n++] = (step_t){ K_MEAS, c, (uint8_t)(2u * c) };
    }
    fe.k = 0;
}

void ads124s08_config(void *ctx, const node_temp_cfg_t *c)
{
    (void)ctx;
    fe.cfg = *c;
    for (unsigned k = 0; k < IOB_TEMP_CONNS; k++)          /* the node checks the ranges; never divide by 0 here */
        if (!known(fe.cfg.type[k]) || (fe.cfg.type[k] == IOB_TT_NTC && (!fe.cfg.ntc_r25[k] || !fe.cfg.ntc_beta[k])))
            fe.cfg.type[k] = IOB_TT_NONE;
    fe.cfg_new = true;
}

void ads124s08_reset_state(void) { memset(&fe, 0, sizeof fe); }

/* ---------------- one conversion's registers and result ---------------- */

static uint8_t tc_gain_log2(uint8_t t) { return t == IOB_TT_TC_E ? 4u : 5u; }   /* E reaches 76 mV: x16 */

static void regs_for(const step_t *s, uint8_t r[8])
{
    uint8_t t = fe.cfg.type[s->conn], a = (uint8_t)(4u * s->conn);
    uint8_t p = (uint8_t)(a + 2u * (s->ch % 2u)), m = (uint8_t)(p + 1u);    /* the pair's + and - */
    memset(r, 0, 8);
    r[5] = (uint8_t)(IDAC_OFF << 4 | IDAC_OFF);
    switch (s->kind) {
    case K_CJ:
        r[0] = (uint8_t)(AINCOM << 4 | AINCOM); r[1] = PGA_ON(0); r[2] = DR_20; r[3] = REF_INT;
        r[7] = SYS(MON_TEMP);
        break;
    case K_BURN:
    case K_MEAS:
        if (is_tc(t)) {
            r[0] = (uint8_t)(p << 4 | m);
            r[1] = PGA_ON(tc_gain_log2(t));
            r[2] = s->kind == K_BURN ? DR_50 : DR_10;
            r[3] = REF_INT;
            r[6] = (uint8_t)(VB_AINC | (m <= 5u ? 1u << m : 0u));
            r[7] = SYS(s->kind == K_BURN ? MON_BO_1UA : MON_OFF);
        } else if (is_rtd(t)) {                               /* sense A+/A-, excitation into B+ (and B-) */
            r[0] = (uint8_t)(a << 4 | (a + 1u));
            r[1] = is_pt1000(t) ? PGA_BYPASS : PGA_ON(3);
            r[2] = DR_10;
            r[3] = REF_P0;
            r[4] = is_pt1000(t) ? I_100UA : I_250UA;
            r[5] = (uint8_t)((is_3w(t) ? a + 3u : IDAC_OFF) << 4 | (a + 2u));
            r[7] = SYS(MON_OFF);
        } else {                                              /* NTC: A+ with B+, A- with RTN */
            r[0] = (uint8_t)(a << 4 | (a + 1u));
            r[1] = PGA_BYPASS;
            r[2] = DR_10;
            r[3] = REF_INT;
            r[4] = fe.cfg.ntc_r25[s->conn] >= 500u ? I_10UA : I_50UA;
            r[5] = (uint8_t)(IDAC_OFF << 4 | (a + 2u));
            r[7] = SYS(MON_OFF);
        }
        break;
    default: break;
    }
}

/* the conversion time in single-shot mode with the low-latency filter, with a margin */
static uint32_t conv_ms(uint8_t dr)
{
    switch (dr & 0x0Fu) {
    case 0x2: return 127u;
    case 0x4: return 65u;
    case 0x5: return 27u;
    default:  return 127u;
    }
}

static int16_t to_ds(double t) { return (int16_t)lround(t * 10.0); }

static void result(const step_t *s, uint8_t status, int32_t code)
{
    uint8_t t = fe.cfg.type[s->conn], ch = s->ch;
    double x = (double)code / FS;
    if (s->kind == K_CJ) {
        double v = x * VREF_INT, c = 25.0 + (v - 0.129) / 0.000403;
        fe.cj_ok = c > -40.0 && c < 125.0;
        fe.cj = c;
        fe.out.cj = fe.cj_ok ? to_ds(c) : IOB_TEMP_INVALID;
        if (fe.cj_ok) fe.out.flags &= (uint16_t)~IOB_TF_CJ; else fe.out.flags |= IOB_TF_CJ;
        return;
    }
    if (s->kind == K_BURN) { fe.open[ch] = x >= 0.98; return; }
    double tc = 0.0;
    int rng = 0;
    uint8_t st = IOB_TS_OK;
    if (is_tc(t)) {
        if (fe.open[ch]) st = IOB_TS_OPEN;
        else if (!fe.cj_ok) st = IOB_TS_CJ;
        else rng = tc_temp_c(t - IOB_TT_TC_K, x * VREF_INT * 1000.0 / (double)(1u << tc_gain_log2(t)), fe.cj, &tc);
    } else if (is_rtd(t)) {
        double r0 = is_pt1000(t) ? 1000.0 : 100.0;
        double ohm = x * R_REF * (is_3w(t) ? 2.0 : 1.0) / (is_pt1000(t) ? 1.0 : 8.0);
        if ((status & ST_REF_L0) || x >= 0.98) st = IOB_TS_OPEN;
        else if (ohm < 0.1 * r0) st = IOB_TS_SHORT;
        else rng = rtd_temp_c(r0, ohm, &tc);
    } else {
        double r25 = 10.0 * fe.cfg.ntc_r25[s->conn], beta = fe.cfg.ntc_beta[s->conn];
        double ohm = x * VREF_INT / (r25 >= 5000.0 ? 10e-6 : 50e-6);
        if (x >= 0.98) st = IOB_TS_OPEN;
        else if (ohm < 0.5 * ntc_ohm(r25, beta, NTC_MAX_C)) st = IOB_TS_SHORT;
        else rng = ntc_temp_c(r25, beta, ohm, &tc);
    }
    if (st == IOB_TS_OK && rng) st = rng < 0 ? IOB_TS_UNDER : IOB_TS_OVER;
    fe.out.st[ch] = st;
    fe.out.t[ch] = st == IOB_TS_OK ? to_ds(tc) : IOB_TEMP_INVALID;
}

/* ---------------- SPI ---------------- */

static bool xfer(const uint8_t *tx, uint8_t *rx, uint16_t n)
{
    return port_spi_xfer(PORT_CS_TEMP, SPI_MODE, tx, rx, n) == 0;
}

static bool cmd(uint8_t c)
{
    uint8_t rx;
    return xfer(&c, &rx, 1);
}

static bool rreg(uint8_t r, uint8_t *v, uint8_t n)
{
    uint8_t tx[2 + 8] = { (uint8_t)(CMD_RREG | r), (uint8_t)(n - 1u) }, rx[2 + 8];
    if (!xfer(tx, rx, (uint16_t)(2u + n))) return false;
    memcpy(v, rx + 2, n);
    return true;
}

static bool wreg(uint8_t r, const uint8_t *v, uint8_t n)
{
    uint8_t tx[2 + 8] = { (uint8_t)(CMD_WREG | r), (uint8_t)(n - 1u) }, rx[2 + 8];
    memcpy(tx + 2, v, n);
    if (!xfer(tx, rx, (uint16_t)(2u + n))) return false;
    uint8_t back[8];
    return rreg(r, back, n) && memcmp(back, v, n) == 0;
}

/* every configured channel without a reading, and the chip tried again in RETRY_MS */
static void adc_failed(uint32_t now, uint16_t why)
{
    reset_out();
    for (unsigned ch = 0; ch < IOB_TEMPS; ch++)
        if (fe.out.st[ch] != IOB_TS_OFF) fe.out.st[ch] = IOB_TS_ADC;
    fe.out.flags |= why;
    fe.ph = PH_START;
    fe.at = now + RETRY_MS;
}

/* ---------------- the measurement loop ---------------- */

/* the isolated supply reports a fault: off for 20 ms, then on and a second to start before the reset */
static void power_cycle(uint32_t now)
{
    port_temp_enable(false);
    adc_failed(now, IOB_TF_SUPPLY);
    fe.ph = PH_DOWN;
    fe.at = now + 20u;
}

void ads124s08_poll(void *ctx, uint32_t now, node_temp_t *out)
{
    (void)ctx;
#if !ADS124S08_CONFIRMED
    if (fe.cfg_new) { fe.cfg_new = false; plan(); }
    adc_failed(now, IOB_TF_ADC);                    /* fail-safe form: no SPI, every channel "converter fault" */
    *out = fe.out;
    return;
#endif
    if (fe.cfg_new) {                               /* a new configuration: start the round again */
        fe.cfg_new = false;
        plan();
        reset_out();
        if (fe.ph == PH_CONV) fe.ph = PH_IDLE;      /* the conversion under way is for the old settings */
    }
    bool sup = port_temp_supply_ok();
    if (!sup && (fe.ph == PH_RESET || fe.ph == PH_IDLE || fe.ph == PH_CONV)) power_cycle(now);
    switch (fe.ph) {
    case PH_DOWN:
        if (!due(now, fe.at)) break;
        port_temp_enable(true);
        fe.ph = PH_START;
        fe.at = now + RETRY_MS;                     /* the supply's start-up, then a reset */
        break;
    case PH_START:
        if (!due(now, fe.at)) break;
        if (!sup) { power_cycle(now); break; }     /* still failed: again, once a second */
        if (!cmd(CMD_RESET)) { adc_failed(now, IOB_TF_ADC); break; }
        fe.ph = PH_RESET;
        fe.at = now + 3u;                           /* 4096 clocks (1 ms) and a margin */
        break;
    case PH_RESET: {
        if (!due(now, fe.at)) break;
        uint8_t id[2], clr = 0;
        /* ADS124S08: DEV_ID 000; just after a reset FL_POR is set (a dead bus reads all 0 or all 1) */
        if (!rreg(R_ID, id, 2) || (id[0] & 7u) != 0u || !(id[1] & ST_POR) || !wreg(R_STATUS, &clr, 1)) {
            adc_failed(now, IOB_TF_ADC);
            break;
        }
        fe.out.flags &= (uint16_t)~(IOB_TF_ADC | IOB_TF_SUPPLY);
        reset_out();
        plan();
        fe.ph = PH_IDLE;
        break;
    }
    case PH_IDLE:
        if (fe.n == 0) break;
        regs_for(&fe.plan[fe.k], fe.reg);
        if (!wreg(R_INPMUX, fe.reg, 8) || !cmd(CMD_START)) { adc_failed(now, IOB_TF_ADC); break; }
        fe.ph = PH_CONV;
        fe.reads = 0;
        fe.at = now + conv_ms(fe.reg[2]);
        break;
    case PH_CONV: {
        if (!due(now, fe.at)) break;
        uint8_t tx[6] = { CMD_RDATA }, rx[6];
        if (!xfer(tx, rx, 6) || ads124s08_crc8(rx + 1, 4) != rx[5]) {
            if (++fe.reads >= 3u) adc_failed(now, IOB_TF_ADC);   /* one damaged read is tried again */
            else fe.at = now + 1u;
            break;
        }
        int32_t code = (int32_t)((uint32_t)rx[2] << 24 | (uint32_t)rx[3] << 16 | (uint32_t)rx[4] << 8) >> 8;
        result(&fe.plan[fe.k], rx[1], code);
        if (++fe.k >= fe.n) { fe.k = 0; fe.out.count++; }
        fe.ph = PH_IDLE;
        break;
    }
    default: break;
    }
    *out = fe.out;
}
