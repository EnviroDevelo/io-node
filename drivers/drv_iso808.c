/* drv_iso808.c - bank B: ST ISO808A-1, 8 x 1 A isolated high-side switch, SPI.
 * Source: ISO808A / ISO808A-1 datasheet DS14170 Rev 6, July 2025 (sections 6.1-6.7, tables 9-10).
 *
 * The ISO808A-1 is an SPI device (not 8 parallel inputs with SYNC/LOAD, as the design spec's HW-08
 * assumed): pins SS, CLK, SDI, SDO, OUT_EN, FAULT and PGOOD.
 * Board: its own chip select on the node's SPI bus, OUT_EN from the safe-state gate (low turns
 * every output off and clears the output register, 6.5.2), FAULT and PGOOD (open drain, active
 * low) to controller pins with pull-ups.
 *
 * Frame (6.1, tables 14-15): 8 bits MSB first, CPOL 0 / CPHA 0, at most 20 MHz. SDI = IN7..IN0, the
 * channel states, taken on SS rising. SDO = all 0, or all 1 if a fault is present. SS must stay
 * high at least 20 us between frames (table 9, tc(SS)).
 * Watchdog (6.5.1): inside the chip, refreshed by its own logic side; it only trips when the
 * chip's logic supply is lost, so there is nothing for the controller to service (FW-24), and the
 * controller cannot see it trip: B_FAULT bit 1 (WDG) always reads 0 with this part.
 * FAULT (6.6): channel or case over-temperature, an SPI frame that is not a multiple of 8 bits, or
 * an isolation communication error. PGOOD (6.6, table 7): VCC below the power-good threshold
 * (off 14.5 to 16.1 V falling, on 16.6 to 18.7 V rising): this is FIELD_B_OK (HW-06).
 * Channel mapping: bit x-1 of the node's bank B byte = INx-1 = OUTx (assumed from table 14 and the
 * pinout; checked on the bench in AT-04). */
#include "drivers.h"
#include "node_port.h"

#define SPI_MODE 0u
#define SS_GAP_US 20u

bool iso808_fault(void *ctx) { (void)ctx; return port_bank_b_fault(); }      /* FW-41 */
bool iso808_supply_ok(void *ctx) { (void)ctx; return port_bank_b_pgood(); }  /* HW-06 */
bool iso808_wdg_tripped(void *ctx) { (void)ctx; return false; }
void iso808_wdg_service(void *ctx) { (void)ctx; }

#if ISO808_CONFIRMED
void iso808_write(void *ctx, uint8_t o)
{
    (void)ctx;
    uint8_t rx;
    (void)port_spi_xfer(PORT_CS_BANK_B, SPI_MODE, &o, &rx, 1);
    port_delay_us(SS_GAP_US);
}
#else
void iso808_write(void *ctx, uint8_t o) { (void)ctx; (void)o; }
#endif
