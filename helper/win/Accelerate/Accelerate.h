/* The few vDSP calls entry.c and model.c use, over a radix-2 FFT in double (vDSP's packed real
 * FFT: DC in re[0], Nyquist in im[0]; forward = 2x the DFT, inverse = the plain sum). */
#ifndef RBSP_WIN_ACCELERATE_H
#define RBSP_WIN_ACCELERATE_H
#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct { float *realp, *imagp; } DSPSplitComplex;
typedef struct { double *realp, *imagp; } DSPDoubleSplitComplex;
typedef struct { float real, imag; } DSPComplex;
typedef struct { double real, imag; } DSPDoubleComplex;
typedef struct { int bits; } rbp_fft;
typedef rbp_fft *FFTSetup;
typedef rbp_fft *FFTSetupD;
#define kFFTRadix2 0
#define kFFTDirection_Forward 1
#define kFFTDirection_Inverse (-1)
typedef long vDSP_Stride;
typedef unsigned long vDSP_Length;

static FFTSetup vDSP_create_fftsetup(int bits, int radix) { (void)radix; rbp_fft *f = malloc(sizeof *f); if (f) f->bits = bits; return f; }
static FFTSetupD vDSP_create_fftsetupD(int bits, int radix) { return vDSP_create_fftsetup(bits, radix); }
static void vDSP_destroy_fftsetup(FFTSetup f) { free(f); }
static void vDSP_destroy_fftsetupD(FFTSetupD f) { free(f); }

/* in place on zr/zi (N points), sign -1 forward, +1 inverse */
static void rbp_fft_c(double *zr, double *zi, size_t N, int sign) {
    for (size_t i = 1, j = 0; i < N; i++) {
        size_t b = N >> 1;
        for (; j & b; b >>= 1) j ^= b;
        j |= b;
        if (i < j) { double t = zr[i]; zr[i] = zr[j]; zr[j] = t; t = zi[i]; zi[i] = zi[j]; zi[j] = t; }
    }
    for (size_t len = 2; len <= N; len <<= 1) {
        double ang = sign * 2.0 * 3.14159265358979323846 / (double)len;
        for (size_t k = 0; k < len / 2; k++) {
            double wr = cos(ang * (double)k), wi = sin(ang * (double)k);
            for (size_t i = k; i < N; i += len) {
                size_t j = i + len / 2;
                double xr = zr[j] * wr - zi[j] * wi, xi = zr[j] * wi + zi[j] * wr;
                zr[j] = zr[i] - xr; zi[j] = zi[i] - xi;
                zr[i] += xr; zi[i] += xi;
            }
        }
    }
}

/* the packed real FFT of N = 2^bits points, in place: re/im hold the N reals as pairs
 * (x[2k], x[2k+1]) going in (forward) and the packed spectrum coming out; reversed for inverse */
static void rbp_zrip(double *re, double *im, int bits, int dir) {
    size_t N = (size_t)1 << bits, H = N / 2;
    double *zr = malloc(sizeof(double) * N * 2), *zi;
    if (!zr) return;
    zi = zr + N;
    if (dir == kFFTDirection_Forward) {
        for (size_t k = 0; k < H; k++) { zr[2 * k] = re[k]; zr[2 * k + 1] = im[k]; }
        memset(zi, 0, sizeof(double) * N);
        rbp_fft_c(zr, zi, N, -1);
        for (size_t k = 1; k < H; k++) { re[k] = 2 * zr[k]; im[k] = 2 * zi[k]; }
        re[0] = 2 * zr[0];
        im[0] = 2 * zr[H];
    } else {
        for (size_t k = 0; k < N; k++) {
            if (k == 0) { zr[k] = re[0]; zi[k] = 0; }
            else if (k == H) { zr[k] = im[0]; zi[k] = 0; }
            else if (k < H) { zr[k] = re[k]; zi[k] = im[k]; }
            else { zr[k] = re[N - k]; zi[k] = -im[N - k]; }
        }
        rbp_fft_c(zr, zi, N, 1);
        for (size_t k = 0; k < H; k++) { re[k] = zr[2 * k]; im[k] = zr[2 * k + 1]; }
    }
    free(zr);
}

static void vDSP_fft_zrip(FFTSetup s, DSPSplitComplex *c, vDSP_Stride st, int bits, int dir) {
    size_t H = (size_t)1 << (bits - 1);
    double *re = malloc(sizeof(double) * H * 2), *im;
    (void)s; (void)st;
    if (!re) return;
    im = re + H;
    for (size_t k = 0; k < H; k++) { re[k] = c->realp[k]; im[k] = c->imagp[k]; }
    rbp_zrip(re, im, bits, dir);
    for (size_t k = 0; k < H; k++) { c->realp[k] = (float)re[k]; c->imagp[k] = (float)im[k]; }
    free(re);
}
static void vDSP_fft_zripD(FFTSetupD s, DSPDoubleSplitComplex *c, vDSP_Stride st, int bits, int dir) {
    (void)s; (void)st;
    rbp_zrip(c->realp, c->imagp, bits, dir);
}
/* interleaved complex <-> split */
static void vDSP_ztoc(const DSPSplitComplex *z, vDSP_Stride sz, DSPComplex *c, vDSP_Stride sc, vDSP_Length n) {
    (void)sz; (void)sc;
    for (vDSP_Length k = 0; k < n; k++) { c[k].real = z->realp[k]; c[k].imag = z->imagp[k]; }
}
static void vDSP_ctozD(const DSPDoubleComplex *c, vDSP_Stride sc, const DSPDoubleSplitComplex *z, vDSP_Stride sz, vDSP_Length n) {
    (void)sz; (void)sc;
    for (vDSP_Length k = 0; k < n; k++) { z->realp[k] = c[k].real; z->imagp[k] = c[k].imag; }
}
#endif
