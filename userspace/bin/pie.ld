/*
 * pie.ld -- for aslrtest only (see Makefile: PIE_LDFLAGS).
 *
 * A real -static -pie link needs the linker's *default* script,
 * not a custom SECTIONS layout like fixed.ld's -- the default one
 * already knows how to correctly place .dynamic, .rela.dyn,
 * .dynsym, and friends for a self-relocating static PIE, which is
 * exactly the machinery apply_relocations() in kernel/elf/elf.c
 * reads. Replacing it with a hand-written SECTIONS block (the way
 * fixed.ld does for a plain ET_EXEC) risks getting that placement
 * subtly wrong.
 *
 * The only thing actually missing from the default script is the
 * __bss_start/__bss_end symbols crt0.asm needs to zero BSS at
 * startup (fixed.ld defines these itself, inside its own custom
 * .bss rule). INSERT BEFORE/AFTER adds just these two symbol
 * definitions right around the default script's own .bss section,
 * leaving everything else -- including PT_DYNAMIC placement -- as
 * the default linker script already handles it.
 */
SECTIONS {
    __bss_start = .;
}
INSERT BEFORE .bss;

SECTIONS {
    __bss_end = .;
}
INSERT AFTER .bss;
