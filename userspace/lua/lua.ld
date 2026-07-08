ENTRY(_start)
OUTPUT_FORMAT(elf64-x86-64)
SECTIONS {
    /*
     * Loaded at a fixed, non-zero base (unlike hello.ld's 0x0) so
     * Exploidus's ELF loader takes its "ET_EXEC: fixed addr, no ASLR"
     * path (kernel/elf/elf.c: entry >= 0x100000 skips ASLR).
     *
     * Why this matters specifically for Lua and not e.g. hello.c:
     * onelua.c is full of static luaL_Reg tables — {name, function
     * pointer} arrays used to register every standard library
     * function. Those function pointers are absolute addresses baked
     * in at link time (assuming this file loads at exactly the base
     * it was linked for). Exploidus's loader doesn't process ELF
     * relocations at load time, so if it relocated this binary to a
     * randomized ASLR base like it does for small position-
     * independent-enough programs, every one of those table entries
     * would still hold its original base-0 address — jumping through
     * them would land at the wrong (unmapped, near-zero) address and
     * page-fault, which is exactly what was happening before this
     * fix. Loading at a fixed base sidesteps the whole issue.
     * Exploidus's isolated per-process page tables only zero out (make
     * safe for user mappings) the [32MB, 288MB) range — the first 16MB
     * is reserved by the kernel's own huge-page identity map (see
     * kernel/elf/elf.c's ASLR_MIN_BASE comment). This must land in
     * that safe window, so 0x400000 (4MB) doesn't work — 0x4000000
     * (64MB) does.
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
