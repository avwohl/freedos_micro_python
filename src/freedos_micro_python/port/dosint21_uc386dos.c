// dosint21_uc386dos.c — DPMI-thunked DOS file I/O for uc386-dos.
//
// Why this exists
// ---------------
// uc386's libc (`i386_dos_libc.asm`) implements open/read/write/close
// as direct `int 21h` from protected mode. PMODE/W intercepts INT 21h
// in its IDT and provides "DPMI translation" for the buffer-using
// services (AH=0x3F read, AH=0x40 write, etc.) — it copies the PM
// buffer through a real-mode bounce, executes the real-mode INT 21h,
// copies data back, and returns.
//
// That translation has a deep-stack bug, empirically reproduced with
// a 30-line standalone PM smoke test: with a shallow stack, AH=0x3F
// returns fine; with ~64 KB of stack pad before the call, AH=0x3F
// hangs and never returns. Same class of bug as AH=0x48 (Allocate
// Memory), which we already work around by pre-allocating bounce +
// thunk paragraphs at the top of main() while the stack is shallow
// (see `_preallocate_bounce_buffer` in ports/minimal/main.c).
//
// MicroPython's interpreter naturally pushes a deep stack (recursive
// parser/compiler/eval). By the time a user `f.read()` reaches the
// `_read` shim, we're well past the safe depth, and the int 21h
// silently hangs.
//
// Fix: bypass PMODE/W's translation. Allocate a tiny real-mode thunk
// paragraph containing `CD 21 CB` (INT 21h; RETF) and dispatch it via
// DPMI fn 0x0301 (Call Real Mode Procedure With Far Return). 0x0301
// reliably reaches real-mode code regardless of caller stack depth —
// we already use this exact pattern to drive Crynwr's INT 0x60 from
// PM (see pktdrv_uc386dos.c::pktdrv_call_int60_thunk). The trade-off
// is that buffer translation is now our responsibility: we copy data
// through the pre-allocated bounce buffer the pktdrv code already
// owns.
//
// `dos_int21_open` / `_read` / `_write` / `_close` / `_lseek` /
// `_fstat_size` form the user-visible API. file_uc386dos.c calls
// these instead of libc's POSIX wrappers.
//
// Initialisation
// --------------
// `dos_int21_thunk_seg` is the real-mode segment of our `CD 21 CB`
// paragraph. main.c's startup-shallow allocator pre-allocates it via
// DPMI fn 0x0100 (same as the INT 0x60 thunk does); we just lazily
// fill the bytes the first time the API is called.

#include <string.h>

#include "py/runtime.h"

// ------ externs from pktdrv_uc386dos.c -----------------------------
// We reuse the same DPMI plumbing the packet driver does so we don't
// duplicate the asm wrapper or the rmcs layout. pktdrv_int_invoke is
// the only asm entry point — it patches the INT immediate, dispatches
// INT n with the supplied register block, captures CF + writebacks.
extern unsigned char pktdrv_int_invoke(unsigned int int_num,
                                       unsigned int *regs);

// Pre-allocated by main.c via DPMI 0x0100 while the stack is shallow.
// Layout in conventional memory:
//   pktdrv_preallocated_bounce_seg  : 128-paragraph (2 KB) buffer the
//                                     PM client can fill/read through.
//   pktdrv_preallocated_bounce_linear: flat-32 linear addr that maps
//                                     the same bytes — resolved via
//                                     DPMI fn 0x0002+0x0006 so PM
//                                     writes land where the real-mode
//                                     INT sees them.
extern unsigned int pktdrv_preallocated_bounce_seg;
extern unsigned int pktdrv_preallocated_bounce_linear;

// A SECOND pre-allocated paragraph for our `CD 21 CB` thunk. Distinct
// from the INT 0x60 thunk paragraph so the two stay isolated. main.c
// fills this global at startup or leaves it 0 on alloc failure.
extern unsigned int dos_int21_preallocated_thunk_seg;

// rmcs layout matches DPMI 0.9. Mirror pktdrv's typedef so we don't
// have to pull its header (which doesn't exist; the struct is local
// to pktdrv_uc386dos.c). Field-for-field equal — verified by the
// lwIP packed_struct_test.
typedef struct {
    unsigned int   edi;
    unsigned int   esi;
    unsigned int   ebp;
    unsigned int   reserved;
    unsigned int   ebx;
    unsigned int   edx;
    unsigned int   ecx;
    unsigned int   eax;
    unsigned short flags;
    unsigned short es;
    unsigned short ds;
    unsigned short fs;
    unsigned short gs;
    unsigned short ip;
    unsigned short cs;
    unsigned short sp;
    unsigned short ss;
} __attribute__((packed)) dos_rmcs_t;

