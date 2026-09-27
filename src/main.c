#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/structs/xip.h"
#include "hardware/xip_cache.h"
#include "hardware/watchdog.h"
#include "pico/bootrom.h"
#include "pico/multicore.h"
#include "pico/runtime_init.h"
#include "pico/stdlib.h"

#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

#include "dev_commands.h"
#include "usb/usb_msc.h"

// Forward declarations for display functions used in hardfault_c below.
// The full #include "drivers/display.h" appears later in the file after other
// includes; these declarations let the fault handler use them before that point.
#include <stdint.h>
void display_clear(uint16_t color);
int  display_draw_text(int x, int y, const char *text, uint16_t fg, uint16_t bg);
void display_flush(void);

// Forward declarations for launcher functions used in hardfault_c.
const char* launcher_get_running_app_name(void);
uint32_t    launcher_get_app_uptime_ms(void);

// kbd_i2c_halt(): hardfault_c stops the keyboard bus engine before resetting.
#include "drivers/kbd_i2c.h"

// Linker symbols for the main stack limits (see boot2/memmap_*.ld)
extern uint32_t __StackTop;    // initial SP (stack grows DOWN from here)
extern uint32_t __StackBottom; // lowest valid address (4KB below StackTop)
extern uint32_t __StackOneBottom; // Core 1 MSP lower bound (SCRATCH_X)

// ── Stack limits (ARMv8-M MSPLIM) ─────────────────────────────────────────────
// PICO_USE_STACK_GUARDS=1 (CMakeLists.txt) makes the SDK call this hook for
// each core before its code runs: Core 0 from runtime_init() with
// &__StackBottom, Core 1 from core1_wrapper() with __StackOneBottom. We supply
// the implementation (PICO_RUNTIME_NO_INIT_PER_CORE_INSTALL_STACK_GUARD=1) to
// add a tiny margin: 32 bytes = one 8-word basic exception frame, and a
// multiple of 8 (the MSPLIM granule). A limit hit taken during exception-entry
// stacking clamps SP at the limit, so whatever that stacking writes still
// lands inside the real stack rather than in whatever lies below it (the
// frame's contents are UNKNOWN then, see isr_hardfault). The margin is
// deliberately not sized for the handler: isr_hardfault clears MSPLIM and,
// when MSP is near the bottom, moves it to the top of this core's (now dead)
// stack before hardfault_c runs. Keeping the margin small preserves almost
// the full 4 KB of usable stack on both cores.
// Runs before runtime init completes: no printf, no asserts.
#define STACK_LIMIT_MARGIN 32u
void runtime_init_per_core_install_stack_guard(void *stack_bottom) {
  uint32_t limit = ((uint32_t)(uintptr_t)stack_bottom + STACK_LIMIT_MARGIN + 7u) & ~7u;
  __asm volatile ("msr msplim, %0" : : "r"(limit));
}

// ── HardFault handler ─────────────────────────────────────────────────────────
// Captures the exception frame (stacked registers) and prints fault registers
// to UART+USB so we can identify the crash address.  UART stdio is polling-
// based, so this works even with interrupts disabled inside the fault handler.

// The app runtime's PSP stack (g_app_stack_base/owner, app_stack.h): the
// native app or the Lua VM. NULL when no app runtime is on the PSP. Used to
// bound PSP overflow checks and to name the stack in the crash record.
#include "os/app_stack.h"

// Native app image placement (defined in native_loader.c) — lets the
// hardfault handler report crash PC/LR as ELF-relative offsets.
extern uintptr_t g_native_code_base, g_native_code_limit;
extern uint32_t  g_native_code_vaddr;
extern uintptr_t g_native_data_base, g_native_data_limit;
extern uint32_t  g_native_data_vaddr;

// DIAG: code-corruption watcher state (defined in native_loader.c).
extern const uint8_t *g_code_watch_snap;
extern const uint8_t *g_code_watch_live;
extern uint32_t       g_code_watch_size;
extern _Atomic(bool)  g_code_watch_active;

// Translate an absolute address inside the running native app back to its
// ELF vaddr.  Returns 0xFFFFFFFF when the address is outside the app image.
static uint32_t native_addr_to_elf_vaddr(uint32_t addr) {
  uint32_t a = addr & ~1u;
  if (g_native_code_base && a >= g_native_code_base && a < g_native_code_limit)
    return a - (uint32_t)g_native_code_base + g_native_code_vaddr;
  if (g_native_data_base && a >= g_native_data_base && a < g_native_data_limit)
    return a - (uint32_t)g_native_data_base + g_native_data_vaddr;
  return 0xFFFFFFFFu;
}

// ── Crash record in the watchdog scratch registers ────────────────────────────
// hardfault_c leaves a record for the next boot, which crash_log_save() turns
// into a /system/crashlog.txt entry. The SDK's watchdog_reboot(0, 0, 0) zeroes
// scratch[4] (with a non-zero pc it also writes scratch[5-7] and the bootrom
// reads them as a vectored-boot request), so everything that must survive
// lives in scratch[0-3]:
//   scratch[0] = CRASH_TAG (bits 31-16) | flags (bits 15-8) | SFSR (bits 7-0)
//   scratch[1] = stacked PC (outside a crash record: the one-shot OTA intent
//                token, OTA_MAGIC — main() zeroes it on the crash path)
//   scratch[2] = stacked LR
//   scratch[3] = CFSR (all 32 bits)
// scratch[5-7] hold supplementary data that watchdog_reboot(0, 0, 0) leaves
// alone: [5] pre-fault SP, [6] fault address or diagnostic pack (see
// hardfault_c), [7] app uptime in seconds.
#define CRASH_TAG           0xFA170000u
#define CRASH_TAG_MASK      0xFFFF0000u
#define CRASH_F_BOOTING     (1u << 15) // fault hit before boot completed
#define CRASH_BOOT_ATTEMPT_SHIFT 13    // bits 14-13: that boot's attempt no.
#define CRASH_BOOT_ATTEMPT_MASK  (3u << CRASH_BOOT_ATTEMPT_SHIFT)
#define CRASH_F_PSP         (1u << 12) // fault frame on PSP (app runtime)
#define CRASH_F_HFSR_FORCED (1u << 11) // HFSR bit 30
#define CRASH_F_HFSR_VECTBL (1u << 10) // HFSR bit 1
#define CRASH_F_PSP_LUA     (1u << 9)  // ...and the PSP was the Lua VM's stack
#define CRASH_F_PSP_OS      (1u << 8)  // ...or app_stack_run_os() (dev command)

// Boot-loop detection (main): while booting, scratch[0] holds
// BOOT_MAGIC | attempt; it is zeroed once the launcher is about to run.
#define BOOT_MAGIC_MASK  0xFFFFFF00u
#define BOOT_MAGIC       0xB0070000u
#define BOOT_MAX_RETRIES 3

