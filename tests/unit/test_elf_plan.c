// Host unit tests for src/os/elf_plan.c: the native-app ELF validation and
// relocation shared by the firmware loader and the simulator.
//
// Every malformed case from specs/test-audit-2026-09-24.md §3.5 plus the
// code review's Core Medium row (p_filesz vs p_memsz, e_phentsize,
// offset+size wrap, unbounded DT_* walk, rel table range, unknown
// relocation types) is built by mutating one minimal valid image.
#include "check.h"
#include "elf_plan.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ── A minimal valid ARM PIE ──────────────────────────────────────────────────
//
// file: [0,52) ehdr, [52,116) two phdrs (PT_LOAD, PT_DYNAMIC), segment bytes
// at file offset 0x80 mapped to vaddr 0 (filesz 0x80, memsz 0x100: 0x80 bss).
// Segment layout (vaddr):
//   0x00  word 0x10          R_ARM_RELATIVE target (points into the image)
//   0x04  word 0x200         R_ARM_RELATIVE target (points past the image)
//   0x08  code (entry 0x09, Thumb)
//   0x0c  word 0             R_ARM_ABS32 target (undefined weak symbol 1)
//   0x20  .rel.dyn: 3 entries (24 bytes)
//   0x40  .dynamic: DT_REL, DT_RELSZ, DT_RELENT, DT_NULL (32 bytes)
#define SEG_OFF   0x80u
#define SEG_FILE  0x80u
#define SEG_MEM   0x100u
#define FILE_LEN  (SEG_OFF + SEG_FILE)
#define PH_LOAD   0
#define PH_DYN    1
#define REL_VA    0x20u
#define DYN_VA    0x40u

typedef struct {
  uint8_t b[FILE_LEN];
} elf_img_t;

static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static uint32_t get32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }

static elf32_ehdr_t *ehdr(elf_img_t *e) { return (elf32_ehdr_t *)e->b; }
static elf32_phdr_t *phdr(elf_img_t *e, int i) {
  return (elf32_phdr_t *)(e->b + ELF_EHDR_SIZE + (size_t)i * ELF_PHDR_SIZE);
}
static uint8_t *seg(elf_img_t *e, uint32_t vaddr) { return e->b + SEG_OFF + vaddr; }

static void make_elf(elf_img_t *e) {
  memset(e, 0, sizeof(*e));
  elf32_ehdr_t *h = ehdr(e);
  memcpy(h->e_ident, "\x7f" "ELF", 4);
  h->e_ident[4] = 1;  // ELFCLASS32
  h->e_ident[5] = 1;  // ELFDATA2LSB
  h->e_ident[6] = 1;  // EV_CURRENT
  h->e_type = 3;      // ET_DYN
  h->e_machine = 40;  // EM_ARM
  h->e_version = 1;
  h->e_entry = 0x09;
  h->e_phoff = ELF_EHDR_SIZE;
  h->e_ehsize = ELF_EHDR_SIZE;
  h->e_phentsize = ELF_PHDR_SIZE;
  h->e_phnum = 2;

  elf32_phdr_t *l = phdr(e, PH_LOAD);
  l->p_type = ELF_PT_LOAD;
  l->p_offset = SEG_OFF;
  l->p_filesz = SEG_FILE;
  l->p_memsz = SEG_MEM;
  l->p_flags = 7;  // RWX, as the PicoDeck SDK links apps
  l->p_align = 4;

  elf32_phdr_t *d = phdr(e, PH_DYN);
  d->p_type = ELF_PT_DYNAMIC;
  d->p_offset = SEG_OFF + DYN_VA;
  d->p_vaddr = DYN_VA;
  d->p_filesz = d->p_memsz = 32;
  d->p_flags = 6;

  put32(seg(e, 0x00), 0x10);
  put32(seg(e, 0x04), 0x200);
  put32(seg(e, 0x0c), 0);
  // .rel.dyn
  put32(seg(e, REL_VA + 0), 0x00); put32(seg(e, REL_VA + 4), 23);
  put32(seg(e, REL_VA + 8), 0x04); put32(seg(e, REL_VA + 12), 23);
  put32(seg(e, REL_VA + 16), 0x0c); put32(seg(e, REL_VA + 20), (1u << 8) | 2);
  // .dynamic
  put32(seg(e, DYN_VA + 0), 17);  put32(seg(e, DYN_VA + 4), REL_VA);   // DT_REL
  put32(seg(e, DYN_VA + 8), 18);  put32(seg(e, DYN_VA + 12), 24);      // DT_RELSZ
  put32(seg(e, DYN_VA + 16), 19); put32(seg(e, DYN_VA + 20), 8);       // DT_RELENT
  put32(seg(e, DYN_VA + 24), 0);  put32(seg(e, DYN_VA + 28), 0);       // DT_NULL
}