#define R_EAX 0
#define R_EBX 1
#define R_ECX 2
#define R_EDX 3
#define R_ESI 4
#define R_EDI 5
#define R_DS  6
#define R_ES  7

static unsigned int dos_int21_thunk_seg = 0;
static int dos_int21_thunk_ready = 0;

// Real-mode stack for the INT 21h call, allocated once at startup.
//
// dos_int21_call used to leave ss:sp = 0:0 in the register block,
// which tells the DPMI host to supply a real-mode stack of its own.
// PMODE/W's is small and shared. AH=0x3F / AH=0x40 against an
// already-open console handle barely touch it — which is exactly why
// printing and the REPL work — but AH=0x3D walks the FAT directory
// chain and drops into BIOS INT 13h on a floppy, which is far
// hungrier. Overflowing the host's stack corrupts whatever sits below
// it and produces precisely what we see: no fault, no return, and a
// failure that depends on timing and on which media is being touched.
//
// 256 paragraphs = 4 KB, which is comfortably more than a DOS file
// open plus a BIOS disk call needs.
static unsigned int dos_rm_stack_seg = 0;
static unsigned int dos_rm_stack_bytes = 0;

static int dos_int21_thunk_init(void) {
    if (dos_int21_thunk_ready) return 0;
    if (dos_int21_preallocated_thunk_seg == 0) return -1;
    unsigned int seg = dos_int21_preallocated_thunk_seg;

    // Resolve the linear address via DPMI 0x0002 (descriptor for
    // real-mode seg) + 0x0006 (selector base). PMODE/W doesn't
    // necessarily identity-map every real-mode paragraph, so a raw
    // `seg << 4` cast can land at a totally different linear page —
    // and our `CD 21 CB` write then doesn't end up at the real-mode
    // address DPMI 0x0301 dispatches to. The pktdrv bounce buffer
    // does the same dance for the same reason (see main.c).
    // Two candidate linear addresses for the paragraph:
    //   (a) the DPMI 0x0002 + 0x0006 dance, and
    //   (b) the plain `seg << 4` identity mapping.
    // Under PMODE/W with a zero-based flat client these agree, but
    // we must not *assume* it: if we write `CD 21 CB` to the wrong
    // linear address, the write silently corrupts whatever lives
    // there AND DPMI 0x0301 then dispatches CS:IP = seg:0000 into an
    // uninitialised real-mode paragraph. Executing garbage in real
    // mode with DOS's structures live is unrecoverable — no fault,
    // no traceback, the machine simply dies (observed as MP.EXE
    // vanishing and FreeCOM reporting "Cannot terminate permanent
    // FreeCOM instance / System halted").
    //
    // So: resolve, write, and then READ BACK through the same
    // pointer. Only a successful read-back sets `..._ready`. If both
    // candidates fail we leave the thunk unarmed, and every caller
    // returns -1 -> a clean Python OSError instead of a dead machine.
    unsigned int linear_dpmi = 0;
    unsigned int r2[8] = {0};
    r2[R_EAX] = 0x0002;
    r2[R_EBX] = seg;
    if (!pktdrv_int_invoke(0x31, r2)) {
        unsigned int sel = r2[R_EAX] & 0xFFFF;
        unsigned int r3[8] = {0};
        r3[R_EAX] = 0x0006;
        r3[R_EBX] = sel;
        if (!pktdrv_int_invoke(0x31, r3)) {
            linear_dpmi = ((r3[R_ECX] & 0xFFFF) << 16) |
                          (r3[R_EDX] & 0xFFFF);
        }
    }
    unsigned int linear_shift = seg << 4;

    unsigned int candidates[2];
    candidates[0] = linear_dpmi;
    candidates[1] = linear_shift;

    for (int i = 0; i < 2; i++) {
        unsigned int linear = candidates[i];
        if (linear == 0) continue;
        if (i == 1 && linear == candidates[0]) continue;  // already tried

        volatile unsigned char *thunk = (volatile unsigned char *)linear;
        thunk[0] = 0xCD;             // INT
        thunk[1] = 0x21;             // vector 21h
        thunk[2] = 0xCB;             // RETF

        // Read back before trusting it. A paragraph we cannot write
        // through is a paragraph we must never set CS:IP to.
        if (thunk[0] == 0xCD && thunk[1] == 0x21 && thunk[2] == 0xCB) {
            dos_int21_thunk_seg = seg;
            dos_int21_thunk_ready = 1;
            return 0;
        }
    }

    return -1;
}

