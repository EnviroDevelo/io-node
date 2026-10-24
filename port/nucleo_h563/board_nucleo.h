/* board_nucleo.h - a NUCLEO-H563ZI as a bench stand-in for the I/O board (not the product: that is
 * the STM32G0B1 in port/stm32g0). The node core (node/node.c), the register map and the Modbus
 * slave are the product's own; only the board underneath differs:
 *   - RS-485: one USART in RS-485 mode (DE from the USART) to a 3.3 V transceiver module,
 *     the same receiver-timeout code as the product (port/stm32g0/rs485_g0.c)
 *   - the "USB-C port" (FW-56: address 247, set_address.py, at02.py --usb) is the ST-LINK virtual
 *     COM port, USART3 on PD8/PD9 (vcp_h5.c)
 *   - outputs, relays and inputs are GPIO pins (bench_hw.c); LD1 green = heartbeat, LD2 yellow =
 *     comms, LD3 red = fault
 *   - the address and serial number live in the TAMP backup registers: kept over a reset, lost when
 *     the board is powered off (then NODE_BENCH_ADDRESS, default 1; 0 = start as a new board)
 *   - no hardware gate; HEALTHY is a pin to look at with a scope; the bootloader request (register 37)
 *     only resets the board. HW_REV reads NODE_BENCH_HW_REV (90) so the supervisor shows a bench board.
 * CUBEMX_SETUP.md makes the CubeIDE project. */
#ifndef BOARD_NUCLEO_H
#define BOARD_NUCLEO_H
#include <stdbool.h>
#include <stdint.h>
#include "node.h"

#ifndef NODE_BENCH_ADDRESS
#define NODE_BENCH_ADDRESS 1u
#endif
#ifndef NODE_BENCH_HW_REV
#define NODE_BENCH_HW_REV 90u
#endif

void board_boot_info(node_boot_t *b);       /* address and serial (backup registers), UID, reset cause and count */
void board_iwdg_start(void);                /* about 100 ms (FW-23) */
void board_iwdg_refresh(void);
void board_healthy_toggle(void);
void board_leds(bool comms, bool fault, bool heartbeat);
bool bench_store_address(uint8_t a);        /* written and read back */
bool bench_store_serial(const uint8_t s[16]);
const node_hw_t *bench_hw(void);

#endif
