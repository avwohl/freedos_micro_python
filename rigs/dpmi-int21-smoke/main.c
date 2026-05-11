/* dpmi_int21_smoke — standalone PM smoke test for our DPMI-thunked
 * DOS INT 21h dispatch. Lets us iterate on the thunk path WITHOUT
 * rebuilding MicroPython (which takes 14 min).
 *
 * Stages, each gated on the prior one. Markers go to stdout via
 * INT 21h AH=0x40 (write) — that path we know works.
 *
 *   [s0]  DPMI fn 0x0100 alloc 1 paragraph (real-mode DOS memory)
 *   [s1]  DPMI fn 0x0002 + 0x0006 → linear address of the paragraph
 *   [s2]  Write `90 CB` (NOP, RETF) to the paragraph at linear+0
 *   [s3]  DPMI fn 0x0301 dispatch the thunk — should return cleanly
 *         (proves DPMI 0x0301 works from PM, NO INT involved)
 *   [s4]  Rewrite the paragraph with `B4 2A CD 21 CB` (mov ah,2A;
 *         int 21h; retf) — INT 21h AH=2A is "get date" which has
 *         NO buffer translation. Dispatch via 0x0301. On return
 *         rmcs.ecx should hold the current year. Proves the thunk
 *         can call DOS at all.
 *   [s5]  Rewrite with `CD 21 CB` and dispatch AH=0x3D (open).
 *         DS:DX = paragraph_seg:offset_of_path. Path lives in a
 *         second paragraph that we also allocate + resolve.
 *   [s6]  AH=0x3F (read) — DS:DX = second_paragraph:1024 (data
 *         region). On return ecx_low (= AX) is bytes_read.
 *
 * Iteration: edit, `make` (~30 sec), run rig (~10 sec). 50x faster
 * than the MP debug loop.
 */

#include <string.h>

/* uc386 libc provides this in i386_dos_libc.asm. Same wrapper the
 * port code uses; we share it so behavior is identical. */
extern unsigned char pktdrv_int_invoke(unsigned int int_num,
                                       unsigned int *regs);

#define R_EAX 0
#define R_EBX 1
#define R_ECX 2
#define R_EDX 3
#define R_ESI 4
#define R_EDI 5
#define R_DS  6
#define R_ES  7

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
} __attribute__((packed)) rmcs_t;

/* INT 21h AH=0x40 write to fd=1 (stdout). Same path the libc shim
 * uses; we know it works because we can see these markers in the
 * QEMU serial log. */
extern int write(int fd, const void *buf, unsigned int n);

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

/* DPMI fn 0x0100: Allocate DOS Memory. Returns real-mode segment in
 * AX, selector in DX, paragraphs-available in BX on failure.
 * Returns -1 on failure, segment on success. */
static int dpmi_alloc_dos_mem(unsigned int paragraphs) {
    unsigned int regs[8] = {0};
    regs[R_EAX] = 0x0100;
    regs[R_EBX] = paragraphs;
    if (pktdrv_int_invoke(0x31, regs)) return -1;
    return (int)(regs[R_EAX] & 0xFFFF);
}

/* Resolve flat-32 linear address of a real-mode segment via DPMI
 * 0x0002 (allocate descriptor for segment) + 0x0006 (get selector
 * base). Returns linear or 0 on failure. */
static unsigned int dpmi_seg_to_linear(unsigned int seg) {
    unsigned int r2[8] = {0};
    r2[R_EAX] = 0x0002;
    r2[R_EBX] = seg;
    if (pktdrv_int_invoke(0x31, r2)) return 0;
    unsigned int sel = r2[R_EAX] & 0xFFFF;
    unsigned int r3[8] = {0};
    r3[R_EAX] = 0x0006;
    r3[R_EBX] = sel;
    if (pktdrv_int_invoke(0x31, r3)) return 0;
    unsigned int hi = r3[R_ECX] & 0xFFFF;
    unsigned int lo = r3[R_EDX] & 0xFFFF;
    return (hi << 16) | lo;
}

/* DPMI fn 0x0301: Call Real Mode Procedure With Far Return. Loads
 * rm.cs/ip and switches to V86 at that address with the rest of
 * rm.* loaded into real-mode registers. On return rm is updated. */
