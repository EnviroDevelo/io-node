# iob-node: CEMS I/O board node firmware

Firmware for the CEMS Supervisor I/O board's own controller: a Modbus slave that drives 8 outputs of
0.6 A (bank A, ISO2H823V2.5), 8 outputs of 1 A (bank B, ISO808A-1) and 2 relays, reads 8 isolated
inputs (ISO1I813T), and turns everything off by itself when the main board goes quiet.

**24 Oct 2026 (Gary): the board moves to the STM32H563 (H563RGT6 + LAN8742A) and Ethernet with
Modbus TCP replaces the RS-485 pair** (the platform's D27). The H5 is the standard for all
development; the NUCLEO-H563ZI bench port (`port/nucleo_h563`) runs it today, Ethernet included.
The core is unchanged: `node/node_net.c` turns Modbus TCP into the RTU frames `node.c` handles
and answers the supervisor's discovery, so the board's IP is set from the supervisor's HTTPS page
(see "Ethernet" below). `port/stm32g0` is the earlier G0B1 + RS-485 design, kept until the
product's H563 port replaces it.

**The contract** is *CEMS I/O Module Requirements: Firmware and Hardware* (FW-nn, HW-nn, AT-nn
below). Register numbers and bits come from the platform's `platform/include/iobus.h`, and the CRC
from `platform/core/mb_rtu.c`, used in place from `..\cems-platform` (the two repositories sit side
by side in `CEMS Supervisor\Firmware`). The platform's simulated node, `iob_node_sim.c`, is the
reference this firmware is compared against byte for byte.

## Layout

| Path | What |
|---|---|
| `node/node.c`, `node.h` | The core: register map, Modbus slave (functions 03, 04, 06, 16, 23), write rules, hold time, gate, HEALTHY condition, chip health, USB rules. Pure C, no hardware. |
| `node/node_net.c`, `node_net.h` | Ethernet: the Modbus TCP server (frames into `node_frame` on the master port) and the discovery responder (FIND, SET of IP settings and address), per the platform's `iob_net.h`. Pure C. |
| `node/node_hw.h` | What the core needs from the board: the three chips, bank B's status pins and the relays. |
| `drivers/` | Chip drivers, written and checked against the three datasheets (see "Chip drivers"). |
| `port/stm32g0/` | STM32G0B1: main loop, RS-485 USART, USB virtual COM port, address F-RAM (I2C), straps, unique ID, reset cause, watchdog, bootloader entry. `CUBEMX_SETUP.md` makes the CubeIDE project. |
| `port/nucleo_h563/` | A NUCLEO-H563ZI as a bench stand-in for the board: the same node core on GPIO pins, Ethernet through the Nucleo's LAN8742A (lwIP without an OS: `net_bench.c`, `lwip/eth_h5.c`), RS-485 through a transceiver module, the ST-LINK port as the USB port (`CUBEMX_SETUP.md` there) |
| `tools/at02.py` | AT-02 conformance test for a real board, from a PC (below). |
| `tools/at13.py` | AT-13 digital input bench test, from a PC (below). |
| `tools/set_address.py` | Read or set a board's address over its USB-C port (FW-60, below). |
| `drivers/nvcfg.c` | The address record in the board's F-RAM: two copies, CRC, sequence number. |
| `tests/` | Host tests (below). `tests/platform/iob_node_real.c` puts this firmware in place of the platform's simulated node. |

## Ethernet

The supervisor talks to the board over Modbus TCP, port 502, unit id = the board's address; every
RS-485 rule (addresses, write rules, hold time, comms watchdog, input changes) holds unchanged,
because `node_net.c` hands each frame to `node_frame` on the master port. One connection at a
time (a new one replaces the old). A new board has no IP: it serves no Modbus TCP but answers
discovery on UDP 50223, so the supervisor's web page (Administration → I/O boards on the network)
lists it with its chip ID and MAC and gives it a board slot's IP and address. The board saves them
(F-RAM on the product, backup registers on the bench), reads them back, answers with them and
changes over. From then on it takes settings and connections only from that supervisor's IP.
Setting the address to 0 over USB (`set_address.py --clear`) also clears the IP: a new board again.
The MAC is 02:xx:.. from the chip's unique ID unless an EUI-48 chip is fitted.

## Building and testing

```
make test          # host: requirement tests, 200,000 random frames compared with iob_node_sim.c, the chip drivers against frame-level chip models
make master-test   # the platform's whole host suite with every simulated I/O board replaced by this firmware
make target        # arm-none-eabi-gcc: compile and link for the STM32G0B1 against ST's HAL headers
make bench         # the NUCLEO-H563ZI bench board build, compile and link (port/nucleo_h563)
make vnode         # a virtual board on a pseudo-terminal, for trying tools/at02.py and at13.py (Linux/macOS)
make vnode_net     # a virtual board on Ethernet (real sockets), the real core and node_net.c
make net-e2e       # the supervisor's netsim finds that board, gives it board 1's IP over HTTPS and runs it over Modbus TCP
```

`make target` needs `..\ext` with the xPack arm-none-eabi-gcc 13.3.1 and ST's
`stm32g0xx_hal_driver`, `cmsis_device_g0` and `cmsis-core` repositories (see `port/stm32g0/check.mk`).

Results on 22 Oct 2026:

| Check | Result |
|---|---|
| `make test` (gcc, -Werror, AddressSanitizer, UBSan) | 953 checks, 0 failed (node 666, comparison 17, drivers, F-RAM records and the whole board on them 270) |
| Comparison with `iob_node_sim.c` | 200,000 random frames (reads, writes, function 23, bad CRCs, runts, wrong addresses, unknown functions, hold time running out) with a changing outside world: every reply byte-identical, every register equal after every frame |
| `make master-test` | 13,993 platform checks, 0 failed, with this firmware answering the master in every I/O board test (connect, settings, outputs, inputs, faults, lost and back, latched comms fault, short input openings, address change and commissioning, node restart, hold time) |
| `make target` | Compiles and links with -Werror for Cortex-M0+, also as a product build (`EXTRA=-DNODE_PRODUCT_BUILD`): 10.3 KB flash, 3.5 KB RAM (of 256 KB and 144 KB) |
| On a board | Not yet: no hardware |

## AT-13 on a board: tools/at13.py

The digital inputs: each one on and off, 5 ms and 12 ms pulses with the 8 ms filter, and wire
break with the check on and off. Run it over the board's **USB-C port** (it reads register 17
every 1-2 ms there; a USB-RS485 adapter needs its latency timer at 1 ms):

