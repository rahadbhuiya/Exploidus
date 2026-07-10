ENTRY(_start)
OUTPUT_FORMAT(elf64-x86-64)
SECTIONS {
    /*
     * Fixed base (skips ASLR — see kernel/elf/elf.c: entry >= 0x100000
     * takes the "no ASLR" path), same address lua.ld uses. Safe for
     * multiple different binaries to share: each process gets its own
     * isolated page tables, so the same virtual address in two
     * different processes is unrelated memory.
     *
     * WHY THIS IS NEEDED: any function pointer taken in an
     * ASLR-relocated binary (signal handlers, callbacks, static
     * tables of function pointers) ends up holding its link-time
     * (base-0) value instead of the correct runtime address, because
     * Exploidus's ELF loader doesn't process relocations — it just
     * shifts the whole binary to a random base and hopes nothing
     * needed adjusting. RIP-relative code (most instructions) is
     * self-correcting under a pure shift, but an explicit address
     * VALUE computed and stored/passed as data (like `&my_handler`
     * passed to signal()) is not. First discovered via lua.ld; this
     * generic version is for any other binary hitting the same thing.
     */
    . = 0x4000000;

    .text ALIGN(4096) : {
        *(.text._start)
        *(.text)
        *(.text.*)
    }
    .rodata ALIGN(4096) : {
        *(.rodata)
        *(.rodata.*)
    }
    .data ALIGN(4096) : {
        *(.data)
        *(.data.*)
    }
    .bss ALIGN(4096) : {
        __bss_start = .;
        *(.bss)
        *(.bss.*)
        *(COMMON)
        __bss_end = .;
    }
    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
        *(.gcc_except_table*)
    }
}
