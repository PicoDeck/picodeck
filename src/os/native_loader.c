#include "native_loader.h"
#include "launcher_types.h"
#include "app_identity.h"
#include "app_abi.h"
#include "../drivers/audio.h"
#include "../drivers/display.h"
#include "../drivers/http.h"
#include "../drivers/keyboard.h"
#include "../drivers/sdcard.h"
#include "../drivers/tcp.h"
#include "../drivers/sound.h"
#include "../drivers/fileplayer.h"
#include "../drivers/mp3_player.h"
#include "../drivers/image_api.h"
#include "../drivers/video_player.h"
#include "../drivers/mod_player.h"
#include "../dev_commands.h"
#include "../os/os.h"
#include "terminal.h"
#ifndef PICODECK_SIMULATOR
#include "crypto.h"
#endif

#include "umm_malloc.h"
#include "crashlog.h"
#include "app_stack.h"
#include "elf_plan.h"
#include "lua_psram_alloc.h"
#include "pico/stdlib.h"
#include "hardware/watchdog.h"
#include "hardware/xip_cache.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// RP2350 XIP address ranges for PSRAM CS1.
// umm_malloc returns addresses in the cached alias (0x11xxxxxx).
// Code is WRITTEN through the uncached alias (0x15xxxxxx) to guarantee
// writes reach physical PSRAM (bypassing the write-back cache).  After
// writing, the XIP cache is invalidated and execution uses the CACHED
// alias (0x11xxxxxx) so the 16KB XIP cache serves most instruction
// fetches — dramatically reducing QMI bus traffic and eliminating the
// random IBUSERR/PRECISERR faults seen with uncached execution.
#define PSRAM_CS1_CACHED_BASE    0x11000000u
#define PSRAM_CS1_CACHED_END     0x12000000u
#define PSRAM_CACHED_TO_UNCACHED 0x04000000u  // add to get uncached alias

// Maximum virtual address range accepted for a native app image.
// Rejects malformed or malicious ELFs before attempting a heap allocation.
// 7 MB covers apps like TIC-80 (~5.4 MB with 4 MB sbrk heap in BSS).
// The real limit is available PSRAM (6 MB on Pimoroni Pico Plus 2 W).
#define NATIVE_MAX_IMAGE_SIZE (7u * 1024u * 1024u)

// =============================================================================
// ELF validation and relocation live in elf_plan.c (pure, host-tested and
// fuzzed under tests/unit and tests/fuzz); this file does the I/O, placement
// and cache maintenance around them.
// =============================================================================

// =============================================================================
// Helpers
// =============================================================================

#define C_BG COLOR_BLACK

extern PicoCalcAPI g_api;

// Name of the app currently being loaded, for error records.
static const char *s_loading_app_name = NULL;

// Loader failure: show it with the heap state, log it to /system/error.log,
// and pause so it can be read before the launcher repaints.
static void show_error(const char *line1, const char *line2) {
  char heap[96];
  crashlog_describe_heap(heap, sizeof(heap));
  crashlog_write("NATIVE ERROR", s_loading_app_name, line1, line2);

  display_clear(C_BG);
  display_draw_text(8, 8, line1, COLOR_RED, C_BG);
  if (line2)
    display_draw_text(8, 20, line2, COLOR_WHITE, C_BG);
  display_draw_text(8, 36, heap, COLOR_GRAY, C_BG);
  display_flush();
  for (int i = 0; i < 30; i++) {
    watchdog_update();
    sleep_ms(100);
  }
}

// =============================================================================
// App stack (PSP-based isolation)
// =============================================================================

// Native apps run on the PSP (Process Stack Pointer) through app_stack_run()
// (app_stack.c, shared with the Lua runner).  Interrupt handlers always use
// the MSP (Main Stack Pointer) regardless of SPSEL, so the two stacks are
// completely independent: app stack pressure and interrupt stacking do not
// interfere with each other.
//
// 64 KB is allocated from PSRAM (via umm_malloc) at launch time, giving
// plenty of headroom for deep recursion (e.g. Doom's BSP tree traversal).
// Stack accesses go through the cached XIP alias for performance.
#define NATIVE_STACK_SIZE (64 * 1024)
// Preferred SRAM stack size — used when the SRAM heap can supply it (see the
// allocation site).  Double the 8 KB static SRAM stack all native apps
// originally ran on (Doom included), so it is not a regression for depth.
#define NATIVE_STACK_SRAM_SIZE (16 * 1024)

// Where the running native app's image landed, read by the HardFault handler
// (main.c) to report crash PC/LR as ELF-relative offsets so they can be
// symbolicated against the app's .elf.  In split mode the code segment lives
// in SRAM apart from the PSRAM data segment, hence two ranges.  All zero when
// no native app is running.
uintptr_t g_native_code_base = 0, g_native_code_limit = 0;
uint32_t  g_native_code_vaddr = 0;
uintptr_t g_native_data_base = 0, g_native_data_limit = 0;
uint32_t  g_native_data_vaddr = 0;

