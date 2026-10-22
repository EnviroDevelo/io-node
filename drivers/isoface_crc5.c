/* isoface_crc5.c - the CRC-5 of Infineon's ISOFACE SPI mode 3 (24-bit frames): polynomial
 * x^5 + x^4 + x^2 + 1, register preset 11111, over the first 19 bits, MSB first.
 * ISO2H823V2.5 datasheet 3.2.2.3; ISO1I813T datasheet 3.8.2 (figure 28) - the same engine. */
#include "drivers.h"

uint8_t isoface_crc5(const uint8_t *data, unsigned nbits)
{
    uint8_t crc = 0x1Fu;
    for (unsigned i = 0; i < nbits; i++) {
        unsigned bit = (data[i / 8u] >> (7u - i % 8u)) & 1u;
        unsigned fb = ((crc >> 4) & 1u) ^ bit;
        crc = (uint8_t)((crc << 1) & 0x1Fu);
        if (fb) crc ^= 0x15u;           /* x^4 + x^2 + 1 below the x^5 term */
    }
    return crc;
}
