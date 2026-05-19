---
title: machine
---

# `machine` — direct hardware access

```python
import machine
```

Read / write 8-, 16-, 32-bit values at arbitrary linear addresses
in protected mode. On DOS under PMODE/W this gives you access to
the entire 32-bit address space — conventional + extended memory,
ISA bus, VGA frame buffer, etc.

⚠️ **There is no protection.** Writing to the wrong address can
hang DOS, crash PMODE/W, or scribble on PMODE/W's own internal
structures. This is the lowest-level interface in MicroPython —
use it the way you'd use a debugger's "edit memory" feature.

## `machine.mem8` / `machine.mem16` / `machine.mem32`

Subscriptable read/write at the given address:

```python
import machine

# VGA text-mode buffer is at linear 0xB8000 (each cell is 2 bytes:
# attribute + char).
machine.mem16[0xB8000] = 0x0F41        # bright white 'A' at top-left

# Read it back
print(hex(machine.mem16[0xB8000]))     # '0xf41'

# Read 4 bytes at the BIOS data area's tick counter:
ticks = machine.mem32[0x46C]
print('BIOS ticks since midnight:', ticks)
```

The base address is a flat 32-bit linear address. PMODE/W
identity-maps conventional memory (below 1 MB), so `0xB8000` is
the actual VGA buffer.

## Use cases

### Read BIOS data area

```python
# Equipment word at 0x410
equip = machine.mem16[0x410]
print('serial ports:', (equip >> 9) & 7)
print('floppies:', (equip >> 6) & 3)
```

### Write a marker to VRAM

```python
# Top-left of color text mode: blink + green-on-blue '@'
machine.mem16[0xB8000] = 0x1A40
```

### Snoop / replace an interrupt vector

```python
# Real-mode IVT entry for INT 0x21
ivt_21 = (machine.mem16[0x21 * 4 + 2] << 16) | machine.mem16[0x21 * 4]
print(f'INT 21 handler at 0x{ivt_21:08x}')
```

(For a PM IDT entry, see how `dosint21.py` does it — different
table.)

## Constants

There aren't any built-in constants for memory regions; you supply
the linear address.

## Not implemented

The wider `machine` module that microcontroller MicroPython exposes
— `machine.Pin`, `machine.I2C`, `machine.SPI`, `machine.UART`,
`machine.RTC`, `machine.WDT`, `machine.Timer` — is NOT provided.
DOS has no pin model; ISA peripherals would each need a typed
driver.

The lower-level `machine.mem*` accessors are enough to talk to ISA
devices directly. For a real `RTC` you can call INT 1Ah AH=2 via
[`dosint21`](dosint21.md).

## See also

- [`dosint21`](dosint21.md) — INT 21h dispatch
- [DOS file I/O](../dos-features.md)

---

*Credit: surface from
[MicroPython machine docs](https://docs.micropython.org/en/latest/library/machine.html)
(MIT). DOS context is this port's.*
