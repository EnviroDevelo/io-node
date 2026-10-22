/* drv_iso2h823.c - bank A: Infineon ISO2H823V2.5, 8 x 0.6 A isolated high-side switch, SPI.
 * Source: ISO2H823V2.5 datasheet Rev 2.1, 2024-11-01 (sections 3.2.2-3.2.10, 3.3.3, 4.5, 5).
 *
 * Board: SEL high (serial), MS0 = MS1 = 1 (SPI mode 3 of the chip: 24-bit register access with
 * CRC), SYNC tied high (continuous mode, 3.2.5), ALE/RST tied low (no hardware reset, 3.2.9.1),
 * /ODIS from the safe-state gate (it switches every output off and clears DRIVE, 3.2.6).
 *
 * Frame (figure 15): 24 bits MSB first, SPI CPOL 0 / CPHA 0 (SDI sampled on the rising edge, SDO
 * changed on the falling edge, first bit valid after /CS falls; table 24: at most 6 MHz).
 *   write: [1 A6..A0] [D7..D0]  [000 C4..C0]
 *   read:  [0 A6..A0] [00000000] [000 C4..C0]   -> reply [COLDIAG] [value] [UV MV CF C4..C0]
 * CRC-5 x^5+x^4+x^2+1, preset 11111, over the first 19 bits (3.2.2.3). The reply carries its own
 * CRC over COLDIAG, the 8 middle bits and UV MV CF; a reply that fails it (or no chip: MISO stuck
 * high or low both fail) is "no answer". /CS high between frames at least 400 ns, and 2 us between
 * reads of the same register (table 22).
 *
 * Registers (table 29): DRIVE 00h, COLDIAG 02h, GLERR 04h, DIAGCFG 06h, DIAG0..7 08h..16h,
 * INTERR 18h, GLCFG 1Ah.
 *
 * Diagnostics are sticky (3.2.4): reading COLDIAG clears it and loads DIAG0..7 with what was seen
 * since the last read. Reading every 5 ms therefore gives the state over the last 5 ms, which is
 * what FW-40 asks for ("present state, not a latch"). The mapping onto the node's registers:
 *   A_OPEN     = OLA (open load, channel on) | OLI (open load, channel off)
 *   A_OVERLOAD = OCL (current limit) | SCV (short to VBB, channel off) - SCV only when no open load
 *                was seen in the same read: with the 12 nF output capacitor an unplugged valve
 *                also shows SCV briefly after switching off (table 10, note 1)
 *   A_OVERTEMP = OT
 * Field supply A: GLERR.UV (VBB below 16 V) or INTERR.W4P (power chip not ready). MV is not used:
 * the datasheet contradicts itself on its polarity (5.1.4 table says 1 = no missing voltage, 3.2.3
 * and the reset value 46h say 1 = missing), and UV (16 V) always trips before MV (13 V).
 * Chip fault: INTERR.OTP (package over-temperature, volatile) or INTERR.TE (isolation transmission
 * error, sticky) seen in two diagnostic reads in a row. */
#include "drivers.h"
#include "node_port.h"

#define R_DRIVE   0x00u
#define R_COLDIAG 0x02u
#define R_GLERR   0x04u
#define R_DIAGCFG 0x06u
#define R_DIAG0   0x08u
#define R_INTERR  0x18u
#define R_GLCFG   0x1Au
#define WR        0x80u

#define GLERR_VERS_MASK 0xF0u
#define GLERR_VERS      0x40u   /* Vers_3..0 = 0100 (5.1.4) */
#define GLERR_UV        0x04u
#define INTERR_TE       0x01u
#define INTERR_W4P      0x02u
#define INTERR_OTP      0x10u
#define GLCFG_OLOFF     0x02u
#define GLCFG_LEDGOFF   0x04u
#define DIAG_OT   0x01u
#define DIAG_OLI  0x02u
#define DIAG_OLA  0x04u
#define DIAG_OCL  0x08u
#define DIAG_SCV  0x10u

#define SPI_MODE  0u
#define GAP_US    2u            /* /CS high between frames (tCSD 400 ns; same-register reads 2 us) */

#if ISO2H823_CONFIRMED

static uint8_t te_seen;                 /* diagnostic reads in a row with TE set */

static int frame(uint8_t b0, uint8_t b1, uint8_t rx[3])
{
    uint8_t tx[3] = { b0, b1, 0 };
    tx[2] = isoface_crc5(tx, 19);
    int rc = port_spi_xfer(PORT_CS_BANK_A, SPI_MODE, tx, rx, 3);
    port_delay_us(GAP_US);
    if (rc) return NODE_HW_NO_ANSWER;
    if (isoface_crc5(rx, 19) != (rx[2] & 0x1Fu)) return NODE_HW_NO_ANSWER;
    return NODE_HW_OK;
}

