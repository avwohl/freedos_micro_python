// Crynwr "FTP Software" packet-driver bindings (INT 0x60–0x7F).
//
// Detection: walk the IVT looking for the 8-byte signature
// "PKT DRVR" at offset +3 of the handler routine (the leading two
// bytes are a `JMP SHORT +8` over the signature). The IVT lives at
// linear 0x0000:0xNNNN — under PMODE/W's flat 32-bit model the
// real-mode IVT is reachable via the same flat address space, just
// at the bottom of memory.
//
// Calls used:
//   AH=0x01 driver_info     (probe — confirms a handle is valid)
//   AH=0x02 access_type     (register a receiver callback)
//   AH=0x04 send_pkt        (transmit a frame)
//   AH=0x06 get_address     (read the NIC's MAC)
//   AH=0x05 release_type    (unregister at shutdown — not yet wired)
//
// Receive is callback-driven: on packet arrival, the driver calls
// our handler twice — first with AX=0 to get a buffer pointer, then
// with AX=1 once it's copied the bytes in. We funnel both into a
// small RX queue (`pktdrv_rx_queue`) that ethdrv_recv polls.
//
// Real-DOS deployment notes (TODO): the AX=0/AX=1 callback runs in
// real mode; PMODE/W requires us to allocate a real-mode trampoline
// via DPMI INT 31h fn 0x0303 that ferries the call into 32-bit
// protected mode and translates the seg:offset return into a flat
// linear pointer. Under dos_emu the receiver is invoked directly as
// a 32-bit cdecl function — the emulator does the impedance match.

#include <stddef.h>
#include <string.h>

extern unsigned char pktdrv_int_invoke(unsigned int int_num,
                                       unsigned int regs_in_out[8]);

// regs_in_out indices, mirrored in the asm wrapper.
#define R_EAX 0
#define R_EBX 1
#define R_ECX 2
#define R_EDX 3
#define R_ESI 4
#define R_EDI 5
#define R_DS  6
#define R_ES  7

static const unsigned char PKT_SIG[8] = "PKT DRVR";

static int pktdrv_int_num   = 0;   // 0 = not detected
static int pktdrv_handle    = -1;  // access_type result
static unsigned char pktdrv_mac_cache[6];

// RX ring: the receiver callback is split into TWO calls per packet
// (give-buffer / packet-copied). We hand back `pktdrv_rx_buf` from
// give-buffer, then mark `pktdrv_rx_pending = 1` once the copy is
// done. ethdrv_recv (in lwip_uc386dos.c) polls the pending flag,
// drains the buffer, clears the flag — single-slot ring is enough
// because lwip.callback() pumps after every TX and dos_emu is
// single-threaded so packets land one at a time.
#define PKTDRV_MAX_FRAME 1518
unsigned char pktdrv_rx_buf[PKTDRV_MAX_FRAME];
volatile int          pktdrv_rx_pending  = 0;
volatile unsigned int pktdrv_rx_len      = 0;

// DPMI 0.9 "Real Mode Call Structure" — what fn 0x0303 saves the
// real-mode register state into when the trampoline fires. Field
// order is fixed by the DPMI spec; total size is 0x32 (50 bytes).
// __attribute__((packed)) keeps the 16-bit segment fields adjacent
// to their dword neighbors; uc386 honors packed (verified via the
// lwIP packed_struct_test self-check).
typedef struct {
    unsigned int   edi;        // 0x00
    unsigned int   esi;        // 0x04
    unsigned int   ebp;        // 0x08
    unsigned int   reserved;   // 0x0C
    unsigned int   ebx;        // 0x10
    unsigned int   edx;        // 0x14
    unsigned int   ecx;        // 0x18
    unsigned int   eax;        // 0x1C
    unsigned short flags;      // 0x20
    unsigned short es;         // 0x22
    unsigned short ds;         // 0x24
    unsigned short fs;         // 0x26
    unsigned short gs;         // 0x28
    unsigned short ip;         // 0x2A
    unsigned short cs;         // 0x2C
    unsigned short sp;         // 0x2E
    unsigned short ss;         // 0x30
} __attribute__((packed)) pktdrv_rmcs_t;

pktdrv_rmcs_t pktdrv_rmcs;
unsigned int  pktdrv_dpmi_seg = 0;   // real-mode trampoline segment
unsigned int  pktdrv_dpmi_off = 0;   // real-mode trampoline offset

// Diagnostic counters for the static-IP rig — exposed to Python so
// the test script can poll without rebuilding to add markers.
volatile unsigned int pktdrv_thunk_invocations = 0;
volatile unsigned int pktdrv_thunk_phase0_count = 0;
volatile unsigned int pktdrv_thunk_phase1_count = 0;
unsigned int pktdrv_last_handle = 0xDEADBEEF;

// ============================================================
// PM-native NE2000 driver. Bypasses Crynwr / DPMI 0x0303 entirely.
// IO base is hardcoded to 0x300 (QEMU's ne2k_isa default).
// We talk directly to the NIC registers via PM IO instructions
// (PMODE/W grants PM clients full IO permission by default).
// ============================================================
extern void ne2k_outb(unsigned int port, unsigned int val);
extern unsigned int ne2k_inb(unsigned int port);
extern void ne2k_outsw(unsigned int port, const void *src, unsigned int wcount);
extern void ne2k_insw(unsigned int port, void *dst, unsigned int wcount);

#define NE2K_BASE   0x300
#define NE2K_DATA   (NE2K_BASE + 0x10)
#define NE2K_RESET  (NE2K_BASE + 0x1F)

// NE2000 register offsets (page 0)
#define NE_CR    0x00   // Command
#define NE_PSTART 0x01
#define NE_PSTOP  0x02
#define NE_BNRY   0x03
#define NE_TPSR   0x04
#define NE_TBCR0  0x05
#define NE_TBCR1  0x06
#define NE_ISR    0x07
#define NE_RSAR0  0x08
#define NE_RSAR1  0x09
#define NE_RBCR0  0x0A
#define NE_RBCR1  0x0B
#define NE_RCR    0x0C
#define NE_TCR    0x0D
#define NE_DCR    0x0E
#define NE_IMR    0x0F
// Page 1
#define NE_PAR0   0x01
#define NE_CURR   0x07

// RX ring layout in NIC SRAM: TX page 0x40 (single TX buffer),
// RX ring 0x46..0x80 (58 pages = 14848 bytes).
#define TX_PAGE_START   0x40
#define RX_PAGE_START   0x46
#define RX_PAGE_STOP    0x80

static unsigned char ne2k_mac[6];
static int ne2k_initialized = 0;

static void ne2k_dma_read(unsigned int nic_addr, void *dst, unsigned int byte_count) {
    // Set up remote DMA read
    ne2k_outb(NE2K_BASE + NE_ISR, 0x40);   // Pre-clear RDC
    ne2k_outb(NE2K_BASE + NE_RBCR0, byte_count & 0xFF);
    ne2k_outb(NE2K_BASE + NE_RBCR1, (byte_count >> 8) & 0xFF);
    ne2k_outb(NE2K_BASE + NE_RSAR0, nic_addr & 0xFF);
    ne2k_outb(NE2K_BASE + NE_RSAR1, (nic_addr >> 8) & 0xFF);
    ne2k_outb(NE2K_BASE + NE_CR, 0x0A);    // Start, RD0=1 (remote DMA read)
    ne2k_insw(NE2K_DATA, dst, (byte_count + 1) >> 1);
    // Bounded wait for RDC (QEMU sets it immediately after insw, but
    // a real NIC may take a few microseconds; cap to ~10ms-equivalent
    // so a stuck NIC can't wedge the whole program).
    unsigned int spin = 100000;
    while (spin-- && !(ne2k_inb(NE2K_BASE + NE_ISR) & 0x40)) {}
    ne2k_outb(NE2K_BASE + NE_ISR, 0x40);
}

