// Lanczos approximation for tgamma + lgamma.
// Replaces the NaN stubs that lived in lib/i386_dos_libc.asm.
//
// References:
//   - Numerical Recipes in C, §6.1
//   - https://en.wikipedia.org/wiki/Lanczos_approximation
//
// Coefficients (g=5, n=6) from Press et al. give ~10⁻¹⁰ relative
// error on the half-plane Re(z) ≥ 0.5. For x < 0.5 we use the
// reflection formula
//     gamma(z) * gamma(1 - z) = pi / sin(pi z)
//
// uc386 lowers `double` through the x87 FPU and has sin/cos/exp/
// log/pow/sqrt in the libc — Lanczos compiles to a few dozen FPU
// ops via the C math primitives.

#include <math.h>

static const double LANCZOS_G = 5.0;
static const double LANCZOS_C[7] = {
     1.000000000190015,
    76.18009172947146,
   -86.50532032941677,
    24.01409824083091,
    -1.231739572450155,
     1.208650973866179e-3,
    -5.395239384953e-6,
};
#define LANCZOS_SQRT_2PI 2.5066282746310005

double tgamma(double x) {
    // Special cases.
    if (isnan(x)) return x;
    if (x == 0.0) return 1.0 / x;        // ±inf, preserves sign
    if (x < 0.0 && x == (double)(long long)x) return 0.0 / 0.0; // negative integer → NaN
    if (isinf(x) && x > 0.0) return x;

    // Reflection for x < 0.5.
    if (x < 0.5) {
        double pi = 3.141592653589793;
        return pi / (sin(pi * x) * tgamma(1.0 - x));
    }

    // Lanczos: shift z = x - 1 so the formula gives gamma(z+1) = gamma(x).
    double z = x - 1.0;
    double t = z + LANCZOS_G + 0.5;
    double sum = LANCZOS_C[0];
    for (int k = 1; k <= 6; k++) {
        sum += LANCZOS_C[k] / (z + (double)k);
    }
    return LANCZOS_SQRT_2PI * pow(t, z + 0.5) * exp(-t) * sum;
}

double lgamma(double x) {
    // log(|gamma(x)|), implemented via tgamma. For large positive
    // x where tgamma overflows, this loses precision; a proper
    // lgamma would use Stirling-with-Bernoulli directly. Adequate
    // for moderate ranges.
    if (isnan(x)) return x;
    if (isinf(x)) return x > 0.0 ? x : -x;
    double g = tgamma(x);
    return log(g < 0.0 ? -g : g);
}
