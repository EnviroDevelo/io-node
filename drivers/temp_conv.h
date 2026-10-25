/* temp_conv.h - sensor signal to temperature: thermocouples (NIST ITS-90 reference functions, NIST SRD 60),
 * platinum RTDs (IEC 60751 Callendar-Van Dusen) and NTC thermistors (B-parameter equation).
 * Pure maths in double precision: no hardware, host-tested against NIST table values (tests/test_temp.c).
 *
 * Thermocouples: the reference functions give E(T); the inverse is solved by Newton's method from E(T)
 * itself, so the forward and inverse can never disagree (NIST's own inverse polynomials differ from the
 * forward ones by up to 0.06 degC). Cold-junction compensation: E = E_measured + E(T_cj). */
#ifndef TEMP_CONV_H
#define TEMP_CONV_H
#include <stdbool.h>

enum { TC_K = 0, TC_J, TC_T, TC_E, TC_S, TC_N_TYPES };

typedef struct { double lo, hi; const double *c; int n; double ex[3]; } tc_piece_t;
typedef struct { const tc_piece_t *p; int n; } tc_table_t;
extern const tc_table_t tc_nist[TC_N_TYPES];      /* tc_nist_tab.c, generated */

/* the range a reading is reported in (the reference functions' own ranges, the low end raised where a
 * thermocouple's sensitivity is too low to be useful) */
double tc_min_c(int type);
double tc_max_c(int type);

/* E(T) in mV for T in degC (clamped to the reference function's range) */
double tc_emf_mv(int type, double t_c);
/* T from the thermocouple voltage (mV) and the cold-junction temperature. Returns 0 and *t_c, or -1 below
 * the type's range, +1 above it (*t_c then holds the limit). */
int tc_temp_c(int type, double mv, double cj_c, double *t_c);

/* platinum RTD, IEC 60751 (alpha 0.00385): R(T) and its inverse, -200..850 degC. r0 = 100 or 1000 ohm */
#define RTD_MIN_C (-200.0)
#define RTD_MAX_C 850.0
double rtd_ohm(double r0, double t_c);
int rtd_temp_c(double r0, double ohm, double *t_c);     /* 0, -1 below the range, +1 above */

/* NTC thermistor, 1/T = 1/T25 + ln(R/R25)/B, reported -50..150 degC */
#define NTC_MIN_C (-50.0)
#define NTC_MAX_C 150.0
double ntc_ohm(double r25, double beta, double t_c);
int ntc_temp_c(double r25, double beta, double ohm, double *t_c);   /* 0, -1 below, +1 above */

#endif
