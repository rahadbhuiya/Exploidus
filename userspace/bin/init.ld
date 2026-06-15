ENTRY(_start)
OUTPUT_FORMAT(elf64-x86-64)
SECTIONS {
    . = 0x0;
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
