; ============================================================
; MiniWin Bootloader -- Stage 1 (the actual MBR)
; - This is the very first code that runs. Ever. On the whole machine.
;   BIOS drops us here at 0x7C00 in ancient 16-bit real mode and just
;   trusts us to figure the rest out. No safety net, no OS, nothing.
; - Its ONLY job: load stage 2 (boot/stage2.asm) off disk and jump to
;   it. That's it. A classic MBR is capped at exactly 512 bytes (510
;   bytes of code/data plus the mandatory 0xAA55 signature), which
;   stopped being enough room the moment this bootloader needed to
;   walk the BIOS's real VBE mode list looking for a true 640x480x32bpp
;   mode instead of just requesting a fixed, hoped-for mode number --
;   see stage2.asm's own header for why that's a meaningfully bigger
;   piece of code than picking a number and hoping. Splitting into two
;   stages is the standard, decades-old way around the 512-byte
;   ceiling; stage 2 has no such limit; everything that used to live
;   here (kernel loading, VBE, A20, GDT, the jump into protected mode)
;   now lives there instead.
; ============================================================
BITS 16
ORG 0x7C00

BOOT_DRIVE_ADDR equ 0x0500   ; must match stage2.asm's own copy of this
                             ; constant -- see stage2.asm's comment on
                             ; it for why a fixed shared address, not
                             ; passing the value through a register or
                             ; introspecting this file's own layout, is
                             ; how the boot drive number crosses the
                             ; handoff between the two stages
STAGE2_LOAD_SEG equ 0x0000
STAGE2_LOAD_OFF equ 0x0600  ; classic safe low-memory address: past the
                             ; IVT/BIOS Data Area (which end around
                             ; 0x0500), comfortably below this stage's
                             ; own 0x7C00 and its stack

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    sti

    mov [BOOT_DRIVE_ADDR], dl   ; BIOS hands us the boot drive number in dl;
                                ; stash it where stage 2 knows to look for it

    mov si, msg_boot
    call print_string

    ; ---- Load stage 2 from disk (BIOS INT13h extended/LBA read) ----
    ; One single read: STAGE2_SECTORS sectors starting at LBA 1 (LBA 0
    ; is this sector), landing at 0x0000:0x0600. STAGE2_SECTORS comes in
    ; via -D from build.sh, which is also what pads stage 2's own
    ; assembled binary out to that many sectors on disk and what tells
    ; stage2.asm itself (also via -D) where the kernel starts -- one
    ; number, defined once by the build script, instead of three
    ; hand-synchronized copies.
    mov word [dap_sectors], STAGE2_SECTORS
    mov word [dap_lba], 1
    mov word [dap_lba+2], 0
    mov word [dap_segment], STAGE2_LOAD_SEG
    mov si, dap
    mov dl, [BOOT_DRIVE_ADDR]
    mov ah, 0x42
    int 0x13
    jc disk_error

    ; Off to stage 2 -- a near jump (still real mode, same segment) to
    ; the address we just loaded it at.
    jmp STAGE2_LOAD_SEG:STAGE2_LOAD_OFF

disk_error:
    ; Something about the disk read went sideways. We don't have the
    ; luxury of a stack trace or a debugger at this point in the universe's
    ; existence, so all we can do is print this and freeze. Sorry.
    mov si, msg_disk_err
    call print_string
    jmp $

; ---- 16-bit helper: print null-terminated string at SI via BIOS teletype ----
print_string:
    pusha
.loop:
    lodsb
    or al, al
    jz .done
    mov ah, 0x0E
    mov bh, 0
    int 0x10
    jmp .loop
.done:
    popa
    ret

msg_boot:      db "MiniWin: booting...", 13, 10, 0
msg_disk_err:  db "MiniWin: DISK READ ERROR", 13, 10, 0

; ============================================================
; Disk Address Packet for the one-shot stage-2 read above. Standard
; 16-byte INT13h AH=42h format: size, reserved, sector count, dest
; offset, dest segment, then an 8-byte starting LBA (only the low word
; is ever touched -- nowhere close to needing the upper bits at this
; disk image's size).
; ============================================================
align 4
dap:
    db 0x10, 0
dap_sectors: dw 0
             dw STAGE2_LOAD_OFF
dap_segment: dw 0
dap_lba:     dq 0

; ============================================================
; Boot sector padding + the magic signature that tells the BIOS
; "yes, this is actually bootable, I promise"
; ============================================================
times 510-($-$$) db 0
dw 0xAA55
