/* board_g0.h - the I/O board's controller functions outside the RS-485 port and the chips:
 * DIP switch, board revision straps, unique ID, reset cause, watchdog, HEALTHY, LEDs, USB VBUS,
 * and entering ST's USB bootloader. STM32G0B1CET6, register level plus the CubeMX pin labels. */
#ifndef BOARD_G0_H
#define BOARD_G0_H
#include <stdbool.h>
#include <stdint.h>
#include "node.h"

/* Call first thing in main(), before HAL_Init(): jumps to ST's bootloader if the previous
 * run asked for it (FW-57), with the independent watchdog still stopped after that reset. */
void board_early_boot_check(void);

void board_boot_info(node_boot_t *b);       /* DIP, straps, UID, reset cause, reset count (FW-02, FW-03) */
void board_iwdg_start(void);                /* about 100 ms (FW-23) */
void board_iwdg_refresh(void);
void board_healthy_toggle(void);            /* FW-22: main loop only */
void board_leds(bool comms, bool fault, bool heartbeat);
bool board_vbus(void);                      /* a PC is connected to the USB-C port */
/* Outputs are already off and HEALTHY stopped (node_boot_due): note the request in .noinit and
 * reset; board_early_boot_check() then starts the bootloader from a clean reset. */
void board_enter_bootloader(void) __attribute__((noreturn));

#endif