#define MAX_IMAGE (7u * 1024u * 1024u)

// Header + segments over the whole file.
static elf_err_t plan_file(const uint8_t *buf, uint32_t len, elf_plan_t *p) {
  elf_err_t e = elf_plan_header(buf, len, len, p);
  if (e != ELF_OK)
    return e;
  return elf_plan_segments(p, buf + p->phoff, p->phdrs_size, len, MAX_IMAGE);
}

// Load like the loaders do (zeroed image, copy p_filesz per PT_LOAD) into
// img (SEG_MEM bytes) and relocate as one region based at `base`.
static elf_err_t load_and_relocate(elf_img_t *e, uint8_t *img, uint32_t base,
                                   elf_reloc_stats_t *st) {
  elf_plan_t p;
  elf_err_t err = plan_file(e->b, FILE_LEN, &p);
  if (err != ELF_OK)
    return err;
  memset(img, 0, p.image_size);
  for (uint16_t i = 0; i < p.phnum; i++) {
    elf32_phdr_t ph = elf_plan_phdr(e->b + p.phoff, i);
    if (ph.p_type == ELF_PT_LOAD && ph.p_filesz)
      memcpy(img + (ph.p_vaddr - p.mem_min), e->b + ph.p_offset, ph.p_filesz);
  }
  elf_region_t r = {p.mem_min, p.image_size, img, base};
  return elf_relocate(&p, &r, 1, st);
}

// ── Valid image ──────────────────────────────────────────────────────────────

static void test_valid_plan(void) {
  elf_img_t e;
  make_elf(&e);
  elf_plan_t p;
  CHECK_EQ_INT(plan_file(e.b, FILE_LEN, &p), ELF_OK);
  CHECK_EQ_U32(p.phnum, 2);
  CHECK_EQ_U32(p.phoff, ELF_EHDR_SIZE);
  CHECK_EQ_U32(p.phdrs_size, 64);
  CHECK_EQ_U32(p.mem_min, 0);
  CHECK_EQ_U32(p.mem_max, SEG_MEM);
  CHECK_EQ_U32(p.image_size, SEG_MEM);
  CHECK_EQ_U32(p.entry, 0x09);
  CHECK_EQ_U32(p.entry_off, 0x08);
  CHECK_EQ_INT(p.code_idx, PH_LOAD);
  CHECK_EQ_U32(p.code_memsz, SEG_MEM);
  CHECK(p.code_first);
  CHECK_EQ_INT(p.dyn_idx, PH_DYN);
  CHECK_EQ_U32(p.dyn_vaddr, DYN_VA);
  CHECK_EQ_U32(p.dyn_size, 32);
}

