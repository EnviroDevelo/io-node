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
bool port_bank_b_fault(void) { return hp.b_fault; }
bool port_bank_b_pgood(void) { return hp.b_supply; }
void port_relays(uint8_t r) { hp.relays = r; }
void port_delay_us(uint32_t us) { hp.delay_us += us; }

int port_fram_read(uint8_t off, uint8_t *buf, uint8_t n)
{
    if (hp.fram_absent) return -1;
    for (unsigned i = 0; i < n; i++) buf[i] = hp.fram[(uint8_t)(off + i)];
    return 0;
}

int port_fram_write(uint8_t off, const uint8_t *buf, uint8_t n)
{
    if (hp.fram_absent) return -1;
    hp.fram_writes++;
    unsigned k = n;
    if (hp.fram_cut > 0) { k = (unsigned)hp.fram_cut - 1u < n ? (unsigned)hp.fram_cut - 1u : n; hp.fram_cut = 0; }
    for (unsigned i = 0; i < k; i++) hp.fram[(uint8_t)(off + i)] = buf[i];
    return k == n ? 0 : -1;
}

bool port_temp_supply_ok(void) { return !hp.temp_supply_bad && !hp.temp_off; }
void port_temp_enable(bool on) { if (!on && !hp.temp_off) hp.temp_cycles++; hp.temp_off = !on; }
void port_pwm(uint16_t duty, uint16_t hz, bool dir) { hp.pwm_duty = duty; hp.pwm_hz = hz; hp.pwm_dir = dir; }
uint32_t port_tach_period_us(void) { return hp.tach_us; }
