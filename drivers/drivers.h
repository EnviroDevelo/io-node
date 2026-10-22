/* drivers.h - the I/O board's chip drivers, assembled into the node_hw_t the core uses.
 *
 *   drv_iso2h823.c  bank A, Infineon ISO2H823V2.5, SPI mode 3 frames (24-bit with CRC-5)
 *   drv_iso1i813t.c inputs, Infineon ISO1I813T, SPI 8-bit frames
 *   drv_iso808.c    bank B, ST ISO808-1, 8 parallel inputs, SYNC, LOAD, watchdog, FAULT
 *
 * DATASHEET STATUS. A driver whose register-level details have not been checked against the
 * chip's datasheet is built in its fail-safe form: it reports "no answer" and drives nothing, so
 * the node reports OUT_A_CHIP / IN_CHIP and its gate never opens. Each driver is switched to its
 * real form by defining its *_CONFIRMED macro to 1 once its constants have been checked
 * (README.md, "Open items"). A product build (NODE_PRODUCT_BUILD) refuses to compile while any
 * driver is unconfirmed. */
#ifndef DRIVERS_H
#define DRIVERS_H
#include "node_hw.h"

#ifndef ISO2H823_CONFIRMED
#define ISO2H823_CONFIRMED 0
#endif
#ifndef ISO1I813T_CONFIRMED
#define ISO1I813T_CONFIRMED 0
#endif
#ifndef ISO808_CONFIRMED
#define ISO808_CONFIRMED 0
#endif

#if defined(NODE_PRODUCT_BUILD) && !(ISO2H823_CONFIRMED && ISO1I813T_CONFIRMED && ISO808_CONFIRMED)
#error "A chip driver is not yet checked against its datasheet (drivers/drivers.h): not a product build"
#endif

const node_hw_t *drivers_hw(void);

/* the individual drivers (node_hw_t signatures, ctx unused) */
int  iso2h823_config(void *ctx, uint8_t ol_enable);
int  iso2h823_write(void *ctx, uint8_t outputs);
int  iso2h823_diag(void *ctx, node_a_diag_t *d);
int  iso1i813t_config(void *ctx, uint8_t filter_ms, uint8_t wb_enable);
int  iso1i813t_read(void *ctx, node_in_t *d);
void iso808_write(void *ctx, uint8_t outputs);
bool iso808_fault(void *ctx);
bool iso808_wdg_tripped(void *ctx);
void iso808_wdg_service(void *ctx);
bool iso808_supply_ok(void *ctx);
void board_relays(void *ctx, uint8_t relays);

#endif
