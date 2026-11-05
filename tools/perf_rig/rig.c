// perf_rig: runs a bare-metal Cortex-M33 guest ELF in Unicorn, feeds every
// instruction fetch and data access through a model of the RP2350 XIP cache,
// and estimates the time. An estimate, never a measurement: README.md says what
// it models, what it leaves out and how far to trust it.
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unicorn/unicorn.h>

// ── Address map ─────────────────────────────────────────────────────────────
#define IS_FLASH(a) (((a) >> 24) == 0x10)   // XIP CS0
#define IS_PSRAM(a) (((a) >> 24) == 0x11)   // XIP CS1, the cached alias
#define IS_CACHED(a) (IS_FLASH(a) || IS_PSRAM(a))
#define RIG_BASE 0x4FF00000u

// ── Cost parameters (cycles) ────────────────────────────────────────────────
typedef struct {
  const char *name;
  double v;
  const char *why;
} param_t;
enum { P_INSTR, P_LOAD, P_STORE, P_BRANCH, P_FDIV, P_FLASH, P_PSRAM, P_WB, P_HIT, P_MHZ, P_COUNT };
static param_t s_param[P_COUNT] = {
    [P_INSTR] = {"instr", 1, "base cycles per instruction"},
    [P_LOAD] = {"load", 1, "extra cycles per word loaded (LDR 2, LDM 1+N)"},
    [P_STORE] = {"store", 1, "extra cycles per word stored"},
    [P_BRANCH] = {"branch", 2, "extra cycles per taken branch (M4 table: 1-3)"},
    [P_FDIV] = {"fdiv", 14, "total cycles of VDIV.F32 and VSQRT.F32"},
    [P_FLASH] = {"flash-line", 56, "stall for a flash line fill, the brief's figure"},
    [P_PSRAM] = {"psram-line", 80, "stall for a PSRAM line fill, the brief's figure"},
    [P_WB] = {"writeback", 64, "stall for a dirty line's write-back, the brief's figure"},
    [P_HIT] = {"xip-hit", 0, "extra cycles per data access that hits the XIP cache"},
    [P_MHZ] = {"mhz", 200, "system clock"},
};

// ── Counters, per phase ─────────────────────────────────────────────────────
enum { PH_IDLE, PH_GEOM, PH_RASTER, PH_COUNT };
typedef struct {
  uint64_t instr, fp, fdiv, loads, stores, taken;
  uint64_t fmiss_flash, fmiss_psram, dmiss_flash, dmiss_psram, wb, dhit;
} counters_t;
static counters_t s_c[PH_COUNT];
static int s_phase;

static double cycles_of(const counters_t *c) {
  return (double)c->instr * s_param[P_INSTR].v + (double)c->loads * s_param[P_LOAD].v +
         (double)c->stores * s_param[P_STORE].v + (double)c->taken * s_param[P_BRANCH].v +
         (double)c->fdiv * (s_param[P_FDIV].v - s_param[P_INSTR].v) +
         (double)(c->fmiss_flash + c->dmiss_flash) * s_param[P_FLASH].v +
         (double)(c->fmiss_psram + c->dmiss_psram) * s_param[P_PSRAM].v +
         (double)c->wb * s_param[P_WB].v + (double)c->dhit * s_param[P_HIT].v;
}

// ── XIP cache model ─────────────────────────────────────────────────────────
// 16 KB, 2 ways, 8-byte lines: 1024 sets, set = (addr / 8) mod 1024, so a line's
// set is its address modulo 8 KB. Flash and PSRAM share it (the tag is the whole
// line address). Write-back, write-allocate.
#define SETS 1024
enum { POLICY_LRU, POLICY_FIFO, POLICY_RANDOM };
static int s_policy = POLICY_LRU;
static uint32_t s_tag[SETS][2];
static uint8_t s_valid[SETS][2], s_dirty[SETS][2];
static uint8_t s_next[SETS];  // the way to evict next
static uint32_t s_rng = 0x2545F491u;

static uint32_t rng(void) {
  s_rng ^= s_rng << 13;
  s_rng ^= s_rng >> 17;
  s_rng ^= s_rng << 5;
  return s_rng;
}

