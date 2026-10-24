/* vcp_h5.c - the node's "USB" Modbus port (FW-56, usb_g0.h) on the NUCLEO-H563ZI: the ST-LINK
 * virtual COM port, USART3 (PD8 TX, PD9 RX), 115200 8N1. A frame ends after 1.75 ms of silence (the
 * USART's receiver timeout), as on RS-485. Interrupt driven; the reply is sent by polling (a few
 * ms at most), which is fine for a bench port.
 *
 * CubeMX: USART3 Asynchronous 115200 8N1, FIFO disabled, global interrupt enabled with
 * "Generate IRQ handler" unticked (this file defines USART3_IRQHandler). */
#include "usb_g0.h"
#include "main.h"

#ifndef NODE_VCP_USART
#define NODE_VCP_USART USART3
#endif
#ifndef NODE_VCP_IRQHandler
#define NODE_VCP_IRQHandler USART3_IRQHandler
#endif
#define V NODE_VCP_USART
#define VCP_BUF 256u

static uint8_t rxb[VCP_BUF];
static volatile uint16_t rxn;
static volatile bool rx_err, ready, busy;

void usb_node_rx(const uint8_t *buf, uint32_t len) { (void)buf; (void)len; }   /* no USB stack here */

void vcp_init(void);
void vcp_init(void)
{
    V->CR1 &= ~USART_CR1_UE;
    V->RTOR = 202u;                                    /* 1.75 ms at 115200 */
    V->CR2 |= USART_CR2_RTOEN;
    V->CR1 &= ~USART_CR1_FIFOEN;
    V->ICR = USART_ICR_ORECF | USART_ICR_FECF | USART_ICR_NECF | USART_ICR_PECF | USART_ICR_RTOCF;
    V->CR1 |= USART_CR1_RE | USART_CR1_TE | USART_CR1_RXNEIE_RXFNEIE | USART_CR1_RTOIE | USART_CR1_UE;
}

bool usb_take(const uint8_t **frame, uint16_t *len, bool *rx_error, uint32_t now)
{
    (void)now;
    if (!ready) return false;
    busy = true;
    *frame = rxb;
    *len = rxn;
    *rx_error = rx_err;
    return true;
}

void usb_done(const uint8_t *reply, uint16_t len)
{
    for (uint16_t i = 0; i < len; i++) {
        while (!(V->ISR & USART_ISR_TXE_TXFNF)) {}
        V->TDR = reply[i];
    }
    while (len && !(V->ISR & USART_ISR_TC)) {}
    __disable_irq();
    rxn = 0; rx_err = false; ready = false; busy = false;
    __enable_irq();
}

void NODE_VCP_IRQHandler(void);
void NODE_VCP_IRQHandler(void)
{
    uint32_t isr = V->ISR;
    if (isr & (USART_ISR_ORE | USART_ISR_FE | USART_ISR_NE | USART_ISR_PE)) {
        V->ICR = USART_ICR_ORECF | USART_ICR_FECF | USART_ICR_NECF | USART_ICR_PECF;
        if (!ready && !busy) rx_err = true;
    }
    if (isr & USART_ISR_RXNE_RXFNE) {
        uint8_t b = (uint8_t)V->RDR;
        if (!ready && !busy) {
            if (rxn < VCP_BUF) rxb[rxn++] = b;
            else rx_err = true;
        }
    }
    if (isr & USART_ISR_RTOF) {
        V->ICR = USART_ICR_RTOCF;
        if (rxn && !ready && !busy) ready = true;
    }
}
