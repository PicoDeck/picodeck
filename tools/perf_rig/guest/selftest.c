// `make check`: does Unicorn execute the M33's VFP and DSP instructions the
// firmware's compiler emits, and do its hooks count what the rig assumes?
#include "rig.h"

static int s_bad;
static void want(uint32_t got, uint32_t expect) { s_bad += got != expect; }
static uint32_t f2u(float f) {
  union {
    float f;
    uint32_t u;
  } x = {f};
  return x.u;
}

__attribute__((naked, section(".text._start"))) void _start(void) {
  __asm__(
      "ldr r0, =0x20040000\n mov sp, r0\n bl rig_main\n"
      "ldr r1, =0x4FF00004\n movs r0, #1\n str r0, [r1]\n1: b 1b\n.ltorg\n");
}

static void test_vfp(void) {
  float a = 1.5f, b = 2.25f, r;
  __asm__("vmul.f32 %0, %1, %2" : "=t"(r) : "t"(a), "t"(b));
  want(f2u(r), f2u(3.375f));
  __asm__("vdiv.f32 %0, %1, %2" : "=t"(r) : "t"(b), "t"(a));
  want(f2u(r), f2u(1.5f));
  __asm__("vsqrt.f32 %0, %1" : "=t"(r) : "t"(b));
  want(f2u(r), f2u(1.5f));
  __asm__("vmaxnm.f32 %0, %1, %2" : "=t"(r) : "t"(a), "t"(b));
  want(f2u(r), f2u(2.25f));
  __asm__("vminnm.f32 %0, %1, %2" : "=t"(r) : "t"(a), "t"(b));
  want(f2u(r), f2u(1.5f));
  float h = 2.5f;
  __asm__("vrintp.f32 %0, %1" : "=t"(r) : "t"(h));
  want(f2u(r), f2u(3.0f));
  __asm__("vrintm.f32 %0, %1" : "=t"(r) : "t"(h));
  want(f2u(r), f2u(2.0f));
  __asm__("vrinta.f32 %0, %1" : "=t"(r) : "t"(h));
  want(f2u(r), f2u(3.0f));
  float m = -2.75f;
  int i;
  __asm__("vcvt.s32.f32 %0, %1" : "=t"(r) : "t"(m));
  __asm__("vmov %0, %1" : "=r"(i) : "t"(r));
  want((uint32_t)i, (uint32_t)-2);
  r = h;
  __asm__("vfma.f32 %0, %1, %2" : "+t"(r) : "t"(a), "t"(b));
  want(f2u(r), f2u(5.875f));
}

static void test_dsp(void) {
  uint32_t r;
  __asm__("qadd %0, %1, %2" : "=r"(r) : "r"(0x7FFFFFF0), "r"(0x100));
  want(r, 0x7FFFFFFF);
  __asm__("smlad %0, %1, %2, %3" : "=r"(r) : "r"(0x00020003), "r"(0x00050007), "r"(100));
  want(r, 131);
  __asm__("sadd16 %0, %1, %2" : "=r"(r) : "r"(0x00010002), "r"(0x00030004));
  want(r, 0x00040006);
  __asm__("uxtab16 %0, %1, %2" : "=r"(r) : "r"(0x00010002), "r"(0x12345678));
  want(r, 0x0035007A);
  __asm__("usat %0, #5, %1" : "=r"(r) : "r"(100));
  want(r, 31);
  __asm__("rev16 %0, %1" : "=r"(r) : "r"(0x11223344));
  want(r, 0x22114433);
  uint32_t lo, hi;
  __asm__("smull %0, %1, %2, %3" : "=&r"(lo), "=&r"(hi) : "r"(-3), "r"(5));
  want(lo, (uint32_t)-15);
  want(hi, 0xFFFFFFFFu);
}

static volatile uint32_t s_ram[8] = {1, 2, 3, 4, 5, 6, 7, 8};

// Known bus traffic: LDM of 4, VLDM of 4 singles, a D register pushed and
// popped (2 words each way), one VDIV, one VSQRT, one VADD.
static void test_counts(void) {
  // In a register before the phase opens: its literal-pool load would count.
  volatile uint32_t *ram = s_ram;
  __asm__ volatile("" : "+r"(ram));
  RIG_PHASE(PHASE_GEOM);
  __asm__ volatile(
      "ldmia %0, {r1, r2, r3, r4}\n"
      "vldmia %0, {s0, s1, s2, s3}\n"
      "vpush {d0}\n"
      "vpop {d0}\n"
      "vdiv.f32 s0, s1, s2\n"
      "vsqrt.f32 s0, s1\n"
      "vadd.f32 s0, s1, s2\n"
      :
      : "r"(ram)
      : "r1", "r2", "r3", "r4", "s0", "s1", "s2", "s3", "memory");
  RIG_PHASE(PHASE_IDLE);
}

// The cache: A, B, C all in one set of an otherwise cold cache. Fill A, fill B,
// C evicts A (LRU), A misses again and evicts B, C still hits. 4 misses in all.
static void test_cache(void) {
  volatile uint32_t *a = (volatile uint32_t *)0x11200000u;
  volatile uint32_t *b = (volatile uint32_t *)0x11202000u;
  volatile uint32_t *c = (volatile uint32_t *)0x11204000u;
  RIG_PHASE(PHASE_RASTER);
  (void)*a;
  (void)*b;
  (void)*c;
  (void)*a;
  (void)*c;
  RIG_PHASE(PHASE_IDLE);
}

void rig_main(void) {
  test_vfp();
  test_dsp();
  test_counts();
  test_cache();
  RIG_STAT(0, (uint32_t)s_bad);
  RIG_STAT(1, 4 + 4 + 2);  // words loaded
  RIG_STAT(2, 2);          // words stored
  RIG_STAT(3, 2);          // VDIV + VSQRT
  RIG_STAT(4, 3);          // FP data-processing: VDIV, VSQRT, VADD
}
