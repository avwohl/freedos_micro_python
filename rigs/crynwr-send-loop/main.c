/* crynwr_send_loop — minimal repro for the "second pktdrv_send
 * crashes" symptom seen in tls-smoke under FORCE_CRYNWR=1.
 *
 * The tls-smoke is too noisy a vehicle: lwIP runs DHCP / ARP
 * triggers, INT 21h writes punctuate the path, and TLS init follows.
 * This rig strips all of that. We call pktdrv_init once and then
 * pktdrv_send N times in a tight loop with a canned 42-byte ARP
 * request. Between each send we print [send N OK] or [send N FAIL=…].
 *
 * If the crash reproduces here, the bug is in the pktdrv_send /
 * DPMI 0x0301 dispatch itself (state reuse in the static RMCS, the
 * thunk segment, or DOS/32A's CallRealMode internals). If only the
 * first send works and the second always crashes regardless of any
 * intervening INT 21h activity, the bug is *deterministic across
 * adjacent calls* — and we can attack it with focused diagnostics.
 *
 * Stages emitted to COM1:
 *   [boot]                            we entered main
 *   [prealloc:enter] / [prealloc:done]  pre-alloc DOS memory
 *                                       (bounce + thunk segments)
 *   [pi:enter] ... [pi:done]          pktdrv_init progress
 *   [send N pre]                      about to send packet N
 *   [send N rc=]<rc>                  pktdrv_send return code
 *   [send N OK] or [send N FAIL=]<rc> tagged outcome
 *   [bench:done]                      loop exit
 */

#include <stdint.h>
#include <string.h>

extern int  write(int fd, const void *buf, unsigned int n);
extern int  pktdrv_init(unsigned char mac[6]);
extern int  pktdrv_send(const unsigned char *buf, unsigned int len);
extern int  pktdrv_is_active(void);
extern volatile unsigned int pktdrv_thunk_invocations;
extern volatile unsigned int pktdrv_thunk_phase0_count;
extern volatile unsigned int pktdrv_thunk_phase1_count;

/* 8259 PIC IO helpers (from uc386 libc — same path the PM-native
 * NE2000 driver uses for direct IO). */
extern void         ne2k_outb(unsigned int port, unsigned int val);
extern unsigned int ne2k_inb(unsigned int port);

/* Pre-allocation symbols read by pktdrv_alloc_bounce / _thunk in
 * pktdrv_uc386dos.c (same shape as tls-smoke main.c). PMODE/W's
 * DOS-alloc hangs from deep stack, so we pre-alloc from main()
 * while shallow and stash the seg/linear values in these globals.
 * Under DOS/32A the deep-stack hang doesn't apply, but the symbols
 * are still required by the port (init checks them first). */
unsigned int pktdrv_preallocated_bounce_seg    = 0;
unsigned int pktdrv_preallocated_bounce_linear = 0;
unsigned int pktdrv_preallocated_thunk_seg     = 0;

static void emit(const char *s) {
    int n = 0;
    while (s[n]) n++;
    write(1, s, n);
}

static void emit_hex8(unsigned int v) {
    static const char hex[] = "0123456789abcdef";
    char buf[12];
    buf[0] = '0'; buf[1] = 'x';
    for (int i = 0; i < 8; i++) {
        buf[2 + i] = hex[(v >> ((7 - i) * 4)) & 0xF];
    }
    buf[10] = '\r';
    buf[11] = '\n';
    write(1, buf, 12);
}

static void emit_dec(unsigned int v) {
    char buf[12]; int n = 0;
    if (v == 0) buf[n++] = '0';
    else {
        char t[12]; int ti = 0;
        while (v > 0) { t[ti++] = (char)('0' + (v % 10u)); v /= 10u; }
        while (ti > 0) buf[n++] = t[--ti];
    }
    write(1, buf, n);
}

extern unsigned char pktdrv_int_invoke(unsigned int int_num,
                                       unsigned int regs[8]);

#define R_EAX 0
#define R_EBX 1
#define R_ECX 2
#define R_EDX 3