// DIAG: code-corruption watcher.  A snapshot of the app's read-only image
// (.text + .rodata) taken right after load; Core 1 scans it against the live
// image in rotating chunks and reports + repairs any divergence (onset time
// + data pattern identify whoever is trampling app code in PSRAM).  Both
// pointers are UNCACHED-alias addresses so the scan neither pollutes the
// XIP cache nor misses direct-to-PSRAM writes.  Active only while a native
// app is running and the snapshot allocation succeeded.
// Cap: Doom's .rodata ends at vaddr 0x58C84; everything below 0x58C00 is
// read-only at runtime (diagnostic constant — adjust per app if reused).
#define CODE_WATCH_MAX_SIZE (0x58C00u)
const uint8_t     *g_code_watch_snap = NULL;   // uncached alias
const uint8_t     *g_code_watch_live = NULL;   // uncached alias
uint32_t           g_code_watch_size = 0;
_Atomic(bool)      g_code_watch_active = false;

// Trampoline for app_stack_run(): unpacks the native entry point's four
// arguments (app_stack_run passes a single pointer).
typedef struct {
  picodeck_app_entry_t fn;
  const PicoCalcAPI *api;
  const char *app_dir, *app_id, *app_name;
} native_launch_t;

static void __attribute__((unused)) native_launch_thunk(void *p) {
  const native_launch_t *l = (const native_launch_t *)p;
  l->fn(l->api, l->app_dir, l->app_id, l->app_name);
}

// =============================================================================
// Per-launch resource tracking (native_loader.h)
// =============================================================================

void native_res_release(int kind, void *h) {
  switch (kind) {
  case NATIVE_RES_IMAGE:      image_free((pc_image_t *)h); break;
  case NATIVE_RES_SAMPLE:     sound_sample_destroy((sound_sample_t *)h); break;
  case NATIVE_RES_PLAYER:     sound_player_destroy((sound_player_t *)h); break;
  case NATIVE_RES_FILEPLAYER: fileplayer_destroy((fileplayer_t *)h); break;
  case NATIVE_RES_MP3:        mp3_player_destroy((mp3_player_t *)h); break;
  case NATIVE_RES_VIDEO:      video_player_destroy((video_player_t *)h); break;
  case NATIVE_RES_MOD:        mod_player_destroy((mod_player_t *)h); break;
  case NATIVE_RES_TERMINAL:   terminal_free((terminal_t *)h); break;
#ifndef PICODECK_SIMULATOR
  // The simulator has no crypto, and its qmiAlloc is the emulator's own
  // heap (gone with the Unicorn instance): neither is ever tracked there.
  case NATIVE_RES_QMI:        umm_free(h); break;
  case NATIVE_RES_AES:        crypto_aes_free((crypto_aes_t *)h); break;
  case NATIVE_RES_ECDH:       crypto_ecdh_free((crypto_ecdh_t *)h); break;
#endif
  default:
    printf("[NATIVE] BUG: release of unknown handle kind %d\n", kind);
    break;
  }
}

void *native_res_adopt(int kind, void *h) {
  if (h && !app_res_track(kind, h)) {
    printf("[NATIVE] out of PSRAM tracking handle (kind %d): refused\n", kind);
    native_res_release(kind, h);
    return NULL;
  }
  return h;
}

void native_res_drop(int kind, void *h) {
  if (app_res_untrack(kind, h))
    native_res_release(kind, h);
}

void *native_file_adopt(void *f) {
  if (f && !app_files_track(f)) {
    sdcard_fclose((sdfile_t)f);
    return NULL;
  }
  return f;
}

void native_file_drop(void *f) {
  if (app_files_untrack(f))
    sdcard_fclose((sdfile_t)f);
}

void native_res_release_all(const char *app_name) {
  int n = app_res_release_all(native_res_release);
  int files = app_files_close_all();
  if (n || files)
    printf("[NATIVE] '%s' exited holding %d handle(s) and %d file(s): "
           "released\n", app_name, n, files);
}

// Everything the app may have left behind that the OS can reclaim without
// Core 1 paused: its handles, then the audio engines' per-app state (as the
// Lua runner does after lua_close).  The pool-wide HTTP/TCP close and the
// tone/stream stop are the caller's (their order differs per build).
static void native_teardown(const app_entry_t *app) {
  native_res_release_all(app->name);
  fileplayer_reset();
  sound_init();
  mp3_player_reset();
}

// =============================================================================
// ELF loader
// =============================================================================

// Declared in main.c — pauses Core 1's Mongoose/WiFi polling loop.
extern _Atomic bool g_core1_pause;
extern _Atomic bool g_core1_paused;

#ifdef PICODECK_SIMULATOR
// Simulator: use Unicorn Engine to emulate the ARM ELF binary
#include "unicorn_runner.h"

static bool native_run_app(const app_entry_t *app) {
  printf("[NATIVE] Loading '%s' via Unicorn Engine\n", app->name);
  char elf_path[256];
  snprintf(elf_path, sizeof(elf_path), "%s/main.elf", app->path);
  bool ok = unicorn_run_app(elf_path, app->path, app->id, app->name);
  // Same teardown as firmware (below), minus the Core 1 pause.
  atomic_store(&g_native_audio_callback, NULL);
  native_teardown(app);
  http_close_all(NULL);
  tcp_close_all();
  audio_stop_stream();
  audio_stop_tone();
  audio_set_volume(100);  // the app's master volume must not outlive it
  audio_output_stop();
  return ok;
}
#else
// Declared in main.c — feeds the watchdog and stamps Core 0's heartbeat.
extern void core0_heartbeat(void);

