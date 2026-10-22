# iob-node: CEMS I/O board node firmware

Firmware for the CEMS Supervisor I/O board's own controller (STM32G0B1CET6): a Modbus RTU slave on
RS-485 that drives 8 outputs of 0.6 A (bank A, ISO2H823V2.5), 8 outputs of 1 A (bank B, ISO808-1)
and 2 relays, reads 8 isolated inputs (ISO1I813T), and turns everything off by itself when the main
board goes quiet.

**The contract** is *CEMS I/O Module Requirements: Firmware and Hardware* (FW-nn, HW-nn, AT-nn
below). Register numbers and bits come from the platform's `platform/include/iobus.h`, and the CRC
from `platform/core/mb_rtu.c`, used in place from `..\cems-platform` (the two repositories sit side
by side in `CEMS Supervisor\Firmware`). The platform's simulated node, `iob_node_sim.c`, is the
reference this firmware is compared against byte for byte.

## Layout

| Path | What |
|---|---|
| `node/node.c`, `node.h` | The core: register map, Modbus slave (functions 03, 04, 06, 16, 23), write rules, hold time, gate, HEALTHY condition, chip health, USB rules. Pure C, no hardware. |
| `node/node_hw.h` | What the core needs from the board: the three chips, bank B pins and relays. |
| `drivers/` | Chip drivers (see "Datasheet status"). |
| `port/stm32g0/` | STM32G0B1: main loop, RS-485 USART, USB virtual COM port, DIP switch, straps, unique ID, reset cause, watchdog, bootloader entry. `CUBEMX_SETUP.md` makes the CubeIDE project. |
| `tests/` | Host tests (below). `tests/platform/iob_node_real.c` puts this firmware in place of the platform's simulated node. |

## Building and testing

```
make test          # host: requirement tests, 200,000 random frames compared with iob_node_sim.c, drivers
make master-test   # the platform's whole host suite with every simulated I/O board replaced by this firmware
make target        # arm-none-eabi-gcc: compile and link for the STM32G0B1 against ST's HAL headers
```

`make target` needs `..\ext` with the xPack arm-none-eabi-gcc 13.3.1 and ST's
`stm32g0xx_hal_driver`, `cmsis_device_g0` and `cmsis-core` repositories (see `port/stm32g0/check.mk`).

Results on 22 Oct 2026:

| Check | Result |
|---|---|
| `make test` (gcc, -Werror, AddressSanitizer, UBSan) | 299 checks, 0 failed |
| Comparison with `iob_node_sim.c` | 200,000 random frames (reads, writes, function 23, bad CRCs, runts, wrong addresses, unknown functions, hold time running out) with a changing outside world: every reply byte-identical, every register equal after every frame |
| `make master-test` | 13,928 platform checks, 0 failed, with this firmware answering the master in every I/O board test (connect, settings, outputs, inputs, faults, lost and back, node restart, hold time) |
| `make target` | Compiles and links with -Werror for Cortex-M0+: 6.8 KB flash, 3.5 KB RAM (of 256 KB and 144 KB), chip drivers in their fail-safe form |
| On a board | Not yet: no hardware |

## Main loop (port/stm32g0/node_main.c)

One pass every millisecond: `node_poll` (hold time, chip reads, bank B pins), HEALTHY toggled if the
gate is open and bank A is healthy, the watchdog refreshed, LEDs. Frames are handled the moment the
USART's 1.75 ms receiver timeout ends them, so the reply starts about 1.8 ms after the request.
HEALTHY and the watchdog are touched nowhere else, so a stuck loop drops the hardware gate within
about 50 ms and resets the node within about 100 ms.

## Datasheet status

Neither this workspace nor the PC could download the chips' datasheets, so the drivers are built in
a **fail-safe form**: bank A and the input chip report "no answer" (OUT_A_CHIP, IN_CHIP), bank B's
inputs are held low, and the node's gate never opens. A product build (`NODE_PRODUCT_BUILD`)
refuses to compile until each driver is filled in and marked confirmed in `drivers/drivers.h`.
What is known so far is in each driver's header comment, with its source.

## Requirement trace

