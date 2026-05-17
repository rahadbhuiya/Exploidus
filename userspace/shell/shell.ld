/*
 * Exploidus userspace shell linker script.
 * Places the shell at 0x1000000 virtual — well within user space
 * and safely below the kernel's high-memory region.
 */

ENTRY(_start)
OUTPUT_FORMAT(elf64-x86-64)

SECTIONS {
    . = 0x0000000001000000;

    .text ALIGN(4096) : {
        /* crt0.asm _start must be first */
        build/userspace/libc/crt0.o(.text)
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
        *(COMMON)
        *(.bss)
        *(.bss.*)
    }

    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
        *(.gcc_except_table*)
    }
}
