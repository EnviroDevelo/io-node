/* node.h - the CEMS I/O board node firmware core: Modbus RTU slave, register map (map version 2)
 * and safe state. Contract: "CEMS I/O Module Requirements: Firmware and Hardware" (FW-nn below);
 * register numbers and bits from the platform's iobus.h; CRC from the platform's mb_rtu.c.
 *
 * Pure C: no hardware, no interrupts, no OS. The board's main loop calls
 *   node_poll(now)           every pass (at least every 1 ms: FW-21 needs "off within 5 ms")
 *   node_frame(port, ...)    for each complete frame (USART receiver timeout, or a USB packet)
 *   node_healthy()           to decide whether to toggle HEALTHY this pass (FW-22)
 * and the chips are reached through node_hw_t.
 *
 * Safe state (FW-01, FW-20..26):
 *   - At start the gate is closed and everything is off. The gate opens on the first valid
 *     write that includes register 0 (OUT_CMD); outputs then follow OUT_CMD / RELAY_CMD.
 *   - Only a valid write that includes register 0 restarts the hold timer. When HOLD_DS x 0.1 s
 *     passes without one, every output and relay goes off, the gate closes and TIMEOUT latches;
 *     the next valid write of register 0 re-opens it.
 *   - HEALTHY toggles only while the gate is open (a bank A chip fault is reported, OUT_A_CHIP, but
 *     does not stop it: bank B and the relays keep running), so a stuck loop or a run-out hold
 *     time lets the hardware gate drop.
 *   - Reads, settings writes and fault clears never switch anything on.
 *
 * Communications watchdog (FW-58):
 *   - The link counts as up while a valid request addressed to this node has arrived on RS-485
 *     within NODE_COMMS_TIMEOUT_MS (any function, exceptions included; not USB, not broadcasts,
 *     not frames with a bad CRC).
 *   - Once the link has been up, losing it latches COMMS_LOST (STATUS bit 8). It is not cleared
 *     by the link coming back: only a write of FAULT_CLEAR with bit 1 set (the main board sends
 *     it when its IOB_LOST alarm is acknowledged) or a restart clears it.
 *   - The outputs are governed by the hold time, not by the link, so a short break or a
 *     main-board restart does not drop the valves.
 *
 * LEDs (FW-55, FW-59), all set from the main loop, so a stuck loop freezes them:
 *   - heartbeat: 1 Hz, 50 %, whenever the firmware runs (nothing else);
 *   - comms: one short flash every 0.5 s while the link is up; a double flash every second
 *     while it is up but a loss is still latched; fast 4 Hz while it is down; off before the
 *     master's first request; steady on while the address is not set (0: never answers, FW-02).
 *
 * Inputs (FW-33): every change of an input is reported at least once to the RS-485 master, so
 * an alarm contact that opens for less than the master's poll period is not missed. A read of
 * INPUTS over RS-485 returns, for each input that changed since the previous such read, the
 * other state from the one last reported; a change and its return count as one change shown.
 * Reads over USB see the inputs as they are now (nothing held, nothing consumed), so a PC tool
 * works the same with or without a master on the bus.
 *
 * Ports (FW-56, FW-57): requests arrive on the RS-485 bus or on the USB virtual COM port.
 *   - RS-485 answers at the node's address only (1..247, kept in the board's F-RAM: FW-60);
 *     0 means not set: never answer.
 *   - USB answers at the node's address or at 247 (NODE_USB_ADDRESS).
 *   - Over USB, writes to registers 0-4 are refused (exception 04) while the RS-485 master is in
 *     control, that is within the hold time of its last valid write of register 0.
 *   - Register 37 (BOOTLOADER) is writable over USB only, value 0xB007; over RS-485 it is
 *     exception 02 like any read-only register.
 *   - Register 38 (NODE_ADDRESS) is writable directly over USB only (factory), 0..245, and not
 *     while the RS-485 master is in control (04). The new address is saved to the F-RAM and read
 *     back before the reply; if that fails the reply is exception 04 and nothing changes. It takes
 *     effect at once.
 *   - Over RS-485 the address changes only through ONE FC16 write of 40-42: the key 0xA55A, the
 *     new address, the new address XOR 0xFFFF (FW-61); anything else there is refused.
 *   - A node without an address answers on RS-485 at 246 (FW-02, commissioning): reads and the
 *     address change only; every other write is exception 04, and it does not count as the link
 *     (FW-58).
 *   - The serial number (52..59, FW-62) is written over USB only and only until the board has been
 *     powered off once with it set.
 *
 * Map 2 (H563 board rev A), present when the board's node_hw_t has them (IOB_R_N_TEMP_PWM):
 *   - pump PWM (3, 4): an output like the others. It follows PWM_CMD only while the gate is open, so
 *     the hold time turns it off with everything else; the hardware gate cuts PWM1 too. Writing 3-4
 *     does not restart the hold time (only register 0 does) and counts as a command for FW-56.
 *     Tacho speed in 29, from the period and TACH_PPR.
 *   - temperatures (80..92, 30, 31): measured by the board's front end in the background from the
 *     sensor types in 96..104; every channel has an explicit state (IOB_TS_*), never a 0 for a fault. */
