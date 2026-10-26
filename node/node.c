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
    return n->chip_a.fault || n->chip_in.fault || !field_a_ok(n) || n->timeout_latched || n->b_fault || n->nv_fault;
}

bool node_comms_ok(const node_t *n, uint32_t now)
{
    return n->comms_seen && (uint32_t)(now - n->last_comms_ms) < NODE_COMMS_TIMEOUT_MS;
}

bool node_comms_led(const node_t *n, uint32_t now)
{
    if (n->address == 0) return true;                       /* FW-02: address not set, steady on */
    if (!n->comms_seen) return false;                       /* waiting for the master */
    if (!node_comms_ok(n, now)) return (now / 125u) % 2u == 0;            /* lost now: 4 Hz */
    if (n->comms_lost) { uint32_t t = now % 1000u; return t < 100u || (t >= 250u && t < 350u); } /* double */
    return now % 500u < 100u;                               /* healthy: a short flash twice a second */
}

bool node_heartbeat_led(uint32_t now) { return (now / 500u) % 2u == 0; }

/* FW-58: losing the link latches COMMS_LOST */
static void comms_check(node_t *n, uint32_t now)
{
    if (n->comms_seen && !node_comms_ok(n, now)) n->comms_lost = true;
}

/* FW-33: every change since the last RS-485 read noted. Without field supply A or a healthy
 * input chip the inputs read 0 at once (FW-32), pending changes or not: a stale "closed" must
 * never be shown while the input cannot be read. */
static void in_track(node_t *n)
{
    if (field_a_ok(n) && in_ok(n)) n->in_pend |= (uint8_t)(n->in.states ^ n->in_rep);
    else n->in_pend = n->in_rep;                              /* shows 0 */
}

/* ---------------- outputs ---------------- */

/* Drive the chips from the commands, or everything off (FW-20, FW-21, FW-25, FW-26). */
static void apply_outputs(node_t *n)
{
    bool on = node_healthy(n);
    uint8_t a = on ? (uint8_t)n->out_cmd : 0u;
    uint8_t b = on ? (uint8_t)(n->out_cmd >> 8) : 0u;
    uint8_t r = on ? (uint8_t)(n->relay_cmd & 3u) : 0u;
    n->pwm_applied = on ? n->pwm_cmd : 0u;
    if (n->hw->pwm) n->hw->pwm(n->hw->ctx, n->pwm_applied, n->pwm_hz, on && (n->pwm_dir & 1u));
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
    n->address = boot->address <= IOB_ADDR_MAX ? boot->address : 0u;    /* FW-02, FW-60 */
    n->nv_fault = !boot->nv_ok;
    memcpy(n->serial, boot->serial, sizeof n->serial);
    n->hw_rev = boot->hw_rev;
    memcpy(n->uid, boot->uid, sizeof n->uid);
    n->resets = boot->resets;
    n->wdg_latched = boot->reset_cause == NODE_RST_IWDG;                   /* FW-03 */
    n->hold_ds = NODE_DEF_HOLD_DS;
    n->filter_ms = NODE_DEF_FILTER;
    n->wb_en = NODE_DEF_WB;
    n->ol_en = NODE_DEF_OL;
    n->pwm_hz = NODE_DEF_PWM_HZ;
    n->tach_ppr = NODE_DEF_PPR;
    for (unsigned k = 0; k < IOB_TEMP_CONNS; k++) {
        n->temp_cfg.ntc_r25[k] = NODE_DEF_NTC_R25;
        n->temp_cfg.ntc_beta[k] = NODE_DEF_NTC_BETA;
    }
    for (unsigned k = 0; k < IOB_TEMPS; k++) { n->temp.t[k] = IOB_TEMP_INVALID; n->temp.st[k] = IOB_TS_OFF; }
    n->temp.cj = IOB_TEMP_INVALID;
    n->temp_cfg_dirty = true;
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
    n->n_temp_pwm = (uint16_t)((hw->temp_poll ? IOB_TEMPS : 0u) | (hw->pwm ? 1u << 8 : 0u));
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
    comms_check(n, now);
    service_chips(n, now);
    in_track(n);
    if (n->hw->temp_poll) {
        if (n->temp_cfg_dirty && n->hw->temp_config) n->hw->temp_config(n->hw->ctx, &n->temp_cfg);
        n->temp_cfg_dirty = false;
        n->hw->temp_poll(n->hw->ctx, now, &n->temp);
    }
    apply_outputs(n);                       /* every pass: also rewrites bank A after any glitch */
    if (n->gate && !n->boot_pending) n->hw->b_wdg_service(n->hw->ctx);    /* FW-24 */
}

