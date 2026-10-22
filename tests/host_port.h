/* host_port.h - the host's node_port.h implementation, with hooks for simulated SPI chips. */
#ifndef HOST_PORT_H
#define HOST_PORT_H
#include "node_port.h"

typedef struct {
    int (*spi)(port_cs_t cs, uint8_t mode, const uint8_t *tx, uint8_t *rx, uint16_t n);   /* NULL: no chip */
    uint8_t  relays;
    bool     b_fault, b_supply;
    uint32_t spi_calls, delay_us;
    /* the address F-RAM (the 256 bytes used) */
    uint8_t  fram[256];
    bool     fram_absent;          /* does not answer */
    int      fram_cut;             /* >0: the next write stops after fram_cut - 1 bytes (a power cut) */
    uint32_t fram_writes;
} host_port_t;
extern host_port_t hp;

#endif
