---
title: time
---

# `time` — time + delays

```python
import time
```

DOS has a BIOS tick counter (INT 1Ah AH=0) at ~18.2 Hz (one tick
every ~55 ms). All `time` functions are built on this counter or
on the CMOS RTC.

## Wall-clock time

### `time.time()` — Unix epoch seconds (32-bit)

Returns seconds since the Unix epoch (1970-01-01 UTC). On this
port the resolution is ~1 second (CMOS RTC tick).

```python
>>> time.time()
1747630800            # 2026-05-19 ...
```

⚠️ This is a 32-bit value (mp_timestamp_t = mp_uint_t). Good
through year 2106 (2038 if signed). If you need nanosecond
resolution, use `time.time_ns()` — but heads-up that on a 386
class machine the granularity is still the BIOS tick, so
`time_ns()` returns multiples of ~55 million.

### `time.time_ns()` — same, nanoseconds

```python
>>> time.time_ns()
1747630800123456000
```

### `time.localtime(secs=None)` / `time.gmtime(secs=None)`

Convert epoch seconds to a `(year, mon, mday, hour, min, sec,
weekday, yearday)` tuple. With no arg, uses `time.time()`.

```python
>>> time.localtime()
(2026, 5, 19, 14, 30, 0, 0, 139)
```

⚠️ No timezone support — `localtime()` and `gmtime()` return the
same thing (whatever the DOS clock reads).

### `time.mktime(tuple)`

Inverse: convert a struct-time-tuple back to epoch seconds.

```python
>>> time.mktime((2026, 5, 19, 14, 30, 0, 0, 139))
1747662600
```

## Ticks (monotonic)

### `time.ticks_ms()` / `time.ticks_us()`

Monotonic millisecond / microsecond counter. Wraps modulo
`2**30` (so a `ticks_diff` is needed to compare).

```python
>>> t0 = time.ticks_ms()
>>> do_stuff()
>>> elapsed = time.ticks_diff(time.ticks_ms(), t0)
>>> print('took', elapsed, 'ms')
```

### `time.ticks_diff(a, b)` — `a - b` with wraparound handling

Always use this instead of subtracting ticks directly:

```python
delta = time.ticks_diff(time.ticks_ms(), t_start)
if delta > 5000:
    print('5 seconds elapsed')
```

### `time.ticks_add(t, delta)`

Add a delta to a ticks value, with wraparound handling.

## Sleep

### `time.sleep(seconds)` — float seconds

```python
time.sleep(0.1)        # 100 ms
time.sleep(1)
```

### `time.sleep_ms(ms)` / `time.sleep_us(us)`

```python
time.sleep_ms(50)
time.sleep_us(500)     # 500 µs (best effort — BIOS tick is 55 ms)
```

Implemented as busy-wait on the BIOS tick counter. CPU is consumed
during the sleep — DOS has nothing to switch to.

## Performance counter

### `time.perf_counter()` — seconds (float), monotonic

Same source as `ticks_ms`, but scaled. Useful for benchmarks:

```python
t0 = time.perf_counter()
do_thing()
print(time.perf_counter() - t0, 'seconds')
```

## Example: timeout loop

```python
import time

t_end = time.ticks_add(time.ticks_ms(), 5000)   # 5-second deadline
while time.ticks_diff(t_end, time.ticks_ms()) > 0:
    if check_something():
        break
    time.sleep_ms(10)
else:
    print('timed out')
```

---

*Credit: signatures from
[MicroPython time docs](https://docs.micropython.org/en/latest/library/time.html)
(MIT). DOS-specific notes are this port's.*