static void test_valid_relocate(void) {
  elf_img_t e;
  make_elf(&e);
  uint8_t img[SEG_MEM];
  elf_reloc_stats_t st;
  CHECK_EQ_INT(load_and_relocate(&e, img, 0x20000000u, &st), ELF_OK);
  CHECK_EQ_U32(get32(img + 0x00), 0x20000010u);
  CHECK_EQ_U32(get32(img + 0x04), 0x20000200u);  // past-the-end: last region's bias
  CHECK_EQ_U32(get32(img + 0x0c), 0);            // ABS32 to undefined weak: untouched
  CHECK_EQ_U32(st.applied, 2);
  CHECK_EQ_U32(st.symbolic, 1);
  CHECK_EQ_U32(get32(img + 0x80), 0);            // bss zero
}

// Firmware split mode: code (the first 0x80 bytes) in one buffer, the rest in
// another, each with its own run address.
static void test_split_regions(void) {
  elf_img_t e;
  make_elf(&e);
  // A second pointer, into the data half, at 0x10 (inside the code half).
  put32(seg(&e, 0x10), 0x90);
  put32(seg(&e, REL_VA + 16), 0x10);
  put32(seg(&e, REL_VA + 20), 23);

  elf_plan_t p;
  CHECK_EQ_INT(plan_file(e.b, FILE_LEN, &p), ELF_OK);
  uint8_t code[0x80], data[0x80];
  memcpy(code, e.b + SEG_OFF, 0x80);
  memset(data, 0, sizeof(data));
  elf_region_t r[2] = {{0x00, 0x80, code, 0x20001000u},
                       {0x80, 0x80, data, 0x11000000u}};
  elf_reloc_stats_t st;
  CHECK_EQ_INT(elf_relocate(&p, r, 2, &st), ELF_OK);
  CHECK_EQ_U32(get32(code + 0x00), 0x20001010u);            // code -> code bias
  CHECK_EQ_U32(get32(code + 0x10), 0x11000000u + 0x10u);    // data -> data bias
  CHECK_EQ_U32(get32(code + 0x04), 0x11000000u + 0x180u);   // past end -> data (last)
  CHECK_EQ_U32(st.applied, 3);
}

static void test_rela(void) {
  elf_img_t e;
  make_elf(&e);
  // Replace .rel with a one-entry .rela at the same place.
  put32(seg(&e, REL_VA + 0), 0x04);
  put32(seg(&e, REL_VA + 4), 23);
  put32(seg(&e, REL_VA + 8), 0x30);  // addend
  put32(seg(&e, DYN_VA + 0), 7);  put32(seg(&e, DYN_VA + 4), REL_VA);   // DT_RELA
  put32(seg(&e, DYN_VA + 8), 8);  put32(seg(&e, DYN_VA + 12), 12);     // DT_RELASZ
  put32(seg(&e, DYN_VA + 16), 9); put32(seg(&e, DYN_VA + 20), 12);     // DT_RELAENT
  uint8_t img[SEG_MEM];
  elf_reloc_stats_t st;
  CHECK_EQ_INT(load_and_relocate(&e, img, 0x1000, &st), ELF_OK);
  CHECK_EQ_U32(get32(img + 0x04), 0x1030);
  CHECK_EQ_U32(get32(img + 0x00), 0x10);  // not relocated
  CHECK_EQ_U32(st.applied, 1);
}

// No PT_DYNAMIC at all: nothing to relocate, still OK.
static void test_no_dynamic(void) {
  elf_img_t e;
  make_elf(&e);
  ehdr(&e)->e_phnum = 1;
  uint8_t img[SEG_MEM];
  CHECK_EQ_INT(load_and_relocate(&e, img, 0x1000, NULL), ELF_OK);
  CHECK_EQ_U32(get32(img), 0x10);
}

