/* tls_smoke — standalone PM client that does a TLS handshake against
 * tls_server.py without MicroPython. Same DPMI 0x0301 thunk path the
 * dpmi_int21_smoke established for file I/O, plus the PM-native
 * NE2000 driver and lwIP and axtls.
 *
 * The TLS-specific glue here is what we want to lift into MP's port
 * later — modtls_axtls.c at upstream level, plus our SOCKET_READ/
 * WRITE bindings, plus the axtls context lifecycle.
 *
 * Stages (each prints a [smoke:...] marker so the rig sees how far
 * we got):
 *
 *   [s0]  eth init (calls into port/pktdrv_uc386dos.c — PM-native
 *         NE2000 driver, already proven in the TLS rig TCP handshake)
 *   [s1]  static IP setup (10.0.2.15/24, gw 10.0.2.2)
 *   [s2]  lwIP raw TCP — tcp_new + tcp_connect to host's TLS server
 *   [s3]  poll until connected (drives lwip.callback() = NE2000 RX
 *         pump + sys_check_timeouts)
 *   [s4]  ssl_ctx_new + ssl_client_new — axtls TLS handshake begins
 *   [s5]  poll until handshake completes (ssl_read with empty data
 *         drives the handshake state machine)
 *   [s6]  ssl_write hello, ssl_read response, compare against
 *         expected marker, PASS or FAIL
 */

#include <stdint.h>
#include <string.h>

#include "lwip/init.h"
#include "lwip/tcp.h"
#include "lwip/timeouts.h"
#include "ssl.h"

extern int write(int fd, const void *buf, unsigned int n);

/* ---- emit helpers ---------------------------------------------- */

static void emit(const char *s) {
    int n = 0;
    while (s[n]) n++;
    write(1, s, n);
}

static void emit_hex8(unsigned int v) {
    static const char hex[] = "0123456789abcdef";
    char buf[11];
    buf[0] = '0'; buf[1] = 'x';
    for (int i = 0; i < 8; i++) {
        buf[2 + i] = hex[(v >> ((7 - i) * 4)) & 0xF];
    }
    buf[10] = '\n';
    write(1, buf, 11);
}

/* ---- network init (port glue) ---------------------------------- */

extern int uc386dos_eth_start(int dhcp_start_now);
extern void uc386dos_eth_set_addr(unsigned int ip, unsigned int mask, unsigned int gw);
extern int  uc386dos_eth_is_up(void);
extern void uc386dos_loopback_poll(void *arg);
extern void lwip_uc386dos_init(void);
extern void lwip_uc386dos_poll(void);

/* ---- BIOS tick clock for short sleeps -------------------------- */
extern unsigned bios_ticks(void);

static void tick_pump(void) {
    /* One callback tick: drives NE2000 RX + sys_check_timeouts via
     * the existing port hook. Keep this tight — no sleep, no delay. */
    uc386dos_loopback_poll(NULL);
}

/* ---- single-socket adapter for axtls --------------------------- */
/* axtls' tls1.c calls SOCKET_READ(sock_id, ...) / SOCKET_WRITE(...).
 * Upstream wires those to mp_stream_posix_read/write; we override
 * those symbols in tls_glue.c so axtls plugs directly into our raw
 * lwIP TCP pcb. The "sock_id" passed to ssl_client_new is the
 * pointer to our smoke_sock_t. */

typedef struct {
    struct tcp_pcb *pcb;
    int             connected;     /* set by connected_cb */
    int             closed;        /* set by err_cb / poll on RST */
    unsigned char   rx[4096];      /* circular-ish ring backed by head/tail */
    unsigned int    rx_head;
    unsigned int    rx_tail;       /* read drains [head..tail) */
} smoke_sock_t;

smoke_sock_t g_sock;

