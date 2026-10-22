/* node.c - see node.h. Requirement numbers (FW-nn) refer to "CEMS I/O Module Requirements:
 * Firmware and Hardware". Where the platform's simulated node (iob_node_sim.c) and this file
 * differ, it is listed in README.md ("Differences from the simulated node"). */
#include "node.h"
#include <string.h>
#include "mb_rtu.h"
#include "node_version.h"

static bool due(uint32_t now, uint32_t at) { return (int32_t)(now - at) >= 0; }
static uint16_t get16(const uint8_t *p) { return (uint16_t)((unsigned)p[0] << 8 | p[1]); }

static bool a_ok(const node_t *n) { return n->chip_a.configured && !n->chip_a.fault; }
static bool in_ok(const node_t *n) { return n->chip_in.configured && !n->chip_in.fault; }

/* Field supply A: both chips that can see it must say it is there; a chip that is not answering
 * has no say. With neither answering the supply is unknown, reported missing. */
static bool field_a_ok(const node_t *n)
{
    bool any = false, ok = true;
    if (a_ok(n))  { any = true; ok = ok && n->a.supply_ok; }
    if (in_ok(n)) { any = true; ok = ok && n->in.supply_ok; }
    return any && ok;
}

/* the hardware gate is open, and so outputs may be on, only while HEALTHY toggles (FW-22).
 * A bank A chip fault does not stop it (Gary, 22 Oct): OUT_A_CHIP is reported for the main
 * board's alarm, and bank B and the relays keep running. */
bool node_healthy(const node_t *n) { return n->gate && !n->boot_pending; }

bool node_master_in_control(const node_t *n, uint32_t now)
{
    return n->master_seen && (uint32_t)(now - n->last_master_ms) < (uint32_t)n->hold_ds * 100u;
}

bool node_boot_due(const node_t *n, uint32_t now) { return n->boot_pending && due(now, n->boot_at_ms); }

bool node_fault_led(const node_t *n)
{
    return n->chip_a.fault || n->chip_in.fault || !field_a_ok(n) || n->timeout_latched || n->b_fault;
}

bool node_addr_led_blink(const node_t *n, uint32_t now) { return n->address == 0 && (now / 250u) % 2u == 0; }

/* ---------------- outputs ---------------- */

/* Drive the chips from the commands, or everything off (FW-20, FW-21, FW-25, FW-26). */
static void apply_outputs(node_t *n)
{
    bool on = node_healthy(n);
    uint8_t a = on ? (uint8_t)n->out_cmd : 0u;
    uint8_t b = on ? (uint8_t)(n->out_cmd >> 8) : 0u;
    uint8_t r = on ? (uint8_t)(n->relay_cmd & 3u) : 0u;
    /* always written, even to a chip marked faulty: "off" should reach it if it can hear */
    if (n->hw->a_write(n->hw->ctx, a) != NODE_HW_OK && n->chip_a.configured && n->chip_a.fails < 255u)
        n->chip_a.fails++;
    n->hw->b_write(n->hw->ctx, b);
    n->hw->relays(n->hw->ctx, r);
    /* bank A counts as driven only while its chip is answering and healthy */
    n->out_applied = (uint16_t)((a_ok(n) ? a : 0u) | (n->b_wdg ? 0u : (unsigned)b << 8));
    n->relay_applied = r;
}

/* ---------------- chips ---------------- */

static void chip_failed(node_chip_t *c, uint32_t now)
{
    c->configured = false;
    c->fault = true;
    c->retry_ms = now + NODE_RETRY_MS;
}

static void config_a(node_t *n, uint32_t now)
{
    if (n->hw->a_config(n->hw->ctx, (uint8_t)n->ol_en) == NODE_HW_OK) {
        n->chip_a.configured = true;
        n->chip_a.fails = 0;
        n->chip_a.fault = false;           /* until a diagnostic read says otherwise */
        n->cfg_a_dirty = false;
        n->next_diag_ms = now;
    } else
        chip_failed(&n->chip_a, now);
}