// Returns true on a hit. Miss costs are charged by the caller's kind.
static inline bool cache_access(uint32_t addr, bool write, bool fetch) {
  const uint32_t line = addr & ~7u, set = (addr >> 3) & (SETS - 1);
  counters_t *c = &s_c[s_phase];
  for (int w = 0; w < 2; w++) {
    if (s_valid[set][w] && s_tag[set][w] == line) {
      if (write) s_dirty[set][w] = 1;
      if (s_policy == POLICY_LRU) s_next[set] = (uint8_t)(1 - w);
      return true;
    }
  }
  int v;
  if (!s_valid[set][0]) v = 0;
  else if (!s_valid[set][1]) v = 1;
  else if (s_policy == POLICY_RANDOM) v = (int)(rng() >> 7) & 1;
  else v = s_next[set];
  if (s_valid[set][v] && s_dirty[set][v]) c->wb++;
  s_valid[set][v] = 1;
  s_dirty[set][v] = write;
  s_tag[set][v] = line;
  s_next[set] = (uint8_t)(1 - v);
  if (fetch) {
    if (IS_FLASH(addr)) c->fmiss_flash++;
    else c->fmiss_psram++;
  } else {
    if (IS_FLASH(addr)) c->dmiss_flash++;
    else c->dmiss_psram++;
  }
  return false;
}

// What ran between frames on the device: at 16 KB the XIP cache has long been
// swept by the Lua VM and the game. Every line is evicted (dirty ones written
// back, charged to no phase) and a fraction of them are left dirty lines of
// someone else, which gfx3d's own misses will have to write back.
static double s_dirty_frac = 0.0;
static bool s_cold_frames = true;
static void frame_start_model(void) {
  if (!s_cold_frames) return;
  for (int s = 0; s < SETS; s++)
    for (int w = 0; w < 2; w++) {
      const bool d = (double)(rng() & 0xFFFF) / 65536.0 < s_dirty_frac;
      s_valid[s][w] = d;
      s_dirty[s][w] = d;
      s_tag[s][w] = 0x11F00000u + (uint32_t)(s * 8) + (uint32_t)w * 0x10000u;
    }
}

// ── Instruction classes ─────────────────────────────────────────────────────
#define CL_KNOWN 1
#define CL_FP 2     // a VFP data-processing instruction (not a load, store, or core move)
#define CL_FDIV 4   // VDIV.F32, VSQRT.F32
static uint8_t s_class[1u << 21];

static uint8_t classify(uc_engine *uc, uint32_t pc, uint32_t size) {
  uint16_t hw[2] = {0, 0};
  uc_mem_read(uc, pc, hw, size >= 4 ? 4 : 2);
  uint8_t cl = CL_KNOWN;
  if (size >= 4 && (hw[1] & 0x0E00) == 0x0A00) {
    const uint16_t a = hw[0], b = hw[1];
    if ((a & 0xEF00) == 0xEE00 && !(b & 0x0010)) {
      cl |= CL_FP;  // CDP-space VFP: VADD, VMUL, VCMP, VCVT, VMOV reg/imm, VRINT...
      if ((a & 0xFFB0) == 0xEE80 && !(b & 0x0040)) cl |= CL_FDIV;  // VDIV
      if ((a & 0xFFBF) == 0xEEB1 && (b & 0x00C0) == 0x00C0) cl |= CL_FDIV;  // VSQRT
    }
    // 0xFExx: VSEL, VMAXNM, VMINNM, VRINTx
    if ((a & 0xFF00) == 0xFE00 && !(b & 0x0010)) cl |= CL_FP;
  }
  return cl;
}

// ── Hooks ───────────────────────────────────────────────────────────────────
static uint32_t s_next_pc, s_last_line = 1;

static void hook_code(uc_engine *uc, uint64_t addr, uint32_t size, void *ud) {
  (void)ud;
  const uint32_t pc = (uint32_t)addr;
  counters_t *c = &s_c[s_phase];
  c->instr++;
  if (pc != s_next_pc) c->taken++;
  s_next_pc = pc + size;
  if (IS_CACHED(pc)) {
    // One access each time the fetch moves to another line. Repeats within a
    // line could not change the result: its way is already the most recent.
    const uint32_t line = pc & ~7u;
    if (line != s_last_line) {
      s_last_line = line;
      cache_access(pc, false, true);
    }
    const uint32_t end = (pc + size - 1) & ~7u;
    if (end != line) {
      s_last_line = end;
      cache_access(end, false, true);
    }
  }
  uint8_t *pcl = &s_class[(pc >> 1) & ((1u << 21) - 1)];
  if (!*pcl) *pcl = classify(uc, pc, size);
  if (*pcl & CL_FP) {
    c->fp++;
    if (*pcl & CL_FDIV) c->fdiv++;
  }
}

