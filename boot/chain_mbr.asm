; SPDX-License-Identifier: GPL-3.0-only
; ============================================================
; chain_mbr.asm -- a minimal "boot the active primary partition" MBR (446 bytes of code).
;
; os-inst writes this into the first 446 bytes of a disk's sector 0 ONLY when that area is completely blank
; (no boot code of any kind), so that a MiniWin installed into a partition of an otherwise empty disk can start.
; A disk that already has a boot loader keeps it -- os-inst never overwrites someone else's boot code.
;
; What it does (the classic MBR job):
;   1. copy itself from 0x7C00 to 0x0600 (so the partition's own boot record can be loaded at 0x7C00),
;   2. find the first partition-table entry (at offset 446 of this sector) whose status byte is 0x80 ("active"),
;   3. read that partition's first sector (its boot record, which for MiniWin is boot/boot.asm with its
;      install_base field patched to this partition's start) to 0x7C00 with INT 13h extensions,
;   4. check its 0xAA55 signature and jump to it with DL = BIOS drive and DS:SI -> the partition entry.
; The partition table (446-509) and the 0xAA55 signature are NOT part of these 446 bytes: os-inst keeps whatever
; table the disk already has.
; ============================================================
BITS 16
ORG 0x0600

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    sti
    mov si, 0x7C00            ; copy ourselves out of the way: the partition's boot record wants 0x7C00
    mov di, 0x0600
    mov cx, 256
    cld
    rep movsw
    jmp 0x0000:relocated

relocated:
    mov [bootdrv], dl
    mov si, 0x0600 + 446      ; the partition table, in the relocated copy
    mov cx, 4
.find:
    cmp byte [si], 0x80
    je .found
    add si, 16
    loop .find
    mov si, msg_noactive
    jmp fail

.found:
    mov [partptr], si
    mov eax, [si + 8]         ; the partition's first sector (LBA)
    mov [dap_lba], eax
    mov si, dap
    mov dl, [bootdrv]
    mov ah, 0x42
    int 0x13
    mov si, msg_readerr
    jc fail
    cmp word [0x7DFE], 0xAA55
    mov si, msg_badsig
    jne fail
    mov si, [partptr]
    mov dl, [bootdrv]
    jmp 0x0000:0x7C00

fail:
    lodsb
    test al, al
    jz .hang
    mov ah, 0x0E
    mov bx, 0x0007
    int 0x10
    jmp fail
.hang:
    hlt
    jmp .hang

bootdrv: db 0
partptr: dw 0
align 4
dap:
    db 0x10, 0
    dw 1                      ; one sector
    dw 0x7C00, 0x0000         ; to 0000:7C00
dap_lba:
    dd 0, 0

; Named like every other boot error (docs/dev/BOOT-PROCESS.md, "Error codes"): MW-C* come from this partition chain loader.
msg_noactive: db "MiniWin ERROR MW-C01 NO_ACTIVE_PARTITION", 0
msg_readerr:  db "MiniWin ERROR MW-C02 PARTITION_READ_FAILED", 0
msg_badsig:   db "MiniWin ERROR MW-C03 PARTITION_NOT_BOOTABLE", 0

times 446 - ($ - $$) db 0
