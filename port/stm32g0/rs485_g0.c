/* rs485_g0.c - see rs485_g0.h.
 *
 * CubeMX (port/stm32g0/CUBEMX_SETUP.md): NODE_RS485_USART (default USART1) in Asynchronous mode,
 * Hardware Flow Control (RS485) so its DE pin is an alternate function, 115200 baud, 8 bits,
 * no parity, 1 stop bit, FIFO mode disabled, its global interrupt enabled, and in the NVIC
 * "Code generation" tab "Generate IRQ handler" UNTICKED for it: this file defines the handler.
 * The RX pin needs a pull-up (the transceiver's RO is off while it transmits). */
#include "rs485_g0.h"
#include "main.h"

#ifndef NODE_RS485_USART
#define NODE_RS485_USART USART1
#endif
#ifndef NODE_RS485_IRQHandler
#define NODE_RS485_IRQHandler USART1_IRQHandler
#endif
#define U NODE_RS485_USART

#define RTO_BITS  202u   /* 1.75 ms at 115200 baud (201.6 bit times) */
#define DE_TIME   8u     /* DE assertion and de-assertion time, in 1/16 bit (half a bit) */

static uint8_t rxb[RS485_BUF], txb[RS485_BUF];
static volatile uint16_t rxn, txn, txi;
static volatile bool rx_err, ready, sending;

void rs485_init(void)
{
    U->CR1 &= ~USART_CR1_UE;
    U->RTOR = RTO_BITS;
    U->CR2 |= USART_CR2_RTOEN;
    U->CR3 = (U->CR3 & ~USART_CR3_DEP) | USART_CR3_DEM;                     /* FW-13: DE active high */
    U->CR1 = (U->CR1 & ~(USART_CR1_DEAT | USART_CR1_DEDT | USART_CR1_FIFOEN))
           | (DE_TIME << USART_CR1_DEAT_Pos) | (DE_TIME << USART_CR1_DEDT_Pos);
    U->ICR = USART_ICR_ORECF | USART_ICR_FECF | USART_ICR_NECF | USART_ICR_PECF | USART_ICR_RTOCF | USART_ICR_TCCF;
    rxn = 0; rx_err = false; ready = false; sending = false;
    U->CR1 |= USART_CR1_RE | USART_CR1_TE | USART_CR1_RXNEIE_RXFNEIE | USART_CR1_RTOIE | USART_CR1_UE;
}

bool rs485_take(const uint8_t **frame, uint16_t *len, bool *rx_error)
{
    if (!ready) return false;
    *frame = rxb;
    *len = rxn;
    *rx_error = rx_err;
    return true;
}

void rs485_release(void)
{
    __disable_irq();
    rxn = 0; rx_err = false; ready = false;
    __enable_irq();
}

void rs485_send(const uint8_t *reply, uint16_t len)
{
    if (len == 0 || len > RS485_BUF) { rs485_release(); return; }
    for (uint16_t i = 0; i < len; i++) txb[i] = reply[i];
    __disable_irq();
    txn = len; txi = 0; sending = true; ready = false;
    U->CR1 |= USART_CR1_TXEIE_TXFNFIE;
    __enable_irq();
}

void rs485_irq(void)
{
    uint32_t isr = U->ISR, cr1 = U->CR1;
    if (isr & (USART_ISR_ORE | USART_ISR_FE | USART_ISR_NE | USART_ISR_PE)) {
        U->ICR = USART_ICR_ORECF | USART_ICR_FECF | USART_ICR_NECF | USART_ICR_PECF;
        if (!ready && !sending) rx_err = true;                          /* FW-12 */
    }
    if (isr & USART_ISR_RXNE_RXFNE) {
        uint8_t b = (uint8_t)U->RDR;
        if (!ready && !sending) {                                       /* FW-14 */
            if (rxn < RS485_BUF) rxb[rxn++] = b;
            else rx_err = true;                                         /* longer than any frame */
        }
    }
    if (isr & USART_ISR_RTOF) {                                         /* FW-10: 1.75 ms of silence */
        U->ICR = USART_ICR_RTOCF;
        if (rxn && !ready && !sending) ready = true;
    }
    if ((cr1 & USART_CR1_TXEIE_TXFNFIE) && (isr & USART_ISR_TXE_TXFNF)) {
        if (txi < txn) U->TDR = txb[txi++];
        else {
            U->CR1 = (U->CR1 & ~USART_CR1_TXEIE_TXFNFIE) | USART_CR1_TCIE;
        }
    }
    if ((cr1 & USART_CR1_TCIE) && (isr & USART_ISR_TC)) {               /* last stop bit out: DE drops */
        U->ICR = USART_ICR_TCCF;
        U->CR1 &= ~USART_CR1_TCIE;
        rxn = 0; rx_err = false; sending = false;
    }
}

void NODE_RS485_IRQHandler(void);
void NODE_RS485_IRQHandler(void) { rs485_irq(); }