// Boundaries that must still be accepted.
static void test_exact_bounds_ok(void) {
  elf_img_t e;
  elf_plan_t p;
  // phdr table ending exactly at EOF.
  make_elf(&e);
  CHECK_EQ_INT(elf_plan_header(e.b, FILE_LEN, ELF_EHDR_SIZE + 64, &p), ELF_OK);
  // Segment data ending exactly at EOF (FILE_LEN) is the default image.
  make_elf(&e);
  CHECK_EQ_INT(plan_file(e.b, FILE_LEN, &p), ELF_OK);
  // filesz == memsz.
  make_elf(&e);
  phdr(&e, PH_LOAD)->p_memsz = SEG_FILE;
  CHECK_EQ_INT(plan_file(e.b, FILE_LEN, &p), ELF_OK);
  // Image exactly at the limit.
  make_elf(&e);
  CHECK_EQ_INT(elf_plan_header(e.b, FILE_LEN, FILE_LEN, &p), ELF_OK);
  CHECK_EQ_INT(elf_plan_segments(&p, e.b + p.phoff, p.phdrs_size, FILE_LEN, SEG_MEM), ELF_OK);
  CHECK_EQ_INT(elf_plan_segments(&p, e.b + p.phoff, p.phdrs_size, FILE_LEN, SEG_MEM - 1),
               ELF_ERR_IMAGE_TOO_LARGE);
  // Entry at the last image byte is fine, one past is not.
  make_elf(&e);
  ehdr(&e)->e_entry = SEG_MEM - 1;
  CHECK_EQ_INT(plan_file(e.b, FILE_LEN, &p), ELF_OK);
  ehdr(&e)->e_entry = SEG_MEM;
  CHECK_EQ_INT(plan_file(e.b, FILE_LEN, &p), ELF_ERR_ENTRY);
  // R_ARM_NONE entries are skipped.
  make_elf(&e);
  put32(seg(&e, REL_VA + 20), 0);
  uint8_t img[SEG_MEM];
  elf_reloc_stats_t st;
  CHECK_EQ_INT(load_and_relocate(&e, img, 0, &st), ELF_OK);
  CHECK_EQ_U32(st.symbolic, 0);
}

// ── Malformed images (spec §3.5 + Core Medium row) ───────────────────────────

typedef void (*mutate_fn)(elf_img_t *e, uint32_t *len);