// ── The app's API: g_api with tracking create/free entries ──────────────────
// A per-launch copy (PSRAM, ~1 KB) of g_api and of the sub-tables that hand
// out objects; every other entry and table is g_api's own, so the layout and
// all other behaviour are identical.  Creators go through g_api and adopt the
// result; frees untrack first (native_res_drop), so a double free or a stray
// pointer from the app is ignored instead of corrupting the heap.
typedef struct {
  PicoCalcAPI api;
  picocalc_fs_t fs;
  picocalc_psram_t psram;
  picocalc_graphics_t graphics;
  picocalc_soundplayer_t soundplayer;
  picocalc_video_t video;
  picocalc_modplayer_t modplayer;
  picocalc_terminal_t terminal;
  picocalc_crypto_t crypto;
} native_api_t;

static pcfile_t napi_fs_open(const char *path, const char *mode) {
  return native_file_adopt(g_api.fs->open(path, mode));
}
static void napi_fs_close(pcfile_t f) { native_file_drop(f); }

static void *napi_qmi_alloc(uint32_t size) {
  return native_res_adopt(NATIVE_RES_QMI, g_api.psram->qmiAlloc(size));
}
static void napi_qmi_free(void *p) { native_res_drop(NATIVE_RES_QMI, p); }

static pcimage_t napi_gfx_load(const char *path) {
  return native_res_adopt(NATIVE_RES_IMAGE, g_api.graphics->load(path));
}
static pcimage_t napi_gfx_new_blank(int w, int h) {
  return native_res_adopt(NATIVE_RES_IMAGE, g_api.graphics->newBlank(w, h));
}
static void napi_gfx_free(pcimage_t img) { native_res_drop(NATIVE_RES_IMAGE, img); }

static pcsound_sample_t napi_sample_load(const char *path) {
  return native_res_adopt(NATIVE_RES_SAMPLE, g_api.soundplayer->sampleLoad(path));
}
static void napi_sample_free(pcsound_sample_t s) {
  native_res_drop(NATIVE_RES_SAMPLE, s);
}
static pcsound_player_t napi_player_new(void) {
  return native_res_adopt(NATIVE_RES_PLAYER, g_api.soundplayer->playerNew());
}
static void napi_player_free(pcsound_player_t p) {
  native_res_drop(NATIVE_RES_PLAYER, p);
}
static pcfileplayer_t napi_fileplayer_new(void) {
  return native_res_adopt(NATIVE_RES_FILEPLAYER,
                          g_api.soundplayer->filePlayerNew());
}
static void napi_fileplayer_free(pcfileplayer_t fp) {
  native_res_drop(NATIVE_RES_FILEPLAYER, fp);
}
static pcmp3player_t napi_mp3_new(void) {
  return native_res_adopt(NATIVE_RES_MP3, g_api.soundplayer->mp3PlayerNew());
}
static void napi_mp3_free(pcmp3player_t mp) { native_res_drop(NATIVE_RES_MP3, mp); }

static pcvideo_t napi_video_new(void) {
  return native_res_adopt(NATIVE_RES_VIDEO, g_api.video->newPlayer());
}
static void napi_video_free(pcvideo_t vp) { native_res_drop(NATIVE_RES_VIDEO, vp); }

static pcmodplayer_t napi_mod_create(void) {
  return native_res_adopt(NATIVE_RES_MOD, g_api.modplayer->create());
}
static void napi_mod_destroy(pcmodplayer_t mp) { native_res_drop(NATIVE_RES_MOD, mp); }

static terminal_t *napi_term_create(int cols, int rows, int scrollback) {
  return (terminal_t *)native_res_adopt(
      NATIVE_RES_TERMINAL, g_api.terminal->create(cols, rows, scrollback));
}
static void napi_term_free(terminal_t *t) { native_res_drop(NATIVE_RES_TERMINAL, t); }

static pccrypto_aes_t napi_aes_new(const uint8_t *key, uint32_t klen,
                                   const uint8_t *nonce) {
  return native_res_adopt(NATIVE_RES_AES, g_api.crypto->aesNew(key, klen, nonce));
}
static void napi_aes_free(pccrypto_aes_t ctx) { native_res_drop(NATIVE_RES_AES, ctx); }
static pccrypto_ecdh_t napi_ecdh_x25519(void) {
  return native_res_adopt(NATIVE_RES_ECDH, g_api.crypto->ecdhX25519());
}
static pccrypto_ecdh_t napi_ecdh_p256(void) {
  return native_res_adopt(NATIVE_RES_ECDH, g_api.crypto->ecdhP256());
}
static void napi_ecdh_free(pccrypto_ecdh_t ctx) { native_res_drop(NATIVE_RES_ECDH, ctx); }

