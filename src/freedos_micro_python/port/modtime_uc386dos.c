// uc386-dos `time` module port shim — included into
// `extmod/modtime.c` via `MICROPY_PY_TIME_INCLUDEFILE`. Provides
// `mp_time_time_get` (used by `time.time()`) and
// `mp_time_localtime_get` (used by `time.localtime()` / `gmtime()`)
// by reading the DOS clock via INT 21h AH=0x2A (date) and
// AH=0x2C (time). Matching `mp_hal_time_ns` lives in
// `mphal_uc386dos.c`.
//
// Caveats:
//  - DOS has only second-precision clock; the time_ns helper
//    multiplies seconds * 1e9 (no sub-second contribution).
//  - DOS dates start in 1980, so anything before that comes back
//    as 1970-01-01.
//  - Default uc386-dos epoch is 2000-01-01 (mpconfig.h's default
//    `MICROPY_EPOCH_IS_2000=1`); seconds returned are
//    `seconds_since_2000`.
//  - Smoke tests under dos_emu get a synthetic deterministic
//    date (2026-05-03 12:34:00) — see src/uc386/dos_emu.py's
//    INT 21h AH=0x2A/0x2C handlers. Real DOS via PMODE/W reads
//    the host's RTC.

#include "shared/timeutils/timeutils.h"

extern void dos_get_datetime(unsigned char out[7]);

static inline void uc386dos_read_datetime(timeutils_struct_time_t *tm) {
    unsigned char raw[7];
    dos_get_datetime(raw);
    tm->tm_year = (uint16_t)(raw[0] | (raw[1] << 8));
    tm->tm_mon  = raw[2];
    tm->tm_mday = raw[3];
    tm->tm_hour = raw[4];
    tm->tm_min  = raw[5];
    tm->tm_sec  = raw[6];
    // tm_wday / tm_yday left zero (timeutils helpers fill them
    // when they convert seconds → struct_time, but the DOS
    // get-date INT only returns day-of-week which we don't
    // bother harvesting).
    tm->tm_wday = 0;
    tm->tm_yday = 0;
}

static mp_obj_t mp_time_time_get(void) {
    timeutils_struct_time_t tm;
    uc386dos_read_datetime(&tm);
    // With MICROPY_TIMESTAMP_IMPL=1 (UINT) `mp_timestamp_t` is
    // `mp_uint_t` (32-bit on i386). seconds_since_epoch fits a
    // 32-bit small int through year 2068 (epoch=1970) or
    // year 2136 (epoch=2000) — fine for DOS. Avoid
    // `mp_obj_new_int_from_ll` here: it's a stub that always
    // raises OverflowError when LONGINT_IMPL_NONE.
    mp_timestamp_t secs = timeutils_seconds_since_epoch(
        tm.tm_year, tm.tm_mon, tm.tm_mday,
        tm.tm_hour, tm.tm_min, tm.tm_sec);
    return mp_obj_new_int_from_uint((mp_uint_t)secs);
}

static void mp_time_localtime_get(timeutils_struct_time_t *tm) {
    uc386dos_read_datetime(tm);
}