static void m_truncated(elf_img_t *e, uint32_t *len) { (void)e; *len = 20; }
static void m_bad_magic(elf_img_t *e, uint32_t *len) { (void)len; e->b[1] = 'X'; }
static void m_class64(elf_img_t *e, uint32_t *len) { (void)len; e->b[4] = 2; }
static void m_big_endian(elf_img_t *e, uint32_t *len) { (void)len; e->b[5] = 2; }
static void m_et_exec(elf_img_t *e, uint32_t *len) { (void)len; ehdr(e)->e_type = 2; }
static void m_not_arm(elf_img_t *e, uint32_t *len) { (void)len; ehdr(e)->e_machine = 62; }
static void m_hard_float(elf_img_t *e, uint32_t *len) { (void)len; ehdr(e)->e_flags = 0x05000400; }
static void m_phnum0(elf_img_t *e, uint32_t *len) { (void)len; ehdr(e)->e_phnum = 0; }
static void m_phnum_ffff(elf_img_t *e, uint32_t *len) { (void)len; ehdr(e)->e_phnum = 0xFFFF; }
static void m_phentsize8(elf_img_t *e, uint32_t *len) { (void)len; ehdr(e)->e_phentsize = 8; }
static void m_phentsize64(elf_img_t *e, uint32_t *len) { (void)len; ehdr(e)->e_phentsize = 64; }
static void m_phoff_wrap(elf_img_t *e, uint32_t *len) { (void)len; ehdr(e)->e_phoff = 0xFFFFFFF0u; }
static void m_phoff_past_eof(elf_img_t *e, uint32_t *len) { (void)len; ehdr(e)->e_phoff = FILE_LEN - 32; }
static void m_seg_offset_wrap(elf_img_t *e, uint32_t *len) {
  (void)len; phdr(e, PH_LOAD)->p_offset = 0xFFFFFF00u; phdr(e, PH_LOAD)->p_filesz = 0x100;
}
static void m_seg_past_eof(elf_img_t *e, uint32_t *len) { (void)len; phdr(e, PH_LOAD)->p_offset = SEG_OFF + 4; }
static void m_filesz_gt_memsz(elf_img_t *e, uint32_t *len) {
  // The truncated-p_filesz case: the loaders copied p_filesz but only bounds-
  // checked p_memsz, so this overran the destination slot.
  (void)len; phdr(e, PH_LOAD)->p_memsz = SEG_FILE - 4;
}
static void m_vaddr_wrap(elf_img_t *e, uint32_t *len) {
  (void)len; phdr(e, PH_LOAD)->p_vaddr = 0xFFFFFF80u;
}
static void m_no_load(elf_img_t *e, uint32_t *len) { (void)len; phdr(e, PH_LOAD)->p_type = 6; }
static void m_empty_load(elf_img_t *e, uint32_t *len) {
  (void)len; phdr(e, PH_LOAD)->p_filesz = 0; phdr(e, PH_LOAD)->p_memsz = 0;
}
static void m_entry_outside(elf_img_t *e, uint32_t *len) { (void)len; ehdr(e)->e_entry = 0x4001; }
static void m_dyn_outside(elf_img_t *e, uint32_t *len) { (void)len; phdr(e, PH_DYN)->p_vaddr = 0x1000; }
static void m_dyn_overhang(elf_img_t *e, uint32_t *len) {
  (void)len; phdr(e, PH_DYN)->p_vaddr = SEG_MEM - 8; phdr(e, PH_DYN)->p_memsz = 16;
}
static void m_dyn_size_wrap(elf_img_t *e, uint32_t *len) { (void)len; phdr(e, PH_DYN)->p_memsz = 0xFFFFFFF8u; }
static void m_rel_outside(elf_img_t *e, uint32_t *len) { (void)len; put32(seg(e, DYN_VA + 4), 0x4000); }
static void m_relsz_huge(elf_img_t *e, uint32_t *len) { (void)len; put32(seg(e, DYN_VA + 12), 0x7FFFFFF8u); }
static void m_relsz_wrap(elf_img_t *e, uint32_t *len) {
  (void)len; put32(seg(e, DYN_VA + 4), 0xF0); put32(seg(e, DYN_VA + 12), 0xFFFFFF18u);
}
static void m_relsz_partial(elf_img_t *e, uint32_t *len) { (void)len; put32(seg(e, DYN_VA + 12), 20); }
static void m_relent_12(elf_img_t *e, uint32_t *len) { (void)len; put32(seg(e, DYN_VA + 20), 12); }
static void m_roffset_outside(elf_img_t *e, uint32_t *len) { (void)len; put32(seg(e, REL_VA + 8), 0x5000); }
static void m_roffset_last_bytes(elf_img_t *e, uint32_t *len) {
  (void)len; put32(seg(e, REL_VA + 8), SEG_MEM - 2);  // 4-byte write would overhang
}
static void m_roffset_wrap(elf_img_t *e, uint32_t *len) { (void)len; put32(seg(e, REL_VA + 8), 0xFFFFFFFEu); }
static void m_unknown_rel_type(elf_img_t *e, uint32_t *len) {
  (void)len; put32(seg(e, REL_VA + 12), 10);  // R_ARM_THM_CALL: was silently skipped
}
static void m_symbolic_outside(elf_img_t *e, uint32_t *len) { (void)len; put32(seg(e, REL_VA + 16), 0x9000); }
static void m_no_dt_null(elf_img_t *e, uint32_t *len) {
  // Unbounded DT_* walk: without DT_NULL the old loop ran off the table.
  (void)len; put32(seg(e, DYN_VA + 24), 19); put32(seg(e, DYN_VA + 28), 8);
}
static void m_dyn_zero_size(elf_img_t *e, uint32_t *len) { (void)len; phdr(e, PH_DYN)->p_memsz = 4; }

typedef struct {
  const char *name;
  mutate_fn mutate;
  elf_err_t want;
} mcase_t;

