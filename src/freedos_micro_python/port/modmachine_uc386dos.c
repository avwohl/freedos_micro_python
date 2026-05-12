// FreeDOS / uc386 port supplement for upstream extmod/modmachine.c.
// Pulled in via MICROPY_PY_MACHINE_INCLUDEFILE — never compiled
// standalone.
//
// Scope: only the bare minimum extmod/modmachine.c demands of the
// port — a single `mp_machine_idle()` symbol. Everything else
// (reset, freq, unique_id, lightsleep, disable_irq) is gated behind
// MICROPY_PY_MACHINE_RESET / MICROPY_PY_MACHINE_BARE_METAL_FUNCS /
// MICROPY_PY_MACHINE_DISABLE_IRQ_ENABLE_IRQ — those are off in our
// mpconfigport.h so this file doesn't need to provide their helpers.
//
// What IS useful on DOS comes from upstream extmod/machine_mem.c
// (mem8/16/32) directly: under DOS/32A's flat 32-bit DS, linear
// addresses 0..4G are mapped 1:1, so `machine.mem8[0x400]` reads
// the BIOS data area, `machine.mem8[0xB8000]` is VGA text mode
// memory, and so on. No translation layer needed — the default
// `machine_mem_get_addr` cast-to-pointer implementation works.
//
// Under CWSDPMI the picture changes: low pages may be intentionally
// unmapped (page 0 traps NULL deref, etc.) so a `machine.mem8[0x4]`
// read PFs. That's a property of the DPMI host, not something the
// port can paper over — document it where it matters.

static void mp_machine_idle(void) {
    // No-op: DOS has no scheduler to yield to. A `hlt` here would
    // either trap to the DPMI host's idle loop (best case) or hang
    // the VM (worst case under bare-metal builds); silent return is
    // the safe choice that matches the upstream unix port's stance.
}
