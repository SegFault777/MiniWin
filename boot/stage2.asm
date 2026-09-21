; ============================================================
; MiniWin Bootloader -- Stage 2
;
; Stage 1 (boot/boot.asm) is squeezed into the MBR's unforgiving 512-byte
; budget and can only just barely load THIS file off disk before running
; out of room -- there's no space left there for anything as involved as
; walking the BIOS's actual list of supported VBE modes looking for a
; real 640x480 32-bit truecolor mode (as opposed to just requesting a
; fixed, hoped-for mode number, which is all the old single-stage
; bootloader ever did). So that work, plus everything else stage 1
; didn't have room for -- loading the kernel itself, enabling A20,
; setting up the GDT, and the actual jump into 32-bit protected mode --
; lives here instead, in a file with no such size limit.
;
; Loaded by stage 1 at 0x0000:0x0600 and entered via a near jump (still
; 16-bit real mode at that point) -- everything below assumes it's
; running from that address, which is what ORG 0x0600 below tells NASM
; to bake into every absolute reference.
; ============================================================
BITS 16
ORG 0x0600

BOOT_DRIVE_ADDR equ 0x0500      ; fixed, low-memory scratch address both
                                 ; stage 1 and stage 2 agree on independently
                                 ; for the one byte of state that needs to
                                 ; survive the handoff between them (the BIOS
                                 ; boot drive number) -- simpler and far less
                                 ; fragile than either file trying to know
                                 ; the other's internal layout.

KERNEL_LOAD_SEG   equ 0x1000      ; kernel lands at physical 0x10000 (seg 0x1000, off 0)
KERNEL_CHUNKS             equ 5   ; 5 x 64 sectors = 320 sectors = 160KB budget --
                                  ; see kernel/fs.h for how this budget lines up
                                  ; with where the file-storage slots start.
KERNEL_SECTORS_PER_CHUNK  equ 64
KERNEL_START_LBA equ (1 + STAGE2_SECTORS)   ; LBA 0 is stage 1, then
                                            ; STAGE2_SECTORS sectors of
                                            ; this file, then the kernel
                                            ; -- STAGE2_SECTORS is defined
                                            ; by build.sh via -D so this
                                            ; number lives in exactly one
                                            ; place instead of needing to
                                            ; be kept in sync by hand
                                            ; between here, boot.asm, and
                                            ; build.sh's own layout math.

stage2_entry:
    ; Segments/stack were already set up by stage 1 and don't need
    ; redoing -- this is a near jump within the same running program,
    ; not a fresh boot.

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
    mov word [dap_lba], KERNEL_START_LBA
    mov word [dap_lba+2], 0
    mov word [dap_segment], KERNEL_LOAD_SEG
    mov cx, KERNEL_CHUNKS

.load_loop:
    mov si, dap
    mov dl, [BOOT_DRIVE_ADDR]
    mov ah, 0x42
    int 0x13
    jc disk_error

    add word [dap_lba], KERNEL_SECTORS_PER_CHUNK
    add word [dap_segment], 0x0800      ; += 32KB, in paragraph units
    loop .load_loop

    mov si, msg_loaded
    call print_string

    ; ---- Find and switch to a real 640x480, 32 bits/pixel, direct-color
    ; VBE mode -- true color, not a fixed 256-entry palette ----
    ;
    ; The OLD approach here just asked for a fixed mode NUMBER (0100h)
    ; and trusted it to mean "640x400x8bpp" -- that number happens to be
    ; a stable VESA convention for that specific resolution/depth, but
    ; there is no equivalent stable number for 640x480 at 32 bits/pixel:
    ; different VBE implementations (real graphics cards, QEMU's Bochs
    ; VBE, VirtualBox, VMware) are free to assign that mode whatever
    ; number they like. The only portable way to find it is to ask the
    ; BIOS for its own list of supported modes and check each one's
    ; actual properties -- which is what this whole block does, instead
    ; of guessing a number and hoping.
    ;
    ; Step 1: VBE Controller Info (AX=4F00h) -- writing the "VBE2"
    ; signature into the buffer *before* calling is the standard way to
    ; ask for the VBE 2.0+ extended info block (VideoModePtr's location
    ; doesn't move between 1.x/2.0, but some BIOSes only fill in the
    ; 2.0+ fields when they see this signature first).
    xor ax, ax
    mov es, ax
    mov di, 0x6000
    mov word [es:di], 'VB'
    mov word [es:di+2], 'E2'
    mov ax, 0x4F00
    int 0x10
    cmp ax, 0x004F
    jne vbe_error

    ; VideoModePtr is a far pointer stored offset-first: word at +14 is
    ; the offset, word at +16 is the segment.
    mov si, [0x6000 + 14]
    mov ax, [0x6000 + 16]
    mov [mode_list_seg], ax
    mov [mode_list_off], si