static void ne2k_dma_write(unsigned int nic_addr, const void *src, unsigned int byte_count) {
    ne2k_outb(NE2K_BASE + NE_ISR, 0x40);
    ne2k_outb(NE2K_BASE + NE_RBCR0, byte_count & 0xFF);
    ne2k_outb(NE2K_BASE + NE_RBCR1, (byte_count >> 8) & 0xFF);
    ne2k_outb(NE2K_BASE + NE_RSAR0, nic_addr & 0xFF);
    ne2k_outb(NE2K_BASE + NE_RSAR1, (nic_addr >> 8) & 0xFF);
    ne2k_outb(NE2K_BASE + NE_CR, 0x12);    // Start, RD1=1 (remote DMA write)
    ne2k_outsw(NE2K_DATA, src, (byte_count + 1) >> 1);
    unsigned int spin = 100000;
    while (spin-- && !(ne2k_inb(NE2K_BASE + NE_ISR) & 0x40)) {}
    ne2k_outb(NE2K_BASE + NE_ISR, 0x40);
}

// Initialise the NIC. Returns 0 on success; fills mac with the read MAC.
static int ne2k_init_direct(unsigned char mac[6]) {
    extern int write(int fd, const void *buf, unsigned int n);
    write(1, "[ne2k:init]", 11);
    // Reset
    unsigned int reset_val = ne2k_inb(NE2K_RESET);
    ne2k_outb(NE2K_RESET, reset_val);
    // Wait for RST bit
    unsigned int spin = 100000;
    while (spin-- && !(ne2k_inb(NE2K_BASE + NE_ISR) & 0x80)) {}
    if (spin == 0) {
        write(1, "[ne2k:reset-timeout]", 20);
        return -1;
    }
    write(1, "[ne2k:reset-ok]", 15);

    // Stop NIC, page 0
    ne2k_outb(NE2K_BASE + NE_CR, 0x21);    // STP=1, RD2=1, PS=0
    ne2k_outb(NE2K_BASE + NE_DCR, 0x49);   // word transfers, FIFO 8, normal
    ne2k_outb(NE2K_BASE + NE_RBCR0, 0);
    ne2k_outb(NE2K_BASE + NE_RBCR1, 0);
    ne2k_outb(NE2K_BASE + NE_RCR, 0x20);   // Monitor mode (init)
    ne2k_outb(NE2K_BASE + NE_TCR, 0x02);   // Loopback (init)
    ne2k_outb(NE2K_BASE + NE_TPSR, TX_PAGE_START);
    ne2k_outb(NE2K_BASE + NE_PSTART, RX_PAGE_START);
    ne2k_outb(NE2K_BASE + NE_PSTOP, RX_PAGE_STOP);
    ne2k_outb(NE2K_BASE + NE_BNRY, RX_PAGE_START);
    ne2k_outb(NE2K_BASE + NE_ISR, 0xFF);   // Clear all interrupts
    ne2k_outb(NE2K_BASE + NE_IMR, 0x00);   // No interrupts (we poll)

    // Read MAC from PROM (32 bytes — PROM is 16 bytes, each byte
    // appears twice in word reads). Word transfers give us 32 bytes.
    unsigned char prom[32];
    ne2k_dma_read(0x0000, prom, 32);
    // Each word's low byte == high byte == one MAC byte. Take low.
    for (int i = 0; i < 6; i++) {
        mac[i] = prom[i * 2];
        ne2k_mac[i] = mac[i];
    }
    write(1, "[ne2k:mac-read]", 15);

    // Switch to page 1
    ne2k_outb(NE2K_BASE + NE_CR, 0x61);    // STP=1, RD2=1, PS=1
    // Set our MAC into PAR0..5
    for (int i = 0; i < 6; i++) {
        ne2k_outb(NE2K_BASE + NE_PAR0 + i, mac[i]);
    }
    // CURR = page after PSTART (so empty ring has CURR == BNRY+1)
    ne2k_outb(NE2K_BASE + NE_CURR, RX_PAGE_START + 1);
    // MAR0..7 = 0 (no multicast)
    for (int i = 0; i < 8; i++) {
        ne2k_outb(NE2K_BASE + 0x08 + i, 0);
    }

    // Back to page 0, start
    ne2k_outb(NE2K_BASE + NE_CR, 0x22);    // STA=1, RD2=1, PS=0
    ne2k_outb(NE2K_BASE + NE_TCR, 0x00);   // Normal TX
    ne2k_outb(NE2K_BASE + NE_RCR, 0x04);   // Accept broadcast (and own-MAC unicast implicit)

    ne2k_initialized = 1;

    // Belt-and-suspenders: mask IRQ 9 at the slave 8259 PIC. The
    // chip's NE_IMR is already 0 (above) so it shouldn't assert IRQ,
    // but a stuck-high line during reset could still latch into the
    // slave PIC and fire whatever stale ISR is registered for IRQ 9 —
    // and since we don't install one, the BIOS default is unpredictable
    // (suspected source of f.read() hangs after eth_init: INT 21h
    // AH=0x3F starts a sector read, the floppy's IRQ 6 handling races
    // with a phantom IRQ 9, and the read never completes). IRQ 9 is
    // bit 1 of the slave PIC IMR at port 0xA1.
    write(1, "[ne2k:pic-mask]", 15);
    unsigned int slave_imr = ne2k_inb(0xA1);
    ne2k_outb(0xA1, slave_imr | 0x02);

    write(1, "[ne2k:ready]", 12);
    return 0;
}

// Send a packet. Returns 0 on success.
static int ne2k_send_direct(const unsigned char *buf, unsigned int len) {
    if (!ne2k_initialized) return -1;
    if (len < 60) len = 60;       // Pad to minimum ethernet frame
    if (len > 1518) return -1;
    // Wait for any in-progress TX to finish
    while (ne2k_inb(NE2K_BASE + NE_CR) & 0x04) {}
    // DMA the packet into TX page
    ne2k_dma_write((unsigned int)TX_PAGE_START << 8, buf, len);
    // Set TX byte count + start TX
    ne2k_outb(NE2K_BASE + NE_TBCR0, len & 0xFF);
    ne2k_outb(NE2K_BASE + NE_TBCR1, (len >> 8) & 0xFF);
    ne2k_outb(NE2K_BASE + NE_CR, 0x26);   // Start, TXP=1, RD2=0(?), STA=1
    // Wait for PTX or TXE
    unsigned int spin = 1000000;
    while (spin--) {
        unsigned int isr = ne2k_inb(NE2K_BASE + NE_ISR);
        if (isr & 0x0A) {       // PTX(1) or TXE(8)
            ne2k_outb(NE2K_BASE + NE_ISR, isr & 0x0A);  // ACK
            return (isr & 0x02) ? 0 : -1;  // PTX=1 means success
        }
    }
    return -1;
}

// Poll for received packets. Returns the bytes copied to out, or 0 if
// nothing pending. Drains one packet at a time. maxlen caps the copy.
static unsigned int ne2k_poll_rx(unsigned char *out, unsigned int maxlen) {
    if (!ne2k_initialized) return 0;
    // Read CURR via page 1
    unsigned int saved_cr = ne2k_inb(NE2K_BASE + NE_CR);
    ne2k_outb(NE2K_BASE + NE_CR, 0x62);    // STA, RD2, PS=1
    unsigned int curr = ne2k_inb(NE2K_BASE + NE_CURR);
    ne2k_outb(NE2K_BASE + NE_CR, saved_cr | 0x02);  // Back to page 0, start
    unsigned int bnry = ne2k_inb(NE2K_BASE + NE_BNRY);
    unsigned int next_pkt_page = bnry + 1;
    if (next_pkt_page >= RX_PAGE_STOP) next_pkt_page = RX_PAGE_START;
    if (next_pkt_page == curr) {
        return 0;     // Empty
    }

    // Read 4-byte header at next_pkt_page
    unsigned char hdr[4];
    ne2k_dma_read((unsigned int)next_pkt_page << 8, hdr, 4);
    // hdr[0]=status, hdr[1]=next_page, hdr[2..3]=packet length (LE, includes header)
    unsigned int pkt_len = (unsigned int)hdr[2] | ((unsigned int)hdr[3] << 8);
    unsigned int next_page = hdr[1];
    unsigned int copy_len = pkt_len > 4 ? pkt_len - 4 : 0;
    if (copy_len > maxlen) copy_len = maxlen;
    if (copy_len > 0 && (hdr[0] & 0x01)) {
        // Status OK — read packet body (skip 4-byte header)
        ne2k_dma_read(((unsigned int)next_pkt_page << 8) + 4, out, copy_len);
    } else {
        copy_len = 0;
    }
    // Advance BNRY = next_page - 1, wrapping
    if (next_page == RX_PAGE_START) {
        ne2k_outb(NE2K_BASE + NE_BNRY, RX_PAGE_STOP - 1);
    } else {
        ne2k_outb(NE2K_BASE + NE_BNRY, next_page - 1);
    }
    // Clear PRX + OVW + RXE in ISR for hygiene
    ne2k_outb(NE2K_BASE + NE_ISR, 0x15);
    return copy_len;
}
// ============================================================
// End of PM-native NE2000 driver.
// ============================================================