static int rd(uint8_t reg, uint8_t *v)
{
    uint8_t rx[3];
    int rc = frame((uint8_t)(reg & 0x7Fu), 0, rx);
    if (rc == NODE_HW_OK) *v = rx[1];
    return rc;
}

static int wr(uint8_t reg, uint8_t v)
{
    uint8_t rx[3];
    return frame((uint8_t)(WR | reg), v, rx);
}

/* FW-04: configure and read back. OL checks: the chip's OLOFF switches open-load detection off for
 * all channels, used when no channel has its check on; per channel the node masks A_OPEN itself
 * (DIAGCFG would switch off every diagnostic of a channel, overload included, so it stays FFh). */
int iso2h823_config(void *ctx, uint8_t ol_enable)
{
    (void)ctx;
    uint8_t v, glcfg = (uint8_t)(GLCFG_LEDGOFF | (ol_enable ? 0u : GLCFG_OLOFF));
    if (rd(R_GLERR, &v)) return NODE_HW_NO_ANSWER;              /* also clears UV/MV after power-up */
    if ((v & GLERR_VERS_MASK) != GLERR_VERS) return NODE_HW_READBACK;
    if (rd(R_INTERR, &v)) return NODE_HW_NO_ANSWER;             /* clears TE/W4P/ALLOFF from power-up */
    if (wr(R_DIAGCFG, 0xFFu) || wr(R_GLCFG, glcfg)) return NODE_HW_NO_ANSWER;
    if (rd(R_DIAGCFG, &v)) return NODE_HW_NO_ANSWER;
    if (v != 0xFFu) return NODE_HW_READBACK;
    if (rd(R_GLCFG, &v)) return NODE_HW_NO_ANSWER;
    if ((v & 0xFEu) != glcfg) return NODE_HW_READBACK;           /* bit 0 SWRST clears itself */
    if (rd(R_COLDIAG, &v)) return NODE_HW_NO_ANSWER;            /* start the diagnostics afresh */
    te_seen = 0;
    return NODE_HW_OK;
}

int iso2h823_write(void *ctx, uint8_t outputs)
{
    (void)ctx;
    return wr(R_DRIVE, outputs);       /* channel x = OUTx = SWx; outputs bit 0 = node output 1 */
}

int iso2h823_diag(void *ctx, node_a_diag_t *d)
{
    (void)ctx;
    uint8_t cd, glerr, interr;
    if (rd(R_COLDIAG, &cd)) return NODE_HW_NO_ANSWER;           /* first: loads DIAG0..7 (3.2.5.2) */
    d->open = d->overload = d->overtemp = 0;
    for (unsigned ch = 0; ch < 8u; ch++) {
        if (!(cd & (1u << ch))) continue;
        uint8_t v;
        if (rd((uint8_t)(R_DIAG0 + 2u * ch), &v)) return NODE_HW_NO_ANSWER;
        uint8_t m = (uint8_t)(1u << ch);
        bool open = (v & (DIAG_OLA | DIAG_OLI)) != 0;
        if (open) d->open |= m;
        if ((v & DIAG_OCL) || ((v & DIAG_SCV) && !open)) d->overload |= m;
        if (v & DIAG_OT) d->overtemp |= m;
    }
    if (rd(R_GLERR, &glerr) || rd(R_INTERR, &interr)) return NODE_HW_NO_ANSWER;
    if ((glerr & GLERR_VERS_MASK) != GLERR_VERS) return NODE_HW_NO_ANSWER;   /* not the chip answering */
    d->supply_ok = !(glerr & GLERR_UV) && !(interr & INTERR_W4P);
    te_seen = (interr & INTERR_TE) ? (uint8_t)(te_seen < 2u ? te_seen + 1u : 2u) : 0u;
    d->chip_fault = (interr & INTERR_OTP) || te_seen >= 2u;
    return NODE_HW_OK;
}

#else
int iso2h823_config(void *ctx, uint8_t ol) { (void)ctx; (void)ol; return NODE_HW_NO_ANSWER; }
int iso2h823_write(void *ctx, uint8_t o) { (void)ctx; (void)o; return NODE_HW_NO_ANSWER; }
int iso2h823_diag(void *ctx, node_a_diag_t *d) { (void)ctx; (void)d; return NODE_HW_NO_ANSWER; }
#endif
