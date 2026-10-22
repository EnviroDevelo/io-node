/* rs485_g0.h - the node's RS-485 port: one STM32G0 USART, register level, interrupts.
 *
 * FW-10: a frame ends after 1.75 ms of line silence (the USART's receiver timeout, 202 bit times).
 * FW-11: the reply is started by the main loop as soon as the frame is handled, so it begins
 *        1.75 ms after the request (the receiver timeout) plus the handling time, well under 3 ms.
 * FW-12: framing, noise and overrun errors mark the frame; the node core drops and counts it.
 * FW-13: the USART drives DE itself (CR3.DEM); RE is tied to DE on the board.
 * FW-14: while a frame waits to be handled, and while the reply is going out, received bytes are
 *        ignored, so each request is handled completely before the next is accepted.
 * FW-50: static 256-byte buffers. */
#ifndef RS485_G0_H
#define RS485_G0_H
#include <stdbool.h>
#include <stdint.h>

#define RS485_BUF 256u

void rs485_init(void);                       /* after MX_USARTx_UART_Init (baud rate, pins) */
/* A complete frame, if one is waiting: its bytes, length and whether an error was seen in it. */
bool rs485_take(const uint8_t **frame, uint16_t *len, bool *rx_error);
void rs485_release(void);                    /* no reply: listen again */
void rs485_send(const uint8_t *reply, uint16_t len);   /* reply; listens again after the last stop bit */
void rs485_irq(void);                        /* the USART's interrupt handler body */

#endif
