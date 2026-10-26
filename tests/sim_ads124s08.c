/* sim_ads124s08.c - see sim_ads124s08.h. The circuit per connector is the board's (temperature sheet):
 * A+/A-/B+/B- on AIN4c..AIN4c+3, RTN through 100R to REFP0, 5.11k from REFP0 to GND (REFN0). */
#include <math.h>
#include <string.h>
#include "sim_ads124s08.h"

sim_ads_t sads;

#define R_REF 5110.0

static const uint8_t reset_regs[18] = { 0x00, 0x80, 0x01, 0x00, 0x14, 0x10, 0x00, 0xFF, 0x00, 0x10 };

uint8_t sim_ads_crc8(const uint8_t *d, unsigned n)
{
    uint8_t c = 0xFFu;
    for (unsigned i = 0; i < n; i++) {
        c ^= d[i];
        for (int b = 0; b < 8; b++) c = (uint8_t)((c & 0x80u) ? ((unsigned)c << 1) ^ 0x07u : (unsigned)c << 1);
    }
    return c;
}

void sim_ads_reset(void)
{
    memset(&sads, 0, sizeof sads);
    memcpy(sads.reg, reset_regs, sizeof reset_regs);
    sads.die_c = 25.0;
    sads.idac_err = 1.0;
}

static double idac_a(uint8_t imag)
{
    static const double ua[] = { 0, 10, 50, 100, 250, 500, 750, 1000, 1500, 2000 };
    return (imag & 0xFu) < 10u ? ua[imag & 0xFu] * 1e-6 * sads.idac_err : 0.0;
}

static double dr_ms(uint8_t dr)
{
    static const double sps[] = { 2.5, 5, 10, 16.6, 20, 50, 60, 100, 200, 400, 800, 1000, 2000, 4000, 4000, 4000 };
    return 1000.0 / sps[dr & 0xFu] + 1.0;              /* single-shot, low-latency filter: about one period */
}

/* the differential input voltage and the reference for the current settings */
static void convert(uint8_t *status, int32_t *code)
{
    uint8_t mux = sads.reg[2], pga = sads.reg[3], ref = sads.reg[5], imag = sads.reg[6], imux = sads.reg[7];
    uint8_t sys = sads.reg[9], mon = (uint8_t)(sys >> 5);
    uint8_t p = (uint8_t)(mux >> 4), n = (uint8_t)(mux & 0xFu), i1 = (uint8_t)(imux & 0xFu), i2 = (uint8_t)(imux >> 4);
    double gain = (pga & 0x18u) == 0x08u ? (double)(1u << (pga & 7u)) : 1.0;
    double v = 0.0, vref = 2.5, cur = idac_a(imag);
    bool sat = false;
    *status = 0;
    sads.last_vbias = sads.reg[8]; sads.last_pga = pga; sads.last_idacmag = imag;
    if (mon == 2) v = 0.129 + 0.000403 * (sads.die_c - 25.0);
    else if (p < 12 && n == p + 1u && p % 2u == 0u) {
        const sim_conn_t *c = &sads.conn[p / 4u];
        unsigned j = (p % 4u) / 2u, base = (p / 4u) * 4u;
        bool on1 = cur > 0 && i1 == base + 2u, on2 = cur > 0 && i2 == base + 3u;
        if ((cur > 0 && i1 != base + 2u && i1 < 12u) || (i2 < 12u && !on2)) sads.bad_idac++;
        double itot = 0.0;
        switch (c->kind) {
        case SC_TC: {
            if (cur > 0) sads.bad_idac++;
            double bo = mon == 5 ? 0.2e-6 : mon == 6 ? 1e-6 : mon == 7 ? 10e-6 : 0.0;
            if (c->tc_open[j]) { if (bo > 0) sat = true; else v = 0.0123; }   /* open, no burn-out: anything */
            else v = c->tc_mv[j] * 1e-3 + bo * 2010.0;
            break;
        }
        case SC_RTD2: case SC_RTD3: case SC_RTD4: case SC_NTC:
            if (j != 0) { sads.bad_mux++; break; }
            if (c->open) { sat = on1 || on2; break; }
            if (c->kind == SC_RTD3) {
                double ia = on1 ? cur : 0.0, ib = on2 ? cur : 0.0;
                v = ia * (c->lead[0] + c->ohm) - ib * c->lead[1];
                itot = ia + ib;
            } else {
                double ia = on1 ? cur : 0.0;
                v = ia * (c->ohm + (c->kind == SC_RTD4 ? 0.0 : c->lead[0] + c->lead[1]));
                itot = ia;
            }
            break;
        default: break;
        }
        if (((ref >> 2) & 3u) == 0u) {
            vref = itot * R_REF;
            if ((ref & 0xC0u) && vref < 0.3) *status |= 0x01u;   /* FL_REF_L0 */
        }
    } else sads.bad_mux++;
    if (sat || vref <= 0.0) { *code = sat ? 0x7FFFFF : 0; if (sat) sads.rails++; return; }
    double x = v * gain / vref * 8388608.0;
    if (x > 8388607.0) x = 8388607.0;
    if (x < -8388608.0) x = -8388608.0;
    *code = (int32_t)lround(x);
}