static void __attribute__((used)) hardfault_c(uint32_t *frame, uint32_t exc_return) {
  // First, so Core 0's keyboard bus engine is off the bus well before the
  // reset below (see the comment there).
  kbd_i2c_halt();

  // ARM exception frame layout (8 words pushed by hardware on entry):
  //   frame[0]=R0, [1]=R1, [2]=R2, [3]=R3,
  //   [4]=R12, [5]=LR(EXC), [6]=PC(fault), [7]=xPSR
  uint32_t pc   = frame[6];
  uint32_t lr   = frame[5];
  uint32_t r0   = frame[0];
  // Cortex-M33 fault status registers
  uint32_t cfsr = *(volatile uint32_t *)0xE000ED28u; // CFSR (UFSR|BFSR|MMFSR)
  uint32_t hfsr = *(volatile uint32_t *)0xE000ED2Cu; // HFSR
  uint32_t bfar = *(volatile uint32_t *)0xE000ED38u; // BFAR (if BFARVALID)
  uint32_t mmar = *(volatile uint32_t *)0xE000ED34u; // MMFAR (if MMFARVALID)
  uint32_t ccr = *(volatile uint32_t *)0xE000ED14u;  // SCB CCR
  // ARMv8-M Security Extension fault registers.  A SecureFault escalates to
  // HardFault with HFSR.FORCED set but leaves CFSR untouched — its syndrome
  // lives in SFSR/SFAR instead.  (CFSR=0 + FORCED is the telltale.)
  uint32_t sfsr = *(volatile uint32_t *)0xE000EDE4u; // SFSR
  uint32_t sfar = *(volatile uint32_t *)0xE000EDE8u; // SFAR (if SFARVALID)
  uint32_t dfsr = *(volatile uint32_t *)0xE000ED30u; // DFSR (debug events)

  // Grab app context before anything else (best-effort, pointers may be bad).
  const char *app_name = launcher_get_running_app_name();
  uint32_t uptime_ms = launcher_get_app_uptime_ms();
  uint32_t uptime_sec = uptime_ms / 1000u;

  // Which exception is actually executing (3 = genuine HardFault; anything
  // else means this handler was reached through a different vector), what
  // was executing before (stacked xPSR ISR field, 0 = thread mode), and the
  // instruction at the faulting PC (verifies the stacked PC is sane).
  uint32_t ipsr;
  __asm volatile ("mrs %0, ipsr" : "=r"(ipsr));
  uint32_t stacked_xpsr = frame[7];
  uint16_t instr16 = 0;
  uint32_t pc_probe = frame[6] & ~1u;
  if ((pc_probe >= 0x10000000u && pc_probe < 0x18000000u) ||
      (pc_probe >= 0x20000000u && pc_probe < 0x20082000u)) {
    instr16 = *(volatile uint16_t *)pc_probe;
  }

  // A fault before boot completed must count toward the boot-loop limit, so
  // carry the attempt number from the boot counter (or from a record the
  // other core already wrote) into the crash record.
  uint32_t prev0 = watchdog_hw->scratch[0];
  uint32_t boot_bits = 0;
  if ((prev0 & BOOT_MAGIC_MASK) == BOOT_MAGIC) {
    uint32_t attempt = prev0 & 0xFFu;
    if (attempt > BOOT_MAX_RETRIES) attempt = BOOT_MAX_RETRIES;
    boot_bits = CRASH_F_BOOTING | (attempt << CRASH_BOOT_ATTEMPT_SHIFT);
  } else if ((prev0 & CRASH_TAG_MASK) == CRASH_TAG) {
    boot_bits = prev0 & (CRASH_F_BOOTING | CRASH_BOOT_ATTEMPT_MASK);
  }
  // This boot fault uses up the last attempt: rebooting would only fault
  // again (every boot + 3 s, appending a crashlog entry each time once the
  // SD is up). Halt on the fault screen instead. The watchdog goes off first
  // so nothing can reset us; Core 1 only ever feeds it, never enables it.
  // Power-cycling clears the scratch registers (reset value 0, kept only
  // through soft resets) and with them the attempt count.
  uint32_t boot_failures = ((boot_bits & CRASH_BOOT_ATTEMPT_MASK)
                            >> CRASH_BOOT_ATTEMPT_SHIFT) + 1u;
  bool boot_halt = (boot_bits & CRASH_F_BOOTING) &&
                   boot_failures >= BOOT_MAX_RETRIES;
  if (boot_halt)
    watchdog_disable();

  // Persist fault data in watchdog scratch registers so it survives the
  // reboot and can be dumped to SD on next boot (layout above hardfault_c).
  watchdog_hw->scratch[0] = CRASH_TAG | boot_bits
                          | ((exc_return & 4u) ? CRASH_F_PSP : 0u)
                          | (((exc_return & 4u) &&
                              g_app_stack_owner == APP_STACK_LUA)
                                 ? CRASH_F_PSP_LUA : 0u)
                          | (((exc_return & 4u) &&
                              g_app_stack_owner == APP_STACK_OS)
                                 ? CRASH_F_PSP_OS : 0u)
                          | ((hfsr & (1u << 30)) ? CRASH_F_HFSR_FORCED : 0u)
                          | ((hfsr & (1u << 1)) ? CRASH_F_HFSR_VECTBL : 0u)
                          | (sfsr & 0xFFu);
  watchdog_hw->scratch[1] = pc;
  watchdog_hw->scratch[2] = lr;
  watchdog_hw->scratch[3] = cfsr;
  watchdog_hw->scratch[5] = (uint32_t)(uintptr_t)frame + 32u; // pre-fault SP
  // scratch[6] is context-dependent (crash_log_save decodes with the same
  // conditions): SFAR when SFSR.SFARVALID; BFAR when any CFSR/SFSR syndrome
  // exists; otherwise (no syndrome at all — BFAR is stale garbage then) a
  // diagnostic pack: instr16@PC (0-15) | live IPSR (16-24) | stacked ISR (25-31).
  if (sfsr & (1u << 6)) {
    watchdog_hw->scratch[6] = sfar;
  } else if (cfsr != 0 || sfsr != 0) {
    watchdog_hw->scratch[6] = bfar;
  } else {
    watchdog_hw->scratch[6] = (uint32_t)instr16
                            | ((ipsr & 0x1FFu) << 16)
                            | ((stacked_xpsr & 0x7Fu) << 25);
  }
  watchdog_hw->scratch[7] = uptime_sec;

  // frame IS the MSP/PSP just after the hardware pushed the 8-word exception
  // frame.  Pre-fault SP = frame + 32 (8 words × 4 bytes).
  uint32_t sp_at_fault = (uint32_t)(uintptr_t)frame + 32u;

  // Determine which stack was active: EXC_RETURN bit 2 = 1 means PSP (the
  // native app or the Lua VM, see app_stack.h), 0 means MSP (OS).  Compare
  // SP against the correct stack bounds.
  // Each core has its own MSP: Core 0 in SCRATCH_Y (__StackBottom), Core 1 in
  // SCRATCH_X (__StackOneBottom) — comparing against the wrong core's bounds
  // yields false OVERFLOW reports for Core 1 faults.
  uint32_t core = sio_hw->cpuid;
  bool on_psp = (exc_return & 4u) != 0;
  bool stack_overflow;
  uint32_t stack_limit;
  if (on_psp) {
    stack_limit = (uint32_t)(uintptr_t)g_app_stack_base;
    stack_overflow = g_app_stack_base && (sp_at_fault < stack_limit);
  } else if (core == 1) {
    stack_limit = (uint32_t)(uintptr_t)&__StackOneBottom;
    stack_overflow = (sp_at_fault < stack_limit);
  } else {
    stack_limit = (uint32_t)(uintptr_t)&__StackBottom;
    stack_overflow = (sp_at_fault < stack_limit);
  }
  // A MSPLIM/PSPLIM hit clamps SP at the limit (above the bottom), so the
  // SP comparison alone misses it; CFSR.STKOF is the authoritative signal.
  if (cfsr & (1u << 20))
    stack_overflow = true;

  // ── UART output (always works — polling-based, no IRQ required) ────────────
  printf("\n!!! HARDFAULT (core %lu) !!!\n", (unsigned long)core);
  if (app_name)
    printf("  App  = %s (uptime %lum %lus)\n",
           app_name, (unsigned long)(uptime_sec / 60u), (unsigned long)(uptime_sec % 60u));
  else
    printf("  App  = (none -- OS/launcher)\n");
  printf("  PC   = 0x%08lx\n", (unsigned long)pc);
  printf("  LR   = 0x%08lx\n", (unsigned long)lr);
  printf("  R0   = 0x%08lx\n", (unsigned long)r0);
  uint32_t pc_rel = native_addr_to_elf_vaddr(pc);
  uint32_t lr_rel = native_addr_to_elf_vaddr(lr);
  if (pc_rel != 0xFFFFFFFFu)
    printf("  PC-ELF = 0x%08lx (app image offset)\n", (unsigned long)pc_rel);
  if (lr_rel != 0xFFFFFFFFu)
    printf("  LR-ELF = 0x%08lx (app image offset)\n", (unsigned long)lr_rel);
  printf("  SP   = 0x%08lx  (frame @ 0x%08lx)  stack_limit=0x%08lx [%s]%s\n",
         (unsigned long)sp_at_fault, (unsigned long)(uintptr_t)frame,
         (unsigned long)stack_limit,
         on_psp ? "PSP" : "MSP",
         stack_overflow ? " *** OVERFLOW ***" : "");
  printf("  CFSR = 0x%08lx\n", (unsigned long)cfsr);
  printf("  HFSR = 0x%08lx\n", (unsigned long)hfsr);
  printf("  BFAR = 0x%08lx\n", (unsigned long)bfar);
  printf("  MMAR = 0x%08lx\n", (unsigned long)mmar);
  printf("  SFSR = 0x%08lx\n", (unsigned long)sfsr);
  printf("  SFAR = 0x%08lx\n", (unsigned long)sfar);
  printf("  DFSR = 0x%08lx\n", (unsigned long)dfsr);
  printf("  IPSR = %lu (3=HardFault)  stacked xPSR = 0x%08lx  [PC] = 0x%04x\n",
         (unsigned long)(ipsr & 0x1FFu), (unsigned long)stacked_xpsr,
         (unsigned)instr16);
  printf("  CCR  = 0x%08lx (bit3=UNALIGNED_TRP)\n", (unsigned long)ccr);
  // Decode CFSR flags to UART for easy diagnosis
  if (cfsr & (1u<<17)) printf("  INVSTATE: invalid CPU state (bad Thumb bit?)\n");
  if (cfsr & (1u<<16)) printf("  UNDEFINSTR: undefined instruction\n");
  if (cfsr & (1u<<18)) printf("  INVPC: invalid EXC_RETURN / PC load\n");
  if (cfsr & (1u<<19)) printf("  NOCP: coprocessor access\n");
  if (cfsr & (1u<< 9)) printf("  PRECISERR: precise data bus fault (BFAR=0x%08lx)\n", (unsigned long)bfar);
  if (cfsr & (1u<< 8)) printf("  IBUSERR: instruction bus fault\n");
  if (cfsr & (1u<<10)) printf("  IMPRECISERR: imprecise data bus fault\n");
  if (cfsr & (1u<<12)) printf("  STKERR: exception stack push fault (stack overflow?)\n");
  if (cfsr & (1u<<11)) printf("  UNSTKERR: exception stack pop fault\n");
  if (cfsr & (1u<< 1)) printf("  DACCVIOL: MPU data access violation (MMAR=0x%08lx)\n", (unsigned long)mmar);
  if (cfsr & (1u<< 0)) printf("  IACCVIOL: MPU instr access violation (MMAR=0x%08lx)\n", (unsigned long)mmar);
  if (cfsr & (1u<<25)) printf("  DIVBYZERO: divide by zero\n");
  if (cfsr & (1u<<24)) printf("  UNALIGNED: unaligned access\n");
  if (cfsr & (1u<<20)) printf("  STKOF: stack limit violation (MSPLIM/PSPLIM)\n");
  if (hfsr & (1u<<30)) printf("  HFSR FORCED: escalated from configurable fault\n");
  if (hfsr & (1u<< 1)) printf("  HFSR VECTTBL: vector table read fault\n");
  // SecureFault syndrome (ARMv8-M Security Extension)
  if (sfsr & (1u<<0)) printf("  SFSR INVEP: invalid NS->S entry point\n");
  if (sfsr & (1u<<1)) printf("  SFSR INVIS: invalid integrity signature\n");
  if (sfsr & (1u<<2)) printf("  SFSR INVER: invalid exception return\n");
  if (sfsr & (1u<<3)) printf("  SFSR AUVIOL: attribution unit violation (SFAR=0x%08lx)\n", (unsigned long)sfar);
  if (sfsr & (1u<<4)) printf("  SFSR INVTRAN: invalid S<->NS transition\n");
  if (sfsr & (1u<<5)) printf("  SFSR LSPERR: lazy FP state preservation error\n");
  if (sfsr & (1u<<7)) printf("  SFSR LSERR: lazy state error\n");
  stdio_flush();

  // ── Display output (best-effort — lets us see fault info without a UART) ──
  // display_clear/draw_text only write to the SRAM framebuffer (no SPI).
  // display_flush sends the buffer to the LCD via PIO+DMA.
  // lcd_spi_wait_idle() now has a 100ms timeout (C2 fix), so even if the
  // LCD is disconnected or PIO is stuck, we won't hang forever.
  // Uses static buffers to avoid touching an already-damaged stack.
  static char ln[56]; // static to avoid stack usage
  display_clear(0x0000); // black

  snprintf(ln, sizeof(ln), "!!! HARDFAULT !!!");
  display_draw_text(4,  4, ln, 0xF800, 0x0000); // red

  if (app_name)
    snprintf(ln, sizeof(ln), "App: %.28s (%lum %lus)",
             app_name, (unsigned long)(uptime_sec / 60u), (unsigned long)(uptime_sec % 60u));
  else
    snprintf(ln, sizeof(ln), "App: (none -- OS/launcher)");
  display_draw_text(4, 18, ln, 0x07FF, 0x0000); // cyan

  snprintf(ln, sizeof(ln), "PC %08lx  LR %08lx", (unsigned long)pc, (unsigned long)lr);
  display_draw_text(4, 34, ln, 0xFFFF, 0x0000);

  snprintf(ln, sizeof(ln), "SP %08lx  lim %08lx%s",
           (unsigned long)sp_at_fault,
           (unsigned long)stack_limit,
           stack_overflow ? " OVFL!" : "");
  display_draw_text(4, 48, ln, stack_overflow ? 0xF800 : 0xFFFF, 0x0000);

  snprintf(ln, sizeof(ln), "Stack: %s core %lu",
           !on_psp ? "MSP (OS)"
           : g_app_stack_owner == APP_STACK_LUA ? "PSP (Lua VM)"
           : g_app_stack_owner == APP_STACK_OS  ? "PSP (OS command)"
                                                : "PSP (native app)",
           (unsigned long)core);
  display_draw_text(4, 62, ln, 0x07E0, 0x0000); // green

  snprintf(ln, sizeof(ln), "CFSR %08lx  HFSR %08lx", (unsigned long)cfsr, (unsigned long)hfsr);
  display_draw_text(4, 76, ln, 0xFFFF, 0x0000);

  snprintf(ln, sizeof(ln), "BFAR %08lx  MMAR %08lx", (unsigned long)bfar, (unsigned long)mmar);
  display_draw_text(4, 90, ln, 0xFFFF, 0x0000);

  snprintf(ln, sizeof(ln), "SFSR %08lx  SFAR %08lx", (unsigned long)sfsr, (unsigned long)sfar);
  display_draw_text(4, 104, ln, 0xFFFF, 0x0000);

  if (pc_rel != 0xFFFFFFFFu) {
    snprintf(ln, sizeof(ln), "PC-ELF %08lx LR-ELF %08lx",
             (unsigned long)pc_rel,
             (unsigned long)((lr_rel != 0xFFFFFFFFu) ? lr_rel : 0));
    display_draw_text(4, 118, ln, 0x07FF, 0x0000); // cyan
  }

  // Decode CFSR fault type flags on-screen
  int y = 136;
  uint16_t warn = 0xFD20; // orange
  if (cfsr & (1u<<17)) { display_draw_text(4, y, "INVSTATE: invalid CPU state", warn, 0); y += 14; }
  if (cfsr & (1u<<16)) { display_draw_text(4, y, "UNDEFINSTR", warn, 0);                  y += 14; }
  if (cfsr & (1u<<18)) { display_draw_text(4, y, "INVPC: bad EXC_RETURN/PC",  warn, 0);  y += 14; }
  if (cfsr & (1u<<19)) { display_draw_text(4, y, "NOCP: coprocessor",          warn, 0); y += 14; }
  if (cfsr & (1u<< 9)) { display_draw_text(4, y, "PRECISERR: data bus fault",  warn, 0); y += 14; }
  if (cfsr & (1u<< 8)) { display_draw_text(4, y, "IBUSERR: instr bus fault",   warn, 0); y += 14; }
  if (cfsr & (1u<<10)) { display_draw_text(4, y, "IMPRECISERR: data bus",       warn, 0); y += 14; }
  if (cfsr & (1u<<12)) { display_draw_text(4, y, "STKERR: stack push fault!",   warn, 0); y += 14; }
  if (cfsr & (1u<<11)) { display_draw_text(4, y, "UNSTKERR: stack pop fault",   warn, 0); y += 14; }
  if (cfsr & (1u<< 1)) { display_draw_text(4, y, "DACCVIOL: MPU data viol",    warn, 0); y += 14; }
  if (cfsr & (1u<< 0)) { display_draw_text(4, y, "IACCVIOL: MPU instr viol",   warn, 0); y += 14; }
  if (cfsr & (1u<<25)) { display_draw_text(4, y, "DIVBYZERO",                   warn, 0); y += 14; }
  if (cfsr & (1u<<24)) { display_draw_text(4, y, "UNALIGNED access",            warn, 0); y += 14; }
  if (cfsr & (1u<<20)) { display_draw_text(4, y, "STKOF: stack limit viol",     warn, 0); y += 14; }
  if (hfsr & (1u<<30)) { display_draw_text(4, y, "HFSR: FORCED escalation",    warn, 0); y += 14; }
  if (hfsr & (1u<< 1)) { display_draw_text(4, y, "HFSR: vector table fault",   warn, 0); y += 14; }
  if (sfsr & (1u<<0))  { display_draw_text(4, y, "SFSR INVEP",                  warn, 0); y += 14; }
  if (sfsr & (1u<<1))  { display_draw_text(4, y, "SFSR INVIS",                  warn, 0); y += 14; }
  if (sfsr & (1u<<2))  { display_draw_text(4, y, "SFSR INVER",                  warn, 0); y += 14; }
  if (sfsr & (1u<<3))  { display_draw_text(4, y, "SFSR AUVIOL",                 warn, 0); y += 14; }
  if (sfsr & (1u<<4))  { display_draw_text(4, y, "SFSR INVTRAN",                warn, 0); y += 14; }
  if (sfsr & (1u<<5))  { display_draw_text(4, y, "SFSR LSPERR",                 warn, 0); y += 14; }
  if (sfsr & (1u<<7))  { display_draw_text(4, y, "SFSR LSERR",                  warn, 0); y += 14; }
  if (y == 136)        { display_draw_text(4, y, "(no fault flags set)",         warn, 0); }

  if (boot_halt) {
    snprintf(ln, sizeof(ln), "Boot failed %lu times - halted.",
             (unsigned long)boot_failures);
    display_draw_text(4, 290, ln, 0xF800, 0x0000);
    display_draw_text(4, 304, "Power-cycle to retry.", 0xF800, 0x0000);
    printf("  Boot failed %lu times: halted (no reboot). Power-cycle to retry.\n",
           (unsigned long)boot_failures);
    stdio_flush();
  }

  display_flush();

  if (boot_halt) {
    // No reboot, so no further boots and no further SD writes.
    while (1) tight_loop_contents();
  }

  // Hold the fault screen long enough to read (~3 s), then reboot. Bounded
  // and well inside the 10 s watchdog (8 s during the boot QMI init), so the
  // explicit reboot below normally wins; if the watchdog fires first the
  // record in scratch[0-3] survives that reset too. busy_wait reads the
  // timer directly and needs no interrupts. No key wait: the keyboard sits
  // on I2C behind a driver the fault may have broken.
  busy_wait_ms(3000);

  // Reboot explicitly — watchdog scratch already has the crash data (saved
  // at the top of this function).  On next boot, crash_log_save() writes it
  // to /system/crashlog.txt.
  // The keyboard bus (an STM32 transaction cut mid-byte can lock its I2C
  // slave until a power cycle): kbd_prepare_reset() cannot run here (it
  // waits on the engine's interrupts), and need not. A Core 0 fault freezes
  // the engine, whose interrupts cannot preempt HardFault; the 3 s wait
  // above lets a transfer already queued in the I2C block finish on its own.
  // A Core 1 fault leaves Core 0 running the engine: kbd_i2c_halt() at the
  // top let it finish the transaction in flight (~6 ms) and start no other.
  watchdog_reboot(0, 0, 0);

  // Fallback if reboot doesn't fire immediately.
  while (1) tight_loop_contents();
}