// ── Frames and results ──────────────────────────────────────────────────────
#define NSTAT 5
static uint64_t s_stat_sum[NSTAT], s_stat_max[NSTAT];
static uint32_t s_stat[NSTAT];
static int s_frames;
static uint64_t s_fnv = 0xcbf29ce484222325ull;
static uint16_t s_fb_copy[320 * 320];
static bool s_done;
static double s_frame_cycles_geom[4096], s_frame_cycles_raster[4096];
static double s_mark_geom, s_mark_raster;

static void doorbell(uc_engine *uc, uint32_t a, uint32_t value) {
  const uint32_t off = a - RIG_BASE;
  if (off == 0x00) {
    s_phase = value < PH_COUNT ? (int)value : PH_IDLE;
  } else if (off == 0x04) {
    s_done = true;
    uc_emu_stop(uc);
  } else if (off == 0x08) {
    frame_start_model();
    s_mark_geom = cycles_of(&s_c[PH_GEOM]);
    s_mark_raster = cycles_of(&s_c[PH_RASTER]);
  } else if (off == 0x0C) {
    uc_mem_read(uc, value, s_fb_copy, sizeof s_fb_copy);
    const uint8_t *b = (const uint8_t *)s_fb_copy;
    for (size_t i = 0; i < sizeof s_fb_copy; i++) {
      s_fnv ^= b[i];
      s_fnv *= 0x100000001b3ull;
    }
    for (int i = 0; i < NSTAT; i++) {
      s_stat_sum[i] += s_stat[i];
      if (s_stat[i] > s_stat_max[i]) s_stat_max[i] = s_stat[i];
    }
    if (s_frames < 4096) {
      s_frame_cycles_geom[s_frames] = cycles_of(&s_c[PH_GEOM]) - s_mark_geom;
      s_frame_cycles_raster[s_frames] = cycles_of(&s_c[PH_RASTER]) - s_mark_raster;
    }
    s_frames++;
  } else if (off >= 0x20 && off < 0x20 + 4 * NSTAT) {
    s_stat[(off - 0x20) / 4] = value;
  }
}

static void hook_mem(uc_engine *uc, uc_mem_type type, uint64_t addr, int size, int64_t value,
                     void *ud) {
  (void)ud;
  const uint32_t a = (uint32_t)addr;
  if ((a & 0xFFFFF000u) == RIG_BASE) {
    if (type == UC_MEM_WRITE) doorbell(uc, a, (uint32_t)value);
    return;
  }
  counters_t *c = &s_c[s_phase];
  const bool wr = type == UC_MEM_WRITE;
  const int xfers = (size + 3) / 4;
  if (wr) c->stores += (uint64_t)xfers;
  else c->loads += (uint64_t)xfers;
  if (IS_CACHED(a)) {
    if (cache_access(a, wr, false)) c->dhit++;
    const uint32_t end = a + (uint32_t)size - 1;
    if ((end ^ a) & ~7u) {
      if (cache_access(end, wr, false)) c->dhit++;
    }
  }
}

static bool hook_bad(uc_engine *uc, uc_mem_type type, uint64_t addr, int size, int64_t value,
                     void *ud) {
  (void)ud;
  (void)value;
  uint32_t pc = 0;
  uc_reg_read(uc, UC_ARM_REG_PC, &pc);
  fprintf(stderr, "rig: bad memory access type %d at 0x%08" PRIx64 " size %d, pc 0x%08x\n", type,
          addr, size, pc);
  return false;
}

static void hook_intr(uc_engine *uc, uint32_t no, void *ud) {
  (void)ud;
  uint32_t pc = 0;
  uc_reg_read(uc, UC_ARM_REG_PC, &pc);
  fprintf(stderr, "rig: guest exception %u at pc 0x%08x\n", no, pc);
  uc_emu_stop(uc);
}

