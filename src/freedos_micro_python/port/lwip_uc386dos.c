// uc386-dos lwIP port glue. Provides:
//   - sys_now() — millisecond tick from BIOS (INT 1Ah AH=0).
//   - lwip_uc386dos_init() — calls lwip_init() once + adds the
//     loopback netif. Called from main.c at boot.
//   - mp_module_socket — re-export of lwIP's mp_module_lwip under
//     `MP_QSTR_socket` so `import socket` lights up the BSD-style
//     surface modlwip.c provides.
//
// We don't ship modnetwork.c — that's the higher-level network
// configuration interface used by ports with WLAN/Ethernet drivers.
// For loopback-only Phase 1 testing, the raw socket surface
// (modlwip.c → MP_QSTR_socket) is sufficient.

#include <string.h>

#include "py/runtime.h"
#include "lwip/init.h"
#include "lwip/timeouts.h"
#include "lwip/netif.h"
#include "lwip/dhcp.h"
#include "lwip/etharp.h"
#include "lwip/pbuf.h"
#include "netif/ethernet.h"
#if LWIP_HAVE_LOOPIF
#include "lwip/sys.h"
#endif

// INT 0x83 packet-driver shim — see lib/i386_dos_libc.asm. dos_emu
// intercepts and routes to a NetworkSimulator (sim mode).
extern int ethdrv_init(unsigned char mac[6]);
extern int ethdrv_send(const unsigned char *buf, unsigned int len);
extern unsigned int ethdrv_recv(unsigned char *buf, unsigned int maxlen);

// Crynwr packet-driver bindings (real-DOS path) — pktdrv_uc386dos.c.
// Returns 0 on success when a "PKT DRVR" signature is found in the
// IVT 0x60–0x7F range; otherwise we fall back to the INT 0x83 sim.
extern int pktdrv_init(unsigned char mac[6]);
extern int pktdrv_send(const unsigned char *buf, unsigned int len);
extern unsigned int pktdrv_recv(unsigned char *buf, unsigned int maxlen);
extern int pktdrv_is_active(void);

// `bios_ticks()` from lib/i386_dos_libc.asm — INT 1Ah AH=0 read of
// the BIOS tick counter (~18.2 Hz). Multiply by 55 for an
// approximate millisecond clock; lwIP's timeouts only need
// monotonic ms, not absolute wall time, so the small drift is fine.
extern unsigned bios_ticks(void);

uint32_t sys_now(void) {
    return bios_ticks() * 55u;
}

#if LWIP_HAVE_LOOPIF
// One-shot lwIP init. Call from `main()` after mp_init.
// Loopback netif is auto-added by lwip_init() when LWIP_HAVE_LOOPIF
// is on, so we just call lwip_init() and we're done.
void lwip_uc386dos_init(void) {
    lwip_init();
}
#endif

// Drive periodic timer checks. Call from the REPL idle loop or a
// dedicated tick. modlwip.c expects sys_check_timeouts() to be
// pumped regularly so TCP retransmits / DNS timeouts fire.
void lwip_uc386dos_poll(void) {
    sys_check_timeouts();
}

// Stub for `mp_mod_network_prefer_dns_use_ip_version` — modlwip.c
// reads it inside getaddrinfo() to pick AF_INET vs AF_INET6 ordering.
// modnetwork.c (which we don't compile) defines it as a real global;
// we ship the stub here defaulting to IPv4 preferred. If we ever
// turn LWIP_IPV6 on, switch this to 6 or wire modnetwork properly.
int mp_mod_network_prefer_dns_use_ip_version = 4;

// modlwip.c also references the hostname buffer that modnetwork.c
// otherwise owns. Provide a stub matching the symbol shape.
char mod_network_hostname_data[16 + 1] = "uc386-dos";

