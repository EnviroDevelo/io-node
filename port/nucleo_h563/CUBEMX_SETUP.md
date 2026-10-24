# NUCLEO-H563ZI as a bench I/O board

A stand-in for the I/O board on the bench, so the supervisor's RS-485 master, its web page (I/O
boards, I/O board addresses) and the VM/MUX application can be tried before the real board exists.
It runs the product's node core (`node/node.c`): the same register map, Modbus slave, hold time,
comms watchdog, address change and serial number. What differs is listed in `board_nucleo.h`.

`make bench EXT=<ext>` compiles and links it for the H563 (see `check.mk`); the board's real build
is the CubeIDE project below.

## What you need

- NUCLEO-H563ZI (MB1404), powered and programmed over its ST-LINK USB.
- A 3.3 V RS-485 transceiver module with a DE input (MAX3485 / SP3485 class), RE tied to DE (or a
  module that drives /RE from DE). A, B and ground to the supervisor's RS-485 terminal; 120 Ohm at
  each end of the bus.
- Optional: LEDs (with resistors) on the output pins, switches to 0 V on the input pins.

## CubeMX project

STM32CubeIDE → New STM32 Project → Board Selector → **NUCLEO-H563ZI**, initialise peripherals with
their default mode: yes (LD1-LD3, B1 and the ST-LINK virtual COM port on USART3 come from the board).
Project `iob-bench-h563` next to this repository. TrustZone off.

Clocks: leave the board default (HSE 8 MHz from the ST-LINK → PLL; 250 MHz is fine).

| Peripheral / label | Setting |
|---|---|
| **USART2** | Asynchronous, **Hardware Flow Control (RS485)**, 115200 8N1, FIFO **disabled**, global interrupt on, NVIC code generation: **untick "Generate IRQ handler"** for USART2. Suggested pins: PD5 TX, PD6 RX (**pull-up**), PD4 DE, on the Zio connector; check they are free in CubeMX's pinout for this board. Another USART works: change `NODE_RS485_USART` / `NODE_RS485_IRQHandler` (Preprocessor symbols) |
| **USART3** (ST-LINK VCP, PD8/PD9) | Asynchronous 115200 8N1, FIFO disabled, global interrupt on, **untick "Generate IRQ handler"** (`vcp_h5.c` defines it). This is the board's "USB-C port": Modbus at address 247 |
| `LD1`, `LD2`, `LD3` | the board's LEDs: heartbeat (1 Hz), comms, fault |
| `B1` | the user button: input 1 while pressed |
| `OUT1` .. `OUT16`, `REL1`, `REL2` | GPIO outputs, push-pull, initial level low, on any free Zio/morpho pins |
| `IN1` .. `IN8` | GPIO inputs, **pull-up** (a switch to 0 V turns the input on) |
| `HEALTHY` | GPIO output: toggles every loop pass while the gate is open (scope it, or leave it unconnected) |
| IWDG | off in CubeMX: `board_nucleo.c` starts it (100 ms) |
| ICACHE | enabled (board default) |

Add as linked folders: `iob-node/node`, `iob-node/port/nucleo_h563` (exclude `check/`), and the
single files `iob-node/port/stm32g0/rs485_g0.c` and `cems-platform/platform/core/mb_rtu.c`.
Include paths: `iob-node/node`, `iob-node/port/nucleo_h563`, `iob-node/port/stm32g0`,
`cems-platform/platform/include`. Symbols: `NODE_RS485_USART=USART2`,
`NODE_RS485_IRQHandler=USART2_IRQHandler`; optionally `NODE_BENCH_ADDRESS=<n>` (default 1; 0 =
start as a new board) .

In `main.c`, USER CODE BEGIN 2: `void node_main(void); node_main();`

## Using it

- The address: `python tools/set_address.py COMx` over the ST-LINK port (it is the "USB" port),
  `--address 3` to set it, `--clear` to make it a new board (then the supervisor's web page finds
  it at 246). Kept over a reset (backup registers), back to `NODE_BENCH_ADDRESS` after a power-off.
- The supervisor: enable the board (`iob1_enable`) at its address. HW_REV reads 90, so the web
  page shows "hardware 90" for a bench board.
- `tools/at02.py COMx --usb` and the RS-485 form run against it like a real board, except the
  checks that need the chips' diagnostics.
