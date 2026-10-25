/* sim_ads124s08.h - the ADS124S08 temperature front end modelled at frame level (commands, registers,
 * single-shot conversions with their conversion time, STATUS + data + CRC), with the board's three
 * TEMP connectors and the sensors on them. Hooked into host_port's SPI (PORT_CS_TEMP) by sim_chips.c. */
#ifndef SIM_ADS124S08_H
#define SIM_ADS124S08_H
#include <stdbool.h>
#include <stdint.h>

enum { SC_NONE = 0, SC_TC, SC_RTD2, SC_RTD3, SC_RTD4, SC_NTC };

typedef struct {
    int    kind;                 /* SC_*: what is wired to the connector */
    double tc_mv[2];             /* thermocouples A and B: their voltage at the board (mV) */
    bool   tc_open[2];
    double ohm;                  /* RTD or NTC resistance */
    double lead[3];              /* lead resistances (RTD 3-wire: leads 1, 2, 3) */
    bool   open;                 /* RTD/NTC broken */
} sim_conn_t;

typedef struct {
    /* the world */
    sim_conn_t conn[3];
    double   die_c;              /* the ADC's own temperature (the cold junction) */
    double   idac_err;           /* actual / nominal IDAC current (1.0) */
    bool     dead;               /* no chip: MISO high */
    int      corrupt_replies;    /* the next N RDATA replies have a bit flipped */
    uint32_t now;                /* ms, set by the test before each poll */
    /* chip */
    uint8_t  reg[18];
    int32_t  data, pending;
    uint32_t ready_at;
    bool     converting;
    /* what the driver did */
    uint32_t frames, wrong_mode, starts, resets, early_reads, bad_mux, bad_idac, rails;
    uint8_t  last_vbias, last_pga, last_idacmag;
} sim_ads_t;

extern sim_ads_t sads;
void sim_ads_reset(void);        /* power-up state (world cleared) */
int  sim_ads_frame(uint8_t mode, const uint8_t *tx, uint8_t *rx, uint16_t n);
uint8_t sim_ads_crc8(const uint8_t *d, unsigned n);

#endif
