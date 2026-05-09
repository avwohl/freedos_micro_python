; ============================================================
; Minimal MP.EXE-shaped DOSBox-X PMODE/W INT 21h bug repro:
; the user-code half of a two-obj LE binary built via the
; exe.py bridge.
;
; Mimics MP.EXE BYTE-FOR-BYTE at _main and _write so the bridge's
; existing diagnostic markers ([main+0], [main+4], [main+7],
; [direct-write], [stack-write], [mainlike]) report the same
; values they do when running MP.EXE — making any deviation here
; clearly attributable to the test infrastructure rather than
; the user code.
;
; MP.EXE _main bytes (per README.md):
;   c8 04 00 00       enter 4, 0
;   6a 12             push 18
;   68 a3 27 02 00    push msg_addr
;   6a 01             push 1
;   e8 bd 2e 00 00    call _write     ; rel32 = 0x2ebd
;   ...
;
; MP.EXE _write bytes:
;   55 89 e5 8b 5d 08 8b 55 0c 8b 4d 10 b4 40 cd 21
;   = push ebp / mov ebp,esp / mov ebx,[ebp+8] /
;     mov edx,[ebp+12] / mov ecx,[ebp+16] /
;     mov ah,0x40 / int 0x21
;
; Build:
;   python -m addons.harness.exe \
;       addons/gnu/micropython/dosbox-x-rig/pmwbug/pmwbug_user.asm \
;       -o addons/gnu/micropython/dosbox-x-rig/pmwbug/PMWBUG2.EXE
; ============================================================

        bits    32

        section _TEXT use32 class=CODE

        global  _main
        global  _write

; Padding to push _main + _write to high VA inside user.obj.
; The bridge stub is ~2 KB; with 2 MB of padding here, _main
; lands at ~2 MB into the code segment — same neighborhood as
; MP.EXE's _main at runtime VA ~0x1c3000.
        times   2 * 1024 * 1024 db 0x90 ; nop

; ------------------------------------------------------------
; _main: enter / push count / push str / push fd / call _write.
; Same byte layout as MP.EXE's main().
; ------------------------------------------------------------

_main:
        enter   4, 0                    ; c8 04 00 00 (4 bytes)
        push    dword msg_high_len      ; 6a 12       (2 bytes)
        push    msg_high                 ; 68 imm32   (5 bytes)
        push    1                       ; 6a 01       (2 bytes)
        call    _write                   ; e8 rel32   (5 bytes)
        leave
        ret

; ------------------------------------------------------------
; _write(fd, buf, count): byte-for-byte clone of MP.EXE's _write
; (uc386's libc-emitted write()). cdecl, no stack frame for the
; INT 21h itself — args read directly via [ebp+...].
; ------------------------------------------------------------

_write:
        push    ebp                      ; 55
        mov     ebp, esp                 ; 89 e5
        mov     ebx, [ebp + 8]           ; 8b 5d 08
        mov     edx, [ebp + 12]          ; 8b 55 0c
        mov     ecx, [ebp + 16]          ; 8b 4d 10
        mov     ah, 0x40                 ; b4 40
        int     0x21                     ; cd 21
        leave
        ret

; ------------------------------------------------------------
; BSS to match MP.EXE's substantial uninitialized data layout.
; ------------------------------------------------------------

        section _BSS use32 class=BSS
        global  _bss_zero_start
        global  _bss_zero_end
_bss_zero_start:
_bss_filler:
        resb    1 * 1024 * 1024
_bss_zero_end:

; ------------------------------------------------------------
; Marker string. Same length and starting bytes ("[mp-") as
; MP.EXE's "[mp-main-entered]\n" for parity with bridge tests.
; ------------------------------------------------------------

        section _DATA use32 class=DATA

msg_high:
        db      "[mp-main-entered]", 10
msg_high_len equ $ - msg_high
