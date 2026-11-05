#include "elf_plan.h"

#include <stddef.h>
#include <string.h>

// ELF constants (newlib-arm does not always ship <elf.h>).
#define EI_CLASS     4
#define EI_DATA      5
#define ELFCLASS32   1
#define ELFDATA2LSB  1
#define ET_DYN       3
#define EM_ARM       40
#define EF_ARM_ABI_FLOAT_HARD 0x00000400u

#define DT_NULL      0
#define DT_RELA      7
#define DT_RELASZ    8
#define DT_RELAENT   9
#define DT_REL       17
#define DT_RELSZ     18
#define DT_RELENT    19

#define R_ARM_NONE       0
#define R_ARM_ABS32      2
#define R_ARM_GLOB_DAT   21
#define R_ARM_JUMP_SLOT  22
#define R_ARM_RELATIVE   23

#define REL_SIZE   8u
#define RELA_SIZE  12u
#define DYN_SIZE   8u

_Static_assert(sizeof(elf32_ehdr_t) == ELF_EHDR_SIZE, "Elf32_Ehdr layout");
_Static_assert(sizeof(elf32_phdr_t) == ELF_PHDR_SIZE, "Elf32_Phdr layout");

const char *elf_strerror(elf_err_t err) {
    switch (err) {
    case ELF_OK:                   return "ELF: ok";
    case ELF_ERR_TOO_SMALL:        return "ELF: file too small";
    case ELF_ERR_MAGIC:            return "ELF: bad magic";
    case ELF_ERR_CLASS:            return "ELF: must be 32-bit little-endian";
    case ELF_ERR_NOT_PIE:          return "ELF: must be PIE (ET_DYN)";
    case ELF_ERR_NOT_ARM:          return "ELF: must be ARM";
    case ELF_ERR_FLOAT_ABI:        return "ELF: hard-float ABI (-mfloat-abi=hard) not supported";
    case ELF_ERR_PHENTSIZE:        return "ELF: bad phdr entry size";
    case ELF_ERR_PHNUM:            return "ELF: bad phdr count";
    case ELF_ERR_PHDR_BOUNDS:      return "ELF: phdr table out of bounds";
    case ELF_ERR_SEG_FILESZ:       return "ELF: segment filesz > memsz";
    case ELF_ERR_SEG_FILE_BOUNDS:  return "ELF: segment data out of bounds";
    case ELF_ERR_SEG_VADDR_WRAP:   return "ELF: segment vaddr overflow";
    case ELF_ERR_NO_LOAD:          return "ELF: no PT_LOAD segments";
    case ELF_ERR_IMAGE_TOO_LARGE:  return "ELF: image too large";
    case ELF_ERR_ENTRY:            return "ELF: entry point out of bounds";
    case ELF_ERR_DYN_BOUNDS:       return "ELF: dynamic section out of bounds";
    case ELF_ERR_DYN_UNTERMINATED: return "ELF: dynamic section unterminated";
    case ELF_ERR_REL_BOUNDS:       return "ELF: relocation table out of bounds";
    case ELF_ERR_REL_TARGET:       return "ELF: relocation target out of bounds";
    case ELF_ERR_REL_TYPE:         return "ELF: unsupported relocation type";
    case ELF_ERR_ARGS:             return "ELF: bad loader arguments";
    }
    return "ELF: unknown error";
}

