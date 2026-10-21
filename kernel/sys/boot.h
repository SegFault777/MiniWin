/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef MW_SYS_BOOT_H
#define MW_SYS_BOOT_H

/* sys/boot.h -- everything kmain used to do once before its loop: serial, video mode, syscall table, PCI, network, saved files, window slots.
 * (Split out of kmain() in pre-29; still one translation unit -- see the module map in kernel.c.) */

/* Reads a u32 the boot loader left at a fixed low-memory address. The empty asm hides the constant address from the
 * optimizer, which otherwise warns that "address 0x504" is outside any object (it is: it is a hardware/loader
 * contract, not a C object). */
static inline u32 boot_info_u32(u32 addr) {
    volatile u32 *p = (volatile u32 *)addr;
    __asm__ volatile ("" : "+r"(p));
    return *p;
}

static void kernel_boot(void) {
    /* Serial first, before anything else touches the framebuffer --
     * if the memory-safety check just below ever fails, this is the
     * only way left to say why, since writing to backbuf at that point
     * is exactly the unsafe operation being guarded against. */
    serial_init();
    sysinfo_probe();             /* CPUID / TSC availability, before anything reads the TSC */
    idt_init();                  /* from here a CPU exception shows a named error screen instead of resetting the machine */
#ifdef MW_TEST_FAULT             /* test builds only (EXTRA_CFLAGS=-DMW_TEST_FAULT): provoke vector 6 to see the screen */
    __asm__ volatile ("ud2");
#endif

    /* The Korean font ships compressed (font_ko_data.h) and has to be inflated before the first glyph is drawn. A
     * failure here is a build bug, not a runtime condition: say so over serial and carry on (Hangul would be garbage,
     * everything else works) rather than refuse to boot a machine whose only problem is a font. */
    serial_puts(ko_font_init() ? "[BOOT] Korean font inflated and verified\n" : "[BOOT] WARNING: Korean font failed to inflate/verify\n");

    /* The PCI bus has to be known before the disk is: the BIOS may have booted us from an AHCI SATA disk (or, later, a SCSI
     * controller), and those are PCI devices the storage layer finds through this list. (It also dumps every device over
     * serial, which is how real hardware IDs get confirmed before a driver is written against them.) */
    pci_scan();

    /* Where does THIS installation live? The boot loader left two numbers at fixed low addresses (boot/boot.asm):
     * the sector this installation starts at, and its install id. A stock image has both 0 and owns the primary
     * master; one placed by os-inst may sit in a partition and/or on another IDE position, so find the disk that
     * carries our id. This has to run before anything reads the disk (icons, files) -- every ata_read_sector()
     * from here on is relative to this base on this device. */
    st_boot_locate(boot_info_u32(0x0504), boot_info_u32(0x0508));
    serial_puts("[BOOT] installation base LBA ");
    serial_put_dec(blk_boot_base);
    serial_puts(" on ");
    serial_puts(st_type_names[blk_type_of(blk_boot_dev)]);
    serial_put_dec((u32)(blk_boot_dev - blk_first_of(blk_type_of(blk_boot_dev))) + 1);
    serial_putc('\n');

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
        char need[12], have[12];
        fatal_hex(need, VGA_MIN_RAM_KB / 1024, 2); fatal_hex(have, boot_info_u32(0x9200) / 1024, 4);
        char l3[64] = "need at least 0x", l4[64] = "the BIOS reported 0x";
        fatal_cat(l3, 64, need); fatal_cat(l3, 64, " MB (hex), see vga.h"); fatal_cat(l4, 64, have); fatal_cat(l4, 64, " MB (hex)");
        const char *const msg[] = { "MiniWin has stopped.", "", "ERROR MW-K01 NOT_ENOUGH_RAM", l3, l4,
                                    "", "Give the machine more memory (64 MB is enough)." };
        fatal_screen(msg, 7);
    }
    if (!vga_color_layout_is_standard()) {
        /* Every real 32bpp direct-color VBE implementation this kernel
         * has ever encountered uses the same red=16/green=8/blue=0 bit
         * layout every COL_* constant assumes -- see vga.h's own
         * comment on vga_color_layout_is_standard(). A machine that
         * genuinely differs would render every color wrong in a way
         * that's much harder to diagnose from a garbled screen than
         * from this one clear line over serial. */
        const char *const msg[] = { "MiniWin has stopped.", "", "ERROR MW-K02 VBE_PIXEL_FORMAT_UNSUPPORTED",
                                    "the video mode's colour channels are not in the",
                                    "standard red-green-blue order this kernel uses.",
                                    "", "See docs/dev/BOOT-PROCESS.md." };
        fatal_screen(msg, 7);
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

    /* If a supported NIC is present, bring the whole network stack up:
     * ARP cache, IPv4, ICMP, UDP, and a DHCP client that goes and asks
     * whatever network we're plugged into for a real address -- no more
     * hardcoding 10.0.2.15 and hoping the guest is always QEMU SLIRP.
     * Only one NIC is ever "active" (see nic.h) -- try RTL8139 first,
     * then e1000, whichever one QEMU (or real hardware) actually
     * presented on the PCI bus. */
    sysinfo_boot_s = net_wall_seconds();          /* uptime reference for `fetch` */
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

    desktop_refresh_programs();                 /* the program shortcuts */

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
