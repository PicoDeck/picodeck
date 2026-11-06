// unicorn_runner.c — Unicorn Engine integration for running ARM ELF native apps
// in the PicoDeck simulator. Loads ELF32 PIE binaries, sets up emulated memory
// regions, wires the PicoCalcAPI struct with trampoline-based function pointers,
// and dispatches API calls to host simulator functions.

#include "unicorn_runner.h"
#include <unicorn/unicorn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

// PicoDeck includes
#include "os.h"
#include "elf_plan.h"
#include "crashlog.h"
#include "sim_hooks.h"
#include "hal/hal_display.h"
#include "hal/hal_input.h"
#include "hal/hal_sdcard.h"
#include "hal/hal_timing.h"

// Display functions (from simulator stubs)
extern void display_clear_clip_rect(void);
extern void display_set_font(int font_id);
extern void display_clear(uint16_t color);
extern int  display_draw_text(int x, int y, const char *text, uint16_t fg, uint16_t bg);
extern void display_flush(void);

// =============================================================================
// Memory map constants
// =============================================================================

// What a native app has on the device: one QMI PSRAM heap (lua_psram_alloc.c)
// that holds its image, its stack and its qmiAlloc blocks, and a 64 KB stack
// (NATIVE_STACK_SIZE, native_loader.c).
#define DEVICE_HEAP_SIZE    (6u * 1024 * 1024 - 128u * 1024)
#define DEVICE_STACK_SIZE   (64u * 1024)
#define DEVICE_BLOCK_SIZE   200u                 // UMM_BLOCK_BODY_SIZE

#define EMU_CODE_BASE       0x10000000u
#define EMU_CODE_SIZE       (8u * 1024 * 1024)   // above the largest image allowed

#define EMU_DATA_BASE       0x20000000u
#define EMU_DATA_SIZE       (4u * 1024 * 1024)   // 4MB

#define EMU_STACK_BASE      0x30000000u
// More than the device's, because a real app goes well past 64 KB here (C-Dogs, on
// the way into a mission) and refusing it would stop it running at all. The part
// below the device's stack starts read-only, so the first write there is noticed
// and logged at no cost to any other access (write_prot_hook). Below that, pages
// are mapped as they are touched, down to the end of the data region, as they
// always were (mem_error_hook): only an access outside that window is a fault.
#define EMU_STACK_SIZE      (256u * 1024)
#define EMU_STACK_DEEP_SIZE (EMU_STACK_SIZE - DEVICE_STACK_SIZE)

#define EMU_HEAP_BASE       0x40000000u
#define EMU_HEAP_SIZE       ((DEVICE_HEAP_SIZE + 0xFFFu) & ~0xFFFu)

#define EMU_ARENA_BASE      0x50000000u
#define EMU_ARENA_SIZE      (64u * 1024)          // 64KB

#define EMU_FB_BASE         0xD0000000u
#define EMU_FB_SIZE         (320 * 320 * 2)       // ~200KB, round up to page

#define EMU_API_BASE        0xE0000000u
#define EMU_API_SIZE        (64u * 1024)          // 64KB for g_api + sub-tables

#define EMU_TRAMP_BASE      0xF0000000u
#define EMU_TRAMP_SIZE      (4u * 1024)           // 4KB for trampolines

#define EMU_REVCALL_BASE    0xF0010000u
#define EMU_REVCALL_SIZE    (4u * 1024)           // 4KB for reverse-call stubs
#define EMU_RETURN_STUB     (EMU_REVCALL_BASE + 8u)  // picodeck_main returns here

// Maximum number of trampoline slots (API functions)
#define MAX_TRAMP_SLOTS     512

// Flag set when uc_emu_stop was called during nested emulation (SVC #254).
// The outer emulation loop checks this to know it should restart.
int g_emu_nested_stop = 0;

// =============================================================================
// ELF validation/relocation: src/os/elf_plan.c (shared with the firmware loader)
// =============================================================================

// =============================================================================
// Handle table: maps 32-bit emulated handles <-> 64-bit host pointers
// =============================================================================

#define MAX_HANDLES 256

static void *s_handle_to_host[MAX_HANDLES];
static uint32_t s_next_handle = 1;  // 0 = NULL

uint32_t handle_wrap(void *host_ptr) {
    if (!host_ptr) return 0;
    // Search for existing mapping first
    for (uint32_t i = 1; i < MAX_HANDLES; i++) {
        if (s_handle_to_host[i] == host_ptr) return i;
    }
    // Scan for a free slot starting from s_next_handle
    for (int i = 0; i < MAX_HANDLES; i++) {
        uint32_t idx = (s_next_handle + i) % MAX_HANDLES;
        if (idx == 0) idx = 1;  // skip slot 0 (reserved for NULL)
        if (s_handle_to_host[idx] == NULL) {
            s_handle_to_host[idx] = host_ptr;
            s_next_handle = idx + 1;
            return idx;
        }
    }
    fprintf(stderr, "[UNICORN] Handle table exhausted!\n");
    return 0;
}