// Pre-init entry called from main() at shallow stack. The DPMI
// 0x0002/0x0006 + thunk-memory write used to run lazily inside the
// first `open()` call, deep inside the MicroPython interpreter
// stack. Empirically that combination wedges in PMODE/W's DPMI
// dispatch when the stack is deep enough (the same family of
// deep-stack sensitivities documented on `_preallocate_bounce_buffer`).
// Running it once at program entry, before MP builds up its stack,
// keeps every subsequent `dos_int21_call` to just the DPMI 0x0301
// real-mode-call gate, which is stack-depth-safe.
// Emit "<label>=XXXXXXXX\n" so the serial log carries real values.
// There is no printf on this path and no debugger on real hardware,
// and every interesting quantity here is an address.
static void dos_dbg_hex(const char *label, unsigned int v) {
    extern int write(int fd, const void *buf, unsigned int n);
    static const char hexd[] = "0123456789abcdef";
    char buf[64];
    unsigned int n = 0;
    while (label[n] && n < 40) { buf[n] = label[n]; n++; }
    buf[n++] = '=';
    for (int s = 28; s >= 0; s -= 4) buf[n++] = hexd[(v >> s) & 0xF];
    buf[n++] = '\n';
    write(1, buf, n);
}

// Establish the flat-32 linear address that aliases the real-mode
// bounce paragraph, and prove it before anything relies on it.
//
// main.c resolves this with DPMI 0x0002 (segment -> descriptor) and
// 0x0006 (descriptor -> linear base) but DISCARDS the carry flag from
// both calls, so a failure silently leaves a wild value in the global.
// Every dos_int21_* call then copies its path/data there while real
// mode reads the actual paragraph — surfacing as a phantom "file not
// found" for a file that plainly exists.
//
// The two source files also disagreed about the memory model: main.c
// asserts "PMODE/W uses paging, seg << 4 is NOT the linear address",
// while dos_int21_open assumed the bounce is "identity-mapped by the
// extender". Rather than pick a side, resolve both, cross-check, and
// log what was chosen so the serial log settles it.
// Base of our own flat data selector, per DPMI fn 0x0006.
//
// DPMI 0x0002/0x0006 report ABSOLUTE linear addresses. A pointer this
// code dereferences is an offset inside the client's flat segment.
// The two coincide only when the client base is zero. If PMODE/W
// gives us a non-zero base, then casting the absolute address of a
// conventional-memory paragraph to a pointer addresses
// base + that_address instead — landing inside our own image, so we
// scribble on ourselves while real mode reads an untouched paragraph.
// That would explain both the nondeterministic wedges and DOS
// answering "file not found" for a file that exists.
extern unsigned int dos_get_ds_selector(void);
void dos_int21_thunk_preinit(void);   // fwd decl: used as a text address below

static unsigned int dos_client_base(void) {
    unsigned int r[8] = {0};
    r[R_EAX] = 0x0006;
    r[R_EBX] = dos_get_ds_selector();
    if (pktdrv_int_invoke(0x31, r)) return 0;
    return ((r[R_ECX] & 0xFFFF) << 16) | (r[R_EDX] & 0xFFFF);
}