// ── ELF ─────────────────────────────────────────────────────────────────────
static uint32_t load_elf(uc_engine *uc, const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) {
    perror(path);
    exit(2);
  }
  fseek(f, 0, SEEK_END);
  const long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  uint8_t *b = malloc((size_t)n);
  if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) exit(2);
  fclose(f);
  uint32_t entry, phoff;
  uint16_t phentsize, phnum;
  memcpy(&entry, b + 24, 4);
  memcpy(&phoff, b + 28, 4);
  memcpy(&phentsize, b + 42, 2);
  memcpy(&phnum, b + 44, 2);
  for (int i = 0; i < phnum; i++) {
    const uint8_t *ph = b + phoff + (uint32_t)i * phentsize;
    uint32_t type, off, vaddr, filesz;
    memcpy(&type, ph, 4);
    memcpy(&off, ph + 4, 4);
    memcpy(&vaddr, ph + 8, 4);
    memcpy(&filesz, ph + 16, 4);
    if (type != 1 || !filesz) continue;
    uc_err e = uc_mem_write(uc, vaddr, b + off, filesz);
    if (e != UC_ERR_OK) {
      fprintf(stderr, "rig: cannot load %u bytes at 0x%08x: %s\n", filesz, vaddr, uc_strerror(e));
      exit(2);
    }
  }
  free(b);
  return entry;
}

// ── Report ──────────────────────────────────────────────────────────────────
static void row_u(const char *name, uint64_t g, uint64_t r, int n) {
  printf("  %-24s %12.0f %12.0f %12.0f\n", name, (double)g / n, (double)r / n,
         (double)(g + r) / n);
}

static int cmp_d(const void *a, const void *b) {
  const double x = *(const double *)a, y = *(const double *)b;
  return x < y ? -1 : x > y;
}

static void report(const char *label, bool show_device) {
  const int n = s_frames ? s_frames : 1;
  const counters_t *g = &s_c[PH_GEOM], *r = &s_c[PH_RASTER];
  const double mhz = s_param[P_MHZ].v;
  const double cg = cycles_of(g) / n, cr = cycles_of(r) / n;
  printf("perf_rig: %s, %d frames, per-frame averages (ESTIMATE, not a measurement)\n", label,
         s_frames);
  printf("  %-24s %12s %12s %12s\n", "", "geometry", "raster", "total");
  row_u("instructions", g->instr, r->instr, n);
  row_u("FP operations", g->fp, r->fp, n);
  row_u("VDIV+VSQRT", g->fdiv, r->fdiv, n);
  row_u("words loaded", g->loads, r->loads, n);
  row_u("words stored", g->stores, r->stores, n);
  row_u("taken branches", g->taken, r->taken, n);
  row_u("fetch misses (flash)", g->fmiss_flash, r->fmiss_flash, n);
  row_u("fetch misses (PSRAM)", g->fmiss_psram, r->fmiss_psram, n);
  row_u("data misses (flash)", g->dmiss_flash, r->dmiss_flash, n);
  row_u("data misses (PSRAM)", g->dmiss_psram, r->dmiss_psram, n);
  row_u("write-backs", g->wb, r->wb, n);
  printf("  %-24s %12.0f %12.0f %12.0f\n", "cycles (estimate)", cg, cr, cg + cr);
  const double us_miss_g = ((double)(g->fmiss_flash + g->dmiss_flash) * s_param[P_FLASH].v +
                            (double)(g->fmiss_psram + g->dmiss_psram) * s_param[P_PSRAM].v +
                            (double)g->wb * s_param[P_WB].v) / n;
  const double us_miss_r = ((double)(r->fmiss_flash + r->dmiss_flash) * s_param[P_FLASH].v +
                            (double)(r->fmiss_psram + r->dmiss_psram) * s_param[P_PSRAM].v +
                            (double)r->wb * s_param[P_WB].v) / n;
  printf("  %-24s %11.0f%% %11.0f%% %11.0f%%\n", "of which cache stalls", 100 * us_miss_g / (cg ? cg : 1),
         100 * us_miss_r / (cr ? cr : 1), 100 * (us_miss_g + us_miss_r) / (cg + cr ? cg + cr : 1));
  printf("  %-24s %12.2f %12.2f %12.2f\n", "time at clock (ms)", cg / (mhz * 1000),
         cr / (mhz * 1000), (cg + cr) / (mhz * 1000));
  if (s_frames > 0 && s_frames <= 4096) {
    qsort(s_frame_cycles_geom, (size_t)s_frames, sizeof(double), cmp_d);
    qsort(s_frame_cycles_raster, (size_t)s_frames, sizeof(double), cmp_d);
    const int p = s_frames * 95 / 100 >= s_frames ? s_frames - 1 : s_frames * 95 / 100;
    printf("  %-24s %12.2f %12.2f\n", "time, p95 frame (ms)", s_frame_cycles_geom[p] / (mhz * 1000),
           s_frame_cycles_raster[p] / (mhz * 1000));
  }
  if (show_device) {
    printf("  %-24s %12.2f %12.2f %12.2f\n", "device, 200 MHz (ms)", 6.3, 4.1, 10.4);
    printf("  %-24s %+11.0f%% %+11.0f%% %+11.0f%%\n", "estimate vs device",
           100 * (cg / (mhz * 1000) / 6.3 - 1), 100 * (cr / (mhz * 1000) / 4.1 - 1),
           100 * ((cg + cr) / (mhz * 1000) / 10.4 - 1));
  }
  printf("  scene: tris_in avg %.0f max %" PRIu64 ", culled avg %.0f, clipped avg %.0f, drawn avg %.0f "
         "max %" PRIu64 ", overflow %" PRIu64 " (device: tris_in max 2353, drawn avg 358 max 587)\n",
         (double)s_stat_sum[0] / n, s_stat_max[0], (double)s_stat_sum[1] / n,
         (double)s_stat_sum[2] / n, (double)s_stat_sum[3] / n, s_stat_max[3], s_stat_sum[4]);
  printf("  pixels: FNV-1a of every frame's 320x320 buffer %016" PRIx64 "\n", s_fnv);
}

