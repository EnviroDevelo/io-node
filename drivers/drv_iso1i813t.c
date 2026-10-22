/* drv_iso1i813t.c - inputs: Infineon ISO1I813T, 8 isolated 24 V inputs, SPI.
 * Source: ISO1I813T datasheet V2.2, 2017-01-03 (sections 3.4.2, 3.5, 3.6, 3.8, 3.10, 5.4 tables 11,
 * 16, 17, chapter 6).
 *
 * Board: SEL high (serial), MS0 = MS1 = 1 (SPI mode 3: 24-bit frames with CRC, as bank A), SSI tied
 * low and SSO unused (one SPI channel), SYNC high (continuous), DC_ENA low (VBB from the field
 * supply, so the reply's error bits are UV MV CF), ROSC 22.1 kOhm (the filter times below are for
 * its nominal 500 kHz scan).
 *
 * Frame (figure 27): 24 bits MSB first, CPOL 0 / CPHA 0 (SDI sampled on the rising edge, SDO
 * changed on the falling edge; table 16: at most 5 MHz).
 *   write: [1 A6..A0] [D7..D0]  [000 C4..C0]   -> reply [INPDATA] [DIAG]  [UV MV CF C4..C0]
 *   read:  [0 A6..A0] [00000000] [000 C4..C0]  -> reply [INPDATA] [value] [UV MV CF C4..C0]
 * CRC-5 as bank A (3.8.2, isoface_crc5.c). Every reply carries the filtered inputs. Gaps (table 16,
 * 17): /CS high 0.1 us between reads, 4.8 us between a write and reading it back, 4 us between two
 * filter writes; 2.3 us between reads of the same register.
 *
 * Registers (table 6-2): DIAG 00h (wire break per channel, sticky), INPDATA 02h, GLERR 04h,
 * COEFIL0..7 06h..14h (write 86h..94h), INTERR 16h, GLCFG 18h (write 98h).
 * COEFIL (6.3.4): bits 3..0 FT filter time, bit 4 MWB (1 = wire break reported; 0 = masked).
 *   FT 0..7 = 50 us, 100 us, 400 us, 800 us, 1.6 ms, 3.2 ms, 10 ms, 20 ms; 8..F = bypass.
 *   FILTER_MS maps to the nearest at or above (FW-30): 0 -> bypass, 1 -> 1.6 ms, 2-3 -> 3.2 ms,
 *   4-10 -> 10 ms, 11-20 -> 20 ms; above 20 ms the chip has nothing longer, so 20 ms.
 *   The default 8 ms becomes 10 ms: a 5 ms pulse is ignored and a 12 ms pulse seen (AT-13).
 * Reading DIAG in mode 3 clears the DIAG bits read and the sticky UV, MV, W4S once their conditions
 * have gone (table 6-3); reading INTERR clears TE.
 * Field supply A: UV (VBB below 15..17 V) or INTERR.W4S (sense side not ready).
 * Chip fault: INTERR.TE (isolation transmission error) in two reads in a row. */
#include "drivers.h"
#include "node_port.h"

#define R_DIAG    0x00u
#define R_GLERR   0x04u
#define R_COEFIL0 0x06u
#define R_INTERR  0x16u
#define R_GLCFG   0x18u
#define WR        0x80u
#define COEF_MWB  0x10u
#define FT_BYPASS 0x08u
#define ERR_UV    0x80u        /* reply byte 3, DC_ENA = 0: UV MV CF */
#define INTERR_TE  0x02u
#define INTERR_W4S 0x04u
#define SPI_MODE  0u
#define GAP_RD_US 3u
#define GAP_WR_US 5u

uint8_t iso1i813t_filter_code(uint8_t filter_ms);
uint8_t iso1i813t_filter_code(uint8_t ms)
{
    if (ms == 0) return FT_BYPASS;
    if (ms <= 1) return 0x04u;     /* 1.6 ms */
    if (ms <= 3) return 0x05u;     /* 3.2 ms */
    if (ms <= 10) return 0x06u;    /* 10 ms */
    return 0x07u;                  /* 20 ms */
}