static int dpmi_call_rm(rmcs_t *rm) {
    unsigned int regs[8] = {0};
    regs[R_EAX] = 0x0301;
    regs[R_EBX] = 0;
    regs[R_ECX] = 0;
    regs[R_EDI] = (unsigned int)(unsigned long)rm;
    if (pktdrv_int_invoke(0x31, regs)) return -1;
    return 0;
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    emit("[smoke:start]\n");

    /* Stage 0: alloc thunk paragraph */
    int thunk_seg = dpmi_alloc_dos_mem(1);
    emit("[s0:thunk_seg=]");
    emit_hex8((unsigned int)thunk_seg);
    if (thunk_seg < 0) { emit("[s0:FAIL]\n"); return 1; }

    /* Stage 1: resolve linear */
    unsigned int thunk_lin = dpmi_seg_to_linear((unsigned int)thunk_seg);
    emit("[s1:thunk_lin=]");
    emit_hex8(thunk_lin);
    if (thunk_lin == 0) { emit("[s1:FAIL]\n"); return 1; }

    /* Stage 2: write 90 CB (nop + retf) and verify the bytes */
    unsigned char *t = (unsigned char *)thunk_lin;
    t[0] = 0x90;             // NOP
    t[1] = 0xCB;             // RETF
    unsigned int rb = ((unsigned int)t[0] << 8) | t[1];
    emit("[s2:thunk_rb=]");
    emit_hex8(rb);

    /* Stage 3: dispatch the do-nothing thunk via DPMI 0x0301. If
     * this hangs, DPMI 0x0301 itself is broken (or our rmcs is). */
    rmcs_t rm;
    memset(&rm, 0, sizeof(rm));
    rm.cs = (unsigned short)thunk_seg;
    rm.ip = 0;
    rm.ss = 0;
    rm.sp = 0;
    emit("[s3:pre-dispatch]\n");
    if (dpmi_call_rm(&rm) != 0) {
        emit("[s3:DISPATCH-FAIL]\n");
        return 1;
    }
    emit("[s3:post-dispatch]\n");

    /* Stage 4: real-mode INT 21h AH=2A (get date). 5-byte thunk:
     *   B4 2A    mov ah, 2Ah
     *   CD 21    int 21h
     *   CB       retf
     * After return rm.ecx (low word) = year, dh = month, dl = day. */
    t[0] = 0xB4; t[1] = 0x2A;
    t[2] = 0xCD; t[3] = 0x21;
    t[4] = 0xCB;
    memset(&rm, 0, sizeof(rm));
    rm.cs = (unsigned short)thunk_seg;
    rm.ip = 0;
    emit("[s4:pre-dispatch]\n");
    if (dpmi_call_rm(&rm) != 0) {
        emit("[s4:DISPATCH-FAIL]\n");
        return 1;
    }
    emit("[s4:post-dispatch]\n");
    emit("[s4:cx=]");
    emit_hex8(rm.ecx & 0xFFFF);  // year
    emit("[s4:dx=]");
    emit_hex8(rm.edx & 0xFFFF);  // month:day

    /* Stage 5: real-mode INT 21h AH=3D (open). Stash the path in the
     * thunk paragraph itself at offset 3 (just past `CD 21 CB`) — the
     * thunk paragraph is the only memory we *know* maps identically
     * between PM linear and V86 physical (proven by s3+s4). The
     * higher-segment paragraph DPMI returned for a 64-paragraph
     * alloc lives at 0x31C30 PM linear, but V86 reads from real-mode
     * 0x31C3:0 don't actually see those bytes — different page-map. */
    t[0] = 0xCD; t[1] = 0x21; t[2] = 0xCB;
    const char *fname = "TESTCA.PEM";
    int flen = 0;
    while (fname[flen]) { t[3 + flen] = (unsigned char)fname[flen]; flen++; }
    t[3 + flen] = 0;                          // ASCIIZ terminator

    memset(&rm, 0, sizeof(rm));
    rm.cs = (unsigned short)thunk_seg;
    rm.ip = 0;
    rm.eax = 0x3D00;                          // AH=3D AL=0 (read-only)
    rm.ds  = (unsigned short)thunk_seg;
    rm.edx = 3;                               // offset of path inside thunk seg
    emit("[s5:pre-dispatch]\n");
    if (dpmi_call_rm(&rm) != 0) {
        emit("[s5:DISPATCH-FAIL]\n");
        return 1;
    }
    emit("[s5:post-dispatch]\n");
    emit("[s5:flags=]");
    emit_hex8(rm.flags);
    emit("[s5:ax=]");
    emit_hex8(rm.eax & 0xFFFF);
    if (rm.flags & 1) {
        emit("[s5:open-error]\n");
        return 1;
    }
    int fd = (int)(rm.eax & 0xFFFF);
    emit("[s5:fd=]");
    emit_hex8((unsigned int)fd);

    /* Stage 6: real-mode INT 21h AH=3F (read N bytes). Reuse the
     * thunk paragraph: bytes 0..2 stay as CD 21 CB, bytes 3..15 are
     * 13 free bytes for path/data scratch. Path bytes from s5 still
     * sit at thunk[3..]; that's harmless because read writes there.
     * Verify thunk[0..2] is still CD 21 CB (in case something
     * corrupted it). */
    t[0] = 0xCD; t[1] = 0x21; t[2] = 0xCB;
    unsigned int t012 = ((unsigned int)t[0] << 16)
                      | ((unsigned int)t[1] << 8)
                      |  (unsigned int)t[2];
    emit("[s6:thunk012=]");
    emit_hex8(t012);

    /* Try 1 byte first — if AH=3F itself hangs, even 1 byte hangs. */
    memset(&rm, 0, sizeof(rm));
    rm.cs = (unsigned short)thunk_seg;
    rm.ip = 0;
    rm.eax = 0x3F00;
    rm.ebx = (unsigned int)fd;
    rm.ecx = 1;
    rm.ds  = (unsigned short)thunk_seg;
    rm.edx = 3;
    emit("[s6:pre-dispatch]\n");
    if (dpmi_call_rm(&rm) != 0) {
        emit("[s6:DISPATCH-FAIL]\n");
        return 1;
    }
    emit("[s6:post-dispatch]\n");
    emit("[s6:flags=]");
    emit_hex8(rm.flags);
    emit("[s6:ax=]");
    emit_hex8(rm.eax & 0xFFFF);
    if (rm.flags & 1) {
        emit("[s6:read-error]\n");
        return 1;
    }
    unsigned int n = rm.eax & 0xFFFF;
    if (n > 13) n = 13;
    emit("[s6:bytes=]");
    write(1, (const char *)(t + 3), n);
    emit("\n[smoke:PASS]\n");

    /* close(fd) — AH=3E, no buffer. */
    t[0] = 0xCD; t[1] = 0x21; t[2] = 0xCB;
    memset(&rm, 0, sizeof(rm));
    rm.cs = (unsigned short)thunk_seg;
    rm.ip = 0;
    rm.eax = 0x3E00;
    rm.ebx = (unsigned int)fd;
    dpmi_call_rm(&rm);

    return 0;
}
