# STM32CubeIDE project for the I/O board node (STM32G0B1CET6)

The node firmware is plain C in this repository; CubeMX supplies the clocks, pins, the USB device
stack and `main.c`. These steps make the project. Pin numbers come from the schematic: only the
**user labels** below matter to the code.

## 1. New project

STM32CubeIDE → New STM32 Project → part **STM32G0B1CETx** (LQFP48). Firmware package: the current
STM32Cube FW_G0. Project name `iob-node-g0`, created in `CEMS Supervisor\Firmware\iob-node-g0\`
(next to this repository, not inside it).

## 2. Clocks (RCC)

- HSI16 → PLL → SYSCLK 64 MHz (PLLM /1, PLLN ×8, PLLR /2). The USART clock is PCLK (64 MHz).
- USB: **HSI48 with the clock recovery system (CRS) synchronised to USB SOF**, so no crystal is
  needed (HW-01). RCC → CRS SYNC: Source USB.
- Leave the 8 MHz crystal footprint unused unless the HSI16 test over temperature fails (open item).

## 3. Pins and labels

Every output pin: push-pull, **initial level low**, no pull (the board's own pull-downs hold the
outputs off before this code runs, HW-05). Inputs as listed.

| Label | Mode | Notes |
|---|---|---|
| `HEALTHY` | GPIO output | feeds the monostable (HW-04); toggled every loop pass |
| `LED_ACT`, `LED_FAULT` | GPIO output | |
| `CS_A`, `CS_IN` | GPIO output, **initial level high** | chip selects for bank A and the input chip |
| `DIP1`..`DIP4` | GPIO input, pull-up | switch ON connects to 0 V; DIP1 = address bit 0 |
| `HWREV0`, `HWREV1` | GPIO input, pull-up | straps to 0 V; HW_REV = 1 + value |
| `B_IN1`..`B_IN8` | GPIO output | bank B parallel inputs |
| `B_SYNC`, `B_LOAD`, `B_WD` | GPIO output | levels per the ISO808 datasheet (open item) |
| `B_FAULT` | GPIO input, no pull | open drain, pulled up on the board, active low |
| `B_SUPPLY` | GPIO input | only if the opto-isolated supply sense is fitted; leave the label out otherwise |
| `RELAY1`, `RELAY2` | GPIO output | MOSFET gates |
| `VBUS_SENSE` | GPIO input, no pull | divided USB VBUS |

## 4. Peripherals

- **SPI1**: Full-Duplex Master, 8 bits, MSB first, NSS software, prescaler for the chips' maximum
  clock (open item; start at /32 = 2 MHz). The driver sets CPOL/CPHA per chip.
- **USART1**: Asynchronous, **Hardware Flow Control (RS485)** (its DE pin becomes an alternate
  function), 115200, 8 bits, none, 1 stop, FIFO mode **disabled**, Advanced: nothing else.
  RX pin **pull-up** (the transceiver's RO is off while the node transmits).
  NVIC: USART1 global interrupt enabled, priority 1. **NVIC → Code generation: untick
  "Generate IRQ handler" for USART1** — `rs485_g0.c` defines `USART1_IRQHandler`.
  (Another USART with receiver timeout support works: define `NODE_RS485_USART` and
  `NODE_RS485_IRQHandler` in main.h.)
- **USB**: Device (FS). Middleware **USB_DEVICE**: Class "Communication Device Class (Virtual Port
  Com)". VID/PID left at ST's defaults for now.
- **IWDG**: leave it **off** in CubeMX; `board_g0.c` starts it (about 100 ms, FW-23).
- **SYS**: Debug Serial Wire. Timebase SysTick (1 ms).

## 5. Code generation

Project Manager → Code Generator: "Generate peripheral initialization as a pair of .c/.h files"
off; keep user code when regenerating.

## 6. Source files and paths

Add as linked folders (Project → Properties → C/C++ General → Paths and Symbols → Source Location,
"Link Folder"), so the project builds the repository's files in place:

| Link | Points to |
|---|---|
| `node` | `iob-node/node` |
| `drivers` | `iob-node/drivers` |
| `port` | `iob-node/port/stm32g0` (exclude its `check` subfolder from the build) |
| `platform_core/mb_rtu.c` | `cems-platform/platform/core/mb_rtu.c` (link the one file) |

Include paths: `iob-node/node`, `iob-node/drivers`, `iob-node/port/stm32g0`,
`cems-platform/platform/include`.

Symbols: `NODE_PRODUCT_BUILD` for release builds (refuses to build while a chip driver is
unconfirmed); `NODE_FW_BUILD=<n>` from the release script.

Compiler: C11, `-Wall -Wextra -Wshadow -Wconversion -Wno-sign-conversion -Werror` (FW-53).

## 7. Linker script: `.noinit`

The reset counter and the bootloader request survive a reset in `.noinit`. In the generated
`STM32G0B1CETX_FLASH.ld`, after the `.bss` section, add:

```
  .noinit (NOLOAD) :
  {
    . = ALIGN(4);
    *(.noinit)
    *(.noinit*)
    . = ALIGN(4);
  } >RAM
```

## 8. main.c

```c
/* USER CODE BEGIN Includes */
#include "board_g0.h"
void node_main(void);
/* USER CODE END Includes */
...
int main(void)
{
  /* USER CODE BEGIN 1 */
  board_early_boot_check();      /* jumps to ST's bootloader if register 37 asked for it */
  /* USER CODE END 1 */
  HAL_Init();
  SystemClock_Config();
  MX_GPIO_Init();
  MX_SPI1_Init();
  MX_USART1_UART_Init();
  MX_USB_Device_Init();
  /* USER CODE BEGIN 2 */
  node_main();                   /* never returns */
  /* USER CODE END 2 */
```

In main.h, USER CODE BEGIN Private defines: `#define NODE_USB 1`.

In `USB_DEVICE/App/usbd_cdc_if.c`, `CDC_Receive_FS`, USER CODE section, before the two generated
lines:

```c
  usb_node_rx(Buf, *Len);
```
(and `#include "usb_g0.h"` in its USER CODE Includes).

## 9. Option bytes (ST's USB bootloader, FW-57)

The BOOT button drives BOOT0. On the STM32G0, the BOOT0 pin is only read when the option bit
`nBOOT_SEL` = 0; parts ship with `nBOOT_SEL` = 1. Set it once with STM32CubeProgrammer (Option
bytes → User configuration → nBOOT_SEL unticked). Check the remaining conditions against ST AN2606
(the "empty flash" rule and `nBOOT1`) before layout — open item in the requirements.

## 10. Check before the first board

`make target` in this repository compiles and links the same sources against ST's HAL headers
with a stand-in `main.h` (`port/stm32g0/check/`). Its pins are placeholders.
