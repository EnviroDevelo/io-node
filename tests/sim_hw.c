/* sim_hw.c - see sim_hw.h */
#include "sim_hw.h"
#include <string.h>
#include "iobus.h"

static int a_config(void *ctx, uint8_t ol)
{
    sim_hw_t *s = ctx;
    s->a_configs++;
    if (s->a_dead) return NODE_HW_NO_ANSWER;
    if (s->a_readback_bad) return NODE_HW_READBACK;
    s->a_ol_cfg = ol;
    return NODE_HW_OK;
}
static int a_write(void *ctx, uint8_t o)
{
    sim_hw_t *s = ctx;
    s->a_writes++;
    if (s->a_dead) return NODE_HW_NO_ANSWER;
    s->a_out = o;
    return NODE_HW_OK;
}
static int a_diag(void *ctx, node_a_diag_t *d)
{
    sim_hw_t *s = ctx;
    s->a_diags++;
    if (s->a_dead) return NODE_HW_NO_ANSWER;
    if (s->a_fail_reads > 0) { s->a_fail_reads--; return NODE_HW_NO_ANSWER; }
    d->open = s->a_open_raw;
    d->overload = s->a_overload;
    d->overtemp = s->a_overtemp;
    d->supply_ok = s->field_a;
    d->chip_fault = s->a_internal_fault;
    return NODE_HW_OK;
}
static int in_config(void *ctx, uint8_t f, uint8_t wb)
{
    sim_hw_t *s = ctx;
    s->in_configs++;
    if (s->in_dead) return NODE_HW_NO_ANSWER;
    s->in_filter_cfg = f;
    s->in_wb_cfg = wb;
    return NODE_HW_OK;
}
static int in_read(void *ctx, node_in_t *d)
{
    sim_hw_t *s = ctx;
    s->in_reads++;
    if (s->in_dead) return NODE_HW_NO_ANSWER;
    if (s->in_fail_reads > 0) { s->in_fail_reads--; return NODE_HW_NO_ANSWER; }
    d->states = s->field_a ? s->inputs : 0;
    d->wirebreak = s->wirebreak_raw;
    d->supply_ok = s->field_a;
    d->chip_fault = s->in_internal_fault;
    return NODE_HW_OK;
}
static void b_write(void *ctx, uint8_t o) { sim_hw_t *s = ctx; s->b_writes++; s->b_out = o; }
static bool b_fault_pin(void *ctx) { return ((sim_hw_t *)ctx)->b_fault_pin; }
static bool b_wdg_tripped(void *ctx) { return ((sim_hw_t *)ctx)->b_wdg_tripped; }
static void b_wdg_service(void *ctx) { ((sim_hw_t *)ctx)->b_services++; }
static bool b_supply_ok(void *ctx) { return ((sim_hw_t *)ctx)->field_b; }
static void relays(void *ctx, uint8_t r) { ((sim_hw_t *)ctx)->relays = r; }
static int addr_store(void *ctx, uint8_t a)
{
    sim_hw_t *s = ctx;
    s->addr_stores++;
    if (s->addr_store_rc == NODE_HW_OK) s->stored_address = a;
    return s->addr_store_rc;
}
static int serial_store(void *ctx, const uint8_t sn[16])
{
    sim_hw_t *s = ctx;
    s->serial_stores++;
    if (s->serial_store_rc == NODE_HW_OK) memcpy(s->stored_serial, sn, 16);
    return s->serial_store_rc;
}

static void temp_config(void *ctx, const node_temp_cfg_t *c) { sim_hw_t *s = ctx; s->temp_configs++; s->temp_cfg = *c; }
static void temp_poll(void *ctx, uint32_t now, node_temp_t *t) { sim_hw_t *s = ctx; (void)now; s->temp_polls++; *t = s->temp; }
static void pwm(void *ctx, uint16_t d, uint16_t hz, bool dir) { sim_hw_t *s = ctx; s->pwm_duty = d; s->pwm_hz = hz; s->pwm_dir = dir; }
static uint32_t tach(void *ctx) { return ((sim_hw_t *)ctx)->tach_us; }

void sim_hw_map1(sim_hw_t *s)
{
    s->hw.temp_config = 0; s->hw.temp_poll = 0; s->hw.pwm = 0; s->hw.tach_period_us = 0;
}

void sim_hw_map2(sim_hw_t *s)
{
    s->hw.temp_config = temp_config; s->hw.temp_poll = temp_poll; s->hw.pwm = pwm; s->hw.tach_period_us = tach;
}

void sim_hw_init(sim_hw_t *s)
{
    memset(s, 0, sizeof *s);
    s->field_a = s->field_b = true;
    s->hw = (node_hw_t){ s, a_config, a_write, a_diag, in_config, in_read,
                         b_write, b_fault_pin, b_wdg_tripped, b_wdg_service, b_supply_ok, relays, addr_store, serial_store,
                         temp_config, temp_poll, pwm, tach };
    for (unsigned k = 0; k < IOB_TEMPS; k++) { s->temp.t[k] = IOB_TEMP_INVALID; s->temp.st[k] = IOB_TS_OFF; }
    s->temp.cj = IOB_TEMP_INVALID;
}
