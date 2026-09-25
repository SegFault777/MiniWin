/* Linker script for loadable .mwp programs (see kernel/mwp.h). Same
 * shape as kernel/link.ld -- position everything from a fixed load
 * address, keep .text/.rodata/.data, drop debug sections nobody at
 * runtime reads -- just anchored at MWP_LOAD_ADDR (kernel/mwp.h)
 * instead of the kernel's own 0x10000. The two addresses are
 * completely unrelated on purpose: a .mwp never runs at the same time
 * the kernel is still being loaded, so there's no reason for their
 * load addresses to relate to each other, only for THIS one to match
 * whatever address kernel/mwp.h's mwp_run() actually copies bytes to.
 *
 * No .bss section here, deliberately: mwp_run() copies exactly the
 * program's on-disk byte count into RAM and jumps straight in -- it
 * never zeroes anything past the end of the file the way
 * kernel/kentry.asm zeroes the kernel's own .bss. A .mwp with global
 * variables that need to start at zero would start out full of
 * whatever garbage happened to be sitting at MWP_LOAD_ADDR from
 * whatever ran there last. Rather than silently build that footgun in,
 * programs/README.md just says: keep state on the stack (locals),
 * don't rely on zero-initialized globals. A tiny greeter program has
 * no business needing global state anyway.
 */
ENTRY(mwp_entry)

SECTIONS
{
    . = 0x85000; /* must match kernel/mwp.h's MWP_LOAD_ADDR exactly */

    .text : {
        *(.text.mwp_entry) /* entry point first, so byte 0 of the
                            * output really is the first instruction to
                            * run -- mwp_run() jumps to
                            * MWP_LOAD_ADDR + entry_offset, and every
                            * program built by tools/build_mwp.sh uses
                            * entry_offset 0 */
        *(.text*)
    }

    .rodata : {
        *(.rodata*)
    }

    .data : {
        *(.data*)
    }

    /DISCARD/ : {
        *(.eh_frame*)
        *(.comment*)
        *(.note*)
        *(.bss*)   /* see the top-of-file note: intentionally not linked
                    * in at all, so a program that accidentally declares
                    * a global finds out at build time (an undefined
                    * reference from referencing it) rather than at run
                    * time (silently reading garbage) */
        *(COMMON)
    }
}
