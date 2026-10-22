/* sim_chips.c - see sim_chips.h. ISO2H823V2.5 behaviour per its datasheet Rev 2.1: figure 15 frames,
 * 3.2.2.3 CRC, 3.2.4 sticky diagnostics (COLDIAG read loads DIAG0..7), 5.1 registers and reset
 * values (GLERR 46h, INTERR 07h, DIAGCFG FFh). ISO808A-1 per DS14170: 8-bit frame, mode 0. */
#include "sim_chips.h"
#include <string.h>
#include "host_port.h"

#include "drivers.h"

sim_iso2h823_t sa;
sim_iso808a_t  sb;
sim_iso1i813t_t si;

/* ISO1I813T per its datasheet V2.2: mode 3 frames (figure 27), registers (chapter 6), reset values
 * COEFIL 1Fh, GLERR 06h, INTERR 06h; DIAG bits only for channels with MWB set; reading DIAG (or any
 * frame, table 6-3 mode 3) clears the sticky DIAG, UV, MV, W4S once gone; reading INTERR clears TE. */
static int iso1i813t_frame(uint8_t mode, const uint8_t *tx, uint8_t *rx, uint16_t n)
{
    si.frames++;
    if (mode != 0) si.wrong_mode++;
    if (n != 3) { si.wrong_len++; memset(rx, 0xFF, n); return 0; }
    if (si.dead) { memset(rx, 0xFF, 3); return 0; }
    uint8_t mask = 0;
    for (int c = 0; c < 8; c++) if (si.coefil[c] & 0x10u) mask |= (uint8_t)(1u << c);
    si.s_diag |= (uint8_t)(si.wb & mask);
    if (si.uv) si.s_uv = true;
    if (si.w4s) si.s_w4s = true;
    if (si.te_events > 0) { si.te_events--; si.s_te = true; }
    bool crc_ok = isoface_crc5(tx, 19) == (tx[2] & 0x1Fu);
    uint8_t a = (uint8_t)(tx[0] & 0x7Fu), val = 0;
    rx[0] = si.states;
    if (!crc_ok) si.bad_crc_in++;
    else if (tx[0] & 0x80u) {
        si.writes++;
        if (a >= 0x06u && a <= 0x14u && !(a & 1u)) si.coefil[(a - 0x06u) / 2u] = (uint8_t)(tx[1] & 0x1Fu);
        else if (a == 0x18u) si.glcfg = (uint8_t)(tx[1] & 0x1Cu);
        val = si.s_diag;                       /* a write shifts out DIAG */
    } else {
        switch (a) {
        case 0x00: val = si.s_diag; break;
        case 0x02: val = si.states; break;
        case 0x04: val = (uint8_t)((si.s_uv ? 4u : 0u) | (si.s_diag ? 1u : 0u)); break;
        case 0x16: val = (uint8_t)((si.s_w4s ? 4u : 0u) | (si.s_te ? 2u : 0u)); si.s_te = false; break;
        case 0x18: val = si.glcfg; break;
        default:
            if (a >= 0x06u && a <= 0x14u && !(a & 1u)) val = si.coefil[(a - 0x06u) / 2u];
            break;
        }
    }
    rx[1] = val;
    rx[2] = (uint8_t)((si.s_uv ? 0x80u : 0u) | (si.s_diag ? 0x20u : 0u));
    rx[2] = (uint8_t)(rx[2] | isoface_crc5(rx, 19));
    if (crc_ok && (a == 0x00u || (tx[0] & 0x80u))) si.s_diag = (uint8_t)(si.wb & mask);   /* cleared once gone */
    si.s_uv = si.uv;
    si.s_w4s = si.w4s;
    if (si.corrupt_replies > 0) { si.corrupt_replies--; rx[0] ^= 0x01u; }
    return 0;
}

static void sample(void)          /* the power chip reports every 35 us: fold the world into the sticky bits */
{
    for (int c = 0; c < 8; c++) sa.sticky[c] |= sa.cond[c];
    if (sa.uv) sa.s_uv = true;
    if (sa.w4p) sa.s_w4p = true;
    if (sa.te_events > 0) { sa.te_events--; sa.s_te = true; }
}

static uint8_t visible(int c)    /* what reaches COLDIAG: enabled channels, OL unless OLOFF */
{
    if (!(sa.diagcfg & (1u << c))) return 0;
    uint8_t v = sa.sticky[c];
    if (sa.glcfg & 0x02u) v &= (uint8_t)~(SA_OLI | SA_OLA);
    return v;
}

