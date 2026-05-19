---
title: re
---

# `re` — regular expressions

```python
import re
```

Pattern matching for strings and bytes, via re1.5. Supports the
most common metacharacters; not as full-featured as CPython's
`re` (no lookahead, no Unicode classes), but covers the realistic
DOS use cases (parsing CONFIG.SYS, log files, etc.).

## Quick syntax

| Pattern  | Matches                                  |
|----------|------------------------------------------|
| `.`      | any single character                     |
| `\d`     | digit `[0-9]`                            |
| `\D`     | non-digit                                |
| `\s`     | whitespace `[ \t\n\r\f]`                 |
| `\S`     | non-whitespace                           |
| `\w`     | word char `[A-Za-z0-9_]`                 |
| `\W`     | non-word                                 |
| `[abc]`  | one of a, b, c                           |
| `[^abc]` | none of a, b, c                          |
| `[a-z]`  | range                                    |
| `^`      | start of string                          |
| `$`      | end of string                            |
| `\b`     | word boundary                            |
| `*`      | zero or more (greedy)                    |
| `+`      | one or more (greedy)                     |
| `?`      | zero or one                              |
| `*?` `+?` | non-greedy                              |
| `{n}` `{m,n}` | counted repetition                  |
| `(...)`  | capture group                            |
| `(?:...)` | non-capturing group                     |
| `\|`     | alternation                              |
| `\\`     | literal backslash                        |

Note: use `r'raw strings'` for patterns to avoid `\b` becoming a
backspace etc.

## Compile + match

### `re.compile(pattern)` → `Pattern`

```python
p = re.compile(r'\d+')
```

### `Pattern.match(s)` — match anchored at start

```python
m = p.match('42 banana')
m.group()                    # '42'
m.span()                     # (0, 2)
m.start(), m.end()           # 0, 2

if p.match('hello'):
    ...
else:
    print('no match')         # this branch
```

### `Pattern.search(s)` — match anywhere

```python
m = p.search('item 42 of 100')
m.group()                    # '42'
m.span()                     # (5, 7)
```

### `Pattern.findall(s)`

```python
re.compile(r'\d+').findall('a 1 b 22 c 333')   # ['1', '22', '333']
```

### `Pattern.split(s, maxsplit=-1)`

```python
re.compile(r'\s+').split('  a   b\tc  ')       # ['', 'a', 'b', 'c', '']
```

### `Pattern.sub(repl, s, count=-1)`

```python
re.compile(r'\d+').sub('#', 'item 42, item 99')
# 'item #, item #'
re.compile(r'(\w+)@(\w+)').sub(r'\1 at \2', 'foo@bar')
# 'foo at bar'
```

`repl` can be a string with `\1`, `\2`, ... back-references, or a
callable invoked with the Match object.

## Groups

```python
m = re.compile(r'(\w+)=(\w+)').match('name=Dave')
m.group(0)                   # 'name=Dave'  (whole match)
m.group(1)                   # 'name'
m.group(2)                   # 'Dave'
m.groups()                   # ('name', 'Dave')
m.span(1)                    # (0, 4)
```

## Module-level shortcuts

```python
re.match(pattern, s)
re.search(pattern, s)
re.findall(pattern, s)
re.split(pattern, s)
re.sub(pattern, repl, s)
```

Each compiles the pattern internally — fine for one-shot use, but
compile once if you'll match repeatedly.

## Not supported

- Lookahead / lookbehind (`(?=...)`, `(?<=...)`)
- Unicode character classes (`\d` matches `[0-9]`, not Unicode digits)
- Named groups (`(?P<name>...)`)
- Verbose mode (`re.X`)
- Most flags (`re.I`, `re.M`, etc.)

For these, you'll need real CPython.

---

*Credit: surface from
[MicroPython re docs](https://docs.micropython.org/en/latest/library/re.html)
(MIT). Pattern table is original.*