// Conventional-memory bounce buffer for talking to the real-mode
// Crynwr packet driver under PMODE/W. The driver expects ES:DI /
// DS:SI to point at real-mode-addressable memory (linear < 1 MB,
// representable as a 16-bit seg:offset). Our flat-32 BSS sits well
// above 1 MB on a port the size of MicroPython, so flat pointers
// can't be encoded that way.
//
// Allocated once at init via DPMI fn 0x0100. Sized to comfortably
// fit a max ethernet frame (1518 bytes) plus headroom, and reused
// for every TX / RX / get_addr — the packet driver is single-
// threaded from our perspective, so one shared buffer is fine.
//
// Linear address (seg << 4) is what we read/write from flat-32
// code. Standard DOS extenders (PMODE/W, DOS/4GW, CWSDPMI) map
// conventional memory at low linear addresses, so flat reads at
// `bounce_seg << 4` work directly.
#define PKTDRV_BOUNCE_SIZE 2048
static unsigned int  pktdrv_bounce_seg     = 0;   // real-mode segment
static unsigned int  pktdrv_bounce_sel     = 0;   // PM selector (unused, kept for free)
static unsigned int  pktdrv_bounce_linear  = 0;   // flat-32 linear (= seg << 4)

// Real-mode INT 0x60 thunk. PMODE/W's DPMI fn 0x0300 (Simulate Real
// Mode INT) silently no-ops INT 0x60 — it returns CF=0 with the rmcs
// untouched, never actually reaching Crynwr. Verified empirically:
// driver_info / access_type / get_address / send_pkt all "succeed"
// per DPMI but the bounce buffer stays at its pre-call contents and
// no NE2000 TX command is ever issued. (Same behavior under DOS/32A.)
//
// Workaround: call DPMI fn 0x0301 (Call Real Mode Procedure With Far
// Return) targeting a tiny real-mode stub at `pktdrv_thunk_seg:0`
// containing `INT 0x60; RETF`. DPMI 0x0301 reliably dispatches to
// the specified real-mode address, the stub issues INT 0x60 from
// real mode (where Crynwr's IVT[0x60] hook fires correctly), Crynwr
// IRETs back to the stub's RETF, and DPMI returns the modified rmcs
// to us. Allocated once at init via DPMI fn 0x0100.
static unsigned int  pktdrv_thunk_seg      = 0;   // real-mode segment of the int-60 thunk

// DPMI fn 0x0100 — Allocate DOS Memory.
// On entry: AX=0x0100, BX=number of paragraphs (16-byte units).
// On exit (CF clear): AX=real-mode segment, DX=PM selector.
// On error: CF set, AX=DOS error code, BX=largest paragraphs free.
// Pre-allocated bounce buffer segment + flat-32 linear addr from
// main() at startup. PMODE/W uses paging — `seg << 4` is NOT the
// linear address that maps the conventional memory at that real-mode
// segment. main() resolves the true linear address via DPMI fn
// 0x0002 (Allocate Descriptor for Real-Mode Segment) followed by
// fn 0x0006 (Get Selector Base Address) and stashes both globally.
extern unsigned int pktdrv_preallocated_bounce_seg;
extern unsigned int pktdrv_preallocated_bounce_linear;

static int pktdrv_alloc_bounce(void) {
    extern int write(int fd, const void *buf, unsigned int n);
    write(1, "[ab:enter]", 10);
    if (pktdrv_bounce_seg != 0) {
        write(1, "[ab:cached]", 11);
        return 0;
    }
    if (pktdrv_preallocated_bounce_seg == 0) {
        write(1, "[ab:not-prealloced]", 19);
        return -1;
    }
    pktdrv_bounce_seg    = pktdrv_preallocated_bounce_seg;
    pktdrv_bounce_sel    = 0;
    pktdrv_bounce_linear = pktdrv_preallocated_bounce_linear;
    write(1, "[ab:from-prealloc]", 18);
    return 0;
}

// Pre-allocated thunk segment from main() (same shallow-stack
// workaround as pktdrv_preallocated_bounce_seg — PMODE/W's DOS-alloc
// hangs from deep stack).
extern unsigned int pktdrv_preallocated_thunk_seg;

static void _diag_hex32(const char *tag, unsigned int val);

// Initialise the INT 0x60 thunk: take the segment main() pre-
// allocated and write CD 60 CB at offset 0. Idempotent.
static int pktdrv_alloc_thunk(void) {
    extern int write(int fd, const void *buf, unsigned int n);
    write(1, "[at:enter]", 10);
    if (pktdrv_thunk_seg != 0) {
        write(1, "[at:cached]", 11);
        return 0;
    }
    if (pktdrv_preallocated_thunk_seg == 0) {
        write(1, "[at:not-prealloced]", 19);
        return -1;
    }
    unsigned int seg = pktdrv_preallocated_thunk_seg;
    unsigned int linear = seg << 4;
    // Write CD 60 CB at linear:0 (the real-mode INT 0x60 thunk).
    unsigned char *thunk = (unsigned char *)linear;
    thunk[0] = 0xCD;             // INT
    thunk[1] = 0x60;             // vector 60h
    thunk[2] = 0xCB;             // RETF (far return)
    // Stash type filter bytes at offsets 4..15 (12 bytes total).
    // Crynwr v11 NE2000 needs a real ethertype in the type pattern
    // (0x00 0x00 doesn't match any real frame). We register six
    // 2-byte patterns at consecutive offsets so a single thunk
    // paragraph can serve multiple access_type registrations:
    //   off 4..5  : ARP   = 0x08 0x06 (network byte order)
    //   off 6..7  : IPv4  = 0x08 0x00
    //   off 8..9  : IPv6  = 0x86 0xDD
    //   off 10..11: 0x00 0x00 (reserved)
    //   off 12..13: 0x00 0x00 (reserved)
    //   off 14..15: 0x00 0x00 (reserved)
    thunk[4] = 0x08; thunk[5] = 0x06;        // ARP
    thunk[6] = 0x08; thunk[7] = 0x00;        // IPv4
    thunk[8] = 0x86; thunk[9] = 0xDD;        // IPv6
    thunk[10] = 0; thunk[11] = 0;
    thunk[12] = 0; thunk[13] = 0;
    thunk[14] = 0; thunk[15] = 0;
    pktdrv_thunk_seg = seg;
    // Dump thunk content (16 bytes) as 4 dwords for verification.
    _diag_hex32("tk03", ((unsigned int)thunk[0]<<24)
                       |((unsigned int)thunk[1]<<16)
                       |((unsigned int)thunk[2]<<8)
                       |(unsigned int)thunk[3]);
    _diag_hex32("tk47", ((unsigned int)thunk[4]<<24)
                       |((unsigned int)thunk[5]<<16)
                       |((unsigned int)thunk[6]<<8)
                       |(unsigned int)thunk[7]);
    _diag_hex32("tkSG", seg);
    write(1, "[at:ready]", 10);
    return 0;
}

// Call the thunk via DPMI fn 0x0301 (Call Real Mode Procedure With
// Far Return). The caller has already populated `rm` with the inputs
// they want Crynwr to see (ax/bx/cx/dx/si/di/es/ds plus any segment
// regs). We overwrite rm.cs/rm.ip to point at our int-60 thunk and
// dispatch. Returns 0 if DPMI itself succeeded (CF clear); the
// caller still needs to inspect rm.flags for the Crynwr-side CF.
static int pktdrv_call_int60_thunk(pktdrv_rmcs_t *rm) {
    if (pktdrv_thunk_seg == 0) {
        return -1;
    }
    rm->cs = (unsigned short)pktdrv_thunk_seg;
    rm->ip = 0;
    rm->ss = 0;
    rm->sp = 0;
    unsigned int dpmi[8] = {0};
    dpmi[R_EAX] = 0x0301;
    dpmi[R_EBX] = 0;
    dpmi[R_ECX] = 0;
    dpmi[R_EDI] = (unsigned int)(unsigned long)rm;
    unsigned char carry = pktdrv_int_invoke(0x31, dpmi);
    return carry ? -1 : 0;
}

