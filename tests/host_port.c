/* host_port.c - node_port.h on the host: records what the drivers drive. */
#include <string.h>
#include "host_port.h"

host_port_t hp;

int port_spi_xfer(port_cs_t cs, uint8_t mode, const uint8_t *tx, uint8_t *rx, uint16_t n)
{
    hp.spi_calls++;
    if (hp.spi) return hp.spi(cs, mode, tx, rx, n);
    if (rx) memset(rx, 0xFF, n);               /* nothing on the bus: MISO floats high */
    return 0;
}
void port_bank_b_inputs(uint8_t o) { hp.b_inputs = o; }
void port_bank_b_pin(port_bpin_t p, bool v) { hp.b_pin[p] = v; }
bool port_bank_b_fault(void) { return hp.b_fault; }
bool port_bank_b_supply(void) { return hp.b_supply; }
void port_relays(uint8_t r) { hp.relays = r; }
void port_delay_us(uint32_t us) { hp.delay_us += us; }
