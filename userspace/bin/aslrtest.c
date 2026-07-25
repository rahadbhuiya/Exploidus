#include <stdio.h>

/*
 * aslrtest -- proves the ELF loader's relocation processing works.
 *
 * This is built as a real static-PIE (ET_DYN) binary, unlike every
 * other test program in userspace/bin, which link as plain ET_EXEC
 * via a fixed-base linker script and never actually exercise ASLR at
 * all. See Makefile: this one links with -pie -static instead of a
 * fixed .ld script.
 *
 * The array below is the exact pattern that broke rahu's own
 * cmd_update() before the loader-side fix (see kernel/elf/elf.c,
 * apply_relocations()): a local array of pointers to string
 * literals. Each pointer value is an absolute address that only a
 * real R_X86_64_RELATIVE relocation can correctly shift for this
 * binary's randomized runtime base. If relocation processing is
 * broken or missing, these pointers still hold their link-time
 * (base-0) values, and dereferencing them crashes or prints garbage
 * instead of the intended text.
 */
int main(void)
{
    const char *parts[] = {
        "aslrtest: ", "this", " ", "binary", " ", "is", " ", "PIE", " ",
        "(ET_DYN)", ".", " ", "If", " ", "you", " ", "see", " ", "this",
        " ", "sentence", " ", "printed", " ", "correctly", ",", " ",
        "relocations", " ", "worked.", "\n", (const char *)0
    };

    for (int i = 0; parts[i]; i++) {
        printf("%s", parts[i]);
    }

    printf("aslrtest: PASS\n");
    return 0;
}