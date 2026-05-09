// uc386-dos lwIP `arch/cc.h` shim. Included via `-I` rather than
// the conventional `arch/cc.h` path because uc386's preprocessor
// doesn't have a per-port arch include directory convention.
// build_port.sh adds `uc386-dos/` to the include path AND aliases
// the conventional name via a stub at uc386-dos/arch/cc.h that
// just `#include`s this file.

#ifndef LWIP_ARCH_CC_H
#define LWIP_ARCH_CC_H

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>

// Endianness — i386 is little-endian.
#define BYTE_ORDER LITTLE_ENDIAN
#ifndef LITTLE_ENDIAN
#define LITTLE_ENDIAN 1234
#define BIG_ENDIAN    4321
#endif

// Packed structs — uc386 honors `__attribute__((packed))` (member
// byte-alignment, struct alignment 1). lwIP's init.c self-tests the
// layout via `sizeof(struct { u8; u32; } __packed) == 5` at boot;
// without real packing this fails because u32 lands at offset 4.
// The other PACK_STRUCT_* slots (BEGIN/END/FIELD) remain empty —
// upstream's lwip/arch.h defines them that way for GCC/clang.
#define PACK_STRUCT_FIELD(x) x
#define PACK_STRUCT_STRUCT __attribute__((packed))
#define PACK_STRUCT_BEGIN
#define PACK_STRUCT_END

// Diagnostics — route lwIP's debug printf through the regular
// printf chain. LWIP_DEBUG=0 in lwipopts.h makes these no-ops in
// the common case.
#include <stdio.h>
#define LWIP_PLATFORM_DIAG(x) do { printf x; } while (0)

// sys_prot_t — a critical-section level. We're NO_SYS=1 with no
// real interrupts to mask, so use a stub type and let lwipopts.h's
// SYS_ARCH_* macros expand to nothing.
typedef uint32_t sys_prot_t;

// strerror — uc386's libc has it.

#endif  // LWIP_ARCH_CC_H
