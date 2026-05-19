---
title: math
---

# `math` — mathematical functions

```python
import math
```

Backed by x87 FPU instructions for the core functions. Full
double-precision throughout.

## Constants

```python
math.pi       # 3.141592653589793
math.e        # 2.718281828459045
math.tau      # 6.283185307179586 (2π)
math.inf      # float('inf')
math.nan      # float('nan')
```

## Trig

`sin`, `cos`, `tan`, `asin`, `acos`, `atan`, `atan2(y, x)`, `sinh`,
`cosh`, `tanh`, `asinh`, `acosh`, `atanh`. Arguments in radians.

```python
math.sin(math.pi / 2)             # 1.0
math.atan2(1, 1)                  # π/4
math.degrees(math.pi)             # 180.0
math.radians(180)                 # π
```

## Powers + logarithms

`exp(x)`, `expm1(x)`, `log(x[, base])`, `log2(x)`, `log10(x)`,
`log1p(x)`, `pow(x, y)`, `sqrt(x)`.

```python
math.sqrt(2)                      # 1.4142135623730951
math.log(math.e)                  # 1.0
math.log(8, 2)                    # 3.0
math.log2(1024)                   # 10.0
math.exp(1)                       # math.e
```

## Rounding + parts

`ceil(x)`, `floor(x)`, `trunc(x)`, `round(x)`, `fmod(x, y)`,
`modf(x)` → `(frac, int)`, `frexp(x)` → `(m, e)`, `ldexp(m, e)`,
`copysign(x, y)`.

```python
math.floor(3.7)                   # 3
math.ceil(3.2)                    # 4
math.trunc(-3.7)                  # -3
math.fmod(7, 2)                   # 1.0
math.modf(3.75)                   # (0.75, 3.0)
```

## Special functions

`fabs(x)`, `gamma(x)`, `lgamma(x)`, `erf(x)`, `erfc(x)`,
`factorial(n)`, `isclose(a, b, rel_tol=1e-9, abs_tol=0.0)`,
`isnan(x)`, `isinf(x)`, `isfinite(x)`.

```python
math.factorial(5)                 # 120
math.gamma(6)                     # 120.0 (= 5!)
math.isclose(0.1 + 0.2, 0.3)      # True
math.isnan(float('nan'))          # True
```

## Edge-case fixes

This port enables MicroPython's "fix-infnan" suite for `atan2`,
`fmod`, `modf`, `pow`, `gamma` — return CPython-matching values at
infinity / NaN boundaries instead of platform-native FPU results.
Lets code that relies on `math.atan2(0, 0) == 0` actually run.

---

*Credit: signatures from
[MicroPython math docs](https://docs.micropython.org/en/latest/library/math.html)
(MIT, © 2014-2024 Damien P. George and contributors).*