static int dos_bounce_resolve(void) {
    unsigned int seg = pktdrv_preallocated_bounce_seg;
    if (seg == 0) return -1;

    // Where does this program actually live, and what is our base?
    // Printed unconditionally: these four numbers decide whether the
    // absolute-vs-relative bug above is real, and there is no other
    // way to find out on hardware.
    dos_dbg_hex("[client:dssel]", dos_get_ds_selector());
    dos_dbg_hex("[client:dsbase]", dos_client_base());
    dos_dbg_hex("[addr:bss]", (unsigned int)(void *)&dos_int21_thunk_seg);
    dos_dbg_hex("[addr:text]", (unsigned int)(void *)&dos_int21_thunk_preinit);

    unsigned int linear_shift = seg << 4;
    unsigned int linear_dpmi = 0;

    unsigned int r2[8] = {0};
    r2[R_EAX] = 0x0002;
    r2[R_EBX] = seg;
    if (!pktdrv_int_invoke(0x31, r2)) {
        unsigned int sel = r2[R_EAX] & 0xFFFF;
        unsigned int r3[8] = {0};
        r3[R_EAX] = 0x0006;
        r3[R_EBX] = sel;
        if (!pktdrv_int_invoke(0x31, r3)) {
            linear_dpmi = ((r3[R_ECX] & 0xFFFF) << 16) |
                          (r3[R_EDX] & 0xFFFF);
        }
    }

    dos_dbg_hex("[bounce:seg]", seg);
    dos_dbg_hex("[bounce:shift]", linear_shift);
    dos_dbg_hex("[bounce:dpmi]", linear_dpmi);
    dos_dbg_hex("[bounce:main]", pktdrv_preallocated_bounce_linear);

    // Prefer an address the two methods agree on; otherwise take the
    // one backed by a successful DPMI resolution. seg << 4 is the
    // fallback of last resort, not a guess: conventional memory under
    // a zero-based flat client is identity-mapped.
    unsigned int chosen;
    if (linear_dpmi != 0) {
        chosen = linear_dpmi;
    } else {
        chosen = linear_shift;
    }

    // Translate the absolute linear address into an offset within our
    // own flat segment. A zero base leaves this untouched, which is
    // the historical behaviour; a non-zero base is precisely the bug
    // described above. Underflow would mean conventional memory sits
    // below our segment base and simply is not reachable through DS —
    // report rather than fabricate a pointer.
    unsigned int base = dos_client_base();
    if (base != 0) {
        if (chosen < base) {
            dos_dbg_hex("[bounce:BELOW-BASE]", base);
            return -1;
        }
        chosen -= base;
        dos_dbg_hex("[bounce:rebased]", chosen);
    }

    // Writable-and-readable only proves the memory exists, not that it
    // aliases the paragraph — but a failure here is decisive.
    volatile unsigned char *b = (volatile unsigned char *)chosen;
    b[0] = 0x55; b[1] = 0xAA;
    if (b[0] != 0x55 || b[1] != 0xAA) {
        dos_dbg_hex("[bounce:UNWRITABLE]", chosen);
        return -1;
    }

    pktdrv_preallocated_bounce_linear = chosen;
    dos_dbg_hex("[bounce:chosen]", chosen);
    return 0;
}

void dos_int21_thunk_preinit(void) {
    // Report the outcome. This used to be discarded, which meant a
    // failed thunk arm was indistinguishable from a good one until
    // the first open() took the machine down. The markers are cheap
    // and they are the only visibility into this path on real
    // hardware, where there is no debugger.
    extern int write(int fd, const void *buf, unsigned int n);
    if (dos_bounce_resolve() != 0) {
        write(1, "[bounce:RESOLVE-FAILED]\n", 24);
    }

    // Allocate the real-mode stack now, at main()'s shallow stack,
    // for the same reason everything else here is pre-allocated:
    // DPMI 0x0100 from a deep interpreter stack is unreliable under
    // PMODE/W.
    // Ask for the biggest block we can get, backing off on refusal.
    // A 1.44 MB FreeDOS boot with COMMAND.COM resident does not leave
    // much conventional memory once the bounce (2 KB) and two thunk
    // paragraphs are taken, and a flat request for 4 KB is refused
    // outright — which silently left this feature disabled.
    {
        static const unsigned int paras[] = {256u, 128u, 64u, 32u};
        for (unsigned int i = 0; i < 4; i++) {
            unsigned int r[8] = {0};
            r[R_EAX] = 0x0100;
            r[R_EBX] = paras[i];
            if (!pktdrv_int_invoke(0x31, r)) {
                dos_rm_stack_seg = r[R_EAX] & 0xFFFF;
                dos_rm_stack_bytes = paras[i] * 16u;
                dos_dbg_hex("[i21stack:seg]", dos_rm_stack_seg);
                dos_dbg_hex("[i21stack:bytes]", dos_rm_stack_bytes);
                break;
            }
        }
        if (dos_rm_stack_seg == 0) {
            write(1, "[i21stack:ALLOC-FAILED]\n", 24);
        }
    }
    if (dos_int21_thunk_init() == 0) {
        write(1, "[i21thunk:armed]\n", 17);
    } else {
        write(1, "[i21thunk:FAILED]\n", 18);
    }
}

// Stack-switching DPMI 0x0301 dispatcher in uc386's libc — switches
// to a dedicated shallow stack before the INT 31h so PMODE/W's int
// 31 handler doesn't choke on a deep PM stack. The deep-stack
// chokehold otherwise wedges open()/read()/write() with no return
// once MicroPython has built up its interpreter stack.
extern unsigned char dpmi0301_call_shallow(void *rmcs);

