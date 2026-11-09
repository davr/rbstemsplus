/* rekordbox 7.2.19's sample-rate converter, re-implemented from its arm64 code
 * (rbxfrm::f2dsp::SamplingFunctionSRC<Float2> / c_src<Float2>): a Kaiser-windowed sinc, 29 taps
 * per output sample, from a reset state. rekordbox resamples every track that isn't at 44.1 kHz
 * before separating it; this gives its samples exactly (8 to 192 kHz, verified in the VM).
 * Compiled with -ffp-contract=off: the fused multiply-adds are written out where rekordbox has
 * them, and nowhere else. */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "rbsp.h"
#include "big.h"

#define TAPS 29        /* taps per output sample (0x1d) */
#define HIST 28        /* zero samples in the history buffer after reset (0x1c) */

typedef struct { float l, r; } F2;
/* SamplingFunctionSRC<Float2>::init(int in, int out): cutoff and Kaiser beta per rate pair */
static int pick_params(int in, int out, double *cutoff, double *beta)
{
    if (in - 8000u >= 0x2ee01u || out - 8000u >= 0x2ee01u) return 0; /* 8000..200000 */
    if (in == 44100 && out == 48000)       { *cutoff = 0.54; *beta = 8;  }
    else if (in == 44100 && out == 96000)  { *cutoff = 0.58; *beta = 10; }
    else if (in == 48000 && out == 44100)  { *cutoff = 0.52; *beta = 8;  }
    else if (in == 48000 && out == 96000)  { *cutoff = 0.45; *beta = 10; }
    else if (in == 96000 && out == 44100)  { *cutoff = 0.55; *beta = 6;  }
    else if (in == 96000 && out == 48000)  { *cutoff = 0.47; *beta = 10; }
    else if (in == 88200 && out == 44100)  { *cutoff = 0.55; *beta = 6;  }
    else if (in == 44100 && out == 88200)  { *cutoff = 0.58; *beta = 10; }
    else if (in == 44100 && out == 176400) { *cutoff = 0.48; *beta = 9;  }
    else if (in == 44100 && out == 192000) { *cutoff = 0.48; *beta = 9;  }
    else                                   { *cutoff = 0.45; *beta = 12; } /* incl. 192000->44100 */
    return 1;
}

/* c_src<Float2>::initIdxAndIrForKaiser(fc, beta, 0): prototype filter in double, then the
 * per-phase table of 29 float coefficients (stored as Float2 pairs with L == R). */
static float *build_table(int L, int M, double fc, double beta, int **idx_out)
{
    int N = 29 * L;
    double *proto = malloc(sizeof(double) * N);
    int half = N / 2;
    double twopi_fc = fc * 6.283185307179586;      /* 0x401921fb54442d18 */
    double two_fc = fc + fc;
    double inv = 1.0 / (double)(N - 1);
    double step = inv + inv;
    double hb = beta * 0.5;
    for (int i = 0; i < N; i++) {
        int n = i - half;
        double h;
        if (n == 0) h = two_fc;
        else {
            double dn = (double)n;
            double s = sin(twopi_fc * dn);
            h = s / (dn * 3.141592653589793);       /* 0x400921fb54442d18 */
        }
        double x = (double)i * step;
        double xm1 = x + -1.0;
        double t = fma(x, xm1, -xm1);               /* fnmsub d0, d0, d1, d1 */
        double arg = sqrt(1.0 - t) * hb;
        double s1 = 0.0, s2 = 0.0, kd = 0.0;
        for (int k = 0; k < 22; k++) {
            double f = 1.0;
            for (int m = 2; m <= k; m++) f *= (double)m;  /* exact for k <= 21 */
            f = f * f;
            double e = kd + kd;
            double rcp = 1.0 / f;
            s1 = fma(rcp, pow(arg, e), s1);
            s2 = fma(rcp, pow(hb, e), s2);
            kd += 1.0;
        }
        proto[i] = (s1 * h) / s2;
    }
    int *idx = malloc(sizeof(int) * L);
    float *tab = malloc(sizeof(float) * L * TAPS);
    double scale = (double)L, norm = 1.0;           /* flag == 0: no 1/sum normalisation */
    for (int p = 0; p < L; p++) {
        idx[p] = (M * p) / L;
        for (int j = 0; j < TAPS; j++) {
            int k = L * (idx[p] + j + 1) + ~(M * p);  /* L*(idx+j+1) - M*p - 1 */
            double v = proto[k] * scale;
            v = v * norm;
            tab[p * TAPS + j] = (float)v;
        }
    }
    free(proto);
    *idx_out = idx;
    return tab;
}

/* c_src<Float2>::convolute_fine: pairs from both ends, fused multiply-add per lane */
static inline F2 conv_fine(const F2 *x, const float *h)
{
    float al = 0.f, ar = 0.f;
    int a = 0, b = TAPS - 1;
    for (; a < b; a++, b--) {
        al = fmaf(h[a], x[a].l, al); ar = fmaf(h[a], x[a].r, ar);
        al = fmaf(h[b], x[b].l, al); ar = fmaf(h[b], x[b].r, ar);
    }
    al = fmaf(h[a], x[a].l, al); ar = fmaf(h[a], x[a].r, ar);
    F2 o = { al, ar };
    return o;
}

int rb_resample(const float *in, size_t nin, int rate, float **out, size_t *nout) {
    const int to = 44100;
    double cutoff, beta;
    if (rate % 50 || !pick_params(rate, to, &cutoff, &beta) || !nin) return 1;
    int L = to / 50, M = rate / 50;
    double fc = cutoff / (double)(rate < to ? L : M);
    int *idx;
    float *tab = build_table(L, M, fc, beta, &idx);
    /* X = HIST zeros, then the input (interleaved pairs) */
    F2 *X = big_calloc(nin + HIST + TAPS, sizeof(F2));
    if (!tab || !idx || !X) { free(tab); free(idx); big_free(X); return 1; }
    for (size_t i = 0; i < nin; i++) { X[HIST + i].l = in[i]; X[HIST + i].r = in[nin + i]; }
    /* output n uses X[q .. q+28], q = floor(M*n/L); produced only when X[q+28] is an input
     * sample, i.e. q <= nin-1 (SamplingFunctionSRC::calcOutputNum from a reset state) */
    size_t n_out = (size_t)(((int64_t)(nin - 1) * L) / M) + 1;
    while (n_out > 0 && (size_t)(((int64_t)M * (int64_t)(n_out - 1)) / L) > nin - 1) n_out--;
    while ((size_t)(((int64_t)M * (int64_t)n_out) / L) <= nin - 1) n_out++;
    float *Y = big_alloc(sizeof(float) * 2 * n_out);
    if (!Y) { free(tab); free(idx); big_free(X); return 1; }
    for (size_t n = 0; n < n_out; n++) {
        size_t q = (size_t)(((int64_t)M * (int64_t)n) / L);
        int p = (int)(n % (size_t)L);
        F2 y = conv_fine(X + q, tab + p * TAPS);
        Y[n] = y.l;
        Y[n_out + n] = y.r;
    }
    free(tab); free(idx); big_free(X);
    *out = Y;
    *nout = n_out;
    return 0;
}