// Naked trampoline: inspect EXC_RETURN to find which stack held the frame,
// then pass its address to the C handler.
void __attribute__((naked)) isr_hardfault(void) {
  __asm volatile (
    // After a stack-limit fault MSP sits AT MSPLIM: the handler's first push
    // would fault again and lock up. Drop this core's limit before any push.
    "movs r2, #0     \n"
    "msr  msplim, r2 \n"
    "mov  r1, lr     \n" // r1 = EXC_RETURN (2nd arg to hardfault_c)
    "tst  lr, #4     \n" // bit 2 of EXC_RETURN: 0=MSP, 1=PSP
    "ite  eq         \n"
    "mrseq r0, msp   \n" // frame on MSP (normal for thread mode without RTOS)
    "mrsne r0, psp   \n" // frame on PSP (if PSP was active thread stack)
    // If MSP is within 2KB of this core's stack bottom (a stack-limit fault,
    // or just very deep), hardfault_c and its printf/TinyUSB/display path
    // (estimated ~1KB, never measured) would run below the bottom: Core 0
    // into Core 1's live stack, which then faults and overwrites the crash
    // record; Core 1 into the heap end. This core's stack is dead anyway (the
    // handler reboots), so move MSP to its top. 2KB is half of the 4KB
    // stack, which maximises the room the handler is guaranteed either way:
    // re-homed, it has >= 2KB above the old MSP (and the frame below it);
    // not re-homed, it has >= 2KB below MSP. That is about twice the
    // estimate. r0 is not moved. After a STKOF taken while stacking, the
    // ARMv8-M frame contents are UNKNOWN: hardfault_c still reads a PC/LR
    // from it, but they may be garbage (the crashlog marks them unreliable).
    "movs r2, #0            \n"
    "movt r2, #0xd000       \n" // SIO base: CPUID at offset 0
    "ldr  r2, [r2]          \n"
    "cbnz r2, 1f            \n"
    "movw r2, #:lower16:__StackBottom    \n"
    "movt r2, #:upper16:__StackBottom    \n"
    "movw r3, #:lower16:__StackTop       \n"
    "movt r3, #:upper16:__StackTop       \n"
    "b    2f                \n"
    "1:                     \n"
    "movw r2, #:lower16:__StackOneBottom \n"
    "movt r2, #:upper16:__StackOneBottom \n"
    "movw r3, #:lower16:__StackOneTop    \n"
    "movt r3, #:upper16:__StackOneTop    \n"
    "2:                     \n"
    "add  r2, r2, #2048     \n"
    "mrs  r12, msp          \n"
    "cmp  r12, r2           \n"
    "it   lo                \n"
    "msrlo msp, r3          \n"
    "b    hardfault_c       \n"
  );
}

#include "drivers/audio.h"
#include "drivers/fileplayer.h"
#include "drivers/mp3_player.h"
#include "drivers/pio_psram.h"
#include "drivers/qmi_psram.h"
#include "drivers/pio_psram_bulk.h"
#include "drivers/sound.h"
#include "drivers/mod_player.h"
#include "drivers/display.h"
#include "drivers/image_api.h"
#include "drivers/image_preload.h"
#include "drivers/video_player.h"
#include "drivers/http.h"
#include "drivers/tcp.h"
#include "drivers/keyboard.h"
#include "drivers/sdcard.h"
#include "drivers/wifi.h"
#include "drivers/rng.h"
#include "fonts/font_registry.h"
#include "core1_stats.h"
#include "hardware.h"
#include "os/app_identity.h"
#include "os/appconfig.h"
#include "os/config.h"
#include "os/core1_alloc.h"
#include "os/crashlog.h"
#include "os/crypto.h"
#include "os/file_browser.h"
#include "os/idle_dim.h"
#include "os/launcher.h"
#include "os/lua_psram_alloc.h"
#include "os/os.h"
#include "os/ota_update.h"
#include "os/perf.h"
#include "os/system_menu.h"
#include "os/toast.h"
#include "os/terminal.h"
#include "os/terminal_render.h"
#include "os/text_input.h"
#include "os/ui.h"
#include "umm_malloc.h"

// ── OS API implementation stubs (wiring the function pointer table)
// ─────────── Full implementations live in each driver. This wires them all
// together into the global g_api struct that Lua and future C apps can
// reference.

PicoCalcAPI g_api;

// Wrapper that composites toast notifications before flushing to display.
// Assigned to g_api.display->flush so all callers (Lua, native, launcher)
// see toasts without modifying their render loops.
static void display_flush_with_toasts(void) {
    toast_draw();
    display_flush();
}

// Native TCP: allocate a slot, then queue the connect.  (connect used to be
// tcp_connect cast to the wrong signature — host landed in the conn slot.)
static pctcp_t native_tcp_connect_ex(const char *host, uint16_t port,
                                     uint32_t flags) {
    if (!host) return NULL;
    tcp_conn_t *c = tcp_alloc();
    if (!c) return NULL;
    c->insecure = (flags & PCTCP_TLS_INSECURE) != 0;
    if (!tcp_connect(c, host, port, (flags & PCTCP_TLS) != 0)) {
        tcp_free(c);
        return NULL;
    }
    return (pctcp_t)c;
}

static pctcp_t native_tcp_connect(const char *host, uint16_t port,
                                  bool use_ssl) {
    return native_tcp_connect_ex(host, port, use_ssl ? PCTCP_TLS : 0);
}

// Native close releases the slot through the close protocol (tcp.h): the
// handle is invalid afterwards; the slot is reclaimed once Core 1 lets go.
static void native_tcp_close(pctcp_t c) {
    tcp_free((tcp_conn_t *)c);
}

static picocalc_tcp_t s_tcp_impl = {
    .connect = native_tcp_connect,
    .write = (int (*)(pctcp_t, const void *, int))tcp_write,
    .read = (int (*)(pctcp_t, void *, int))tcp_read,
    .close = native_tcp_close,
    .available = (int (*)(pctcp_t))tcp_bytes_available,
    .getError = (const char *(*)(pctcp_t))tcp_get_error,
    .getEvents = (uint32_t (*)(pctcp_t))tcp_take_pending,
    .connectEx = native_tcp_connect_ex,
};

static picocalc_input_t s_input_impl = {
    .getButtons = kbd_get_buttons,
    .getButtonsPressed = kbd_get_buttons_pressed,
    .getButtonsReleased = kbd_get_buttons_released,
    .getChar = kbd_get_char,
};

static int display_get_width_fn(void) { return FB_WIDTH; }
static int display_get_height_fn(void) { return FB_HEIGHT; }
static picocalc_display_t s_display_impl = {
    .clear = display_clear,
    .setPixel = display_set_pixel,
    .fillRect = display_fill_rect,
    .drawRect = display_draw_rect,
    .drawLine = display_draw_line,
    .drawCircle = display_draw_circle,
    .fillCircle = display_fill_circle,
    .drawText = display_draw_text,
    .flush = display_flush_with_toasts,
    .getWidth = display_get_width_fn,
    .getHeight = display_get_height_fn,
    .setBrightness = display_set_brightness,
    .drawImageNN = display_draw_image_nn,
    .flushRows = display_flush_rows,
    .flushRegion = display_flush_region,
    .getBackBuffer = display_get_back_buffer,
    .effectInvert = display_effect_invert,
    .effectDarken = display_effect_darken,
    .effectBrighten = display_effect_brighten,
    .effectTint = display_effect_tint,
    .effectGrayscale = display_effect_grayscale,
    .effectBlend = display_effect_blend,
    .effectPalette = display_effect_palette,
    .effectDither = display_effect_dither,
    .effectScanline = display_effect_scanline,
    .effectPosterize = display_effect_posterize,
    .fillVLine = display_fill_vline,
    .drawTexturedColumn = display_draw_textured_column,
    .fillVLineGradient = display_fill_vline_gradient,
    .setClipRect = display_set_clip_rect,
    .getClipRect = display_get_clip_rect,
    .clearClipRect = display_clear_clip_rect,
    .fillHLine = display_fill_hline,
    .fillTriangle = display_fill_triangle,
    .setScrollArea = display_set_scroll_area,
    .setScrollOffset = display_set_scroll_offset,
    .drawPlane = display_draw_plane,
    .setFont = display_set_font,
    .getFont = display_get_font,
    .getFontWidth = display_get_font_width,
    .getFontHeight = display_get_font_height,
    .textWidth = display_text_width,
    .loadFont = font_registry_load,
    .unloadFont = font_registry_unload,
    .drawTextTransparent = display_draw_text_transparent,
};

static uint32_t sys_getTimeMs(void) {
  return to_ms_since_boot(get_absolute_time());
}
static uint64_t sys_getTimeUs(void) {
  return time_us_64();
}
static void sys_reboot(void) {
  crashlog_clear_running(); // intentional — not an unclean exit
  kbd_prepare_reset();
  watchdog_enable(1, true);
  for (;;)
    tight_loop_contents();
}
static bool sys_isUSBPowered(void) {
  return gpio_get(USB_VBUS_PIN);
}
static void sys_log(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vprintf(fmt, ap);
  va_end(ap);
  printf("\n");
}

// Native-app exit flag: set by the system menu "Exit App" action,
// consumed by sys_shouldExit() which the app checks each frame.
static volatile bool s_native_exit = false;

// Core 0 liveness heartbeat, stamped by sys_poll().  Core 1 keeps the
// watchdog fed while this is fresh (< 60s), so apps doing long CPU-bound
// work between polls (asset decoding, sprite mask generation) are not
// rebooted by the 10s watchdog; a genuinely hung Core 0 still trips it
// once the heartbeat goes stale.
volatile uint32_t g_core0_heartbeat_ms = 0;
#define CORE0_HEARTBEAT_STALE_MS 60000u

// Feed the watchdog from Core 0 and stamp the heartbeat. Long Core 0 work
// that runs with Core 1 paused (ELF segment reads, USB MSC setup) calls this
// so Core 1 keeps relaying the watchdog for it.
void core0_heartbeat(void) {
  watchdog_update();
  g_core0_heartbeat_ms = to_ms_since_boot(get_absolute_time());
}

// Pending app launch from serial command
static const char* s_pending_launch = NULL;

// Native-app tick: poll keyboard, fire pending C HTTP callbacks, and
// check the Sym (Menu) key to show the system menu overlay.
static void sys_poll(void) {
  kbd_poll();
  core0_heartbeat();
  http_fire_c_pending();
  http_reap();  // reclaim connection slots Core 1 has released
  tcp_reap();
  if (kbd_consume_menu_press()) {
    if (system_menu_show_for_native())
      s_native_exit = true;
  }

  // Poll for serial commands
  dev_commands_poll();
  dev_commands_process();

  if (dev_commands_wants_exit()) {
    s_native_exit = true;
    dev_commands_clear_exit();
  }
  if (dev_commands_wants_reboot()) {
    printf("[DEV] Rebooting...\n");
    crashlog_clear_running(); // intentional — not an unclean exit
    stdio_flush();
    sleep_ms(100);
    kbd_prepare_reset();
    watchdog_reboot(0, 0, 0);
  }
  if (dev_commands_wants_reboot_flash()) {
    printf("[DEV] Rebooting to BOOTSEL mode...\n");
    crashlog_clear_running();
    stdio_flush();
    sleep_ms(100);
    kbd_prepare_reset();
    reset_usb_boot(0, 0);
  }
  if (dev_commands_wants_reboot_ota()) {
    // Launcher-only: drop it rather than let it fire when the app exits.
    dev_commands_clear_reboot_ota();
    printf("[DEV] reboot-ota ignored: an app is running (exit it first)\n");
  }
}

// Check if there's a pending launch request (from serial command)
// Returns app name if pending, NULL otherwise
const char* sys_get_pending_launch(void) {
  return s_pending_launch;
}

void sys_clear_pending_launch(void) {
  s_pending_launch = NULL;
}

static bool sys_shouldExit(void) {
  bool v = s_native_exit;
  s_native_exit = false;
  return v;
}

static void sys_setAudioCallback(void (*cb)(void)) {
  g_native_audio_callback = cb;
}

static picocalc_sys_t s_sys_impl = {
    .getTimeMs = sys_getTimeMs,
    .getTimeUs = sys_getTimeUs,
    .reboot = sys_reboot,
    .getBatteryPercent = kbd_get_battery_percent,
    .isUSBPowered = sys_isUSBPowered,
    .addMenuItem = system_menu_add_item,
    .clearMenuItems = system_menu_clear_items,
    .log = sys_log,
    .poll = sys_poll,
    .shouldExit = sys_shouldExit,
    .setAudioCallback = sys_setAudioCallback,
};

static picocalc_wifi_t s_wifi_impl = {
    .connect = wifi_connect,
    .disconnect = wifi_disconnect,
    .getStatus = wifi_get_status,
    .getIP = wifi_get_ip,
    .getSSID = wifi_get_ssid,
    .isAvailable = wifi_is_available,
};

static picocalc_audio_t s_audio_impl = {
    .playTone = audio_play_tone,
    .stopTone = audio_stop_tone,
    .setVolume = audio_set_volume,
    .startStream = audio_start_stream,
    .stopStream = audio_stop_stream,
    .pushSamples = audio_push_samples,
};

static pcfile_t fs_open(const char *path, const char *mode) {
    return (pcfile_t)sdcard_fopen(path, mode);
}
static int fs_read(pcfile_t f, void *buf, int len) {
    return sdcard_fread((sdfile_t)f, buf, len);
}
static int fs_write(pcfile_t f, const void *buf, int len) {
    return sdcard_fwrite((sdfile_t)f, buf, len);
}
static void fs_close(pcfile_t f) {
    sdcard_fclose((sdfile_t)f);
}
static bool fs_exists(const char *path) {
    return sdcard_fexists(path);
}
static int fs_size(const char *path) {
    return sdcard_fsize(path);
}
static int fs_fsize(pcfile_t f) {
    return sdcard_fsize_handle((sdfile_t)f);
}
static bool fs_seek(pcfile_t f, uint32_t offset) {
    return sdcard_fseek((sdfile_t)f, offset);
}
static uint32_t fs_tell(pcfile_t f) {
    return sdcard_ftell((sdfile_t)f);
}

