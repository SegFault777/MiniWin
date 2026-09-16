; ============================================================
; MiniWin Bootloader (Stage 1)
; - This is the very first code that runs. Ever. On the whole machine.
;   BIOS drops us here at 0x7C00 in ancient 16-bit real mode and just
;   trusts us to figure the rest out. No safety net, no OS, nothing.
; - Loads the kernel from disk
; - Wrestles the CPU into 32-bit protected mode
; - Jumps into the C kernel and washes its hands of the whole affair
; ============================================================
BITS 16
ORG 0x7C00

KERNEL_LOAD_SEG   equ 0x1000      ; kernel lands at physical 0x10000 (seg 0x1000, off 0)
KERNEL_CHUNKS             equ 5   ; 5 x 64 sectors = 320 sectors = 160KB budget --
                                  ; shrunk down from 512KB now that -Os and
                                  ; --gc-sections (see build.sh) keep the real
                                  ; kernel around 118KB; 160KB leaves ~35%
                                  ; headroom for what's still coming (TCP, an
                                  ; HTTP client) without dragging the whole
                                  ; disk image's minimum size up with it. See
                                  ; kernel/fs.h for how this budget lines up
                                  ; with where the file-storage slots start.
KERNEL_SECTORS_PER_CHUNK  equ 64

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    sti

    mov [boot_drive], dl        ; BIOS hands us the boot drive number in dl, don't lose it

    ; Print a boot message using BIOS teletype
    mov si, msg_boot
    call print_string

    ; ---- Load kernel from disk using BIOS INT13h extended (LBA) reads ----
    ; One reusable DAP (disk address packet), KERNEL_CHUNKS iterations of
    ; KERNEL_SECTORS_PER_CHUNK sectors apiece, walking the destination
    ; segment up by 0x800 each time so every chunk lands back-to-back
    ; starting at 0x10000 -- right where the linker script expects the
    ; kernel. Every physical address involved stays comfortably under
    ; the 1MB mark, so none of this needs the A20 line enabled yet --
    ; that still happens after, same as before. Legacy CHS reads (AH=02h)
    ; cap out at 255 sectors per call anyway, which is exactly the kind
    ; of limit an LBA-based DAP loop like this one sidesteps entirely.
    mov word [dap_sectors], KERNEL_SECTORS_PER_CHUNK
    mov word [dap_lba], 1              ; kernel starts at LBA 1 (LBA 0 is this boot sector)
    mov word [dap_lba+2], 0
    mov word [dap_segment], KERNEL_LOAD_SEG
    mov cx, KERNEL_CHUNKS

.load_loop:
    mov si, dap
    mov dl, [boot_drive]
    mov ah, 0x42
    int 0x13
    jc disk_error

    add word [dap_lba], KERNEL_SECTORS_PER_CHUNK
    add word [dap_segment], 0x0800      ; += 32KB, in paragraph units
    loop .load_loop

    mov si, msg_loaded
    call print_string

    ; ---- Switch video to VBE mode 0100h: 640x400, 256 colors ----
    ; Standard VGA only ever offered fixed resolutions (mode 13h =
    ; 320x200x256), but VBE (VESA BIOS Extensions) has been a near-
    ; universal VGA BIOS standard since the mid-90s -- QEMU's own VGA
    ; BIOS implements it too. Mode 0100h is the standard VBE mode number
    ; for 640x400 at 8 bits/pixel: same one-byte-per-pixel, 256-color-
    ; palette format as mode 13h, just a bigger canvas, so the kernel's
    ; whole drawing model (backbuf, bb_putpixel, the DAC palette) barely
    ; has to change -- only VGA_WIDTH/VGA_HEIGHT do.
    ;
    ; Unlike mode 13h, the linear framebuffer for a VBE mode isn't
    ; guaranteed to sit at a fixed address, so we ask the BIOS where it
    ; actually put it (PhysBasePtr, at offset 40 in the mode info block)
    ; and leave that answer sitting in memory at 0x9000 for the kernel
    ; to read directly later -- there's nothing else using that address
    ; at this point, and the kernel runs without paging, so "read a
    ; physical address" is just "read a pointer" to it.
    xor ax, ax
    mov es, ax
    mov di, 0x9000               ; scratch buffer: 256 bytes, well below the kernel's 0x10000 load point
    mov ax, 0x4F01
    mov cx, 0x0100
    int 0x10
    cmp ax, 0x004F
    jne vbe_error

    mov ax, 0x4F02
    mov bx, 0x4100               ; mode 0100h | 4000h (request a linear framebuffer)
    int 0x10
    cmp ax, 0x004F
    jne vbe_error
    jmp vbe_done