/* Match rigs/tls-smoke/main.c's prealloc style exactly: bounce
 * buffer via INT 21h AH=0x48 (DOS allocate-memory) + DPMI 0x0002 /
 * 0x0006 to resolve the PM linear address, thunk segment via DPMI
 * 0x0100. The smoke's pktdrv_send works once before crashing, so
 * matching its allocation flow exactly isolates whether the bug
 * lives in the allocator path or in the send-loop itself. */
static int dos_alloc_int21_AH48(unsigned int paras, unsigned int *out_seg) {
    unsigned int regs[8] = {0};
    regs[R_EAX] = 0x4800;
    regs[R_EBX] = paras;
    if (pktdrv_int_invoke(0x21, regs)) return -1;
    *out_seg = regs[R_EAX] & 0xFFFF;
    return 0;
}

static int resolve_seg_to_linear(unsigned int seg, unsigned int *out_linear) {
    /* DPMI 0x0002 — get PM selector for the real-mode segment. */
    unsigned int r2[8] = {0};
    r2[R_EAX] = 0x0002;
    r2[R_EBX] = seg;
    if (pktdrv_int_invoke(0x31, r2)) return -1;
    unsigned int sel = r2[R_EAX] & 0xFFFF;
    /* DPMI 0x0006 — get the selector's base linear address. CX:DX = base. */
    unsigned int r3[8] = {0};
    r3[R_EAX] = 0x0006;
    r3[R_EBX] = sel;
    if (pktdrv_int_invoke(0x31, r3)) return -1;
    unsigned int bh = r3[R_ECX] & 0xFFFF;
    unsigned int bl = r3[R_EDX] & 0xFFFF;
    *out_linear = (bh << 16) | bl;
    return 0;
}

static int dpmi_alloc_dos_mem(unsigned int paras, unsigned int *out_seg) {
    unsigned int regs[8] = {0};
    regs[R_EAX] = 0x0100;
    regs[R_EBX] = paras;
    if (pktdrv_int_invoke(0x31, regs)) return -1;
    *out_seg = regs[R_EAX] & 0xFFFF;
    return 0;
}

