// Real time() / mktime() / gettimeofday() backed by the DOS RTC
// (INT 21h AH=0x2A + 0x2C via libc's _dos_get_datetime). axtls's
// cert verification path needs both:
//   - mktime() to convert each cert's parsed notBefore/notAfter
//     calendar fields into a Unix epoch second count
//   - gettimeofday() to get the current epoch second to compare
//     against those bounds (x509.c:506,513)
//
// The libc stubs we shipped at SSL bringup (returning 0 / a fake
// counter) made every cert look "currently valid forever," which
// is fine for CONFIG_SSL_CERT_VERIFICATION=undef but obviously
// wrong once the flag is on.
//
// Calendar→epoch uses Howard Hinnant's "days from civil"
// algorithm (no table lookups, no leap-year branches; works for
// any Gregorian year). For DOS dates (1980–2099) the year is
// always positive so we skip the negative-y branch.
//
// We replace libc.asm's `_time`, `_mktime`, `_gettimeofday`
// (those stubs are removed). The libc wrappers compiled into
// every TU's bundle now `extern` to these symbols.

#include <time.h>
#include <sys/time.h>

extern void dos_get_datetime(unsigned char out[7]);

// Returns days since 1970-01-01 for the given (year, month, day).
// Month is 1-12, day is 1-31. Assumes year >= 0 (DOS minimum is 1980).
static long days_from_civil(int y, unsigned m, unsigned d) {
    y -= (m <= 2);
    long era = y / 400;
    unsigned yoe = (unsigned)(y - era * 400);              // [0, 399]
    unsigned m_adj = (m > 2) ? (m - 3) : (m + 9);          // [0, 11], March=0
    unsigned doy = (153u * m_adj + 2u) / 5u + d - 1u;      // [0, 365]
    unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return era * 146097L + (long)doe - 719468L;
}

static time_t epoch_now(void) {
    unsigned char raw[7];
    dos_get_datetime(raw);
    int year = (int)(raw[0] | (raw[1] << 8));
    long days = days_from_civil(year, raw[2], raw[3]);
    long secs_today = (long)raw[4] * 3600L + (long)raw[5] * 60L + (long)raw[6];
    return (time_t)(days * 86400L + secs_today);
}

time_t time(time_t *t) {
    time_t now = epoch_now();
    if (t) {
        *t = now;
    }
    return now;
}

time_t mktime(struct tm *tm) {
    if (tm == 0) {
        return (time_t)-1;
    }
    long days = days_from_civil(tm->tm_year + 1900,
                                (unsigned)(tm->tm_mon + 1),
                                (unsigned)tm->tm_mday);
    long secs_today = (long)tm->tm_hour * 3600L
                    + (long)tm->tm_min * 60L
                    + (long)tm->tm_sec;
    return (time_t)(days * 86400L + secs_today);
}

int gettimeofday(struct timeval *tv, void *tz) {
    (void)tz;
    if (tv) {
        tv->tv_sec = epoch_now();
        tv->tv_usec = 0;  // DOS clock is second-precision
    }
    return 0;
}