static const mcase_t k_cases[] = {
    {"truncated to 20 bytes", m_truncated, ELF_ERR_TOO_SMALL},
    {"bad magic", m_bad_magic, ELF_ERR_MAGIC},
    {"ELFCLASS64", m_class64, ELF_ERR_CLASS},
    {"big-endian", m_big_endian, ELF_ERR_CLASS},
    {"e_type ET_EXEC", m_et_exec, ELF_ERR_NOT_PIE},
    {"e_machine x86-64", m_not_arm, ELF_ERR_NOT_ARM},
    {"e_phnum 0", m_phnum0, ELF_ERR_PHNUM},
    {"e_phnum 0xFFFF", m_phnum_ffff, ELF_ERR_PHNUM},
    {"e_phentsize 8", m_phentsize8, ELF_ERR_PHENTSIZE},
    {"e_phentsize 64", m_phentsize64, ELF_ERR_PHENTSIZE},
    {"e_phoff 0xFFFFFFF0", m_phoff_wrap, ELF_ERR_PHDR_BOUNDS},
    {"phdr table past EOF", m_phoff_past_eof, ELF_ERR_PHDR_BOUNDS},
    {"p_offset+p_filesz wrap", m_seg_offset_wrap, ELF_ERR_SEG_FILE_BOUNDS},
    {"segment past EOF", m_seg_past_eof, ELF_ERR_SEG_FILE_BOUNDS},
    {"p_filesz > p_memsz", m_filesz_gt_memsz, ELF_ERR_SEG_FILESZ},
    {"p_vaddr+p_memsz wrap", m_vaddr_wrap, ELF_ERR_SEG_VADDR_WRAP},
    {"no PT_LOAD", m_no_load, ELF_ERR_NO_LOAD},
    {"only an empty PT_LOAD", m_empty_load, ELF_ERR_NO_LOAD},
    {"entry outside image", m_entry_outside, ELF_ERR_ENTRY},
    {"PT_DYNAMIC outside image", m_dyn_outside, ELF_ERR_DYN_BOUNDS},
    {"PT_DYNAMIC overhangs image", m_dyn_overhang, ELF_ERR_DYN_BOUNDS},
    {"PT_DYNAMIC size wraps", m_dyn_size_wrap, ELF_ERR_DYN_BOUNDS},
    {"DT_REL outside image", m_rel_outside, ELF_ERR_REL_BOUNDS},
    {"DT_RELSZ huge", m_relsz_huge, ELF_ERR_REL_BOUNDS},
    {"DT_REL+DT_RELSZ wrap", m_relsz_wrap, ELF_ERR_REL_BOUNDS},
    {"DT_RELSZ not a multiple of 8", m_relsz_partial, ELF_ERR_REL_BOUNDS},
    {"DT_RELENT 12", m_relent_12, ELF_ERR_REL_BOUNDS},
    {"r_offset outside image", m_roffset_outside, ELF_ERR_REL_TARGET},
    {"r_offset in the last 2 bytes", m_roffset_last_bytes, ELF_ERR_REL_TARGET},
    {"r_offset wraps", m_roffset_wrap, ELF_ERR_REL_TARGET},
    {"unknown relocation type", m_unknown_rel_type, ELF_ERR_REL_TYPE},
    {"symbolic reloc outside image", m_symbolic_outside, ELF_ERR_REL_TARGET},
    {"no DT_NULL terminator", m_no_dt_null, ELF_ERR_DYN_UNTERMINATED},
    {"PT_DYNAMIC smaller than one entry", m_dyn_zero_size, ELF_ERR_DYN_UNTERMINATED},
};