static void config_in(node_t *n, uint32_t now)
{
    if (n->hw->in_config(n->hw->ctx, (uint8_t)n->filter_ms, (uint8_t)n->wb_en) == NODE_HW_OK) {
        n->chip_in.configured = true;
        n->chip_in.fails = 0;
        n->chip_in.fault = false;
        n->cfg_in_dirty = false;
        n->next_in_ms = now;
    } else
        chip_failed(&n->chip_in, now);
}

static void service_chips(node_t *n, uint32_t now)
{
    /* configure (FW-04), again after a failure every second, and after a settings change */
    if (!n->chip_a.configured ? due(now, n->chip_a.retry_ms) : n->cfg_a_dirty) config_a(n, now);
    if (!n->chip_in.configured ? due(now, n->chip_in.retry_ms) : n->cfg_in_dirty) config_in(n, now);

    if (n->chip_in.configured && due(now, n->next_in_ms)) {          /* FW-30 */
        n->next_in_ms = now + NODE_IN_PERIOD_MS;
        node_in_t d;
        if (n->hw->in_read(n->hw->ctx, &d) == NODE_HW_OK) {
            n->in = d;
            n->chip_in.fails = 0;
            n->chip_in.fault = d.chip_fault;
        } else if (++n->chip_in.fails >= 2u)                         /* FW-43 */
            chip_failed(&n->chip_in, now);
    }
    if (n->chip_a.configured && due(now, n->next_diag_ms)) {        /* FW-40 */
        n->next_diag_ms = now + NODE_DIAG_PERIOD_MS;
        node_a_diag_t d;
        if (n->hw->a_diag(n->hw->ctx, &d) == NODE_HW_OK) {
            n->a = d;
            n->chip_a.fails = 0;
            n->chip_a.fault = d.chip_fault;
        } else if (++n->chip_a.fails >= 2u)
            chip_failed(&n->chip_a, now);
    }
    if (n->chip_a.configured && n->chip_a.fails >= 2u) chip_failed(&n->chip_a, now);   /* failed DRIVE writes */

    /* bank B: FAULT pin debounced for 1 ms (FW-41), its watchdog state, its supply */
    bool raw = n->hw->b_fault_pin(n->hw->ctx);
    if (raw != n->b_fault_raw) { n->b_fault_raw = raw; n->b_fault_since = now; }
    if ((uint32_t)(now - n->b_fault_since) >= NODE_BFAULT_MS) n->b_fault = raw;
    n->b_wdg = n->hw->b_wdg_tripped(n->hw->ctx);
    n->field_b = n->hw->b_supply_ok(n->hw->ctx);
}

/* ---------------- start-up and main loop ---------------- */

void node_init(node_t *n, const node_hw_t *hw, const node_boot_t *boot, uint32_t now)
{
    memset(n, 0, sizeof *n);
    n->hw = hw;
    n->address = boot->dip_address <= 15u ? boot->dip_address : 0u;       /* FW-02 */
    n->hw_rev = boot->hw_rev;
    memcpy(n->uid, boot->uid, sizeof n->uid);
    n->resets = boot->resets;
    n->wdg_latched = boot->reset_cause == NODE_RST_IWDG;                   /* FW-03 */
    n->hold_ds = NODE_DEF_HOLD_DS;
    n->filter_ms = NODE_DEF_FILTER;
    n->wb_en = NODE_DEF_WB;
    n->ol_en = NODE_DEF_OL;
    n->config_needed = true;
    n->gate = false;                                                       /* FW-01 */
    n->last_write_ms = now;
    n->magic = IOB_MAGIC;
    n->map_version = IOB_MAP_VERSION;
    n->fw_version = NODE_FW_VERSION;
    n->fw_build = NODE_FW_BUILD;
    n->n_out_a = NODE_N_OUT_A;
    n->n_out_b = NODE_N_OUT_B;
    n->n_relays = NODE_N_RELAYS;
    n->n_inputs = NODE_N_INPUTS;
    n->chip_a.retry_ms = n->chip_in.retry_ms = now;
    n->b_fault_since = now;
    hw->b_write(hw->ctx, 0);
    hw->relays(hw->ctx, 0);
    config_a(n, now);                                                      /* FW-04 */
    config_in(n, now);
    service_chips(n, now);
    apply_outputs(n);
}

