/* bench_hw.c - the node_hw_t of the Nucleo-H563ZI bench board (board_nucleo.h): plain GPIO in
 * place of the three I/O chips, and the TAMP backup registers in place of the address F-RAM.
 *
 * Outputs 1-16 and the two relays drive the pins labelled OUT1..OUT16, REL1, REL2 (1 = on), for
 * LEDs or an opto board. Inputs 1-8 read the pins IN1..IN8 (pull-up, active low: a switch to 0 V
 * turns the input on); the blue user button B1 is also input 1. Each input is debounced with the
 * filter time the master sets (FILTER_MS, register 33), as the ISO1I813T would. There are no
 * diagnostics: no open circuit, overload, over-temperature, wire break or chip fault, and both
 * field supplies read present. */
#include "board_nucleo.h"
#include "main.h"
#include "node_hw.h"

#define PIN(l) { l##_GPIO_Port, l##_Pin }
typedef struct { GPIO_TypeDef *port; uint16_t pin; } pin_t;
static const pin_t out_pins[16] = {
    PIN(OUT1),
    PIN(OUT2),
    PIN(OUT3),
    PIN(OUT4),
    PIN(OUT5),
    PIN(OUT6),
    PIN(OUT7),
    PIN(OUT8),
    PIN(OUT9),
    PIN(OUT10),
    PIN(OUT11),
    PIN(OUT12),
    PIN(OUT13),
    PIN(OUT14),
    PIN(OUT15),
    PIN(OUT16)
};
static const pin_t rel_pins[2] = { PIN(REL1), PIN(REL2) };
static const pin_t in_pins[8] = { PIN(IN1), PIN(IN2), PIN(IN3), PIN(IN4), PIN(IN5), PIN(IN6), PIN(IN7), PIN(IN8) };

static inline void pin_set(const pin_t *p, bool v) { p->port->BSRR = v ? p->pin : (uint32_t)p->pin << 16; }
static inline bool pin_get(const pin_t *p) { return (p->port->IDR & p->pin) != 0; }

static uint8_t  filter_ms;
static uint8_t  stable, last_raw;
static uint32_t since[8];

static void drive(const pin_t *pins, unsigned n, uint8_t bits)
{
    for (unsigned i = 0; i < n; i++) pin_set(&pins[i], (bits >> i) & 1u);
}

static int a_config(void *ctx, uint8_t ol) { (void)ctx; (void)ol; return NODE_HW_OK; }
static int a_write(void *ctx, uint8_t o) { (void)ctx; drive(out_pins, 8, o); return NODE_HW_OK; }
static int a_diag(void *ctx, node_a_diag_t *d)
{
    (void)ctx;
    *d = (node_a_diag_t){ .supply_ok = true };
    return NODE_HW_OK;
}

static int in_config(void *ctx, uint8_t f, uint8_t wb) { (void)ctx; (void)wb; filter_ms = f; return NODE_HW_OK; }
static int in_read(void *ctx, node_in_t *d)
{
    (void)ctx;
    uint32_t now = HAL_GetTick();
    uint8_t raw = 0;
    for (unsigned i = 0; i < 8; i++) if (!pin_get(&in_pins[i])) raw |= (uint8_t)(1u << i);
    if (B1_GPIO_Port->IDR & B1_Pin) raw |= 1u;                     /* B1: high while pressed */
    for (unsigned i = 0; i < 8; i++) {
        uint8_t m = (uint8_t)(1u << i);
        if ((raw ^ last_raw) & m) since[i] = now;                  /* the pin moved: start again */
        if (((raw ^ stable) & m) && (uint32_t)(now - since[i]) >= filter_ms) stable ^= m;
    }
    last_raw = raw;
    *d = (node_in_t){ .states = stable, .supply_ok = true };
    return NODE_HW_OK;
}

static void b_write(void *ctx, uint8_t o) { (void)ctx; drive(&out_pins[8], 8, o); }
static bool b_false(void *ctx) { (void)ctx; return false; }
static bool b_true(void *ctx) { (void)ctx; return true; }
static void b_nop(void *ctx) { (void)ctx; }
static void relays(void *ctx, uint8_t r) { (void)ctx; drive(rel_pins, 2, r); }
static int addr_store(void *ctx, uint8_t a) { (void)ctx; return bench_store_address(a) ? NODE_HW_OK : NODE_HW_READBACK; }
static int serial_store(void *ctx, const uint8_t s[16]) { (void)ctx; return bench_store_serial(s) ? NODE_HW_OK : NODE_HW_READBACK; }

static const node_hw_t hw = {
    .a_config = a_config, .a_write = a_write, .a_diag = a_diag,
    .in_config = in_config, .in_read = in_read,
    .b_write = b_write, .b_fault_pin = b_false, .b_wdg_tripped = b_false, .b_wdg_service = b_nop,
    .b_supply_ok = b_true, .relays = relays, .addr_store = addr_store, .serial_store = serial_store,
};

const node_hw_t *bench_hw(void) { return &hw; }