| Req | Where | Tested |
|---|---|---|
| FW-01 | `node_init` (all off, gate closed); HEALTHY only from `node_healthy` | `t_startup`, `t_fail_safe` |
| FW-02 | `board_boot_info` (DIP once); `node_frame` (DIP 0 never answers); `node_addr_led_blink` | `t_rules` |
| FW-03 | `node_init` defaults, WDG_RESET from the reset cause; `reset_cause` clears the flags | `t_startup` |
| FW-04 | `config_a` / `config_in` before anything opens; retry every 1 s | `t_chips` |
| FW-10, 11 | `rs485_g0.c` receiver timeout 202 bit times; reply from the main loop | target build; AT-03 on the bench |
| FW-12 | `node_frame` drops and counts; `rs485_irq` marks FE/NE/ORE | `t_rules`, comparison |
| FW-13 | `rs485_init`: CR3.DEM | target build |
| FW-14 | `rs485_irq` ignores bytes while a frame waits or a reply goes out | target build |
| FW-20 | `write_apply` → `apply_outputs` before CMD_ECHO | `t_outputs` |
| FW-21 | `node_poll` hold check every 1 ms pass | `t_hold` (incl. 10 min and the 2^32 ms wrap) |
| FW-22 | `node_healthy`; `node_main` toggles once per pass | `t_chips`, `t_hold` |
| FW-23 | `board_iwdg_start` (100 ms), refreshed only in `node_main` | target build; AT-07 |
| FW-24 | `node_poll`: `b_wdg_service` only while the gate is open | `t_outputs`, `t_hold` (servicing itself: datasheet) |
| FW-25 | `apply_outputs` (relays follow RELAY_CMD, off with the gate) | `t_outputs`, `t_hold` |
| FW-26 | only a write of register 0 opens the gate | `t_hold` |
| FW-30..32 | input read every 2 ms; masks; 0 without supply A | `t_chips` |
| FW-40..43 | diagnostics every 5 ms; present state; FAULT 1 ms debounce; two failures | `t_chips` |
| FW-44 | FAULT_CLEAR clears TIMEOUT and WDG_RESET only | `t_hold` |
| FW-50 | static 256-byte buffers, no heap | code |
| FW-51 | `node_reg`, reserved read 0 | `t_startup`, comparison |
| FW-52 | `node/node_version.h` | `t_startup` |
| FW-53 | `make target` -Werror; iobus.h and mb_rtu.c used in place | target build |
| FW-55 | activity pulse per valid frame; fault LED | `node_main` |
| FW-56 | USB at the DIP address or 247; exception 04 while the master is in control | `t_usb` |
| FW-57 | register 37 over USB; outputs off 100 ms, then a reset into the bootloader | `t_usb`, `t_rules` |

AT-02 (register rules from a PC) is what `t_rules` and the comparison do in software; the rest of
the acceptance tests need the board.

## Differences from the simulated node (to raise, per the requirements' rule)

With healthy chips the two are byte-identical. They differ where the simulator has no model:

1. **Bank A chip fault closes the gate.** FW-22 lets HEALTHY toggle only while the output chips are
   healthy, so OUT_A_CHIP drops the hardware gate: OUT_ENABLED, OUT_APPLIED and RELAY_APPLIED read 0,
   and **bank B and the relays go off too**. The simulator keeps the gate open. Please confirm that a
   bank A fault should also stop bank B and the relays (it is what FW-22 with one gate, HW-04, means).
2. While IN_CHIP is set, INPUTS and WIREBREAK read 0 rather than the last values read; while
   OUT_A_CHIP is set, A_OPEN, A_OVERLOAD and A_OVERTEMP read 0.
3. FIELD_A_OK comes from both chips' supply status; with neither chip answering it reads missing.
4. RX_ERRORS counts RS-485 errors only, not bad USB frames.
5. The USB port (247, exception 04, register 37) is not in the simulator.

## Decisions made here (change any of them)

| # | Decision |
|---|---|
| N1 | Main loop pass every 1 ms (FW-21's 5 ms needs faster than FW-22's 10 ms minimum). |
| N2 | Bootloader entry (FW-57) through a flag in `.noinit` and a software reset, then the jump before HAL_Init: the watchdog cannot be stopped once started, and it is stopped after a reset. |
| N3 | DIP ON = 0 V with pull-ups; DIP1 = bit 0. HW_REV = 1 + strap value (two straps: 1..4). |
| N4 | UID: register 69 = bits 0-15 of the first UID word, 70 = bits 16-31, and so on. |
| N5 | USB frames end after 2 ms without a packet; USB frames are only answered while VBUS is present. |
| N6 | The activity LED pulses 20 ms for each valid frame addressed to the node on either port. The fault LED shows a chip fault, field supply A missing, a latched TIMEOUT or a bank B fault. |
| N7 | Input reads every 2 ms (FW-30: 5 ms), bank A diagnostics every 5 ms (FW-40: 10 ms). |

## Open items

1. **Datasheets** for the ISO2H823V2.5, ISO1I813T and ISO808-1 (`Firmware\datasheets\`): register
   maps, SPI modes and clocks, filter coding, wire-break and supply bits, ISO808 SYNC/LOAD and watchdog.
2. HSI16 accuracy over temperature at 115200 baud (HW-01): bench test, or the crystal footprint.
3. AN2606 conditions for the G0B1's USB bootloader, and the nBOOT_SEL option byte (CUBEMX_SETUP.md 9).
4. The RS-485 RX pin needs a pull-up (on the board or the pin's own).