void *handle_unwrap(uint32_t handle) {
    if (handle == 0 || handle >= MAX_HANDLES) return NULL;
    return s_handle_to_host[handle];
}

void handle_free(uint32_t handle) {
    if (handle > 0 && handle < MAX_HANDLES) {
        s_handle_to_host[handle] = NULL;
    }
}

static void handle_reset(void) {
    memset(s_handle_to_host, 0, sizeof(s_handle_to_host));
    s_next_handle = 1;
}

// =============================================================================
// String arena: bump allocator for returning strings to emulated code
// =============================================================================

static uint32_t s_arena_offset = 0;
static uint32_t s_arena_reserved = 0;  // watermark: protect startup strings from wrap-around

uint32_t arena_write_string(uc_engine *uc, const char *str) {
    if (!str) return 0;
    uint32_t len = (uint32_t)strlen(str) + 1;
    if (s_arena_offset + len > EMU_ARENA_SIZE) {
        // Wrap around, but skip past the reserved startup strings
        s_arena_offset = s_arena_reserved;
        if (s_arena_offset + len > EMU_ARENA_SIZE) return 0;
    }
    uint32_t addr = EMU_ARENA_BASE + s_arena_offset;
    uc_mem_write(uc, addr, str, len);
    s_arena_offset += (len + 3) & ~3u;  // Align to 4 bytes
    return addr;
}

uint32_t arena_write_buffer(uc_engine *uc, const void *data, uint32_t len) {
    if (!data || len == 0) return 0;
    if (s_arena_offset + len > EMU_ARENA_SIZE) {
        s_arena_offset = s_arena_reserved;
        if (s_arena_offset + len > EMU_ARENA_SIZE) return 0;
    }
    uint32_t addr = EMU_ARENA_BASE + s_arena_offset;
    uc_mem_write(uc, addr, data, len);
    s_arena_offset += (len + 3) & ~3u;
    return addr;
}

// =============================================================================
// Heap emulator for qmiAlloc/qmiFree: first fit over the spans of the region,
// sorted by offset, with neighbours merged on free. Sizes round up to the
// device's block, so many small requests run out as they do there. The list
// lives on the host: nothing is written into emulated memory, and a reused block
// keeps its old bytes (umm_malloc does not zero either).
// =============================================================================

typedef struct {
    uint32_t off;
    uint32_t size;
    bool used;
} heap_span_t;

static heap_span_t *s_heap_spans = NULL;
static int s_heap_count = 0;
static int s_heap_cap = 0;
static uint32_t s_heap_limit = 0;

static bool heap_span_insert(int at, heap_span_t span) {
    if (s_heap_count == s_heap_cap) {
        int cap = s_heap_cap ? s_heap_cap * 2 : 64;
        heap_span_t *grown = realloc(s_heap_spans, (size_t)cap * sizeof(*grown));
        if (!grown)
            return false;
        s_heap_spans = grown;
        s_heap_cap = cap;
    }
    memmove(&s_heap_spans[at + 1], &s_heap_spans[at],
            (size_t)(s_heap_count - at) * sizeof(*s_heap_spans));
    s_heap_spans[at] = span;
    s_heap_count++;
    return true;
}

static void heap_span_remove(int at) {
    s_heap_count--;
    memmove(&s_heap_spans[at], &s_heap_spans[at + 1],
            (size_t)(s_heap_count - at) * sizeof(*s_heap_spans));
}

// `limit` bytes are free, all in one span.
static void heap_reset(uint32_t limit) {
    s_heap_limit = limit;
    s_heap_count = 0;
    if (limit > 0)
        heap_span_insert(0, (heap_span_t){0, limit, false});
}

uint32_t heap_alloc(uint32_t size) {
    if (size == 0 || size > s_heap_limit)  // umm_malloc(0) is NULL too
        return 0;
    size = (size + DEVICE_BLOCK_SIZE - 1) / DEVICE_BLOCK_SIZE * DEVICE_BLOCK_SIZE;
    for (int i = 0; i < s_heap_count; i++) {
        heap_span_t *span = &s_heap_spans[i];
        if (span->used || span->size < size)
            continue;
        uint32_t off = span->off, left = span->size - size;
        if (left > 0 && !heap_span_insert(i + 1, (heap_span_t){off + size, left, false}))
            return 0;
        s_heap_spans[i].size = size;
        s_heap_spans[i].used = true;
        return EMU_HEAP_BASE + off;
    }
    return 0;
}