vbe_error:
    mov si, msg_vbe_err
    call print_string
    jmp $

vbe_done:
    ; ---- Enable A20 line (fast method via port 0x92) ----
    ; Without this, memory access wraps around at 1MB like it's still
    ; 1981, and everything above that just silently aliases back to zero.
    ; Flip the bit, move on with our lives.
    in al, 0x92
    or al, 2
    out 0x92, al

    ; ---- Load GDT and shove the CPU into protected mode ----
    cli
    lgdt [gdt_descriptor]

    mov eax, cr0
    or eax, 1
    mov cr0, eax

    jmp CODE_SEG:protected_mode_entry

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

boot_drive: db 0
msg_boot:      db "MiniWin: booting...", 13, 10, 0
msg_loaded:    db "MiniWin: kernel loaded, entering 32-bit mode...", 13, 10, 0
msg_disk_err:  db "MiniWin: DISK READ ERROR", 13, 10, 0
msg_vbe_err:   db "MiniWin: VBE 640x400 MODE NOT SUPPORTED", 13, 10, 0

; ============================================================
; Single reusable Disk Address Packet for the load loop above. Standard
; 16-byte INT13h AH=42h format: size, reserved, sector count, dest
; offset (always 0 here), dest segment (walked forward each iteration),
; then an 8-byte starting LBA (we only ever touch its low word --
; 1024 sectors total never comes close to needing the upper bits).
; ============================================================
align 4
dap:
    db 0x10, 0
dap_sectors: dw 0
             dw 0x0000
dap_segment: dw 0
dap_lba:     dq 0

; ============================================================
; Global Descriptor Table (flat memory model, because life's too short
; for segmented memory models)
; ============================================================
align 8
gdt_start:
gdt_null:
    dq 0x0

gdt_code:                       ; CODE_SEG selector = gdt_code - gdt_start = 0x08
    dw 0xFFFF                   ; limit low
    dw 0x0000                   ; base low
    db 0x00                     ; base middle
    db 10011010b                ; access: present, ring0, code, executable, readable
    db 11001111b                ; flags(4-bit) + limit high(4-bit): 4KB granularity, 32-bit
    db 0x00                     ; base high

gdt_data:                       ; DATA_SEG selector = gdt_data - gdt_start = 0x10
    dw 0xFFFF
    dw 0x0000
    db 0x00
    db 10010010b                ; access: present, ring0, data, writable
    db 11001111b
    db 0x00
gdt_end:

gdt_descriptor:
    dw gdt_end - gdt_start - 1  ; GDT size - 1
    dd gdt_start                ; GDT address

CODE_SEG equ gdt_code - gdt_start
DATA_SEG equ gdt_data - gdt_start

; ============================================================
; 32-bit protected mode entry point -- welcome to the future (1985)
; ============================================================
BITS 32
protected_mode_entry:
    mov ax, DATA_SEG
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    ; Stack base: as high as conventional memory safely goes. Real BIOS
    ; conventional memory ends at 0xA0000 (640KB) -- above that is the
    ; VGA framebuffer's memory-mapped window, not RAM, so writing there
    ; corrupts the screen instead of the stack. 0x9FC00 leaves exactly
    ; 1KB of breathing room below that hard ceiling (a nod to the fact
    ; that the last KB of conventional memory has historically been
    ; reserved for the Extended BIOS Data Area on real hardware, even
    ; though QEMU doesn't enforce that -- no reason to test the theory).
    ; This used to be 0x90000, which put the stack a mere 64KB above
    ; where .bss starts growing (kernel/link.ld) -- fine when this
    ; kernel's globals were small, but every NIC ring buffer, font
    ; table, and now the whole network stack's tables have been eating
    ; into that gap for a while. Moving the stack up here instead of
    ; trimming .bss buys a much larger, much longer-lived margin for
    ; whatever gets added next -- see build.sh's .bss-vs-stack check,
    ; which enforces that margin at every build from now on instead of
    ; letting it erode silently again.
    mov esp, 0x9FC00

    ; Off we go into the C kernel, loaded at 0x10000. This bootloader's
    ; entire job is now done. It was a good run.
    jmp CODE_SEG:0x10000

; ============================================================
; Boot sector padding + the magic signature that tells the BIOS
; "yes, this is actually bootable, I promise"
; ============================================================
times 510-($-$$) db 0
dw 0xAA55
