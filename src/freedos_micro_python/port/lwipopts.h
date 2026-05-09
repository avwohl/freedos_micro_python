// uc386-dos lwipopts.h — minimum-viable config for IPv4-only TCP/UDP
// with loopback testing. No threading (NO_SYS=1), no DHCP for now,
// no IPv6, no stats. Just enough surface so MICROPY_PY_LWIP=1 can
// expose `socket` and a Python program can `socket.socket()`,
// `bind`, `listen`, `connect`, `send`, `recv` over 127.0.0.1.
//
// Tuning will follow once we have real packet I/O wired up — DHCP
// + DNS + a real netif driver are deferred until the dos-emu fake
// packet driver (or real-DOS testing) lands.

#ifndef MICROPY_INCLUDED_LWIPOPTS_H
#define MICROPY_INCLUDED_LWIPOPTS_H

// MicroPython's modlwip.c uses the raw API (tcp_new etc.) on a
// single-threaded foreground loop. NO_SYS=1 disables the
// thread-aware netconn/socket API and the rtos abstractions —
// matches the rp2 / esp32-port shape for raw-mode lwip.
#define NO_SYS                          1
#define LWIP_RAW                        1
#define LWIP_NETCONN                    0
#define LWIP_SOCKET                     0
#define LWIP_NETIF_API                  0

// Core protocols. ARP+IP for the link layer, ICMP+IGMP for control,
// TCP+UDP for transport, DNS for name resolution, AUTOIP for fallback
// addressing when DHCP isn't running.
#define LWIP_ARP                        1
#define LWIP_ETHERNET                   1
#define LWIP_IPV4                       1
#define LWIP_IPV6                       0
#define LWIP_ICMP                       1
#define LWIP_IGMP                       1
#define LWIP_TCP                        1
#define LWIP_UDP                        1
#define LWIP_DNS                        1
#define LWIP_DHCP                       1
#define LWIP_AUTOIP                     0
#define LWIP_ACD                        0
#define LWIP_DHCP_DOES_ACD_CHECK        0  // ACD off → don't probe
#define LWIP_DHCP_BOOTP_FILE            0
#define LWIP_DHCP_GET_NTP_SRV           0
#define LWIP_DHCP_PROVIDE_DNS_SERVERS   1  // accept DNS from option 6
// dhcp.c includes <stddef.h> via lwIP's def.h, then time.h via
// timeouts.h's chain — both typedef size_t under _SIZE_T_DEFINED.
// stddef.h's separate _PTRDIFF_T_DEFINED guard (fixed in lib/include)
// keeps ptrdiff_t reachable.

// Loopback — lets unit tests bind/connect 127.0.0.1 to itself
// without any real netif driver. Phase 1 testing relies entirely
// on this; phase 2 will add a packet-driver netif alongside.
#define LWIP_HAVE_LOOPIF                1
#define LWIP_NETIF_LOOPBACK             1
#define LWIP_LOOPBACK_MAX_PBUFS         8

// Memory. uc386's GC heap is 256 KB total — keep lwIP's footprint
// modest. These knobs roughly mirror the minimal-port config from
// upstream lwIP examples.
#define MEM_ALIGNMENT                   4
#define MEM_SIZE                        4096
#define MEMP_NUM_PBUF                   16
#define MEMP_NUM_RAW_PCB                4
#define MEMP_NUM_UDP_PCB                4
#define MEMP_NUM_TCP_PCB                4
#define MEMP_NUM_TCP_PCB_LISTEN         4
#define MEMP_NUM_TCP_SEG                16
#define MEMP_NUM_REASSDATA              4
#define MEMP_NUM_FRAG_PBUF              4
#define MEMP_NUM_ARP_QUEUE              2
#define MEMP_NUM_IGMP_GROUP             2
#define MEMP_NUM_SYS_TIMEOUT            8
#define PBUF_POOL_SIZE                  16
#define PBUF_POOL_BUFSIZE               256
#define TCP_MSS                         536
#define TCP_WND                         (4 * TCP_MSS)
#define TCP_SND_BUF                     (4 * TCP_MSS)
#define TCP_SND_QUEUELEN                (4 * (TCP_SND_BUF / TCP_MSS))

// Statistics off — saves a few KB.
#define LWIP_STATS                      0

// LWIP_DEBUG is INTENTIONALLY left undefined here. err.h gates
// `extern const char *lwip_strerr(err_t)` on `#ifdef LWIP_DEBUG`,
// so even `#define LWIP_DEBUG 0` is enough to pull in the extern
// reference (which isn't satisfied because lwip_strerr's
// definition lives behind LWIP_DEBUG too). Leaving it undefined
// makes err.h fall through to `#define lwip_strerr(x) ""`.

// Critical-section primitives. NO_SYS=1 with no real interrupts
// to mask, so make these all no-ops. modlwip.c's tcpip thread
// abstraction uses both macro and function forms — provide both.
#define SYS_ARCH_DECL_PROTECT(lev)   do { } while (0)
#define SYS_ARCH_PROTECT(lev)        do { } while (0)
#define SYS_ARCH_UNPROTECT(lev)      do { } while (0)
#define sys_arch_protect()           ((sys_prot_t)0)
#define sys_arch_unprotect(lev)      do { (void)(lev); } while (0)

// We don't have an OS to call. Print the assert message + abort()
// so the dos_emu test path surfaces which assertion tripped (the
// message + filename + line make it down to stdout before exit).
#include <stdio.h>
#define LWIP_PLATFORM_ASSERT(x) \
    do { printf("lwIP assert: %s at %s:%d\n", (x), __FILE__, __LINE__); \
         abort(); } while (0)

// Use the standard lwip_htons/lwip_htonl from def.c — uc386 doesn't
// ship a system byteorder.h.
// (Don't define LWIP_DONT_PROVIDE_BYTEORDER_FUNCTIONS — that path
// expects htons/htonl etc. to come from elsewhere.)

// Random — lwIP's DNS uses LWIP_RAND() for transaction IDs / port
// allocation. Route through libc's `random()` (LCG, seeded by
// MICROPY_PY_RANDOM_SEED_INIT_FUNC's bios_ticks() at boot). Not
// crypto-strength, but transaction-ID strength is fine.
extern long random(void);
#define LWIP_RAND() ((unsigned long)random())

#endif  // MICROPY_INCLUDED_LWIPOPTS_H