// Static-global callback state avoids passing a stack-allocated pointer
// through the deep FatFS call chain (f_opendir → f_readdir → ...).
// A stack pointer can be clobbered by an ISR or a compiler tail-call before
// sdcard_list_dir's callback fires, causing an INVSTATE hard fault.
static struct {
    void (*fn)(const char *, bool, uint32_t, void *);
    void *user;
} s_list_cb;

static void fs_list_dir_callback(const sdcard_entry_t *entry, void *user) {
    (void)user;
    s_list_cb.fn(entry->name, entry->is_dir, entry->size, s_list_cb.user);
}

static int fs_list_dir(const char *path,
                       void (*callback)(const char *name, bool is_dir,
                                        uint32_t size, void *user),
                       void *user) {
    s_list_cb.fn = callback;
    s_list_cb.user = user;
    return sdcard_list_dir(path, fs_list_dir_callback, NULL);
}

static const picocalc_ui_t s_ui_impl = {
    .textInput       = text_input_show,
    .textInputSimple = ui_text_input,
    .confirm         = ui_confirm,
};

// PSRAM wrapper functions
static bool psram_pio_available(void) { return pio_psram_available(); }
static bool psram_pio_bulk_available(void) { return pio_psram_bulk_available(); }

// Native apps get [PIO_PSRAM_APP_BASE, chip end) only, as Lua does
// (sys.pioPsramRead/Write): below it are the OS's MP3 ring and video pool.
// A refused call does nothing (a read leaves dst untouched) and is logged.
static bool psram_pio_app_range_ok(const char *fn, uint32_t addr, uint32_t len) {
    uint32_t chip = pio_psram_available() ? pio_psram_size() : PIO_PSRAM_SIZE;
    pio_psram_range_t r = pio_psram_app_range_check((int64_t)addr, (int64_t)len, chip);
    if (r == PIO_PSRAM_RANGE_OK)
        return true;
    printf("[NATIVE] psram->%s(0x%lx, %lu) refused: %s (apps may use 0x%lx..0x%lx)\n",
           fn, (unsigned long)addr, (unsigned long)len,
           r == PIO_PSRAM_RANGE_RESERVED ? "reserved by the OS" : "out of range",
           (unsigned long)PIO_PSRAM_APP_BASE, (unsigned long)chip);
    return false;
}
static void psram_pio_read(uint32_t addr, uint8_t *dst, uint32_t len) {
    if (psram_pio_app_range_ok("pioRead", addr, len)) pio_psram_read(addr, dst, len);
}
static void psram_pio_write(uint32_t addr, const uint8_t *src, uint32_t len) {
    if (psram_pio_app_range_ok("pioWrite", addr, len)) pio_psram_write(addr, src, len);
}
static void psram_pio_bulk_read(uint32_t addr, uint8_t *dst, uint32_t len) {
    if (psram_pio_app_range_ok("pioBulkRead", addr, len)) pio_psram_bulk_read(addr, dst, len);
}
static void psram_pio_bulk_write(uint32_t addr, const uint8_t *src, uint32_t len) {
    if (psram_pio_app_range_ok("pioBulkWrite", addr, len)) pio_psram_bulk_write(addr, src, len);
}
static void *psram_qmi_alloc(uint32_t size) { return umm_malloc(size); }
static void psram_qmi_free(void *ptr) { umm_free(ptr); }

static const picocalc_psram_t s_psram_impl = {
    .pioAvailable     = psram_pio_available,
    .pioBulkAvailable = psram_pio_bulk_available,
    .pioRead          = psram_pio_read,
    .pioWrite         = psram_pio_write,
    .pioBulkRead      = psram_pio_bulk_read,
    .pioBulkWrite     = psram_pio_bulk_write,
    .qmiAlloc         = psram_qmi_alloc,
    .qmiFree          = psram_qmi_free,
};

static void perf_draw_fps_wrapper(int x, int y) {
    int fps = perf_get_fps();
    char buf[16];
    snprintf(buf, sizeof(buf), "FPS: %d", fps);
    uint16_t color = (fps >= 55)   ? COLOR_GREEN
                     : (fps >= 30) ? COLOR_YELLOW
                                   : COLOR_RED;
    display_draw_text(x, y, buf, color, COLOR_BLACK);
}

static const picocalc_perf_t s_perf_impl = {
    .beginFrame = perf_begin_frame,
    .endFrame = perf_end_frame,
    .getFPS = perf_get_fps,
    .getFrameTime = perf_get_frame_time,
    .drawFPS = perf_draw_fps_wrapper,
    .setTargetFPS = perf_set_target_fps,
};

static picocalc_terminal_t s_terminal_impl = {
    .create = terminal_new,
    .free = terminal_free,
    .clear = terminal_clear,
    .write = terminal_putString,
    .putChar = terminal_putChar,
    .setCursor = terminal_setCursor,
    .getCursor = terminal_getCursor,
    .setColors = terminal_setColors,
    .getColors = terminal_getColors,
    .scroll = terminal_scroll,
    .render = terminal_render,
    .renderDirty = terminal_renderDirty,
    .getCols = terminal_getCols,
    .getRows = terminal_getRows,
    .setCursorVisible = terminal_setCursorVisible,
    .setCursorBlink = terminal_setCursorBlink,
    .markAllDirty = terminal_markAllDirty,
    .isFullDirty = terminal_isFullDirty,
    .getDirtyRange = terminal_getDirtyRange,
    .getScrollbackCount = terminal_getScrollbackCount,
    .setScrollbackOffset = terminal_setScrollbackOffset,
    .getScrollbackOffset = terminal_getScrollbackOffset,
    .getScrollbackLine = terminal_getScrollbackLine,
    .getScrollbackLineColors = terminal_getScrollbackLineColors,
    // Line numbers
    .setLineNumbers = terminal_setLineNumbers,
    .setLineNumberStart = terminal_setLineNumberStart,
    .setLineNumberCols = terminal_setLineNumberCols,
    .setLineNumberColors = terminal_setLineNumberColors,
    .getContentCols = terminal_getContentCols,
    // Scrollbar
    .setScrollbar = terminal_setScrollbar,
    .setScrollbarColors = terminal_setScrollbarColors,
    .setScrollbarWidth = terminal_setScrollbarWidth,
    .setScrollInfo = terminal_setScrollInfo,
    .setRenderBounds = terminal_setRenderBounds,
    // Word wrap (visual - content not modified)
    .setWordWrap = terminal_setWordWrap,
    .setWordWrapColumn = terminal_setWordWrapColumn,
    .setWrapIndicator = terminal_setWrapIndicator,
    .getWordWrap = terminal_getWordWrap,
    .getVisualRowCount = terminal_getVisualRowCount,
    .logicalToVisual = terminal_logicalToVisual,
    .visualToLogical = terminal_visualToLogical,
    .calculateLineWraps = terminal_calculateLineWraps,
};

static bool fs_mkdir(const char *path) {
    return sdcard_mkdir(path);
}

static bool fs_delete(const char *path) {
    return sdcard_delete(path);
}

static bool fs_rename(const char *src, const char *dst) {
    return sdcard_rename(src, dst);
}

static bool fs_is_dir(const char *path) {
    sdcard_stat_t st;
    return sdcard_stat(path, &st) && st.is_dir;
}

static picocalc_fs_t s_fs_impl = {
    .open = fs_open,
    .read = fs_read,
    .write = fs_write,
    .close = fs_close,
    .exists = fs_exists,
    .size = fs_size,
    .fsize = fs_fsize,
    .seek = fs_seek,
    .tell = fs_tell,
    .listDir = fs_list_dir,
    .mkdir = fs_mkdir,
    .deleteFile = fs_delete,
    .renameFile = fs_rename,
    .isDir = fs_is_dir,
    .browse = file_browser_show,
};

// ── HTTP impl ─────────────────────────────────────────────────────────────────

static pchttp_t http_newConn_w(const char *server, uint16_t port, bool use_ssl) {
    http_conn_t *c = http_alloc();
    if (!c) return NULL;
    // Copy server hostname and port/ssl into the connection slot
    strncpy(c->server, server, HTTP_SERVER_MAX - 1);
    c->server[HTTP_SERVER_MAX - 1] = '\0';
    c->port    = port;
    c->use_ssl = use_ssl;
    return (pchttp_t)c;
}

static void http_get_w(pchttp_t c, const char *path, const char *extra_hdrs) {
    http_get((http_conn_t *)c, path, extra_hdrs);
}

static void http_post_w(pchttp_t c, const char *path, const char *extra_hdrs,
                        const char *body, uint32_t body_len) {
    http_post((http_conn_t *)c, path, extra_hdrs, body, (size_t)body_len);
}

static int http_read_w(pchttp_t c, uint8_t *buf, uint32_t len) {
    return (int)http_read((http_conn_t *)c, buf, len);
}

static uint32_t http_available_w(pchttp_t c) {
    return http_bytes_available((http_conn_t *)c);
}

static void http_close_w(pchttp_t c) {
    http_free((http_conn_t *)c);
}

static int http_getStatus_w(pchttp_t c) {
    return http_get_status((http_conn_t *)c);
}

static const char *http_getError_w(pchttp_t c) {
    return http_get_error((http_conn_t *)c);
}

static int http_getProgress_w(pchttp_t c, int *received, int *total) {
    int r, t;
    http_get_progress((http_conn_t *)c, &r, &t);  // t = -1 if unknown
    if (received) *received = r;
    if (total)    *total    = t;
    return t;
}

static void http_setKeepAlive_w(pchttp_t c, bool keep_alive) {
    ((http_conn_t *)c)->keep_alive = keep_alive;
}

static void http_setByteRange_w(pchttp_t c, int from, int to) {
    ((http_conn_t *)c)->range_from = from;
    ((http_conn_t *)c)->range_to   = to;
}

static void http_setConnectTimeout_w(pchttp_t c, int seconds) {
    ((http_conn_t *)c)->connect_timeout_ms = (uint32_t)(seconds * 1000);
}

static void http_setReadTimeout_w(pchttp_t c, int seconds) {
    ((http_conn_t *)c)->read_timeout_ms = (uint32_t)(seconds * 1000);
}

static bool http_setReadBufferSize_w(pchttp_t c, int bytes) {
    return http_set_recv_buf((http_conn_t *)c, (uint32_t)bytes);
}

static bool http_isComplete_w(pchttp_t c) {
    return http_is_complete((http_conn_t *)c);
}

static void http_setInsecure_w(pchttp_t c, bool insecure) {
    ((http_conn_t *)c)->insecure = insecure;
}

static const picocalc_http_t s_http_impl = {
    .newConn           = http_newConn_w,
    .get               = http_get_w,
    .post              = http_post_w,
    .read              = http_read_w,
    .available         = http_available_w,
    .close             = http_close_w,
    .getStatus         = http_getStatus_w,
    .getError          = http_getError_w,
    .getProgress       = http_getProgress_w,
    .setKeepAlive      = http_setKeepAlive_w,
    .setByteRange      = http_setByteRange_w,
    .setConnectTimeout = http_setConnectTimeout_w,
    .setReadTimeout    = http_setReadTimeout_w,
    .setReadBufferSize = http_setReadBufferSize_w,
    .isComplete        = http_isComplete_w,
    .setInsecure       = http_setInsecure_w,
};

// ── Sound player impl ─────────────────────────────────────────────────────────

static pcsound_sample_t sp_sampleLoad(const char *path) {
    sound_sample_t *s = sound_sample_create();
    if (!s) return NULL;
    if (!sound_sample_load(s, path)) { sound_sample_destroy(s); return NULL; }
    return (pcsound_sample_t)s;
}

static void sp_sampleFree(pcsound_sample_t s) {
    sound_sample_destroy((sound_sample_t *)s);
}

static pcsound_player_t sp_playerNew(void) {
    return (pcsound_player_t)sound_player_create();
}

static void sp_playerSetSample(pcsound_player_t p, pcsound_sample_t s) {
    sound_player_set_sample((sound_player_t *)p, (sound_sample_t *)s);
}

static void sp_playerPlay(pcsound_player_t p, uint8_t repeat_count) {
    sound_player_play((sound_player_t *)p, repeat_count);
}

static void sp_playerStop(pcsound_player_t p) {
    sound_player_stop((sound_player_t *)p);
}

static bool sp_playerIsPlaying(pcsound_player_t p) {
    return sound_player_is_playing((const sound_player_t *)p);
}

static uint8_t sp_playerGetVolume(pcsound_player_t p) {
    return sound_player_get_volume((const sound_player_t *)p);
}

static void sp_playerSetVolume(pcsound_player_t p, uint8_t vol) {
    sound_player_set_volume((sound_player_t *)p, vol);
}

// sound.h has no sound_player_set_loop; repeat_count=0 with playerPlay means
// the player won't loop — callers should use repeat_count to control looping.
// We store loop intent as repeat_count=255 (max repeats) when loop=true,
// and stop the player when loop=false.
static void sp_playerSetLoop(pcsound_player_t p, bool loop) {
    sound_player_t *sp = (sound_player_t *)p;
    sp->repeat_count = loop ? 255 : 0;
}

static void sp_playerFree(pcsound_player_t p) {
    sound_player_destroy((sound_player_t *)p);
}

static pcfileplayer_t sp_filePlayerNew(void) {
    return (pcfileplayer_t)fileplayer_create();
}

static void sp_filePlayerLoad(pcfileplayer_t fp, const char *path) {
    fileplayer_load((fileplayer_t *)fp, path);
}

static void sp_filePlayerPlay(pcfileplayer_t fp, uint8_t repeat_count) {
    fileplayer_play((fileplayer_t *)fp, repeat_count);
}

static void sp_filePlayerStop(pcfileplayer_t fp) {
    fileplayer_stop((fileplayer_t *)fp);
}

static void sp_filePlayerPause(pcfileplayer_t fp) {
    fileplayer_pause((fileplayer_t *)fp);
}

static void sp_filePlayerResume(pcfileplayer_t fp) {
    fileplayer_resume((fileplayer_t *)fp);
}

