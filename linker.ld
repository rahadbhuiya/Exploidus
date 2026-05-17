ENTRY(_start)

SECTIONS
{
    . = 0x100000;

    .multiboot2 ALIGN(8) :
    {
        KEEP(*(.multiboot2))
    }

    .text ALIGN(4096) :
    {
        *(.text*)
    }

    .rodata ALIGN(4096) :
    {
        *(.rodata*)
    }

    .data ALIGN(4096) :
    {
        *(.data*)
    }

    .bss ALIGN(4096) :
    {
        *(COMMON)
        *(.bss*)
    }

    .debug_info    0 : { *(.debug_info)    }
    .debug_abbrev  0 : { *(.debug_abbrev)  }
    .debug_aranges 0 : { *(.debug_aranges) }
    .debug_line    0 : { *(.debug_line)    }
    .debug_str     0 : { *(.debug_str)     }

    /DISCARD/ :
    {
        *(.eh_frame)
        *(.note.GNU-stack)
    }
}