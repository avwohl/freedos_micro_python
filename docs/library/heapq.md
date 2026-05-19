---
title: heapq
---

# `heapq` — priority queue (heap)

```python
import heapq
```

In-place binary min-heap on a Python `list`. The smallest item is
always at `list[0]`; push/pop are O(log n).

## Functions

### `heapq.heappush(heap, item)`

```python
h = []
heapq.heappush(h, 3)
heapq.heappush(h, 1)
heapq.heappush(h, 4)
heapq.heappush(h, 1)
h                          # [1, 1, 4, 3]  — heap-ordered, not sorted
```

### `heapq.heappop(heap)` — returns smallest

```python
heapq.heappop(h)           # 1
heapq.heappop(h)           # 1
heapq.heappop(h)           # 3
heapq.heappop(h)           # 4
```

### `heapq.heapify(list)`

In-place: turn an arbitrary list into a valid heap. O(n).

```python
xs = [9, 5, 2, 7, 1, 8, 3]
heapq.heapify(xs)
xs                          # heap-ordered
heapq.heappop(xs)           # 1  (smallest)
```

## Pattern: schedule a callback

```python
import heapq, time

# (when_ms, callback) — sort by when
schedule = []

def at(delay_ms, fn):
    heapq.heappush(schedule, (time.ticks_ms() + delay_ms, fn))

def run_due():
    now = time.ticks_ms()
    while schedule and schedule[0][0] <= now:
        _, fn = heapq.heappop(schedule)
        fn()

at(500, lambda: print('half second'))
at(1000, lambda: print('one second'))
at(2000, lambda: print('two seconds'))

while schedule:
    run_due()
    time.sleep_ms(10)
```

## Sorting a stream

For "top-K largest from a long iterable" pattern, push everything
and pop the top K — or maintain a bounded heap:

```python
def top_k(it, k):
    h = []
    for x in it:
        if len(h) < k:
            heapq.heappush(h, x)
        elif x > h[0]:
            heapq.heappushpop(h, x)
    return sorted(h, reverse=True)

# ⚠️ `heapq.heappushpop` isn't in our build — combine push + pop:
def heappushpop(h, x):
    if h and x > h[0]:
        return heapq.heapreplace(h, x)
    return x
```

## Not included

- `heapq.heapreplace` — present
- `heapq.nlargest`, `heapq.nsmallest`, `heapq.merge` — not built
  in. Implement on top of the core 3 above.

---

*Credit:
[MicroPython heapq docs](https://docs.micropython.org/en/latest/library/heapq.html)
(MIT).*
