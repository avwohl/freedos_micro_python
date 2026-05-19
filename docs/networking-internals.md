---
title: Networking internals — packet driver + lwIP
---

# Networking internals: packet driver INT 60h, lwIP poll loop

Most users only need [`socket`](library/socket.md) +
[`uc386_net`](library/uc386_net.md). This page is for "the
network isn't working, where do I look?"

## The packet driver

DOS NIC drivers expose a "Crynwr Packet Driver" interface at INT
60h (configurable to other slots — 0x60 is by far the most
common). They handle the NIC-specific I/O and present a uniform
API the OS networking layer can call.

Common DOS packet drivers:

- `NE2000.COM` — generic NE2000-compatible (RTL8019, …)
- `PCNTPK.COM` — AMD PCnet (qemu's `-device pcnet`)
- `EL3C.COM` — 3Com Etherlink III
- `RTSPKT.COM` — RealTek 8169

You load one BEFORE running `MP.EXE`:

```
A:\> NE2000 0x60 9 0x300
A:\> MP.EXE
```

The args are: software-interrupt vector (0x60), hardware IRQ (9),
I/O base port (0x300). Defaults usually work.

## How this port finds the driver

`uc386_net.eth_init()` tries two paths in order:

### 1. PM-native NE2000 (preferred when available)

Direct I/O port access from PM, no DOS driver involvement. The
PM-native code in `port/pktdrv_uc386dos.c` knows the NE2000
register layout (CR, IO base 0x300, etc.) and reads/writes the
chip directly. Bypasses Crynwr's TSR.

Used by:
- Real DOS hardware with an NE2000-compatible card
- DOSBox-X's NE2000 emulation
- qemu's `-device ne2k_isa,iobase=0x300,irq=9`

Probe: write CR=0x21, read back; mismatch = no NE2000.

### 2. Crynwr packet driver thunk

INT 60h calls via the DPMI 0x0301 thunk we built for INT 21h.
Used when PM-native probe fails, including:

- dosbox-staging's synthetic packet driver (dosiz uses this)
- Real DOS with a non-NE2000 NIC (PCnet, 3Com, etc.) — driver
  loaded as a TSR, exposed at INT 60h

## lwIP layer

Above the packet driver runs [lwIP](https://savannah.nongnu.org/projects/lwip/),
a fully-featured TCP/IP stack. We use the "raw" lwIP API
(callback-based, not blocking socket). The [`socket`](library/socket.md)
module bridges the BSD-style API to lwIP's callbacks.

`lwip.callback()` does three things each call:

1. Drains any RX packets the packet driver has queued, hands them
   to lwIP for processing
2. Runs lwIP's periodic timers (TCP retransmit, ARP age-out, DHCP
   renew, …)
3. Triggers any pending TX

It's safe to call frequently — does nothing if there's nothing to
do. Most code in this port calls it implicitly (`socket.recv`,
`socket.accept`, asyncio loop).

## Debugging "no connectivity"

```python
import uc386_net, lwip, time
lwip.reset()
print('driver init:', uc386_net.eth_init(True))     # verbose
print('MAC:', uc386_net.eth_mac())
uc386_net.eth_set_static('10.0.2.15', '255.255.255.0', '10.0.2.2')
for _ in range(10):
    lwip.callback()
    time.sleep_ms(100)
print('status:', uc386_net.eth_status())

import socket
addr = socket.getaddrinfo('10.0.2.2', 22)[0][-1]
print('resolved to:', addr)
s = socket.socket()
s.settimeout(2.0)
try:
    s.connect(addr)
    print('connected!')
    s.close()
except OSError as e:
    print('connect failed:', e)
```

If `eth_init` returns non-zero: no packet driver. Load `NE2000.COM`
(or whatever your NIC needs) before running MP.EXE.

If `eth_status[3]` is False: link not up. Check cable, switch,
DHCP server.

If `getaddrinfo` fails: no DNS. Add a nameserver via
`uc386_net.eth_set_static(..., dns='8.8.8.8')` or set up DHCP.

If `connect` times out: the route is broken or the destination
isn't listening. From DOS, you have no `ping` to test with — try
a known-up host like `10.0.2.2` (the SLIRP gateway in qemu/dosbox).

## See also

- [`socket`](library/socket.md)
- [`uc386_net`](library/uc386_net.md)
- [`lwip`](library/lwip.md)
- [Troubleshooting](troubleshooting.md)
