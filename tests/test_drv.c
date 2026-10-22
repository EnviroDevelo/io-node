/* test_drv.c - the chip drivers as built (DATASHEET STATUS in drivers/drivers.h). */
#include <string.h>
#include "drivers.h"
#include "host_port.h"
#include "mb_rtu.h"
#include "node.h"
#include "test.h"

uint8_t iso2h823_crc5(const uint8_t *data, unsigned nbits);

static void t_crc5(void)
{
    /* The receiver runs the CRC over all 24 bits, CRC included, and must get 0: append the CRC of
     * the first 19 bits as the last 5 bits of the frame and check that, for many frames. */
    unsigned bad = 0;
    for (unsigned v = 0; v < (1u << 19); v += 37u) {
        uint8_t f[3] = { (uint8_t)(v >> 11), (uint8_t)(v >> 3), (uint8_t)((v & 7u) << 5) };
        f[2] = (uint8_t)(f[2] | iso2h823_crc5(f, 19));
        if (iso2h823_crc5(f, 24) != 0) bad++;
    }
    CHECK_EQ(bad, 0);
    CHECK_EQ(iso2h823_crc5((const uint8_t *)"\0\0\0", 0), 0x1F);
}

/* With unconfirmed drivers the board must stay safe: chips "not answering", gate shut, bank B
 * inputs low, relays off, whatever the master writes. */
static void t_fail_safe(void)
{
    if (ISO2H823_CONFIRMED && ISO1I813T_CONFIRMED && ISO808_CONFIRMED) return;
    memset(&hp, 0, sizeof hp);
    hp.b_inputs = 0xFF; hp.relays = 3; hp.b_supply = true;
    node_t n;
    node_boot_t b = { 1, 1, NODE_RST_POWER_ON, 0, { 0 } };
    node_init(&n, drivers_hw(), &b, 0);
    uint8_t f[64], tx[NODE_FRAME_MAX];
    uint16_t w[3] = { 0xFFFF, 3, 1 };
    size_t len = mbr_req_rw(f, 1, IOB_R_STATUS, 12, 0, w, 3);
    CHECK(node_frame(&n, NODE_PORT_RS485, 1, f, (uint16_t)len, false, tx) > 0);
    for (uint32_t t = 2; t < 3000; t++) node_poll(&n, t);
    uint16_t st = node_reg(&n, IOB_R_STATUS);
    if (!ISO2H823_CONFIRMED) { CHECK(st & IOB_ST_OUT_A_CHIP); CHECK(!node_healthy(&n)); CHECK(!(st & IOB_ST_OUT_ENABLED)); }
    if (!ISO1I813T_CONFIRMED) { CHECK(st & IOB_ST_IN_CHIP); CHECK_EQ(node_reg(&n, IOB_R_INPUTS), 0); }
    if (!ISO808_CONFIRMED) CHECK_EQ(hp.b_inputs, 0);
    CHECK_EQ(hp.relays, 0);
    CHECK_EQ(node_reg(&n, IOB_R_OUT_APPLIED), 0);
}

void test_drv(void)
{
    t_crc5();
    t_fail_safe();
}
