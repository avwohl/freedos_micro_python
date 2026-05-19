---
title: lwip
---

# `lwip` — raw lwIP TCP/IP stack

```python
import lwip
```

Direct access to the bundled
[lwIP](https://savannah.nongnu.org/projects/lwip/) stack. Most
users only need `lwip.reset()` and `lwip.callback()`; the higher-
level [`socket`](socket.md) module sits on top of this.

## `lwip.reset()`

Re-initialise the entire stack: tear down all PCBs, flush ARP /
DNS / routing caches, re-add the loopback interface. Call once at
program start (before `socket` is used) and any time you want a
clean slate (e.g. after switching networks).

```python
import lwip
lwip.reset()
```

## `lwip.callback()` → int

Run the lwIP timer + RX poll once. Returns the number of TCP
timer ticks that fired. Most networking code in this port calls
this implicitly:

- `socket.recv()` / `socket.accept()` poll automatically
- `asyncio` event loop polls per iteration
- The bundled `wget.py` / `scp.py` / `sftp.py` insert calls in
  their main loops

You only need to call it explicitly when you're holding the CPU in
a tight Python loop with no socket I/O — typically you wouldn't.

```python
# Pattern: warm up after eth_init/static
for i in range(5):
    lwip.callback()
    time.sleep_ms(50)
```

## `lwip.tcp_info(sock_fileno)` — debug

Returns a dict of stats for a TCP socket's underlying PCB.
Useful for diagnosing "why did this stall" problems.

```python
info = lwip.tcp_info(s.fileno())
print(info)
# {'state': 'ESTABLISHED', 'snd_wnd': 65535, ...}
```

## Versions / build

```python
>>> lwip.version()
'2.1.3'
```

## See also

- [`uc386_net`](uc386_net.md) — packet driver detection + interface
  configuration (DHCP / static)
- [`socket`](socket.md) — the BSD socket API on top

---

*This is a port-specific module; no upstream-MicroPython
counterpart.*
