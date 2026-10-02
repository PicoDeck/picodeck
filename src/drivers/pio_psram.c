#include "pio_psram.h"
#include "pio_psram_bulk.h"
#include "pio_psram_qpi.h"
#include "../hardware.h"
#include "display.h"

#include "pico/time.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
    PSRAM_MODE_NONE = 0,
    PSRAM_MODE_QPI,
    PSRAM_MODE_SERIAL,
} psram_mode_t;

static psram_mode_t s_mode = PSRAM_MODE_NONE;

bool pio_psram_init(void) {
    if (s_mode != PSRAM_MODE_NONE) return true;

    if (pio_psram_qpi_init()) {
        s_mode = PSRAM_MODE_QPI;
        printf("[PIO_PSRAM] QPI mode @ %lu kHz SPI\n",
               (unsigned long)pio_psram_qpi_spi_khz());
        return true;
    }

    if (pio_psram_bulk_init()) {
        s_mode = PSRAM_MODE_SERIAL;
        printf("[PIO_PSRAM] Serial fallback mode (25 MHz SPI)\n");
        return true;
    }

    printf("[PIO_PSRAM] Not available\n");
    return false;
}

void pio_psram_read(uint32_t addr, uint8_t *dst, uint32_t len) {
    if (s_mode == PSRAM_MODE_QPI)
        pio_psram_qpi_read(addr, dst, len);
    else if (s_mode == PSRAM_MODE_SERIAL)
        pio_psram_bulk_read(addr, dst, len);
}

void pio_psram_write(uint32_t addr, const uint8_t *src, uint32_t len) {
    if (s_mode == PSRAM_MODE_QPI)
        pio_psram_qpi_write(addr, src, len);
    else if (s_mode == PSRAM_MODE_SERIAL)
        pio_psram_bulk_write(addr, src, len);
}

uint32_t pio_psram_size(void) {
    return (s_mode != PSRAM_MODE_NONE) ? PIO_PSRAM_SIZE : 0;
}

bool pio_psram_available(void) {
    return s_mode != PSRAM_MODE_NONE;
}

void pio_psram_set_sysclk(uint32_t sys_khz) {
    if (s_mode == PSRAM_MODE_QPI)
        pio_psram_qpi_set_sysclk(sys_khz);
    else if (s_mode == PSRAM_MODE_SERIAL)
        pio_psram_bulk_set_sysclk(sys_khz);
}

const char *pio_psram_mode_str(void) {
    switch (s_mode) {
    case PSRAM_MODE_QPI:    return "qpi";
    case PSRAM_MODE_SERIAL: return "serial";
    default:                return "none";
    }
}

// ── Dev-command test ─────────────────────────────────────────────────────────

#define DBG_BLOCK 4096u

// The buffer is the start of the display's back buffer (SRAM, so the
// figures stay SRAM <-> PIO PSRAM). A static 4 KB buffer held SRAM the MP3
// decoder now uses (issue #28), and the SRAM heap can be too fragmented for
// a 4 KB malloc. Dev commands run on Core 0 between the launcher's or the
// app's own steps, so nothing draws meanwhile; a partial flush still
// DMA-ing the back buffer (display_flush_rows) is waited out first. The
// command leaves its pattern in the buffer's first rows: the launcher
// redraws them, an app that never redraws them shows it there until it
// does (and the command already overwrites the app's PIO PSRAM).
// Single buffer: the expected pattern is regenerated per byte on compare.
static uint8_t *dbg_buf(void) {
    display_wait_for_flush();
    return (uint8_t *)display_get_back_buffer();
}

static inline uint8_t debug_pattern_byte(uint32_t addr, uint32_t i) {
    return (uint8_t)((addr >> 12) * 197u + i * 13u + 5u);
}

static void debug_fill_pattern(uint8_t *buf, uint32_t addr) {
    for (uint32_t i = 0; i < DBG_BLOCK; i++)
        buf[i] = debug_pattern_byte(addr, i);
}

