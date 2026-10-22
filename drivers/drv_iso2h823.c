/* drv_iso2h823.c - bank A: Infineon ISO2H823V2.5, 8 x 0.6 A isolated high-side switch, SPI.
 *
 * Board: MS0 = MS1 = 1 (mode 3: 24-bit register access with a 5-bit CRC), its own chip select.
 * Known so far (datasheet excerpt read 22 Oct, sections 2 and 4 only):
 *   - frame: 8-bit address byte (A6..A0 plus a write flag), 8 data bits, 3 dummy bits, CRC-5
 *   - CRC-5 polynomial x^5 + x^4 + x^2 + 1, register preset 11111, over the first 19 bits; the
 *     whole 24 bits checked at the receiver give 0
 *   - data sampled on SCLK rising, output changed on falling
 *   - registers named GLCFG, DRIVE (80h), DRIVE_RESYNCH, DIAG0..7, COLDIAG, GLERR (04h), INTERR,
 *     DIAGCFG; /ODIS low switches every output off and clears DRIVE
 * Still to read from the datasheet (section 6, tables 8, 14, 15, 24): every register's address,
 * bits and reset value, the read frame's reply timing, the undervoltage bits, the maximum SPI clock
 * and the SPI mode. Until then (ISO2H823_CONFIRMED 0) this driver is fail-safe: no answer. */
#include "drivers.h"
#include "node_port.h"

/* CRC-5 over the first nbits of msb-first data, as the datasheet describes (exposed for tests). */
uint8_t iso2h823_crc5(const uint8_t *data, unsigned nbits);
uint8_t iso2h823_crc5(const uint8_t *data, unsigned nbits)
{
    uint8_t crc = 0x1Fu;
    for (unsigned i = 0; i < nbits; i++) {
        unsigned bit = (data[i / 8u] >> (7u - i % 8u)) & 1u;
        unsigned fb = ((crc >> 4) & 1u) ^ bit;
        crc = (uint8_t)((crc << 1) & 0x1Fu);
        if (fb) crc ^= 0x15u;           /* x^4 + x^2 + 1 below the x^5 term */
    }
    return crc;
}

#if ISO2H823_CONFIRMED
#error "ISO2H823V2.5: fill in the register-level driver from the datasheet before confirming it"
#else
int iso2h823_config(void *ctx, uint8_t ol) { (void)ctx; (void)ol; return NODE_HW_NO_ANSWER; }
int iso2h823_write(void *ctx, uint8_t o) { (void)ctx; (void)o; return NODE_HW_NO_ANSWER; }
int iso2h823_diag(void *ctx, node_a_diag_t *d) { (void)ctx; (void)d; return NODE_HW_NO_ANSWER; }
#endif