// ---- Virtual ethernet netif (uc386dos_eth) ---------------------
//
// A second netif on top of the INT 0x83 packet-driver shim. Used
// for DHCP discovery and any non-loopback IPv4 traffic the program
// wants. Stays separate from the loopback netif so 127.0.0.1 traffic
// doesn't bounce off the wire.
//
// Frame path:
//   TX: lwIP → etharp_output → ethdrv_send → INT 0x83 AH=1
//                                          → dos_emu hook
//                                          → NetworkSimulator
//   RX: ethdrv_recv (INT 0x83 AH=2) → pbuf_alloc + take
//                                   → ethernet_input → ip_input
// Pumped from `uc386dos_loopback_poll` (which is wired in as
// modlwip's poll-list hook), so `lwip.callback()` drives both
// loopback delivery and eth RX in one shot.

static struct netif uc386dos_eth_netif;
static int uc386dos_eth_active = 0;

// Static TX scratch — one outgoing frame at a time, no concurrency
// since NO_SYS=1 + single-threaded REPL. Sized to lwIP MTU + eth hdr.
static unsigned char uc386dos_eth_tx_buf[1500 + 14];
static unsigned char uc386dos_eth_rx_buf[1500 + 14];

static err_t uc386dos_eth_linkoutput(struct netif *netif, struct pbuf *p) {
    (void)netif;
    if (p->tot_len > sizeof(uc386dos_eth_tx_buf)) {
        return ERR_IF;
    }
    pbuf_copy_partial(p, uc386dos_eth_tx_buf, p->tot_len, 0);
    int rc;
    if (pktdrv_is_active()) {
        rc = pktdrv_send(uc386dos_eth_tx_buf, p->tot_len);
    } else {
        rc = ethdrv_send(uc386dos_eth_tx_buf, p->tot_len);
    }
    return (rc != 0) ? ERR_IF : ERR_OK;
}

static err_t uc386dos_eth_netif_init_cb(struct netif *netif) {
    netif->name[0] = 'e';
    netif->name[1] = 'n';
    netif->output = etharp_output;
    netif->linkoutput = uc386dos_eth_linkoutput;
    netif->mtu = 1500;
    netif->hwaddr_len = 6;
    netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP
                 | NETIF_FLAG_ETHERNET;
    // hwaddr is filled in by uc386dos_eth_start() before netif_add.
    return ERR_OK;
}

// Pull queued frames from the RX path and feed them into lwIP.
// When Crynwr is the active driver, drain `pktdrv_recv` — under
// dos_emu the harness fills the buffer through AH=0x99 polling
// registration; on real DOS this requires a DPMI INT 31h fn 0x0303
// trampoline (see pktdrv_uc386dos.c) that's still TODO. With INT
// 0x83 sim active, drain `ethdrv_recv` instead. The two never
// coexist in a single boot.
static void uc386dos_eth_pump_rx(void) {
    if (!uc386dos_eth_active) {
        return;
    }
    for (;;) {
        unsigned int len = pktdrv_is_active()
            ? pktdrv_recv(uc386dos_eth_rx_buf,
                          sizeof(uc386dos_eth_rx_buf))
            : ethdrv_recv(uc386dos_eth_rx_buf,
                          sizeof(uc386dos_eth_rx_buf));
        if (len == 0) {
            break;
        }
        struct pbuf *p = pbuf_alloc(PBUF_RAW, (u16_t)len, PBUF_POOL);
        if (p == NULL) {
            // Drop the frame; lwIP will time out and retransmit.
            continue;
        }
        pbuf_take(p, uc386dos_eth_rx_buf, (u16_t)len);
        if (uc386dos_eth_netif.input(p, &uc386dos_eth_netif) != ERR_OK) {
            pbuf_free(p);
        }
    }
}

