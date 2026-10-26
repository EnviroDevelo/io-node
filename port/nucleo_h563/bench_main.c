/* bench_main.c - the main loop of the NUCLEO-H563ZI bench board (board_nucleo.h). The same loop as
 * the product's port/stm32g0/node_main.c, on the bench board's functions, plus the Ethernet link
 * (net_bench.c: Modbus TCP and discovery). RS-485 stays: the supervisor uses one or the other.
 *
 * In CubeMX's main.c, USER CODE BEGIN 2 (after the MX_..._Init calls): node_main(); (never returns) */
#include "main.h"
#include "board_nucleo.h"
#include "node.h"
#include "net_bench.h"
#include "rs485_g0.h"
#include "usb_g0.h"

void vcp_init(void);

static node_t node;
static uint8_t tx[NODE_FRAME_MAX];

void node_main(void);
void node_main(void)
{
    node_boot_t b;
    board_boot_info(&b);
    board_iwdg_start();
    uint32_t now = HAL_GetTick();
    node_init(&node, bench_hw(), &b, now);
    rs485_init();
    vcp_init();
    net_bench_init(&node, b.uid);                       /* Ethernet: Modbus TCP and discovery */
    uint32_t last_pass = now;

    for (;;) {
        now = HAL_GetTick();
        const uint8_t *f;
        uint16_t len;
        bool err;
        if (rs485_take(&f, &len, &err)) {
            uint16_t r = node_frame(&node, NODE_PORT_RS485, now, f, len, err, tx);
            if (r) rs485_send(tx, r); else rs485_release();
        }
        if (usb_take(&f, &len, &err, now)) usb_done(tx, node_frame(&node, NODE_PORT_USB, now, f, len, err, tx));
        net_bench_poll(now);
        if (now == last_pass) continue;
        last_pass = now;

        node_poll(&node, now);
        if (node_healthy(&node)) board_healthy_toggle();
        board_iwdg_refresh();
        board_leds(node_comms_led(&node, now), node_fault_led(&node), node_heartbeat_led(now));
        if (node_boot_due(&node, now)) NVIC_SystemReset();      /* no bootloader on the bench board */
    }
}