static uint8_t coldiag(void)
{
    uint8_t cd = 0;
    for (int c = 0; c < 8; c++) if (visible(c)) cd |= (uint8_t)(1u << c);
    return cd;
}

static uint8_t read_reg(uint8_t a)
{
    switch (a & 0x7Eu) {          /* even addresses, LSB ignored */
    case 0x00: return sa.drive;
    case 0x02: {                  /* read: DIAG0..7 loaded, sticky cleared down to what is still present */
        uint8_t cd = coldiag();
        for (int c = 0; c < 8; c++) { sa.diag[c] = visible(c); sa.sticky[c] = sa.cond[c]; }
        return cd;
    }
    case 0x04: {
        uint8_t v = (uint8_t)(sa.version | (sa.s_uv ? 0x04u : 0u) | (coldiag() ? 0x01u : 0u));
        sa.s_uv = sa.uv;
        return v;
    }
    case 0x06: return sa.diagcfg;
    case 0x18: {
        uint8_t v = (uint8_t)((sa.s_te ? 0x01u : 0u) | (sa.s_w4p ? 0x02u : 0u) | (sa.otp ? 0x10u : 0u));
        sa.s_te = false;
        sa.s_w4p = sa.w4p;
        return v;
    }
    case 0x1A: return sa.glcfg;
    default:
        if (a >= 0x08u && a <= 0x16u) return sa.diag[(a - 0x08u) / 2u];
        return 0;
    }
}

static void write_reg(uint8_t a, uint8_t v)
{
    switch (a & 0x7Eu) {
    case 0x00: sa.drive = v; sa.drive_writes++; break;
    case 0x06: sa.diagcfg = v; break;
    case 0x1A: sa.glcfg = (uint8_t)(v & 0xF6u); break;   /* SWRST self-clears, bit 3 reserved */
    default: break;
    }
}

static int iso2h823_frame(uint8_t mode, const uint8_t *tx, uint8_t *rx, uint16_t n)
{
    sa.frames++;
    if (mode != 0) sa.wrong_mode++;
    if (n != 3) { sa.wrong_len++; memset(rx, 0xFF, n); return 0; }
    if (sa.dead) { memset(rx, 0xFF, 3); return 0; }
    sample();
    uint8_t cd = coldiag(), val = 0;
    bool crc_ok = isoface_crc5(tx, 19) == (tx[2] & 0x1Fu);
    if (!crc_ok) sa.bad_crc_in++;
    else if (tx[0] & 0x80u) write_reg((uint8_t)(tx[0] & 0x7Fu), tx[1]);
    else val = read_reg(tx[0]);
    rx[0] = cd;
    rx[1] = val;
    rx[2] = (uint8_t)((sa.s_uv ? 0x80u : 0u) | 0x40u /* MV: not used */ | (cd ? 0x20u : 0u));
    rx[2] = (uint8_t)(rx[2] | isoface_crc5(rx, 19));
    if (sa.corrupt_replies > 0) { sa.corrupt_replies--; rx[1] ^= 0x10u; }
    return 0;
}

static int iso808a_frame(uint8_t mode, const uint8_t *tx, uint8_t *rx, uint16_t n)
{
    sb.frames++;
    if (mode != 0) sb.wrong_mode++;
    if (n != 1) { sb.wrong_len++; return 0; }
    sb.out = tx[0];
    rx[0] = 0;
    return 0;
}

static int spi(port_cs_t cs, uint8_t mode, const uint8_t *tx, uint8_t *rx, uint16_t n)
{
    switch (cs) {
    case PORT_CS_BANK_A: return iso2h823_frame(mode, tx, rx, n);
    case PORT_CS_BANK_B: return iso808a_frame(mode, tx, rx, n);
    case PORT_CS_INPUTS: return iso1i813t_frame(mode, tx, rx, n);
    default: memset(rx, 0xFF, n); return 0;
    }
}

void sim_chips_reset(void)
{
    memset(&sa, 0, sizeof sa);
    memset(&sb, 0, sizeof sb);
    memset(&si, 0, sizeof si);
    memset(si.coefil, 0x1F, sizeof si.coefil);
    si.s_uv = si.s_w4s = si.s_te = true;          /* GLERR 06h, INTERR 06h after power-up */
    sa.version = 0x40u;
    sa.diagcfg = 0xFFu;
    sa.s_uv = sa.s_te = sa.s_w4p = true;            /* GLERR 46h, INTERR 07h after power-up */
    memset(&hp, 0, sizeof hp);
    memset(hp.fram, 0xFF, sizeof hp.fram);              /* a new F-RAM is erased */
    hp.spi = spi;
    hp.b_supply = true;
}
