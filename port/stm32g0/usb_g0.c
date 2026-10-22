/* usb_g0.c - see usb_g0.h. Built only when CubeMX's USB device (CDC) is in the project and
 * main.h defines NODE_USB (CUBEMX_SETUP.md). */
#include "usb_g0.h"
#include "main.h"

#ifdef NODE_USB
#include "usbd_cdc_if.h"

#define USB_BUF 256u
#define USB_GAP_MS 2u

static uint8_t rxb[USB_BUF], txb[USB_BUF];
static volatile uint16_t rxn;
static volatile uint32_t last_ms;
static volatile bool rx_err, busy;

void usb_node_rx(const uint8_t *buf, uint32_t len)
{
    if (busy) return;                                  /* FW-14: one request at a time */
    for (uint32_t i = 0; i < len; i++) {
        if (rxn < USB_BUF) rxb[rxn++] = buf[i];
        else rx_err = true;
    }
    last_ms = HAL_GetTick();
}

bool usb_take(const uint8_t **frame, uint16_t *len, bool *rx_error, uint32_t now)
{
    if (busy || rxn == 0 || (uint32_t)(now - last_ms) < USB_GAP_MS) return false;
    busy = true;
    *frame = rxb;
    *len = rxn;
    *rx_error = rx_err;
    return true;
}

void usb_done(const uint8_t *reply, uint16_t len)
{
    if (len && len <= USB_BUF) {
        for (uint16_t i = 0; i < len; i++) txb[i] = reply[i];
        (void)CDC_Transmit_FS(txb, len);               /* busy (PC not reading): the reply is dropped */
    }
    __disable_irq();
    rxn = 0; rx_err = false; busy = false;
    __enable_irq();
}

#else  /* no USB in this build */
void usb_node_rx(const uint8_t *buf, uint32_t len) { (void)buf; (void)len; }
bool usb_take(const uint8_t **frame, uint16_t *len, bool *rx_error, uint32_t now)
{
    (void)frame; (void)len; (void)rx_error; (void)now;
    return false;
}
void usb_done(const uint8_t *reply, uint16_t len) { (void)reply; (void)len; }
#endif