// DPMI fn 0x0300 — Simulate Real Mode Interrupt. Required to reach
// real-mode INT handlers (like the Crynwr packet driver at INT 0x60)
// from a protected-mode DPMI client like ours under PMODE/W. A bare
// `INT 0x60` instruction from prot mode goes through the IDT, which
// is *not* the real-mode IVT; the Crynwr handler lives in real-mode
// memory and only fires when reached via DPMI fn 0x0300.
//
// Inputs (regs[]):  EAX, EBX, ECX, EDX, ESI, EDI all copied into the
// RMCS as-is (these are the real-mode register values the int
// handler sees on entry).
// Outputs (regs[]): EAX, EBX, ECX, EDX, ESI, EDI written back from
// the post-int RMCS.
// Return: bit 0 of the post-int real-mode flags (CF as the
// driver-success indicator), or 1 on outright DPMI failure.
//
// Only used for non-DPMI INT vectors (anything that needs to be
// dispatched into real mode). For DPMI services themselves —
// INT 31h fn 0x0200 / 0x0303 / etc. — call pktdrv_int_invoke
// directly: PMODE/W's IDT handles INT 31h in protected mode.
static unsigned char pktdrv_simulate_real_int(
        unsigned int int_num, unsigned int regs[8]) {
    static pktdrv_rmcs_t rm;
    // Memset would be cleaner; uc386's libc has memset, but we
    // also avoid the dependency by zeroing field-by-field.
    rm.edi = regs[R_EDI];
    rm.esi = regs[R_ESI];
    rm.ebp = 0;
    rm.reserved = 0;
    rm.ebx = regs[R_EBX];
    rm.edx = regs[R_EDX];
    rm.ecx = regs[R_ECX];
    rm.eax = regs[R_EAX];
    rm.flags = 0;
    rm.es = 0; rm.ds = 0; rm.fs = 0; rm.gs = 0;
    rm.ip = 0; rm.cs = 0; rm.sp = 0; rm.ss = 0;

    // INT 0x31 fn 0x0300: AX=0x0300, BL=int_num, BH=0,
    // CX=words-to-copy=0, ES:EDI=ptr to RMCS.
    unsigned int dpmi[8] = {0};
    dpmi[R_EAX] = 0x0300;
    dpmi[R_EBX] = int_num & 0xFF;
    dpmi[R_ECX] = 0;
    dpmi[R_EDI] = (unsigned int)(unsigned long)&rm;
    unsigned char carry = pktdrv_int_invoke(0x31, dpmi);
    if (carry) {
        return 1;
    }

    regs[R_EAX] = rm.eax;
    regs[R_EBX] = rm.ebx;
    regs[R_ECX] = rm.ecx;
    regs[R_EDX] = rm.edx;
    regs[R_ESI] = rm.esi;
    regs[R_EDI] = rm.edi;
    return (rm.flags & 1) ? 1 : 0;
}

// Probe an IVT slot to see if a Crynwr packet driver is installed
// there. Two checks, in order:
//
//   1. Functional probe — issue the driver's `driver_info` call
//      (AH=0x01, BX=0xFFFF). A Crynwr driver returns CF clear and
//      a sensible class+type+version triple in BX/CX/DX/DH; an
//      unrelated default INT vector either returns CF set or
//      garbage. This is the canonical Crynwr-presence test and
//      doesn't depend on conventional-memory mapping.
//
//   2. Fallback signature scan — DPMI fn 0x0200 + linear-address
//      byte read at offset 3 ("PKT DRVR"). Works under dos_emu
//      (where conventional memory is mapped flat at low linear
//      addresses) but is fragile under PMODE/W on real DOS in
//      QEMU+FreeDOS, where the linear-to-real mapping isn't
//      uniformly available. Kept as a backstop for the dos_emu
//      smoke tests where pktdrv_int_invoke's own probe path
//      isn't fully wired.
//
// Returns the linear handler address (or just `int_num` rebadged
// as a positive non-zero token) on success, 0 on miss.
static unsigned int pktdrv_probe_slot(unsigned int int_num) {
    // (1) driver_info call: AH=0x01, BX=0xFFFF (= "no handle yet").
    // Routed through DPMI 0x0300 (Simulate Real Mode Interrupt) so
    // it reaches the real-mode Crynwr handler under PMODE/W.
    // Crynwr returns: DH=class, DL=type, BX=version, CL=number,
    // CX=basic/extended, ES:SI=driver name string, CF=clear on
    // success.
    {
        unsigned int regs[8] = {0};
        regs[R_EAX] = 0x0100;       // AH=0x01 driver_info, AL=0
        regs[R_EBX] = 0xFFFF;
        unsigned char carry = pktdrv_simulate_real_int(int_num, regs);
        if (!carry) {
            // Spot-check: a real Crynwr driver returns BX with a
            // version like 0x0100..0x01FF (1.x), and class DH in
            // {0x01..0x10} (Ethernet et al.). Reject obvious
            // garbage from a default IVT vector that happened to
            // not set CF.
            unsigned int bx = regs[R_EBX] & 0xFFFF;
            unsigned int dh = (regs[R_EDX] >> 8) & 0xFF;
            if (bx >= 0x0100 && bx <= 0x01FF
                    && dh >= 0x01 && dh <= 0x20) {
                return int_num | 0x80000000;  // non-zero token
            }
        }
    }

    // (2) Signature-scan fallback for dos_emu compatibility.
    unsigned int regs[8] = {0};
    regs[R_EAX] = 0x0200;
    regs[R_EBX] = int_num & 0xFF;
    unsigned char carry = pktdrv_int_invoke(0x31, regs);
    if (carry) {
        return 0;
    }
    unsigned int seg = regs[R_ECX] & 0xFFFF;
    unsigned int off = regs[R_EDX] & 0xFFFF;
    unsigned int linear = seg * 16 + off;
    if (linear == 0 || linear >= 0x110000) {
        return 0;
    }
    unsigned char *handler = (unsigned char *)linear;
    for (int i = 0; i < 8; i++) {
        if (handler[3 + i] != PKT_SIG[i]) {
            return 0;
        }
    }
    return linear;
}

// Find a Crynwr packet driver in the IVT. Returns the INT number
// it's installed on, or 0 if none. Caches in pktdrv_int_num.
int pktdrv_detect(void) {
    if (pktdrv_int_num != 0) {
        return pktdrv_int_num;
    }
    for (unsigned int i = 0x60; i < 0x80; i++) {
        if (pktdrv_probe_slot(i) != 0) {
            pktdrv_int_num = (int)i;
            return pktdrv_int_num;
        }
    }
    return 0;
}

static void _diag_hex32(const char *tag, unsigned int val);