```
python tools\at13.py COM7 --usb               # you switch the inputs; the script sees each change
python tools\at13.py COM7 --usb --loopback    # bank B outputs 9-16 wired to inputs 1-8: no pulse source needed
```

Manual mode needs a 24 V source you can pulse for 5 ms and 12 ms into input 1 (a function
generator driving a transistor, a PLC output, a timer relay); the script watches 15-20 s while you
send them, then asks how many you sent. In loopback mode the script switches the inputs through
bank B itself (the only test that turns outputs on, and only those) and times the pulses on the
PC. The wire-break step is always by hand. Settings it changes are put back.

Tried here against the firmware on a pseudo-terminal with a model of the ISO1I813T's filter
(`build/vnode -L`, and a scripted operator for the manual steps): all 21 checks pass over USB and
over RS-485, and with the filter switched off (`-m 5`) the 5 ms check fails as it should. A 12 ms
pulse is visible in register 17 for about 11-12 ms (the chip's up/down-counter filter delays both
edges by its 10 ms).

## Setting a board's address: tools/set_address.py

The address is set in software (no DIP switch) and kept in the board's F-RAM (MB85RC04V or
FM24CL04B), a separate chip that programming the STM32 can never reach, so a firmware update,
even with a full chip erase, never changes it. A new board has address 0: silent on RS-485, comms
LED steady on. Over its USB-C port:

```
python tools\set_address.py COM7               # shows UID, hardware and firmware, serial number, address
python tools\set_address.py COM7 --address 3   # set (1..245), saved and read back
python tools\set_address.py COM7 --clear       # 0: answers on RS-485 only at 246 (commissioning)
python tools\set_address.py COM7 --serial CIO-26-00042   # production: permanent after the next power-off
```

Over RS-485 the address changes only through one guarded write (FW-61): an FC16 of registers 40-42
holding the key 0xA55A, the new address and the new address XOR 0xFFFF. Anything else written
there is refused and changes nothing, so no other Modbus master changes an address by accident.
The main board's browser pages use it. A board without an address answers on RS-485 at 246, for
reads and that write only, so the main board can find a new board by scanning and give it an
address (one new board on the bus at a time).