static bool sp_filePlayerIsPlaying(pcfileplayer_t fp) {
    return fileplayer_is_playing((const fileplayer_t *)fp);
}

static void sp_filePlayerSetVolume(pcfileplayer_t fp, uint8_t vol) {
    fileplayer_set_volume((fileplayer_t *)fp, vol, vol);
}

static uint8_t sp_filePlayerGetVolume(pcfileplayer_t fp) {
    uint8_t left = 0, right = 0;
    fileplayer_get_volume((const fileplayer_t *)fp, &left, &right);
    return left;
}

static uint32_t sp_filePlayerGetOffset(pcfileplayer_t fp) {
    return fileplayer_get_offset((const fileplayer_t *)fp);
}

static void sp_filePlayerSetOffset(pcfileplayer_t fp, uint32_t pos) {
    fileplayer_set_offset((fileplayer_t *)fp, pos);
}

static bool sp_filePlayerDidUnderrun(pcfileplayer_t fp) {
    (void)fp;
    return fileplayer_did_underrun();
}

static void sp_filePlayerFree(pcfileplayer_t fp) {
    fileplayer_destroy((fileplayer_t *)fp);
}

static pcmp3player_t sp_mp3PlayerNew(void) {
    return (pcmp3player_t)mp3_player_create();
}

static void sp_mp3PlayerLoad(pcmp3player_t mp, const char *path) {
    mp3_player_load((mp3_player_t *)mp, path);
}

static void sp_mp3PlayerPlay(pcmp3player_t mp, uint8_t repeat_count) {
    mp3_player_play((mp3_player_t *)mp, repeat_count);
}

static void sp_mp3PlayerStop(pcmp3player_t mp) {
    mp3_player_stop((mp3_player_t *)mp);
}

static void sp_mp3PlayerPause(pcmp3player_t mp) {
    mp3_player_pause((mp3_player_t *)mp);
}

static void sp_mp3PlayerResume(pcmp3player_t mp) {
    mp3_player_resume((mp3_player_t *)mp);
}

static bool sp_mp3PlayerIsPlaying(pcmp3player_t mp) {
    return mp3_player_is_playing((const mp3_player_t *)mp);
}

static void sp_mp3PlayerSetVolume(pcmp3player_t mp, uint8_t vol) {
    mp3_player_set_volume((mp3_player_t *)mp, vol);
}

static uint8_t sp_mp3PlayerGetVolume(pcmp3player_t mp) {
    return mp3_player_get_volume((const mp3_player_t *)mp);
}

static void sp_mp3PlayerSetLoop(pcmp3player_t mp, bool loop) {
    mp3_player_set_loop((mp3_player_t *)mp, loop);
}

static void sp_mp3PlayerFree(pcmp3player_t mp) {
    mp3_player_destroy((mp3_player_t *)mp);
}

static const picocalc_soundplayer_t s_soundplayer_impl = {
    .sampleLoad          = sp_sampleLoad,
    .sampleFree          = sp_sampleFree,
    .playerNew           = sp_playerNew,
    .playerSetSample     = sp_playerSetSample,
    .playerPlay          = sp_playerPlay,
    .playerStop          = sp_playerStop,
    .playerIsPlaying     = sp_playerIsPlaying,
    .playerGetVolume     = sp_playerGetVolume,
    .playerSetVolume     = sp_playerSetVolume,
    .playerSetLoop       = sp_playerSetLoop,
    .playerFree          = sp_playerFree,
    .filePlayerNew       = sp_filePlayerNew,
    .filePlayerLoad      = sp_filePlayerLoad,
    .filePlayerPlay      = sp_filePlayerPlay,
    .filePlayerStop      = sp_filePlayerStop,
    .filePlayerPause     = sp_filePlayerPause,
    .filePlayerResume    = sp_filePlayerResume,
    .filePlayerIsPlaying = sp_filePlayerIsPlaying,
    .filePlayerSetVolume = sp_filePlayerSetVolume,
    .filePlayerGetVolume = sp_filePlayerGetVolume,
    .filePlayerGetOffset = sp_filePlayerGetOffset,
    .filePlayerSetOffset = sp_filePlayerSetOffset,
    .filePlayerDidUnderrun = sp_filePlayerDidUnderrun,
    .filePlayerFree      = sp_filePlayerFree,
    .mp3PlayerNew        = sp_mp3PlayerNew,
    .mp3PlayerLoad       = sp_mp3PlayerLoad,
    .mp3PlayerPlay       = sp_mp3PlayerPlay,
    .mp3PlayerStop       = sp_mp3PlayerStop,
    .mp3PlayerPause      = sp_mp3PlayerPause,
    .mp3PlayerResume     = sp_mp3PlayerResume,
    .mp3PlayerIsPlaying  = sp_mp3PlayerIsPlaying,
    .mp3PlayerSetVolume  = sp_mp3PlayerSetVolume,
    .mp3PlayerGetVolume  = sp_mp3PlayerGetVolume,
    .mp3PlayerSetLoop    = sp_mp3PlayerSetLoop,
    .mp3PlayerFree       = sp_mp3PlayerFree,
};

// ── App config impl ───────────────────────────────────────────────────────────

static const picocalc_appconfig_t s_appconfig_impl = {
    .load     = app_config_load_own,  // the running app's own id only
    .save     = appconfig_save,
    .get      = appconfig_get,
    .set      = appconfig_set,
    .clear    = appconfig_clear,
    .reset    = appconfig_reset,
    .getAppId = appconfig_get_app_id,
};

// ── Crypto impl ───────────────────────────────────────────────────────────────

// Fails closed like picocalc.crypto.randomBytes: without a seeded, working
// DRBG buf is zeroed and false is returned (it used to be zeros, silently).
static bool crypto_random_bytes_w(uint8_t *buf, uint32_t len) {
    if (!rng_bytes(buf, len)) {
        printf("[CRYPTO] randomBytes(%lu): no cryptographic RNG, buffer zeroed\n",
               (unsigned long)len);
        return false;
    }
    return true;
}

static pccrypto_aes_t crypto_aes_new_w(const uint8_t *key, uint32_t klen,
                                        const uint8_t *nonce) {
    return (pccrypto_aes_t)crypto_aes_new(key, klen, nonce);
}

static int crypto_aes_update_w(pccrypto_aes_t ctx,
                                const uint8_t *in, uint8_t *out, uint32_t len) {
    return crypto_aes_update((crypto_aes_t *)ctx, in, out, len);
}

static void crypto_aes_free_w(pccrypto_aes_t ctx) {
    crypto_aes_free((crypto_aes_t *)ctx);
}

static pccrypto_ecdh_t crypto_ecdh_x25519_w(void) {
    return (pccrypto_ecdh_t)crypto_ecdh_x25519();
}

static pccrypto_ecdh_t crypto_ecdh_p256_w(void) {
    return (pccrypto_ecdh_t)crypto_ecdh_p256();
}

static void crypto_ecdh_get_pubkey_w(pccrypto_ecdh_t ctx,
                                      uint8_t *out, uint32_t *out_len) {
    crypto_ecdh_get_public_key((crypto_ecdh_t *)ctx, out, out_len);
}

static int crypto_ecdh_shared_w(pccrypto_ecdh_t ctx,
                                 const uint8_t *remote, uint32_t rlen,
                                 uint8_t *out, uint32_t *out_len) {
    return crypto_ecdh_compute_shared((crypto_ecdh_t *)ctx, remote, rlen,
                                      out, out_len);
}

static void crypto_ecdh_free_w(pccrypto_ecdh_t ctx) {
    crypto_ecdh_free((crypto_ecdh_t *)ctx);
}

static const picocalc_crypto_t s_crypto_impl = {
    .sha256            = crypto_sha256,
    .sha1              = crypto_sha1,
    .hmacSha256        = crypto_hmac_sha256,
    .hmacSha1          = crypto_hmac_sha1,
    .randomBytes       = crypto_random_bytes_w,
    .deriveKey         = crypto_derive_key,
    .aesNew            = crypto_aes_new_w,
    .aesUpdate         = crypto_aes_update_w,
    .aesFree           = crypto_aes_free_w,
    .ecdhX25519        = crypto_ecdh_x25519_w,
    .ecdhP256          = crypto_ecdh_p256_w,
    .ecdhGetPublicKey  = crypto_ecdh_get_pubkey_w,
    .ecdhComputeShared = crypto_ecdh_shared_w,
    .ecdhFree          = crypto_ecdh_free_w,
    .rsaVerify         = crypto_rsa_verify,
    .ecdsaP256Verify   = crypto_ecdsa_p256_verify,
};

// ── Graphics impl ─────────────────────────────────────────────────────────────

static pcimage_t gfx_load(const char *path) {
    return (pcimage_t)image_load(path);
}
static pcimage_t gfx_new_blank(int w, int h) {
    return (pcimage_t)image_new_blank(w, h);
}
static void gfx_free(pcimage_t img) {
    image_free((pc_image_t *)img);
}
static int gfx_width(pcimage_t img) {
    return ((pc_image_t *)img)->w;
}
static int gfx_height(pcimage_t img) {
    return ((pc_image_t *)img)->h;
}
static uint16_t *gfx_pixels(pcimage_t img) {
    return ((pc_image_t *)img)->data;
}
static void gfx_set_transparent_color(pcimage_t img, uint16_t color) {
    ((pc_image_t *)img)->transparent_color = color;
}
static void gfx_draw(pcimage_t img, int x, int y) {
    image_draw((const pc_image_t *)img, x, y);
}
static void gfx_draw_region(pcimage_t img, int sx, int sy, int sw, int sh,
                             int dx, int dy) {
    image_draw_region((const pc_image_t *)img, sx, sy, sw, sh, dx, dy);
}
static void gfx_draw_scaled(pcimage_t img, int x, int y, int dst_w, int dst_h) {
    image_draw_scaled((const pc_image_t *)img, x, y, dst_w, dst_h);
}

static const picocalc_graphics_t s_graphics_impl = {
    .load                = gfx_load,
    .newBlank            = gfx_new_blank,
    .free                = gfx_free,
    .width               = gfx_width,
    .height              = gfx_height,
    .pixels              = gfx_pixels,
    .setTransparentColor = gfx_set_transparent_color,
    .draw                = gfx_draw,
    .drawRegion          = gfx_draw_region,
    .drawScaled          = gfx_draw_scaled,
};

// ── Video impl ────────────────────────────────────────────────────────────────

static pcvideo_t video_new_player(void) {
    return (pcvideo_t)video_player_create();
}
static void video_free_player(pcvideo_t vp) {
    video_player_destroy((video_player_t *)vp);
}
static bool video_load_w(pcvideo_t vp, const char *path) {
    return video_player_load((video_player_t *)vp, path);
}
static void video_play_w(pcvideo_t vp) {
    video_player_play((video_player_t *)vp);
}
static void video_pause_w(pcvideo_t vp) {
    video_player_pause((video_player_t *)vp);
}
static void video_resume_w(pcvideo_t vp) {
    video_player_resume((video_player_t *)vp);
}
static void video_stop_w(pcvideo_t vp) {
    video_player_stop((video_player_t *)vp);
}
static bool video_update_w(pcvideo_t vp) {
    return video_player_update((video_player_t *)vp);
}
static void video_seek_w(pcvideo_t vp, uint32_t frame) {
    video_player_seek((video_player_t *)vp, frame);
}
static float video_get_fps_w(pcvideo_t vp) {
    return video_player_get_fps((video_player_t *)vp);
}
static void video_get_size_w(pcvideo_t vp, uint32_t *w, uint32_t *h) {
    video_player_t *p = (video_player_t *)vp;
    if (w) *w = p->width;
    if (h) *h = p->height;
}
static bool video_is_playing_w(pcvideo_t vp) {
    video_player_t *p = (video_player_t *)vp;
    return p->playing && !p->paused;
}
static bool video_is_paused_w(pcvideo_t vp) {
    return ((video_player_t *)vp)->paused;
}
static void video_set_loop_w(pcvideo_t vp, bool loop) {
    ((video_player_t *)vp)->loop = loop;
}
static void video_set_auto_flush_w(pcvideo_t vp, bool af) {
    ((video_player_t *)vp)->auto_flush = af;
}
static bool video_has_audio_w(pcvideo_t vp) {
    return video_player_has_audio((video_player_t *)vp);
}
static void video_set_volume_w(pcvideo_t vp, uint8_t vol) {
    video_player_set_audio_volume((video_player_t *)vp, vol);
}
static uint8_t video_get_volume_w(pcvideo_t vp) {
    return video_player_get_audio_volume((video_player_t *)vp);
}
static void video_set_muted_w(pcvideo_t vp, bool muted) {
    video_player_set_audio_muted((video_player_t *)vp, muted);
}
static bool video_get_muted_w(pcvideo_t vp) {
    return video_player_get_audio_muted((video_player_t *)vp);
}
static uint32_t video_get_dropped_w(pcvideo_t vp) {
    return video_player_get_dropped_frames((video_player_t *)vp);
}
static void video_reset_stats_w(pcvideo_t vp) {
    video_player_reset_stats((video_player_t *)vp);
}
static uint32_t video_get_frame_count_w(pcvideo_t vp) {
    return video_player_get_frame_count((video_player_t *)vp);
}
static uint32_t video_get_duration_ms_w(pcvideo_t vp) {
    return video_player_get_duration_ms((video_player_t *)vp);
}
static uint32_t video_get_position_ms_w(pcvideo_t vp) {
    return video_player_get_position_ms((video_player_t *)vp);
}
static void video_seek_ms_w(pcvideo_t vp, uint32_t ms) {
    video_player_seek_ms((video_player_t *)vp, ms);
}
static void video_seek_relative_ms_w(pcvideo_t vp, int32_t delta_ms) {
    video_player_seek_relative_ms((video_player_t *)vp, delta_ms);
}
static bool video_has_ended_w(pcvideo_t vp) {
    return video_player_has_ended((video_player_t *)vp);
}
static void video_set_osd_w(pcvideo_t vp, bool enabled) {
    video_player_set_osd((video_player_t *)vp, enabled);
}
static void video_show_osd_w(pcvideo_t vp) {
    video_player_show_osd((video_player_t *)vp);
}
static void video_set_osd_timeout_w(pcvideo_t vp, uint32_t ms) {
    video_player_set_osd_timeout((video_player_t *)vp, ms);
}