// AH=0x06 get_address — fetch the NIC's MAC into `out[6]`.
// Routed through DPMI 0x0300; ES:DI points at the bounce buffer
// (real-mode addressable). After the call, copy the 6 MAC bytes
// from the bounce buffer's linear address into the caller's
// flat-32 `out` buffer.
//
// Under dos_emu (which emulates Crynwr at the prot-mode INT level
// and also intercepts DPMI 0x0300), the same code path works —
// dos_emu treats the seg:off ES:DI as a linear pointer and writes
// to bounce_linear directly.
static int pktdrv_get_addr(unsigned char out[6]) {
    if (pktdrv_int_num == 0 || pktdrv_handle < 0) {
        return -1;
    }
    if (pktdrv_bounce_seg == 0) {
        // Fallback for the rare case the bounce buffer wasn't
        // allocated (DPMI 0x0100 failed). Use the flat pointer —
        // works under dos_emu, broken on real DOS but at least
        // doesn't crash.
        unsigned int regs[8] = {0};
        regs[R_EAX] = 0x0600;
        regs[R_EBX] = (unsigned int)pktdrv_handle;
        regs[R_ECX] = 6;
        regs[R_EDI] = (unsigned int)(unsigned long)out;
        unsigned char carry = pktdrv_int_invoke(
            (unsigned int)pktdrv_int_num, regs);
        if (carry) {
            return -1;
        }
    } else {
        // Use the bounce buffer. ES gets set in the RMCS by the
        // simulate-real-int wrapper variant below (the basic
        // wrapper zeros ES; we need a custom path here).
        static pktdrv_rmcs_t rm;
        rm.edi = 0;                // offset within the bounce buffer
        rm.esi = 0;
        rm.ebp = 0; rm.reserved = 0;
        rm.ebx = (unsigned int)pktdrv_handle;
        rm.edx = 0;
        rm.ecx = 6;
        rm.eax = 0x0600;
        rm.flags = 0;
        rm.es = (unsigned short)pktdrv_bounce_seg;
        rm.ds = 0; rm.fs = 0; rm.gs = 0;
        rm.ip = 0; rm.cs = 0; rm.sp = 0; rm.ss = 0;
        unsigned char carry;
        if (pktdrv_thunk_seg != 0) {
            carry = (unsigned char)(pktdrv_call_int60_thunk(&rm) ? 1 : 0);
        } else {
            unsigned int dpmi[8] = {0};
            dpmi[R_EAX] = 0x0300;
            dpmi[R_EBX] = (unsigned int)pktdrv_int_num & 0xFF;
            dpmi[R_ECX] = 0;
            dpmi[R_EDI] = (unsigned int)(unsigned long)&rm;
            carry = pktdrv_int_invoke(0x31, dpmi);
        }
        _diag_hex32("gaDC", (unsigned int)carry);
        _diag_hex32("gaAX", rm.eax);
        _diag_hex32("gaBX", rm.ebx);
        _diag_hex32("gaDX", rm.edx);
        _diag_hex32("gaFL", (unsigned int)rm.flags);
        // Also dump bounce buffer's first 6 bytes (the MAC if Crynwr wrote it).
        unsigned char *bb = (unsigned char *)pktdrv_bounce_linear;
        _diag_hex32("bb01", ((unsigned int)bb[0] << 24)
                          | ((unsigned int)bb[1] << 16)
                          | ((unsigned int)bb[2] << 8)
                          | (unsigned int)bb[3]);
        _diag_hex32("bb45", ((unsigned int)bb[4] << 24)
                          | ((unsigned int)bb[5] << 16));
        if (carry) {
            return -1;
        }
        if (rm.flags & 1) {
            return -1;
        }
        memcpy(out, (unsigned char *)pktdrv_bounce_linear, 6);
    }
    memcpy(pktdrv_mac_cache, out, 6);
    return 0;
}

// AH=0x02 access_type — register `receiver` as the packet handler
// for `ethertype`. Use ethertype=0x0000 with type_len=0 to catch
// every protocol (broadcast netif sees ARP + IPv4 alike).
// AH=0x02 access_type — register the receiver with the driver.
// The `linear_receiver` argument is already encoded as a real-mode
// seg:offset packed into a single 32-bit value (high word = seg,
// low word = offset) by pktdrv_init when DPMI 0x0303 succeeded.
// We unpack that into the RMCS's ES:EDI directly. With type_len=0
// (catch-all), DS:SI is ignored.
//
// Under dos_emu the legacy bare-INT path remains as a fallback —
// dos_emu's AH=02 hook reads the linear receiver value out of EDI
// regardless.
// Catch-all type filter: 2 bytes, value 0x0000. Crynwr V11 supports
// type_len > 0 with data — we use 2 bytes 0x0000 which matches "any"
// per the convention some Crynwr drivers prefer over type_len=0 (which
// can be rejected silently).
static unsigned char _pktdrv_catchall_type[2] = {0, 0};

static void _diag_hex32(const char *tag, unsigned int val);

// Diag: emit "[tag=XXXXXXXX]" to fd 1. 4-byte tag, 8 hex digits.
static void _diag_hex32(const char *tag, unsigned int val) {
    extern int write(int fd, const void *buf, unsigned int n);
    char out[16];
    out[0] = '[';
    out[1] = tag[0];
    out[2] = tag[1];
    out[3] = tag[2];
    out[4] = tag[3];
    out[5] = '=';
    for (int i = 0; i < 8; i++) {
        unsigned int nib = (val >> ((7 - i) * 4)) & 0xF;
        out[6 + i] = (char)(nib < 10 ? '0' + nib : 'a' + nib - 10);
    }
    out[14] = ']';
    out[15] = 0;
    write(1, out, 15);
}

static int pktdrv_access(unsigned int linear_receiver) {
    extern int write(int fd, const void *buf, unsigned int n);
    if (pktdrv_int_num == 0) {
        return -1;
    }
    static pktdrv_rmcs_t rm;
    rm.edi = linear_receiver & 0xFFFF;     // offset (receiver real-mode IP)
    // DS:SI = real-mode pointer to type bytes (thunk paragraph, off 6 = IPv4).
    // We register the IPv4 ethertype (0x0800) rather than ARP (0x0806)
    // because DHCP OFFER replies come back as UDP/IP -- the slirp/qemu
    // DHCP server doesn't need to ARP us first, it sends OFFER straight
    // to the broadcast.  ARP-only registration silently drops every
    // DHCP reply.  ARP can be added as a second access_type handle if
    // we ever need to reach hosts on a non-trivial L2 (none of our
    // smoke targets do; slirp/SLIRP both bridge IP without exposing
    // their gateway MAC).
    rm.esi = 6;                             // IPv4 type pattern at thunk[6..7]
    rm.ebp = 0; rm.reserved = 0;
    rm.ebx = 0xFFFF;                        // if_type=0xFFFF (any/wildcard)
    rm.edx = 0;                             // if_number=0 (first card)
    rm.ecx = 2;                             // type_len=2 (match 2-byte ethertype)
    rm.eax = 0x0201;                        // AH=02 access_type, AL=01 Ethernet DIX
    rm.flags = 0;
    rm.es = (unsigned short)((linear_receiver >> 16) & 0xFFFF);
    rm.ds = (unsigned short)pktdrv_thunk_seg;  // type bytes live in our thunk paragraph
    rm.fs = 0; rm.gs = 0;
    rm.ip = 0; rm.cs = 0; rm.sp = 0; rm.ss = 0;

    // Route INT 0x60 through the real-mode thunk via DPMI 0x0301 if
    // available — DPMI 0x0300 silently no-ops INT 0x60 in PMODE/W.
    unsigned char carry;
    if (pktdrv_thunk_seg != 0) {
        carry = (unsigned char)(pktdrv_call_int60_thunk(&rm) ? 1 : 0);
    } else {
        unsigned int dpmi[8] = {0};
        dpmi[R_EAX] = 0x0300;
        dpmi[R_EBX] = (unsigned int)pktdrv_int_num & 0xFF;
        dpmi[R_ECX] = 0;
        dpmi[R_EDI] = (unsigned int)(unsigned long)&rm;
        carry = pktdrv_int_invoke(0x31, dpmi);
    }
    _diag_hex32("acDC", (unsigned int)carry);     // DPMI carry
    _diag_hex32("acAX", rm.eax);
    _diag_hex32("acBX", rm.ebx);
    _diag_hex32("acDX", rm.edx);
    _diag_hex32("acFL", (unsigned int)rm.flags);
    if (carry) {
        // DPMI 0x0300 itself failed (no DPMI host?). Try bare INT
        // for dos_emu compatibility. dos_emu intercepts INT 0x60
        // at the prot-mode level, so this works there.
        unsigned int regs[8] = {0};
        regs[R_EAX] = 0x0201;       // AH=02 access_type, AL=01 Ethernet DIX
        regs[R_EBX] = 0;             // if_type=0 (driver default)
        regs[R_ECX] = 0;
        regs[R_EDX] = 0;
        regs[R_EDI] = linear_receiver;
        carry = pktdrv_int_invoke((unsigned int)pktdrv_int_num, regs);
        if (carry) {
            return -1;
        }
        pktdrv_handle = (int)(regs[R_EAX] & 0xFFFF);
        pktdrv_last_handle = (unsigned int)(regs[R_EAX] & 0xFFFFFFFF);
        return 0;
    }
    if (rm.flags & 1) {
        return -1;
    }
    pktdrv_handle = (int)(rm.eax & 0xFFFF);
    pktdrv_last_handle = (unsigned int)(rm.eax & 0xFFFFFFFF);
    return 0;
}

