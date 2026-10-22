/* drivers.c - see drivers.h */
#include "drivers.h"
#include "node_port.h"
#include "nvcfg.h"

void board_relays(void *ctx, uint8_t r) { (void)ctx; port_relays(r & 3u); }

static const node_hw_t board = {
    0,
    iso2h823_config, iso2h823_write, iso2h823_diag,
    iso1i813t_config, iso1i813t_read,
    iso808_write, iso808_fault, iso808_wdg_tripped, iso808_wdg_service, iso808_supply_ok,
    board_relays,
    nvcfg_store_address,
};

const node_hw_t *drivers_hw(void) { return &board; }