void heap_free(uint32_t addr) {
    if (addr < EMU_HEAP_BASE)
        return;
    uint32_t off = addr - EMU_HEAP_BASE;
    for (int i = 0; i < s_heap_count; i++) {
        heap_span_t *span = &s_heap_spans[i];
        if (span->off != off || !span->used)
            continue;
        span->used = false;
        if (i + 1 < s_heap_count && !s_heap_spans[i + 1].used) {
            span->size += s_heap_spans[i + 1].size;
            heap_span_remove(i + 1);
        }
        if (i > 0 && !s_heap_spans[i - 1].used) {
            s_heap_spans[i - 1].size += s_heap_spans[i].size;
            heap_span_remove(i);
        }
        return;
    }
    // A pointer qmiAlloc did not return: ignored, as native_res_drop does.
}

// =============================================================================
// Helper: read NUL-terminated string from emulated memory
// =============================================================================

static char s_str_buf[4][1024];
static int s_str_idx = 0;

char *uc_read_string(uc_engine *uc, uint32_t addr) {
    if (addr == 0) return NULL;
    char *buf = s_str_buf[s_str_idx];
    s_str_idx = (s_str_idx + 1) & 3;
    // Read in small chunks that stop at page ends, so none crosses a memory
    // region boundary (a string can end right at the top of the stack). A bulk
    // uc_mem_read that spans past a mapped region zeroes the buffer.
    size_t max = sizeof(s_str_buf[0]) - 1;
    size_t pos = 0;
    while (pos < max) {
        size_t chunk = 64;
        size_t to_page_end = 0x1000u - ((addr + pos) & 0xFFFu);
        if (chunk > to_page_end) chunk = to_page_end;
        if (pos + chunk > max) chunk = max - pos;
        uc_err err = uc_mem_read(uc, addr + pos, &buf[pos], chunk);
        if (err != UC_ERR_OK) {
            buf[pos] = '\0';
            return buf;
        }
        // Check for NUL within this chunk
        for (size_t i = pos; i < pos + chunk; i++) {
            if (buf[i] == '\0') return buf;
        }
        pos += chunk;
    }
    buf[max] = '\0';
    return buf;
}

// =============================================================================
// Trampoline dispatch system
// =============================================================================

// Forward declarations for trampoline handlers (defined in unicorn_trampolines.c)
extern void unicorn_tramp_init(uc_engine *uc);
extern void unicorn_tramp_dispatch(uc_engine *uc, uint32_t slot);
extern uint32_t unicorn_tramp_get_slot_count(void);

// Global Unicorn engine pointer (used by trampolines)
uc_engine *g_uc = NULL;

// What stopped the app where the device would have taken a HardFault: an access
// to unmapped memory, a write to the read-only low addresses, a bad instruction.
// The runner ends the app on it (see end_app_on_fault).
static bool s_faulted = false;
static char s_fault_text[96];

static void fault_note(const char *fmt, ...) {
    if (s_faulted)
        return;  // the first is the cause
    s_faulted = true;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_fault_text, sizeof(s_fault_text), fmt, ap);
    va_end(ap);
}

// Interrupt hook for SVC-based trampolines.
// On Cortex-M, SVC generates exception #11 (SVCall). Unicorn passes the
// interrupt number via the `intno` parameter.
static void trampoline_hook(uc_engine *uc, uint32_t intno, void *user_data) {
    (void)user_data;

    // SVC on ARM: Unicorn reports intno=2 (QEMU convention) or intno=3.
    // We need to read the SVC number from the instruction itself.
    // The PC points to the instruction AFTER the SVC, so read PC-2.
    if (intno == 2 || intno == 3) {
        uint32_t pc;
        uc_reg_read(uc, UC_ARM_REG_PC, &pc);
        // In Unicorn's interrupt hook, PC points AT the triggering instruction.
        uint16_t insn;
        uc_mem_read(uc, pc, &insn, 2);

        // picodeck_main returned: LR started out pointing at this stub.
        if (pc == EMU_RETURN_STUB) {
            printf("[UNICORN] Normal exit: app returned from picodeck_main()\n");
            uc_emu_stop(uc);
            return;
        }

        // Only handle SVC instructions (0xDFxx).
        if ((insn & 0xFF00) != 0xDF00) {
            fault_note("bad instruction 0x%04x", insn);
            uc_emu_stop(uc);
            return;
        }

        // SVC #254 at REVCALL_BASE: nested callback return sentinel.
        // (Checked by address, not SVC number, so slot 254 stays usable.)
        if (pc == EMU_REVCALL_BASE) {
            g_emu_nested_stop = 1;
            uc_emu_stop(uc);
            return;
        }

        // Derive the slot from the PC, not the SVC immediate: SVC #imm8
        // wraps at 256, but slot count has grown past that.
        if (pc < EMU_TRAMP_BASE || pc >= EMU_TRAMP_BASE + MAX_TRAMP_SLOTS * 4) {
            fault_note("SVC outside the API table");
            uc_emu_stop(uc);
            return;
        }
        uint32_t slot = (pc - EMU_TRAMP_BASE) / 4;

        // Dispatch the API call
        unicorn_tramp_dispatch(uc, slot);

        // A callback it ran into the app (listDir, a menu item) faulted.
        if (s_faulted) {
            uc_emu_stop(uc);
            return;
        }

        // Return to caller: read LR (set by the BLX that called the trampoline)
        // and write it to PC.  Strip Thumb bit (bit 0) since PC doesn't encode it.
        uint32_t lr;
        uc_reg_read(uc, UC_ARM_REG_LR, &lr);
        uc_reg_write(uc, UC_ARM_REG_PC, &lr);
    } else {
        fault_note("exception %u", intno);
        uc_emu_stop(uc);
    }
}