// Dispatch an INT 21h with the caller-supplied rmcs. CS:IP is forced
// to our thunk; SS:SP=0:0 → DPMI picks a real-mode stack from its
// own pool. On return, rm.flags has the real-mode FLAGS register
// (caller checks CF in bit 0 for the DOS error condition); the rest
// of rm is updated with the post-INT register state.
static int dos_int21_call(dos_rmcs_t *rm) {
    if (dos_int21_thunk_init() != 0) return -1;
    extern int write(int fd, const void *buf, unsigned int n);
    rm->cs = (unsigned short)dos_int21_thunk_seg;
    rm->ip = 0;
    // Point the real-mode call at our own stack when we have one, and
    // only fall back to ss:sp = 0:0 (host-supplied) if the allocation
    // failed. sp starts two bytes below the top so a push cannot wrap
    // the segment.
    if (dos_rm_stack_seg != 0) {
        rm->ss = (unsigned short)dos_rm_stack_seg;
        rm->sp = (unsigned short)(dos_rm_stack_bytes - 2);
    } else {
        rm->ss = 0;
        rm->sp = 0;
    }
    // Enter the real-mode INT 21h with interrupts ENABLED. The rmcs
    // is memset to 0 by every caller, which left IF clear. FreeDOS's
    // INT 21h does STI early so it usually recovers, but a
    // floppy-backed open() reaches BIOS INT 13h, which waits on
    // IRQ 6 — with IF clear that wait never completes.
    // 0x0202 = IF | the x86 always-set bit 1.
    rm->flags = 0x0202;
    // Bracket the gate. Without these, a wedge inside PMODE/W's 0301h
    // handler is indistinguishable from a wedge anywhere else in the
    // call — the difference decides where to look next.
    write(1, "[i21:call]\n", 11);
    unsigned char cf = dpmi0301_call_shallow(rm);
    write(1, "[i21:ret]\n", 10);
    return cf ? -1 : 0;
}

// ------ public API -------------------------------------------------

// `open(path, dos_access_mode)` — INT 21h AH=0x3D.
//   dos_access_mode: 0=read, 1=write, 2=read-write (DOS values, not
//   POSIX O_*). The path must be ASCIIZ.
//
// We copy `path` into the bounce buffer at offset 0 so DS:DX can
// point at it. Returns the DOS file handle on success (0..255) or -1
// on error (caller can check errno via DOS error code in *err_out
// when provided).
int dos_int21_open(const char *path, int dos_access_mode, int *err_out) {
    if (err_out) *err_out = 0;
    if (pktdrv_preallocated_bounce_seg == 0
        || pktdrv_preallocated_bounce_linear == 0) {
        return -1;
    }
    size_t plen = strlen(path);
    if (plen >= 1024) return -1;       // bounce buffer is 2 KB; leave headroom

    // Copy path into bounce (linear write — bounce is below 1 MB,
    // identity-mapped by the extender, so flat-32 store lands where
    // the real-mode side reads).
    unsigned char *bp = (unsigned char *)pktdrv_preallocated_bounce_linear;
    memcpy(bp, path, plen + 1);

    dos_rmcs_t rm;
    memset(&rm, 0, sizeof(rm));
    rm.eax = 0x3D00 | (dos_access_mode & 0xFF);
    rm.ds  = (unsigned short)pktdrv_preallocated_bounce_seg;
    rm.edx = 0;                    // offset within DS

    if (dos_int21_call(&rm) != 0) return -1;
    if (rm.flags & 1) {            // CF set → error; AX has DOS error code
        if (err_out) *err_out = (int)(rm.eax & 0xFFFF);
        return -1;
    }
    return (int)(rm.eax & 0xFFFF);
}

// `read(fd, buf, count)` — INT 21h AH=0x3F. count is clamped to the
// bounce buffer size (~2 KB minus path scratch); callers that want
// more should loop.
//
// The bounce is 128 paragraphs = 2048 bytes; reserve 1024 for path
// scratch (matches the open() limit) and leave 1024 for data.
#define DOS_BOUNCE_DATA_OFFSET 1024
#define DOS_BOUNCE_DATA_MAX    1024