static const picocalc_video_t s_video_impl = {
    .newPlayer       = video_new_player,
    .free            = video_free_player,
    .load            = video_load_w,
    .play            = video_play_w,
    .pause           = video_pause_w,
    .resume          = video_resume_w,
    .stop            = video_stop_w,
    .update          = video_update_w,
    .seek            = video_seek_w,
    .getFPS          = video_get_fps_w,
    .getSize         = video_get_size_w,
    .isPlaying       = video_is_playing_w,
    .isPaused        = video_is_paused_w,
    .setLoop         = video_set_loop_w,
    .setAutoFlush    = video_set_auto_flush_w,
    .hasAudio        = video_has_audio_w,
    .setVolume       = video_set_volume_w,
    .getVolume       = video_get_volume_w,
    .setMuted        = video_set_muted_w,
    .getMuted        = video_get_muted_w,
    .getDroppedFrames = video_get_dropped_w,
    .resetStats      = video_reset_stats_w,
    .getFrameCount   = video_get_frame_count_w,
    .getDurationMs   = video_get_duration_ms_w,
    .getPositionMs   = video_get_position_ms_w,
    .seekMs          = video_seek_ms_w,
    .seekRelativeMs  = video_seek_relative_ms_w,
    .hasEnded        = video_has_ended_w,
    .setOSD          = video_set_osd_w,
    .showOSD         = video_show_osd_w,
    .setOSDTimeout   = video_set_osd_timeout_w,
};

// ── MOD player wrappers (opaque void* API) ───────────────────────────────────
static pcmodplayer_t mod_create_w(void) { return mod_player_create(); }
static void mod_destroy_w(pcmodplayer_t mp) { mod_player_destroy((mod_player_t *)mp); }
static bool mod_load_w(pcmodplayer_t mp, const char *p) { return mod_player_load((mod_player_t *)mp, p); }
static void mod_play_w(pcmodplayer_t mp, bool loop) { mod_player_play((mod_player_t *)mp, loop); }
static void mod_stop_w(pcmodplayer_t mp) { mod_player_stop((mod_player_t *)mp); }
static void mod_pause_w(pcmodplayer_t mp) { mod_player_pause((mod_player_t *)mp); }
static void mod_resume_w(pcmodplayer_t mp) { mod_player_resume((mod_player_t *)mp); }
static bool mod_is_playing_w(pcmodplayer_t mp) { return mod_player_is_playing((const mod_player_t *)mp); }
static void mod_set_volume_w(pcmodplayer_t mp, uint8_t v) { mod_player_set_volume((mod_player_t *)mp, v); }
static uint8_t mod_get_volume_w(pcmodplayer_t mp) { return mod_player_get_volume((const mod_player_t *)mp); }
static void mod_set_loop_w(pcmodplayer_t mp, bool l) { mod_player_set_loop((mod_player_t *)mp, l); }

static const picocalc_modplayer_t s_modplayer_impl = {
    .create    = mod_create_w,
    .destroy   = mod_destroy_w,
    .load      = mod_load_w,
    .play      = mod_play_w,
    .stop      = mod_stop_w,
    .pause     = mod_pause_w,
    .resume    = mod_resume_w,
    .isPlaying = mod_is_playing_w,
    .setVolume = mod_set_volume_w,
    .getVolume = mod_get_volume_w,
    .setLoop   = mod_set_loop_w,
};

// ── ZIP extraction (thin wrappers for g_api — Lua bridge has its own richer API) ──
// zip_archive.c delegates to the shared hardened engine (os/zip_util.c): the
// archive is streamed from SD (no whole-file PSRAM copy), entry names are
// validated (the old native path had NO traversal guard), parent directories
// are created, and size/entry-count caps are enforced. The simulator's Unicorn
// trampolines link the same zip_archive.c, so both worlds get the same engine.

#include "os/zip_archive.h"

static const picocalc_zip_t s_zip_impl = {
    .extract      = zip_archive_extract,
    .list         = zip_archive_list,
    .open         = zip_archive_open,
    .close        = zip_archive_close,
    .numEntries   = zip_archive_num_entries,
    .locate       = zip_archive_locate,
    .statIndex    = zip_archive_stat_index,
    .read         = zip_archive_read,
    .extractEntry = zip_archive_extract_entry,
};

// ── Core 1 entry — background WiFi polling ──────��─────────────────────────────
// Core 1 runs the audio pollers and the Mongoose / CYW43 network stack on a
// 1 ms tick (wifi_poll spaces its idle polls to 5 ms on its own).
// wifi_poll() acquires display_spi_lock() internally, so the SPI1 bus
// (shared between the LCD and the WiFi chip) is safe to access from here.
// Lua apps benefit automatically; native apps benefit via http_fire_c_pending().

// Set to true to temporarily pause Core 1's Mongoose/WiFi polling.
// Used during native app loading to eliminate PSRAM heap contention between
// Core 1's umm_malloc/umm_free and the ELF loader on Core 0.
_Atomic bool g_core1_pause = false;
_Atomic bool g_core1_paused = false; // acknowledgment flag for Core 1 pause handshake

// Optional audio callback for native apps (e.g. DOOM) that offload mixing
// to Core 1.  Set by the app at startup, cleared on exit.
_Atomic(void (*)(void)) g_native_audio_callback = NULL;

static repeating_timer_t s_core1_timer;
static volatile bool s_core1_tick_pending = false;

// core1_stats.h. Core 1 writes these (Core 0 only asks for a reset, which
// Core 1 carries out at its next tick); Core 0 reads them unlocked.
static volatile uint32_t s_tick_count, s_tick_over, s_tick_missed, s_tick_max_us;
static volatile uint64_t s_tick_window_t0_us;
static volatile bool s_tick_reset_req;

static bool core1_timer_callback(repeating_timer_t *rt) {
  (void)rt;
  if (s_core1_tick_pending)
    s_tick_missed++;  // the previous tick has not started: this one merges into it
  s_core1_tick_pending = true;
  return true;
}

void core1_get_tick_stats(core1_tick_stats_t *out) {
  out->window_ms = (uint32_t)((time_us_64() - s_tick_window_t0_us) / 1000);
  out->ticks = s_tick_count;
  out->over = s_tick_over;
  out->missed = s_tick_missed;
  out->max_us = s_tick_max_us;
}

void core1_reset_tick_stats(void) {
  s_tick_reset_req = true;
}

// Doorbell ISR: Core 0 rings WIFI_IPC_DOORBELL after pushing to the IPC
// queue.  This wakes Core 1 from __wfi() immediately (<1us latency)
// instead of waiting for the next 1 ms tick.
static void core1_doorbell_isr(void) {
  multicore_doorbell_clear_current_core(WIFI_IPC_DOORBELL);
  s_core1_tick_pending = true;
}

// Relay the watchdog for Core 0 while its heartbeat is fresh — long
// CPU-bound stretches in apps (no poll for >10s) must not reboot the
// device, but a Core 0 hung for over a minute still should. Applied while
// Core 1 is paused too: Core 0 pauses it around ELF loads and clock
// changes, and a hang there must still reset.
static inline void core1_relay_watchdog(void) {
  uint32_t now_ms = to_ms_since_boot(get_absolute_time());
  if (now_ms - g_core0_heartbeat_ms < CORE0_HEARTBEAT_STALE_MS)
    watchdog_update();
}

// app_stack_run_os() adapter: Core 0 RNG seed + CA bundle parse at boot.
static void boot_crypto_init(void *arg) {
  (void)arg;
  (void)rng_init_this_core();
  // Same stack, same reason: parse the TLS root bundle once (wifi.h).
  (void)wifi_tls_init();
}

static void core1_entry(void) {
  volatile uint32_t *scb_ccr = (volatile uint32_t *)(0xE000ED14);
  *scb_ccr &= ~(1u << 3);
  __asm volatile ("dsb sy" ::: "memory");
  __asm volatile ("isb sy" ::: "memory");

  // Paint Core 1's stack for the `stack` dev command's core1_peak (the
  // verified-TLS handshake runs on this 4 KB stack).
  app_stack_paint_core1();

  // Seed Core 1's CSPRNG (Mongoose TLS) here, at the bottom of its 4 KB
  // stack: the seed path is ~2 KB deep and must never run inside
  // mg_mgr_poll (see rng.h).  On failure TLS is refused, not weakened.
  (void)rng_init_this_core();

  audio_core1_init();

  // Register doorbell ISR for instant IPC wake-up from Core 0
  uint doorbell_irq = multicore_doorbell_irq_num(WIFI_IPC_DOORBELL);
  irq_set_exclusive_handler(doorbell_irq, core1_doorbell_isr);
  irq_set_enabled(doorbell_irq, true);

  // 1 ms, not 5: several audio pollers refill a fixed amount per call.
  // mod_player_update renders 128 frames @22050 Hz (5.8 ms of audio) per
  // tick, so a 5 ms tick leaves only 16% headroom, and under WiFi load the
  // real tick rate has been measured at ~260/s even at 1 ms nominal. MP3
  // staging (8 KB, ~46 ms) likewise relies on frequent refills between
  // decode bursts. Negative period = fixed rate from the previous start.
  alarm_pool_t *pool = audio_get_core1_alarm_pool();
  alarm_pool_add_repeating_timer_ms(pool, -1, core1_timer_callback, NULL, &s_core1_timer);

  while (true) {
    if (atomic_load(&g_core1_pause)) {
      atomic_store(&g_core1_paused, true);
      __dmb(); // ensure paused flag visible to Core 0 before we spin
      while (atomic_load(&g_core1_pause)) {
        core1_relay_watchdog();
        sleep_ms(1);
      }
      atomic_store(&g_core1_paused, false);
      __dmb(); // ensure resumed state visible to Core 0
      continue;
    }

    if (s_core1_tick_pending) {
      s_core1_tick_pending = false;
      if (s_tick_reset_req) {
        s_tick_count = s_tick_over = s_tick_missed = s_tick_max_us = 0;
        s_tick_window_t0_us = time_us_64();
        s_tick_reset_req = false;
      }
      uint32_t tick_t0 = time_us_32();

      core1_relay_watchdog();

      wifi_poll();
      http_fire_c_pending();

      audio_stream_poll();
      mp3_player_update();
      fileplayer_update();
      sound_pump_callbacks();
      mod_player_update();
      void (*audio_cb)(void) = atomic_load(&g_native_audio_callback);
      if (audio_cb)
        audio_cb();

      image_preload_update();
      video_prefetch_update();

      // DIAG: code-corruption watcher — scan the native app's read-only
      // image against the load-time snapshot in rotating 32KB chunks (both
      // via the uncached PSRAM alias: no XIP-cache pollution).  On a hit,
      // report onset time, offsets, and old/new/cached-view bytes — the
      // data pattern fingerprints whoever tramples app code — then repair
      // the damage so the run continues and repeat events get logged.
      if (atomic_load(&g_code_watch_active)) {
        static uint32_t s_watch_pos = 0;
        static uint32_t s_watch_passes = 0;
        static int s_watch_reports = 0;
        const uint8_t *snap = g_code_watch_snap;
        const uint8_t *live = g_code_watch_live;
        uint32_t size = g_code_watch_size;
        if (snap && live && size) {
          uint32_t chunk = 32u * 1024;
          if (s_watch_pos >= size) {
            s_watch_pos = 0;
            if ((++s_watch_passes % 64) == 0)
              printf("[CODEWATCH] clean pass #%lu\n",
                     (unsigned long)s_watch_passes);
          }
          uint32_t n = size - s_watch_pos;
          if (n > chunk) n = chunk;
          const uint8_t *s = snap + s_watch_pos;
          const uint8_t *l = live + s_watch_pos;
          if (memcmp(s, l, n) != 0 && s_watch_reports < 40) {
            uint32_t first = 0, last = 0, count = 0;
            for (uint32_t i = 0; i < n; i++) {
              if (s[i] != l[i]) {
                if (!count) first = i;
                last = i;
                count++;
              }
            }
            // Re-read the differing span: uncached PSRAM reads have been
            // seen to glitch transiently, and a transient misread is itself
            // a finding (report, but don't "repair" good memory with it).
            if (memcmp(s + first, l + first, last - first + 1) == 0) {
              printf("[CODEWATCH] TRANSIENT misread t=%lums off=0x%05lx n=%lu\n",
                     (unsigned long)(time_us_64() / 1000),
                     (unsigned long)(s_watch_pos + first),
                     (unsigned long)count);
            } else {
              uint32_t off = s_watch_pos + first;
              printf("[CODEWATCH] CORRUPT t=%lums off=0x%05lx..0x%05lx n=%lu\n",
                     (unsigned long)(time_us_64() / 1000),
                     (unsigned long)off,
                     (unsigned long)(s_watch_pos + last),
                     (unsigned long)count);
              uint32_t d = first & ~15u;
              printf("[CODEWATCH] was:");
              for (int i = 0; i < 16; i++) printf(" %02x", s[d + i]);
              printf("\n[CODEWATCH] psram:");
              for (int i = 0; i < 16; i++) printf(" %02x", l[d + i]);
              // Cached view of the same bytes — if it differs from the psram
              // view, the corruption lives in the XIP cache, not PSRAM.
              const uint8_t *lc = l + d - 0x04000000u; // uncached -> cached
              printf("\n[CODEWATCH] cache:");
              for (int i = 0; i < 16; i++) printf(" %02x", lc[i]);
              printf("\n");
              // Repair: restore PSRAM from the snapshot, then invalidate the
              // XIP range so the CPU refetches the clean bytes.
              memcpy((void *)(uintptr_t)(l + first), s + first,
                     last - first + 1);
              uintptr_t inv_start =
                  ((uintptr_t)(l + first) - 0x04000000u - XIP_BASE) & ~7u;
              uintptr_t inv_end =
                  (((uintptr_t)(l + last) - 0x04000000u - XIP_BASE) + 8u) & ~7u;
              __asm volatile ("dsb sy");
              xip_cache_invalidate_range(inv_start, inv_end - inv_start);
              __asm volatile ("isb sy");
            }
            s_watch_reports++;
          }
          s_watch_pos += n;
        }
      }

      // PHASE-0 AUDIO DIAGNOSTICS: log stream stats every ~2s when something
      // is driving the audio callback.  Helps classify Doom-audio symptoms —
      // zero ISR ⇒ DMA never started; spiking underruns ⇒ Core 1 starvation;
      // steady ISR + zero underruns ⇒ producer issue in the app itself.
      if (audio_cb) {
        static uint64_t s_last_audio_debug_us = 0;
        uint64_t now = time_us_64();
        if (now - s_last_audio_debug_us >= 2000000) {
          s_last_audio_debug_us = now;
          uint32_t isr_cnt = 0, underruns = 0, ring_used = 0;
          audio_stream_debug(&isr_cnt, &underruns, &ring_used);
          printf("[AUDIO] ISR=%lu underruns=%lu ring_used=%lu\n",
                 (unsigned long)isr_cnt, (unsigned long)underruns,
                 (unsigned long)ring_used);
        }
      }

      uint32_t tick_us = time_us_32() - tick_t0;
      s_tick_count++;
      if (tick_us > 1000)
        s_tick_over++;
      if (tick_us > s_tick_max_us)
        s_tick_max_us = tick_us;
    }

    __wfi();
  }
}

