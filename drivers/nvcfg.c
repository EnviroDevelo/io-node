/* nvcfg.c - see nvcfg.h */
#include "nvcfg.h"
#include <string.h>
#include "mb_rtu.h"
#include "node_hw.h"
#include "node_port.h"

#define SLOT_SIZE 8u
#define VERSION   1u

static int8_t  newest = -1;          /* slot holding the newest good record (-1: none) */
static uint8_t seq;                  /* its sequence number */
static uint8_t cur;                  /* its address */
static bool    known;                /* the slots have been read since start-up */

static bool good(const uint8_t *r)
{
    if (r[0] != 'N' || r[1] != 'C' || r[2] < VERSION) return false;
    if ((uint8_t)~r[4] != r[5]) return false;
    uint16_t c = mbr_crc16(r, 6);
    return r[6] == (uint8_t)c && r[7] == (uint8_t)(c >> 8);
}

int nvcfg_load(uint8_t *address)
{
    uint8_t b[2 * SLOT_SIZE];
    newest = -1;
    *address = 0;
    known = false;
    if (port_fram_read(0, b, sizeof b) != 0) return NVCFG_NO_CHIP;
    known = true;
    bool g0 = good(b), g1 = good(b + SLOT_SIZE);
    if (!g0 && !g1) return NVCFG_BLANK;
    if (g0 && g1) newest = (int8_t)((int8_t)(b[SLOT_SIZE + 3] - b[3]) > 0 ? 1 : 0);   /* sequence wraps */
    else newest = g0 ? 0 : 1;
    const uint8_t *r = b + (unsigned)newest * SLOT_SIZE;
    seq = r[3];
    cur = r[4];
    *address = cur;
    return NVCFG_OK;
}

int nvcfg_store_address(void *ctx, uint8_t address)
{
    (void)ctx;
    if (!known) {                     /* not readable at start-up: never guess which slot is older */
        uint8_t a;
        if (nvcfg_load(&a) < 0) return NODE_HW_NO_ANSWER;
    }
    if (newest >= 0 && cur == address) return NODE_HW_OK;          /* nothing to change: no wear */
    uint8_t slot = newest == 0 ? 1u : 0u;
    uint8_t r[SLOT_SIZE], back[SLOT_SIZE];
    r[0] = 'N'; r[1] = 'C'; r[2] = VERSION;
    r[3] = (uint8_t)(newest >= 0 ? seq + 1u : 0u);
    r[4] = address;
    r[5] = (uint8_t)~address;
    uint16_t c = mbr_crc16(r, 6);
    r[6] = (uint8_t)c;
    r[7] = (uint8_t)(c >> 8);
    if (port_fram_write((uint8_t)(slot * SLOT_SIZE), r, SLOT_SIZE) != 0) return NODE_HW_NO_ANSWER;
    if (port_fram_read((uint8_t)(slot * SLOT_SIZE), back, SLOT_SIZE) != 0) return NODE_HW_NO_ANSWER;
    if (memcmp(r, back, SLOT_SIZE) != 0) return NODE_HW_READBACK;
    newest = (int8_t)slot;
    seq = r[3];
    cur = address;
    return NODE_HW_OK;
}

/* ---------------- serial number (FW-62) ---------------- */
#define SN_AT   0x20u
#define SN_SIZE 22u

int nvcfg_load_serial(uint8_t serial[16])
{
    uint8_t r[SN_SIZE];
    memset(serial, 0, 16);
    if (port_fram_read(SN_AT, r, SN_SIZE) != 0) return NVCFG_NO_CHIP;
    uint16_t c = mbr_crc16(r, 20);
    if (r[0] != 'S' || r[1] != 'N' || r[2] < VERSION || r[20] != (uint8_t)c || r[21] != (uint8_t)(c >> 8))
        return NVCFG_BLANK;
    memcpy(serial, r + 4, 16);
    return NVCFG_OK;
}

int nvcfg_store_serial(void *ctx, const uint8_t serial[16])
{
    (void)ctx;
    uint8_t r[SN_SIZE], back[SN_SIZE];
    r[0] = 'S'; r[1] = 'N'; r[2] = VERSION; r[3] = 0;
    memcpy(r + 4, serial, 16);
    uint16_t c = mbr_crc16(r, 20);
    r[20] = (uint8_t)c;
    r[21] = (uint8_t)(c >> 8);
    if (port_fram_write(SN_AT, r, SN_SIZE) != 0) return NODE_HW_NO_ANSWER;
    if (port_fram_read(SN_AT, back, SN_SIZE) != 0) return NODE_HW_NO_ANSWER;
    return memcmp(r, back, SN_SIZE) == 0 ? NODE_HW_OK : NODE_HW_READBACK;
}
