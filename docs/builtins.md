---
title: Built-in functions
---

# Built-in functions

Functions in the default namespace — no `import` required. The
list below is complete for what this port supports. Anything
upstream Python has and we don't is called out at the end.

## Output / input

### `print(*args, sep=' ', end='\n', file=sys.stdout)`

Writes each positional arg, separated by `sep`, ending with `end`.
`file` defaults to stdout (the DOS console or the redirected
COM1 if `CTTY COM1` is active).

```python
print('hello')                          # hello\n
print('a', 'b', 'c')                    # a b c\n
print('a', 'b', sep=':')                # a:b\n
print('no newline', end='')             # no newline (cursor stays)
import sys; print('err!', file=sys.stderr)
```

### `input(prompt='')`

Prints `prompt` to stdout (if any), then reads a line from stdin and
returns it without the trailing newline.

```python
>>> name = input('what is your name? ')
what is your name? Dave
>>> print('hi,', name)
hi, Dave
```

## Type-related

### `type(obj)` / `type(name, bases, dict)`

One-arg: returns the object's class.
Three-arg: creates a new class dynamically (rare; metaclass use).

```python
type(42)                # <class 'int'>
type('hi')              # <class 'str'>
type([1, 2])            # <class 'list'>
```

### `isinstance(obj, cls)` / `issubclass(cls, parent)`

```python
isinstance(42, int)             # True
isinstance(42, (int, float))    # True (tuple of types)
issubclass(bool, int)           # True  (bool subclasses int)
```

### Conversions

`int(x, base=10)` / `float(x)` / `str(x)` / `bytes(x)` /
`bool(x)` / `list(x)` / `tuple(x)` / `dict(x)` / `set(x)` /
`frozenset(x)` — convert between types where the conversion makes
sense.

```python
int('42')               # 42
int('ff', 16)           # 255
int('1010', 2)          # 10
float('3.14')           # 3.14
str(42)                 # '42'
bytes([0, 1, 255])      # b'\x00\x01\xff'
list('hello')           # ['h','e','l','l','o']
list(range(5))          # [0,1,2,3,4]
dict([('a', 1), ('b', 2)])  # {'a': 1, 'b': 2}
```

`hex(n)`, `oct(n)`, `bin(n)` — return string representations:

```python
hex(255)                # '0xff'
oct(8)                  # '0o10'
bin(10)                 # '0b1010'
```

`chr(n)` / `ord(c)` — character ↔ codepoint conversion:

```python
chr(65)                 # 'A'
ord('A')                # 65
```

`repr(x)` — official string representation (round-trips through
`eval()` when possible):

```python
repr('hi\n')            # "'hi\\n'"
repr(b'\x01')           # "b'\\x01'"
```

## Numeric

### `abs(x)`, `round(x, n=None)`, `min(...)`, `max(...)`, `sum(it, start=0)`, `pow(x, y, m=None)`, `divmod(a, b)`

```python
abs(-5)                 # 5
round(3.7)              # 4
round(3.14159, 2)       # 3.14
min(3, 1, 4, 1, 5)      # 1
max([3, 1, 4, 1, 5])    # 5
sum([1, 2, 3])          # 6
sum([1, 2, 3], 10)      # 16
pow(2, 10)              # 1024
pow(2, 10, 1000)        # 24    (modular exponentiation)
divmod(7, 2)            # (3, 1)
```

## Iteration

### `len(seq)`

Length of a sequence / collection.

### `range(stop)` / `range(start, stop, step=1)`

Lazy integer sequence. Doesn't materialize the list:

```python
list(range(5))          # [0, 1, 2, 3, 4]
list(range(2, 8, 2))    # [2, 4, 6]
list(range(10, 0, -1))  # [10, 9, ..., 1]
```

### `enumerate(it, start=0)`

```python
for i, c in enumerate('hi', start=1):
    print(i, c)
# 1 h
# 2 i
```

### `zip(*iters)`

Stops at the shortest input:

```python
list(zip([1, 2, 3], ['a', 'b', 'c']))
# [(1, 'a'), (2, 'b'), (3, 'c')]
```

### `map(fn, *iters)` / `filter(fn, it)`

Lazy. Wrap in `list()` to materialize:

```python
list(map(str.upper, ['a', 'b']))    # ['A', 'B']
list(filter(lambda x: x > 0, [-1, 2, -3, 4]))    # [2, 4]
```

### `sorted(it, *, key=None, reverse=False)` / `reversed(seq)`

```python
sorted([3, 1, 4, 1, 5])              # [1, 1, 3, 4, 5]
sorted(['hello','hi'], key=len)      # ['hi', 'hello']
sorted([3, 1, 4], reverse=True)      # [4, 3, 1]
list(reversed([1, 2, 3]))            # [3, 2, 1]
```

### `iter(obj)` / `next(it, default=None)`

```python
it = iter([1, 2, 3])
next(it)                # 1
next(it)                # 2
```