#ifndef NODE_H
#define NODE_H
#include <stdbool.h>
#include <stdint.h>
#include "iobus.h"
#include "node_hw.h"

#define NODE_REG_SPACE   107u     /* registers 0..106 */
#define NODE_FRAME_MAX   256u     /* FW-50: static 256-byte receive and transmit buffers */
#define NODE_USB_ADDRESS 247u     /* FW-56: USB service address */

/* defaults (FW-03) */
#define NODE_DEF_HOLD_DS  3000u   /* 5 min */
#define NODE_DEF_FILTER     8u
#define NODE_DEF_WB         0u
#define NODE_DEF_OL      0xFFu
#define NODE_DEF_NTC_R25 1000u    /* 10k */
#define NODE_DEF_NTC_BETA 3435u
#define NODE_DEF_PWM_HZ  1000u
#define NODE_DEF_PPR        2u

/* timing (FW-30, FW-40, FW-41, FW-04) */
#define NODE_IN_PERIOD_MS     2u  /* input chip read (FW-30: at least every 5 ms) */
#define NODE_DIAG_PERIOD_MS   5u  /* bank A diagnostics (FW-40: at least every 10 ms) */
#define NODE_RETRY_MS      1000u  /* a failed chip is configured again every second */
#define NODE_BFAULT_MS        1u  /* bank B FAULT pin debounce */
#define NODE_BOOT_DELAY_MS  100u  /* outputs off and HEALTHY stopped this long before the bootloader jump */
#define NODE_COMMS_TIMEOUT_MS 1000u /* FW-58: link down after this long without a valid request (= the master's IOB_LOST) */

typedef enum { NODE_PORT_RS485 = 0, NODE_PORT_USB = 1 } node_port_t;

typedef enum {                      /* reset cause, from the port's RCC flags */
    NODE_RST_POWER_ON = 0,          /* power-on or brown-out */
    NODE_RST_IWDG,                  /* independent watchdog */
    NODE_RST_SOFTWARE,
    NODE_RST_PIN,                   /* NRST pin (debugger, button) */
    NODE_RST_OTHER
} node_reset_t;

typedef struct {
    uint8_t      address;           /* 0..247 from the board's F-RAM, 0 = not set (FW-02, FW-60) */
    bool         nv_ok;             /* the F-RAM answered (else address 0 and the fault LED on) */
    uint8_t      serial[16];        /* the board's serial number from the F-RAM, NUL padded; all 0 = not set */
    uint8_t      hw_rev;            /* from the straps (HW-11) */
    node_reset_t reset_cause;
    uint16_t     resets;            /* resets other than power-on since power-on (kept by the port in .noinit) */
    uint16_t     uid[6];            /* 96-bit unique ID, first word = register 69 */
} node_boot_t;

/* one chip's health: two failed reads in a row make it faulty (FW-43) */
typedef struct {
    bool     configured;            /* configured and read back since the last failure */
    bool     fault;                 /* STATUS IN_CHIP / OUT_A_CHIP */
    uint8_t  fails;                 /* failed reads in a row */
    uint32_t retry_ms;              /* next configuration attempt */
} node_chip_t;