// ── Main ─────────────────────────────────────────────────────────────────────

// ── Crash persistence across watchdog resets ─────────────────────────────────
static uint32_t s_crash_data[8];
static bool s_had_crash = false;

#define CRASH_LINE_CAP 2048
static void crash_log_save(const char *app_name) {
  if (!s_had_crash) return;

  // Truncate if log is too large (>64KB)
  int size = sdcard_fsize("/system/crashlog.txt");
  if (size > 0 && (uint32_t)size > 64u * 1024u) {
    sdfile_t tf = sdcard_fopen("/system/crashlog.txt", "w");
    if (tf) sdcard_fclose(tf);
  }

  sdfile_t f = sdcard_fopen("/system/crashlog.txt", "a");
  if (!f) return;

  // Record layout: see the comment above hardfault_c.
  uint32_t flags = s_crash_data[0];
  uint32_t cfsr = s_crash_data[3];
  uint32_t hfsr = ((flags & CRASH_F_HFSR_FORCED) ? (1u << 30) : 0u)
                | ((flags & CRASH_F_HFSR_VECTBL) ? (1u << 1) : 0u);
  uint32_t sfsr = flags & 0xFFu;
  bool was_psp = (flags & CRASH_F_PSP) != 0;
  const char *stack_name = !was_psp                     ? "MSP (OS)"
                           : (flags & CRASH_F_PSP_LUA) ? "PSP (Lua VM)"
                           : (flags & CRASH_F_PSP_OS)  ? "PSP (OS command)"
                                                       : "PSP (native app)";
  uint32_t crash_uptime_sec = s_crash_data[7];
  // A stack-limit violation taken while stacking the exception frame leaves
  // the frame contents UNKNOWN (ARMv8-M), so the stacked PC/LR are not
  // trustworthy then.
  bool stkof = (cfsr & (1u << 20)) != 0;
  const char *unreliable = stkof ? "  (unreliable: STKOF)" : "";

  // From the PSRAM heap: main() runs on the 4 KB MSP. Sized so that every
  // decoded line at once still fits (snprintf offsets never pass the end).
  char *line = (char *)umm_malloc(CRASH_LINE_CAP);
  if (!line) { sdcard_fclose(f); return; }
  int n = snprintf(line, CRASH_LINE_CAP,
    "--- HARDFAULT ---\n"
    "  Uptime: %lum %lus\n"
    "  App: %s\n"
    "%s"
    "  PC   = 0x%08lx%s\n"
    "  LR   = 0x%08lx%s\n"
    "  SP   = 0x%08lx\n"
    "  CFSR = 0x%08lx\n"
    "  HFSR = 0x%08lx\n"
    "  SFSR = 0x%08lx\n"
    "  Stack: %s\n",
    (unsigned long)(crash_uptime_sec / 60u), (unsigned long)(crash_uptime_sec % 60u),
    (app_name && app_name[0]) ? app_name : "(none -- OS/launcher)",
    stkof ? "  STACK OVERFLOW (MSPLIM/PSPLIM)\n" : "",
    (unsigned long)s_crash_data[1], unreliable,
    (unsigned long)s_crash_data[2], unreliable,
    (unsigned long)s_crash_data[5], (unsigned long)cfsr,
    (unsigned long)hfsr, (unsigned long)sfsr,
    stack_name);

  if (flags & CRASH_F_BOOTING)
    n += snprintf(line+n, CRASH_LINE_CAP-n, "  During boot (attempt %lu)\n",
                  (unsigned long)(((flags & CRASH_BOOT_ATTEMPT_MASK)
                                   >> CRASH_BOOT_ATTEMPT_SHIFT) + 1u));

  // scratch[6] decode mirrors hardfault_c's packing conditions.
  if (sfsr & (1u << 6)) {
    n += snprintf(line+n, CRASH_LINE_CAP-n, "  SFAR = 0x%08lx\n",
                  (unsigned long)s_crash_data[6]);
  } else if (cfsr != 0 || sfsr != 0) {
    n += snprintf(line+n, CRASH_LINE_CAP-n, "  BFAR = 0x%08lx\n",
                  (unsigned long)s_crash_data[6]);
  } else {
    n += snprintf(line+n, CRASH_LINE_CAP-n,
                  "  IPSR = %lu (3=HardFault)  stackedISR = %lu  [PC] = 0x%04lx\n",
                  (unsigned long)((s_crash_data[6] >> 16) & 0x1FFu),
                  (unsigned long)((s_crash_data[6] >> 25) & 0x7Fu),
                  (unsigned long)(s_crash_data[6] & 0xFFFFu));
  }

  // Decode CFSR/HFSR flags into human-readable text
  if (cfsr & (1u<<17)) n += snprintf(line+n, CRASH_LINE_CAP-n, "  INVSTATE: invalid CPU state\n");
  if (cfsr & (1u<<16)) n += snprintf(line+n, CRASH_LINE_CAP-n, "  UNDEFINSTR: undefined instruction\n");
  if (cfsr & (1u<<18)) n += snprintf(line+n, CRASH_LINE_CAP-n, "  INVPC: invalid EXC_RETURN/PC\n");
  if (cfsr & (1u<<19)) n += snprintf(line+n, CRASH_LINE_CAP-n, "  NOCP: coprocessor access\n");
  if (cfsr & (1u<< 9)) n += snprintf(line+n, CRASH_LINE_CAP-n, "  PRECISERR: precise data bus fault\n");
  if (cfsr & (1u<< 8)) n += snprintf(line+n, CRASH_LINE_CAP-n, "  IBUSERR: instruction bus fault\n");
  if (cfsr & (1u<<10)) n += snprintf(line+n, CRASH_LINE_CAP-n, "  IMPRECISERR: imprecise data bus fault\n");
  if (cfsr & (1u<<12)) n += snprintf(line+n, CRASH_LINE_CAP-n, "  STKERR: exception stack push fault\n");
  if (cfsr & (1u<<11)) n += snprintf(line+n, CRASH_LINE_CAP-n, "  UNSTKERR: exception stack pop fault\n");
  if (cfsr & (1u<< 1)) n += snprintf(line+n, CRASH_LINE_CAP-n, "  DACCVIOL: MPU data access violation\n");
  if (cfsr & (1u<< 0)) n += snprintf(line+n, CRASH_LINE_CAP-n, "  IACCVIOL: MPU instruction access violation\n");
  if (cfsr & (1u<<25)) n += snprintf(line+n, CRASH_LINE_CAP-n, "  DIVBYZERO\n");
  if (cfsr & (1u<<24)) n += snprintf(line+n, CRASH_LINE_CAP-n, "  UNALIGNED access\n");
  if (cfsr & (1u<<20)) n += snprintf(line+n, CRASH_LINE_CAP-n, "  STKOF: stack overflow (MSPLIM/PSPLIM limit hit)\n");
  if (hfsr & (1u<<30)) n += snprintf(line+n, CRASH_LINE_CAP-n, "  HFSR: FORCED escalation\n");
  if (hfsr & (1u<< 1)) n += snprintf(line+n, CRASH_LINE_CAP-n, "  HFSR: vector table fault\n");
  if (sfsr & (1u<<0))  n += snprintf(line+n, CRASH_LINE_CAP-n, "  SFSR INVEP: invalid NS->S entry\n");
  if (sfsr & (1u<<1))  n += snprintf(line+n, CRASH_LINE_CAP-n, "  SFSR INVIS: invalid integrity signature\n");
  if (sfsr & (1u<<2))  n += snprintf(line+n, CRASH_LINE_CAP-n, "  SFSR INVER: invalid exception return\n");
  if (sfsr & (1u<<3))  n += snprintf(line+n, CRASH_LINE_CAP-n, "  SFSR AUVIOL: attribution violation\n");
  if (sfsr & (1u<<4))  n += snprintf(line+n, CRASH_LINE_CAP-n, "  SFSR INVTRAN: invalid S<->NS transition\n");
  if (sfsr & (1u<<5))  n += snprintf(line+n, CRASH_LINE_CAP-n, "  SFSR LSPERR: lazy FP preservation error\n");
  if (sfsr & (1u<<7))  n += snprintf(line+n, CRASH_LINE_CAP-n, "  SFSR LSERR: lazy state error\n");
  n += snprintf(line+n, CRASH_LINE_CAP-n, "\n");

  sdcard_fwrite(f, line, n);
  sdcard_fclose(f);
  umm_free(line);
  printf("[CRASH] Saved crash log to /system/crashlog.txt\n");
}

