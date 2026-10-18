; SPDX-License-Identifier: GPL-3.0-only
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

BOOT_BASE_ADDR  equ 0x0504      ; u32 written by stage 1: the LBA this installation starts at (see boot.asm)

BOOT_GEOM_ADDR  equ 0x050C      ; u8 sectors-per-track, u8 heads: the BIOS geometry when the boot disk is read by CHS (floppy)
BOOT_DRIVE_ADDR equ 0x0500      ; fixed, low-memory scratch address both
                                 ; stage 1 and stage 2 agree on independently
                                 ; for the one byte of state that needs to
                                 ; survive the handoff between them (the BIOS
                                 ; boot drive number) -- simpler and far less
                                 ; fragile than either file trying to know
                                 ; the other's internal layout.

KERNEL_LOAD_SEG   equ 0x1000      ; kernel lands at physical 0x10000 (seg 0x1000, off 0)
KERNEL_CHUNKS             equ 16  ; 16 x 64 sectors = 1024 sectors = 512KB budget -- grown from 448KB for
                                  ; the HTML5 engine (kernel/dom.h, css.h, layout.h, render.h). It ends at
                                  ; 0x10000 + 512KB = 0x90000, still under the 0x9FC00 stack/EBDA line.
                                  ; (history:
                                  ; bumped up from 160KB now that the UI's bitmap
                                  ; fonts (kernel/font_latin_data.h, font_ko_data.h)
                                  ; are baked from Galmuri11 at 11x11 instead of the
                                  ; old hand-drawn 8x8 -- 11172 Hangul syllables at
                                  ; 11 rows of u16 apiece is just a bigger table than
                                  ; 11172 rows of u8 was, no way around that short of
                                  ; a real compression scheme this bootloader has no
                                  ; business implementing. Ends comfortably under the
                                  ; 1MB mark (0x10000 + 448KB = 0x80000) so this still
                                  ; doesn't need the A20 line enabled to load safely.
                                  ; See kernel/fs.h for how this budget lines up with
                                  ; where the file-storage slots start.)
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
    ;
    ; Written for BIOSes that are less forgiving than QEMU's (VMware's, VirtualBox's, real machines'). Three lessons are
    ; baked in: (1) nothing that must survive an INT 13h call lives in a register -- the loop state is in memory, because
    ; a BIOS is only obliged to preserve what its documentation says and some do not preserve CX (the old loop counted
    ; its chunks in CX); (2) the disk address packet is rebuilt for every call, because some BIOSes write the transfer
    ; count back into it; (3) a failed read is retried after a disk reset and then retried again with HALF the sectors
    ; per call, down to one, because some BIOSes cap the count per call below 64. Only if even a single sector will not
    ; read is it an error, and then the message names the LBA and the BIOS's own error code.
    ; A FLOPPY (BIOS drive 00h/01h) has no extended reads: use CHS from the start. (A hard disk whose BIOS lacks the
    ; extensions is switched over inside read_chunk, on its first "invalid function".)
    cmp byte [BOOT_DRIVE_ADDR], 0x80
    jae .edd_mode
    call setup_chs
.edd_mode:
    mov eax, [BOOT_BASE_ADDR]
    add eax, KERNEL_START_LBA    ; relative to where this installation starts (0 for a whole-disk image)
    mov [rd_lba], eax
    mov word [rd_seg], KERNEL_LOAD_SEG
    mov word [rd_left], KERNEL_CHUNKS * KERNEL_SECTORS_PER_CHUNK
    mov word [rd_chunk], KERNEL_SECTORS_PER_CHUNK

.load_loop:
    mov ax, [rd_chunk]
    cmp ax, [rd_left]
    jbe .count_ok
    mov ax, [rd_left]            ; (never more than what is left)
.count_ok:
    mov [rd_count], ax
    call read_chunk
    jnc .chunk_done
    cmp word [rd_chunk], 1       ; failed even after retries: try again with half as many sectors per call
    jbe disk_error
    shr word [rd_chunk], 1
    jmp .load_loop

