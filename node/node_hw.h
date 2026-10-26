/* node_hw.h - what the node core needs from the board: the three I/O chips, the relays and
 * the bank B pins. One table of functions with a context pointer, so the host tests can run
 * several simulated boards side by side.
 *
 * Every function is called from the main loop only (never from an interrupt), and returns
 * quickly: an SPI transfer of a few bytes or a pin write.
 *
 * Chip drivers: drivers/ (ISO2H823V2.5 bank A, ISO1I813T inputs, ISO808A-1 bank B, nvcfg F-RAM).
 * Host: tests/sim_hw.c. */
#ifndef NODE_HW_H
#define NODE_HW_H
#include <stdbool.h>
#include <stdint.h>

/* Return codes of the chip calls. A chip that does not answer, or answers with a bad CRC,
 * is NODE_HW_NO_ANSWER; a chip that answers but reports an internal or supply fault in
 * its own status is reported through the result structure, not the return code. */
#define NODE_HW_OK         0
#define NODE_HW_NO_ANSWER -1
#define NODE_HW_READBACK  -2   /* configuration written but read back different */

typedef struct {
    uint8_t open;        /* open load, per channel (raw: the core masks it with OL_ENABLE) */
    uint8_t overload;    /* short or current limit, per channel */
    uint8_t overtemp;    /* channel over-temperature */
    bool    supply_ok;   /* field supply A present (chip's undervoltage status) */
    bool    chip_fault;  /* the chip reports an internal fault */
} node_a_diag_t;

typedef struct {
    uint8_t states;      /* inputs 1-8 after the chip's filter (1 = active) */
    uint8_t wirebreak;   /* per channel (raw: the core masks it with WB_ENABLE) */
    bool    supply_ok;   /* field supply A present at the input chip */
    bool    chip_fault;  /* the chip reports an internal fault */
} node_in_t;

/* map 2: the temperature front end's settings (registers 96..104) and readings (80..92, 30, 31) */
typedef struct {
    uint8_t  type[3];        /* IOB_TT_* per connector */
    uint16_t ntc_r25[3];     /* an NTC's resistance at 25 degC, 10 ohm units */
    uint16_t ntc_beta[3];    /* K */
} node_temp_cfg_t;

typedef struct {
    int16_t  t[6];           /* 0.1 degC, IOB_TEMP_INVALID without a reading */
    uint8_t  st[6];          /* IOB_TS_* */
    int16_t  cj;             /* cold junction, 0.1 degC */
    uint16_t flags;          /* IOB_TF_* */
    uint16_t count;          /* finished measurement rounds */
} node_temp_t;

typedef struct node_hw {
    void *ctx;
    /* bank A (SPI). config: mode, open-load detection per channel; written and read back. */
    int  (*a_config)(void *ctx, uint8_t ol_enable);
    int  (*a_write)(void *ctx, uint8_t outputs);           /* outputs 1-8 */
    int  (*a_diag)(void *ctx, node_a_diag_t *d);
    /* inputs (SPI). config: filter (ms, the chip's nearest at or above), wire-break per channel. */
    int  (*in_config)(void *ctx, uint8_t filter_ms, uint8_t wb_enable);
    int  (*in_read)(void *ctx, node_in_t *d);
    /* bank B (ISO808A-1 on SPI, status pins) */
    void (*b_write)(void *ctx, uint8_t outputs);           /* outputs 9-16 */
    bool (*b_fault_pin)(void *ctx);                        /* common FAULT pin active */
    bool (*b_wdg_tripped)(void *ctx);                      /* bank B's own watchdog holds its outputs off */
    void (*b_wdg_service)(void *ctx);                      /* called every pass while the gate is open */
    bool (*b_supply_ok)(void *ctx);                        /* field supply B present */
    /* relays (GPIO, through the hardware gate) */
    void (*relays)(void *ctx, uint8_t relays);             /* bits 0-1 */
    /* the node's address in the board's F-RAM (FW-60): written and read back; NODE_HW_* */
    int  (*addr_store)(void *ctx, uint8_t address);
    /* the board's serial number in the F-RAM (FW-62): 16 bytes, written and read back; NODE_HW_* */
    int  (*serial_store)(void *ctx, const uint8_t serial[16]);
    /* map 2, each NULL when the board has none (IOB_R_N_TEMP_PWM then reads 0 for it).
     * temp_poll is called every pass: it measures in the background (one round of conversions
     * takes about a second) and keeps *t up to date. */
    void (*temp_config)(void *ctx, const node_temp_cfg_t *c);
    void (*temp_poll)(void *ctx, uint32_t now_ms, node_temp_t *t);
    /* pump PWM: duty 0..1000 (0.1 %), frequency Hz, the DIR output. Behind the hardware gate. */
    void (*pwm)(void *ctx, uint16_t duty, uint16_t freq_hz, bool dir);
    uint32_t (*tach_period_us)(void *ctx);                 /* the last full tacho period; 0 = no edge for 2 s */
} node_hw_t;

#endif