int main(void) {
  // Paint the free part of the 4 KB main stack so the `stack` dev command
  // can report its high-water mark (the launcher and IRQs stay on the MSP).
  app_stack_paint_msp();

  // ── Boot-loop detection using watchdog scratch[0] ──────────────────────────
  // scratch[0] encoding:
  //   CRASH_TAG   (0xFA17xxxx) = HardFault record present (see hardfault_c);
  //                              CRASH_F_BOOTING marks a fault during boot
  //   BOOT_MAGIC  (0xB00700xx) = boot attempt counter (low byte = count)
  //   anything else            = fresh boot (power-on or clean reset)
  // A boot that hangs (watchdog) or faults counts as a failed attempt. The
  // counter only goes back to 0 when a boot reaches the launcher; power-on
  // clears the scratch registers.
  uint32_t scratch0 = watchdog_hw->scratch[0];
  bool wdt_reset = watchdog_caused_reboot();
  int boot_attempt = 0;
  bool skip_boot_watchdog = false;

  if ((scratch0 & CRASH_TAG_MASK) == CRASH_TAG) {
    // HardFault crash recovery — preserve fault data for later display
    memcpy(s_crash_data, (void *)watchdog_hw->scratch, sizeof(s_crash_data));
    s_had_crash = true;
    // scratch[1] holds the stacked PC in a crash record, and is also the OTA
    // intent token's slot (OTA_SCRATCH_IDX): clear it so a PC that happens to
    // equal OTA_MAGIC can never read as an update request.
    watchdog_hw->scratch[OTA_SCRATCH_IDX] = 0;
    if (scratch0 & CRASH_F_BOOTING)
      boot_attempt = (int)((scratch0 & CRASH_BOOT_ATTEMPT_MASK)
                           >> CRASH_BOOT_ATTEMPT_SHIFT) + 1;
  } else if ((scratch0 & BOOT_MAGIC_MASK) == BOOT_MAGIC) {
    // Previous boot failed during init — increment attempt counter
    boot_attempt = (int)(scratch0 & 0xFF) + 1;
  } else if (watchdog_caused_reboot()) {
    // Watchdog timeout without HardFault or boot counter (legacy path)
    printf("[WATCHDOG] Reset due to timeout (no fault data)\n");
  }
  if (boot_attempt >= BOOT_MAX_RETRIES) {
    // Too many boot failures — leave the watchdog off for the rest of boot
    // so the device stays on whatever screen it stops at instead of
    // looping. The count stays at the limit (not reset), so a further
    // reset keeps the watchdog off; power-cycling clears it.
    skip_boot_watchdog = true;
    boot_attempt = BOOT_MAX_RETRIES;
    watchdog_disable();
  }

  // Write boot attempt counter — cleared once launcher starts successfully
  watchdog_hw->scratch[0] = BOOT_MAGIC | (uint32_t)boot_attempt;

  // Overclock to 200 MHz for better display throughput (RP2350 supports 150+)
  // NOTE: If the keyboard fails to initialise reliably, try commenting this
  // out to test at the default 125 MHz — it isolates whether the overclock
  // is affecting I2C timing.
  set_sys_clock_khz(200000, false);

  // Configure peripheral clock to 125 MHz (enables 62.5 MHz SPI for LCD)
  // clk_peri drives UART, SPI, I2C, PWM — ST7789 rated max is 62.5 MHz
  clock_configure(
      clk_peri,
      0,                                                // No glitchless mux
      CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLKSRC_PLL_SYS, // Source: PLL_SYS
                                                        // (200MHz)
      200 * MHZ,                                        // Input frequency
      200 * MHZ); // Output: 200 MHz → SPI can reach 100 MHz

  // Enable lazy floating-point context save.  Without this, any ISR that
  // *might* touch FP registers stacks the full 32-register FP context (128
  // bytes) on every entry.  With LSPEN+ASPEN the context is only saved if
  // the ISR actually executes an FP instruction, saving ~12 cycles on ISR
  // entry for the common case (audio DMA, keyboard, etc.).
  // FPCCR: bit 31 = ASPEN (automatic state preservation),
  //        bit 30 = LSPEN (lazy stacking enable)
  volatile uint32_t *fpccr = (volatile uint32_t *)0xE000EF34u;
  *fpccr |= (1u << 31) | (1u << 30);

  stdio_init_all();

  // Wait up to 3 s for a USB serial host to connect so early printf output
  // isn't lost. Skips automatically if already connected.
  for (int i = 0; i < 30 && !stdio_usb_connected(); i++) {
    watchdog_update();
    sleep_ms(100);
  }
  watchdog_update();

  printf("\n--- PicoDeck booting ---\n");
  printf("[BOOT] scratch0_at_entry=0x%08lx s_had_crash=%d boot_attempt=%d\n",
         (unsigned long)scratch0, s_had_crash, boot_attempt);
  printf("[BOOT] watchdog_caused_reboot=%d\n", watchdog_caused_reboot());
  if (skip_boot_watchdog)
    printf("[BOOT] %d failed boots in a row: boot watchdog off, QMI PSRAM "
           "stays in serial mode\n", BOOT_MAX_RETRIES);

  // Wire up the global API struct
  g_api.input = &s_input_impl;
  g_api.display = &s_display_impl;
  g_api.sys = &s_sys_impl;
  g_api.wifi = &s_wifi_impl;
  g_api.audio = &s_audio_impl;
  g_api.tcp = &s_tcp_impl;
  g_api.ui = &s_ui_impl;
  g_api.psram = &s_psram_impl;
  g_api.perf = &s_perf_impl;
  g_api.terminal    = &s_terminal_impl;
  g_api.http        = &s_http_impl;
  g_api.soundplayer = &s_soundplayer_impl;
  g_api.appconfig   = &s_appconfig_impl;
  g_api.crypto      = &s_crypto_impl;
  g_api.graphics    = &s_graphics_impl;
  g_api.video       = &s_video_impl;
  g_api.modplayer   = &s_modplayer_impl;
  g_api.zip         = &s_zip_impl;
  g_api.version     = 8;  // 8 = TLS verify: http->setInsecure, tcp->connectEx; 7 = video seek/OSD; 6 = fonts; 5 = zip handles
  // fs wired after SD card init

  // Bring up the QMI PSRAM in quad (QPI) mode before any PSRAM pointers are
  // accessed.  Falls back to the reset-default serial mode if the chip fails
  // the quad self-test (see drivers/qmi_psram.c).
  //
  // Self-recovery guard: a bug in the direct-mode window stalls flash XIP and
  // hangs the CPU with no watchdog armed — an unrecoverable brick (happened
  // 2026-07-19; needed BOOTSEL).  Arm the watchdog for the attempt and mark
  // it in a scratch register: if the init hangs, the watchdog reboots and the
  // next boot sees the flag and stays on the reset-default serial mode.
#ifdef PICO_RP2350
#define QMI_QUAD_ATTEMPT_MAGIC 0x51AD9E7Bu
  //
  // After BOOT_MAX_RETRIES failed boots the watchdog must stay off, and the
  // quad attempt is unguarded without it, so stay on serial mode then too.
  if (watchdog_hw->scratch[4] == QMI_QUAD_ATTEMPT_MAGIC || skip_boot_watchdog) {
    if (!skip_boot_watchdog)
      printf("[QMI_PSRAM] previous quad-mode attempt hung — serial mode\n");
    gpio_set_function(47, GPIO_FUNC_XIP_CS1);
    xip_ctrl_hw->ctrl |= XIP_CTRL_WRITABLE_M1_BITS;
  } else {
    // watchdog_enable() itself writes scratch[4] (the SDK's non-reboot
    // magic), so set the attempt flag after it, not before.
    watchdog_enable(8000, true);
    watchdog_hw->scratch[4] = QMI_QUAD_ATTEMPT_MAGIC;
    qmi_psram_init(47);
    watchdog_update();
    watchdog_hw->scratch[4] = 0;
  }
#endif

  gpio_init(USB_VBUS_PIN);
  gpio_set_dir(USB_VBUS_PIN, GPIO_IN);

  // Initialise display first so we can show progress
  display_init();

  // Arm watchdog early so any boot hang triggers a reset.
  // Disabled after BOOT_MAX_RETRIES failures so the device stays on the
  // error screen instead of looping forever.
  if (!skip_boot_watchdog)
    watchdog_enable(10000, true); // 10s, pause on debug

  // Initialise mainboard PIO PSRAM (8MB on PIO1, independent of QMI PSRAM).
  // Non-fatal if chip not present (some boards may not have it).
  // Uses bulk driver internally for 8KB transfers (300x faster than 27-byte chunks).
  pio_psram_init();
  printf("[BOOT] pio_psram done\n"); stdio_flush(); watchdog_update();

  sound_init();
  printf("[BOOT] sound done\n"); stdio_flush(); watchdog_update();

  audio_init();
  printf("[BOOT] audio done\n"); stdio_flush(); watchdog_update();

  mod_player_init();
  printf("[BOOT] mod_player done\n"); stdio_flush(); watchdog_update();

  printf("[BOOT] drawing splash...\n"); stdio_flush();
  ui_draw_splash("Initialising keyboard...", NULL);
  printf("[BOOT] splash done\n"); stdio_flush(); watchdog_update();

  bool kbd_ok = kbd_init();
  watchdog_update();
  if (kbd_ok) {
    // Bootstrap level only — SD/config aren't up yet; the user's saved
    // brightness is restored after config_load() later in boot.
    kbd_set_backlight(128);
  } else {
    // Keyboard failed - STM32 didn't respond
    display_clear(COLOR_BLACK);
    display_draw_text(8, 8, "Keyboard Controller Error!", COLOR_RED,
                      COLOR_BLACK);
    display_draw_text(8, 20, "STM32 (I2C 0x%02X) NACK.", COLOR_WHITE,
                      COLOR_BLACK);
    display_draw_text(8, 36, "The bus may be stuck.", COLOR_GRAY, COLOR_BLACK);
    display_draw_text(8, 48, "Try power cycling device.", COLOR_GRAY,
                      COLOR_BLACK);
    display_flush();
    // We can't wait for a keypress if the keyboard is dead,
    // but we'll wait a few seconds so the user can see the error.
    watchdog_update();
    sleep_ms(5000);
  }

  ui_draw_splash("Mounting SD card...", NULL);
  watchdog_update();
  bool sd_ok = sdcard_init();
  watchdog_update();

  if (!sd_ok) {
    display_clear(COLOR_BLACK);
    display_draw_text(8, 8, "SD card not found!", COLOR_RED, COLOR_BLACK);
    display_draw_text(8, 20, "Insert a FAT32 SD card", COLOR_WHITE,
                      COLOR_BLACK);
    display_draw_text(8, 32, "and press Enter to retry.", COLOR_GRAY,
                      COLOR_BLACK);
    display_flush();

    // Wait for A press then try again
    while (true) {
      kbd_poll();
      watchdog_update();
      if (kbd_get_buttons_pressed() & BTN_ENTER) {
        sd_ok = sdcard_remount();
        watchdog_update();
        if (sd_ok)
          break;
      }
      sleep_ms(100);
    }
  }

  printf("SD card mounted OK\n");

  g_api.fs = &s_fs_impl;
  watchdog_update();

  watchdog_update();

  // Initialize the PSRAM allocator BEFORE anything that uses it
  // (SD card file ops use umm_malloc for FIL/FILINFO structs, config_load,
  // WiFi, Lua, OTA update, etc.)
  lua_psram_alloc_init();
  {
    void *pool = lua_psram_get_core1_pool();
    size_t pool_size = lua_psram_get_core1_pool_size();
    if (pool && pool_size > 0) {
      core1_alloc_init(pool, pool_size);
      // Flush dirty cache lines so Core 1 sees the init block header.
      // core1_alloc_init writes through Core 0's write-back XIP cache;
      // Core 1 has its own cache and would read stale zeros on a cold miss.
#ifndef PICODECK_SIMULATOR
      __asm volatile ("dsb sy" ::: "memory");
      xip_cache_clean_all();
      __asm volatile ("isb sy" ::: "memory");
#endif
      printf("[MAIN] Core 1 allocator: %u KB at %p\n",
             (unsigned)(pool_size / 1024), pool);
    }
  }
  watchdog_update();

  // Check for pending OTA firmware update (must be before Core 1 launch).
  // The ONLY trigger is the one-shot token in watchdog scratch that C sets
  // after consent (ota_trigger_update: sys.applyUpdate's confirm, or the
  // `reboot-ota` dev command).  A staged /system/update.bin without it —
  // e.g. written by an app that then called sys.reboot() — is not flashed:
  // it is renamed to update.bin.stale and logged.
  bool ota_pending = ota_check_pending();
  if (!ota_pending && sdcard_fsize(OTA_BIN_PATH) > 0)
    ota_discard_unrequested();
  if (ota_pending) {
    ui_draw_splash("Applying firmware update...", "DO NOT POWER OFF!");
    watchdog_update();
    if (!ota_apply_update()) {
      // Update failed — ota_apply_update left "Update failed!" and the
      // reason on screen; show it briefly, continue normal boot.
      display_draw_text(8, 188, "Booting previous firmware.", COLOR_GRAY,
                        COLOR_BLACK);
      display_flush();
      watchdog_update();
      sleep_ms(3000);
    }
    // ota_apply_update reboots on success, so we only get here on failure
  }
  watchdog_update();

  // Write crash log from previous boot (if any) — must be after PSRAM init
  // because sdcard_fopen() uses umm_malloc() for the FIL struct.
  //
  // The dirty-exit marker (/system/running.txt) survives whatever ended the
  // previous session and names the app that was running.  It feeds the
  // HardFault record, and on its own it identifies a hang the watchdog had
  // to break.  A plain power-off mid-app leaves the marker too; that is
  // normal use, so it is echoed to serial only and not logged.
  {
    // Scratch buffers come from the PSRAM heap: main() runs on the 4KB MSP
    // and the SRAM image is full to the last few bytes, so neither the
    // stack nor .bss can spare 400 bytes.
    char *prev_app = (char *)umm_malloc(64 + 320 + 56);
    char *prev_detail = prev_app ? prev_app + 64 : NULL;
    char *ln = prev_app ? prev_detail + 320 : NULL;
    bool dirty = prev_app && crashlog_read_running(prev_app, 64,
                                                   prev_detail, 320);
    crash_log_save(dirty ? prev_app : NULL);
    if (dirty) {
      const char *reason = s_had_crash ? "hardfault (see entry above)"
                         : wdt_reset   ? "watchdog timeout (Core 0 hung)"
                                       : "power loss or reset";
      printf("[BOOT] Previous app '%s' did not exit cleanly: %s\n%s",
             prev_app, reason, prev_detail);
      if (!s_had_crash && wdt_reset)
        crashlog_write_unclean_exit(reason, prev_detail);
      crashlog_clear_running();
    }
    if (s_had_crash || (dirty && wdt_reset)) {
      display_clear(COLOR_BLACK);
      display_draw_text(8, 8,
                        s_had_crash ? "Recovered from crash"
                                    : "App hung - watchdog reset",
                        COLOR_YELLOW, COLOR_BLACK);
      if (dirty) {
        snprintf(ln, 56, "App: %.48s", prev_app);
        display_draw_text(8, 24, ln, COLOR_WHITE, COLOR_BLACK);
      }
      display_draw_text(8, 40, "See /system/crashlog.txt", COLOR_GRAY, COLOR_BLACK);
      display_flush();
      watchdog_update();
      sleep_ms(2000);
      watchdog_update();
    }
    if (prev_app) umm_free(prev_app);
  }
  watchdog_update();

  // Load persisted settings from /system/config.json
  config_load();
  watchdog_update();

  // Idle screen dimming (burn-in protection). Config "dim_timeout_s"
  // overrides the 60s default; "0" disables dimming entirely. Backlight
  // level restores from config "brightness" (floor 16 — see config.h).
  {
    uint32_t dim_timeout_s = 60;
    const char *dt = config_get("dim_timeout_s");
    if (dt)
      dim_timeout_s = (uint32_t)atoi(dt);
    uint8_t brightness = config_parse_brightness(config_get("brightness"));
    kbd_set_backlight(brightness);
    idle_dim_init(brightness, dim_timeout_s);
  }

  // Seed Core 0's CSPRNG before anything draws from it (wifi_init's
  // mg_tcpip_init already calls mg_random).  Seeding needs ~2 KB of stack:
  // run it on the OS stack, not the 4 KB MSP (see rng.h).
  if (!app_stack_run_os(boot_crypto_init, NULL))
    printf("[RNG] core 0: no OS stack for seeding\n");
  watchdog_update();

  // Initialise WiFi hardware (auto-connects if credentials are in config)
  ui_draw_splash("Initialising WiFi...", NULL);
  watchdog_update();
  toast_init();
  wifi_init();
  http_init();
  tcp_init();
  watchdog_update();

  // Debug: check free size after WiFi/HTTP init
  extern size_t lua_psram_alloc_free_size(void);
  printf("[PSRAM] Free after WiFi/HTTP init: %zu bytes (%zuK)\n",
         lua_psram_alloc_free_size(), lua_psram_alloc_free_size() / 1024);

  image_preload_init();

  // Launch Core 1 background tasks
  multicore_launch_core1(core1_entry);
  watchdog_update();

  system_menu_init();

  ui_draw_splash("Loading...", NULL);
  sleep_ms(300); // Brief pause so the splash is visible

  // Boot completed successfully — clear the boot attempt counter so a
  // future watchdog reset starts fresh.
  watchdog_hw->scratch[0] = 0;
  // The loop guard only covers boot: an app hang must still reset.
  if (skip_boot_watchdog)
    watchdog_enable(10000, true);

  // Hand off to the launcher — this never returns
  launcher_run();

  // Only a failed launcher allocation gets here. Returning from main() would
  // run newlib's exit() -> _exit(), a breakpoint loop that HardFaults with
  // no debugger attached: record why and reboot instead.
  printf("[MAIN] launcher_run returned, rebooting\n");
  crashlog_write("OS ERROR", "OS", "launcher", "launcher_run returned");
  stdio_flush();
  kbd_prepare_reset();
  watchdog_reboot(0, 0, 0);
  while (true)
    tight_loop_contents();
}