#if ISO1I813T_CONFIRMED

static uint8_t te_seen;

static int frame(uint8_t b0, uint8_t b1, uint8_t rx[3], uint32_t gap_us)
{
    uint8_t tx[3] = { b0, b1, 0 };
    tx[2] = isoface_crc5(tx, 19);
    int rc = port_spi_xfer(PORT_CS_INPUTS, SPI_MODE, tx, rx, 3);
    port_delay_us(gap_us);
    if (rc) return NODE_HW_NO_ANSWER;
    if (isoface_crc5(rx, 19) != (rx[2] & 0x1Fu)) return NODE_HW_NO_ANSWER;
    return NODE_HW_OK;
}

static int rd(uint8_t reg, uint8_t rx[3]) { return frame((uint8_t)(reg & 0x7Fu), 0, rx, GAP_RD_US); }
static int wr(uint8_t reg, uint8_t v)
{
    uint8_t rx[3];
    return frame((uint8_t)(WR | reg), v, rx, GAP_WR_US);
}

/* FW-04: write the 8 filter registers, then read them back. */
int iso1i813t_config(void *ctx, uint8_t filter_ms, uint8_t wb_enable)
{
    (void)ctx;
    uint8_t rx[3], ft = iso1i813t_filter_code(filter_ms);
    if (rd(R_GLERR, rx) || rd(R_INTERR, rx)) return NODE_HW_NO_ANSWER;   /* clear the power-up flags */
    if (wr(R_GLCFG, 0)) return NODE_HW_NO_ANSWER;                         /* defaults: auto-clear DIAG */
    for (unsigned ch = 0; ch < 8u; ch++) {
        uint8_t coef = (uint8_t)(ft | ((wb_enable >> ch) & 1u ? COEF_MWB : 0u));
        if (wr((uint8_t)(R_COEFIL0 + 2u * ch), coef)) return NODE_HW_NO_ANSWER;
    }
    for (unsigned ch = 0; ch < 8u; ch++) {
        uint8_t want = (uint8_t)(ft | ((wb_enable >> ch) & 1u ? COEF_MWB : 0u));
        if (rd((uint8_t)(R_COEFIL0 + 2u * ch), rx)) return NODE_HW_NO_ANSWER;
        if ((rx[1] & 0x1Fu) != want) return NODE_HW_READBACK;
    }
    if (rd(R_GLCFG, rx)) return NODE_HW_NO_ANSWER;
    if (rx[1] & 0x1Cu) return NODE_HW_READBACK;
    te_seen = 0;
    return NODE_HW_OK;
}

int iso1i813t_read(void *ctx, node_in_t *d)
{
    (void)ctx;
    uint8_t rx[3], ie[3];
    if (rd(R_DIAG, rx)) return NODE_HW_NO_ANSWER;       /* inputs, wire breaks (cleared if gone), UV */
    if (rd(R_INTERR, ie)) return NODE_HW_NO_ANSWER;     /* W4S, TE (cleared) */
    d->states = rx[0];
    d->wirebreak = rx[1];
    d->supply_ok = !(rx[2] & ERR_UV) && !(ie[1] & INTERR_W4S);
    te_seen = (ie[1] & INTERR_TE) ? (uint8_t)(te_seen < 2u ? te_seen + 1u : 2u) : 0u;
    d->chip_fault = te_seen >= 2u;
    return NODE_HW_OK;
}

#else
int iso1i813t_config(void *ctx, uint8_t f, uint8_t wb) { (void)ctx; (void)f; (void)wb; return NODE_HW_NO_ANSWER; }
int iso1i813t_read(void *ctx, node_in_t *d) { (void)ctx; (void)d; return NODE_HW_NO_ANSWER; }
#endif