The board refuses a change (04) while the main board is controlling it: unplug RS-485 first.

## Main loop (port/stm32g0/node_main.c)

One pass every millisecond: `node_poll` (hold time, chip reads, bank B pins), HEALTHY toggled if the
gate is open, the watchdog refreshed, LEDs. Frames are handled the moment the
USART's 1.75 ms receiver timeout ends them, so the reply starts about 1.8 ms after the request.
HEALTHY and the watchdog are touched nowhere else, so a stuck loop drops the hardware gate within
about 50 ms and resets the node within about 100 ms.

## Chip drivers

All three share SPI1 (mode 0, 4 MHz; the ISO1I813T's limit is 5 MHz), each with its own chip select.
Each driver's header comment cites the datasheet sections it follows.

| Chip | Datasheet | Driver | What it does |
|---|---|---|---|
| ISO2H823V2.5 (bank A) | Rev 2.1, 2024-11-01 | `drv_iso2h823.c` | SPI mode 3 of the chip: 24-bit frames with CRC-5 both ways. Config: version check (GLERR 0100b), power-up flags read away, DIAGCFG FFh, GLCFG LEDGOFF (+OLOFF if no channel is checked), all read back. DRIVE written every 1 ms pass. Every 5 ms: COLDIAG, then DIAGx of flagged channels, GLERR, INTERR. |
| ISO1I813T (inputs) | V2.2, 2017-01-03 | `drv_iso1i813t.c` | SPI mode 3 of the chip, same frame and CRC. Config: 8 COEFIL registers (filter + wire-break mask), read back. Every 2 ms: DIAG (the reply also carries the inputs and UV), INTERR. |
| ISO808A-1 (bank B) | DS14170 Rev 6, July 2025 | `drv_iso808.c` | One 8-bit frame of output states every 1 ms pass, SS high 20 us after it. FAULT and PGOOD pins. Nothing to service: the chip's watchdog is internal. |

How the chips' diagnostics become the node's registers:

| Register | From |
|---|---|
| A_OPEN | OLA (open, channel on) or OLI (open, channel off); masked by OL_ENABLE |
| A_OVERLOAD | OCL (current limit), or SCV (short to 24 V) when no open load was seen in the same read: an unplugged valve with the 12 nF output capacitor also shows SCV briefly after switching off (ISO2H823 table 10) |
| A_OVERTEMP | OT |
| OUT_A_CHIP | no answer or bad CRC twice in a row, configuration read back wrong, package over-temperature (OTP), or isolation transmission errors (TE) in two reads in a row |
| FIELD_A_OK | neither chip reports UV (VBB below 16 V on bank A, 15 to 17 V on the inputs) nor its far side not ready (W4P, W4S) |
| INPUTS, WIREBREAK | INPDATA and DIAG of the input chip; FILTER_MS to the nearest filter at or above (0 bypass, 1 → 1.6 ms, 2-3 → 3.2 ms, 4-10 → 10 ms, 11-100 → 20 ms, the longest): the default 8 ms is 10 ms |
| IN_CHIP | no answer or bad CRC twice in a row, configuration read back wrong, TE in two reads in a row |
| B_FAULT bit 0 | the ISO808A-1 FAULT pin, debounced 1 ms (over-temperature, a bad SPI frame, an isolation error) |
| B_FAULT bit 1 | always 0: the ISO808A-1's watchdog cannot be seen from outside |
| FIELD_B_OK | the ISO808A-1 PGOOD pin (off at 14.5 to 16.1 V, on at 16.6 to 18.7 V) |

### Board strapping these drivers assume

| Chip | Pins |
|---|---|
| ISO2H823V2.5 | SEL high, MS0 and MS1 high, SYNC high, **ALE/RST low** (high with /CS low for 100 us resets the chip), /ODIS from the safe-state gate, /CRCERR and /ERR may be left unconnected |
| ISO1I813T | SEL high, MS0 and MS1 high, SSI low, SYNC high, DC_ENA low, ROSC 22.1 kOhm (the filter times are for its nominal 500 kHz) |
| ISO808A-1 | SS, CLK, SDI, SDO on the node's SPI bus; **OUT_EN from the safe-state gate**; FAULT and PGOOD open drain, pulled up to 3.3 V, to controller pins |

## Requirement trace

| Req | Where | Tested |
|---|---|---|
| FW-01 | `node_init` (all off, gate closed); HEALTHY only from `node_healthy` | `t_startup`, `t_board` |
| FW-02 | `board_boot_info` (address from the F-RAM once); `node_frame` (0 = not set: on RS-485 only at 246, reads and the address change only); `node_comms_led` steady on | `t_rules`, `t_address`, `t_commission` |
| FW-03 | `node_init` defaults, WDG_RESET from the reset cause; `reset_cause` clears the flags | `t_startup` |
| FW-04 | `config_a` / `config_in` before anything opens; retry every 1 s; drivers read every setting back | `t_chips`, `t_iso2h823`, `t_iso1i813t` |
| FW-10, 11 | `rs485_g0.c` receiver timeout 202 bit times; reply from the main loop | target build; AT-03 on the bench |
| FW-12 | `node_frame` drops and counts; `rs485_irq` marks FE/NE/ORE | `t_rules`, comparison |
| FW-13 | `rs485_init`: CR3.DEM | target build |
| FW-14 | `rs485_irq` ignores bytes while a frame waits or a reply goes out | target build |
| FW-20 | `write_apply` → `apply_outputs` before CMD_ECHO | `t_outputs` |
| FW-21 | `hold_check` every 1 ms pass and before every frame | `t_hold` (incl. 10 min and the 2^32 ms wrap) |
| FW-22 | `node_healthy` (gate open, not entering the bootloader; a bank A fault does not stop it); `node_main` toggles once per pass | `t_chips`, `t_hold` |
| FW-23 | `board_iwdg_start` (100 ms), refreshed only in `node_main` | target build; AT-07 |
| FW-24 | Nothing to service on the ISO808A-1 (its watchdog is internal, DS14170 6.5.1) | `t_iso808a` |
| FW-25 | `apply_outputs` (relays follow RELAY_CMD, off with the gate) | `t_outputs`, `t_hold` |
| FW-26 | only a write of register 0 opens the gate | `t_hold` |
| FW-30..32 | input read every 2 ms; filter mapping; masks; 0 without supply A | `t_chips`, `t_iso1i813t`, `t_board` |
| FW-33 | `in_track` and the consume in `node_frame`: every input change shown once to the RS-485 master; field A or input chip fault reads 0 at once; USB reads the live inputs | `t_inputs_hold`, comparison; platform `t_input_pulse` |
| FW-40..43 | diagnostics every 5 ms; present state; FAULT 1 ms debounce; two failures | `t_chips`, `t_iso2h823`, `t_board` |
| FW-44 | FAULT_CLEAR clears TIMEOUT and WDG_RESET only | `t_hold` |
| FW-50 | static 256-byte buffers, no heap | code |
| FW-51 | `node_reg`, reserved read 0 | `t_startup`, comparison |
| FW-52 | `node/node_version.h` | `t_startup` |
| FW-53 | `make target` -Werror; iobus.h and mb_rtu.c used in place | target build |
| FW-55 | `node_comms_led` (flash twice a second / double flash while a loss is latched / 4 Hz lost / off / steady while the address is not set), fault LED; power LED is hardware | `t_comms`; `node_main` |
| FW-56 | USB at the node's address or 247; exception 04 while the master is in control | `t_usb` |
| FW-57 | register 37 over USB; outputs off 100 ms, then a reset into the bootloader | `t_usb`, `t_rules` |
| FW-60 | register 38 directly over USB only (0..245, 04 while the master is in control); `nvcfg.c` saves it to the F-RAM and reads it back before the reply; 04 and nothing changed if that fails; `board_g0.c` I2C with the write-protect pin released for the write only | `t_address`, `t_nvcfg` (power cut at every byte, 600 saves, spoilt slot, no chip), `t_board_address`; target build |
| FW-61 | `addr_set`: ONE FC16 of 40-42 = key 0xA55A, new address, address XOR 0xFFFF; anything else into 40-42 is 02, a wrong key, complement or range 03, a failed save 04; reply from the old address; over USB also 04 while the master is in control | `t_addr_set`, `t_commission`, comparison (accepted and refused changes); platform `t_node_cfg`; at02 |
| FW-62 | `serial_write` (USB only, printable ASCII, permanent after a power cycle), `nvcfg_store_serial` | `t_serial`, `t_nvcfg` |
| FW-58 | `node_comms_ok`, `comms_check`: losing the link latches STATUS COMMS_LOST; only FAULT_CLEAR bit 1 or a restart clears it; outputs follow the hold time | `t_comms`, comparison; platform `t_comms_latch` |
| FW-59 | `node_heartbeat_led`: 1 Hz from the main loop, nothing else | `t_comms`; target build |

AT-02 (register rules from a PC) is what `t_rules` and the comparison do in software, and
`tools/at02.py` does on the board; the rest of the acceptance tests need the board.

## AT-02 on a board: tools/at02.py

A USB-RS485 adapter on the board's bus terminals, **main board disconnected**, then:

```
pip install pyserial
python tools\at02.py COM5 --address 3      # RS-485; the board's address
python tools\at02.py COM7 --usb            # the board's USB-C port (address 247)
```

71 checks over RS-485 (69 over USB): identification; every reserved register reads 0; FC03 and
FC04 agree; reads past register 79 or with a bad count; writes to every kind of read-only
register (02); every setting just out of range (03) and nothing changed, also when one bad value
sits in a multi-register write; nine unknown functions (01); malformed requests (03); another
address, a broadcast and two bad-CRC frames get no reply and RX_ERRORS goes up by exactly 2; the
accepted writes echo correctly and function 23 writes before it reads. It prints PASS / FAIL per
check, writes the same report to `at02-<time>.txt`, and exits 0 only when everything passed.
`-v` shows every frame.

It never switches anything on: its only output write is all-off, it puts settings 32-35 back at
the end, and it never sends the bootloader key or FAULT_CLEAR = 1.

Tried here against the firmware itself on a pseudo-terminal (`make vnode`, then
`python3 tools/at02.py <the pty it prints> -a 1`): all 71 pass over RS-485 and all 69 over USB;
with `build/vnode -m 1..4` the node deliberately breaks one rule each (answers another address,
does not count bad CRCs, answers unknown functions with 02, half-applies a refused write) and the
script fails exactly those checks.

## Differences from the simulated node (to raise, per the requirements' rule)

With healthy chips the two are byte-identical. They differ where the simulator has no model:

1. **A bank A chip fault is reported, not acted on** (Gary, 22 Oct). OUT_A_CHIP is set, the master
   raises its alarm ("outs 1-8 chip fault", outputs 1-8 marked faulty), and HEALTHY, bank B and the
   relays keep running. OUT_APPLIED shows bank A as not driven while the fault lasts (bits 0-7 read
   0); the simulator still shows them. Requirement FW-22 is updated to match.
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
| N3 | The address is software only (Gary, 23 Oct): F-RAM, set over USB; a new board is 0 (silent on RS-485). HW_REV = 1 + strap value (two straps: 1..4). |
| N4 | UID: register 69 = bits 0-15 of the first UID word, 70 = bits 16-31, and so on. |
| N5 | USB frames end after 2 ms without a packet; USB frames are only answered while VBUS is present. |
| N6 | The activity LED pulses 20 ms for each valid frame addressed to the node on either port. The fault LED shows a chip fault, field supply A missing, a latched TIMEOUT or a bank B fault. |
| N7 | Input reads every 2 ms (FW-30: 5 ms), bank A diagnostics every 5 ms (FW-40: 10 ms). |

## Open items

1. **Bank B board change.** The ISO808A-1 is an SPI device: HW-08 and the pin budget in the
   requirements are updated (CS_B, PGOOD, FAULT, OUT_EN from the gate; the 8 parallel inputs, SYNC,
   LOAD and the watchdog pin are gone). The design specification's bank B section still describes
   parallel inputs and SYNC/LOAD.
2. On the bench: the ISO808A-1 channel order (bit 0 = OUT1 assumed), the ISO2H823 reply CRC (same
   engine as the request assumed, as the datasheet describes), the OLADJ value for a VX21 coil.
3. HSI16 accuracy over temperature at 115200 baud (HW-01): bench test, or the crystal footprint.
4. AN2606 conditions for the G0B1's USB bootloader, and the nBOOT_SEL option byte (CUBEMX_SETUP.md 9).
5. The RS-485 RX pin needs a pull-up (on the board or the pin's own).
6. ISO2H823 datasheet inconsistency: GLERR.MV is "1 = no missing voltage" in table 5.1.4 but "1 =
   missing" in 3.2.3 and the reset value; the driver does not use MV (UV at 16 V trips first).