### `all(it)` / `any(it)`

```python
all([1, 2, 3])          # True
all([1, 0, 3])          # False
any([0, 0, 3])          # True
any([])                 # False
```

## Code execution

### `compile(src, filename, mode)`

Compiles `src` (a string) to a code object. `mode` is `'exec'`,
`'eval'`, or `'single'`.

```python
co = compile('x = 42', '<string>', 'exec')
exec(co)
print(x)                # 42
```

### `eval(src, globals=None, locals=None)`

Evaluates a single expression. Returns the result.

```python
eval('2 + 2')           # 4
eval('x * 2', {'x': 21})  # 42
```

### `exec(src, globals=None, locals=None)`

Executes a multi-statement string. Returns `None`.

```python
exec('print("hi"); x = 5')
```

Both `eval` and `exec` are powerful and dangerous — treat input
strings the same way you'd treat a shell command.

### `globals()` / `locals()`

Return the current namespace as a `dict`:

```python
globals()['MY_CONST'] = 42
```

## Attribute access

### `getattr(obj, name, default=...)` / `setattr(obj, name, val)` / `hasattr(obj, name)` / `delattr(obj, name)`

```python
class C:  x = 42
getattr(C, 'x')         # 42
getattr(C, 'y', 0)      # 0  (default)
hasattr(C, 'x')         # True
setattr(C, 'y', 100)
del C.y                 # delattr(C, 'y')
```

### `dir(obj=None)`

Lists attribute names. With no arg, returns names in the current
scope.

```python
dir([])                 # ['__add__', 'append', 'clear', ...]
```

### `vars(obj=None)`

Returns `obj.__dict__`. With no arg, returns `locals()`.

### `id(obj)`

Returns the integer memory address of `obj`. Useful for "are these
the same object" checks (though `obj1 is obj2` is the idiomatic
way).

### `hash(obj)`

Returns the integer hash of an immutable object. Same hash for two
equal objects; uses an internal mix.

## File I/O

### `open(path, mode='r', encoding=None)`

Returns a file object. Modes:

- `'r'` — read text
- `'rb'` — read binary (returns `bytes` from `.read()`)
- `'w'` — write text (truncate)
- `'wb'` — write binary
- `'a'` — append (open create-if-missing currently maps to
  read-write; truncation is not yet wired to AH=0x6C)

`encoding` defaults to UTF-8 for text mode. The file object
supports `.read([n])`, `.readline()`, `.readlines()`, `.write(s)`,
`.close()`, iteration (`for line in f:`), and context manager
(`with open(...) as f:`).

DOS specifics — see [DOS file I/O](dos-features.md) for details on
8.3 filenames, CRLF handling, and INT 21h paths.

## Other

### `help(obj=None)`

Prints help text. `help()` with no args prints REPL hints.

### `repr(obj)`, `str(obj)`, `format(value, spec='')`

```python
format(3.14159, '.2f')   # '3.14'
format(255, 'x')         # 'ff'
```

### `slice(stop)` / `slice(start, stop, step=None)`

The slice constructor — usually you write `a[1:5:2]`, but you can
build slice objects explicitly:

```python
s = slice(1, 10, 2)
'abcdefghij'[s]          # 'bdfhj'
```

### `memoryview(obj)`

Zero-copy view into a bytes-like object:

```python
buf = bytearray(b'hello')
mv = memoryview(buf)
mv[0] = ord('H')
buf                      # bytearray(b'Hello')
```

### `bytearray(...)` / `bytes(...)` / `set(...)` / `frozenset(...)`

Same as the type constructors.

### `super()` — call methods of a parent class

```python
class A:
    def hi(self): return 'A'
class B(A):
    def hi(self): return 'B/' + super().hi()
B().hi()                 # 'B/A'
```

### `staticmethod` / `classmethod` / `property`

Decorators:

```python
class C:
    @staticmethod
    def add(a, b): return a + b
    @classmethod
    def from_str(cls, s): return cls(int(s))
    @property
    def doubled(self): return self.x * 2
```

### `callable(obj)`

True if `obj` can be called with `()`.

### `__import__(name)`

Low-level import primitive — use `import` instead unless you have
a reason.

## Functions NOT provided (vs CPython)

These are present in desktop Python but not built into this port:

- `breakpoint()` — no `pdb` backend
- `aiter` / `anext` — async iter helpers (use `async for` instead)
- `ascii(obj)` — use `repr()`
- `complex(...)` — no `cmath` / complex numbers
- `copyright()`, `credits()`, `license()` — REPL niceties
- `delattr` ... wait that IS provided. (above)
- `print_exception` — use `sys.print_exception()` instead

---

*Credit: ordering and signature shapes follow
[Python's built-in functions doc](https://docs.python.org/3/library/functions.html)
(PSF License, © 2001-2024 Python Software Foundation). Examples
adapted for the DOS context.*
