/* bigint_bench — direct-measure the throughput of the uc386 codegen
 * on the axtls bigint regular_multiply inner-loop pattern.
 *
 * The smoke (rigs/tls-smoke) times out under QEMU TCG-on-ARM not
 * because of any codegen we can fix, but because RSA-2048 modexp
 * goes through this same inner loop millions of times. The codegen
 * fix series (commits 04968c3 .. 6f35425) shrank that inner loop
 * from ~38 emitted instructions per iteration to 17. To verify the
 * wins translate into measurable throughput (rather than just a
 * smaller `.asm`), we run the literal `regular_multiply` shape here
 * — no axtls, no lwIP, no TLS — and measure inner-loop iterations
 * per second via the BIOS tick counter.
 *
 * Stages, emitted to COM1:
 *
 *   [bench:start]    bench has begun
 *   [bench:t0=…]     starting BIOS tick reading
 *   [bench:t1=…]     ending BIOS tick reading
 *   [bench:ticks=…]  elapsed ticks (BIOS @ 18.2 Hz → ~55 ms/tick)
 *   [bench:mul-iters=…]  number of bi_multiply equivalents
 *   [bench:inner=…]  inner-loop iteration count (= mul-iters × n × n)
 *   [bench:ms=…]     total ms elapsed
 *   [bench:us/inner=…]  microseconds per inner-loop iteration
 *   [bench:sr0=…]    sanity hex dump of sr[0] (prevents DCE)
 *
 * Picks `n = 64` to match RSA-2048 (64 × 32-bit components = 2048
 * bits). N_MUL_ITERS is tuned so the run takes ~10 BIOS ticks
 * (~550 ms) on QEMU TCG-on-ARM — enough to be measurable without
 * blowing past the run.sh timeout.
 */

#include <stdint.h>
#include <string.h>

extern int write(int fd, const void *buf, unsigned int n);
extern unsigned long bios_ticks(void);

/* ---- emit helpers (same pattern as tls-smoke/main.c) ----------- */

static void emit(const char *s) {
    int n = 0;
    while (s[n]) n++;
    write(1, s, n);
}

static void emit_hex8(unsigned int v) {
    static const char hex[] = "0123456789abcdef";
    char buf[12];
    buf[0] = '0'; buf[1] = 'x';
    for (int i = 0; i < 8; i++) {
        buf[2 + i] = hex[(v >> ((7 - i) * 4)) & 0xF];
    }
    buf[10] = '\r';
    buf[11] = '\n';
    write(1, buf, 12);
}

static void emit_dec(unsigned long v) {
    char buf[12];
    int n = 0;
    if (v == 0) {
        buf[n++] = '0';
    } else {
        char tmp[12]; int t = 0;
        while (v > 0) { tmp[t++] = (char)('0' + (v % 10u)); v /= 10u; }
        while (t > 0) buf[n++] = tmp[--t];
    }
    buf[n++] = '\r';
    buf[n++] = '\n';
    write(1, buf, n);
}

/* ---- the inner loop --------------------------------------------- */
/* Lifted verbatim from axtls/crypto/bigint.c:regular_multiply (lines
 * ~828-867 in the 32-bit-comp configuration). Same operand types
 * (`uint32_t` for comp, `uint64_t` for long_comp), same nested-loop
 * shape: outer i ∈ [0, n), inner j ∈ [0, n). The codegen for this
 * function is what the fix series targeted. */

#define N 64

static uint32_t sa[N];
static uint32_t sb[N];
static uint32_t sr[2 * N];

static void regular_multiply(uint32_t *sr_, const uint32_t *sa_,
                             const uint32_t *sb_, int n) {
    int i, j;
    for (i = 0; i < n; i++) {
        uint32_t b = sb_[i];
        uint32_t carry = 0;
        for (j = 0; j < n; j++) {
            int r = i + j;
            uint64_t tmp = sr_[r] + (uint64_t)sa_[j] * b + carry;
            sr_[r] = (uint32_t)tmp;
            carry = (uint32_t)(tmp >> 32);
        }
        sr_[i + n] = carry;
    }
}

/* ---- main ------------------------------------------------------- */

#define N_MUL_ITERS 600  /* Tunable. 600 × 4096 = ~2.46M inner iters,
                            should land ~30 BIOS ticks for ±3% accuracy. */

int main(void) {
    emit("[bench:start]\r\n");

    /* Fill operands with arbitrary non-zero values so the codegen
     * can't see any operand-zero shortcuts and the high halves of
     * sa[j]*sb[i] products are actually populated. */
    for (int i = 0; i < N; i++) {
        sa[i] = 0x12345600u + (unsigned)i * 0x01010101u;
        sb[i] = 0xabcdef00u + (unsigned)i * 0x10101010u;
    }

    emit("[bench:t0=]");
    unsigned long t0 = bios_ticks();
    emit_hex8((unsigned int)t0);

    for (int k = 0; k < N_MUL_ITERS; k++) {
        /* Zero sr before each multiply (regular_multiply expects
         * a fresh accumulator; axtls does the equivalent memset). */
        for (int z = 0; z < 2 * N; z++) sr[z] = 0;
        regular_multiply(sr, sa, sb, N);
    }

    emit("[bench:t1=]");
    unsigned long t1 = bios_ticks();
    emit_hex8((unsigned int)t1);

    unsigned long ticks = t1 - t0;
    unsigned long ms = ticks * 55u;  /* BIOS @ 18.2 Hz */
    unsigned long inner = (unsigned long)N_MUL_ITERS * N * N;

    emit("[bench:ticks=]");
    emit_dec(ticks);
    emit("[bench:mul-iters=]");
    emit_dec(N_MUL_ITERS);
    emit("[bench:inner=]");
    emit_dec(inner);
    emit("[bench:ms=]");
    emit_dec(ms);
    /* us/inner = ms * 1000 / inner.  Use integer math; for the
     * expected ~50us/iter range this fits in 32 bits comfortably. */
    if (inner > 0) {
        unsigned long us_per_inner_x10 = (ms * 10000ul) / inner;
        emit("[bench:us/inner_x10=]");
        emit_dec(us_per_inner_x10);
    }

    /* Sanity: print sr[0] and sr[2N-1] so the optimizer can't
     * dead-code-eliminate the loop. */
    emit("[bench:sr0=]");
    emit_hex8(sr[0]);
    emit("[bench:srN-1=]");
    emit_hex8(sr[2 * N - 1]);

    emit("[bench:done]\r\n");
    return 0;
}