static void test_malformed(void) {
  for (size_t i = 0; i < sizeof(k_cases) / sizeof(k_cases[0]); i++) {
    const mcase_t *c = &k_cases[i];
    elf_img_t e;
    make_elf(&e);
    uint32_t len = FILE_LEN;
    c->mutate(&e, &len);

    // Planning and relocation over a heap copy of exactly `len` bytes, so
    // ASan flags any read past the file.
    uint8_t *file = malloc(len);
    memcpy(file, e.b, len);
    elf_plan_t p;
    elf_err_t got = elf_plan_header(file, len, len, &p);
    if (got == ELF_OK) {
      uint8_t *ph = malloc(p.phdrs_size);
      memcpy(ph, file + p.phoff, p.phdrs_size);
      got = elf_plan_segments(&p, ph, p.phdrs_size, len, MAX_IMAGE);
      if (got == ELF_OK) {
        uint8_t *img = calloc(1, p.image_size);
        for (uint16_t k = 0; k < p.phnum; k++) {
          elf32_phdr_t s = elf_plan_phdr(ph, k);
          if (s.p_type == ELF_PT_LOAD && s.p_filesz)
            memcpy(img + (s.p_vaddr - p.mem_min), file + s.p_offset, s.p_filesz);
        }
        elf_region_t r = {p.mem_min, p.image_size, img, 0x1000};
        got = elf_relocate(&p, &r, 1, NULL);
        free(img);
      }
      free(ph);
    }
    free(file);
    if (got != c->want)
      printf("  case '%s': got %d (%s), want %d (%s)\n", c->name, got,
             elf_strerror(got), c->want, elf_strerror(c->want));
    CHECK_EQ_INT(got, c->want);
  }
}

// The API passes floats in core registers: soft and softfp builds load, a hard
// one is refused (its drawPlane arguments would be garbage).
static void test_float_abi(void) {
  elf_img_t e;
  elf_plan_t p;
  make_elf(&e);
  ehdr(&e)->e_flags = 0x05000200;  // EABI5, EF_ARM_ABI_FLOAT_SOFT: soft, softfp
  CHECK_EQ_INT(plan_file(e.b, FILE_LEN, &p), ELF_OK);
  ehdr(&e)->e_flags = 0x05000000;  // EABI5, no float flag: elfgen's images
  CHECK_EQ_INT(plan_file(e.b, FILE_LEN, &p), ELF_OK);
  m_hard_float(&e, NULL);
  CHECK_EQ_INT(plan_file(e.b, FILE_LEN, &p), ELF_ERR_FLOAT_ABI);
  CHECK(strstr(elf_strerror(ELF_ERR_FLOAT_ABI), "hard") != NULL);
}

// Split mode is only offered for a code-first layout: a data segment below
// the code segment would underflow the data offset.
static void test_code_first(void) {
  elf_img_t e;
  make_elf(&e);
  // Turn PT_DYNAMIC into a second PT_LOAD below... well, overlapping the code.
  elf32_phdr_t *d = phdr(&e, PH_DYN);
  d->p_type = ELF_PT_LOAD;
  d->p_vaddr = 0x40;
  elf_plan_t p;
  CHECK_EQ_INT(plan_file(e.b, FILE_LEN, &p), ELF_OK);
  CHECK(!p.code_first);
  // Data wholly above the code: code_first.
  make_elf(&e);
  phdr(&e, PH_LOAD)->p_memsz = 0x80;
  d = phdr(&e, PH_DYN);
  d->p_type = ELF_PT_LOAD;
  d->p_vaddr = 0x80;
  d->p_flags = 6;
  CHECK_EQ_INT(plan_file(e.b, FILE_LEN, &p), ELF_OK);
  CHECK(p.code_first);
  CHECK_EQ_U32(p.image_size, 0xa0);
  // Code not lowest.
  phdr(&e, PH_LOAD)->p_flags = 6;
  d->p_flags = 7;
  CHECK_EQ_INT(plan_file(e.b, FILE_LEN, &p), ELF_OK);
  CHECK_EQ_INT(p.code_idx, PH_DYN);
  CHECK(!p.code_first);
}

