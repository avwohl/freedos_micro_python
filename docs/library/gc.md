---
title: gc
---

# `gc` — garbage collection

```python
import gc
```

MicroPython has a real garbage collector (mark-and-sweep across
the 1 MB Python heap on this port). Mostly you don't need to think
about it; `gc` lets you tune / observe.

## Functions

### `gc.collect()`

Force a collection. Returns `None`.

```python
gc.collect()
```

### `gc.enable()` / `gc.disable()`

Turn the GC on or off. By default it's on, with collection
triggered when an allocation can't be satisfied.

```python
gc.disable()
# performance-critical section with predictable allocation
gc.enable()
```

### `gc.mem_alloc()` / `gc.mem_free()`

Bytes currently in use / available on the Python heap.

```python
>>> gc.mem_free()
734528
>>> data = bytearray(100000)
>>> gc.mem_free()
634416
>>> del data; gc.collect()
>>> gc.mem_free()
734528
```

### `gc.threshold(n)` / `gc.threshold()`

Set / get the byte allocation threshold that triggers a collection.
Default is "as needed".

## Tuning tips

- On this port the Python heap is fixed at 1 MB. PMODE/W's PM heap
  (for libssh2 / axtls / lwIP buffers) is separate and not tracked
  by `gc`.
- `gc.collect()` is fast on this port (a few ms for the full heap).
  Calling it before a known-large allocation defragments and gives
  you the best chance of finding a contiguous block.
- `gc.mem_free()` is the truth of free GC memory; PMODE/W's
  internal `_libssh2_malloc` / axtls allocations don't show up
  here.

---

*Credit:
[MicroPython gc docs](https://docs.micropython.org/en/latest/library/gc.html)
(MIT).*
