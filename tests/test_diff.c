/* test_diff.c - the node core against the platform's simulated node (iob_node_sim.c), the
 * reference the master is tested with. Both get the same random frames at the same times, with
 * the same outside world, and every reply must be byte for byte the same, and every register
 * the same afterwards. Healthy chips only: with a chip fault the two differ on purpose
 * (README.md, "Differences from the simulated node"). */
#include <stdlib.h>
#include <string.h>
#include "iob_node_sim.h"
#include "mb_rtu.h"
#include "node.h"
#include "sim_hw.h"
#include "test.h"

uint32_t bsp_ms(void);
uint32_t bsp_ms(void) { return 0; }       /* iob_node_sim.c's bus uses it; not used here */

static uint32_t rnd(uint32_t n) { return (uint32_t)rand() % n; }

/* a register address that is usually interesting */
static uint16_t pick_reg(void)
{
    static const uint16_t hot[] = { 0, 1, 2, 16, 17, 18, 25, 26, 32, 33, 34, 35, 36, 37, 64, 69, 78, 79 };
    switch (rnd(4)) {
    case 0: return (uint16_t)rnd(90);
    default: return hot[rnd(sizeof hot / sizeof hot[0])];
    }
}
static uint16_t pick_val(uint16_t r)
{
    switch (rnd(6)) {
    case 0: return (uint16_t)rnd(65536);
    case 1: return 0;
    case 2: return r == IOB_R_HOLD_DS ? (uint16_t)(1 + rnd(30)) : (uint16_t)rnd(4);
    case 3: return (uint16_t)rnd(256);
    case 4: return IOB_BOOT_KEY;
    default: return (uint16_t)rnd(7000);
    }
}

static size_t make_frame(uint8_t *f, uint8_t addr)
{
    uint16_t v[125];
    size_t len = 0;
    uint16_t r = pick_reg(), c = (uint16_t)(1 + rnd(rnd(3) ? 4 : 130));
    switch (rnd(9)) {
    case 0: case 1: len = mbr_req_read(f, addr, r, c > 125 ? 125 : c); if (rnd(4) == 0 && len) f[1] = 4; break;
    case 2: case 3: len = mbr_req_write1(f, addr, r, pick_val(r)); break;
    case 4: {
        uint16_t k = c > 123 ? 123 : c;
        for (uint16_t i = 0; i < k; i++) v[i] = pick_val((uint16_t)(r + i));
        len = mbr_req_write(f, addr, r, v, k);
        break;
    }
    case 5: case 6: case 7: {
        uint16_t k = (uint16_t)(1 + rnd(4));
        uint16_t wr = rnd(3) ? 0 : pick_reg();
        for (uint16_t i = 0; i < k; i++) v[i] = pick_val((uint16_t)(wr + i));
        len = mbr_req_rw(f, addr, rnd(3) ? 16 : pick_reg(), rnd(3) ? 12 : c > 125 ? 125 : c, wr, v, k);
        break;
    }
    default:                                  /* odd frames: unknown function, wrong lengths, raw bytes */
        len = 2 + rnd(20);
        for (size_t i = 0; i < len; i++) f[i] = (uint8_t)rnd(256);
        f[0] = addr;
        if (rnd(2)) f[1] = (uint8_t)(rnd(2) ? 23 : 16);
        { uint16_t crc = mbr_crc16(f, len); f[len] = (uint8_t)crc; f[len + 1] = (uint8_t)(crc >> 8); len += 2; }
        break;
    }
    if (len == 0) return make_frame(f, addr);
    if (rnd(25) == 0) f[rnd((uint32_t)len)] ^= (uint8_t)(1u << rnd(8));   /* corrupted on the wire */
    if (rnd(30) == 0 && len > 3) len -= 1 + rnd(3);                       /* cut short */
    return len;
}