.chunk_done:
    movzx eax, word [rd_count]
    add [rd_lba], eax            ; 32-bit: a partition's LBAs routinely exceed 65535
    shl ax, 5                    ; sectors * 512 bytes / 16 = paragraphs
    add [rd_seg], ax
    mov ax, [rd_count]
    sub [rd_left], ax
    jnz .load_loop

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
    ; Not even one sector would read. Say exactly what and where: error name, the BIOS's code, the LBA, the drive.
    mov si, msg_err_kernel
    call print_string
    mov si, msg_ah
    call print_string
    mov al, [rd_ah]
    call print_hex8
    mov si, msg_lba
    call print_string
    mov eax, [rd_lba]
    call print_hex32
    mov si, msg_dl
    call print_string
    mov al, [BOOT_DRIVE_ADDR]
    call print_hex8
    mov si, msg_crlf
    call print_string
    jmp $

; Reads [rd_count] sectors from LBA [rd_lba] to [rd_seg]:0000 with INT 13h AH=42h. CF=1 and [rd_ah] set if it failed three
; times (with a disk reset between attempts). The packet is rebuilt on every attempt; DS/ES are reloaded after each call.
read_chunk:
    cmp byte [use_chs], 0
    jne read_chunk_chs
    mov byte [rd_tries], 3
.again:
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov byte [dap], 0x10
    mov byte [dap+1], 0
    mov ax, [rd_count]
    mov [dap_sectors], ax
    mov word [dap_off], 0
    mov ax, [rd_seg]
    mov [dap_segment], ax
    mov eax, [rd_lba]
    mov [dap_lba], eax
    mov dword [dap_lba+4], 0
    mov si, dap
    mov dl, [BOOT_DRIVE_ADDR]
    mov ah, 0x42
    int 0x13
    jnc .ok
    mov bl, ah
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov [rd_ah], bl
    mov dl, [BOOT_DRIVE_ADDR]
    int 0x13                     ; AH=0: reset the disk system
    dec byte [rd_tries]
    jnz .again
    cmp byte [rd_ah], 0x01       ; "invalid function": this BIOS has no extended reads (floppy / old BIOS) -> CHS instead
    jne .failed
    call setup_chs
    jmp read_chunk
.failed:
    stc
    ret
.ok:
    xor ax, ax                   ; the BIOS may have changed DS/ES even on success
    mov ds, ax
    mov es, ax
    clc
    ret

; ---- CHS reads (floppies, and hard disks whose BIOS has no INT 13h extensions) ----
; setup_chs: switch read_chunk to CHS and ask the BIOS for the drive geometry (AH=08h); if it will not say, assume a 1.44 MB
; floppy (18 sectors per track, 2 heads).
setup_chs:
    mov byte [use_chs], 1
    mov byte [chs_spt], 18
    mov byte [chs_heads], 2
    xor ax, ax
    mov es, ax
    mov di, ax                   ; (ES:DI = 0:0, as the call asks for floppies)
    mov ah, 0x08
    mov dl, [BOOT_DRIVE_ADDR]
    int 0x13
    xor ax, ax                   ; the call may have changed anything: DS/ES back first
    mov ds, ax
    mov es, ax
    jc .geom_done
    mov al, cl
    and al, 0x3F                 ; CL bits 0-5: sectors per track
    jz .geom_done
    mov [chs_spt], al
    inc dh                       ; DH: highest head number
    jz .geom_done
    mov [chs_heads], dh
.geom_done:
    mov al, [chs_spt]            ; hand the geometry to the kernel's floppy driver (kernel/fdc.h)
    mov [BOOT_GEOM_ADDR], al
    mov al, [chs_heads]
    mov [BOOT_GEOM_ADDR + 1], al
    ret

