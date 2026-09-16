ENTRY(_start)

/* Every section pattern below uses the trailing "*" wildcard
 * (.text*, .data*, etc.) instead of matching the bare section name.
 * That's not decoration -- it's load-bearing. Compiling with
 * -ffunction-sections/-fdata-sections (see build.sh) puts every single
 * function and global into its own uniquely-named section (.text.foo,
 * .data.bar, ...) specifically so the linker's --gc-sections pass can
 * throw away the ones nothing calls. Match only the bare ".text" here
 * and every one of those per-function sections silently falls through
 * the cracks -- gc-sections has nothing to collect, and the whole
 * optimization quietly does nothing while looking like it's enabled.
 * This is exactly the kind of "builds fine, works fine, just doesn't
 * do the one thing you added it for" mistake that's invisible until
 * someone diffs the binary size before and after. */
SECTIONS
{
    . = 0x10000;

    .text : {
        *(.text*)
    }

    .rodata : {
        *(.rodata*)
    }

    .data : {
        *(.data*)
    }

    .bss : {
        _bss_start = .;
        *(.bss*)
        *(COMMON)
        _bss_end = .;
    }

    /* Debug/metadata sections nobody at runtime ever reads -- a
     * freestanding kernel with no debugger attached has no use for
     * unwind tables or comment strings, so drop them instead of
     * shipping them inside the boot image. */
    /DISCARD/ : {
        *(.eh_frame*)
        *(.comment*)
        *(.note*)
    }
}