void test_diff(void)
{
    srand(20261022);
    const uint8_t ADDR = 2;
    iob_node_t ref;
    node_t me;
    sim_hw_t hw;
    iob_node_init(&ref, ADDR);
    sim_hw_init(&hw);
    node_boot_t b = { ADDR, 1, NODE_RST_POWER_ON, 0, { 0 } };
    for (int i = 0; i < 6; i++) b.uid[i] = ref.uid[i];
    uint32_t now = 0;
    node_init(&me, &hw.hw, &b, now);
    me.fw_version = ref.fw_version;            /* identity values the simulator makes up */
    me.fw_build = 1;
    me.resets = ref.resets;

    uint8_t f[300], t1[300], t2[NODE_FRAME_MAX];
    long frames = 0, replies = 0, mism_reply = 0, mism_reg = 0, exceptions = 0, timeouts = 0, gate_opened = 0;
    for (long step = 0; step < 200000; step++) {
        /* the outside world changes now and then, identically for both */
        if (rnd(200) == 0) {
            ref.inputs = hw.inputs = (uint8_t)rnd(256);
            ref.a_open = hw.a_open_raw = (uint8_t)rnd(256);
            ref.a_overload = hw.a_overload = (uint8_t)(rnd(4) ? 0 : rnd(256));
            ref.a_overtemp = hw.a_overtemp = (uint8_t)(rnd(4) ? 0 : rnd(256));
            ref.wirebreak = hw.wirebreak_raw = (uint8_t)rnd(256);
            ref.field_a = hw.field_a = rnd(5) != 0;
            ref.field_b = hw.field_b = rnd(5) != 0;
            hw.b_fault_pin = rnd(5) == 0;
            hw.b_wdg_tripped = rnd(8) == 0;
            ref.b_fault = (uint16_t)((hw.b_fault_pin ? IOB_BF_FAULT : 0) | (hw.b_wdg_tripped ? IOB_BF_WDG : 0));
            /* let the node's periodic reads catch up, as on the board */
            for (int k = 0; k < 10; k++) { now++; node_poll(&me, now); iob_node_tick(&ref, now); }
        }
        now += rnd(3) ? 20 : rnd(4000);
        node_poll(&me, now);
        iob_node_tick(&ref, now);
        bool was_open = me.gate;
        size_t len = make_frame(f, rnd(10) ? ADDR : (uint8_t)rnd(4));
        if (len > 256) continue;
        uint16_t r1 = iob_node_frame(&ref, now, f, (uint16_t)len, t1, sizeof t1);
        uint16_t r2 = node_frame(&me, NODE_PORT_RS485, now, f, (uint16_t)len, false, t2);
        frames++;
        if (r1) replies++;
        if (r1 >= 3 && (t1[1] & 0x80)) exceptions++;
        if (!was_open && me.gate) gate_opened++;
        if (r1 != r2 || memcmp(t1, t2, r1) != 0) {
            if (mism_reply++ < 5) {
                printf("  reply differs at step %ld (fc %u, len %zu): sim %u bytes, node %u bytes\n", step, f[1], len, r1, r2);
            }
        }
        for (uint16_t r = 0; r < NODE_REG_SPACE; r++) {
            uint16_t a = iob_node_reg(&ref, r), c = node_reg(&me, r);
            if (a != c) { if (mism_reg++ < 5) printf("  register %u differs at step %ld: sim %u, node %u\n", r, step, a, c); break; }
        }
        if (ref.timeout_latched && !(node_reg(&me, IOB_R_STATUS) & IOB_ST_TIMEOUT)) mism_reg++;
        if (ref.timeout_latched) timeouts++;
    }
    CHECK_EQ(mism_reply, 0);
    CHECK_EQ(mism_reg, 0);
    CHECK_EQ(me.rx_errors, ref.rx_errors);
    CHECK_EQ(me.writes0, ref.writes0);
    CHECK_EQ(me.replies, ref.replies);
    /* the random frames reached every corner that matters */
    CHECK(frames > 190000);
    CHECK(replies > 50000);
    CHECK(exceptions > 10000);
    CHECK(gate_opened > 100);
    CHECK(timeouts > 1000);
    CHECK(me.rx_errors > 1000);
    printf("  diff: %ld frames, %ld replies, %ld exceptions, %u writes of register 0, %u CRC/runt errors, gate opened %ld times\n",
           frames, replies, exceptions, (unsigned)me.writes0, (unsigned)me.rx_errors, gate_opened);
}
