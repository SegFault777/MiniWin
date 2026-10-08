#ifndef MW_SYS_BOOT_H
#define MW_SYS_BOOT_H

/* sys/boot.h -- everything kmain used to do once before its loop: serial, video mode, syscall table, PCI, network, saved files, window slots.
 * (Split out of kmain() in pre-29; still one translation unit -- see the module map in kernel.c.) */

static void kernel_boot(void) {
    /* Serial first, before anything else touches the framebuffer --
     * if the memory-safety check just below ever fails, this is the
     * only way left to say why, since writing to backbuf at that point
     * is exactly the unsafe operation being guarded against. */
    serial_init();

    vga_init_display();

    /* The truecolor backbuffer lives at a fixed physical address well
     * above 1MB (see vga.h's VGA_BACKBUF_PHYS_ADDR) rather than in this
     * kernel's own .bss, because it's simply too big (1.2MB) to fit
     * there. That address is only real, usable RAM if the machine
     * actually has enough memory -- checked here against
     * boot/stage2.asm's own INT15h/E801h probe, rather than assumed,
     * before this kernel ever writes a single pixel through it. A
     * machine that fails this check gets a clear reason over serial
     * and a clean halt instead of silent memory corruption. */
    if (!vga_verify_memory_safe()) {
        serial_puts("[FATAL] not enough RAM for the truecolor framebuffer "
                     "(need ~");
        serial_put_dec(VGA_MIN_RAM_KB / 1024);
        serial_puts("MB, see kernel/vga.h VGA_MIN_RAM_KB) -- halting\n");
        for (;;) { __asm__ volatile ("hlt"); }
    }
    if (!vga_color_layout_is_standard()) {
        /* Every real 32bpp direct-color VBE implementation this kernel
         * has ever encountered uses the same red=16/green=8/blue=0 bit
         * layout every COL_* constant assumes -- see vga.h's own
         * comment on vga_color_layout_is_standard(). A machine that
         * genuinely differs would render every color wrong in a way
         * that's much harder to diagnose from a garbled screen than
         * from this one clear line over serial. */
        serial_puts("[FATAL] this VBE mode's color channel layout isn't "
                     "the standard RGB order this kernel assumes -- halting\n");
        for (;;) { __asm__ volatile ("hlt"); }
    }

    /* Fills in the fixed-address syscall table loadable .mwp programs
     * call into -- see kernel/mwp.h's own top-of-file comment for why
     * this has to happen before anything could possibly `run` one. Must
     * come after vga_init_display() (the table's draw_ and present
     * entries point at framebuffer-touching functions), but otherwise has no
     * ordering dependency on anything else in this init sequence. */
    icon_cache_init();
    mwp_init();
    mwp_syscalls.key_poll = keyboard_poll_key;

    mouse_init();

    /* Diagnostic-only for now: dumps every PCI device found (including
     * whatever network controller QEMU is presenting) out over the
     * serial port, so real hardware IDs can be confirmed before writing
     * a driver against them. Doesn't touch the GUI at all. */
    pci_scan();

    /* If a supported NIC is present, bring the whole network stack up:
     * ARP cache, IPv4, ICMP, UDP, and a DHCP client that goes and asks
     * whatever network we're plugged into for a real address -- no more
     * hardcoding 10.0.2.15 and hoping the guest is always QEMU SLIRP.
     * Only one NIC is ever "active" (see nic.h) -- try RTL8139 first,
     * then e1000, whichever one QEMU (or real hardware) actually
     * presented on the PCI bus. */
    if (rtl8139_init() || e1000_init()) {
        net_stack_init();
    }

    /* Check for previously-saved files on disk (real, persistent storage
     * via the ATA driver -- this survives across QEMU runs as long as the
     * disk image itself isn't rebuilt from scratch). */
    for (int slot = 0; slot < FS_MAX_FILES; slot++) {
        u32 loaded_len = 0;
        if (fs_check_slot(slot, &loaded_len)) {
            desktop_file_exists[slot] = 1;
            desktop_file_len[slot] = loaded_len;
        }
    }

    /* Each of the NOTEPAD_MAX window slots gets its own cascaded default
     * position (each one nudged 16px right/down from the last) so that
     * opening several at once doesn't stack them in a single unreadable
     * pile -- and since position is stored per-slot and persists across
     * that slot's own opens/closes, this cascade only ever needs setting
     * up once, here, at boot. */
    for (int i = 0; i < NOTEPAD_MAX; i++) {
        int ox = WIN_DEFAULT_X + i * 16;
        int oy = WIN_DEFAULT_Y + i * 14;
        notepads[i].id = i;
        notepads[i].win.x = notepads[i].win.restore_x = ox;
        notepads[i].win.y = notepads[i].win.restore_y = oy;
        notepads[i].win.w = notepads[i].win.restore_w = WIN_DEFAULT_W;
        notepads[i].win.h = notepads[i].win.restore_h = WIN_DEFAULT_H;
        notepads[i].bound_slot = -1;
    }
    active_np = &notepads[0];

}

#endif
