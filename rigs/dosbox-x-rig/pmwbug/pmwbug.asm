; ============================================================
; Minimal DOSBox-X PMODE/W INT 21h AH=0x40 caller-EIP bug repro
; ============================================================
;
; Standalone NASM 32-bit source — no external dependencies. The
; binary has TWO write call paths:
;
;   1. Baseline:  _start                       -> _writer_low  -> INT 21h
;                 (low CS:EIP throughout)
;
;   2. Failing:   _start -> _main_high (HIGH)  -> _writer_high -> INT 21h
;                 (call instruction in _main at high CS:EIP)
;
; Both paths pass identical fd / buffer-content-category / count
; into the INT 21h. Under DOSBox-X 2026.05.02 the second path
; produces NUL bytes; under classic DOSBox 0.74-3, QEMU+FreeDOS,
; or real DOS hardware both paths produce the expected text.
;
; Structure mimics MP.EXE (the originally observed binary): a
; small entry stub at low VA that does "bridge" duty, calls into
; a large user code region at high VA, which then calls a write
; helper. The bug only fires when this chain is at depth >= 3
; AND the call instruction in the depth-2 frame (here _main_high)
; sits at high CS:EIP inside a >= ~1 MB code object.
;
; Build (Linux, with Open Watcom V2 + NASM):
;   ./build.sh
;
; Run, capture stdout via DOS redirection so byte values are
; preserved (terminals may render NUL bytes as nothing):
;   PMWBUG.EXE > OUT.TXT
;
; Expected (classic DOSBox / QEMU+FreeDOS / real DOS):
;   [low caller (baseline)]
;   [high caller (test)]
;
; Actual (DOSBox-X):
;   [low caller (baseline)]
;   <count NUL bytes; no newline>
; ============================================================

        bits    32

        section _TEXT use32 class=CODE

        global  _start

; ------------------------------------------------------------
; _start: entry point set via wlink `option start=_start`. PMODE/W
; enters in flat 32-bit protected mode with SS:ESP set up.
; ------------------------------------------------------------

_start:
        ; (a) Baseline path: _start -> _writer_low -> INT 21h. All
        ;     three frames at low CS:EIP.
        push    dword msg_low_len
        push    msg_low
        push    1                       ; fd = stdout
        call    _writer_low
        add     esp, 12

        ; (b) Failing path: _start -> _main_high -> _writer_high
        ;     -> INT 21h. The depth-2 frame (_main_high) sits at
        ;     HIGH CS:EIP. _writer_high is right next to _main_high
        ;     so its EIP is also high — but the WORKING bridge -> _write
        ;     case in MP.EXE also has _write at high VA, so high _writer
        ;     EIP alone isn't sufficient to trigger the bug.
        call    _main_high

        ; Exit cleanly.
        mov     ax, 0x4c00
        int     0x21

; ------------------------------------------------------------
; _writer_low: write helper at LOW CS:EIP. Used by the baseline
; path. Identical body to _writer_high below.
; ------------------------------------------------------------

_writer_low:
        push    ebp
        mov     ebp, esp
        push    ebx
        mov     ebx, [ebp + 8]          ; fd
        mov     edx, [ebp + 12]         ; buffer (flat linear addr)
        mov     ecx, [ebp + 16]         ; count
        mov     ah, 0x40
        int     0x21
        pop     ebx
        leave
        ret

; ------------------------------------------------------------
; Padding to push _main_high + _writer_high to HIGH CS:EIP.
;
; 2 MB exceeds the observed-failing threshold (MP.EXE's _main
; lives at ~0x1c3000, ~1.8 MB into the code segment) and pushes
; the next call site well clear of any 256/512 KB boundary
; uncertainty.
; ------------------------------------------------------------

        times   2 * 1024 * 1024 db 0x90 ; nop

; ------------------------------------------------------------
; _main_high: high-CS:EIP function called from _start. It calls
; _writer_high to do the actual INT 21h. The CALL instruction in
; this function (just after the prolog) is the high-CS:EIP
; caller that triggers the bug.
; ------------------------------------------------------------

_main_high:
        push    ebp
        mov     ebp, esp

        push    dword msg_high_len
        push    msg_high
        push    1                       ; fd = stdout
        call    _writer_high
        add     esp, 12

        leave
        ret

; ------------------------------------------------------------
; _writer_high: write helper at HIGH CS:EIP. Identical body to
; _writer_low. Both helpers share the same _writer-style frame.
; ------------------------------------------------------------

_writer_high:
        push    ebp
        mov     ebp, esp
        push    ebx
        mov     ebx, [ebp + 8]          ; fd
        mov     edx, [ebp + 12]         ; buffer
        mov     ecx, [ebp + 16]         ; count
        mov     ah, 0x40
        int     0x21
        pop     ebx
        leave
        ret

; ------------------------------------------------------------
; BSS: 1 MB to mimic MP.EXE's substantial uninitialized data.
; The LE loader allocates this at runtime; PMODE/W's view of the
; flat address space depends on it.
; ------------------------------------------------------------

        section _BSS use32 class=BSS
        global  _bss_filler
_bss_filler:
        resb    1 * 1024 * 1024

; ------------------------------------------------------------
; Test strings. Both the same length so DOSBox-X's truncation
; logic at dos.cpp:2143 (which keys on count + reg_dx > 0xFFFF
; AND reg_dx & 0xF != 0) doesn't differentiate them.
; ------------------------------------------------------------

        section _DATA use32 class=DATA

msg_low:
        db      "[low caller (baseline)]", 10
msg_low_len equ $ - msg_low

msg_high:
        db      "[high caller (test)]   ", 10
msg_high_len equ $ - msg_high
