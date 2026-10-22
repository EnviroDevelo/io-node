/* nvcfg.h - the node's settings kept in the I/O board's I2C F-RAM (FW-60): its Modbus address.
 *
 * Why a separate F-RAM and not the controller's flash: programming the STM32 (USB bootloader or SWD,
 * with or without a full chip erase) can never reach a separate chip, so a firmware update can
 * never change or lose the address. The firmware writes it only when register 38 is written
 * over USB, with the F-RAM's write-protect pin released for that write alone.
 *
 * Part: 4-Kbit I2C F-RAM (Fujitsu MB85RC04V or Infineon FM24CL04B, 512 bytes, SOIC-8, 3.3 V):
 * writes at bus speed with no write cycle or page limit, 10^12 writes or more, data kept for
 * decades without power. Only the first 256 bytes are used.
 * Layout: two 8-byte slots at 0x00 and 0x08, each written in one go:
 *   [0] 'N' [1] 'C' [2] version (1) [3] sequence [4] address [5] ~address [6..7] CRC-16 of 0..5
 * A save goes to the slot not holding the newest good record, with the sequence one higher, and
 * is read back. A power cut during a save spoils at most that slot; the other still holds the
 * previous address. A record with a later version is read as long as bytes 0-7 keep this
 * meaning, so newer firmware may add records after 0x40 but must keep these.
 *
 * Serial number (FW-62), one record at 0x20, written once in production:
 *   [0] 'S' [1] 'N' [2] version (1) [3] 0 [4..19] 16 characters, NUL padded [20..21] CRC-16 of 0..19
 * A power cut while it is written leaves no good record, which reads as "not set" and can be
 * written again. */
#ifndef NVCFG_H
#define NVCFG_H
#include <stdint.h>

#define NVCFG_OK      0      /* a good record was found */
#define NVCFG_BLANK   1      /* the F-RAM answers but holds no good record: address 0 (not set) */
#define NVCFG_NO_CHIP (-1)   /* the F-RAM does not answer */

int nvcfg_load(uint8_t *address);                    /* at start-up */
int nvcfg_store_address(void *ctx, uint8_t address); /* NODE_HW_OK, NODE_HW_NO_ANSWER or NODE_HW_READBACK */
int nvcfg_load_serial(uint8_t serial[16]);           /* NVCFG_*; all 0 unless a good record */
int nvcfg_store_serial(void *ctx, const uint8_t serial[16]);   /* NODE_HW_* */

#endif