// 32-bit receiver. cdecl signature; dos_emu's AH=0x99 polling path
// writes directly to pktdrv_rx_buf without ever calling this, but
// the DPMI thunk below DOES call it from real-mode-context after
// the packet driver has bounced through DPMI.
//   phase==0: caller wants a buffer for `len` bytes. Return a flat
//             pointer into pktdrv_rx_buf or NULL to drop.
//   phase==1: caller has finished the copy. Mark the slot full.
unsigned char *uc386dos_pktdrv_receiver(int phase, unsigned int len) {
    if (phase == 0) {
        if (pktdrv_rx_pending || len > sizeof(pktdrv_rx_buf)) {
            return NULL;
        }
        pktdrv_rx_len = len;
        // On dos_emu the receiver writes directly to the flat
        // pktdrv_rx_buf in BSS. On real DOS under PMODE/W the
        // BSS lands above 1 MB so the seg:off encoding in the
        // dpmi_thunk below would overflow — return the bounce
        // buffer's flat-linear address instead so the encoding
        // stays in range. After phase=1 we copy from bounce_linear
        // to pktdrv_rx_buf for MP-side consumption.
        if (pktdrv_bounce_seg != 0
                && len <= PKTDRV_BOUNCE_SIZE) {
            return (unsigned char *)pktdrv_bounce_linear;
        }
        return pktdrv_rx_buf;
    }
    // phase == 1: real-mode driver has copied the frame in.
    if (pktdrv_bounce_seg != 0 && pktdrv_rx_len <= sizeof(pktdrv_rx_buf)) {
        memcpy(pktdrv_rx_buf,
               (unsigned char *)pktdrv_bounce_linear,
               pktdrv_rx_len);
    }
    pktdrv_rx_pending = 1;
    return NULL;
}

// DPMI fn 0x0303 callback worker — the 32-bit handler the DPMI
// host invokes (via the IRETD-terminated `_uc386dos_pktdrv_dpmi_
// thunk` stub in i386_dos_libc.asm) when the real-mode trampoline
// fires. On entry to the stub:
//   DS:ESI = pointer to saved real-mode SS:SP
//   ES:EDI = pointer to our pktdrv_rmcs (DPMI fills it from the
//   saved real-mode register frame)
// The stub reloads DS/ES to our flat data selector before calling
// in here, so global accesses (`pktdrv_rmcs`, etc.) work normally.
//
// Crynwr's calling convention puts phase in AX and length in CX,
// expects ES:DI = buffer on phase=0 return. We translate from the
// RMCS, drive the existing receiver, and write the buffer's real-
// mode seg:offset back into RMCS so DPMI can hand it to real mode
// on its way out (the stub leaves the RMCS in place).
//
// The asm stub does IRETD, not RET — DPMI 0.9 fn 0x0303 invokes
// the PM handler via FAR CALL with EFLAGS underneath CS:EIP, and a
// near-return desyncs the host's stack (which presents as a GPF on
// the NEXT DPMI dispatch, not this one — so the bug looked like
// "second send crashes" instead of "first callback corrupts
// state"). The C function itself stays a normal cdecl void(void).
void uc386dos_pktdrv_dpmi_thunk_c(void) {
    // Bump the invocation counter ASAP — IRQ context, can't safely
    // call DOS for write(); the counter is what Python polls.
    pktdrv_thunk_invocations++;
    unsigned int phase  = pktdrv_rmcs.eax & 0xFFFF;
    unsigned int length = pktdrv_rmcs.ecx & 0xFFFF;
    if (phase == 0) pktdrv_thunk_phase0_count++;
    if (phase == 1) pktdrv_thunk_phase1_count++;
    unsigned char *buf = uc386dos_pktdrv_receiver((int)phase, length);
    if (phase == 0 && buf != NULL) {
        unsigned int linear = (unsigned int)(unsigned long)buf;
        // Real-mode seg:off encoding. The buffer must live in
        // <1 MB conventional memory for this to be reachable;
        // pktdrv_rx_buf is a static in our flat 32-bit BSS, so
        // its linear address satisfies that on any reasonable
        // PMODE/W layout (BSS lands well below 1 MB in our binary).
        pktdrv_rmcs.es  = (unsigned short)((linear >> 4) & 0xFFFF);
        pktdrv_rmcs.edi = (linear & 0xF) | (pktdrv_rmcs.edi & 0xFFFF0000);
    }
}

// Allocate a real-mode callback via DPMI INT 0x31 fn 0x0303.
// Inputs (per spec):
//   DS:ESI = address of our 32-bit handler
//   ES:EDI = address of the RMCS buffer DPMI should populate
// Returns:
//   CX:DX = real-mode segment:offset of the trampoline
//   CF clear on success.
// On a host without DPMI (raw real-mode DOS), this returns CF=1
// and we leave pktdrv_dpmi_{seg,off} at 0; pktdrv_init then falls
// back to passing the flat receiver address to access_type, which
// works under dos_emu (with AH=0x99 polling) but not on real DOS.
// IRETD wrapper in lib/i386_dos_libc.asm. Registers as a DPMI 0.9
// fn 0x0303 PM callback; on RX it calls uc386dos_pktdrv_dpmi_thunk_c
// above and then IRETDs (the host's required return convention).
extern void uc386dos_pktdrv_dpmi_thunk(void);

// Anchor: uc386's asm DCE walks references in the emitted .asm only,
// not in the appended libc.asm — so the `extern _uc386dos_pktdrv_
// dpmi_thunk_c` declared by the IRETD wrapper is invisible to it,
// and DCE would prune the C function as unreachable. Taking the
// address here (in a reachable function) makes the symbol show up
// in this function's emitted asm and keeps it live through DCE.
// The value is discarded; the side-effect is purely the reference.
static volatile unsigned int _pktdrv_dpmi_thunk_c_anchor;

static int pktdrv_alloc_dpmi_callback(void) {
    _pktdrv_dpmi_thunk_c_anchor =
        (unsigned int)(unsigned long)&uc386dos_pktdrv_dpmi_thunk_c;
    unsigned int regs[8] = {0};
    regs[R_EAX] = 0x0303;
    regs[R_ESI] = (unsigned int)(unsigned long)
                  &uc386dos_pktdrv_dpmi_thunk;
    regs[R_EDI] = (unsigned int)(unsigned long)&pktdrv_rmcs;
    unsigned char carry = pktdrv_int_invoke(0x31, regs);
    if (carry) {
        return -1;
    }
    pktdrv_dpmi_seg = regs[R_ECX] & 0xFFFF;
    pktdrv_dpmi_off = regs[R_EDX] & 0xFFFF;
    return 0;
}