/* FW-21: the hold time runs out -> everything off, gate closed, TIMEOUT latched. Checked every
 * loop pass and before every frame, so no frame can act on an expired hold. */
static void hold_check(node_t *n, uint32_t now)
{
    if (n->gate && (uint32_t)(now - n->last_write_ms) >= (uint32_t)n->hold_ds * 100u) {
        n->gate = false;
        n->timeout_latched = true;
        apply_outputs(n);
    }
}

void node_poll(node_t *n, uint32_t now)
{
    hold_check(n, now);
    service_chips(n, now);
    apply_outputs(n);                       /* every pass: also rewrites bank A after any glitch */
    if (n->gate && !n->boot_pending) n->hw->b_wdg_service(n->hw->ctx);    /* FW-24 */
}

/* ---------------- registers ---------------- */

uint16_t node_reg(const node_t *n, uint16_t r)
{
    uint16_t st = 0;
    switch (r) {
    case IOB_R_OUT_CMD: return n->out_cmd;
    case IOB_R_RELAY_CMD: return n->relay_cmd;
    case IOB_R_CMD_SEQ: return n->seq;
    case IOB_R_STATUS:
        if (node_healthy(n)) st |= IOB_ST_OUT_ENABLED;
        if (n->timeout_latched) st |= IOB_ST_TIMEOUT;
        if (field_a_ok(n)) st |= IOB_ST_FIELD_A_OK;
        if (n->field_b) st |= IOB_ST_FIELD_B_OK;
        if (n->wdg_latched) st |= IOB_ST_WDG_RESET;
        if (n->config_needed) st |= IOB_ST_CONFIG_NEEDED;
        if (n->chip_in.fault) st |= IOB_ST_IN_CHIP;
        if (n->chip_a.fault) st |= IOB_ST_OUT_A_CHIP;
        return st;
    case IOB_R_INPUTS: return (field_a_ok(n) && in_ok(n)) ? n->in.states : 0u;          /* FW-32 */
    case IOB_R_OUT_APPLIED: return n->out_applied;
    case IOB_R_RELAY_APPLIED: return n->relay_applied;
    case IOB_R_A_OPEN: return a_ok(n) ? (uint16_t)(n->a.open & n->ol_en) : 0u;
    case IOB_R_A_OVERLOAD: return a_ok(n) ? n->a.overload : 0u;
    case IOB_R_A_OVERTEMP: return a_ok(n) ? n->a.overtemp : 0u;
    case IOB_R_B_FAULT: return (uint16_t)((n->b_fault ? IOB_BF_FAULT : 0u) | (n->b_wdg ? IOB_BF_WDG : 0u));
    case IOB_R_WIREBREAK: return in_ok(n) ? (uint16_t)(n->in.wirebreak & n->wb_en) : 0u;   /* FW-31 */
    case IOB_R_CMD_ECHO: return n->echo;
    case IOB_R_RX_ERRORS: return n->rx_errors;
    case IOB_R_RESETS: return n->resets;
    case IOB_R_HOLD_DS: return n->hold_ds;
    case IOB_R_FILTER_MS: return n->filter_ms;
    case IOB_R_WB_ENABLE: return n->wb_en;
    case IOB_R_OL_ENABLE: return n->ol_en;
    case IOB_R_MAGIC: return n->magic;
    case IOB_R_MAP_VERSION: return n->map_version;
    case IOB_R_HW_REV: return n->hw_rev;
    case IOB_R_FW_VERSION: return n->fw_version;
    case IOB_R_FW_BUILD: return n->fw_build;
    case IOB_R_N_OUT_A: return n->n_out_a;
    case IOB_R_N_OUT_B: return n->n_out_b;
    case IOB_R_N_RELAYS: return n->n_relays;
    case IOB_R_N_INPUTS: return n->n_inputs;
    default:
        if (r >= IOB_R_UID && r < IOB_R_UID + 6u) return n->uid[r - IOB_R_UID];
        return 0;                            /* reserved, FAULT_CLEAR and BOOTLOADER read 0 */
    }
}