int dos_int21_read(int fd, void *buf, unsigned int count, int *err_out) {
    if (err_out) *err_out = 0;
    if (pktdrv_preallocated_bounce_seg == 0
        || pktdrv_preallocated_bounce_linear == 0) {
        return -1;
    }
    if (count == 0) return 0;
    if (count > DOS_BOUNCE_DATA_MAX) count = DOS_BOUNCE_DATA_MAX;

    dos_rmcs_t rm;
    memset(&rm, 0, sizeof(rm));
    rm.eax = 0x3F00;
    rm.ebx = (unsigned int)fd;
    rm.ecx = count;
    rm.ds  = (unsigned short)pktdrv_preallocated_bounce_seg;
    rm.edx = DOS_BOUNCE_DATA_OFFSET;

    if (dos_int21_call(&rm) != 0) return -1;
    if (rm.flags & 1) {
        if (err_out) *err_out = (int)(rm.eax & 0xFFFF);
        return -1;
    }
    unsigned int n = rm.eax & 0xFFFF;
    if (n > count) n = count;      // defensive — DOS shouldn't exceed CX
    if (n > 0) {
        unsigned char *bp = (unsigned char *)pktdrv_preallocated_bounce_linear;
        memcpy(buf, bp + DOS_BOUNCE_DATA_OFFSET, n);
    }
    return (int)n;
}

int dos_int21_write(int fd, const void *buf, unsigned int count, int *err_out) {
    if (err_out) *err_out = 0;
    if (pktdrv_preallocated_bounce_seg == 0
        || pktdrv_preallocated_bounce_linear == 0) {
        return -1;
    }
    if (count == 0) return 0;
    if (count > DOS_BOUNCE_DATA_MAX) count = DOS_BOUNCE_DATA_MAX;

    unsigned char *bp = (unsigned char *)pktdrv_preallocated_bounce_linear;
    memcpy(bp + DOS_BOUNCE_DATA_OFFSET, buf, count);

    dos_rmcs_t rm;
    memset(&rm, 0, sizeof(rm));
    rm.eax = 0x4000;
    rm.ebx = (unsigned int)fd;
    rm.ecx = count;
    rm.ds  = (unsigned short)pktdrv_preallocated_bounce_seg;
    rm.edx = DOS_BOUNCE_DATA_OFFSET;

    if (dos_int21_call(&rm) != 0) return -1;
    if (rm.flags & 1) {
        if (err_out) *err_out = (int)(rm.eax & 0xFFFF);
        return -1;
    }
    return (int)(rm.eax & 0xFFFF);
}

int dos_int21_close(int fd, int *err_out) {
    if (err_out) *err_out = 0;
    dos_rmcs_t rm;
    memset(&rm, 0, sizeof(rm));
    rm.eax = 0x3E00;
    rm.ebx = (unsigned int)fd;
    if (dos_int21_call(&rm) != 0) return -1;
    if (rm.flags & 1) {
        if (err_out) *err_out = (int)(rm.eax & 0xFFFF);
        return -1;
    }
    return 0;
}

// `lseek(fd, offset, whence)` — INT 21h AH=0x42. DOS whence: 0=SET,
// 1=CUR, 2=END. Returns the new absolute file position, or -1.
long dos_int21_lseek(int fd, long offset, int whence, int *err_out) {
    if (err_out) *err_out = 0;
    dos_rmcs_t rm;
    memset(&rm, 0, sizeof(rm));
    rm.eax = 0x4200 | (whence & 0xFF);
    rm.ebx = (unsigned int)fd;
    // CX:DX = offset (high:low). DOS interprets as a 32-bit signed
    // displacement.
    rm.ecx = ((unsigned long)offset >> 16) & 0xFFFF;
    rm.edx = (unsigned long)offset & 0xFFFF;
    if (dos_int21_call(&rm) != 0) return -1;
    if (rm.flags & 1) {
        if (err_out) *err_out = (int)(rm.eax & 0xFFFF);
        return -1;
    }
    // Result is DX:AX (high:low).
    return (long)(((rm.edx & 0xFFFF) << 16) | (rm.eax & 0xFFFF));
}

// Helper: file size via lseek-to-end + lseek-back-to-current. Used
// by mp_lexer_new_from_file to size the read buffer.
long dos_int21_fsize(int fd) {
    int err = 0;
    long cur = dos_int21_lseek(fd, 0, 1, &err);   // SEEK_CUR
    if (cur < 0) return -1;
    long end = dos_int21_lseek(fd, 0, 2, &err);   // SEEK_END
    if (end < 0) return -1;
    (void)dos_int21_lseek(fd, cur, 0, &err);      // SEEK_SET → restore
    return end;
}