// NULL when PSRAM has no ~1 KB block.  Free with umm_free after the app's
// handles have been released.
static native_api_t *native_api_new(void) {
  native_api_t *n = (native_api_t *)umm_malloc(sizeof(*n));
  if (!n)
    return NULL;
  n->api = g_api;
  n->fs = *g_api.fs;
  n->fs.open = napi_fs_open;
  n->fs.close = napi_fs_close;
  n->api.fs = &n->fs;
  n->psram = *g_api.psram;
  n->psram.qmiAlloc = napi_qmi_alloc;
  n->psram.qmiFree = napi_qmi_free;
  n->api.psram = &n->psram;
  n->graphics = *g_api.graphics;
  n->graphics.load = napi_gfx_load;
  n->graphics.newBlank = napi_gfx_new_blank;
  n->graphics.free = napi_gfx_free;
  n->api.graphics = &n->graphics;
  n->soundplayer = *g_api.soundplayer;
  n->soundplayer.sampleLoad = napi_sample_load;
  n->soundplayer.sampleFree = napi_sample_free;
  n->soundplayer.playerNew = napi_player_new;
  n->soundplayer.playerFree = napi_player_free;
  n->soundplayer.filePlayerNew = napi_fileplayer_new;
  n->soundplayer.filePlayerFree = napi_fileplayer_free;
  n->soundplayer.mp3PlayerNew = napi_mp3_new;
  n->soundplayer.mp3PlayerFree = napi_mp3_free;
  n->api.soundplayer = &n->soundplayer;
  n->video = *g_api.video;
  n->video.newPlayer = napi_video_new;
  n->video.free = napi_video_free;
  n->api.video = &n->video;
  n->modplayer = *g_api.modplayer;
  n->modplayer.create = napi_mod_create;
  n->modplayer.destroy = napi_mod_destroy;
  n->api.modplayer = &n->modplayer;
  n->terminal = *g_api.terminal;
  n->terminal.create = napi_term_create;
  n->terminal.free = napi_term_free;
  n->api.terminal = &n->terminal;
  n->crypto = *g_api.crypto;
  n->crypto.aesNew = napi_aes_new;
  n->crypto.aesFree = napi_aes_free;
  n->crypto.ecdhX25519 = napi_ecdh_x25519;
  n->crypto.ecdhP256 = napi_ecdh_p256;
  n->crypto.ecdhFree = napi_ecdh_free;
  n->api.crypto = &n->crypto;
  return n;
}