void pio_psram_debug_test(bool full) {
    printf("[PSRAM] mode=%s\n", pio_psram_mode_str());
    if (s_mode == PSRAM_MODE_NONE) return;
    if (s_mode == PSRAM_MODE_QPI)
        printf("[PSRAM] spi_clock=%lu kHz\n",
               (unsigned long)pio_psram_qpi_spi_khz());
    pio_psram_qpi_print_diag();

    uint8_t *buf = dbg_buf();

    // Throughput: 256KB write then read, app region (above mp3/video pools).
    const uint32_t bench_bytes = 256u * 1024u;
    debug_fill_pattern(buf, PIO_PSRAM_APP_BASE);
    uint64_t t0 = time_us_64();
    for (uint32_t off = 0; off < bench_bytes; off += DBG_BLOCK)
        pio_psram_write(PIO_PSRAM_APP_BASE + off, buf, DBG_BLOCK);
    uint64_t t1 = time_us_64();
    for (uint32_t off = 0; off < bench_bytes; off += DBG_BLOCK)
        pio_psram_read(PIO_PSRAM_APP_BASE + off, buf, DBG_BLOCK);
    uint64_t t2 = time_us_64();
    printf("[PSRAM] write 256KB: %lu us (%lu KB/s)\n",
           (unsigned long)(t1 - t0),
           (unsigned long)((uint64_t)bench_bytes * 1000000u / (t1 - t0) / 1024u));
    printf("[PSRAM] read  256KB: %lu us (%lu KB/s)\n",
           (unsigned long)(t2 - t1),
           (unsigned long)((uint64_t)bench_bytes * 1000000u / (t2 - t1) / 1024u));

    // Small-transfer latency: the MP3 staging refill reads ~1KB per call.
    uint64_t t3 = time_us_64();
    for (int i = 0; i < 1000; i++)
        pio_psram_read(PIO_PSRAM_APP_BASE, buf, 1024);
    uint64_t t4 = time_us_64();
    printf("[PSRAM] 1000x 1KB reads: %lu us total, %lu us/call\n",
           (unsigned long)(t4 - t3), (unsigned long)((t4 - t3) / 1000u));

    // Integrity: two-pass sweep (write everything, then verify everything).
    // Two passes catch address aliasing that immediate readback would miss.
    uint32_t start = full ? 0u : PIO_PSRAM_APP_BASE;
    uint32_t end = full ? PIO_PSRAM_SIZE
                        : (PIO_PSRAM_APP_BASE + 1024u * 1024u);
    printf("[PSRAM] integrity sweep %06lX-%06lX (destructive)...\n",
           (unsigned long)start, (unsigned long)(end - 1));

    for (uint32_t addr = start; addr < end; addr += DBG_BLOCK) {
        debug_fill_pattern(buf, addr);
        pio_psram_write(addr, buf, DBG_BLOCK);
    }
    uint32_t errors = 0;
    uint32_t first_bad = 0;
    uint8_t *expect = NULL;  // pattern regenerated per byte below
    for (uint32_t addr = start; addr < end; addr += DBG_BLOCK) {
        memset(buf, 0, DBG_BLOCK);
        pio_psram_read(addr, buf, DBG_BLOCK);
        for (uint32_t i = 0; i < DBG_BLOCK; i++) {
            uint8_t exp = debug_pattern_byte(addr, i);
            if (buf[i] != exp) {
                if (errors == 0) first_bad = addr + i;
                if (errors < 6)
                    printf("[PSRAM]   mismatch @%06lX exp %02X got %02X\n",
                           (unsigned long)(addr + i), exp, buf[i]);
                errors++;
            }
        }
    }
    if (errors)
        printf("[PSRAM] integrity FAIL: %lu bad bytes, first at %06lX\n",
               (unsigned long)errors, (unsigned long)first_bad);
    else
        printf("[PSRAM] integrity OK\n");
}