static void test_null_args(void) {
  elf_plan_t p;
  uint8_t b[ELF_EHDR_SIZE] = {0};
  CHECK_EQ_INT(elf_plan_header(NULL, 52, 52, &p), ELF_ERR_ARGS);
  CHECK_EQ_INT(elf_plan_header(b, 52, 52, NULL), ELF_ERR_ARGS);
  CHECK_EQ_INT(elf_relocate(NULL, NULL, 0, NULL), ELF_ERR_ARGS);
  for (int e = ELF_OK; e <= ELF_ERR_ARGS; e++)
    CHECK(strncmp(elf_strerror((elf_err_t)e), "ELF: ", 5) == 0);
}

// ── Real apps: every committed main.elf must still plan and relocate ─────────

static uint8_t *read_file(const char *path, uint32_t *len) {
  FILE *f = fopen(path, "rb");
  if (!f)
    return NULL;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  uint8_t *b = malloc(n > 0 ? (size_t)n : 1);
  if (fread(b, 1, (size_t)n, f) != (size_t)n) {
    free(b);
    b = NULL;
  }
  fclose(f);
  *len = (uint32_t)n;
  return b;
}

static void check_real_elf(const char *rel, bool required) {
  char path[512];
  if (rel[0] == '/')
    snprintf(path, sizeof(path), "%s", rel);
  else
    snprintf(path, sizeof(path), "%s/%s", PICODECK_ROOT, rel);
  uint32_t len;
  uint8_t *b = read_file(path, &len);
  if (!b) {
    if (required)
      printf("FAIL missing %s\n", path);
    CHECK(!required);
    return;
  }
  elf_plan_t p;
  elf_err_t err = plan_file(b, len, &p);
  if (err != ELF_OK)
    printf("  %s: %s\n", rel, elf_strerror(err));
  CHECK_EQ_INT(err, ELF_OK);
  if (err == ELF_OK) {
    CHECK(p.code_first);
    uint8_t *img = calloc(1, p.image_size);
    for (uint16_t i = 0; i < p.phnum; i++) {
      elf32_phdr_t ph = elf_plan_phdr(b + p.phoff, i);
      if (ph.p_type == ELF_PT_LOAD && ph.p_filesz)
        memcpy(img + (ph.p_vaddr - p.mem_min), b + ph.p_offset, ph.p_filesz);
    }
    elf_region_t r = {p.mem_min, p.image_size, img, 0x11000000u};
    elf_reloc_stats_t st;
    err = elf_relocate(&p, &r, 1, &st);
    if (err != ELF_OK)
      printf("  %s: %s\n", rel, elf_strerror(err));
    CHECK_EQ_INT(err, ELF_OK);
    printf("  %s: image %u bytes, %u relocations applied, %u symbolic\n", rel,
           (unsigned)p.image_size, (unsigned)st.applied, (unsigned)st.symbolic);
    free(img);
  }
  free(b);
}

static void test_real_apps(void) {
  check_real_elf("apps/hello_c/main.elf", true);
  check_real_elf("sdk/native/main.elf", true);
  check_real_elf("apps/c64/main.elf", true);
  // Gitignored / out-of-tree builds: checked when present.
  check_real_elf("apps/dos86/main.elf", false);
  check_real_elf("apps/gbc/main.elf", false);
  check_real_elf("apps/tic-80/main.elf", false);
  check_real_elf("apps/doom/main.elf", false);
}

// Extra ELFs named on the command line (absolute or repo-relative), e.g.
//   build_unit/test_elf_plan /path/to/doom/main.elf
int main(int argc, char **argv) {
  for (int i = 1; i < argc; i++)
    check_real_elf(argv[i], true);
  if (argc > 1)
    return check_report("test_elf_plan (extra ELFs)");

  test_valid_plan();
  test_valid_relocate();
  test_split_regions();
  test_rela();
  test_no_dynamic();
  test_exact_bounds_ok();
  test_malformed();
  test_float_abi();
  test_code_first();
  test_null_args();
  test_real_apps();
  return check_report("test_elf_plan");
}
