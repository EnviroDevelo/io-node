/* node_port.h - what the chip drivers (drivers/) need from the controller: the SPI bus with two
 * chip selects, the bank B pins and a microsecond delay. Implemented for the STM32G0B1 in
 * port_stm32g0.c and, for the host tests, by the simulated chips in tests/. */
#ifndef NODE_PORT_H
#define NODE_PORT_H
#include <stdbool.h>
#include <stdint.h>

typedef enum { PORT_CS_BANK_A = 0, PORT_CS_INPUTS = 1 } port_cs_t;

/* One full-duplex SPI transfer of n bytes with the chip selected for its whole length.
 * spi_mode 0..3 (CPOL << 1 | CPHA), MSB first. Returns 0, or <0 on a bus timeout. */
int  port_spi_xfer(port_cs_t cs, uint8_t spi_mode, const uint8_t *tx, uint8_t *rx, uint16_t n);

/* bank B (ISO808-1): the 8 parallel inputs, SYNC, LOAD, the watchdog input and the FAULT pin */
typedef enum { PORT_B_SYNC = 0, PORT_B_LOAD, PORT_B_WD } port_bpin_t;
void port_bank_b_inputs(uint8_t outputs);      /* drive IN1..IN8 (bit 0 = IN1) */
void port_bank_b_pin(port_bpin_t pin, bool level);
bool port_bank_b_fault(void);                  /* the open-drain FAULT pin is active (low) */
bool port_bank_b_supply(void);                 /* opto-isolated supply sense, if fitted (true if not) */

/* relay drivers (MOSFET gates, behind the hardware gate) */
void port_relays(uint8_t relays);

void port_delay_us(uint32_t us);

#endif
