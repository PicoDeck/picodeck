#pragma once

// Pure ELF32 validation and relocation for native apps.
//
// Shared by the firmware loader (native_loader.c) and the simulator's Unicorn
// loader (simulator/unicorn_runner.c), and built on the host by the unit tests
// and fuzzer (tests/unit). No Pico SDK, no I/O, no allocation: callers read
// the file and own every buffer.
//
// Loading an app is three steps:
//   1. elf_plan_header()   - the 52-byte ELF header: magic, class, type,
//                            machine, and where the program header table is.
//   2. elf_plan_segments() - the program header table: every PT_LOAD is
//                            proven to lie inside the file and the image, and
//                            the image size, code segment and entry point are
//                            computed.  After this, copying p_filesz bytes
//                            from p_offset to (p_vaddr - mem_min) of an
//                            image_size buffer is in bounds for every
//                            PT_LOAD.
//   3. elf_relocate()      - once the segments are copied: a bounded walk of
//                            the PT_DYNAMIC table and the DT_REL/DT_RELA
//                            tables it names, applying R_ARM_RELATIVE.

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint32_t e_entry;
    uint32_t e_phoff;
    uint32_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} elf32_ehdr_t;

typedef struct {
    uint32_t p_type;
    uint32_t p_offset;
    uint32_t p_vaddr;
    uint32_t p_paddr;
    uint32_t p_filesz;
    uint32_t p_memsz;
    uint32_t p_flags;
    uint32_t p_align;
} elf32_phdr_t;

#define ELF_EHDR_SIZE 52u
#define ELF_PHDR_SIZE 32u

#define ELF_PT_LOAD    1u
#define ELF_PT_DYNAMIC 2u
#define ELF_PF_X       0x1u

// Upper bound on e_phnum.  Real PicoDeck apps have 3-8 program headers; the cap
// bounds the phdr allocation and rejects PN_XNUM (0xFFFF) outright.
#define ELF_PLAN_MAX_PHNUM 64u

typedef enum {
    ELF_OK = 0,
    ELF_ERR_TOO_SMALL,         // file shorter than an ELF header
    ELF_ERR_MAGIC,             // not \x7fELF
    ELF_ERR_CLASS,             // not ELFCLASS32 little-endian
    ELF_ERR_NOT_PIE,           // e_type != ET_DYN
    ELF_ERR_NOT_ARM,           // e_machine != EM_ARM
    ELF_ERR_FLOAT_ABI,         // built with -mfloat-abi=hard
    ELF_ERR_PHENTSIZE,        // e_phentsize != sizeof(Elf32_Phdr)
    ELF_ERR_PHNUM,             // e_phnum 0 or > ELF_PLAN_MAX_PHNUM
    ELF_ERR_PHDR_BOUNDS,       // phdr table outside the file (or wraps)
    ELF_ERR_SEG_FILESZ,        // PT_LOAD p_filesz > p_memsz
    ELF_ERR_SEG_FILE_BOUNDS,   // PT_LOAD p_offset + p_filesz outside the file
    ELF_ERR_SEG_VADDR_WRAP,    // PT_LOAD p_vaddr + p_memsz wraps
    ELF_ERR_NO_LOAD,           // no non-empty PT_LOAD
    ELF_ERR_IMAGE_TOO_LARGE,   // mem_max - mem_min > caller's limit
    ELF_ERR_ENTRY,             // e_entry outside the image
    ELF_ERR_DYN_BOUNDS,        // PT_DYNAMIC outside the image
    ELF_ERR_DYN_UNTERMINATED,  // no DT_NULL inside PT_DYNAMIC
    ELF_ERR_REL_BOUNDS,        // DT_REL/DT_RELA table outside the image,
                               // or its size is not a whole number of entries
    ELF_ERR_REL_TARGET,        // relocation r_offset outside the image
    ELF_ERR_REL_TYPE,          // relocation type the loader does not know
    ELF_ERR_ARGS,              // bad arguments (NULL, no regions)
} elf_err_t;

// Human-readable error for the loader's on-screen message and error.log.
const char *elf_strerror(elf_err_t err);

typedef struct {
    // From elf_plan_header().
    uint32_t entry;        // e_entry (Thumb bit included)
    uint32_t phoff;        // file offset of the phdr table
    uint32_t phdrs_size;   // bytes of the phdr table (phnum * ELF_PHDR_SIZE)
    uint16_t phnum;

    // From elf_plan_segments().
    uint32_t mem_min;      // lowest PT_LOAD p_vaddr
    uint32_t mem_max;      // highest PT_LOAD p_vaddr + p_memsz
    uint32_t image_size;   // mem_max - mem_min
    uint32_t entry_off;    // (entry & ~1) - mem_min, always < image_size
    int      code_idx;     // first PT_LOAD with PF_X, or -1
    uint32_t code_vaddr;   // that segment's p_vaddr ...
    uint32_t code_memsz;   // ... and p_memsz
    // True when the code segment is the lowest PT_LOAD and every other
    // PT_LOAD starts at or after its end - the layout the firmware's split
    // mode (code in SRAM, the rest in PSRAM) assumes.
    bool     code_first;
    int      dyn_idx;      // first PT_DYNAMIC, or -1
    uint32_t dyn_vaddr;    // its p_vaddr, inside the image
    uint32_t dyn_size;     // its p_memsz, inside the image
} elf_plan_t;

// Step 1.  buf holds the first buf_len bytes of the file (at least
// ELF_EHDR_SIZE); file_len is the whole file's size.  Fills plan->entry,
// phoff, phdrs_size, phnum.
elf_err_t elf_plan_header(const uint8_t *buf, uint32_t buf_len,
                          uint32_t file_len, elf_plan_t *plan);

// Step 2.  phdrs holds plan->phdrs_size bytes read from plan->phoff.
// max_image is the largest image the caller can place.
elf_err_t elf_plan_segments(elf_plan_t *plan, const uint8_t *phdrs,
                            uint32_t phdrs_len, uint32_t file_len,
                            uint32_t max_image);

// Program header idx of a table elf_plan_segments() accepted (idx < phnum).
// Copies out, so the table needs no particular alignment.
elf32_phdr_t elf_plan_phdr(const uint8_t *phdrs, uint16_t idx);

// Where a range of the image's virtual addresses lives once loaded.
typedef struct {
    uint32_t vaddr;  // ELF virtual address of the region's first byte
    uint32_t size;   // bytes
    uint8_t *ptr;    // where the loader reads and writes those bytes
    uint32_t base;   // address the region runs at; bias = base - vaddr
} elf_region_t;

typedef struct {
    uint32_t applied;   // R_ARM_RELATIVE entries rewritten
    uint32_t symbolic;  // R_ARM_ABS32/GLOB_DAT/JUMP_SLOT left as-is (no
                        // symbol table is loaded; in PicoDeck apps these name
                        // undefined weak symbols, which resolve to 0)
} elf_reloc_stats_t;

// Step 3.  regions map every image address the dynamic section can name
// (the firmware passes code + data in split mode, one region otherwise).
// A relocated pointer takes the bias of the region it points into, or of
// the LAST region when it points outside all of them (end-of-image symbols
// such as _end).  stats may be NULL.  Returns on the first bad entry; the
// image may then be partly relocated and must not run.
elf_err_t elf_relocate(const elf_plan_t *plan, const elf_region_t *regions,
                       int nregions, elf_reloc_stats_t *stats);
