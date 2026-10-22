/* sim_hw.h - simulated I/O board hardware for the host tests: the three chips, the relays and
 * the bank B pins behind node_hw_t. Tests set the "outside world" fields and read what the node
 * drove. */
#ifndef SIM_HW_H
#define SIM_HW_H
#include <stdbool.h>
#include <stdint.h>
#include "node_hw.h"

typedef struct {
    /* outside world, set by tests */
    uint8_t inputs, wirebreak_raw;
    uint8_t a_open_raw, a_overload, a_overtemp;
    bool    field_a, field_b;
    bool    a_dead, in_dead;            /* chip does not answer */
    int     a_fail_reads, in_fail_reads;/* the next N reads fail */
    bool    a_internal_fault, in_internal_fault;
    bool    a_readback_bad;             /* configuration reads back wrong */
    bool    b_fault_pin, b_wdg_tripped;
    /* what the node drove */
    uint8_t a_out, b_out, relays;
    uint8_t a_ol_cfg, in_filter_cfg, in_wb_cfg;
    uint32_t a_configs, in_configs, a_writes, b_writes, b_services, in_reads, a_diags;
    /* the address F-RAM */
    uint8_t  stored_address;
    int      addr_store_rc;              /* what the next stores return (NODE_HW_OK) */
    uint32_t addr_stores;
    node_hw_t hw;
} sim_hw_t;

void sim_hw_init(sim_hw_t *s);

#endif
