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
    unsigned int r2[8] = {0};
    r2[R_EAX] = 0x0002;
    r2[R_EBX] = seg;
    if (pktdrv_int_invoke(0x31, r2)) return -1;
    unsigned int sel = r2[R_EAX] & 0xFFFF;
    unsigned int r3[8] = {0};
    r3[R_EAX] = 0x0006;
    r3[R_EBX] = sel;
    if (pktdrv_int_invoke(0x31, r3)) return -1;
    unsigned int linear = ((r3[R_ECX] & 0xFFFF) << 16) | (r3[R_EDX] & 0xFFFF);

    unsigned char *thunk = (unsigned char *)linear;
    thunk[0] = 0xCD;             // INT
    thunk[1] = 0x21;             // vector 21h
    thunk[2] = 0xCB;             // RETF

    dos_int21_thunk_seg = seg;
    dos_int21_thunk_ready = 1;
    return 0;
}

// Dispatch an INT 21h with the caller-supplied rmcs. CS:IP is forced
// to our thunk; SS:SP=0:0 → DPMI picks a real-mode stack from its
// own pool. On return, rm.flags has the real-mode FLAGS register
// (caller checks CF in bit 0 for the DOS error condition); the rest
// of rm is updated with the post-INT register state.
static int dos_int21_call(dos_rmcs_t *rm) {
    if (dos_int21_thunk_init() != 0) return -1;
    rm->cs = (unsigned short)dos_int21_thunk_seg;
    rm->ip = 0;
    rm->ss = 0;
    rm->sp = 0;
    unsigned int dpmi[8] = {0};
    dpmi[R_EAX] = 0x0301;
    dpmi[R_EBX] = 0;             // INT number not used by 0x0301
    dpmi[R_ECX] = 0;             // word count to copy from PM stack
    dpmi[R_EDI] = (unsigned int)(unsigned long)rm;
    unsigned char carry = pktdrv_int_invoke(0x31, dpmi);
    return carry ? -1 : 0;
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