/* ---------------- registers ---------------- */

static bool has_temp(const node_t *n) { return n->hw->temp_poll != 0; }
static bool has_pwm(const node_t *n) { return n->hw->pwm != 0; }

/* pump speed from the tacho period: 0 without pulses */
static uint16_t tach_rpm(const node_t *n)
{
    if (!n->hw->tach_period_us) return 0;
    uint32_t p = n->hw->tach_period_us(n->hw->ctx);
    if (p == 0) return 0;
    uint64_t rpm = (60000000ull + (uint64_t)p * n->tach_ppr / 2u) / ((uint64_t)p * n->tach_ppr);
    return rpm > 0xFFFFu ? 0xFFFFu : (uint16_t)rpm;
}

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
        if (n->comms_lost) st |= IOB_ST_COMMS_LOST;
        if (n->field_b) st |= IOB_ST_FIELD_B_OK;
        if (n->wdg_latched) st |= IOB_ST_WDG_RESET;
        if (n->config_needed) st |= IOB_ST_CONFIG_NEEDED;
        if (n->chip_in.fault) st |= IOB_ST_IN_CHIP;
        if (n->chip_a.fault) st |= IOB_ST_OUT_A_CHIP;
        return st;
    case IOB_R_INPUTS: return (uint8_t)(n->in_rep ^ n->in_pend);                      /* FW-32, FW-33 */
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
    case IOB_R_NODE_ADDR: return n->address;
    case IOB_R_N_TEMP_PWM: return n->n_temp_pwm;
    case IOB_R_PWM_CMD: return n->pwm_cmd;
    case IOB_R_PWM_DIR: return n->pwm_dir;
    case IOB_R_PWM_APPLIED: return n->pwm_applied;
    case IOB_R_TACH_RPM: return has_pwm(n) ? tach_rpm(n) : 0u;
    case IOB_R_TEMP_FLAGS: return has_temp(n) ? n->temp.flags : 0u;
    case IOB_R_TEMP_COUNT: return has_temp(n) ? n->temp.count : 0u;
    case IOB_R_CJ_TEMP: return (uint16_t)(has_temp(n) ? n->temp.cj : IOB_TEMP_INVALID);
    case IOB_R_PWM_FREQ: return n->pwm_hz;
    case IOB_R_TACH_PPR: return n->tach_ppr;
    default:
        if (r >= IOB_R_TEMP && r < IOB_R_TEMP + IOB_TEMPS)
            return (uint16_t)(has_temp(n) ? n->temp.t[r - IOB_R_TEMP] : IOB_TEMP_INVALID);
        if (r >= IOB_R_TEMP_STATE && r < IOB_R_TEMP_STATE + IOB_TEMPS)
            return has_temp(n) ? n->temp.st[r - IOB_R_TEMP_STATE] : IOB_TS_OFF;
        if (r >= IOB_R_TEMP_TYPE && r < IOB_R_TEMP_TYPE + IOB_TEMP_CONNS) return n->temp_cfg.type[r - IOB_R_TEMP_TYPE];
        if (r >= IOB_R_NTC_R25 && r < IOB_R_NTC_R25 + IOB_TEMP_CONNS) return n->temp_cfg.ntc_r25[r - IOB_R_NTC_R25];
        if (r >= IOB_R_NTC_BETA && r < IOB_R_NTC_BETA + IOB_TEMP_CONNS) return n->temp_cfg.ntc_beta[r - IOB_R_NTC_BETA];
        if (r >= IOB_R_UID && r < IOB_R_UID + 6u) return n->uid[r - IOB_R_UID];
        if (r >= IOB_R_SERIAL && r < IOB_R_SERIAL + IOB_SERIAL_REGS)
            return (uint16_t)(n->serial[2u * (r - IOB_R_SERIAL)] << 8 | n->serial[2u * (r - IOB_R_SERIAL) + 1u]);
        return 0;                            /* reserved, FAULT_CLEAR and BOOTLOADER read 0 */
    }
}