.mode_scan_loop:
    mov ax, [mode_list_seg]
    mov es, ax
    mov si, [mode_list_off]
    mov cx, [es:si]                ; next mode number in the list
    cmp cx, 0xFFFF                 ; list is terminated by 0xFFFF
    je vbe_no_truecolor_mode
    add si, 2
    mov [mode_list_off], si        ; advance past this entry for next iteration

    ; Fetch this candidate mode's info block (AX=4F01h) into 0x9000 --
    ; the same address the kernel will later read the FINAL chosen
    ; mode's info from, so a matching candidate ends up exactly where
    ; it needs to be with no extra copy.
    push cx                        ; the interrupt clobbers plenty; keep our mode number safe
    xor ax, ax
    mov es, ax
    mov di, 0x9000
    mov ax, 0x4F01
    pop cx
    push cx
    int 0x10
    pop cx
    cmp ax, 0x004F
    jne .mode_scan_loop            ; this BIOS didn't like this mode number -- skip it, not fatal

    ; Check: XResolution==640, YResolution==480, BitsPerPixel==32,
    ; MemoryModel==6 (direct color), and ModeAttributes bit 0 (supported
    ; by current hardware) + bit 7 (linear framebuffer available) both set.
    mov ax, [0x9000 + 18]          ; XResolution
    cmp ax, 640
    jne .mode_scan_loop
    mov ax, [0x9000 + 20]          ; YResolution
    cmp ax, 480
    jne .mode_scan_loop
    mov al, [0x9000 + 25]          ; BitsPerPixel
    cmp al, 32
    jne .mode_scan_loop
    mov al, [0x9000 + 27]          ; MemoryModel
    cmp al, 6
    jne .mode_scan_loop
    mov ax, [0x9000 + 0]           ; ModeAttributes
    and ax, 0x0081                 ; bit0 (supported) | bit7 (LFB available)
    cmp ax, 0x0081
    jne .mode_scan_loop

    ; Found it. `cx` still holds this mode's number from the list walk
    ; above -- set it now, OR'd with 4000h to request the linear
    ; framebuffer addressing this whole kernel is built around.
    mov ax, 0x4F02
    mov bx, cx
    or bx, 0x4000
    int 0x10
    cmp ax, 0x004F
    jne vbe_error
    jmp vbe_done

vbe_no_truecolor_mode:
    ; No 640x480x32bpp direct-color mode was on offer. Rather than fall
    ; back to the old 256-color mode and carry two entirely separate
    ; graphics pipelines in the kernel forever (one real, one a museum
    ; piece), this is treated the same as any other VBE failure: a
    ; clear message and a halt. 640x480 at 32bpp has been close to
    ; universally supported -- real hardware and every major emulator
    ; alike -- for over two decades; a machine that can't offer it is a
    ; genuinely unusual case this kernel chooses not to carry permanent
    ; complexity for.
    mov si, msg_no_truecolor
    call print_string
    jmp $

