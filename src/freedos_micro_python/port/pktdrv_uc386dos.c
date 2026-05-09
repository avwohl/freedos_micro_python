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

// DPMI fn 0x0100 — Allocate DOS Memory.
// On entry: AX=0x0100, BX=number of paragraphs (16-byte units).
// On exit (CF clear): AX=real-mode segment, DX=PM selector.
// On error: CF set, AX=DOS error code, BX=largest paragraphs free.
static int pktdrv_alloc_bounce(void) {
    if (pktdrv_bounce_seg != 0) {
        return 0;  // already allocated
    }
    unsigned int paragraphs = (PKTDRV_BOUNCE_SIZE + 15) / 16;
    unsigned int regs[8] = {0};
    regs[R_EAX] = 0x0100;
    regs[R_EBX] = paragraphs;
    unsigned char carry = pktdrv_int_invoke(0x31, regs);
    if (carry) {
        return -1;
    }
    pktdrv_bounce_seg    = regs[R_EAX] & 0xFFFF;
    pktdrv_bounce_sel    = regs[R_EDX] & 0xFFFF;
    pktdrv_bounce_linear = pktdrv_bounce_seg << 4;
    return 0;
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
        unsigned int dpmi[8] = {0};
        dpmi[R_EAX] = 0x0300;
        dpmi[R_EBX] = (unsigned int)pktdrv_int_num & 0xFF;
        dpmi[R_ECX] = 0;
        dpmi[R_EDI] = (unsigned int)(unsigned long)&rm;
        unsigned char carry = pktdrv_int_invoke(0x31, dpmi);
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
static int pktdrv_access(unsigned int linear_receiver) {
    if (pktdrv_int_num == 0) {
        return -1;
    }
    static pktdrv_rmcs_t rm;
    rm.edi = linear_receiver & 0xFFFF;     // offset
    rm.esi = 0;
    rm.ebp = 0; rm.reserved = 0;
    rm.ebx = 0;                             // if_type=0 (driver default)
    rm.edx = 0;                             // if_number=0 (first card)
    rm.ecx = 0;                             // type_len=0 (catch-all type)
    rm.eax = 0x0201;                        // AH=02 access_type, AL=01 Ethernet DIX
    rm.flags = 0;
    rm.es = (unsigned short)((linear_receiver >> 16) & 0xFFFF);
    rm.ds = 0; rm.fs = 0; rm.gs = 0;
    rm.ip = 0; rm.cs = 0; rm.sp = 0; rm.ss = 0;

    unsigned int dpmi[8] = {0};
    dpmi[R_EAX] = 0x0300;
    dpmi[R_EBX] = (unsigned int)pktdrv_int_num & 0xFF;
    dpmi[R_ECX] = 0;
    dpmi[R_EDI] = (unsigned int)(unsigned long)&rm;
    unsigned char carry = pktdrv_int_invoke(0x31, dpmi);
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

// DPMI fn 0x0303 callback target — the 32-bit handler the DPMI
// host invokes when the real-mode trampoline fires. On entry:
//   DS:ESI = ES:EDI = pointer to our pktdrv_rmcs (DPMI fills it
//   from the saved real-mode register frame).
// Crynwr's calling convention puts phase in AX and length in CX,
// expects ES:DI = buffer on phase=0 return. We translate from
// the RMCS, drive the existing receiver, and write the buffer's
// real-mode seg:offset back into RMCS so DPMI can hand it to
// real mode on its way out.
//
// Cdecl signature so we can call it directly under emulation as
// well — dos_emu's DPMI fn 0x0303 emulation invokes this through
// the same path. (No-op under hardware DPMI; the host calls it.)
void uc386dos_pktdrv_dpmi_thunk(void) {
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
static int pktdrv_alloc_dpmi_callback(void) {
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
static void pktdrv_register_polling_rx(void) {
    unsigned int regs[8] = {0};
    regs[R_EAX] = 0x9900;
    regs[R_EDI] = (unsigned int)(unsigned long)pktdrv_rx_buf;
    regs[R_ESI] = (unsigned int)(unsigned long)&pktdrv_rx_pending;
    regs[R_ECX] = (unsigned int)(unsigned long)&pktdrv_rx_len;
    (void)pktdrv_int_invoke((unsigned int)pktdrv_int_num, regs);
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
    if (pktdrv_detect() == 0) {
        return -1;
    }
    // Allocate the conventional-memory bounce buffer first — used
    // by pktdrv_get_addr / pktdrv_send below, and silently handed
    // to the receiver thunk for the RX seg:offset encoding. On
    // dos_emu DPMI 0x0100 returns success and we get a real
    // segment; on a host without DPMI the call sets CF and we
    // continue without a bounce buffer (fallback flat-pointer
    // paths still work under emulation).
    (void)pktdrv_alloc_bounce();

    unsigned int receiver_linear;
    extern int write(int fd, const void *buf, unsigned int n);
    if (pktdrv_alloc_dpmi_callback() == 0) {
        write(1, "[dpmi:cb-ok]", 12);
        // DPMI trampoline succeeded — encode its real-mode
        // seg:offset for access_type's ES:DI.
        receiver_linear = (pktdrv_dpmi_seg << 16) | (pktdrv_dpmi_off & 0xFFFF);
    } else {
        write(1, "[dpmi:cb-fail]", 14);
        receiver_linear = (unsigned int)(unsigned long)
                          &uc386dos_pktdrv_receiver;
    }
    if (pktdrv_access(receiver_linear) != 0) {
        pktdrv_int_num = 0;
        return -2;
    }
    if (pktdrv_get_addr(mac) != 0) {
        pktdrv_int_num = 0;
        return -3;
    }
    pktdrv_register_polling_rx();
    return 0;
}

// AH=0x04 send_pkt — transmit a frame. The packet driver expects
// DS:SI to point at real-mode-addressable memory; copy `buf` into
// our DPMI-allocated bounce buffer and pass its seg:offset.
//
// Without a bounce buffer (early init or DPMI failure) we fall back
// to the bare-INT path with a flat-linear DS:SI value — dos_emu's
// AH=04 hook handles that, real DOS doesn't.
int pktdrv_send(const unsigned char *buf, unsigned int len) {
    extern int write(int fd, const void *buf, unsigned int n);
    write(1, "[ps:enter]", 10);
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
    write(1, "[ps:cp]", 7);
    memcpy((unsigned char *)pktdrv_bounce_linear, buf, len);
    write(1, "[ps:rm]", 7);
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
    write(1, "[ps:int]", 8);
    unsigned char carry = pktdrv_int_invoke(0x31, dpmi);
    write(1, "[ps:int-done]", 13);
    if (carry || (rm.flags & 1)) {
        return -1;
    }
    return 0;
}

// Drain the RX slot if one is pending. Returns the byte count
// written (clamped to maxlen) or 0 if nothing's queued. Truncates
// silently on overflow.
unsigned int pktdrv_recv(unsigned char *out, unsigned int maxlen) {
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

// True when a Crynwr driver was successfully attached at init.
int pktdrv_is_active(void) { return pktdrv_int_num != 0; }
