// uc386-dos MicroPython HAL — out-of-line implementations.
//
// The minimal port's mphalport.h normally inlines mp_hal_ticks_ms()
// to a constant 0 because the minimal port has no realtime source.
// We override that with a real BIOS-tick-backed version so the
// `time` module can return non-trivial ticks_ms / sleep behavior.
//
// All timing is derived from INT 1Ah AH=00h (BIOS tick counter,
// ~18.2 Hz, ~55 ms per tick). That's the only realtime source DOS
// programs can portably read without programming the PIT directly,
// and it matches what most DOS C runtimes use under the hood.

#include "py/mpconfig.h"
#include "py/mphal.h"

extern unsigned long bios_ticks(void);

// Each BIOS tick is 1000 / 18.20649 ≈ 54.9254 ms. Use 55 — the ~0.13%
// drift is well below what user code that reaches for ticks_ms can
// distinguish, and matches the rounding most DJGPP/Watcom DOS time
// helpers do.
#define BIOS_TICK_MS 55U

mp_uint_t mp_hal_ticks_ms(void) {
    return bios_ticks() * BIOS_TICK_MS;
}

mp_uint_t mp_hal_ticks_us(void) {
    // Single-tick resolution. Multiply by 1000 (giving us 55000 µs
    // step) so callers comparing two ticks_us readings see a
    // monotonically-non-decreasing sequence with predictable steps.
    return bios_ticks() * (BIOS_TICK_MS * 1000U);
}

mp_uint_t mp_hal_ticks_cpu(void) {
    // No RDTSC or PIT-direct reads here; reuse the µs scale so the
    // value is at least monotonically non-decreasing.
    return mp_hal_ticks_us();
}

void mp_hal_delay_ms(mp_uint_t ms) {
    // Convert ms → ticks (round up), then busy-wait until the BIOS
    // counter has advanced that many ticks. Polls in a tight loop;
    // there's no HLT/yield equivalent we can portably issue under
    // PMODE/W since the extender owns interrupt dispatch.
    if (ms == 0) {
        return;
    }
    unsigned long ticks_to_wait = (ms + BIOS_TICK_MS - 1) / BIOS_TICK_MS;
    if (ticks_to_wait == 0) {
        ticks_to_wait = 1;
    }
    unsigned long start = bios_ticks();
    while ((bios_ticks() - start) < ticks_to_wait) {
        // poll
    }
}

void mp_hal_delay_us(mp_uint_t us) {
    // Sub-tick precision isn't available, so we round up to the
    // nearest ms and reuse the ms path. Anything < 55000 µs ends up
    // waiting one BIOS tick, anything >= 55000 µs waits the
    // appropriate number.
    if (us == 0) {
        return;
    }
    unsigned long ms = (us + 999U) / 1000U;
    mp_hal_delay_ms(ms);
}

// `sys.stdio.poll` and `input()`-readiness checks call this. We
// don't have a non-blocking stdin path under PMODE/W (INT 21h
// AH=0Bh exists but isn't reliable inside the dos_emu harness),
// so return 0 (no events ready). REPL still gets stdin via the
// normal `mp_hal_stdin_rx_chr` path in uart_core.c.
uintptr_t mp_hal_stdio_poll(uintptr_t poll_flags) {
    (void)poll_flags;
    return 0;
}

// `time.time_ns()` — nanoseconds since the configured epoch.
// We don't have sub-second precision from the DOS RTC (INT 21h
// AH=0x2C reports hundredths but we drop them for simplicity),
// so just multiply seconds-since-epoch by 1e9. Returned as
// uint64 so it can carry the full epoch timestamp without
// overflow on 32-bit builds.
#include "shared/timeutils/timeutils.h"

extern void dos_get_datetime(unsigned char out[7]);

uint64_t mp_hal_time_ns(void) {
    unsigned char raw[7];
    dos_get_datetime(raw);
    unsigned int year   = (unsigned int)(raw[0] | (raw[1] << 8));
    unsigned int month  = raw[2];
    unsigned int day    = raw[3];
    unsigned int hour   = raw[4];
    unsigned int minute = raw[5];
    unsigned int second = raw[6];
    mp_timestamp_t secs = timeutils_seconds_since_epoch(
        year, month, day, hour, minute, second);
    return (uint64_t)secs * 1000000000ULL;
}
