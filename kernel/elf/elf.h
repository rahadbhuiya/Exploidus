#pragma once
#include <stdint.h>
#include <stdbool.h>

/*
 * Minimal ELF64 definitions — enough for x86-64 user programs
 */

#define ELF_MAGIC       0x464C457F
#define ELF_CLASS_64    2
#define ELF_DATA_LE     1
#define ELF_TYPE_EXEC   2
#define ELF_TYPE_DYN    3   /* PIE executable — ASLR-capable */
#define ELF_MACH_X86_64 0x3E

#define PT_LOAD         1
#define PT_NULL         0
#define PT_DYNAMIC      2

#define PF_X            (1 << 0)
#define PF_W            (1 << 1)
#define PF_R            (1 << 2)

/*
 * Dynamic section tags and RELA relocations -- just enough to
 * self-relocate a statically-linked PIE (built with -static-pie),
 * which only ever emits R_X86_64_RELATIVE entries (no external
 * symbols to resolve, since everything is linked into one image).
 * See apply_relocations() in elf.c.
 */
#define DT_NULL         0
#define DT_RELA         7
#define DT_RELASZ       8
#define DT_RELAENT      9

#define R_X86_64_RELATIVE 8
#define ELF64_R_TYPE(info) ((uint32_t)((info) & 0xffffffffULL))

typedef struct __attribute__((packed)) {
    int64_t  d_tag;
    uint64_t d_val;
} elf64_dyn_t;

typedef struct __attribute__((packed)) {
    uint64_t r_offset;
    uint64_t r_info;
    int64_t  r_addend;
} elf64_rela_t;

/*  ELF HEADER  */
typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t  class;
    uint8_t  data;
    uint8_t  version;
    uint8_t  os_abi;
    uint8_t  abi_version;
    uint8_t  pad[7];

    uint16_t type;
    uint16_t machine;
    uint32_t version2;

    uint64_t entry;
    uint64_t phoff;
    uint64_t shoff;
    uint32_t flags;

    uint16_t ehsize;
    uint16_t phentsize;
    uint16_t phnum;
    uint16_t shentsize;
    uint16_t shnum;
    uint16_t shstrndx;
} elf64_hdr_t;

/*  PROGRAM HEADER  */
typedef struct __attribute__((packed)) {
    uint32_t type;
    uint32_t flags;
    uint64_t offset;
    uint64_t vaddr;
    uint64_t paddr;
    uint64_t filesz;
    uint64_t memsz;
    uint64_t align;
} elf64_phdr_t;

/*  API  */
bool elf_load(const uint8_t *elf_data, uint64_t elf_size,
              uint64_t *pml4_phys,
              uint64_t *entry_out,
              uint64_t *stack_top,
              const char **argv,   /* NULL-terminated, may be NULL */
              const char **envp);  /* NULL-terminated, may be NULL */

bool elf_validate(const uint8_t *data, uint64_t size);