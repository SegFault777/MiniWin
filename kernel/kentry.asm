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
    ; Move the stack out of low memory BEFORE any C runs. The bootloader
    ; left ESP at 0x9FC00 (the top of conventional memory), but the
    ; kernel's .bss now ends at ~0x9B318 -- an 18KB gap -- while a single
    ; tls_write_record() frame is 33KB. So the stack gets its own 512KB
    ; slab of extended memory (0x100000-0x17FFFF, growing down from
    ; 0x180000). This constant is a hand-kept copy of MW_STACK_TOP in
    ; kernel/memmap.h -- an assembler can't read a C header, so if one
    ; changes, change the other. (A20 was enabled in stage2.asm long
    ; before we got here, and vga_verify_memory_safe() (in kmain)
    ; insists on >9MB of RAM before drawing anything, so this address is real, mapped RAM.)
    ; Only do it if the BIOS probe (stage2.asm left the total in KB at
    ; 0x9200; 0 means "couldn't tell") says there's at least 2MB of RAM,
    ; so a machine that's too small still reaches kmain()'s polite
    ; refusal (vga_verify_memory_safe) on the old low stack instead of
    ; crashing here on RAM that doesn't exist.
    cmp dword [0x9200], 2048
    jb .keep_low_stack
    mov esp, 0x180000
.keep_low_stack:

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