// =============================================================================
// ELF loader
// =============================================================================

// Name of the app being loaded, for load_refused().
static const char *s_loading_app_name = NULL;

// A load failure, reported like the firmware loader's show_error()
// (native_loader.c): an "[UNICORN] ELF rejected" line on stderr, a
// NATIVE ERROR entry in /system/error.log, and the test channel's outcome
// (app.exited result "load_failed", error "<reason>[: <detail>]").
static void load_refused(const char *reason, const char *detail) {
    char msg[160];
    if (detail && detail[0])
        snprintf(msg, sizeof(msg), "%s: %s", reason, detail);
    else
        snprintf(msg, sizeof(msg), "%s", reason);
    fprintf(stderr, "[UNICORN] ELF rejected: %s\n", msg);
    crashlog_write("NATIVE ERROR", s_loading_app_name, reason, detail);
    sim_app_outcome_set(SIM_APP_RESULT_LOAD_FAILED, msg);
    sim_log_err("[NATIVE] %s: %s", s_loading_app_name ? s_loading_app_name : "?", msg);
}

static bool load_elf(uc_engine *uc, const char *path, uint32_t *out_entry) {
    // Resolve virtual SD card path to host filesystem path
    char full_path[1024];
    if (!hal_sdcard_resolve(path, full_path, sizeof(full_path))) {
        load_refused("ELF: cannot open file", path);
        return false;
    }

    FILE *f = fopen(full_path, "rb");
    if (!f) {
        fprintf(stderr, "[UNICORN] Failed to open ELF: %s (resolved: %s)\n", path, full_path);
        load_refused("ELF: cannot open file", path);
        return false;
    }

    fseek(f, 0, SEEK_END);
    long file_len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (file_len < 0 || file_len > 0x7FFFFFFFL) file_len = 0;

    // Same validation as the firmware loader (elf_plan.c): a malformed image
    // is refused here, before anything reaches Unicorn.
    uint8_t ehdr_buf[ELF_EHDR_SIZE];
    size_t got = fread(ehdr_buf, 1, sizeof(ehdr_buf), f);
    elf_plan_t plan;
    elf_err_t eerr = elf_plan_header(ehdr_buf, (uint32_t)got,
                                     (uint32_t)file_len, &plan);
    if (eerr != ELF_OK) {
        load_refused(elf_strerror(eerr), NULL);
        fclose(f);
        return false;
    }

    uint8_t *phdrs = (uint8_t *)malloc(plan.phdrs_size);
    if (!phdrs) {
        load_refused("ELF: out of memory for phdr", NULL);
        fclose(f);
        return false;
    }
    if (fseek(f, (long)plan.phoff, SEEK_SET) != 0 ||
        fread(phdrs, plan.phdrs_size, 1, f) != 1) {
        load_refused("ELF: failed to read phdr table", NULL);
        free(phdrs);
        fclose(f);
        return false;
    }

    eerr = elf_plan_segments(&plan, phdrs, plan.phdrs_size,
                             (uint32_t)file_len, NATIVE_MAX_IMAGE_SIZE);
    if (eerr != ELF_OK) {
        char detail[48] = "";
        if (eerr == ELF_ERR_IMAGE_TOO_LARGE)
            snprintf(detail, sizeof(detail), "> %u byte limit",
                     (uint32_t)NATIVE_MAX_IMAGE_SIZE);
        load_refused(elf_strerror(eerr), detail[0] ? detail : NULL);
        free(phdrs);
        fclose(f);
        return false;
    }

    uint32_t mem_min = plan.mem_min;
    uint32_t image_size = plan.image_size;

    // The device would run out here, with the OS's own allocations on top: the
    // image and the stack have to fit its heap, and qmiAlloc gets what is left.
    if (image_size > DEVICE_HEAP_SIZE - DEVICE_STACK_SIZE) {
        char detail[64];
        snprintf(detail, sizeof(detail), "image needs %uK, the device has at most %uK",
                 image_size / 1024u, (DEVICE_HEAP_SIZE - DEVICE_STACK_SIZE) / 1024u);
        load_refused("ELF: out of PSRAM", detail);
        free(phdrs);
        fclose(f);
        return false;
    }
    heap_reset(DEVICE_HEAP_SIZE - DEVICE_STACK_SIZE - image_size);

    printf("[UNICORN] ELF image: %u bytes (vaddr 0x%08x..0x%08x)\n",
           image_size, mem_min, plan.mem_max);

    // Allocate a host-side buffer to build the image, then write it all at once
    uint8_t *image_buf = (uint8_t *)calloc(1, image_size);
    if (!image_buf) {
        load_refused("ELF: out of memory for image", NULL);
        free(phdrs);
        fclose(f);
        return false;
    }

    // Copy PT_LOAD segments into the buffer (bounds proven by elf_plan)
    for (uint16_t i = 0; i < plan.phnum; i++) {
        elf32_phdr_t ph = elf_plan_phdr(phdrs, i);
        if (ph.p_type != ELF_PT_LOAD || ph.p_filesz == 0) continue;
        fseek(f, (long)ph.p_offset, SEEK_SET);
        size_t off = ph.p_vaddr - mem_min;
        if (fread(image_buf + off, ph.p_filesz, 1, f) != 1) {
            load_refused("ELF: failed to read segment", NULL);
            free(image_buf);
            free(phdrs);
            fclose(f);
            return false;
        }
    }

    // Apply R_ARM_RELATIVE relocations: the image runs at EMU_CODE_BASE.
    elf_region_t region = {mem_min, image_size, image_buf, EMU_CODE_BASE};
    elf_reloc_stats_t rstats;
    eerr = elf_relocate(&plan, &region, 1, &rstats);
    if (eerr != ELF_OK) {
        load_refused(elf_strerror(eerr), NULL);
        free(image_buf);
        free(phdrs);
        fclose(f);
        return false;
    }

    // Write the relocated image into Unicorn memory
    uc_err err = uc_mem_write(uc, EMU_CODE_BASE, image_buf, image_size);
    if (err != UC_ERR_OK) {
        load_refused("sim: failed to write ELF image", uc_strerror(err));
        free(image_buf);
        free(phdrs);
        fclose(f);
        return false;
    }

    // Map the ELF's original virtual address range (0x0..image_size) read-only.
    // Newlib and other library code may read through unrelocated pointers (e.g.
    // internal _reent function pointers, lock stubs) into the original VirtAddr
    // space. On the RP2350 low addresses land in the boot ROM, where such a read
    // does not fault: it just returns other bytes. A write or a call does not
    // get this far.
    if (mem_min == 0 && image_size > 0) {
        uint32_t shadow_size = (image_size + 0xFFF) & ~0xFFFu;
        uc_err serr = uc_mem_map(uc, 0, shadow_size, UC_PROT_READ);
        if (serr != UC_ERR_OK) {
            fprintf(stderr, "[UNICORN] WARNING: shadow region map failed: %s (size=0x%08x)\n",
                    uc_strerror(serr), shadow_size);
        }
    }

    // Compute entry point (inside the image, checked by elf_plan)
    *out_entry = EMU_CODE_BASE + plan.entry_off;
    // Set Thumb bit
    *out_entry |= 1u;

    printf("[UNICORN] Entry point: 0x%08x\n", *out_entry);

    free(image_buf);
    free(phdrs);
    fclose(f);
    return true;
}

