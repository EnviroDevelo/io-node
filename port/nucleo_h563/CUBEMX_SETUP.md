# NUCLEO-H563ZI as a bench I/O board

A stand-in for the I/O board on the bench, so the supervisor's I/O master (Ethernet: Modbus TCP, or
RS-485), its web page (I/O boards, I/O boards on the network, I/O board addresses) and the VM/MUX
application can be tried before the real board exists. The H563 is also the product's MCU now
(STM32H563RGT6 + LAN8742A), so this is the same chip and the same Ethernet PHY as the product.
It runs the product's node core (`node/node.c`): the same register map, Modbus slave, hold time,
comms watchdog, address change and serial number. What differs is listed in `board_nucleo.h`.

`make bench EXT=<ext>` compiles and links it for the H563 (see `check.mk`); the board's real build
is the CubeIDE project below.

## What you need

- NUCLEO-H563ZI (MB1404), powered and programmed over its ST-LINK USB.
- An Ethernet cable from the Nucleo's RJ45 (LAN8742A on board) to the supervisor's network.
- Only for the RS-485 link: a 3.3 V RS-485 transceiver module with a DE input (MAX3485 / SP3485 class), RE tied to DE (or a
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
| **ETH** | RMII, the board's default pins (PA1 REF_CLK, PA2 MDIO, PC1 MDC, PA7 CRS_DV, PC4/PC5 RXD0/1, PG11 TX_EN, PG13 TXD0, PB15 TXD1), PHY LAN8742A at address 0. **No** interrupt, **no** LwIP middleware in CubeMX: `net_bench.c` runs its own lwIP (NO_SYS) from the main loop. Keep the generated `MX_ETH_Init` call out (or delete it): `eth_h5.c` initialises ETH; CubeMX's `HAL_ETH_MspInit` (pins, clocks) is used |
| `LD1`, `LD2`, `LD3` | the board's LEDs: heartbeat (1 Hz), comms, fault |
| `B1` | the user button: input 1 while pressed |
| `OUT1` .. `OUT16`, `REL1`, `REL2` | GPIO outputs, push-pull, initial level low, on any free Zio/morpho pins |
| `IN1` .. `IN8` | GPIO inputs, **pull-up** (a switch to 0 V turns the input on) |
| `HEALTHY` | GPIO output: toggles every loop pass while the gate is open (scope it, or leave it unconnected) |
| IWDG | off in CubeMX: `board_nucleo.c` starts it (100 ms) |
| ICACHE | enabled (board default) |

Add as linked folders: `iob-node/node`, `iob-node/port/nucleo_h563` (exclude `check/`), lwIP's
`src/core`, `src/core/ipv4` and `src/netif/ethernet.c` (the GitHub repository lwip-tcpip/lwip, tag
STABLE-2_2_1_RELEASE), ST's `stm32-lan8742/lan8742.c`, and the single files
`iob-node/port/stm32g0/rs485_g0.c`, `cems-platform/platform/core/mb_rtu.c` and
`cems-platform/platform/core/iob_net.c`.
Include paths: `iob-node/node`, `iob-node/port/nucleo_h563`, `iob-node/port/nucleo_h563/lwip`,
`iob-node/port/stm32g0`, `cems-platform/platform/include`, `lwip/src/include`, `stm32-lan8742`. Symbols: `NODE_RS485_USART=USART2`,
`NODE_RS485_IRQHandler=USART2_IRQHandler`; optionally `NODE_BENCH_ADDRESS=<n>` (default 1; 0 =
start as a new board) .

In `main.c`, USER CODE BEGIN 2: `void node_main(void); node_main();`

## Using it

On Ethernet (the supervisor's `iob_link` = Ethernet, the default):

- A fresh bench board has no IP: on the supervisor's web page (admin), **I/O boards on the network →
  Find boards** lists it as new, with its chip ID and MAC (02:..., from the chip ID). Pick the board
  slot it is (Board 1 = 192.168.1.61, address 1 by default; the iobN_ip settings) and **Set**. It
  saves the settings, answers with them and changes over to the new IP; the supervisor connects
  within a second. From then on it takes settings and Modbus TCP only from that supervisor.
- The settings are kept over a reset (backup registers) and lost at power-off, like the address.
  `set_address.py COMx --clear` makes it a new board again (address 0 also clears the IP).
- Ping works once it has an IP.

On RS-485:

- The address: `python tools/set_address.py COMx` over the ST-LINK port (it is the "USB" port),
  `--address 3` to set it, `--clear` to make it a new board (then the supervisor's web page finds
  it at 246). Kept over a reset (backup registers), back to `NODE_BENCH_ADDRESS` after a power-off.
- The supervisor: enable the board (`iob1_enable`) at its address. HW_REV reads 90, so the web
  page shows "hardware 90" for a bench board.
- `tools/at02.py COMx --usb` and the RS-485 form run against it like a real board, except the
  checks that need the chips' diagnostics.