vbe_error:
    mov si, msg_vbe_err
    call print_string
    jmp $

vbe_done:
    ; ---- Probe how much RAM this machine actually has (INT15h AX=E801h) ----
    ; The truecolor backbuffer the kernel is about to set up (640x480 at
    ; 4 bytes/pixel = 1.2MB) is far too big to fit in this kernel's usual
    ; low-memory footprint (everything else lives under the 640KB
    ; conventional-memory line), so it gets placed at a fixed physical
    ; address well above 1MB instead -- see kernel/vga.h's
    ; VGA_BACKBUF_PHYS_ADDR. Blindly trusting that address is safe RAM
    ; on every machine this ever boots on would be exactly the kind of
    ; unverified assumption this project has kept tripping over and
    ; fixing (the BSS/stack collision earlier, GCM's scratch buffer
    ; sized for "typical" instead of "maximum legal" -- see kernel/gcm.h's
    ; own comment on that one) -- so instead, ask the BIOS how much
    ; memory actually exists and let the kernel refuse to proceed rather
    ; than corrupt whatever's really at that address if there isn't
    ; enough. AX=E801h reports memory between 1-16MB in AX (1KB units)
    ; and memory above 16MB in BX (64KB units); summed and stored for
    ; the kernel to check before it ever touches the backbuffer address.
    xor ax, ax
    mov ax, 0xE801
    int 0x15
    jc .mem_probe_failed
    cmp ah, 0x86                 ; some BIOSes signal "unsupported function" this way
    je .mem_probe_failed
    cmp ah, 0x80
    je .mem_probe_failed

    ; Total KB = 1024 (the first 1MB, which E801h doesn't count) +
    ; AX (1-16MB range, already in KB) + BX*64 (above 16MB, in 64KB units)
    movzx eax, ax
    movzx ebx, bx
    shl ebx, 6                   ; BX * 64
    add eax, ebx
    add eax, 1024
    jmp .mem_probe_store

.mem_probe_failed:
    ; No usable answer from the BIOS -- store 0, which the kernel
    ; treats as "couldn't verify, refuse to proceed" rather than
    ; quietly assuming there's enough room. A real machine or emulator
    ; new enough to offer a 640x480x32bpp VBE mode is, in every case
    ; this was tested against, also new enough to answer E801h; this
    ; path exists for honesty, not because it's expected to trigger.
    xor eax, eax

.mem_probe_store:
    mov [0x9200], eax

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

msg_loaded:    db "MiniWin: kernel loaded, entering 32-bit mode...", 13, 10, 0
msg_disk_err:  db "MiniWin: DISK READ ERROR", 13, 10, 0
msg_vbe_err:   db "MiniWin: VBE CALL FAILED", 13, 10, 0
msg_no_truecolor: db "MiniWin: NO 640x480x32bpp TRUECOLOR VBE MODE FOUND", 13, 10, 0

; Far pointer (segment:offset, kept as two separate words since real
; mode has no native 32-bit pointer type) to the BIOS's own list of
; supported VBE mode numbers -- read once from the Controller Info
; Block, then walked one entry at a time by the mode-scan loop above.
mode_list_seg: dw 0
mode_list_off: dw 0

; ============================================================
; Single reusable Disk Address Packet for the kernel-load loop above.
; Standard 16-byte INT13h AH=42h format: size, reserved, sector count,
; dest offset (always 0 here), dest segment (walked forward each
; iteration), then an 8-byte starting LBA (we only ever touch its low
; word -- 1024 sectors total never comes close to needing the upper
; bits).
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
    ; See build.sh's .bss-vs-stack check, which enforces a safety margin
    ; between this and the kernel's own .bss at every build.
    mov esp, 0x9FC00

    ; Off we go into the C kernel, loaded at 0x10000. This bootloader's
    ; entire job is now done. It was a good run.
    jmp CODE_SEG:0x10000