// =============================================================================
// Build emulated g_api struct in Unicorn memory
// =============================================================================

// These are defined in unicorn_trampolines.c
extern void unicorn_build_api_struct(uc_engine *uc, uint32_t api_base, uint32_t tramp_base);

// =============================================================================
// Main entry point
// An access to unmapped memory: the device would fault, so the access fails and
// emulation stops (returning false here).
static bool mem_error_hook(uc_engine *uc, uc_mem_type type,
                           uint64_t address, int size, int64_t value,
                           void *user_data) {
    (void)uc; (void)size; (void)user_data; (void)value;
    uint32_t addr = (uint32_t)address;
    // The window below the stack: the stack still growing, not a bad pointer.
    if (type != UC_MEM_FETCH_UNMAPPED && addr < EMU_STACK_BASE &&
        addr >= EMU_DATA_BASE + EMU_DATA_SIZE) {
        return uc_mem_map(uc, addr & ~0xFFFu, 0x1000, UC_PROT_ALL) == UC_ERR_OK;
    }
    const char *op = (type == UC_MEM_READ_UNMAPPED) ? "read" :
                     (type == UC_MEM_WRITE_UNMAPPED) ? "write" : "call";
    fault_note("%s of unmapped address 0x%08x", op, addr);
    return false;
}

