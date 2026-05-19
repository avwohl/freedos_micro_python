---
title: asyncio
---

# `asyncio` — cooperative async I/O

```python
import asyncio
```

A single-threaded event loop, modelled on CPython's `asyncio`.
Useful for managing multiple concurrent socket connections without
threads.

## Basics

### Define a coroutine

```python
async def greet(name):
    await asyncio.sleep(0.5)
    print('hello', name)
```

### Run a coroutine

```python
asyncio.run(greet('World'))
# (waits 500 ms, then prints 'hello World')
```

### Run several together — `asyncio.gather`

```python
async def main():
    await asyncio.gather(
        greet('A'),
        greet('B'),
        greet('C'),
    )

asyncio.run(main())
# Three greetings appear roughly together after 500 ms (not 1500 ms).
```

### Run with a timeout — `asyncio.wait_for`

```python
async def slow():
    await asyncio.sleep(10)
    return 'done'

try:
    result = await asyncio.wait_for(slow(), 1.0)
except asyncio.TimeoutError:
    print('timed out')
```

## Streams (TCP)

`asyncio.open_connection(host, port)` returns `(reader, writer)`:

```python
async def fetch_motd():
    r, w = await asyncio.open_connection('10.0.2.2', 17)   # qotd
    line = await r.readline()
    print('motd:', line)
    w.close()
    await w.wait_closed()

asyncio.run(fetch_motd())
```

`StreamReader`: `.read(n)`, `.readline()`, `.readexactly(n)`,
`.readuntil(sep)`.
`StreamWriter`: `.write(data)`, `.drain()`, `.close()`,
`.wait_closed()`, `.get_extra_info(name)`.

### TCP server

```python
async def handle(reader, writer):
    data = await reader.read(100)
    writer.write(data.upper())
    await writer.drain()
    writer.close()

asyncio.run(asyncio.start_server(handle, '0.0.0.0', 1234))
```

## Sleep + sync primitives

- `asyncio.sleep(s)` — yield for `s` seconds (float)
- `asyncio.sleep_ms(ms)` — yield for `ms` milliseconds
- `asyncio.Event()` — set/wait/clear sync primitive
- `asyncio.Lock()` — async mutex
- `asyncio.Queue(maxsize=0)` — FIFO; `.put()` / `.get()` are awaitables

## Tasks

```python
async def background():
    while True:
        await asyncio.sleep(1)
        print('tick')

async def main():
    task = asyncio.create_task(background())   # runs concurrently
    await asyncio.sleep(5)
    task.cancel()                              # raises CancelledError in it
    try:
        await task
    except asyncio.CancelledError:
        pass

asyncio.run(main())
```

## Watch out

- **No threads.** Everything happens on one loop. A long-running
  synchronous call (busy loop, blocking socket without timeout)
  blocks all coroutines.
- **No subprocess support.** DOS has no fork/exec, so `asyncio.subprocess` isn't bundled.
- **The lwIP packet poll** still needs to happen. `asyncio`'s
  socket integration does it for you when you use the
  `open_connection` / `start_server` helpers; if you `await` a
  long sleep with no socket traffic, packets may pile up. The
  rigs typically sprinkle `lwip.callback()` calls into
  long-running sleeps.

---

*Credit: shape from
[MicroPython asyncio docs](https://docs.micropython.org/en/latest/library/asyncio.html)
(MIT) and
[CPython asyncio tutorial](https://docs.python.org/3/library/asyncio.html)
(PSF).*