static err_t smoke_recv_cb(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
    (void)arg;
    if (p == NULL) {
        /* peer FIN */
        g_sock.closed = 1;
        return ERR_OK;
    }
    if (err != ERR_OK) {
        pbuf_free(p);
        return err;
    }
    unsigned int free_room = sizeof(g_sock.rx) - (g_sock.rx_tail - g_sock.rx_head);
    if (p->tot_len > free_room) {
        /* No room — refuse, lwIP will resend. */
        return ERR_MEM;
    }
    /* Append to the ring at rx_tail mod size. */
    unsigned int t = g_sock.rx_tail & (sizeof(g_sock.rx) - 1);
    struct pbuf *q = p;
    unsigned int copied = 0;
    while (q) {
        unsigned int n = q->len;
        unsigned int first = sizeof(g_sock.rx) - t;
        if (n <= first) {
            memcpy(g_sock.rx + t, q->payload, n);
            t = (t + n) & (sizeof(g_sock.rx) - 1);
        } else {
            memcpy(g_sock.rx + t, q->payload, first);
            memcpy(g_sock.rx, (char *)q->payload + first, n - first);
            t = n - first;
        }
        copied += n;
        q = q->next;
    }
    g_sock.rx_tail += copied;
    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);
    return ERR_OK;
}

static err_t smoke_connected_cb(void *arg, struct tcp_pcb *pcb, err_t err) {
    (void)arg; (void)pcb;
    if (err == ERR_OK) g_sock.connected = 1;
    return ERR_OK;
}

static void smoke_err_cb(void *arg, err_t err) {
    (void)arg; (void)err;
    g_sock.closed = 1;
    g_sock.pcb = NULL;        /* lwIP has freed the pcb on err */
}

/* Drain bytes from the RX ring. Blocks (with lwIP-pumping) until
 * data arrives, the peer closes, or a 10s budget expires. axtls'
 * SOCKET_READ is wired through this — it expects positive byte
 * count on success, 0 on EOF, NEVER -1. (-1 makes axtls treat the
 * read as SSL_ERROR_CONN_LOST and abort the handshake.) */
int smoke_sock_read(smoke_sock_t *s, unsigned char *buf, unsigned int max) {
    unsigned start = bios_ticks();
    while (1) {
        unsigned int avail = s->rx_tail - s->rx_head;
        if (avail > 0) {
            if (max > avail) max = avail;
            unsigned int h = s->rx_head & (sizeof(s->rx) - 1);
            unsigned int first = sizeof(s->rx) - h;
            if (max <= first) {
                memcpy(buf, s->rx + h, max);
            } else {
                memcpy(buf, s->rx + h, first);
                memcpy(buf + first, s->rx, max - first);
            }
            s->rx_head += max;
            return (int)max;
        }
        if (s->closed) return 0;
        tick_pump();
        if (bios_ticks() - start > 10 * 18) return 0;   /* 10 s budget */
    }
}

/* unreachable but the dead-code path was the original implementation;
 * left for reference if we ever re-introduce non-blocking semantics. */
static int smoke_sock_read_nb(smoke_sock_t *s, unsigned char *buf, unsigned int max) {
    unsigned int avail = s->rx_tail - s->rx_head;
    if (avail == 0) {
        return s->closed ? 0 : -1;
    }
    if (max > avail) max = avail;
    unsigned int h = s->rx_head & (sizeof(s->rx) - 1);
    unsigned int first = sizeof(s->rx) - h;
    if (max <= first) {
        memcpy(buf, s->rx + h, max);
    } else {
        memcpy(buf, s->rx + h, first);
        memcpy(buf + first, s->rx, max - first);
    }
    s->rx_head += max;
    return (int)max;
}

int smoke_sock_write(smoke_sock_t *s, const unsigned char *buf, unsigned int len) {
    if (!s->pcb || s->closed) return -1;
    err_t e = tcp_write(s->pcb, buf, len, TCP_WRITE_FLAG_COPY);
    if (e != ERR_OK) return -1;
    tcp_output(s->pcb);
    return (int)len;
}

/* ---- main -------------------------------------------------- */