// A write to memory mapped read-only. In the stack's deep part it is the app going
// past what the device's stack holds: say so once, with where, and let it run. Any
// other write (the low addresses) is a fault.
static bool write_prot_hook(uc_engine *uc, uc_mem_type type,
                            uint64_t address, int size, int64_t value,
                            void *user_data) {
    (void)type; (void)size; (void)value; (void)user_data;
    uint32_t addr = (uint32_t)address;
    if (addr < EMU_STACK_BASE || addr >= EMU_STACK_BASE + EMU_STACK_DEEP_SIZE) {
        fault_note("write to read-only address 0x%08x", addr);
        return false;
    }
    uint32_t pc, lr;
    uc_reg_read(uc, UC_ARM_REG_PC, &pc);
    uc_reg_read(uc, UC_ARM_REG_LR, &lr);
    fprintf(stderr, "[UNICORN] stack passed %u KB at PC=0x%08x LR=0x%08x\n",
            DEVICE_STACK_SIZE / 1024u, pc, lr);
    sim_log_err("[NATIVE] %s: stack passed %u KB at PC=0x%08x LR=0x%08x (image at "
                "0x%08x): the device's stack is that size, so it would fault here",
                s_loading_app_name ? s_loading_app_name : "?", DEVICE_STACK_SIZE / 1024u,
                pc, lr, (uint32_t)EMU_CODE_BASE);
    return uc_mem_protect(uc, EMU_STACK_BASE, EMU_STACK_DEEP_SIZE, UC_PROT_ALL) == UC_ERR_OK;
}

// The device takes a HardFault here, shows its fault screen and reboots. The
// simulator ends the app, reports it the way a Lua error is, and goes back to
// the launcher. Returns false for the launcher's `ok`.
static bool end_app_on_fault(uc_engine *uc, const char *app_name) {
    uint32_t pc, lr;
    uc_reg_read(uc, UC_ARM_REG_PC, &pc);
    uc_reg_read(uc, UC_ARM_REG_LR, &lr);
    char where[64];
    snprintf(where, sizeof(where), "PC=0x%08x LR=0x%08x (image at 0x%08x)", pc, lr,
             (uint32_t)EMU_CODE_BASE);
    char msg[192];
    snprintf(msg, sizeof(msg), "fault: %s, %s", s_fault_text, where);

    fprintf(stderr, "[UNICORN] %s\n", msg);
    crashlog_write("NATIVE ERROR", app_name, s_fault_text, where);
    sim_app_outcome_set(SIM_APP_RESULT_ERROR, msg);
    sim_log_err("[NATIVE] %s: %s", app_name, msg);

    display_clear_clip_rect();  // whatever the app left set
    display_set_font(0);
    display_clear(0);
    display_draw_text(8, 8, "App crashed", 0xF800, 0);
    display_draw_text(8, 20, app_name, 0xFFFF, 0);
    display_draw_text(8, 36, s_fault_text, 0xFFFF, 0);
    display_draw_text(8, 48, where, 0x8410, 0);
    display_flush();
    for (int i = 0; i < 30 && !sim_test_mode(); i++)
        hal_sleep_ms(100);
    return false;
}

// =============================================================================

