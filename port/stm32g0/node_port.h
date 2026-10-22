/* node_port.h - what the chip drivers (drivers/) need from the controller: the SPI bus with three
 * chip selects, the bank B status pins, the relays and a microsecond delay. Implemented for the
 * STM32G0B1 in board_g0.c and, for the host tests, by the simulated chips in tests/. */
#ifndef NODE_PORT_H
#define NODE_PORT_H
#include <stdbool.h>
#include <stdint.h>

typedef enum { PORT_CS_BANK_A = 0, PORT_CS_INPUTS = 1, PORT_CS_BANK_B = 2 } port_cs_t;

/* One full-duplex SPI transfer of n bytes with the chip selected for its whole length (3 chips).
 * spi_mode 0..3 (CPOL << 1 | CPHA), MSB first. Returns 0, or <0 on a bus timeout. */
int  port_spi_xfer(port_cs_t cs, uint8_t spi_mode, const uint8_t *tx, uint8_t *rx, uint16_t n);

/* bank B (ISO808A-1): its open-drain FAULT and PGOOD pins (both active low) */
bool port_bank_b_fault(void);                  /* FAULT is active */
bool port_bank_b_pgood(void);                  /* PGOOD is inactive: field supply B is good */

/* relay drivers (MOSFET gates, behind the hardware gate) */
void port_relays(uint8_t relays);

void port_delay_us(uint32_t us);

/* I2C F-RAM (MB85RC04V / FM24CL04B, first 256 bytes used) holding the node's address (nvcfg.h).
 * A write releases the write-protect pin for its own length only; F-RAM has no write cycle, so it
 * is done when the transfer is. 0, or <0: no answer. */
int  port_fram_read(uint8_t offset, uint8_t *buf, uint8_t n);
int  port_fram_write(uint8_t offset, const uint8_t *buf, uint8_t n);

#endif