// `--per N`: phase 1 as a cost per unit of work (the Lua loop's iterations).
static void report_per(const char *label, uint64_t n) {
  const counters_t *c = &s_c[PH_GEOM];
  const double d = (double)n, cy = cycles_of(c) / d;
  printf("perf_rig: %s, per iteration of %" PRIu64 " (ESTIMATE, not a measurement)\n", label, n);
  printf("  %-24s %10.2f\n", "instructions", (double)c->instr / d);
  printf("  %-24s %10.2f\n", "words loaded", (double)c->loads / d);
  printf("  %-24s %10.2f\n", "words stored", (double)c->stores / d);
  printf("  %-24s %10.2f\n", "taken branches", (double)c->taken / d);
  printf("  %-24s %10.4f\n", "fetch misses", (double)(c->fmiss_flash + c->fmiss_psram) / d);
  printf("  %-24s %10.4f\n", "data misses", (double)(c->dmiss_flash + c->dmiss_psram) / d);
  printf("  %-24s %10.4f\n", "write-backs", (double)c->wb / d);
  printf("  %-24s %10.2f\n", "cycles (estimate)", cy);
  printf("  %-24s %10.1f\n", "ns at clock", cy * 1000.0 / s_param[P_MHZ].v);
  printf("  %-24s %10.1f   (36 cycles)\n", "device, 200 MHz (ns)", 180.0);
}

// The self-test guest (guest/selftest.c) ran a known instruction sequence in the
// geometry phase, a known cache pattern in the raster phase, and counted its own
// wrong results (VFP, DSP) in stat 0.
static int selftest_report(void) {
  const counters_t *c = &s_c[PH_GEOM];
  int bad = 0;
  const struct {
    const char *what;
    uint64_t got, want;
  } t[] = {
      {"guest result failures", s_stat[0], 0},
      {"words loaded", c->loads, s_stat[1]},
      {"words stored", c->stores, s_stat[2]},
      {"VDIV+VSQRT", c->fdiv, s_stat[3]},
      {"FP operations", c->fp, s_stat[4]},
      {"data misses (PSRAM)", s_c[PH_RASTER].dmiss_psram, 4},
  };
  for (size_t i = 0; i < sizeof t / sizeof t[0]; i++) {
    const bool ok = t[i].got == t[i].want;
    printf("  %-24s %6" PRIu64 " want %6" PRIu64 " %s\n", t[i].what, t[i].got, t[i].want,
           ok ? "ok" : "WRONG");
    bad += !ok;
  }
  printf("selftest: %s\n", bad ? "FAILED" : "passed");
  return bad ? 1 : 0;
}

static void usage(void) {
  fprintf(stderr, "usage: perf_rig <guest.elf> [--label s] [--policy lru|fifo|random] [--warm]\n"
                  "                [--dirty-frac 0..1] [--no-device] [--max-instr N]");
  for (int i = 0; i < P_COUNT; i++) fprintf(stderr, " [--%s N]", s_param[i].name);
  fprintf(stderr, "\n");
  for (int i = 0; i < P_COUNT; i++)
    fprintf(stderr, "  --%-12s default %g: %s\n", s_param[i].name, s_param[i].v, s_param[i].why);
  exit(2);
}