static bool native_run_app(const app_entry_t *app) {
  printf("[NATIVE] Loading '%s'\n", app->name);
  s_loading_app_name = app->name;

  // Pause Core 1 to eliminate PSRAM heap contention during ELF loading.
  // Core 1 runs umm_malloc/umm_free every 5ms for Mongoose; those allocations
  // share the same PSRAM heap where the app image is loaded.  Without this
  // pause, heap metadata corruption can cause Core 1 to overwrite app code.
  g_core1_pause = true;
  // Wait for Core 1 to acknowledge the pause (explicit handshake).
  // The old sleep_ms(10) was a race: Core 1 could be mid-umm_malloc()
  // during DNS resolution when the flag is set.
  for (int i = 0; i < 200 && !g_core1_paused; i++)
    sleep_ms(1);
  if (!g_core1_paused)
    printf("[NATIVE] Core 1 pause timeout (200ms) — proceeding anyway\n");

  // ── 1. Open ELF from SD card ──────────────────────────────────────────────
  char elf_path[160];
  snprintf(elf_path, sizeof(elf_path), "%s/main.elf", app->path);

  bool ok = false;
  uint8_t *load_base = NULL;
  uint8_t *code_buf = NULL;
  uint8_t *stack_buf = NULL;
  bool stack_in_sram = false;
  sdfile_t f = NULL;
  uint8_t *phdr_table = NULL;
  native_api_t *napi = NULL;

  f = sdcard_fopen(elf_path, "rb");
  if (!f) {
    show_error("Failed to open native app:", elf_path);
    goto out;
  }

  int file_len = sdcard_fsize_handle(f);
  printf("[NATIVE] ELF: %d bytes (streaming)\n", file_len);

  // ── 2. Validate ELF header ────────────────────────────────────────────────
  uint8_t ehdr_buf[ELF_EHDR_SIZE];
  if (file_len < (int)sizeof(ehdr_buf)) {
    show_error("ELF: file too small", NULL);
    goto out;
  }
  if (sdcard_fread(f, ehdr_buf, sizeof(ehdr_buf)) != (int)sizeof(ehdr_buf)) {
    show_error("ELF: failed to read header", NULL);
    goto out;
  }

  elf_plan_t plan;
  elf_err_t elf_err = elf_plan_header(ehdr_buf, sizeof(ehdr_buf),
                                      (uint32_t)file_len, &plan);
  if (elf_err != ELF_OK) {
    show_error(elf_strerror(elf_err), NULL);
    goto out;
  }

  // ── 3. Validate program headers, measure the PT_LOAD range ────────────────
  phdr_table = (uint8_t *)umm_malloc(plan.phdrs_size);
  if (!phdr_table) {
    show_error("ELF: out of memory for phdr", NULL);
    goto out;
  }

  if (!sdcard_fseek(f, plan.phoff) ||
      sdcard_fread(f, phdr_table, plan.phdrs_size) != (int)plan.phdrs_size) {
    show_error("ELF: failed to read phdr table", NULL);
    goto out;
  }

  // Proves every PT_LOAD's file range and memory range (p_filesz <= p_memsz,
  // no wrap), the entry point and PT_DYNAMIC lie inside the file / image.
  elf_err = elf_plan_segments(&plan, phdr_table, plan.phdrs_size,
                              (uint32_t)file_len, NATIVE_MAX_IMAGE_SIZE);
  if (elf_err != ELF_OK) {
    show_error(elf_strerror(elf_err), elf_err == ELF_ERR_IMAGE_TOO_LARGE
                                          ? "limit is 7MB" : NULL);
    goto out;
  }

  const uint32_t mem_min = plan.mem_min;
  const uint32_t image_size = plan.image_size;
  // Code segment for split loading (PT_LOAD with PF_X).
  const int code_seg_idx = plan.code_idx;
  const uint32_t code_vaddr = plan.code_vaddr;
  const uint32_t code_memsz = plan.code_memsz;
  const uint32_t code_vend = code_vaddr + code_memsz;

  printf("[NATIVE] Image: %lu bytes (vaddr 0x%08lx..0x%08lx)\n",
         (unsigned long)image_size,
         (unsigned long)mem_min, (unsigned long)plan.mem_max);

  // ── 4. Split allocation: code in SRAM, data/BSS in PSRAM ────────────────
  bool split_mode = false;

  #define MAX_SRAM_CODE_SIZE  (16u * 1024)
  // Split mode assumes the code segment is the lowest PT_LOAD and all other
  // segments sit above it (data offsets are taken from its end).
  if (code_seg_idx >= 0 && plan.code_first && code_memsz > 0 &&
      code_memsz <= MAX_SRAM_CODE_SIZE) {
    code_buf = malloc(code_memsz);
    if (code_buf) {
      split_mode = true;
      printf("[NATIVE] Split mode: code %lu bytes in SRAM @ %p\n",
             (unsigned long)code_memsz, (void *)code_buf);
    } else {
      printf("[NATIVE] SRAM alloc failed for code (%lu bytes), using all-PSRAM\n",
             (unsigned long)code_memsz);
    }
  }

  // PSRAM allocation: in split mode, only data/BSS; otherwise entire image
  uint32_t psram_size = split_mode ? (image_size - code_memsz) : image_size;
  uint32_t data_vaddr_start = split_mode ? code_vend : mem_min;

  if (psram_size > 0) {
    load_base = (uint8_t *)umm_malloc(psram_size);
    if (!load_base) {
      if (split_mode) { free(code_buf); code_buf = NULL; }
      char detail[80];
      snprintf(detail, sizeof(detail), "image needs %luK, largest free block %luK",
               (unsigned long)(psram_size / 1024u),
               (unsigned long)(lua_psram_alloc_largest_block() / 1024u));
      show_error("ELF: out of PSRAM", detail);
      goto out;
    }
  }

  // ── 4a'. Flush dirty cache lines from umm_malloc ─────────────────────────
  #ifndef PICODECK_SIMULATOR
  __asm volatile ("dsb sy");
  #endif
  xip_cache_clean_all();
  #ifndef PICODECK_SIMULATOR
  __asm volatile ("isb sy");
  #endif

  // ── 4b. Compute uncached alias for PSRAM writes ───────────────────────────
  uint8_t *exec_base = load_base;
  if (load_base &&
      (uintptr_t)load_base >= PSRAM_CS1_CACHED_BASE &&
      (uintptr_t)load_base <  PSRAM_CS1_CACHED_END) {
    exec_base = load_base + PSRAM_CACHED_TO_UNCACHED;
  }

  // ── 5. Zero and copy PT_LOAD segments ─────────────────────────────────────
  if (split_mode) {
    memset(code_buf, 0, code_memsz);
    if (psram_size > 0)
      memset(exec_base, 0, psram_size);
  } else {
    memset(exec_base, 0, image_size);
  }

  for (uint16_t i = 0; i < plan.phnum; i++) {
    const elf32_phdr_t seg = elf_plan_phdr(phdr_table, i);
    const elf32_phdr_t *ph = &seg;
    if (ph->p_type != ELF_PT_LOAD || ph->p_filesz == 0)
      continue;
    // Heartbeat per segment: a large segment (e.g. DOOM) takes seconds to
    // read, and Core 1 (paused) relays the watchdog only while it is fresh.
    core0_heartbeat();
    if (ph->p_offset + ph->p_filesz > (uint32_t)file_len) {
      show_error("ELF: segment data out of bounds", NULL);
      goto out;
    }

    if (!sdcard_fseek(f, ph->p_offset)) {
      show_error("ELF: failed to seek to segment", NULL);
      goto out;
    }

    if (split_mode && i == code_seg_idx) {
      if (ph->p_vaddr - code_vaddr + ph->p_filesz > code_memsz) {
        show_error("ELF: code segment exceeds buffer", NULL);
        goto out;
      }
      if (sdcard_fread(f, code_buf + (ph->p_vaddr - code_vaddr), ph->p_filesz) != (int)ph->p_filesz) {
        show_error("ELF: failed to read code segment", NULL);
        goto out;
      }
    } else if (split_mode) {
      uint32_t off = ph->p_vaddr - data_vaddr_start;
      if (off + ph->p_memsz > psram_size) {
        show_error("ELF: data segment exceeds buffer", NULL);
        goto out;
      }
      if (sdcard_fread(f, exec_base + off, ph->p_filesz) != (int)ph->p_filesz) {
        show_error("ELF: failed to read data segment", NULL);
        goto out;
      }
    } else {
      if (ph->p_vaddr - mem_min + ph->p_memsz > image_size) {
        show_error("ELF: segment exceeds image", NULL);
        goto out;
      }
      if (sdcard_fread(f, exec_base + (ph->p_vaddr - mem_min), ph->p_filesz) != (int)ph->p_filesz) {
        show_error("ELF: failed to read segment", NULL);
        goto out;
      }
    }
  }

  // ── 6. Apply relocations (dual-bias for split mode) ───────────────────────
  {
    // Guard against underflow from malformed ELF segment layout
    if (!split_mode && mem_min > (uint32_t)(uintptr_t)load_base) {
      show_error("ELF: invalid segment layout", NULL);
      goto out;
    }
    // (An image that is all code has no PSRAM part: load_base stays NULL and
    // there is no data region to check.  This guard used to fire for it, so
    // every native app of 16 KB or less - hello_c, the SDK template - failed
    // with "invalid segment layout" on hardware.)
    if (split_mode && psram_size > 0 &&
        data_vaddr_start > (uint32_t)(uintptr_t)load_base) {
      show_error("ELF: invalid segment layout", NULL);
      goto out;
    }

    // Writes go through exec_base (the uncached alias); relocated pointers
    // use the address the region runs at.  A pointer outside both split
    // regions takes the data bias (the last region), as before; an all-code
    // image has only the code region.
    elf_region_t regions[2];
    int nregions = 0;
    if (split_mode) {
      regions[nregions++] = (elf_region_t){code_vaddr, code_memsz, code_buf,
                                           (uint32_t)(uintptr_t)code_buf};
      if (psram_size > 0)
        regions[nregions++] = (elf_region_t){data_vaddr_start, psram_size,
                                             exec_base,
                                             (uint32_t)(uintptr_t)load_base};
    } else {
      regions[nregions++] = (elf_region_t){mem_min, image_size, exec_base,
                                           (uint32_t)(uintptr_t)load_base};
    }
    elf_reloc_stats_t rstats;
    elf_err = elf_relocate(&plan, regions, nregions, &rstats);
    if (elf_err != ELF_OK) {
      show_error(elf_strerror(elf_err), NULL);
      goto out;
    }
    printf("[NATIVE] Relocations: %lu applied, %lu symbolic left as-is\n",
           (unsigned long)rstats.applied, (unsigned long)rstats.symbolic);
  }

  // ── 7. Invalidate XIP cache for the app image, compute entry point ──────
  // Scoped to the image range only — NOT invalidate_all.  cyw43 RX IRQs keep
  // enqueuing frames into the Mongoose recv_queue (core1 PSRAM pool) even
  // while both cores' thread loops are paused, so a whole-cache invalidate
  // discards those dirty lines mid-load: the queue head (SRAM) keeps its
  // advance while the queued bytes are lost, and Core 1 hard-faults on a
  // garbage frame length in mg_queue_next after resume.  The image itself was
  // written via the uncached alias, so its lines are at worst stale-clean and
  // a range invalidate is sufficient for fresh instruction/data fetches.
  if (load_base &&
      (uintptr_t)load_base >= PSRAM_CS1_CACHED_BASE &&
      (uintptr_t)load_base <  PSRAM_CS1_CACHED_END) {
    size_t inv_size = split_mode ? psram_size : image_size;
    if (inv_size > 0) {
      uintptr_t start = (uintptr_t)load_base - XIP_BASE;
      uintptr_t end = start + inv_size;
      // Maintenance ops encode the operation in the low address bits, so the
      // range must be cache-line aligned.
      start &= ~(uintptr_t)(XIP_CACHE_LINE_SIZE - 1);
      end = (end + XIP_CACHE_LINE_SIZE - 1) & ~(uintptr_t)(XIP_CACHE_LINE_SIZE - 1);
      #ifndef PICODECK_SIMULATOR
      __asm volatile ("dsb sy");
      #endif
      xip_cache_invalidate_range(start, end - start);
      #ifndef PICODECK_SIMULATOR
      __asm volatile ("isb sy");
      #endif
    }
  }

  uintptr_t entry_voff_raw = plan.entry & ~1u;
  uintptr_t entry_addr;
  if (split_mode && entry_voff_raw >= code_vaddr && entry_voff_raw < code_vend) {
    entry_addr = (uintptr_t)code_buf + (entry_voff_raw - code_vaddr);
  } else if (split_mode) {
    entry_addr = (uintptr_t)load_base + (entry_voff_raw - data_vaddr_start);
  } else {
    uintptr_t entry_voff = entry_voff_raw - mem_min;
    if (entry_voff >= image_size) {
      show_error("ELF: entry point out of bounds", NULL);
      goto out;
    }
    entry_addr = (uintptr_t)load_base + entry_voff;
  }
  entry_addr |= 1u; // Thumb mode

  printf("[NATIVE] Entry %p (thumb%s)\n", (void *)entry_addr,
         split_mode ? ", SRAM" : ", cached PSRAM");

  // Record image placement so the HardFault handler can report crash PC/LR
  // as ELF-relative offsets (symbolicate with: arm-none-eabi-addr2line -e
  // <unstripped>.elf <offset>).
  if (split_mode) {
    g_native_code_base  = (uintptr_t)code_buf;
    g_native_code_limit = (uintptr_t)code_buf + code_memsz;
    g_native_code_vaddr = code_vaddr;
    g_native_data_base  = (uintptr_t)load_base;
    g_native_data_limit = (uintptr_t)load_base + psram_size;
    g_native_data_vaddr = data_vaddr_start;
  } else {
    g_native_code_base  = (uintptr_t)load_base;
    g_native_code_limit = (uintptr_t)load_base + image_size;
    g_native_code_vaddr = mem_min;
    g_native_data_base  = 0;
    g_native_data_limit = 0;
    g_native_data_vaddr = 0;
  }
  printf("[NATIVE] Image base %p = ELF vaddr 0x%08lx\n",
         (void *)g_native_code_base, (unsigned long)g_native_code_vaddr);

  // DIAG: snapshot region for the Core 1 corruption watcher.
  //
  // PERMANENTLY DISARMED (2026-07-19).  The watcher's fixed window
  // (CODE_WATCH_MAX_SIZE, sized for Doom's read-only layout) covered the GBC
  // emulator's writable .bss, and its auto-repair path memcpy'd stale snapshot
  // bytes over the app's live data and invalidated the XIP cache mid-write —
  // corrupting SD reads into PSRAM buffers and tearing pointer loads (wild
  // jumps at fs->close, in-game crashes).  If image watching is ever needed
  // again: derive the watched range from the app's own read-only PT_LOAD
  // flags, and REPORT ONLY — never write to or cache-invalidate memory the
  // running app owns.
  if (0 && !split_mode &&
      (uintptr_t)load_base >= PSRAM_CS1_CACHED_BASE &&
      (uintptr_t)load_base <  PSRAM_CS1_CACHED_END) {
    uint32_t wsize = (uint32_t)(g_native_code_limit - g_native_code_base);
    if (wsize > CODE_WATCH_MAX_SIZE) wsize = CODE_WATCH_MAX_SIZE;
    uint8_t *snap = (uint8_t *)umm_malloc(wsize);
    if (snap) {
      const uint8_t *live_u =
          (const uint8_t *)(g_native_code_base + PSRAM_CACHED_TO_UNCACHED);
      uint8_t *snap_u = snap + PSRAM_CACHED_TO_UNCACHED;
      memcpy(snap_u, live_u, wsize);
      // Verify the copy against the CACHED view: the uncached alias has been
      // seen to misread the image's first word persistently right after
      // load (returns zeros), while the cached view is correct.  One-time
      // cache thrash here is fine — the app hasn't started yet.
      {
        const uint8_t *live_c = (const uint8_t *)g_native_code_base;
        for (int pass = 0;
             pass < 3 && memcmp(snap_u, live_c, wsize) != 0; pass++) {
          for (uint32_t i = 0; i < wsize; i++)
            if (snap_u[i] != live_c[i]) snap_u[i] = live_c[i];
        }
      }
      g_code_watch_snap = snap_u;
      g_code_watch_live = live_u;
      g_code_watch_size = wsize;
      __dmb();
      atomic_store(&g_code_watch_active, true);
      printf("[CODEWATCH] armed: %lu bytes @ %p (uncached)\n",
             (unsigned long)wsize, (const void *)live_u);
    }
  }

  kbd_clear_state();

  // ── 8. Launch app ─────────────────────────────────────────────────────────
  display_clear(C_BG);
  display_flush();

  picodeck_app_entry_t entry_fn = (picodeck_app_entry_t)entry_addr;

  // Prefer an SRAM stack: PSRAM stacks funnel every call frame, local array
  // and register spill through the XIP cache, which measurably slows
  // memory-bound apps (GBC emulator).  16 KB SRAM is double the original
  // static stack all native apps ran on; fall back to the roomy 64 KB PSRAM
  // stack when SRAM is unavailable.
  uint32_t stack_size = 0;
  for (uint32_t try_size = NATIVE_STACK_SRAM_SIZE; try_size >= 8u * 1024;
       try_size -= 4u * 1024) {
    stack_buf = (uint8_t *)malloc(try_size);
    if (stack_buf) {
      stack_size = try_size;
      stack_in_sram = true;
      break;
    }
  }
  if (!stack_in_sram) {
    stack_size = NATIVE_STACK_SIZE;
    stack_buf = (uint8_t *)umm_malloc(NATIVE_STACK_SIZE);
  }
  if (!stack_buf) {
    char detail[64];
    snprintf(detail, sizeof(detail), "no %luK SRAM and no %luK PSRAM block",
             (unsigned long)(8u), (unsigned long)(NATIVE_STACK_SIZE / 1024u));
    show_error("Out of memory for app stack", detail);
    goto out;
  }
  napi = native_api_new();
  if (!napi) {
    show_error("Out of memory for the app's API table", NULL);
    goto out;
  }

  printf("[NATIVE] App stack: %lu KB in %s @ %p\n",
         (unsigned long)(stack_size / 1024), stack_in_sram ? "SRAM" : "PSRAM",
         (void *)stack_buf);
  g_core1_pause = false;

  // app_stack_run paints the stack, arms PSPLIM just above its 32-byte guard
  // (an app push past it faults with CFSR.STKOF, recorded as a stack
  // overflow, instead of silently running into whatever lies below) and
  // runs the entry point on the PSP.
  native_launch_t launch = {entry_fn, &napi->api, app->path, app->id,
                            app->name};
  app_stack_run(stack_buf, stack_size, APP_STACK_NATIVE, native_launch_thunk,
                &launch);

  ok = true;
  printf("[NATIVE] Stack high-water: %lu of %lu bytes\n",
         (unsigned long)app_stack_high_water(stack_buf, stack_size),
         (unsigned long)stack_size);
  if (!app_stack_guard_intact(stack_buf)) {
    printf("[NATIVE] ERROR: stack overflow detected in '%s' (guard words "
           "overwritten)\n", app->name);
    char detail[96];
    snprintf(detail, sizeof(detail),
             "stack guard overwritten after return (%luK %s stack)",
             (unsigned long)(stack_size / 1024u),
             stack_in_sram ? "SRAM" : "PSRAM");
    crashlog_write("NATIVE ERROR", app->name,
                   "stack overflow detected after app returned", detail);
    ok = false;
  }

  printf("[NATIVE] App '%s' returned%s\n", app->name,
         ok ? "" : " (with stack overflow)");

out:
  // ── 9. Cleanup ─────────────────────────────────────────────────────────────
  __dmb(); // ensure all app writes visible before clearing callback
  atomic_store(&g_native_audio_callback, NULL);
  atomic_store(&g_code_watch_active, false);
  g_native_code_base = g_native_code_limit = 0;
  g_native_data_base = g_native_data_limit = 0;
  g_native_code_vaddr = g_native_data_vaddr = 0;
  // Free what the app left open (its own cleanup, if any, has run: it
  // returned), then the audio engines' per-app state.  Core 1 still runs:
  // the players' locks are what make this safe against its updates.
  native_teardown(app);
  // Release the app's HTTP/TCP connections while Core 1 still runs: it has
  // to acknowledge each close before the slot can be reclaimed.
  http_close_all(NULL);
  tcp_close_all();
  g_core1_pause = true;
  for (int i = 0; i < 200 && !g_core1_paused; i++)
    sleep_ms(1);
  if (!g_core1_paused)
    printf("[NATIVE] Core 1 pause timeout (200ms) at cleanup\n");
  audio_stop_stream();
  audio_stop_tone();
  // The master volume is the app's too: an app that turned it down must not
  // leave every later app (and its MP3) quiet until a reboot.
  audio_set_volume(100);
  audio_output_stop();
  // Core 1 is paused (or timed out) — safe to drop the watcher snapshot now.
  // g_code_watch_snap holds the uncached alias; umm_free wants the original.
  if (g_code_watch_snap) {
    umm_free((void *)(g_code_watch_snap - PSRAM_CACHED_TO_UNCACHED));
    g_code_watch_snap = NULL;
    g_code_watch_live = NULL;
    g_code_watch_size = 0;
  }
  if (code_buf)
    free(code_buf);
  if (stack_buf) {
    if (stack_in_sram)
      free(stack_buf);
    else
      umm_free(stack_buf);
  }
  if (load_base)
    umm_free(load_base);
  if (phdr_table)
    umm_free(phdr_table);
  if (napi)
    umm_free(napi);
  if (f)
    sdcard_fclose(f);
  g_core1_pause = false;

  return ok;
}
#endif  // !PICODECK_SIMULATOR

// Same identity lifecycle as Lua apps: installed before the ELF is loaded,
// cleared after the app has returned and been torn down.
static bool native_run(const app_entry_t *app) {
  if (!app_identity_begin(app)) {
    // The launcher refuses invalid ids first; this is the backstop.
    crashlog_write("NATIVE ERROR", app->name, "Failed to start app:",
                   "invalid app id (or out of PSRAM)");
    return false;
  }
  bool ok = native_run_app(app);
  // sys->poll drops a reboot-ota that arrives while an app runs; an app that
  // never polls leaves it latched for the launcher, so drop it here too.
  if (dev_commands_wants_reboot_ota()) {
    dev_commands_clear_reboot_ota();
    printf("[DEV] reboot-ota ignored: it arrived while an app was running\n");
  }
  app_identity_end();
  return ok;
}

static bool native_can_handle(const app_entry_t *app) {
  return app->type == APP_TYPE_NATIVE;
}

const AppRunner g_native_runner = {"native", native_can_handle, native_run};