bool unicorn_run_app(const char *elf_path, const char *app_dir,
                     const char *app_id, const char *app_name) {
    printf("[UNICORN] Loading native app: %s\n", elf_path);
    s_loading_app_name = app_name;

    uc_engine *uc;
    uc_err err;

    // Create Unicorn instance (ARM Thumb mode, Cortex-M33)
    err = uc_open(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS, &uc);
    if (err != UC_ERR_OK) {
        load_refused("sim: uc_open failed", uc_strerror(err));
        return false;
    }

    g_uc = uc;

    // Reset state
    handle_reset();
    s_arena_offset = 0;
    s_arena_reserved = 0;
    s_faulted = false;
    heap_reset(0);

    // ── Map memory regions ──────────────────────────────────────────────────

    // Code region
    err = uc_mem_map(uc, EMU_CODE_BASE, EMU_CODE_SIZE, UC_PROT_ALL);
    if (err != UC_ERR_OK) {
        load_refused("sim: failed to map code region", uc_strerror(err));
        goto fail;
    }

    // Data region (for large ELFs that have data segments far from code)
    err = uc_mem_map(uc, EMU_DATA_BASE, EMU_DATA_SIZE, UC_PROT_ALL);
    if (err != UC_ERR_OK) {
        load_refused("sim: failed to map data region", uc_strerror(err));
        goto fail;
    }

    // Stack (grows down from top)
    err = uc_mem_map(uc, EMU_STACK_BASE, EMU_STACK_SIZE, UC_PROT_ALL);
    if (err == UC_ERR_OK)
        err = uc_mem_protect(uc, EMU_STACK_BASE, EMU_STACK_DEEP_SIZE, UC_PROT_READ);
    if (err != UC_ERR_OK) {
        load_refused("sim: failed to map stack", uc_strerror(err));
        goto fail;
    }

    // Heap
    err = uc_mem_map(uc, EMU_HEAP_BASE, EMU_HEAP_SIZE, UC_PROT_ALL);
    if (err != UC_ERR_OK) {
        load_refused("sim: failed to map heap", uc_strerror(err));
        goto fail;
    }

    // String arena
    err = uc_mem_map(uc, EMU_ARENA_BASE, EMU_ARENA_SIZE, UC_PROT_ALL);
    if (err != UC_ERR_OK) {
        load_refused("sim: failed to map arena", uc_strerror(err));
        goto fail;
    }

    // Framebuffer — use Unicorn-managed memory so emulated app writes stay in
    // the emulated address space.  The flush trampoline copies data out via
    // uc_mem_read() into the simulator's back buffer before presenting.
    {
        uint32_t fb_map_size = (EMU_FB_SIZE + 0xFFF) & ~0xFFFu;
        err = uc_mem_map(uc, EMU_FB_BASE, fb_map_size, UC_PROT_ALL);
        if (err != UC_ERR_OK) {
            load_refused("sim: failed to map framebuffer", uc_strerror(err));
            goto fail;
        }
    }

    // API struct region
    err = uc_mem_map(uc, EMU_API_BASE, EMU_API_SIZE, UC_PROT_ALL);
    if (err != UC_ERR_OK) {
        load_refused("sim: failed to map API region", uc_strerror(err));
        goto fail;
    }

    // Trampoline region
    err = uc_mem_map(uc, EMU_TRAMP_BASE, EMU_TRAMP_SIZE, UC_PROT_ALL);
    if (err != UC_ERR_OK) {
        load_refused("sim: failed to map trampoline region", uc_strerror(err));
        goto fail;
    }

    // Reverse-call stubs region — used as return address for nested uc_emu_start callbacks.
    // Write a NOP at REVCALL_BASE so Unicorn's `until` address check fires before executing.
    err = uc_mem_map(uc, EMU_REVCALL_BASE, EMU_REVCALL_SIZE, UC_PROT_ALL);
    if (err != UC_ERR_OK) {
        load_refused("sim: failed to map reverse-call region", uc_strerror(err));
        goto fail;
    }
    {
        // Write SVC #254 + BX LR at REVCALL_BASE. When a nested callback
        // returns (BX LR → REVCALL_BASE), it executes SVC #254 which the
        // interrupt hook recognizes as "end of nested callback" and calls
        // uc_emu_stop. The global stop flag is acceptable here because
        // the trampoline code that invoked uc_emu_start expects it.
        uint16_t revcall_code[2] = {
            0xDFFE,  // SVC #254
            0x4770,  // BX LR (safety, shouldn't reach here)
        };
        uc_mem_write(uc, EMU_REVCALL_BASE, revcall_code, sizeof(revcall_code));
        // picodeck_main's return address: the hook recognises it by address.
        uint16_t return_code = 0xDFFD;  // SVC #253
        uc_mem_write(uc, EMU_RETURN_STUB, &return_code, sizeof(return_code));
    }

    // ── Fill trampoline region with SVC #N + BX LR instructions ────────
    // Each trampoline slot is 4 bytes: SVC #slot (2 bytes) + BX LR (2 bytes)
    // The SVC triggers an interrupt which we intercept via UC_HOOK_INTR.
    // The interrupt hook dispatches the API call, then BX LR returns to caller.
    {
        for (uint32_t i = 0; i < MAX_TRAMP_SLOTS; i++) {
            // SVC #imm8: encoding = 0xDF00 | (imm8 & 0xFF). The immediate
            // wraps past 255, but the hook derives the slot from the PC,
            // so any slot count works — the encoding just needs to be an
            // SVC. Keep imm = slot & 0xFF for debug readability.
            uint16_t svc = 0xDF00 | (i & 0xFF);  // SVC #i
            uint16_t bx_lr = 0x4770;              // BX LR
            uc_mem_write(uc, EMU_TRAMP_BASE + i * 4, &svc, 2);
            uc_mem_write(uc, EMU_TRAMP_BASE + i * 4 + 2, &bx_lr, 2);
        }
    }

    // ── Set up interrupt hook for SVC-based trampolines ─────────────────
    {
        uc_hook hook;
        err = uc_hook_add(uc, &hook, UC_HOOK_INTR,
                          (void *)trampoline_hook, NULL,
                          1, 0);  // range ignored for INTR hooks
        if (err != UC_ERR_OK) {
            load_refused("sim: failed to add interrupt hook", uc_strerror(err));
            goto fail;
        }
    }


    // ── Unmapped accesses end the app ──────────────────────────────────
    {
        uc_hook mem_hook;
        err = uc_hook_add(uc, &mem_hook,
                          UC_HOOK_MEM_READ_UNMAPPED | UC_HOOK_MEM_WRITE_UNMAPPED | UC_HOOK_MEM_FETCH_UNMAPPED,
                          (void *)mem_error_hook, NULL, 1, 0);
        if (err != UC_ERR_OK) {
            fprintf(stderr, "[UNICORN] Failed to add memory hook: %s\n", uc_strerror(err));
        }
        err = uc_hook_add(uc, &mem_hook, UC_HOOK_MEM_WRITE_PROT,
                          (void *)write_prot_hook, NULL, 1, 0);
        if (err != UC_ERR_OK) {
            fprintf(stderr, "[UNICORN] Failed to add write hook: %s\n", uc_strerror(err));
        }
    }

    // ── Initialize trampolines ──────────────────────────────────────────
    unicorn_tramp_init(uc);

    // ── Build the emulated API struct ───────────────────────────────────
    unicorn_build_api_struct(uc, EMU_API_BASE, EMU_TRAMP_BASE);

    // ── Load the ELF ────────────────────────────────────────────────────
    uint32_t entry_point;
    if (!load_elf(uc, elf_path, &entry_point)) {
        fprintf(stderr, "[UNICORN] Failed to load ELF\n");
        goto fail;
    }

    // ── Write entry strings into the arena ──────────────────────────────
    uint32_t emu_app_dir  = arena_write_string(uc, app_dir);
    uint32_t emu_app_id   = arena_write_string(uc, app_id);
    uint32_t emu_app_name = arena_write_string(uc, app_name);
    s_arena_reserved = s_arena_offset;  // protect startup strings from wrap-around

    // ── Set up registers ────────────────────────────────────────────────
    // picodeck_main(const PicoCalcAPI *api, const char *app_dir,
    //            const char *app_id, const char *app_name)
    uint32_t sp = EMU_STACK_BASE + EMU_STACK_SIZE;  // Stack top
    uint32_t r0 = EMU_API_BASE;     // api pointer
    uint32_t r1 = emu_app_dir;      // app_dir
    uint32_t r2 = emu_app_id;       // app_id
    uint32_t r3 = emu_app_name;     // app_name

    uc_reg_write(uc, UC_ARM_REG_SP, &sp);
    uc_reg_write(uc, UC_ARM_REG_R0, &r0);
    uc_reg_write(uc, UC_ARM_REG_R1, &r1);
    uc_reg_write(uc, UC_ARM_REG_R2, &r2);
    uc_reg_write(uc, UC_ARM_REG_R3, &r3);

    // Returning from picodeck_main lands on the return stub, so a call through a
    // NULL function pointer (PC=0) is told apart from a normal exit.
    uint32_t lr = EMU_RETURN_STUB | 1u;
    uc_reg_write(uc, UC_ARM_REG_LR, &lr);

    printf("[UNICORN] Starting emulation at 0x%08x (SP=0x%08x)\n", entry_point, sp);
    fflush(stdout);  // ensure output visible before emulation starts

    // ── Run ─────────────────────────────────────────────────────────────
    // The main emulation loop must restart when uc_emu_stop is called from
    // within a nested callback (SVC #254). The trampoline handler sets
    // g_emu_nest_depth > 0 during nested calls; if the outer emu_start exits
    // while nested, we resume from the current PC.
    {
        uint32_t start_addr = entry_point;
        for (;;) {
            err = uc_emu_start(uc, start_addr, 0, 0, 0);
            uint32_t pc, sp_val, lr_val;
            uc_reg_read(uc, UC_ARM_REG_PC, &pc);
            uc_reg_read(uc, UC_ARM_REG_SP, &sp_val);
            uc_reg_read(uc, UC_ARM_REG_LR, &lr_val);

            if (err == UC_ERR_OK && g_emu_nested_stop && !s_faulted) {
                // Outer emulation was killed by nested uc_emu_stop (SVC #254).
                // Resume from current PC (the SVC dispatch already set PC
                // to LR, so we continue after the trampoline call).
                g_emu_nested_stop = 0;
                start_addr = pc | 1u;  // Thumb bit
                continue;
            }

            printf("[UNICORN] emu_start returned: err=%d pc=0x%08x sp=0x%08x lr=0x%08x nested=%d\n",
                   err, pc, sp_val, lr_val, g_emu_nested_stop);
            fflush(stdout);
            if (err != UC_ERR_OK)
                fault_note("%s", uc_strerror(err));  // no hook covers it
            fflush(stdout); fflush(stderr);
            break;
        }
    }

    printf("[UNICORN] App '%s' finished\n", app_name);

    bool ok = s_faulted ? end_app_on_fault(uc, app_name) : true;

    uc_close(uc);
    g_uc = NULL;
    return ok;

fail:
    uc_close(uc);
    g_uc = NULL;
    return false;
}