static bool writable(const node_t *n, uint16_t r)
{
    return r <= IOB_R_CMD_SEQ || (r >= IOB_R_HOLD_DS && r <= IOB_R_FAULT_CLEAR) ||
           (has_pwm(n) && (r == IOB_R_PWM_CMD || r == IOB_R_PWM_DIR || r == IOB_R_PWM_FREQ || r == IOB_R_TACH_PPR)) ||
           (has_temp(n) && r >= IOB_R_TEMP_TYPE && r < IOB_R_NTC_BETA + IOB_TEMP_CONNS);
}

static bool temp_type_ok(uint16_t t)
{
    return t == IOB_TT_NONE || (t >= IOB_TT_TC_K && t <= IOB_TT_TC_S) || (t >= IOB_TT_PT100_2W && t <= IOB_TT_PT1000_4W) ||
           t == IOB_TT_NTC;
}

static bool value_ok(uint16_t r, uint16_t v)
{
    if (r >= IOB_R_TEMP_TYPE && r < IOB_R_TEMP_TYPE + IOB_TEMP_CONNS) return temp_type_ok(v);
    if (r >= IOB_R_NTC_R25 && r < IOB_R_NTC_R25 + IOB_TEMP_CONNS) return v >= 10u;
    if (r >= IOB_R_NTC_BETA && r < IOB_R_NTC_BETA + IOB_TEMP_CONNS) return v >= 1000u && v <= 10000u;
    switch (r) {
    case IOB_R_PWM_CMD: return v <= 1000u;
    case IOB_R_PWM_DIR: return v <= 1u;
    case IOB_R_PWM_FREQ: return v >= 10u && v <= 25000u;
    case IOB_R_TACH_PPR: return v >= 1u && v <= 12u;
    case IOB_R_RELAY_CMD: return v <= 3u;
    case IOB_R_HOLD_DS: return v >= 1u && v <= 6000u;
    case IOB_R_FILTER_MS: return v <= 100u;
    case IOB_R_WB_ENABLE: case IOB_R_OL_ENABLE: return v <= 255u;
    case IOB_R_FAULT_CLEAR: return v <= 3u;
    default: return true;
    }
}

/* The whole write is checked before anything is written (exception code, 0 = fine). The first
 * register in the request that fails decides the code, as in the simulated node. */
static uint8_t write_check(const node_t *n, node_port_t port, uint32_t now, uint16_t start, uint16_t cnt,
                           const uint8_t *data)
{
    if (port == NODE_PORT_RS485 && n->address == 0) return 4;          /* commissioning: nothing switches */
    bool touches_cmd = false, touches_addr = false;
    for (uint16_t i = 0; i < cnt; ++i) {
        uint16_t r = (uint16_t)(start + i), v = get16(data + 2u * i);
        if (r == IOB_R_BOOTLOADER) {                                         /* FW-57 */
            if (port != NODE_PORT_USB) return 2;
            if (v != IOB_BOOT_KEY) return 3;
            continue;
        }
        if (r == IOB_R_NODE_ADDR) {                                          /* FW-60 */
            if (port != NODE_PORT_USB) return 2;
            if (v > IOB_ADDR_MAX) return 3;
            touches_addr = true;
            continue;
        }
        if (!writable(n, r)) return 2;
        if (!value_ok(r, v)) return 3;
        if (r <= IOB_R_PWM_DIR) touches_cmd = true;
    }
    if (port == NODE_PORT_USB && (touches_cmd || touches_addr) && node_master_in_control(n, now)) return 4;   /* FW-56 */
    return 0;
}

/* FW-60: a write of NODE_ADDRESS is saved to the F-RAM (and read back) before anything is
 * applied; exception 04 if that fails, and then nothing changes. 0: fine, or nothing to save. */