static bool writable(uint16_t r) { return r <= IOB_R_CMD_SEQ || (r >= IOB_R_HOLD_DS && r <= IOB_R_FAULT_CLEAR); }

static bool value_ok(uint16_t r, uint16_t v)
{
    switch (r) {
    case IOB_R_RELAY_CMD: return v <= 3u;
    case IOB_R_HOLD_DS: return v >= 1u && v <= 6000u;
    case IOB_R_FILTER_MS: return v <= 100u;
    case IOB_R_WB_ENABLE: case IOB_R_OL_ENABLE: return v <= 255u;
    case IOB_R_FAULT_CLEAR: return v <= 1u;
    default: return true;
    }
}

/* The whole write is checked before anything is written (exception code, 0 = fine). The first
 * register in the request that fails decides the code, as in the simulated node. */
static uint8_t write_check(const node_t *n, node_port_t port, uint32_t now, uint16_t start, uint16_t cnt,
                           const uint8_t *data)
{
    bool touches_cmd = false;
    for (uint16_t i = 0; i < cnt; ++i) {
        uint16_t r = (uint16_t)(start + i), v = get16(data + 2u * i);
        if (r == IOB_R_BOOTLOADER) {                                         /* FW-57 */
            if (port != NODE_PORT_USB) return 2;
            if (v != IOB_BOOT_KEY) return 3;
            continue;
        }
        if (!writable(r)) return 2;
        if (!value_ok(r, v)) return 3;
        if (r <= IOB_R_CMD_SEQ) touches_cmd = true;
    }
    if (port == NODE_PORT_USB && touches_cmd && node_master_in_control(n, now)) return 4;   /* FW-56 */
    return 0;
}

static void write_apply(node_t *n, node_port_t port, uint32_t now, uint16_t start, uint16_t cnt, const uint8_t *data)
{
    bool w0 = false;
    for (uint16_t i = 0; i < cnt; ++i) {
        uint16_t r = (uint16_t)(start + i), v = get16(data + 2u * i);
        switch (r) {
        case IOB_R_OUT_CMD: n->out_cmd = v; w0 = true; break;
        case IOB_R_RELAY_CMD: n->relay_cmd = v; break;
        case IOB_R_CMD_SEQ: n->seq = v; break;
        case IOB_R_HOLD_DS: n->hold_ds = v; n->config_needed = false; break;
        case IOB_R_FILTER_MS:
            if (v != n->filter_ms) n->cfg_in_dirty = true;
            n->filter_ms = v; n->config_needed = false; break;
        case IOB_R_WB_ENABLE:
            if (v != n->wb_en) n->cfg_in_dirty = true;
            n->wb_en = v; n->config_needed = false; break;
        case IOB_R_OL_ENABLE:
            if (v != n->ol_en) n->cfg_a_dirty = true;
            n->ol_en = v; n->config_needed = false; break;
        case IOB_R_FAULT_CLEAR:                                               /* FW-44 */
            if (v) { n->timeout_latched = false; n->wdg_latched = false; }
            break;
        case IOB_R_BOOTLOADER:
            n->boot_pending = true;
            n->boot_at_ms = now + NODE_BOOT_DELAY_MS;
            break;
        default: break;
        }
    }
    if (w0) {
        n->last_write_ms = now;
        n->gate = true;
        n->writes0++;
        if (port == NODE_PORT_RS485) { n->master_seen = true; n->last_master_ms = now; }
    }
    apply_outputs(n);                        /* FW-20: on the chips before the echo */
    n->echo = n->seq;
}

/* ---------------- frames ---------------- */

static uint16_t finish(uint8_t *tx, uint16_t len)
{
    uint16_t c = mbr_crc16(tx, len);
    tx[len] = (uint8_t)c;
    tx[len + 1u] = (uint8_t)(c >> 8);
    return (uint16_t)(len + 2u);
}

static uint16_t exception(uint8_t *tx, uint8_t addr, uint8_t fc, uint8_t code)
{
    tx[0] = addr;
    tx[1] = (uint8_t)(fc | 0x80u);
    tx[2] = code;
    return finish(tx, 3);
}

