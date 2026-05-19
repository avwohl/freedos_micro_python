---
title: random
---

# `random` — pseudo-random numbers

```python
import random
```

Yasmarang PRNG (the `MICROPY_PY_RANDOM_EXTRA_FUNCS` build).
Auto-seeded at startup from the BIOS tick counter; you'll get
different sequences across runs.

⚠️ **Not cryptographically secure.** For crypto use the libssh2 /
axtls random sources via `_ssh` / `ssl`.

## Core

### `random.seed(n=None)`

Reseed. `None` uses the BIOS tick counter again. An explicit `n`
makes the sequence reproducible.

```python
random.seed(42)
random.random()                # always same float
random.seed()                  # back to "random" seeding
```

### `random.getrandbits(n)`

Returns an `int` with `n` random bits (`1 <= n <= 32`).

```python
random.getrandbits(8)          # 0..255
random.getrandbits(16)
```

## Floats

### `random.random()`

Returns a float in `[0.0, 1.0)`.

### `random.uniform(a, b)`

Returns a float in `[a, b]`.

```python
random.uniform(-1, 1)          # e.g. 0.3145
```

## Integers

### `random.randint(a, b)`

Returns an `int` in `[a, b]` (inclusive).

```python
random.randint(1, 6)           # dice roll
```

### `random.randrange(stop)` / `random.randrange(start, stop, step=1)`

Returns an `int` from `range(...)`.

## Sequences

### `random.choice(seq)`

Returns one element from a non-empty sequence.

```python
random.choice(['rock', 'paper', 'scissors'])
```

### `random.sample(seq, k)`

Returns a list of `k` unique elements from `seq`.

```python
random.sample(range(100), 10)
```

### `random.shuffle(list)`

Shuffles `list` in place.

```python
deck = list(range(52))
random.shuffle(deck)
```

---

*Credit: surface from
[MicroPython random docs](https://docs.micropython.org/en/latest/library/random.html)
(MIT).*