static uint8_t addr_save(node_t *n, uint16_t start, uint16_t cnt, const uint8_t *data)
{
    if (start > IOB_R_NODE_ADDR || (uint32_t)start + cnt <= IOB_R_NODE_ADDR) return 0;
    uint16_t v = get16(data + 2u * (IOB_R_NODE_ADDR - start));
    if (n->hw->addr_store(n->hw->ctx, (uint8_t)v) != NODE_HW_OK) return 4;
    n->nv_fault = false;
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
        case IOB_R_PWM_CMD: n->pwm_cmd = v; break;
        case IOB_R_PWM_DIR: n->pwm_dir = v; break;
        case IOB_R_PWM_FREQ: n->pwm_hz = v; break;
        case IOB_R_TACH_PPR: n->tach_ppr = v; break;
        case IOB_R_FAULT_CLEAR:                                               /* FW-44 */
            if (v & 1u) { n->timeout_latched = false; n->wdg_latched = false; }
            if (v & 2u) n->comms_lost = false;                                /* FW-58 */
            break;
        case IOB_R_NODE_ADDR: n->address = (uint8_t)v; break;                /* saved by addr_save */
        case IOB_R_BOOTLOADER:
            n->boot_pending = true;
            n->boot_at_ms = now + NODE_BOOT_DELAY_MS;
            break;
        default: {
            uint16_t *p = 0;
            if (r >= IOB_R_TEMP_TYPE && r < IOB_R_TEMP_TYPE + IOB_TEMP_CONNS) {
                uint8_t *t = &n->temp_cfg.type[r - IOB_R_TEMP_TYPE];
                if (*t != v) n->temp_cfg_dirty = true;
                *t = (uint8_t)v;
            } else if (r >= IOB_R_NTC_R25 && r < IOB_R_NTC_R25 + IOB_TEMP_CONNS) p = &n->temp_cfg.ntc_r25[r - IOB_R_NTC_R25];
            else if (r >= IOB_R_NTC_BETA && r < IOB_R_NTC_BETA + IOB_TEMP_CONNS) p = &n->temp_cfg.ntc_beta[r - IOB_R_NTC_BETA];
            if (p) { if (*p != v) n->temp_cfg_dirty = true; *p = v; }
            break;
        }
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

/* ---------------- address change and serial number (FW-61, FW-62) ---------------- */

/* FC16 to IOB_R_ADDR_SET: key, new address, new address XOR 0xFFFF. 0 (and *addr set) or an
 * exception code; nothing is changed unless it is 0. */
static uint8_t addr_set(node_t *n, node_port_t port, uint32_t now, const uint8_t *d, uint8_t *addr)
{
    uint16_t k = get16(d), a = get16(d + 2), inv = get16(d + 4);
    if (k != IOB_ADDR_SET_KEY || inv != (uint16_t)(a ^ 0xFFFFu) || a == 0 || a > IOB_ADDR_MAX) return 3;
    if (port == NODE_PORT_USB && node_master_in_control(n, now)) return 4;  /* as register 38 over USB */
    if (n->hw->addr_store(n->hw->ctx, (uint8_t)a) != NODE_HW_OK) return 4;
    n->nv_fault = false;
    *addr = (uint8_t)a;
    return 0;
}

/* FC16 to IOB_R_SERIAL over USB: printable ASCII, NUL padded, at least one character */
static uint8_t serial_write(node_t *n, node_port_t port, const uint8_t *d)
{
    if (port != NODE_PORT_USB) return 2;
    bool set = false;
    for (unsigned i = 0; i < 16u; i++) set = set || n->serial[i] != 0;
    if (set && !n->serial_written) return 2;                 /* permanent once powered off with it */
    bool end = false;
    for (unsigned i = 0; i < 16u; i++) {
        uint8_t ch = d[i];
        if (ch == 0) { end = true; continue; }
        if (end || ch < 0x20u || ch > 0x7Eu) return 3;
    }
    if (d[0] == 0) return 3;
    if (n->hw->serial_store(n->hw->ctx, d) != NODE_HW_OK) return 4;
    memcpy(n->serial, d, 16);
    n->serial_written = true;
    return 0;
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

/* FW-33: over USB, INPUTS is the inputs as they are now; over RS-485 the held changes */
static uint16_t reg_on(const node_t *n, node_port_t port, uint16_t r)
{
    if (r == IOB_R_INPUTS && port == NODE_PORT_USB) return (field_a_ok(n) && in_ok(n)) ? n->in.states : 0u;
    return node_reg(n, r);
}

static uint16_t read_regs(const node_t *n, node_port_t port, uint8_t *tx, uint8_t addr, uint8_t fc, uint16_t start,
                          uint16_t cnt)
{
    if (cnt == 0 || cnt > 125u) return exception(tx, addr, fc, 3);
    if ((uint32_t)start + cnt > NODE_REG_SPACE) return exception(tx, addr, fc, 2);
    tx[0] = addr;
    tx[1] = fc;
    tx[2] = (uint8_t)(2u * cnt);
    for (uint16_t i = 0; i < cnt; ++i) {
        uint16_t v = reg_on(n, port, (uint16_t)(start + i));
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
    comms_check(n, now);
    in_track(n);
    /* FW-12: damaged frames are dropped and counted (RS-485 only: RX_ERRORS is about the bus) */
    if (rx_error || len < 4u || len > NODE_FRAME_MAX ||
        mbr_crc16(rx, len - 2u) != (uint16_t)(rx[len - 2u] | rx[len - 1u] << 8)) {
        if (port == NODE_PORT_RS485) n->rx_errors++;
        return 0;
    }
    uint8_t addr = rx[0];
    bool commissioning = port == NODE_PORT_RS485 && n->address == 0;
    bool mine = port == NODE_PORT_RS485
                ? (commissioning ? addr == IOB_COMMISSION_ADDR : addr == n->address)              /* FW-02 */
                : (addr == NODE_USB_ADDRESS || (n->address != 0 && addr == n->address));
    if (!mine) return 0;                     /* another node, or a broadcast: never answered */
    n->act_pulses++;
    if (port == NODE_PORT_RS485 && !commissioning) { n->comms_seen = true; n->last_comms_ms = now; }   /* FW-58 */
    uint8_t fc = rx[1];
    uint16_t a = 0, c = 0;
    if (len >= 6u) { a = get16(rx + 2); c = get16(rx + 4); }
    uint16_t out = 0;
    /* FW-61, FW-62: the whole-block writes with their own rules */
    if (fc == 16 && len >= 9u && ((a == IOB_R_ADDR_SET && c == 3u) || (a == IOB_R_SERIAL && c == IOB_SERIAL_REGS))) {
        uint8_t e, new_addr = 0;
        if (rx[6] != 2u * c || len != 9u + 2u * c) e = 3;
        else if (a == IOB_R_ADDR_SET) e = addr_set(n, port, now, rx + 7, &new_addr);
        else e = serial_write(n, port, rx + 7);
        if (e) out = exception(tx, addr, fc, e);
        else { memcpy(tx, rx, 6); out = finish(tx, 6); }
        if (new_addr) n->address = new_addr;                 /* the reply still comes from the old one */
        n->replies++;
        return out;
    }
    switch (fc) {
    case 3: case 4:
        out = len == 8u ? read_regs(n, port, tx, addr, fc, a, c) : exception(tx, addr, fc, 3);
        break;
    case 6: {
        if (len != 8u) { out = exception(tx, addr, fc, 3); break; }
        uint8_t e = write_check(n, port, now, a, 1, rx + 4);
        if (!e) e = addr_save(n, a, 1, rx + 4);
        if (e) { out = exception(tx, addr, fc, e); break; }
        write_apply(n, port, now, a, 1, rx + 4);
        memcpy(tx, rx, 6);
        out = finish(tx, 6);
        break;
    }
    case 16: {
        if (len < 9u || c == 0 || c > 123u || rx[6] != 2u * c || len != 9u + 2u * c) { out = exception(tx, addr, fc, 3); break; }
        uint8_t e = (uint32_t)a + c > NODE_REG_SPACE ? 2 : write_check(n, port, now, a, c, rx + 7);
        if (!e) e = addr_save(n, a, c, rx + 7);
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
        if (!e) e = addr_save(n, wa, wc, rx + 11);
        if (e) { out = exception(tx, addr, fc, e); break; }
        write_apply(n, port, now, wa, wc, rx + 11);            /* FW: write before read */
        out = read_regs(n, port, tx, addr, fc, a, c);
        break;
    }
    default:
        out = exception(tx, addr, fc, 1);
    }
    /* FW-33: a read of INPUTS by the RS-485 master consumes the changes it was shown */
    if (port == NODE_PORT_RS485 && out > 5u && (fc == 3 || fc == 4 || fc == 23) && !(tx[1] & 0x80u) &&
        a <= IOB_R_INPUTS && (uint32_t)a + c > IOB_R_INPUTS) {
        n->in_rep = (uint8_t)(n->in_rep ^ n->in_pend);
        n->in_pend = 0;
    }
    if (out) n->replies++;
    return out;
}