static uint16_t read_regs(const node_t *n, uint8_t *tx, uint8_t addr, uint8_t fc, uint16_t start, uint16_t cnt)
{
    if (cnt == 0 || cnt > 125u) return exception(tx, addr, fc, 3);
    if ((uint32_t)start + cnt > NODE_REG_SPACE) return exception(tx, addr, fc, 2);
    tx[0] = addr;
    tx[1] = fc;
    tx[2] = (uint8_t)(2u * cnt);
    for (uint16_t i = 0; i < cnt; ++i) {
        uint16_t v = node_reg(n, (uint16_t)(start + i));
        tx[3u + 2u * i] = (uint8_t)(v >> 8);
        tx[4u + 2u * i] = (uint8_t)v;
    }
    return finish(tx, (uint16_t)(3u + 2u * cnt));
}

uint16_t node_frame(node_t *n, node_port_t port, uint32_t now, const uint8_t *rx, uint16_t len, bool rx_error,
                    uint8_t *tx)
{
    n->frames++;
    hold_check(n, now);
    /* FW-12: damaged frames are dropped and counted (RS-485 only: RX_ERRORS is about the bus) */
    if (rx_error || len < 4u || len > NODE_FRAME_MAX ||
        mbr_crc16(rx, len - 2u) != (uint16_t)(rx[len - 2u] | rx[len - 1u] << 8)) {
        if (port == NODE_PORT_RS485) n->rx_errors++;
        return 0;
    }
    uint8_t addr = rx[0];
    bool mine = port == NODE_PORT_RS485 ? (n->address != 0 && addr == n->address)          /* FW-02 */
                                        : (addr == NODE_USB_ADDRESS || (n->address != 0 && addr == n->address));
    if (!mine) return 0;                     /* another node, or a broadcast: never answered */
    n->act_pulses++;
    uint8_t fc = rx[1];
    uint16_t a = 0, c = 0;
    if (len >= 6u) { a = get16(rx + 2); c = get16(rx + 4); }
    uint16_t out = 0;
    switch (fc) {
    case 3: case 4:
        out = len == 8u ? read_regs(n, tx, addr, fc, a, c) : exception(tx, addr, fc, 3);
        break;
    case 6: {
        if (len != 8u) { out = exception(tx, addr, fc, 3); break; }
        uint8_t e = write_check(n, port, now, a, 1, rx + 4);
        if (e) { out = exception(tx, addr, fc, e); break; }
        write_apply(n, port, now, a, 1, rx + 4);
        memcpy(tx, rx, 6);
        out = finish(tx, 6);
        break;
    }
    case 16: {
        if (len < 9u || c == 0 || c > 123u || rx[6] != 2u * c || len != 9u + 2u * c) { out = exception(tx, addr, fc, 3); break; }
        uint8_t e = (uint32_t)a + c > NODE_REG_SPACE ? 2 : write_check(n, port, now, a, c, rx + 7);
        if (e) { out = exception(tx, addr, fc, e); break; }
        write_apply(n, port, now, a, c, rx + 7);
        memcpy(tx, rx, 6);
        out = finish(tx, 6);
        break;
    }
    case 23: {
        if (len < 13u) { out = exception(tx, addr, fc, 3); break; }
        uint16_t wa = get16(rx + 6), wc = get16(rx + 8);
        if (wc == 0 || wc > 121u || rx[10] != 2u * wc || len != 13u + 2u * wc || c == 0 || c > 125u) {
            out = exception(tx, addr, fc, 3);
            break;
        }
        uint8_t e = ((uint32_t)wa + wc > NODE_REG_SPACE || (uint32_t)a + c > NODE_REG_SPACE) ? 2
                    : write_check(n, port, now, wa, wc, rx + 11);
        if (e) { out = exception(tx, addr, fc, e); break; }
        write_apply(n, port, now, wa, wc, rx + 11);            /* FW: write before read */
        out = read_regs(n, tx, addr, fc, a, c);
        break;
    }
    default:
        out = exception(tx, addr, fc, 1);
    }
    if (out) n->replies++;
    return out;
}
