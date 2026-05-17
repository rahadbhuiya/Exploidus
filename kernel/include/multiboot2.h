#pragma once
#include <stdint.h>

/*  MULTIBOOT2 CONSTANTS  */
#define MULTIBOOT2_MAGIC          0x36D76289
#define MULTIBOOT2_HEADER_MAGIC   0xE85250D6
#define MULTIBOOT2_ARCH_X86       0

/*  CORE INFO STRUCT  */
typedef struct {
    uint32_t total_size;
    uint32_t reserved;
} __attribute__((packed)) mb2_info_t;

/*  GENERIC TAG HEADER  */
typedef struct {
    uint32_t type;
    uint32_t size;
} __attribute__((packed)) mb2_tag_t;

/*  ALIGNMENT HELPER  */
#define MB2_ALIGN(x)  (((x) + 7) & ~7)

/*  MEMORY MAP TAG  */
#define MB2_TAG_MMAP  6
#define MB2_MMAP_AVAILABLE 1

typedef struct {
    uint64_t base_addr;
    uint64_t length;
    uint32_t type;
    uint32_t reserved;
} __attribute__((packed)) mb2_mmap_entry_t;

/*
 * FIXED DESIGN:
 * - NO flexible array member
 * - kernel must manually compute entries pointer
 */
typedef struct {
    uint32_t type;
    uint32_t size;
    uint32_t entry_size;
    uint32_t entry_version;
} __attribute__((packed)) mb2_tag_mmap_t;
/*  FRAMEBUFFER TAG  */
#define MB2_TAG_FB  8

typedef struct {
    uint32_t type;
    uint32_t size;
    uint64_t addr;
    uint32_t pitch;
    uint32_t width;
    uint32_t height;
    uint8_t  bpp;
    uint8_t  fb_type;
    uint16_t reserved;
} __attribute__((packed)) mb2_tag_fb_t;