; Reads [rd_count] sectors from LBA [rd_lba] to [rd_seg]:0000, one track piece at a time (a floppy read must not cross a
; track), through a bounce buffer below 64 KB (so the floppy controller's DMA never crosses a 64 KB boundary), then copies
; to the kernel's address with a real-mode string move. CF=1 and [rd_ah] set on failure.
CHS_BOUNCE equ 0x1000            ; 0x1000..0x5000: clear of stage 2 (0x600-0xE00), the boot-sector copy (0x7C00) and the stack
read_chunk_chs:
    mov eax, [rd_lba]
    mov [c_lba], eax
    mov ax, [rd_count]
    mov [c_left], ax
    mov ax, [rd_seg]
    mov [c_seg], ax
.next:
    mov eax, [c_lba]
    xor edx, edx
    movzx ebx, byte [chs_spt]
    div ebx                      ; eax = track number, edx = 0-based sector in the track
    mov [c_sect], dl
    xor edx, edx
    movzx ebx, byte [chs_heads]
    div ebx                      ; eax = cylinder, edx = head
    mov [c_head], dl
    mov [c_cyl], ax
    movzx ax, byte [chs_spt]
    movzx bx, byte [c_sect]
    sub ax, bx                   ; sectors left in this track
    cmp ax, 32
    jbe .cap_ok
    mov ax, 32                   ; (a 63-sector track would overrun the bounce buffer)
.cap_ok:
    cmp ax, [c_left]
    jbe .n_ok
    mov ax, [c_left]
.n_ok:
    mov [c_n], ax
    mov byte [rd_tries], 3
.again:
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ax, [c_cyl]
    mov ch, al                   ; cylinder bits 0-7
    shr ax, 2
    and al, 0xC0                 ; cylinder bits 8-9 go to CL bits 6-7
    mov cl, [c_sect]
    inc cl                       ; CHS sectors count from 1
    or cl, al
    mov dh, [c_head]
    mov dl, [BOOT_DRIVE_ADDR]
    mov bx, CHS_BOUNCE
    mov al, [c_n]
    mov ah, 0x02
    int 0x13
    jnc .read_ok
    mov bl, ah
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov [rd_ah], bl
    mov dl, [BOOT_DRIVE_ADDR]
    int 0x13                     ; reset (a floppy often needs it right after the motor has spun up)
    dec byte [rd_tries]
    jnz .again
    stc
    ret
.read_ok:
    xor ax, ax
    mov ds, ax
    mov ax, [c_seg]
    mov es, ax
    mov si, CHS_BOUNCE
    xor di, di
    mov cx, [c_n]
    shl cx, 8                    ; sectors * 256 words
    cld
    rep movsw
    xor ax, ax
    mov es, ax
    movzx eax, word [c_n]
    add [c_lba], eax
    shl ax, 5                    ; sectors * 512 / 16 = paragraphs
    add [c_seg], ax
    mov ax, [c_n]
    sub [c_left], ax
    jnz .next
    clc
    ret

; AL as two hex digits / EAX as eight.
print_hex8:
    pusha
    mov bl, al
    shr al, 4
    call .nib
    mov al, bl
    and al, 15
    call .nib
    popa
    ret
.nib:
    add al, '0'
    cmp al, '9'
    jbe .out
    add al, 7
.out:
    mov ah, 0x0E
    mov bh, 0
    int 0x10
    ret
print_hex32:
    push eax
    shr eax, 16
    call print_hex16
    pop eax
print_hex16:
    push ax
    mov al, ah
    call print_hex8
    pop ax
    call print_hex8
    ret

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
; Boot errors have a code and a NAME; the full table (what each means and what to try) is in docs/BOOT-PROCESS.md.
; MW-B01/B02 live in stage 1 (boot.asm), MW-C* in the partition chain loader (chain_mbr.asm).
msg_err_kernel: db "MiniWin ERROR MW-B03 KERNEL_READ_FAILED", 0
msg_ah:        db " (BIOS AH=", 0
msg_lba:       db " LBA=", 0
msg_dl:        db " DL=", 0
msg_crlf:      db ")", 13, 10, 0
msg_vbe_err:   db "MiniWin ERROR MW-B04 VBE_INFO_UNAVAILABLE (the BIOS has no VESA mode list)", 13, 10, 0
msg_no_truecolor: db "MiniWin ERROR MW-B05 VBE_NO_640x480x32_MODE (no 640x480 truecolor VESA mode)", 13, 10, 0

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
dap_off:     dw 0x0000
dap_segment: dw 0
dap_lba:     dq 0

; The kernel-load loop's state. All of it lives here, not in registers (see the comment above the loop).
rd_lba:      dd 0
rd_seg:      dw 0
rd_count:    dw 0
rd_chunk:    dw 0
rd_left:     dw 0
rd_tries:    db 0
rd_ah:       db 0
use_chs:     db 0
chs_spt:     db 18
chs_heads:   db 2
c_sect:      db 0
c_head:      db 0
c_cyl:       dw 0
c_n:         dw 0
c_left:      dw 0
c_seg:       dw 0
c_lba:       dd 0

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
