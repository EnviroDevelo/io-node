/* sim_chips.h - the I/O board's SPI chips modelled at frame level from their datasheets, for the
 * driver tests: ISO2H823V2.5 (bank A) and ISO808A-1 (bank B). Plugs into host_port's SPI hook. */
#ifndef SIM_CHIPS_H
#define SIM_CHIPS_H
#include <stdbool.h>
#include <stdint.h>

/* per-channel conditions, as the DIAGx bits */
#define SA_OT  0x01u
#define SA_OLI 0x02u
#define SA_OLA 0x04u
#define SA_OCL 0x08u
#define SA_SCV 0x10u

typedef struct {
    /* the world */
    uint8_t cond[8];             /* conditions present now, per channel */
    bool    uv, w4p, otp;        /* VBB below 16 V, power chip not ready, package hot */
    int     te_events;           /* transmission errors still to happen (one per frame) */
    bool    dead;                /* no chip: MISO floats high */
    int     corrupt_replies;     /* the next N replies have a bit flipped */
    uint8_t version;             /* GLERR high nibble (0x40 for this chip) */
    /* chip registers */
    uint8_t drive, diagcfg, glcfg, diag[8];
    uint8_t sticky[8];           /* internal sticky diagnostics */
    bool    s_uv, s_te, s_w4p;   /* sticky GLERR / INTERR flags */
    /* what the driver did */
    uint32_t frames, bad_crc_in, wrong_mode, wrong_len, drive_writes;
} sim_iso2h823_t;

typedef struct {
    /* the world */
    uint8_t states, wb;          /* filtered inputs, wire breaks present now */
    bool    uv, w4s, dead;
    int     te_events, corrupt_replies;
    /* chip registers */
    uint8_t coefil[8], glcfg, s_diag;
    bool    s_uv, s_w4s, s_te;
    uint32_t frames, bad_crc_in, wrong_mode, wrong_len, writes;
} sim_iso1i813t_t;

typedef struct {
    uint8_t  out;                /* the output register */
    uint32_t frames, wrong_mode, wrong_len;
} sim_iso808a_t;

extern sim_iso2h823_t sa;
extern sim_iso808a_t  sb;
extern sim_iso1i813t_t si;
void sim_chips_reset(void);      /* power-up state, and hooks the host SPI */

#endif
