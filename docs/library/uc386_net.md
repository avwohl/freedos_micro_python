---
title: uc386_net
---

# `uc386_net` — NIC + interface configuration

```python
import uc386_net
```

DOS-specific network setup: probe the packet driver at INT 60h,
configure DHCP or static addressing, query interface status.

## `uc386_net.eth_init(verbose=False)` → int

Probe for a Crynwr-compatible packet driver at INT 60h. Returns 0
on success. Required before `socket` is used.

```python
import uc386_net
rc = uc386_net.eth_init()
if rc != 0:
    raise RuntimeError('no packet driver — load NE2000.COM first')
```

The function tries two paths:

1. **PM-native NE2000** (this port has direct I/O port drivers for
   the common NE2000 register layout). Used when probed and found
   on real hardware / dosbox / qemu.
2. **Crynwr packet driver thunk** (calls INT 60h via DPMI 0x0301).
   Used when the PM-native probe fails. dosbox-x and dosiz
   expose synthetic Crynwr drivers this way.

`verbose=True` prints which path was taken.

## Address configuration

### `uc386_net.eth_set_static(ip, netmask, gateway, dns=None)`

```python
uc386_net.eth_set_static('10.0.2.15', '255.255.255.0', '10.0.2.2')
# Optional DNS:
uc386_net.eth_set_static('10.0.2.15', '255.255.255.0', '10.0.2.2',
                         '8.8.8.8')
```

### `uc386_net.eth_set_dhcp()` — request DHCP lease

```python
uc386_net.eth_set_dhcp()
for i in range(50):                # wait up to ~5s
    lwip.callback()
    time.sleep_ms(100)
    info = uc386_net.eth_status()
    if info[3]:
        break
print('got lease', info[0])
```

## Status

### `uc386_net.eth_status()` → tuple

`(ip, netmask, gateway, ifup)`.

```python
>>> uc386_net.eth_status()
('10.0.2.15', '255.255.255.0', '10.0.2.2', True)
```

### `uc386_net.eth_mac()` → str

```python
>>> uc386_net.eth_mac()
'52:54:00:12:34:56'
```

## Minimal bring-up template

```python
import lwip, uc386_net, time

lwip.reset()
rc = uc386_net.eth_init(False)
if rc != 0:
    raise RuntimeError('NIC init failed')

uc386_net.eth_set_static('10.0.2.15', '255.255.255.0', '10.0.2.2')

# Poke the timer a few times so the interface comes up cleanly.
for _ in range(5):
    lwip.callback()
    time.sleep_ms(50)

info = uc386_net.eth_status()
print('ifup:', info[3], 'ip:', info[0])
```

After this, `socket.socket()` etc. work.

## See also

- [`socket`](socket.md), [`lwip`](lwip.md), [`ssl`](ssl.md),
  [`_ssh`](_ssh.md)
- [Packet driver INT 60h, lwIP poll loop](../networking-internals.md)
  for the low-level mechanics.

---

*This is a port-specific module; no upstream-MicroPython
counterpart.*
