; ============================================================
; Kernel entry stub -- this MUST be the very first code sitting at
; 0x10000, because that's the address we pinky-promised the bootloader
; we'd jump to. Its whole job in life is: clean up after the bootloader's
; mess, then call kmain() and get out of the way.
; ============================================================
BITS 32
[extern kmain]
[extern _bss_start]
[extern _bss_end]

section .text
global _start
_start:
    ; Zero out .bss before C gets its hands on anything. The bootloader
    ; only ever copies raw bytes off disk -- it has no idea what BSS even
    ; is -- so every static buffer (looking at you, 64KB video back
    ; buffer, and you, giant Hangul glyph table) would otherwise start
    ; out full of whatever random garbage happened to be sitting in RAM.
    ; Skip this step and enjoy your haunted framebuffer.
    mov edi, _bss_start
    mov ecx, _bss_end
    sub ecx, edi
    xor eax, eax
    cld
    rep stosb

    call kmain
.hang:
    ; kmain() should never actually return (it's an infinite loop), but
    ; just in case reality has other plans, halt forever instead of
    ; running off into undefined memory and causing chaos.
    cli
    hlt
    jmp .hang