// AH=0x99 (uc386dos extension): hand the dos_emu harness pointers
// to pktdrv_rx_buf / pktdrv_rx_len / pktdrv_rx_pending so it can
// post inbound frames directly without going through the
// receiver-callback dance. Bypasses Crynwr's two-phase callback
// semantics — those need a real-mode trampoline that we'd allocate
// via DPMI INT 31h fn 0x0303 on hardware. Until that lands, this
// extension keeps the dos_emu test path strictly on the Crynwr INT
// number while still delivering RX frames. Real DOS Crynwr drivers
// don't implement AH=0x99; pktdrv_recv simply returns 0 there until
// the DPMI trampoline replaces this.
// AH=0x99 (uc386dos/dosiz extension): hand the harness pointers to
// pktdrv_rx_buf / pktdrv_rx_len / pktdrv_rx_pending so it can post
// inbound frames directly without going through the receiver-callback
// dance.  Real-DOS Crynwr drivers (NE2000.COM etc.) don't implement
// this; thunked AH=99 against NE2000.COM v11.4.3 leaves DOS/32A's PM
// scratch state subtly corrupted (INT 0x0D #GP on the next INT 0x1A).
// Probe via a thunked AH=01 driver_info first and only issue AH=99
// when the driver returns dosiz's distinguishing version marker.
// On dos_emu the bare-INT path bypasses the probe (thunk_seg == 0).
static void pktdrv_register_polling_rx(void) {
    if (pktdrv_thunk_seg == 0) {
        unsigned int regs[8] = {0};
        regs[R_EAX] = 0x9900;
        regs[R_EDI] = (unsigned int)(unsigned long)pktdrv_rx_buf;
        regs[R_ESI] = (unsigned int)(unsigned long)&pktdrv_rx_pending;
        regs[R_ECX] = (unsigned int)(unsigned long)&pktdrv_rx_len;
        (void)pktdrv_int_invoke((unsigned int)pktdrv_int_num, regs);
        return;
    }
    // dosiz's dosiz_int60 AH=01 sets ecx_low=0x0202.  Real Crynwr
    // drivers return a 1.x BX version (and CX is basic+extended bits,
    // not the same value).
    static pktdrv_rmcs_t info;
    info.edi = 0; info.esi = 0; info.ebp = 0; info.reserved = 0;
    info.ebx = 0xFFFF; info.edx = 0; info.ecx = 0;
    info.eax = 0x0100;
    info.flags = 0;
    info.es = 0; info.ds = 0; info.fs = 0; info.gs = 0;
    info.ip = 0; info.cs = 0; info.sp = 0; info.ss = 0;
    if (pktdrv_call_int60_thunk(&info) != 0 || (info.flags & 1)) {
        return;
    }
    if ((info.ecx & 0xFFFF) != 0x0202) {
        return;   // not dosiz; let DPMI 0x0303 callback handle RX
    }
    static pktdrv_rmcs_t rm;
    rm.edi = (unsigned int)(unsigned long)pktdrv_rx_buf;
    rm.esi = (unsigned int)(unsigned long)&pktdrv_rx_pending;
    rm.ecx = (unsigned int)(unsigned long)&pktdrv_rx_len;
    rm.ebx = 0; rm.edx = 0; rm.ebp = 0; rm.reserved = 0;
    rm.eax = 0x9900;
    rm.flags = 0;
    rm.es = 0; rm.ds = 0; rm.fs = 0; rm.gs = 0;
    rm.ip = 0; rm.cs = 0; rm.sp = 0; rm.ss = 0;
    (void)pktdrv_call_int60_thunk(&rm);
}

// Public init: detect, register, fetch MAC. Returns 0 on success.
//
// Order matters:
//   1. pktdrv_detect — find the Crynwr driver in the IVT.
//   2. pktdrv_alloc_dpmi_callback — get a real-mode trampoline
//      address. On hardware that's required for AH=02 to register a
//      callable receiver. On dos_emu we emulate fn 0x0303 too so
//      the same code path runs uniformly.
//   3. pktdrv_access — register the trampoline (or, with DPMI
//      missing, the flat receiver address — works under emulator,
//      garbage on real DOS).
//   4. pktdrv_get_addr — read the MAC.
//   5. pktdrv_register_polling_rx — AH=0x99, the dos_emu RX
//      bypass. No-op on real DOS where AH=0x99 isn't implemented.
int pktdrv_init(unsigned char mac[6]) {
    extern int write(int fd, const void *buf, unsigned int n);
    write(1, "[pi:enter]", 10);
#ifndef PKTDRV_FORCE_CRYNWR
    // PM-native NE2000 path: skip Crynwr entirely. Built-in unless
    // -DPKTDRV_FORCE_CRYNWR=1 is set, in which case we fall straight
    // through to the standard mTCP / htget-style Crynwr+DPMI 0x0303
    // path (any NIC with a real-DOS packet driver, not just NE2000).
    if (ne2k_init_direct(mac) == 0) {
        write(1, "[pi:ne2k-ok]", 12);
        // Mark int_num as a sentinel so pktdrv_send takes the
        // direct path below, not the Crynwr fallback.
        pktdrv_int_num = -1;
        // Dump the MAC for sanity
        char buf[40] = "[ne2k:mac=XXXXXXXXXXXX]";
        static const char hex[] = "0123456789abcdef";
        for (int i = 0; i < 6; i++) {
            buf[10 + i*2]     = hex[(mac[i] >> 4) & 0xF];
            buf[10 + i*2 + 1] = hex[mac[i] & 0xF];
        }
        write(1, buf, 23);
        return 0;
    }
    write(1, "[pi:ne2k-fail-fallback-crynwr]", 30);
#else
    write(1, "[pi:force-crynwr]", 17);
#endif
    if (pktdrv_detect() == 0) {
        write(1, "[pi:no-driver]", 14);
        return -1;
    }
    write(1, "[pi:detected]", 13);
    // pktdrv_alloc_bounce reads the segment pre-allocated by main()
    // (PMODE/W's INT 21h AH=0x48 hangs from deep stack frames; main()
    // does the alloc while shallow and stores it to a global).
    (void)pktdrv_alloc_bounce();
    write(1, "[pi:bounce-done]", 16);
    if (pktdrv_alloc_thunk() != 0) {
        // Without the thunk, INT 0x60 calls won't reach Crynwr under
        // PMODE/W. Keep going (dos_emu doesn't need it) but log it.
        write(1, "[pi:no-thunk]", 13);
    }

    unsigned int receiver_linear;
    if (pktdrv_alloc_dpmi_callback() == 0) {
        write(1, "[dpmi:cb-ok]", 12);
        _diag_hex32("dpSG", pktdrv_dpmi_seg);
        _diag_hex32("dpOF", pktdrv_dpmi_off);
        // DPMI trampoline succeeded — encode its real-mode
        // seg:offset for access_type's ES:DI.
        receiver_linear = (pktdrv_dpmi_seg << 16) | (pktdrv_dpmi_off & 0xFFFF);
    } else {
        write(1, "[dpmi:cb-fail]", 14);
        receiver_linear = (unsigned int)(unsigned long)
                          &uc386dos_pktdrv_receiver;
    }
    write(1, "[pi:pre-access]", 15);

    // Pre-cleanup: release any stale subscription. Crynwr v11 may
    // hold residual state from a prior process that crashed without
    // calling release_type. AH=0x03, BX=0xFFFF means "release all".
    if (pktdrv_thunk_seg != 0) {
        static pktdrv_rmcs_t rrm;
        rrm.edi = 0; rrm.esi = 0; rrm.ebp = 0; rrm.reserved = 0;
        rrm.ebx = 0xFFFF;                 // BX = handle (0xFFFF = all)
        rrm.edx = 0; rrm.ecx = 0;
        rrm.eax = 0x0300;                 // AH=0x03 release_type
        rrm.flags = 0;
        rrm.es = 0; rrm.ds = 0; rrm.fs = 0; rrm.gs = 0;
        rrm.ip = 0; rrm.cs = 0; rrm.sp = 0; rrm.ss = 0;
        (void)pktdrv_call_int60_thunk(&rrm);
        _diag_hex32("rlAX", rrm.eax);
        _diag_hex32("rlFL", (unsigned int)rrm.flags);
    }

    if (pktdrv_access(receiver_linear) != 0) {
        write(1, "[pi:access-fail]", 16);
        pktdrv_int_num = 0;
        return -2;
    }
    write(1, "[pi:access-ok]", 14);

    // Set receive mode to 3 (broadcast + own-node). Some Crynwr drivers
    // default to mode 1 (receiver off) — without this call, access_type
    // returns a handle but no packets get delivered to our callback.
    {
        static pktdrv_rmcs_t rm;
        rm.edi = 0; rm.esi = 0; rm.ebp = 0; rm.reserved = 0;
        rm.ebx = (unsigned int)pktdrv_handle;   // BX = handle
        rm.edx = 0;
        rm.ecx = 3;                              // CX = mode 3 (broadcast + node)
        rm.eax = 0x1400;                         // AH=0x14 set_rcv_mode
        rm.flags = 0;
        rm.es = 0; rm.ds = 0; rm.fs = 0; rm.gs = 0;
        rm.ip = 0; rm.cs = 0; rm.sp = 0; rm.ss = 0;
        if (pktdrv_thunk_seg != 0) {
            (void)pktdrv_call_int60_thunk(&rm);
        }
        _diag_hex32("rmAX", rm.eax);
        _diag_hex32("rmDX", rm.edx);
        _diag_hex32("rmFL", (unsigned int)rm.flags);
    }

    if (pktdrv_get_addr(mac) != 0) {
        write(1, "[pi:getaddr-fail]", 17);
        pktdrv_int_num = 0;
        return -3;
    }
    write(1, "[pi:getaddr-ok]", 15);
    // Dump the MAC bytes we actually got. If they're 52:54:00:12:34:56,
    // DPMI 0x0300 worked. If zeros, DPMI didn't actually invoke the
    // real-mode INT (silent failure).
    {
        char buf[40] = "[pi:mac=";
        for (int i = 0; i < 6; i++) {
            unsigned int b = mac[i];
            unsigned int hi = (b >> 4) & 0xF;
            unsigned int lo = b & 0xF;
            buf[8 + i*2] = (hi < 10) ? ('0' + hi) : ('a' + hi - 10);
            buf[8 + i*2 + 1] = (lo < 10) ? ('0' + lo) : ('a' + lo - 10);
        }
        buf[20] = ']';
        write(1, buf, 21);
    }
    // Dump key state as decimals so we can verify they're sensible.
    {
        char buf[32] = "[pi:h=";
        unsigned int h = (unsigned int)pktdrv_handle;
        buf[6] = '0' + ((h / 10000) % 10);
        buf[7] = '0' + ((h / 1000) % 10);
        buf[8] = '0' + ((h / 100) % 10);
        buf[9] = '0' + ((h / 10) % 10);
        buf[10] = '0' + (h % 10);
        buf[11] = ',';
        buf[12] = 'b';
        buf[13] = '=';
        unsigned int s = pktdrv_bounce_seg;
        buf[14] = '0' + ((s / 10000) % 10);
        buf[15] = '0' + ((s / 1000) % 10);
        buf[16] = '0' + ((s / 100) % 10);
        buf[17] = '0' + ((s / 10) % 10);
        buf[18] = '0' + (s % 10);
        buf[19] = ',';
        buf[20] = 'i';
        buf[21] = '=';
        unsigned int i = (unsigned int)pktdrv_int_num;
        buf[22] = '0' + ((i / 100) % 10);
        buf[23] = '0' + ((i / 10) % 10);
        buf[24] = '0' + (i % 10);
        buf[25] = ']';
        write(1, buf, 26);
    }
    pktdrv_register_polling_rx();
    /* Note: the DOS/32A AH=04 IRQ-during-send GPF workaround is now
     * a TRANSIENT mask wrapped around the DPMI 0x0301 dispatch in
     * pktdrv_send (per the original code comment's intent). The
     * earlier permanent mask installed here used to disable IRQ 9
     * for the whole program lifetime, which blocked the Crynwr
     * receive callback entirely — no DHCP, no ARP responses, etc.
     */
    write(1, "[pi:done]", 9);
    return 0;
}