// Bring the eth netif up. Calls INT 0x83 to fetch the host-assigned
// MAC, registers the netif, and (if dhcp_start_now) kicks off DHCP
// discovery. Idempotent — second call is a no-op.
int uc386dos_eth_start(int dhcp_start_now) {
    if (uc386dos_eth_active) {
        return 0;
    }
    unsigned char mac[6];
    // Try a real Crynwr packet driver first (so the same binary
    // lights up on hardware when one's loaded). Fall back to the
    // INT 0x83 sim — what dos_emu provides without a fake Crynwr.
    if (pktdrv_init(mac) != 0 && ethdrv_init(mac) != 0) {
        return -1;
    }
    ip4_addr_t ip0, nm0, gw0;
    ip0.addr = 0;
    nm0.addr = 0;
    gw0.addr = 0;
    if (netif_add(&uc386dos_eth_netif, &ip0, &nm0, &gw0, NULL,
                  uc386dos_eth_netif_init_cb, ethernet_input) == NULL) {
        return -2;
    }
    memcpy(uc386dos_eth_netif.hwaddr, mac, 6);
    netif_set_default(&uc386dos_eth_netif);
    netif_set_up(&uc386dos_eth_netif);
    netif_set_link_up(&uc386dos_eth_netif);
    uc386dos_eth_active = 1;
    if (dhcp_start_now) {
        if (dhcp_start(&uc386dos_eth_netif) != ERR_OK) {
            return -3;
        }
    }
    return 0;
}

// Static-IP override path for tests / environments where DHCP isn't
// available. Caller has already run uc386dos_eth_start(False) so the
// netif exists; this just clears any DHCP state and writes the
// addresses directly.
void uc386dos_eth_set_addr(unsigned int ip, unsigned int mask, unsigned int gw) {
    if (!uc386dos_eth_active) return;
    dhcp_release_and_stop(&uc386dos_eth_netif);
    ip4_addr_t a, n, g;
    a.addr = ip; n.addr = mask; g.addr = gw;
    netif_set_addr(&uc386dos_eth_netif, &a, &n, &g);
    netif_set_up(&uc386dos_eth_netif);
}

// Read-back helpers for the Python-facing module to inspect state.
unsigned int uc386dos_eth_ip(void)      { return uc386dos_eth_netif.ip_addr.addr; }
unsigned int uc386dos_eth_netmask(void) { return uc386dos_eth_netif.netmask.addr; }
unsigned int uc386dos_eth_gateway(void) { return uc386dos_eth_netif.gw.addr; }
int uc386dos_eth_is_up(void)            { return uc386dos_eth_active && netif_is_up(&uc386dos_eth_netif); }

// 0 = no eth init yet, 1 = INT 0x83 sim, 2 = Crynwr packet driver.
int uc386dos_eth_driver(void) {
    if (!uc386dos_eth_active) return 0;
    return pktdrv_is_active() ? 2 : 1;
}

// Loopback packet pump. With LWIP_NETIF_LOOPBACK=1 + NO_SYS=1, lwIP
// queues outgoing-to-127.0.0.1 packets in netif->loop_first/last and
// only delivers them when netif_poll(netif) is called. The loopback
// netif (`loop_netif`) is created by netif_init() but isn't promoted
// to `netif_default`, so use netif_poll_all() — it walks every netif
// on `netif_list` and is provided when LWIP_NETIF_LOOPBACK_MULTITHREADING
// is 0 (our case). Registered from mod_lwip_reset (patched in
// fetch.sh) so each `lwip.reset()` re-arms the loopback pump
// alongside the usual sys_check_timeouts() cadence. Also drains the
// virtual-eth RX queue so DHCP responses + ARP replies land.
void uc386dos_loopback_poll(void *arg) {
    (void)arg;
    extern int write(int fd, const void *buf, unsigned int n);
    write(1, "[lp:np]", 7);
    netif_poll_all();
    write(1, "[lp:rx]", 7);
    uc386dos_eth_pump_rx();
    write(1, "[lp:done]", 9);
}

// Expose `mp_module_lwip` as `socket` too. modlwip.c registers
// itself under MP_QSTR_lwip and MP_QSTR_socket via `MP_REGISTER_*`
// markers — but our hand-rolled moduledefs.h doesn't process those.
// The `socket` registration goes through UCDOS_MOD_ENTRY_SOCKET in
// build.sh's heredoc; we don't need any C-side glue for that.
