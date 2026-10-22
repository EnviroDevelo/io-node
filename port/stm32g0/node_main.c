/* node_main.c - the I/O board's main loop on the STM32G0B1 (no RTOS).
 *
 * In CubeMX's main.c:
 *     USER CODE BEGIN 1:   board_early_boot_check();     (before HAL_Init)
 *     USER CODE BEGIN 2:   node_main();                  (after the MX_..._Init calls; never returns)
 *
 * One pass per millisecond (HAL tick): node_poll (hold time, chips), HEALTHY, watchdog, LEDs.
 * Frames are handled as soon as they are complete, between passes, so a reply starts within
 * about 0.2 ms of the 1.75 ms end-of-frame silence (FW-11). HEALTHY and the watchdog are only
 * ever touched here (FW-22, FW-23): a stuck loop stops both. */
#include "main.h"
#include "board_g0.h"
#include "drivers.h"
#include "node.h"
#include "rs485_g0.h"
#include "usb_g0.h"

#define ACT_PULSE_MS 20u

static node_t node;
static uint8_t tx[NODE_FRAME_MAX];                   /* FW-50 */

void node_main(void);
void node_main(void)
{
    node_boot_t b;
    board_boot_info(&b);
    board_iwdg_start();                              /* FW-23 */
    uint32_t now = HAL_GetTick();
    node_init(&node, drivers_hw(), &b, now);         /* everything off, chips configured (FW-01, FW-04) */
    rs485_init();
    uint32_t last_pass = now, act_until = 0, act_seen = node.act_pulses;

    for (;;) {
        now = HAL_GetTick();
        const uint8_t *f;
        uint16_t len;
        bool err;
        if (rs485_take(&f, &len, &err)) {
            uint16_t r = node_frame(&node, NODE_PORT_RS485, now, f, len, err, tx);
            if (r) rs485_send(tx, r); else rs485_release();
        }
        if (usb_take(&f, &len, &err, now)) {
            uint16_t r = board_vbus() ? node_frame(&node, NODE_PORT_USB, now, f, len, err, tx) : 0u;
            usb_done(tx, r);
        }
        if (now == last_pass) continue;
        last_pass = now;

        node_poll(&node, now);
        if (node_healthy(&node)) board_healthy_toggle();          /* FW-22 */
        board_iwdg_refresh();                                     /* FW-23 */
        if (node.act_pulses != act_seen) { act_seen = node.act_pulses; act_until = now + ACT_PULSE_MS; }
        bool act = node.address == 0 ? node_addr_led_blink(&node, now) : (int32_t)(act_until - now) > 0;
        board_leds(act, node_fault_led(&node));                   /* FW-55, FW-02 */
        if (node_boot_due(&node, now)) board_enter_bootloader();  /* FW-57: outputs off for 100 ms first */
    }
}