/* Pre-allocated by main.c via DPMI 0x0100 — same shallow-stack
 * workaround MicroPython needed. */
extern unsigned char pktdrv_int_invoke(unsigned int, unsigned int *);
unsigned int pktdrv_preallocated_bounce_seg = 0;
unsigned int pktdrv_preallocated_bounce_linear = 0;
unsigned int pktdrv_preallocated_thunk_seg = 0;
unsigned int dos_int21_preallocated_thunk_seg = 0;

static void _preallocate_dpmi_resources(void) {
    emit("[prealloc:enter]\n");

    /* Bounce buffer: 128 paragraphs (2 KB) via INT 21h AH=0x48. */
    unsigned int regs[8] = {0};
    regs[0] = 0x4800;
    regs[1] = 128;
    if (pktdrv_int_invoke(0x21, regs)) {
        emit("[prealloc:bounce-FAIL]\n");
        return;
    }
    unsigned int seg = regs[0] & 0xFFFF;
    pktdrv_preallocated_bounce_seg = seg;

    /* Resolve linear: DPMI 0x0002 (sel for seg) + 0x0006 (sel base). */
    unsigned int r2[8] = {0};
    r2[0] = 0x0002;
    r2[1] = seg;
    pktdrv_int_invoke(0x31, r2);
    unsigned int sel = r2[0] & 0xFFFF;
    unsigned int r3[8] = {0};
    r3[0] = 0x0006;
    r3[1] = sel;
    pktdrv_int_invoke(0x31, r3);
    unsigned int bh = r3[2] & 0xFFFF;
    unsigned int bl = r3[3] & 0xFFFF;
    pktdrv_preallocated_bounce_linear = (bh << 16) | bl;

    /* Two thunk paragraphs via DPMI 0x0100 (registered with the
     * host so DPMI 0x0301 can dispatch to them). */
    unsigned int rt[8] = {0};
    rt[0] = 0x0100; rt[1] = 1;
    if (!pktdrv_int_invoke(0x31, rt)) {
        pktdrv_preallocated_thunk_seg = rt[0] & 0xFFFF;
    }
    unsigned int rt2[8] = {0};
    rt2[0] = 0x0100; rt2[1] = 1;
    if (!pktdrv_int_invoke(0x31, rt2)) {
        dos_int21_preallocated_thunk_seg = rt2[0] & 0xFFFF;
    }
    emit("[prealloc:done]\n");
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    emit("[smoke:start]\n");

    _preallocate_dpmi_resources();
    lwip_uc386dos_init();
    emit("[smoke:lwip-init-done]\n");

    /* Stage 0: eth init (no DHCP — static IP). */
    emit("[s0:eth-init-pre]\n");
    int rc = uc386dos_eth_start(0);
    emit("[s0:eth-init-rc=]");
    emit_hex8((unsigned int)rc);
    if (rc != 0) { emit("[s0:FAIL]\n"); return 1; }

    /* Stage 1: static IP. */
    uc386dos_eth_set_addr(
        0x0F02000A,    /* 10.0.2.15 (network byte order: low→high) */
        0x00FFFFFF,    /* 255.255.255.0 */
        0x0202000A     /* 10.0.2.2 */
    );
    emit("[s1:static-ip-set]\n");

    /* Pump a few ticks so the netif comes up. */
    for (int i = 0; i < 10; i++) tick_pump();
    emit("[s1:up=]");
    emit_hex8((unsigned int)uc386dos_eth_is_up());

    /* Diagnostic: hand-craft an ARP request and send it via the
     * NE2000 driver directly, bypassing lwIP. If the wire shows
     * proper bytes, the chip path is fine and lwIP's TX is the
     * issue. If the wire still shows zeros, the chip/driver
     * binding is broken. */
    extern int pktdrv_send(const unsigned char *buf, unsigned int len);
    extern int pktdrv_is_active(void);
    static unsigned char arp[42];
    /* dst MAC ff:ff:ff:ff:ff:ff. NB: don't use memset — uc386's libc
     * `rep stosb` uses ES:EDI and ES isn't always == DS in this
     * runtime, so memset silently writes nowhere. Indexed writes
     * (which use DS:offset) work fine. */
    arp[0] = arp[1] = arp[2] = arp[3] = arp[4] = arp[5] = 0xFF;
    /* src MAC = QEMU NE2000 default 52:54:00:12:34:56 (matches what
     * the chip's PROM reads in our pktdrv_init logs above). */
    arp[6]=0x52; arp[7]=0x54; arp[8]=0x00;
    arp[9]=0x12; arp[10]=0x34; arp[11]=0x56;
    arp[12]=0x08; arp[13]=0x06;                  /* ethertype = ARP */
    arp[14]=0x00; arp[15]=0x01;                  /* HTYPE = ethernet */
    arp[16]=0x08; arp[17]=0x00;                  /* PTYPE = IPv4 */
    arp[18]=6;    arp[19]=4;                     /* HLEN PLEN */
    arp[20]=0x00; arp[21]=0x01;                  /* OPER = request */
    arp[22]=0x52; arp[23]=0x54; arp[24]=0x00;    /* sender HA */
    arp[25]=0x12; arp[26]=0x34; arp[27]=0x56;
    arp[28]=10; arp[29]=0; arp[30]=2; arp[31]=15; /* sender IP 10.0.2.15 */
    memset(arp + 32, 0, 6);                      /* target HA */
    arp[38]=10; arp[39]=0; arp[40]=2; arp[41]=2;  /* target IP 10.0.2.2 */
    /* Dump arp[0..15] before sending so we can compare against the
     * pcap. If these bytes differ from what the wire shows, the
     * chip TX or DMA-to-NIC path is dropping/offsetting. */
    {
        emit("[diag:arp-mem0=]");
        unsigned int w = ((unsigned int)arp[0] << 24)
                       | ((unsigned int)arp[1] << 16)
                       | ((unsigned int)arp[2] << 8)
                       |  (unsigned int)arp[3];
        emit_hex8(w);
        emit("[diag:arp-mem4=]");
        w = ((unsigned int)arp[4] << 24) | ((unsigned int)arp[5] << 16)
          | ((unsigned int)arp[6] << 8)  |  (unsigned int)arp[7];
        emit_hex8(w);
        emit("[diag:arp-mem8=]");
        w = ((unsigned int)arp[8] << 24) | ((unsigned int)arp[9] << 16)
          | ((unsigned int)arp[10] << 8) |  (unsigned int)arp[11];
        emit_hex8(w);
        emit("[diag:arp-mem12=]");
        w = ((unsigned int)arp[12] << 24) | ((unsigned int)arp[13] << 16)
          | ((unsigned int)arp[14] << 8)  |  (unsigned int)arp[15];
        emit_hex8(w);
    }

    emit("[diag:arp-send-pre]\n");
    int sr = pktdrv_send(arp, 42);
    emit("[diag:arp-send-rc=]");
    emit_hex8((unsigned int)sr);

    /* Stage 2: open a tcp_pcb, connect to 10.0.2.2:8443. */
    emit("[s2:tcp-new]\n");
    g_sock.pcb = tcp_new();
    if (!g_sock.pcb) { emit("[s2:tcp_new-FAIL]\n"); return 2; }
    tcp_arg(g_sock.pcb, &g_sock);
    tcp_recv(g_sock.pcb, smoke_recv_cb);
    tcp_err(g_sock.pcb, smoke_err_cb);

    ip4_addr_t dst;
    IP4_ADDR(&dst, 10, 0, 2, 2);
    err_t ce = tcp_connect(g_sock.pcb, &dst, 8443, smoke_connected_cb);
    emit("[s2:tcp_connect-rc=]");
    emit_hex8((unsigned int)ce);
    if (ce != ERR_OK) { emit("[s2:FAIL]\n"); return 3; }

    /* Stage 3: pump until connected (or 30 s timeout via tick count).
     * lwIP needs time for ARP resolution (multi-second on first contact)
     * + SYN retransmits if the first one is dropped. Also emit a
     * heartbeat every ~1 s so we see polling actually advancing. */
    unsigned start = bios_ticks();
    unsigned last_hb = start;
    while (!g_sock.connected && !g_sock.closed) {
        tick_pump();
        unsigned now = bios_ticks();
        if (now - last_hb > 18) {
            emit("[s3:tick]\n");
            last_hb = now;
        }
        if (now - start > 30 * 18) {     /* ~30 s */
            emit("[s3:connect-TIMEOUT]\n");
            return 4;
        }
    }
    if (g_sock.closed) { emit("[s3:closed-before-connect]\n"); return 5; }
    emit("[s3:tcp-connected]\n");

    /* Stage 4: axtls — create context (no cert verification), wrap
     * the socket pointer (which our SOCKET_READ/WRITE redirect via
     * smoke_sock_read/write). */
    emit("[s4:ssl-ctx-new-pre]\n");
    SSL_CTX *ctx = ssl_ctx_new(SSL_SERVER_VERIFY_LATER, SSL_DEFAULT_CLNT_SESS);
    if (!ctx) { emit("[s4:ctx-new-FAIL]\n"); return 6; }
    emit("[s4:ssl-client-new-pre]\n");
    SSL *ssl = ssl_client_new(ctx, (long)&g_sock, NULL, 0, NULL);
    if (!ssl) { emit("[s4:client-new-FAIL]\n"); ssl_ctx_free(ctx); return 7; }
    emit("[s4:ssl-created]\n");

    /* Stage 5: drive handshake — ssl_handshake_status returns 0
     * when done, < 0 on error, > 0 means in progress. axtls's
     * client-side handshake completes inside ssl_read when called
     * with NULL data buffer (it reads next record + advances state). */
    start = bios_ticks();
    while (1) {
        tick_pump();
        int hs = ssl_handshake_status(ssl);
        if (hs == SSL_OK) break;
        if (hs < 0 && hs != SSL_NOT_OK) {
            emit("[s5:handshake-err=]");
            emit_hex8((unsigned int)hs);
            return 8;
        }
        /* Read available bytes — this drives the state machine. */
        uint8_t *p = NULL;
        int r = ssl_read(ssl, &p);
        if (r < 0 && r != SSL_OK && r != SSL_NOT_OK) {
            emit("[s5:read-err=]");
            emit_hex8((unsigned int)r);
            return 9;
        }
        if (bios_ticks() - start > 30 * 18) {     /* 30 s budget */
            emit("[s5:handshake-TIMEOUT]\n");
            return 10;
        }
    }
    emit("[s5:handshake-OK]\n");

    /* Stage 6: write hello, read response. */
    static const char hello[] = "hi-from-uc386";
    int w = ssl_write(ssl, (const uint8_t *)hello, sizeof(hello) - 1);
    emit("[s6:ssl-write-rc=]");
    emit_hex8((unsigned int)w);
    if (w <= 0) { emit("[s6:write-FAIL]\n"); return 11; }

    start = bios_ticks();
    uint8_t *resp = NULL;
    int n;
    while (1) {
        tick_pump();
        n = ssl_read(ssl, &resp);
        if (n > 0) break;
        if (n < 0 && n != SSL_OK && n != SSL_NOT_OK) {
            emit("[s6:read-err=]");
            emit_hex8((unsigned int)n);
            return 12;
        }
        if (bios_ticks() - start > 10 * 18) {
            emit("[s6:read-TIMEOUT]\n");
            return 13;
        }
    }
    emit("[s6:bytes=");
    if (resp && n > 0) write(1, resp, (unsigned)n);
    emit("]\n");

    ssl_free(ssl);
    ssl_ctx_free(ctx);
    emit("[smoke:PASS]\n");
    return 0;
}
