/* drv_iso808.c - bank B: ST ISO808-1 (ISO808A-1), 8 x 1 A isolated high-side switch.
 *
 * Board (HW-08): 8 parallel inputs from controller pins, SYNC and LOAD from controller pins, the
 * open-drain FAULT pin pulled up to 3.3 V, the chip's watchdog input from a controller pin; its
 * outputs are also disabled by the hardware gate.
 * Still to read from the datasheet: how SYNC and LOAD latch the inputs (levels, order, timing),
 * how its watchdog must be serviced and how it shows that it has tripped, and whether a status
 * pin reports a missing field supply (FW-24, HW-06 open items). Until then (ISO808_CONFIRMED 0)
 * this driver is fail-safe: the inputs are held low, so bank B stays off whatever is commanded. */
#include "drivers.h"
#include "node_port.h"

bool iso808_fault(void *ctx) { (void)ctx; return port_bank_b_fault(); }     /* FW-41: FAULT pin, active low */
bool iso808_supply_ok(void *ctx) { (void)ctx; return port_bank_b_supply(); }

#if ISO808_CONFIRMED
#error "ISO808-1: fill in SYNC/LOAD and the watchdog from the datasheet before confirming it"
#else
void iso808_write(void *ctx, uint8_t o) { (void)ctx; (void)o; port_bank_b_inputs(0); }
bool iso808_wdg_tripped(void *ctx) { (void)ctx; return false; }
void iso808_wdg_service(void *ctx) { (void)ctx; }
#endif
