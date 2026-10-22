/* drv_iso1i813t.c - inputs: Infineon ISO1I813T, 8 isolated 24 V inputs, SPI.
 *
 * Board: SEL tied high (SPI), 8-bit frames, its own chip select.
 * Known so far (Apache NuttX driver drivers/ioexpander/iso1i813t.c, commit 77521cf):
 *   - registers DIAG 00h, INPDATA 02h, GLERR 04h, COEFIL0..7 06h..14h (one per input, 5 bits),
 *     INTERR 16h, GLCFG 18h
 *   - write: the address with bit 7 set, then the data byte, in one 16-bit transfer
 *   - read: the address byte, then a dummy byte; the register comes back in the second byte, and a
 *     one-byte transfer returns the input states
 * Still to read from the datasheet: COEFIL's coding (filter times, the wire-break enable),
 * GLERR / INTERR bits (field supply, internal faults), GLCFG, the SPI mode and clock. Until then
 * (ISO1I813T_CONFIRMED 0) this driver is fail-safe: no answer. */
#include "drivers.h"
#include "node_port.h"

#if ISO1I813T_CONFIRMED
#error "ISO1I813T: fill in the register-level driver from the datasheet before confirming it"
#else
int iso1i813t_config(void *ctx, uint8_t f, uint8_t wb) { (void)ctx; (void)f; (void)wb; return NODE_HW_NO_ANSWER; }
int iso1i813t_read(void *ctx, node_in_t *d) { (void)ctx; (void)d; return NODE_HW_NO_ANSWER; }
#endif