static uint32_t rd32(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

static void wr32(uint8_t *p, uint32_t v) {
    memcpy(p, &v, sizeof(v));
}

// [off, off + len) inside [0, total)?  Overflow-safe.
static bool range_in(uint32_t off, uint32_t len, uint32_t total) {
    return off <= total && len <= total - off;
}

elf_err_t elf_plan_header(const uint8_t *buf, uint32_t buf_len,
                          uint32_t file_len, elf_plan_t *plan) {
    if (!buf || !plan)
        return ELF_ERR_ARGS;
    memset(plan, 0, sizeof(*plan));
    plan->code_idx = -1;
    plan->dyn_idx = -1;

    if (buf_len < ELF_EHDR_SIZE || file_len < ELF_EHDR_SIZE)
        return ELF_ERR_TOO_SMALL;

    elf32_ehdr_t eh;
    memcpy(&eh, buf, sizeof(eh));

    if (eh.e_ident[0] != 0x7f || eh.e_ident[1] != 'E' ||
        eh.e_ident[2] != 'L' || eh.e_ident[3] != 'F')
        return ELF_ERR_MAGIC;
    if (eh.e_ident[EI_CLASS] != ELFCLASS32 || eh.e_ident[EI_DATA] != ELFDATA2LSB)
        return ELF_ERR_CLASS;
    if (eh.e_type != ET_DYN)
        return ELF_ERR_NOT_PIE;
    if (eh.e_machine != EM_ARM)
        return ELF_ERR_NOT_ARM;
    // The API passes and returns floats in core registers (the firmware is
    // softfp). A hard-float app would use s0-s15 and read garbage: it links, it
    // loads, and only the calls that take or return a float go wrong.
    if (eh.e_flags & EF_ARM_ABI_FLOAT_HARD)
        return ELF_ERR_FLOAT_ABI;
    // Each phdr is read as an elf32_phdr_t; a shorter entry would make the
    // table walk read past the allocation.
    if (eh.e_phentsize != ELF_PHDR_SIZE)
        return ELF_ERR_PHENTSIZE;
    if (eh.e_phnum == 0 || eh.e_phnum > ELF_PLAN_MAX_PHNUM)
        return ELF_ERR_PHNUM;

    uint32_t size = (uint32_t)eh.e_phnum * ELF_PHDR_SIZE;  // <= 2048
    if (!range_in(eh.e_phoff, size, file_len))
        return ELF_ERR_PHDR_BOUNDS;

    plan->entry = eh.e_entry;
    plan->phoff = eh.e_phoff;
    plan->phdrs_size = size;
    plan->phnum = eh.e_phnum;
    return ELF_OK;
}

elf32_phdr_t elf_plan_phdr(const uint8_t *phdrs, uint16_t idx) {
    elf32_phdr_t ph;
    memcpy(&ph, phdrs + (size_t)idx * ELF_PHDR_SIZE, sizeof(ph));
    return ph;
}

elf_err_t elf_plan_segments(elf_plan_t *plan, const uint8_t *phdrs,
                            uint32_t phdrs_len, uint32_t file_len,
                            uint32_t max_image) {
    if (!plan || !phdrs || plan->phnum == 0)
        return ELF_ERR_ARGS;
    if (phdrs_len < plan->phdrs_size)
        return ELF_ERR_PHDR_BOUNDS;

    uint32_t mem_min = 0xFFFFFFFFu, mem_max = 0;
    bool found_load = false;
    plan->code_idx = -1;
    plan->dyn_idx = -1;

    for (uint16_t i = 0; i < plan->phnum; i++) {
        elf32_phdr_t ph = elf_plan_phdr(phdrs, i);
        if (ph.p_type == ELF_PT_DYNAMIC && plan->dyn_idx < 0) {
            plan->dyn_idx = i;
            plan->dyn_vaddr = ph.p_vaddr;
            plan->dyn_size = ph.p_memsz;
            continue;
        }
        if (ph.p_type != ELF_PT_LOAD)
            continue;
        // The loader copies p_filesz bytes into a p_memsz-sized slot.
        if (ph.p_filesz > ph.p_memsz)
            return ELF_ERR_SEG_FILESZ;
        if (ph.p_filesz > 0 && !range_in(ph.p_offset, ph.p_filesz, file_len))
            return ELF_ERR_SEG_FILE_BOUNDS;
        if (ph.p_memsz == 0)
            continue;
        uint32_t seg_end = ph.p_vaddr + ph.p_memsz;
        if (seg_end < ph.p_vaddr)
            return ELF_ERR_SEG_VADDR_WRAP;
        if (ph.p_vaddr < mem_min)
            mem_min = ph.p_vaddr;
        if (seg_end > mem_max)
            mem_max = seg_end;
        found_load = true;
        if ((ph.p_flags & ELF_PF_X) && plan->code_idx < 0) {
            plan->code_idx = i;
            plan->code_vaddr = ph.p_vaddr;
            plan->code_memsz = ph.p_memsz;
        }
    }

    if (!found_load)
        return ELF_ERR_NO_LOAD;

    uint32_t image_size = mem_max - mem_min;
    if (image_size > max_image)
        return ELF_ERR_IMAGE_TOO_LARGE;

    plan->mem_min = mem_min;
    plan->mem_max = mem_max;
    plan->image_size = image_size;

    uint32_t entry = plan->entry & ~1u;
    if (entry < mem_min || entry - mem_min >= image_size)
        return ELF_ERR_ENTRY;
    plan->entry_off = entry - mem_min;

    if (plan->dyn_idx >= 0 &&
        (plan->dyn_vaddr < mem_min ||
         !range_in(plan->dyn_vaddr - mem_min, plan->dyn_size, image_size)))
        return ELF_ERR_DYN_BOUNDS;

    // Split-mode layout: the code segment first, everything else above it.
    plan->code_first = false;
    if (plan->code_idx >= 0 && plan->code_vaddr == mem_min) {
        uint32_t code_end = plan->code_vaddr + plan->code_memsz;
        bool ok = true;
        for (uint16_t i = 0; i < plan->phnum && ok; i++) {
            elf32_phdr_t ph = elf_plan_phdr(phdrs, i);
            if (i == plan->code_idx || ph.p_type != ELF_PT_LOAD ||
                ph.p_memsz == 0)
                continue;
            if (ph.p_vaddr < code_end)
                ok = false;
        }
        plan->code_first = ok;
    }
    return ELF_OK;
}

// The region holding [vaddr, vaddr + len), or NULL.
static const elf_region_t *region_for(const elf_region_t *regions, int n,
                                      uint32_t vaddr, uint32_t len) {
    for (int i = 0; i < n; i++) {
        const elf_region_t *r = &regions[i];
        if (vaddr >= r->vaddr && range_in(vaddr - r->vaddr, len, r->size))
            return r;
    }
    return NULL;
}

static uint8_t *region_ptr(const elf_region_t *r, uint32_t vaddr) {
    return r->ptr + (vaddr - r->vaddr);
}

// Bias for a pointer value: its own region's, else the last region's.
static uint32_t bias_for(const elf_region_t *regions, int n, uint32_t value) {
    for (int i = 0; i < n; i++) {
        const elf_region_t *r = &regions[i];
        if (value >= r->vaddr && value - r->vaddr < r->size)
            return r->base - r->vaddr;
    }
    return regions[n - 1].base - regions[n - 1].vaddr;
}

static elf_err_t apply_table(const elf_region_t *regions, int n,
                             uint32_t addr, uint32_t size, uint32_t ent,
                             bool has_addend, elf_reloc_stats_t *st) {
    if (size % ent != 0)
        return ELF_ERR_REL_BOUNDS;
    const elf_region_t *tr = region_for(regions, n, addr, size);
    if (!tr)
        return ELF_ERR_REL_BOUNDS;
    const uint8_t *tab = region_ptr(tr, addr);

    for (uint32_t off = 0; off < size; off += ent) {
        uint32_t r_offset = rd32(tab + off);
        uint32_t r_info = rd32(tab + off + 4);
        uint32_t type = r_info & 0xffu;
        if (type == R_ARM_NONE)
            continue;
        if (type != R_ARM_RELATIVE && type != R_ARM_ABS32 &&
            type != R_ARM_GLOB_DAT && type != R_ARM_JUMP_SLOT)
            return ELF_ERR_REL_TYPE;
        const elf_region_t *wr = region_for(regions, n, r_offset, 4);
        if (!wr)
            return ELF_ERR_REL_TARGET;
        if (type != R_ARM_RELATIVE) {
            st->symbolic++;
            continue;
        }
        uint8_t *target = region_ptr(wr, r_offset);
        uint32_t value = has_addend ? rd32(tab + off + 8) : rd32(target);
        wr32(target, value + bias_for(regions, n, value));
        st->applied++;
    }
    return ELF_OK;
}

elf_err_t elf_relocate(const elf_plan_t *plan, const elf_region_t *regions,
                       int nregions, elf_reloc_stats_t *stats) {
    elf_reloc_stats_t local;
    if (!stats)
        stats = &local;
    stats->applied = 0;
    stats->symbolic = 0;
    if (!plan || !regions || nregions <= 0)
        return ELF_ERR_ARGS;
    if (plan->dyn_idx < 0)
        return ELF_OK;  // static PIE with nothing to relocate

    // The dynamic table must be fully inside one loaded region (it is inside
    // the image by elf_plan_segments; this also covers split mode).
    uint32_t dyn_len = plan->dyn_size - plan->dyn_size % DYN_SIZE;
    const elf_region_t *dr = region_for(regions, nregions, plan->dyn_vaddr,
                                        dyn_len);
    if (!dr)
        return ELF_ERR_DYN_BOUNDS;
    const uint8_t *dyn = region_ptr(dr, plan->dyn_vaddr);

    uint32_t rel_addr = 0, rel_size = 0, rel_ent = REL_SIZE;
    uint32_t rela_addr = 0, rela_size = 0, rela_ent = RELA_SIZE;
    bool terminated = false;
    for (uint32_t off = 0; off < dyn_len; off += DYN_SIZE) {
        int32_t tag = (int32_t)rd32(dyn + off);
        uint32_t val = rd32(dyn + off + 4);
        if (tag == DT_NULL) {
            terminated = true;
            break;
        }
        switch (tag) {
        case DT_REL:     rel_addr = val; break;
        case DT_RELSZ:   rel_size = val; break;
        case DT_RELENT:  rel_ent = val; break;
        case DT_RELA:    rela_addr = val; break;
        case DT_RELASZ:  rela_size = val; break;
        case DT_RELAENT: rela_ent = val; break;
        default: break;
        }
    }
    if (!terminated)
        return ELF_ERR_DYN_UNTERMINATED;
    if (rel_ent != REL_SIZE || rela_ent != RELA_SIZE)
        return ELF_ERR_REL_BOUNDS;

    // Same condition as the original loaders: a table needs both its
    // address and its size.
    if (rel_addr && rel_size) {
        elf_err_t e = apply_table(regions, nregions, rel_addr, rel_size,
                                  REL_SIZE, false, stats);
        if (e != ELF_OK)
            return e;
    }
    if (rela_addr && rela_size) {
        elf_err_t e = apply_table(regions, nregions, rela_addr, rela_size,
                                  RELA_SIZE, true, stats);
        if (e != ELF_OK)
            return e;
    }
    return ELF_OK;
}
