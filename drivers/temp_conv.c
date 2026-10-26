/* temp_conv.c - see temp_conv.h */
#include <math.h>
#include "temp_conv.h"

static const double tc_lo[TC_N_TYPES] = { -200.0, -200.0, -200.0, -200.0, -50.0 };
static const double tc_hi[TC_N_TYPES] = { 1372.0, 1200.0, 400.0, 1000.0, 1768.1 };

double tc_min_c(int type) { return tc_lo[type]; }
double tc_max_c(int type) { return tc_hi[type]; }

/* E(T) and dE/dT */
static double emf(int type, double t, double *d)
{
    const tc_table_t *tab = &tc_nist[type];
    const tc_piece_t *p = &tab->p[0];
    for (int i = 0; i < tab->n; i++)
        if (t >= tab->p[i].lo) p = &tab->p[i];
    double e = 0.0, de = 0.0;
    for (int k = p->n - 1; k >= 0; k--) {           /* Horner, with the derivative alongside */
        de = de * t + e;
        e = e * t + p->c[k];
    }
    if (p->ex[0] != 0.0) {                          /* type K: a0 exp(a1 (t - a2)^2) */
        double u = t - p->ex[2], g = p->ex[0] * exp(p->ex[1] * u * u);
        e += g;
        de += g * 2.0 * p->ex[1] * u;
    }
    if (d) *d = de;
    return e;
}

double tc_emf_mv(int type, double t)
{
    const tc_table_t *tab = &tc_nist[type];
    if (t < tab->p[0].lo) t = tab->p[0].lo;
    if (t > tab->p[tab->n - 1].hi) t = tab->p[tab->n - 1].hi;
    return emf(type, t, 0);
}

int tc_temp_c(int type, double mv, double cj_c, double *t_c)
{
    double e = mv + tc_emf_mv(type, cj_c);
    double lo = tc_lo[type], hi = tc_hi[type];
    if (e < emf(type, lo, 0) - 1e-6) { *t_c = lo; return -1; }     /* 1 nV: the ends of the range are in it */
    if (e > emf(type, hi, 0) + 1e-6) { *t_c = hi; return 1; }
    /* Newton from a straight line through the range ends, kept inside the bracket by bisection */
    double elo = emf(type, lo, 0), ehi = emf(type, hi, 0);
    double t = lo + (e - elo) * (hi - lo) / (ehi - elo);
    for (int i = 0; i < 40; i++) {
        double d, f = emf(type, t, &d) - e;
        if (f > 0.0) hi = t; else lo = t;
        double nt = d > 0.0 ? t - f / d : 0.5 * (lo + hi);
        if (!(nt > lo && nt < hi)) nt = 0.5 * (lo + hi);
        if (fabs(nt - t) < 1e-6) { t = nt; break; }
        t = nt;
    }
    *t_c = t;
    return 0;
}

/* IEC 60751 */
#define CVD_A 3.9083e-3
#define CVD_B (-5.775e-7)
#define CVD_C (-4.183e-12)

double rtd_ohm(double r0, double t)
{
    double r = 1.0 + CVD_A * t + CVD_B * t * t;
    if (t < 0.0) r += CVD_C * (t - 100.0) * t * t * t;
    return r0 * r;
}

int rtd_temp_c(double r0, double ohm, double *t_c)
{
    if (ohm < rtd_ohm(r0, RTD_MIN_C)) { *t_c = RTD_MIN_C; return -1; }
    if (ohm > rtd_ohm(r0, RTD_MAX_C)) { *t_c = RTD_MAX_C; return 1; }
    double q = ohm / r0;
    /* T >= 0: the quadratic's root; below 0 Newton on the full equation, from that root */
    double t = (-CVD_A + sqrt(CVD_A * CVD_A - 4.0 * CVD_B * (1.0 - q))) / (2.0 * CVD_B);
    if (q < 1.0) {
        for (int i = 0; i < 20; i++) {
            double f = rtd_ohm(1.0, t) - q;
            double d = CVD_A + 2.0 * CVD_B * t + CVD_C * (4.0 * t * t * t - 300.0 * t * t);
            double dt = f / d;
            t -= dt;
            if (fabs(dt) < 1e-7) break;
        }
    }
    *t_c = t;
    return 0;
}

#define T25_K 298.15
#define K0_C 273.15

double ntc_ohm(double r25, double beta, double t_c)
{
    return r25 * exp(beta * (1.0 / (t_c + K0_C) - 1.0 / T25_K));
}

int ntc_temp_c(double r25, double beta, double ohm, double *t_c)
{
    if (ohm > ntc_ohm(r25, beta, NTC_MIN_C)) { *t_c = NTC_MIN_C; return -1; }   /* NTC: higher R = colder */
    if (ohm < ntc_ohm(r25, beta, NTC_MAX_C)) { *t_c = NTC_MAX_C; return 1; }
    *t_c = 1.0 / (1.0 / T25_K + log(ohm / r25) / beta) - K0_C;
    return 0;
}