// ── Page-end read-disturb stress ─────────────────────────────────────────────
// The boot self-test only proves the page-end read path with 50 reads of one
// pattern at two addresses. Audio rings do page-boundary-adjacent reads
// ~176x/sec, so a residual error rate far below the self-test's sensitivity
// is still audible as crackle. This hammers the susceptible path: adversarial
// 16-byte patterns written straddling the last bytes of 1KB pages at 64
// spread addresses, each read `iters` times, plus a sequential pass over the
// real MP3 ring region (0x0000-0x7FFF) using the player's own read shape.
// DESTRUCTIVE to the MP3 ring + app region — run from the launcher only.
void pio_psram_stress_test(uint32_t iters) {
    printf("[PSRAM] stress: mode=%s iters=%lu\n",
           pio_psram_mode_str(), (unsigned long)iters);
    if (s_mode == PSRAM_MODE_NONE || iters == 0) return;
    if (s_mode == PSRAM_MODE_QPI)
        printf("[PSRAM] spi_clock=%lu kHz\n",
               (unsigned long)pio_psram_qpi_spi_khz());

    // Same adversarial family as the boot probe: patterns chosen so a slow
    // SIO2 bleeds the previous nibble into the sample near the page end.
    static const uint8_t pats[3][16] = {
        { 0x11, 0x22, 0x33, 0x44, 0x95, 0x95, 0xA2, 0xA3,
          0x74, 0x81, 0x82, 0x55, 0xAA, 0x00, 0xFF, 0x5A },
        { 0xFB, 0x04, 0xFB, 0x04, 0x40, 0xBF, 0x40, 0xBF,
          0x20, 0xDF, 0x20, 0xDF, 0x10, 0xEF, 0x10, 0xEF },
        { 0x5A, 0xA5, 0x5A, 0xA5, 0x95, 0x6A, 0x95, 0x6A,
          0x65, 0x9A, 0x65, 0x9A, 0x25, 0xDA, 0x25, 0xDA },
    };
    uint8_t rd[16];
    uint32_t total_reads = 0, errors = 0;

    // Phase 1: page-end reads at 64 addresses spread across all 8MB.
    for (uint32_t pg = 0; pg < 64; pg++) {
        uint32_t addr = pg * (128u * 1024u) + 1008u;  // last 16 bytes of a page
        const uint8_t *pat = pats[pg % 3];
        pio_psram_write(addr, pat, 16);
        for (uint32_t it = 0; it < iters; it++) {
            memset(rd, 0, sizeof(rd));
            pio_psram_read(addr, rd, 16);
            total_reads++;
            if (memcmp(pat, rd, 16) != 0) {
                if (errors < 8)
                    printf("[PSRAM]   stress mismatch @%06lX it %lu: "
                           "exp %02X%02X%02X%02X got %02X%02X%02X%02X\n",
                           (unsigned long)addr, (unsigned long)it,
                           pat[4], pat[5], pat[6], pat[7],
                           rd[4], rd[5], rd[6], rd[7]);
                errors++;
            }
        }
    }
    printf("[PSRAM] page-end stress: %lu reads, %lu errors\n",
           (unsigned long)total_reads, (unsigned long)errors);

    // Phase 2: sequential readback of the MP3 ring region, same shape as
    // refill_staging_buf (1KB at a time, wrapping at 32KB), written once.
    uint32_t ring_errors = 0;
    uint8_t *dbuf = dbg_buf();
    for (uint32_t off = 0; off < 32u * 1024u; off += DBG_BLOCK) {
        debug_fill_pattern(dbuf, 0xC000 + off);
        pio_psram_write(PIO_PSRAM_MP3_RING_BASE + off, dbuf, DBG_BLOCK);
    }
    for (uint32_t pass = 0; pass < iters; pass++) {
        for (uint32_t off = 0; off < 32u * 1024u; off += DBG_BLOCK) {
            memset(dbuf, 0, DBG_BLOCK);
            pio_psram_read(PIO_PSRAM_MP3_RING_BASE + off, dbuf, DBG_BLOCK);
            for (uint32_t i = 0; i < DBG_BLOCK; i++) {
                uint8_t exp = debug_pattern_byte(0xC000 + off, i);
                if (dbuf[i] != exp) {
                    if (ring_errors < 8)
                        printf("[PSRAM]   ring mismatch pass %lu @%06lX "
                               "exp %02X got %02X\n",
                               (unsigned long)pass,
                               (unsigned long)(PIO_PSRAM_MP3_RING_BASE + off + i),
                               exp, dbuf[i]);
                    ring_errors++;
                }
            }
        }
    }
    printf("[PSRAM] ring stress: %lu passes over 32KB, %lu bad bytes\n",
           (unsigned long)iters, (unsigned long)ring_errors);
    printf("[PSRAM] stress %s\n",
           (errors || ring_errors) ? "FAIL — residual read errors (audible crackle)"
                                   : "clean");
}