int main(void) {
    emit("[boot]\r\n");

    emit("[prealloc:enter]\r\n");
    /* Dummy pad: the smoke's TLS.EXE is ~200 KB, which bumps the
     * DOS heap high-water-mark past Crynwr's NE2000.COM resident
     * region (typically loaded somewhere in 0x0800–0x1000 range).
     * This minimal bench's .EXE is ~45 KB, so without a pad the
     * bounce buffer lands at very low segments (~0x0800) and may
     * overlap Crynwr — at which point send_pkt reads the driver's
     * own resident data as the frame, with predictable misery.
     * Burn a chunk first to push the bounce allocator past 0x1100.
     */
    {
        unsigned int dummy_seg;
        /* 0x100 paragraphs = 4 KB. Enough to clear Crynwr v11 NE2000. */
        if (dos_alloc_int21_AH48(0x100, &dummy_seg) != 0) {
            emit("[prealloc:dummy-pad-FAIL]\r\n");
            return 1;
        }
        emit("[prealloc:dummy-pad-seg=]");
        emit_hex8(dummy_seg);
    }
    /* Bounce buffer: 128 paragraphs (2 KB) via INT 21h AH=0x48 +
     * DPMI 0x0002/0x0006 — same path the smoke uses. */
    if (dos_alloc_int21_AH48(128, &pktdrv_preallocated_bounce_seg) != 0) {
        emit("[prealloc:bounce-FAIL]\r\n");
        return 1;
    }
    if (resolve_seg_to_linear(pktdrv_preallocated_bounce_seg,
                              &pktdrv_preallocated_bounce_linear) != 0) {
        emit("[prealloc:bounce-linear-FAIL]\r\n");
        return 1;
    }
    emit("[prealloc:bounce-seg=]");
    emit_hex8(pktdrv_preallocated_bounce_seg);
    emit("[prealloc:bounce-linear=]");
    emit_hex8(pktdrv_preallocated_bounce_linear);

    /* Thunk segment: 1 paragraph via DPMI 0x0100 — registered with
     * the host so DPMI 0x0301 can dispatch to it. */
    if (dpmi_alloc_dos_mem(1, &pktdrv_preallocated_thunk_seg) != 0) {
        emit("[prealloc:thunk-FAIL]\r\n");
        return 2;
    }
    emit("[prealloc:thunk-seg=]");
    emit_hex8(pktdrv_preallocated_thunk_seg);
    emit("[prealloc:done]\r\n");

    /* pktdrv_init does the Crynwr detect + access_type + DPMI 0x0303
     * registration. Force the Crynwr path via the same -DPKTDRV_FORCE_
     * CRYNWR=1 build flag tls-smoke uses. */
    unsigned char mac[6];
    int rc = pktdrv_init(mac);
    emit("\r\n[pi:rc=]");
    emit_hex8((unsigned int)rc);
    if (rc != 0) {
        emit("[pi:FAIL]\r\n");
        return 3;
    }
    if (!pktdrv_is_active()) {
        emit("[pi:not-active]\r\n");
        return 4;
    }

    /* Canned 42-byte ARP request for 10.0.2.2 from 10.0.2.15.
     * Same shape as tls-smoke's diagnostic ARP — minimal valid
     * ethernet frame with a real ARP payload. */
    static unsigned char arp[42];
    /* dst MAC: broadcast */
    arp[0]=0xff; arp[1]=0xff; arp[2]=0xff; arp[3]=0xff; arp[4]=0xff; arp[5]=0xff;
    /* src MAC: our MAC, filled below from get_address */
    memcpy(arp + 6, mac, 6);
    arp[12]=0x08; arp[13]=0x06;                  /* ethertype = ARP */
    arp[14]=0x00; arp[15]=0x01;                  /* HTYPE = ethernet */
    arp[16]=0x08; arp[17]=0x00;                  /* PTYPE = IPv4 */
    arp[18]=6;    arp[19]=4;
    arp[20]=0x00; arp[21]=0x01;                  /* OPER = request */
    memcpy(arp + 22, mac, 6);                    /* sender HA */
    arp[28]=10; arp[29]=0; arp[30]=2; arp[31]=15;
    memset(arp + 32, 0, 6);
    arp[38]=10; arp[39]=0; arp[40]=2; arp[41]=2;

    /* Send loop. Five iterations should be plenty to repro the
     * "second send crashes" symptom; if we get past N=2 cleanly the
     * tls-smoke crash was triggered by something the smoke does
     * specifically (lwIP timer pump, axtls init, interim INT 21h
     * activity) rather than by adjacent pktdrv_send calls. */
    /* IRQ masking now happens inside pktdrv_init at the end of the
     * Crynwr setup phase — see the comment there for the bug and
     * the cost. This bench can call pktdrv_send as-is. */

    for (int i = 0; i < 5; i++) {
        /* RX-callback diagnostics: if pktdrv_thunk_invocations > 0
         * here, the DPMI 0x0303 PM callback has fired since the
         * last iteration — useful for the "RX IRQ corrupts state
         * during send" hypothesis. */
        emit("[send ");
        emit_dec((unsigned int)i);
        emit(" thunk-inv=]");
        emit_hex8(pktdrv_thunk_invocations);
        emit("[send ");
        emit_dec((unsigned int)i);
        emit(" pre]\r\n");

        int sr = pktdrv_send(arp, 42);

        emit("[send ");
        emit_dec((unsigned int)i);
        emit(" rc=]");
        emit_hex8((unsigned int)sr);
        if (sr != 0) {
            emit("[send ");
            emit_dec((unsigned int)i);
            emit(" FAIL]\r\n");
        } else {
            emit("[send ");
            emit_dec((unsigned int)i);
            emit(" OK]\r\n");
        }
    }

    emit("[bench:done]\r\n");
    return 0;
}