typedef struct {
    const node_hw_t *hw;
    /* registers the master writes */
    uint16_t out_cmd, relay_cmd, seq, echo;
    uint16_t hold_ds, filter_ms, wb_en, ol_en;
    uint16_t pwm_cmd, pwm_dir, pwm_hz, tach_ppr;    /* map 2 */
    node_temp_cfg_t temp_cfg;
    bool     temp_cfg_dirty;
    node_temp_t temp;                /* the front end's latest */
    uint16_t pwm_applied;
    /* state */
    uint8_t  address;               /* from the F-RAM; 0 = not set, never answers on RS-485 */
    bool     nv_fault;              /* the address F-RAM did not answer at start-up */
    /* FW-62: serial number */
    uint8_t  serial[16];
    bool     serial_written;        /* written since this start-up: may still be corrected */
    bool     config_needed, timeout_latched, wdg_latched;
    bool     gate;                  /* outputs follow the commands (FW-21) */
    bool     master_seen;           /* a valid RS-485 write of register 0 since start */
    uint32_t last_write_ms;         /* last valid write of register 0 (any port) */
    uint32_t last_master_ms;        /* last valid RS-485 write of register 0 */
    bool     comms_seen;            /* a valid RS-485 request to this node since start (FW-58) */
    uint32_t last_comms_ms;         /* the last one */
    bool     comms_lost;            /* latched: the link was lost (STATUS COMMS_LOST) */
    uint8_t  in_rep, in_pend;       /* FW-33: inputs last reported to the RS-485 master, changes since */
    uint16_t rx_errors, resets;
    /* chips */
    node_chip_t chip_a, chip_in;
    bool     cfg_a_dirty, cfg_in_dirty;
    uint32_t next_in_ms, next_diag_ms;
    node_a_diag_t a;                /* last good bank A diagnostics */
    node_in_t     in;               /* last good input read */
    bool     b_fault_raw; uint32_t b_fault_since; bool b_fault; bool b_wdg; bool field_b;
    /* what is driven now */
    uint16_t out_applied;           /* bits 0-7 bank A, 8-15 bank B */
    uint8_t  relay_applied;
    /* bootloader request (FW-57) */
    bool     boot_pending; uint32_t boot_at_ms;
    /* identification */
    uint16_t magic, map_version, hw_rev, fw_version, fw_build, uid[6];
    uint16_t n_out_a, n_out_b, n_relays, n_inputs, n_temp_pwm;
    /* statistics */
    uint32_t frames, replies, writes0;
    uint32_t act_pulses;            /* valid frames addressed to this node */
} node_t;

void     node_init(node_t *n, const node_hw_t *hw, const node_boot_t *boot, uint32_t now_ms);
void     node_poll(node_t *n, uint32_t now_ms);
/* One received frame. rx_error: the port saw a framing, noise or overrun error in it (FW-12).
 * Returns the reply length in tx (0: no reply). tx must hold NODE_FRAME_MAX bytes. */
uint16_t node_frame(node_t *n, node_port_t port, uint32_t now_ms, const uint8_t *rx, uint16_t len,
                    bool rx_error, uint8_t *tx);
uint16_t node_reg(const node_t *n, uint16_t reg);      /* what a read of reg returns */
bool     node_healthy(const node_t *n);                /* toggle HEALTHY this pass (FW-22) */
bool     node_master_in_control(const node_t *n, uint32_t now_ms);
bool     node_boot_due(const node_t *n, uint32_t now_ms);   /* time to jump to ST's bootloader */
bool     node_fault_led(const node_t *n);
bool     node_comms_ok(const node_t *n, uint32_t now_ms);       /* FW-58: the master is talking to this node */
bool     node_comms_led(const node_t *n, uint32_t now_ms);      /* FW-55: the comms LED pattern */
bool     node_heartbeat_led(uint32_t now_ms);                   /* FW-59: 1 Hz while the firmware runs */

#endif