int main(int argc, char **argv) {
  if (argc < 2) usage();
  const char *elf = argv[1], *label = argv[1];
  bool show_device = true, selftest = false;
  uint64_t max_instr = 0, per = 0;
  for (int i = 2; i < argc; i++) {
    const char *a = argv[i];
    if (!strcmp(a, "--selftest")) {
      selftest = true;
      continue;
    }
    if (i + 1 >= argc && strcmp(a, "--warm") && strcmp(a, "--no-device")) usage();
    if (!strcmp(a, "--label")) label = argv[++i];
    else if (!strcmp(a, "--warm")) s_cold_frames = false;
    else if (!strcmp(a, "--no-device")) show_device = false;
    else if (!strcmp(a, "--dirty-frac")) s_dirty_frac = atof(argv[++i]);
    else if (!strcmp(a, "--max-instr")) max_instr = strtoull(argv[++i], NULL, 0);
    else if (!strcmp(a, "--per")) per = strtoull(argv[++i], NULL, 0);
    else if (!strcmp(a, "--policy")) {
      const char *p = argv[++i];
      s_policy = !strcmp(p, "lru") ? POLICY_LRU : !strcmp(p, "fifo") ? POLICY_FIFO : POLICY_RANDOM;
    } else {
      int k = 0;
      for (; k < P_COUNT; k++)
        if (a[0] == '-' && a[1] == '-' && !strcmp(a + 2, s_param[k].name)) {
          s_param[k].v = atof(argv[++i]);
          break;
        }
      if (k == P_COUNT) usage();
    }
  }

  uc_engine *uc;
  uc_err e = uc_open(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS, &uc);
  if (e != UC_ERR_OK) {
    fprintf(stderr, "uc_open: %s\n", uc_strerror(e));
    return 2;
  }
  uc_ctl_set_cpu_model(uc, UC_CPU_ARM_CORTEX_M33);
  const struct { uint32_t base, size; } regions[] = {
      {0x10000000u, 0x01000000u},  // flash
      {0x11000000u, 0x01000000u},  // QMI PSRAM (cached alias)
      {0x20000000u, 0x00100000u},  // SRAM
      {0x30000000u, 0x01000000u},  // the rig's own: scene data, free of cost in setup
      {RIG_BASE, 0x1000u},
  };
  for (size_t i = 0; i < sizeof regions / sizeof regions[0]; i++) {
    e = uc_mem_map(uc, regions[i].base, regions[i].size, UC_PROT_ALL);
    if (e != UC_ERR_OK) {
      fprintf(stderr, "uc_mem_map: %s\n", uc_strerror(e));
      return 2;
    }
  }
  const uint32_t entry = load_elf(uc, elf);
  uc_hook h1, h2, h3, h4;
  uc_hook_add(uc, &h1, UC_HOOK_CODE, hook_code, NULL, 1, 0);
  uc_hook_add(uc, &h2, UC_HOOK_MEM_READ | UC_HOOK_MEM_WRITE, hook_mem, NULL, 1, 0);
  uc_hook_add(uc, &h3, UC_HOOK_MEM_INVALID, hook_bad, NULL, 1, 0);
  uc_hook_add(uc, &h4, UC_HOOK_INTR, hook_intr, NULL, 1, 0);
  s_next_pc = entry & ~1u;
  e = uc_emu_start(uc, entry | 1u, 0xFFFFFFFEu, 0, max_instr);
  if (e != UC_ERR_OK || !s_done) {
    uint32_t pc = 0;
    uc_reg_read(uc, UC_ARM_REG_PC, &pc);
    fprintf(stderr, "rig: stopped early at pc 0x%08x: %s (frames so far %d)\n", pc,
            uc_strerror(e), s_frames);
    report(label, show_device);
    return 1;
  }
  if (selftest) return selftest_report();
  if (per) {
    if (s_stat[0]) {
      fprintf(stderr, "rig: the guest reported error %u\n", s_stat[0]);
      return 1;
    }
    report_per(label, per);
    uc_close(uc);
    return 0;
  }
  report(label, show_device);
  uc_close(uc);
  return 0;
}