int sim_ads_frame(uint8_t mode, const uint8_t *tx, uint8_t *rx, uint16_t n)
{
    sads.frames++;
    if (mode != 1) sads.wrong_mode++;
    if (sads.dead) { memset(rx, 0xFF, n); return 0; }
    memset(rx, 0, n);
    if (sads.converting && (int32_t)(sads.now - sads.ready_at) >= 0) { sads.data = sads.pending; sads.converting = false; }
    uint8_t c = tx[0];
    if (c == 0x06) { memcpy(sads.reg, reset_regs, sizeof reset_regs); sads.data = 0; sads.converting = false; sads.resets++; }
    else if (c == 0x08) {
        uint8_t st;
        convert(&st, &sads.pending);
        sads.reg[1] = (uint8_t)((sads.reg[1] & 0x80u) | st);
        sads.converting = true;
        sads.ready_at = sads.now + (uint32_t)ceil(dr_ms(sads.reg[4]));
        sads.starts++;
    } else if (c == 0x12 && n >= 4) {
        if (sads.converting) sads.early_reads++;           /* the previous result: stale */
        uint8_t b[4] = { sads.reg[1], (uint8_t)(sads.data >> 16), (uint8_t)(sads.data >> 8), (uint8_t)sads.data };
        unsigned k = 1;
        if (sads.reg[9] & 1u) rx[k++] = b[0];
        rx[k++] = b[1]; rx[k++] = b[2]; rx[k++] = b[3];
        if ((sads.reg[9] & 2u) && k < n) rx[k] = sim_ads_crc8((sads.reg[9] & 1u) ? b : b + 1, (sads.reg[9] & 1u) ? 4u : 3u);
        if (sads.corrupt_replies > 0) { sads.corrupt_replies--; rx[3] ^= 0x04u; }
    } else if ((c & 0xE0u) == 0x20u && n >= 2) {
        unsigned a = c & 0x1Fu, cnt = tx[1] + 1u;
        for (unsigned i = 0; i < cnt && 2u + i < n && a + i < 18u; i++) rx[2 + i] = sads.reg[a + i];
    } else if ((c & 0xE0u) == 0x40u && n >= 2) {
        unsigned a = c & 0x1Fu, cnt = tx[1] + 1u;
        for (unsigned i = 0; i < cnt && 2u + i < n && a + i < 18u; i++) {
            if (a + i == 0u) continue;                             /* ID is read-only */
            if (a + i == 1u) sads.reg[1] = (uint8_t)(sads.reg[1] & (tx[2 + i] | 0x7Fu));   /* only FL_POR clears */
            else sads.reg[a + i] = tx[2 + i];
        }
    }
    return 0;
}