/* PM-side IO port helpers (PMODE/W and DOS/32A both grant flat PM
 * apps full IO permission by default). Reused from the NE2000 path. */
extern void         ne2k_outb(unsigned int port, unsigned int val);
extern unsigned int ne2k_inb(unsigned int port);

// AH=0x04 send_pkt — transmit a frame. The packet driver expects
// DS:SI to point at real-mode-addressable memory; copy `buf` into
// our DPMI-allocated bounce buffer and pass its seg:offset.
//
// Without a bounce buffer (early init or DPMI failure) we fall back
// to the bare-INT path with a flat-linear DS:SI value — dos_emu's
// AH=04 hook handles that, real DOS doesn't.
//
// IRQ workaround: DOS/32A's RM→PM transition for the AH=04 send_pkt
// path GPFs deterministically when an IRQ fires during the real-
// mode send. The crash is reproduced cleanly in rigs/crynwr-send-
// loop and is symmetric across NE2000.COM (IRQ 9) and PCNTPK.COM
// (IRQ 11) — i.e., it's NOT driver-specific, it's a DOS/32A bug.
// Workaround: mask all slave-PIC IRQs (8-15) across the DPMI call
// and restore on return. Brief masking window; any RX packets are
// queued in the NIC's onboard buffer and delivered when Crynwr's
// handler runs after the unmask.
int pktdrv_send(const unsigned char *buf, unsigned int len) {
    extern int write(int fd, const void *buf, unsigned int n);
    // PM-native NE2000 path
    if (ne2k_initialized) {
        return ne2k_send_direct(buf, len);
    }
    if (pktdrv_int_num == 0) {
        return -1;
    }
    if (len > PKTDRV_BOUNCE_SIZE) {
        return -1;
    }
    if (pktdrv_bounce_seg == 0) {
        // Fallback path.
        unsigned int regs[8] = {0};
        regs[R_EAX] = 0x0400;
        regs[R_ECX] = len;
        regs[R_ESI] = (unsigned int)(unsigned long)buf;
        unsigned char carry = pktdrv_int_invoke(
            (unsigned int)pktdrv_int_num, regs);
        return carry ? -1 : 0;
    }
    memcpy((unsigned char *)pktdrv_bounce_linear, buf, len);
    static pktdrv_rmcs_t rm;
    rm.edi = 0;
    rm.esi = 0;                                 // SI offset = 0 within bounce
    rm.ebp = 0; rm.reserved = 0;
    rm.ebx = (unsigned int)pktdrv_handle;
    rm.edx = 0;
    rm.ecx = len;
    rm.eax = 0x0400;
    rm.flags = 0;
    rm.es = 0; rm.fs = 0; rm.gs = 0;
    rm.ds = (unsigned short)pktdrv_bounce_seg;
    rm.ip = 0; rm.cs = 0; rm.sp = 0; rm.ss = 0;
    unsigned int dpmi[8] = {0};
    dpmi[R_EAX] = 0x0300;
    dpmi[R_EBX] = (unsigned int)pktdrv_int_num & 0xFF;
    dpmi[R_ECX] = 0;
    dpmi[R_EDI] = (unsigned int)(unsigned long)&rm;
    unsigned char carry;
    /* DOS/32A IRQ-during-send GPF workaround: mask slave-PIC IRQs
     * 9/11 (bits 1/3 of OCW1 at port 0xA1) just for the DPMI 0x0301
     * dispatch, restore the original mask byte after. The bare INT
     * 0x60 send via DOS/32A's RM->PM transition GPFs deterministically
     * when an IRQ fires during the real-mode portion of the call
     * (reproduced cleanly in rigs/crynwr-send-loop). Masking only for
     * the duration of dispatch lets RX packets queued in the NIC's
     * onboard buffer get delivered when Crynwr's handler runs after
     * the unmask. The previous permanent mask in pktdrv_init blocked
     * receive entirely. */
    unsigned int saved_mask = ne2k_inb(0xA1);
    ne2k_outb(0xA1, saved_mask | 0x0A);
    if (pktdrv_thunk_seg != 0) {
        carry = (unsigned char)(pktdrv_call_int60_thunk(&rm) ? 1 : 0);
        (void)dpmi;
    } else {
        carry = pktdrv_int_invoke(0x31, dpmi);
    }
    ne2k_outb(0xA1, saved_mask);
    if (carry) {
        write(1, "[ps:dpmi-fail]", 14);
        return -1;
    }
    if (rm.flags & 1) {
        // CF set in real-mode flags = Crynwr returned error.
        // AH = error code per Crynwr spec.
        char buf[24] = "[ps:cf-set,err=";
        unsigned int ah = (rm.eax >> 8) & 0xFF;
        buf[15] = '0' + ((ah / 100) % 10);
        buf[16] = '0' + ((ah / 10) % 10);
        buf[17] = '0' + (ah % 10);
        buf[18] = ']';
        write(1, buf, 19);
        return -1;
    }
    write(1, "[ps:ok]", 7);
    return 0;
}

// Drain the RX slot if one is pending. Returns the byte count
// written (clamped to maxlen) or 0 if nothing's queued. Truncates
// silently on overflow.
unsigned int pktdrv_recv(unsigned char *out, unsigned int maxlen) {
    // PM-native NE2000 path: poll the NIC ring directly.
    if (ne2k_initialized) {
        return ne2k_poll_rx(out, maxlen);
    }
    if (!pktdrv_rx_pending) {
        return 0;
    }
    unsigned int n = pktdrv_rx_len;
    if (n > maxlen) {
        n = maxlen;
    }
    memcpy(out, pktdrv_rx_buf, n);
    pktdrv_rx_pending = 0;
    pktdrv_rx_len = 0;
    return n;
}

// True when either Crynwr is attached OR the PM-native NE2000 driver
// is initialised. Either way, pktdrv_send/recv will do something useful.
int pktdrv_is_active(void) {
    return ne2k_initialized || (pktdrv_int_num != 0 && pktdrv_int_num != -1);
}
