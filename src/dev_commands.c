#include "console_gate.h"
#include "core1_stats.h"
#include "dev_commands.h"
#include "dev_ops.h"
#include "drivers/audio.h"
#include "drivers/display.h"
#include "drivers/kbd_i2c.h"
#include "drivers/keyboard.h"
#include "drivers/mp3_player.h"
#include "drivers/pio_psram.h"
#include "drivers/sdcard.h"
#include "drivers/sound.h"
#include "drivers/wifi.h"
#include "os/launcher.h"
#include "os/app_stack.h"
#include "os/os.h"
#include "os/core0_idle.h"
#include "os/xip_stats.h"
#include "os/pc_prof.h"
#include "os/serial_frame.h"
#include "os/splash.h"
#include "tusb.h"
#include "pico/stdio/driver.h"
#include "pico/stdio_usb.h"
#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/watchdog.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

// Sized for two absolute SD paths on one line (e.g. "unzip <zip> <dest>").
#define CMD_BUF_SIZE 300

static char s_cmd_buf[CMD_BUF_SIZE];
static size_t s_cmd_len = 0;
static bool s_cmd_ready = false;

static bool s_cmd_exit = false;
static bool s_cmd_usb = false;
static bool s_cmd_reboot = false;
static bool s_cmd_reboot_flash = false;
static bool s_cmd_reboot_ota = false;
static bool s_cmd_list = false;
static bool s_cmd_list_all = false;
static const char* s_pending_launch = NULL;

#define FILE_RECEIVE_CHUNK_SIZE 256
static char s_file_recv_path[128];
static sdfile_t s_file_recv_handle = NULL;
static uint32_t s_file_recv_expected = 0;
static uint32_t s_file_recv_received = 0;

// Base64 receive mode (UART-safe alternative to the CDC binary path).
// Chars arrive via stdio, so this works on any transport.  The host sends
// newline-terminated chunks of base64 (multiple of 4 chars); each chunk is
// decoded, flushed to SD, and acknowledged so the sender can pace itself —
// the UART RX FIFO is only 32 bytes and SD write latency can exceed it.
#define B64_RECV_TIMEOUT_US (10u * 1000u * 1000u)
#define B64_WRITE_BUF_SIZE 512
static bool s_b64_recv_active = false;
static char s_b64_group[4];
static uint32_t s_b64_group_len = 0;
static uint8_t s_b64_writebuf[B64_WRITE_BUF_SIZE];
static uint32_t s_b64_writelen = 0;
static uint32_t s_b64_hash = SERIAL_FNV1A_INIT; // running hash of a receive
static uint64_t s_b64_last_rx_us = 0;

bool dev_commands_transfer_active(void) {
    return console_gate_is_closed();
}

// The end of a transfer: the console is everyone's again, and a gap the gate
// left in the log is on record (console_gate.h).
static void transfer_end(void) {
    uint32_t dropped = console_gate_open();
    if (dropped)
        printf("[DEV] Console: %lu bytes of log output dropped during a "
               "transfer\n", (unsigned long)dropped);
}

static const char s_b64_enc[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int b64_decode_char(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static void b64_recv_char(int c);
static void b64_recv_abort(const char *why);

// The default alarm pool's free and lost slots (the `alarmpool` command).
// cmake/picodeck_pico_time.cmake compiles the real one into its patched
// pico_time; an SDK it leaves unpatched (2.3.1 on) gets this one, false.
__attribute__((weak)) bool picodeck_alarm_pool_census(int *free_slots,
                                                      int *lost_slots) {
    (void)free_slots;
    (void)lost_slots;
    return false;
}

// Map a dev-command key name to a BTN_* mask (0 = not a named button).
static uint32_t dev_key_name_to_mask(const char *key) {
    static const struct { const char *name; uint32_t mask; } map[] = {
        {"up", BTN_UP},        {"down", BTN_DOWN},
        {"left", BTN_LEFT},    {"right", BTN_RIGHT},
        {"enter", BTN_ENTER},  {"esc", BTN_ESC},
        {"menu", BTN_MENU},    {"backspace", BTN_BACKSPACE},
        {"tab", BTN_TAB},      {"del", BTN_DEL},
        {"shift", BTN_SHIFT},  {"ctrl", BTN_CTRL},
        {"alt", BTN_ALT},      {"sym", BTN_FN},
        {"fn", BTN_FN},
        {"f1", BTN_F1}, {"f2", BTN_F2}, {"f3", BTN_F3}, {"f4", BTN_F4},
        {"f5", BTN_F5}, {"f6", BTN_F6}, {"f7", BTN_F7}, {"f8", BTN_F8},
        {"f9", BTN_F9}, {"f10", BTN_MENU},
    };
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        if (strcmp(key, map[i].name) == 0)
            return map[i].mask;
    }
    return 0;
}

// Encode exactly n (1-3) bytes into 4 chars.
static void b64_encode_group(const uint8_t *in, uint32_t n, char out[4]) {
    uint32_t v = (uint32_t)in[0] << 16;
    if (n > 1) v |= (uint32_t)in[1] << 8;
    if (n > 2) v |= in[2];
    out[0] = s_b64_enc[(v >> 18) & 63];
    out[1] = s_b64_enc[(v >> 12) & 63];
    out[2] = (n > 1) ? s_b64_enc[(v >> 6) & 63] : '=';
    out[3] = (n > 2) ? s_b64_enc[v & 63] : '=';
}

static void dev_ls_callback(const sdcard_entry_t *entry, void *user) {
    (void)user;
    if (entry->name[0] == '.') return;
    if (entry->is_dir) {
        printf("  %s/\n", entry->name);
    } else {
        printf("  %s  (%u bytes)\n", entry->name, entry->size);
    }
}

void dev_commands_init(void) {
    s_cmd_buf[0] = '\0';
    s_cmd_len = 0;
    s_cmd_ready = false;
    s_cmd_exit = false;
    s_cmd_usb = false;
    s_cmd_reboot = false;
    s_cmd_reboot_flash = false;
    s_cmd_reboot_ota = false;
    s_cmd_list = false;
    s_cmd_list_all = false;
    s_pending_launch = NULL;
    s_file_recv_handle = NULL;
    s_file_recv_expected = 0;
    s_file_recv_received = 0;
    s_b64_recv_active = false;
    s_b64_group_len = 0;
    s_b64_writelen = 0;
}

const char* dev_commands_get_device(void) {
    const char* dev = getenv("SERIAL_DEVICE");
    return dev ? dev : "/dev/ttyACM0";
}

void dev_commands_poll(void) {
    if (s_file_recv_handle && s_b64_recv_active) {
        while (true) {
            int c = getchar_timeout_us(0);
            if (c == PICO_ERROR_TIMEOUT) break;
            b64_recv_char(c);
            if (!s_b64_recv_active) return;
        }
        if (time_us_64() - s_b64_last_rx_us > B64_RECV_TIMEOUT_US)
            b64_recv_abort("timeout");
        return;
    }

    if (s_file_recv_handle) {
        uint8_t buf[FILE_RECEIVE_CHUNK_SIZE];
        while (s_file_recv_received < s_file_recv_expected) {
            // Through the driver: it holds stdio_usb_mutex for the read, which
            // keeps TinyUSB's background task out (console_gate.h has the hazard
            // for the write side). No data, or the mutex busy, reads as < 0.
            int got = stdio_usb.in_chars((char *)buf, FILE_RECEIVE_CHUNK_SIZE);
            if (got <= 0) break;
            s_b64_last_rx_us = time_us_64();
            int written = sdcard_fwrite(s_file_recv_handle, buf, got);
            if (written < 0) {
                printf("[DEV] Error writing file\n");
                sdcard_fclose(s_file_recv_handle);
                s_file_recv_handle = NULL;
                transfer_end();
                return;
            }
            s_b64_hash = serial_fnv1a_update(s_b64_hash, buf, got);
            s_file_recv_received += written;
        }
        if (s_file_recv_received >= s_file_recv_expected) {
            sdcard_fclose(s_file_recv_handle);
            s_file_recv_handle = NULL;
            transfer_end();
            printf("[DEV] File received: %s (%lu bytes) fnv1a=%08lx\n",
                   s_file_recv_path, (unsigned long)s_file_recv_received,
                   (unsigned long)s_b64_hash);
        } else if (time_us_64() - s_b64_last_rx_us > B64_RECV_TIMEOUT_US) {
            // Stalled CDC transfer (e.g. host went away): abort so the console
            // does not stay captured in receive mode forever.
            sdcard_fclose(s_file_recv_handle);
            s_file_recv_handle = NULL;
            transfer_end();
            printf("[DEV] Error: put timed out at %lu/%lu bytes\n",
                   (unsigned long)s_file_recv_received,
                   (unsigned long)s_file_recv_expected);
        }
        return;
    }

    while (true) {
        int c = getchar_timeout_us(0);
        if (c == PICO_ERROR_TIMEOUT) break;

        if (c == '\n' || c == '\r') {
            if (s_cmd_len > 0) {
                s_cmd_buf[s_cmd_len] = '\0';
                s_cmd_ready = true;
                s_cmd_len = 0;
            }
        } else if (c == 0x03) {
            s_cmd_len = 0;
            s_cmd_buf[0] = '\0';
        } else if (s_cmd_len < CMD_BUF_SIZE - 1) {
            s_cmd_buf[s_cmd_len++] = (char)c;
            s_cmd_buf[s_cmd_len] = '\0';
        }
    }
}

// Write binary data over CDC for a transfer under CONSOLE_GATE_RAW. If hash is
// not NULL, the FNV-1a of what is sent is added to it. Returns false if the
// host has closed the port or stopped reading, which ends the transfer at once
// or within the driver's give-up time (console_gate_raw_write): an abandoned
// transfer must not hold the device, which then answers no dev command.
#define CDC_WRITE_CHUNK 1024u
static bool cdc_write_all(const uint8_t *data, uint32_t len, uint32_t *hash) {
    while (len > 0) {
        if (!tud_cdc_connected()) return false;
        uint32_t chunk = len < CDC_WRITE_CHUNK ? len : CDC_WRITE_CHUNK;
        if (hash) *hash = serial_fnv1a_update(*hash, data, chunk);
        uint64_t t0 = time_us_64();
        console_gate_raw_write(data, chunk);
        // A chunk this slow is one the driver gave up on and did not finish.
        if (time_us_64() - t0 >= PICO_STDIO_USB_STDOUT_TIMEOUT_US) return false;
        data += chunk;
        len -= chunk;
        watchdog_update();
    }
    return true;
}

static void b64_recv_abort(const char *why) {
    if (s_file_recv_handle) {
        sdcard_fclose(s_file_recv_handle);
        s_file_recv_handle = NULL;
    }
    s_b64_recv_active = false;
    transfer_end();
    printf("[DEV] Error: b64 receive aborted (%s) at %lu/%lu bytes\n",
           why, (unsigned long)s_file_recv_received,
           (unsigned long)s_file_recv_expected);
}

static void b64_recv_flush(void) {
    if (s_b64_writelen == 0) return;
    int written = sdcard_fwrite(s_file_recv_handle, s_b64_writebuf, s_b64_writelen);
    if (written < 0) {
        b64_recv_abort("SD write failed");
        return;
    }
    s_b64_hash = serial_fnv1a_update(s_b64_hash, s_b64_writebuf, s_b64_writelen);
    s_file_recv_received += (uint32_t)written;
    s_b64_writelen = 0;
}

static void b64_recv_char(int c) {
    s_b64_last_rx_us = time_us_64();
    if (c == 0x03) { // Ctrl-C abort from host
        b64_recv_abort("host abort");
        return;
    }
    if (c == '\n' || c == '\r') {
        if (c == '\n') {
            b64_recv_flush();
            if (!s_b64_recv_active) return; // flush may have aborted
            if (s_file_recv_received >= s_file_recv_expected) {
                sdcard_fclose(s_file_recv_handle);
                s_file_recv_handle = NULL;
                s_b64_recv_active = false;
                transfer_end();
                printf("[DEV] File received: %s (%lu bytes) fnv1a=%08lx\n",
                       s_file_recv_path, (unsigned long)s_file_recv_received,
                       (unsigned long)s_b64_hash);
            } else {
                printf("[DEV] ACK %lu\n", (unsigned long)s_file_recv_received);
            }
        }
        return;
    }
    if (c == '=') { // padding: decode the final partial group
        // "xx==" → 1 byte (handled on first '='; second '=' sees group_len 0),
        // "xxx=" → 2 bytes.
        if (s_b64_group_len >= 2 && s_b64_writelen + 2 <= B64_WRITE_BUF_SIZE) {
            uint32_t v = ((uint32_t)b64_decode_char(s_b64_group[0]) << 18) |
                         ((uint32_t)b64_decode_char(s_b64_group[1]) << 12);
            s_b64_writebuf[s_b64_writelen++] = (uint8_t)(v >> 16);
            if (s_b64_group_len == 3) {
                v |= (uint32_t)b64_decode_char(s_b64_group[2]) << 6;
                s_b64_writebuf[s_b64_writelen++] = (uint8_t)(v >> 8);
            }
        }
        s_b64_group_len = 0;
        return;
    }
    int v = b64_decode_char((char)c);
    if (v < 0) {
        b64_recv_abort("invalid base64 char");
        return;
    }
    s_b64_group[s_b64_group_len++] = (char)c;
    if (s_b64_group_len == 4) {
        s_b64_group_len = 0;
        uint32_t g = ((uint32_t)b64_decode_char(s_b64_group[0]) << 18) |
                     ((uint32_t)b64_decode_char(s_b64_group[1]) << 12) |
                     ((uint32_t)b64_decode_char(s_b64_group[2]) << 6) |
                     (uint32_t)b64_decode_char(s_b64_group[3]);
        if (s_b64_writelen + 3 > B64_WRITE_BUF_SIZE) {
            b64_recv_abort("chunk exceeds buffer");
            return;
        }
        s_b64_writebuf[s_b64_writelen++] = (uint8_t)(g >> 16);
        s_b64_writebuf[s_b64_writelen++] = (uint8_t)(g >> 8);
        s_b64_writebuf[s_b64_writelen++] = (uint8_t)g;
    }
}

void dev_commands_send_screenshot(void) {
    if (!tud_cdc_connected()) {
        printf("[DEV] Error: CDC not connected — use screenshot64\n");
        return;
    }
    const uint16_t *fb = display_get_screen_buffer();

    uint8_t header[SCRN_HEADER_LEN];
    scrn_frame_header(header, FB_WIDTH, FB_HEIGHT, SCRN_FORMAT_RGB565,
                      SCRN_FLAG_TRAILER);

    // Flush stdio so our raw CDC writes follow what printf has queued
    stdio_flush();

    console_gate_close(CONSOLE_GATE_RAW);
    uint32_t hash = SERIAL_FNV1A_INIT;
    bool ok = cdc_write_all(header, sizeof(header), NULL) &&
              cdc_write_all((const uint8_t *)fb,
                            FB_WIDTH * FB_HEIGHT * sizeof(uint16_t), &hash);
    if (ok) {
        char trailer[SCRN_TRAILER_LEN + 1];
        scrn_frame_trailer(trailer, hash);
        ok = cdc_write_all((const uint8_t *)trailer, SCRN_TRAILER_LEN, NULL);
    }
    console_gate_raw_flush();
    transfer_end();
    if (!ok)
        printf("[DEV] Error: CDC write stalled\n");
}

// Stream a memory buffer as '~'-prefixed base64 lines over stdio, under
// CONSOLE_GATE_LINES. The '~' prefix lets the host discard any other line that
// reaches it; the FNV-1a in the caller's footer catches what the gate cannot
// stop (a crash report).
#define B64_LINE_RAW 72 // 96 base64 chars per line
static uint32_t b64_send_buf(const uint8_t *data, uint32_t len, uint32_t hash) {
    char line[(B64_LINE_RAW / 3) * 4 + 1];
    for (uint32_t off = 0; off < len; off += B64_LINE_RAW) {
        uint32_t n = len - off < B64_LINE_RAW ? len - off : B64_LINE_RAW;
        uint32_t li = 0;
        for (uint32_t i = 0; i < n; i += 3) {
            uint32_t g = n - i < 3 ? n - i : 3;
            b64_encode_group(data + off + i, g, &line[li]);
            li += 4;
        }
        line[li] = '\0';
        printf("~%s\n", line);
        hash = serial_fnv1a_update(hash, data + off, n);
        watchdog_update();
    }
    return hash;
}

// Stream a file as base64 over stdio (any transport).  Single pass: the
// FNV-1a of the raw bytes is only known at the end, so it rides the footer.
static void dev_send_file_b64(const char *path) {
    sdfile_t f = sdcard_fopen(path, "rb");
    if (!f) {
        printf("[DEV] Failed to open file: %s\n", path);
        return;
    }
    console_gate_close(CONSOLE_GATE_LINES);
    int size = sdcard_fsize_handle(f);
    printf("[DEV] B64 size=%d\n", size);
    uint8_t buf[B64_LINE_RAW * 4];
    uint32_t hash = SERIAL_FNV1A_INIT;
    int nread;
    while ((nread = sdcard_fread(f, buf, sizeof(buf))) > 0)
        hash = b64_send_buf(buf, (uint32_t)nread, hash);
    sdcard_fclose(f);
    printf("[DEV] B64_END fnv1a=%08lx path=%s\n", (unsigned long)hash, path);
    transfer_end();
}

// `lognoise <ms>`: log from Core 1 and from a Core 0 interrupt every <ms>, the
// two places the console gate has to stop (tests/e2e/test_screenshot_integrity.py).
// `lognoise off` stops it (a reboot does too); a bare `lognoise` reports it and
// the gate's counts.
static volatile uint32_t s_noise_ms = 0;
static repeating_timer_t s_noise_timer;
static bool s_noise_timer_on = false;

static bool noise_timer_cb(repeating_timer_t *t) {
    (void)t;
    printf("[NOISE] core 0 interrupt\n");
    return true;
}

void dev_commands_core1_poll(void) {
    static uint32_t s_last_ms = 0;
    uint32_t ms = s_noise_ms;
    if (!ms) return;
    uint32_t now = to_ms_since_boot(get_absolute_time());
    if (now - s_last_ms < ms) return;
    s_last_ms = now;
    // Two calls, as a line built piecemeal: the print mutex does not hold
    // across them, so this is what can land inside a line-framed transfer.
    printf("[NOISE] core 1 ");
    printf("%lu\n", (unsigned long)now);
}

static void dev_lognoise(const char *arg) {
    if (arg[0]) {
        if (s_noise_timer_on) {
            cancel_repeating_timer(&s_noise_timer);
            s_noise_timer_on = false;
        }
        uint32_t ms = strcmp(arg, "off") == 0
                          ? 0u : (uint32_t)strtoul(arg, NULL, 10);
        s_noise_ms = ms;
        if (ms)
            s_noise_timer_on = add_repeating_timer_ms(-(int32_t)ms, noise_timer_cb,
                                                      NULL, &s_noise_timer);
    }
    uint32_t transfers, dropped;
    console_gate_stats(&transfers, &dropped);
    printf("[DEV] LogNoise: period_ms=%lu transfers=%lu dropped_bytes=%lu\n",
           (unsigned long)s_noise_ms, (unsigned long)transfers,
           (unsigned long)dropped);
}

// `xipstat [reset|off|prio core0|prio none|mp3idle on|mp3idle off]`: the
// XIP cache and the XIP ports' contention counters over a window
// (os/xip_stats.h; issue #28), with Core 0's pacing idle time
// (os/core0_idle.h) and when the MP3 decoder decoded (mp3_sched.h) since
// the reset. "reset" starts a window, which ends by itself when the app it
// measured exits; a bare `xipstat` reports it (and keeps it running).
// "prio" and "mp3idle" are switches for A/B runs ("prio core0" lasts until
// the next app exits: the launcher clears it). One line of key=value
// integers, as audiostat.
static void dev_xipstat(const char *arg) {
    if (strcmp(arg, "reset") == 0) {
        core0_idle_reset_stats();
        mp3_player_reset_sched_stats();
        xip_stats_start();
    } else if (strcmp(arg, "mp3idle on") == 0)
        mp3_player_set_decode_ahead(true);
    else if (strcmp(arg, "mp3idle off") == 0)
        mp3_player_set_decode_ahead(false);
    else if (strcmp(arg, "off") == 0)
        xip_stats_stop();
    else if (strcmp(arg, "prio core0") == 0)
        xip_stats_set_core0_priority(true);
    else if (strcmp(arg, "prio none") == 0)
        xip_stats_set_core0_priority(false);
    else if (arg[0]) {
        printf("[DEV] Error: xipstat [reset|off|prio core0|prio none|"
               "mp3idle on|mp3idle off]\n");
        return;
    }
    xip_stats_t x;
    xip_stats_get(&x);
    uint32_t idle_windows;
    uint64_t idle_us;
    core0_idle_stats(&idle_windows, &idle_us);
    mp3_sched_stats_t m;
    mp3_player_get_sched_stats(&m);
    uint64_t miss = x.accesses - x.hits;
    unsigned long hit_pm = x.accesses
        ? (unsigned long)(x.hits * 1000u / x.accesses) : 0ul;
    printf("[DEV] XIP: window_ms=%lu running=%d frozen=%d saturated=%d "
           "acc=%llu hit=%llu miss=%llu hit_pm=%lu stall0=%llu stall1=%llu "
           "contested0=%llu contested1=%llu prio0=%d idle_windows=%lu "
           "idle_ms=%lu mp3_low_frames=%lu mp3_idle_frames=%lu "
           "mp3_overran=%lu mp3_frame_us=%lu mp3idle=%d mp3_dec_us=%lu "
           "mp3_syn_us=%lu mp3_out_us=%lu sys_khz=%lu\n",
           (unsigned long)x.window_ms, x.running, x.frozen, x.saturated,
           (unsigned long long)x.accesses, (unsigned long long)x.hits,
           (unsigned long long)miss, hit_pm,
           (unsigned long long)x.stall[0], (unsigned long long)x.stall[1],
           (unsigned long long)x.contested[0],
           (unsigned long long)x.contested[1],
           xip_stats_core0_priority(), (unsigned long)idle_windows,
           (unsigned long)(idle_us / 1000u), (unsigned long)m.low_frames,
           (unsigned long)m.idle_frames, (unsigned long)m.overran,
           (unsigned long)m.frame_us, m.decode_ahead,
           (unsigned long)m.dec_us, (unsigned long)m.syn_us,
           (unsigned long)m.out_us,
           (unsigned long)(clock_get_hz(clk_sys) / 1000u));
}

// Executes the line in s_cmd_buf. Runs on an app stack (see
// dev_commands_process): the file commands below reach FatFs and miniz,
// whose frames do not fit the 4 KB main stack.
static void dev_command_run(void *arg) {
    (void)arg;
    printf("[DEV] Command: %s\n", s_cmd_buf);

    if (strcmp(s_cmd_buf, "ping") == 0) {
        printf("[DEV] pong\n");
    } else if (strcmp(s_cmd_buf, "alarmpool") == 0) {
        // Issue #58: a slot that is in none of the default pool's lists
        // (lost) only comes from a leak; tests/e2e/test_kbd_hw.py checks it.
        // A slot Core 1 is adding at that instant reads as lost for a
        // moment, so a nonzero count is read again (up to three reads).
        int free_slots = 0, lost_slots = 0;
        bool ok = false;
        for (int i = 0; i < 3; i++) {
            if (i)
                busy_wait_us(500);
            ok = picodeck_alarm_pool_census(&free_slots, &lost_slots);
            if (!ok || lost_slots == 0)
                break;
        }
        if (ok)
            printf("[DEV] AlarmPool: free=%d lost=%d\n", free_slots, lost_slots);
        else
            printf("[DEV] AlarmPool: unavailable (pico_time not patched)\n");
    } else if (strcmp(s_cmd_buf, "stack") == 0) {
        // Peak use of Core 0's 4 KB main stack since boot, and of the app
        // runtime's PSP stack since launch (when an app is running).
        printf("[DEV] Stack: msp_peak=%lu msp_size=%lu core1_peak=%lu "
               "core1_size=%lu",
               (unsigned long)app_stack_msp_high_water(),
               (unsigned long)PICO_STACK_SIZE,
               (unsigned long)app_stack_core1_high_water(),
               (unsigned long)app_stack_core1_size());
        // At the launcher this command itself runs on the OS command stack;
        // report the app runtime's stack only while an app owns the PSP.
        uint8_t *base = g_app_stack_base;
        uint32_t size = g_app_stack_size;
        if (base && g_app_stack_owner != APP_STACK_OS)
            printf(" app=%s app_peak=%lu app_size=%lu",
                   g_app_stack_owner == APP_STACK_LUA ? "lua" : "native",
                   (unsigned long)app_stack_high_water(base, size),
                   (unsigned long)size);
        // Peak of the last launcher-side command on the OS command stack.
        printf(" os_cmd_peak=%lu os_cmd_size=%lu\n",
               (unsigned long)app_stack_os_last_peak(),
               (unsigned long)APP_STACK_OS_SIZE);
    } else if (strcmp(s_cmd_buf, "ver") == 0) {
        printf("[DEV] PicoDeck build %s %s\n", __DATE__, __TIME__);
    } else if (strcmp(s_cmd_buf, "status") == 0) {
        static const char *wifi_names[] = {
            "disconnected", "connecting", "connected", "failed", "online"};
        wifi_status_t ws = wifi_get_status();
        const char *wifi_str =
            (ws <= WIFI_STATUS_ONLINE) ? wifi_names[ws] : "?";
        const char *app = launcher_get_running_app_name();
        printf("[DEV] Status: app=%s app_uptime_ms=%lu uptime_ms=%lu "
               "wifi=%s sd=%s battery=%d\n",
               (app && app[0]) ? app : "launcher",
               (unsigned long)launcher_get_app_uptime_ms(),
               (unsigned long)to_ms_since_boot(get_absolute_time()),
               wifi_str,
               sdcard_is_mounted() ? "mounted" : "absent",
               kbd_get_battery_percent());
    } else if (strcmp(s_cmd_buf, "splash") == 0) {
        // The boot splash's measurements (src/os/splash.c;
        // tests/e2e/test_splash_hw.py).
        char line[256];
        splash_stats_format(line, sizeof line);
        printf("[DEV] Splash: %s\n", line);
    } else if (strcmp(s_cmd_buf, "exit") == 0) {
        char reply[DEV_OP_REPLY_MAX];
        dev_op_exit(reply, sizeof(reply));
        printf("[DEV] %s\n", reply);
    } else if (strcmp(s_cmd_buf, "usb") == 0) {
        s_cmd_usb = true;
        printf("[DEV] USB MSC mode starting (USB re-enumerates; serial drops until exit)\n");
    } else if (strcmp(s_cmd_buf, "reboot") == 0) {
        s_cmd_reboot = true;
    } else if (strcmp(s_cmd_buf, "reboot-flash") == 0) {
        s_cmd_reboot_flash = true;
    } else if (strcmp(s_cmd_buf, "reboot-ota") == 0) {
        // Apply a staged /system/update.bin (+ .sha256/.sig): the launcher
        // sets the OTA request token and reboots.  While an app runs it is
        // dropped (lua_bridge / native sys_poll clear it), not deferred.
        s_cmd_reboot_ota = true;
        printf("[DEV] reboot-ota requested\n");
    } else if (strncmp(s_cmd_buf, "launch ", 7) == 0) {
        s_pending_launch = s_cmd_buf + 7;
        s_cmd_exit = true;  // Exit current app first
    } else if (strcmp(s_cmd_buf, "list") == 0 ||
               strcmp(s_cmd_buf, "list all") == 0) {
        s_cmd_list_all = s_cmd_buf[4] != '\0';
        s_cmd_list = true;
    } else if (strcmp(s_cmd_buf, "screenshot") == 0) {
        dev_commands_send_screenshot();
    } else if (strcmp(s_cmd_buf, "psram") == 0) {
        pio_psram_debug_test(false);
    } else if (strcmp(s_cmd_buf, "psram full") == 0) {
        pio_psram_debug_test(true);
    } else if (strncmp(s_cmd_buf, "psram stress", 12) == 0) {
        uint32_t iters = 200;
        if (s_cmd_buf[12] == ' ') {
            uint32_t n = (uint32_t)atoi(s_cmd_buf + 13);
            if (n > 0) iters = n;
        }
        pio_psram_stress_test(iters);
    } else if (strcmp(s_cmd_buf, "mp3stats") == 0) {
        uint32_t d[11];
        mp3_player_get_diag(d);
        printf("[DEV] mp3 @%lums: underrun=%lu upd=%lu skip_mtx=%lu refill=%lu "
               "ring_empty=%lu dec_run=%lu frames=%lu mad_err=%lu sd_fail=%lu "
               "max_us=%lu avg_us=%lu (reset)\n",
               (unsigned long)to_ms_since_boot(get_absolute_time()),
               d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7], d[8], d[9],
               d[1] ? (unsigned long)(d[10] / d[1]) : 0ul);
        mp3_player_reset_diag();
        // A video's seeks: restarts of its playing audio (mp3_player.h).
        mp3_fed_restart_stats_t rs;
        mp3_player_fed_restart_stats(&rs, true);
        printf("[DEV] mp3 restarts: restarts=%lu fallbacks=%lu gap_us=%lu "
               "gap_max_us=%lu preroll_max_us=%lu margin_min_us=%lu late=%lu (reset)\n",
               (unsigned long)rs.restarts, (unsigned long)rs.fallbacks,
               (unsigned long)rs.gap_us, (unsigned long)rs.gap_max_us,
               (unsigned long)rs.preroll_max_us, (unsigned long)rs.margin_min_us,
               (unsigned long)rs.late);
    } else if (strcmp(s_cmd_buf, "audiostat") == 0 ||
               strcmp(s_cmd_buf, "audiostat reset") == 0) {
        // Core 1's tick cost, the refill interrupt and the stream's
        // underruns (tests/e2e/test_audio_hw.py); dev_ops.c, shared with
        // the simulator's dev_command RPC.
        char reply[DEV_OP_REPLY_MAX];
        dev_op_audiostat(strcmp(s_cmd_buf, "audiostat reset") == 0, reply,
                         sizeof(reply));
        printf("[DEV] %s\n", reply);
    } else if (strcmp(s_cmd_buf, "kbdstat") == 0 ||
               strcmp(s_cmd_buf, "kbdstat reset") == 0 ||
               strcmp(s_cmd_buf, "kbdstat fault") == 0) {
        // The keyboard bus engine's counters (drivers/kbd_i2c.c): "reset"
        // zeroes them first, "fault" makes the next transaction go
        // unanswered so the recovery path runs (tests/e2e/test_kbd_hw.py).
        // sys_khz is the clock right now: the clock-change test reads it.
        // A KbdFail line follows for the first and the latest failure of
        // the latest failure streak, if there was one ("reset" clears them,
        // except during a streak, and `kbdstat log`'s trace). ago_ms wraps
        // after 71.6 min.
        if (strcmp(s_cmd_buf, "kbdstat reset") == 0)
            kbd_i2c_reset_stats();
        else if (strcmp(s_cmd_buf, "kbdstat fault") == 0)
            kbd_i2c_inject_fault();
        kbd_i2c_stats_t k;
        kbd_i2c_get_stats(&k);
        printf("[DEV] Kbd: state=%s window_ms=%lu reads=%lu items=%lu "
               "dropped=%lu bat_reads=%lu bl_writes=%lu errors=%lu "
               "recoveries=%lu max_gap_us=%lu max_read_us=%lu isr_us=%lu "
               "interval_us=%lu battery=%d sys_khz=%lu aborts=%lu "
               "timeouts=%lu short_reads=%lu lost_alarms=%lu cuts=%lu\n",
               k.state, (unsigned long)k.window_ms, (unsigned long)k.reads,
               (unsigned long)k.items, (unsigned long)k.dropped,
               (unsigned long)k.bat_reads, (unsigned long)k.bl_writes,
               (unsigned long)k.errors, (unsigned long)k.recoveries,
               (unsigned long)k.max_gap_us, (unsigned long)k.max_read_us,
               (unsigned long)k.isr_us, (unsigned long)k.interval_us,
               k.battery, (unsigned long)(clock_get_hz(clk_sys) / 1000u),
               (unsigned long)k.aborts, (unsigned long)k.timeouts,
               (unsigned long)k.short_reads, (unsigned long)k.lost_alarms,
               (unsigned long)k.cuts);
        static const char *const k_why[] = {"abort", "timeout", "short",
                                            "lost", "?", "?", "?", "?"};
        kbd_i2c_fail_t ff[2];
        kbd_i2c_get_failures(&ff[0], &ff[1]);
        uint32_t now = time_us_32();
        for (int i = 0; i < 2; i++) {
            const kbd_i2c_fail_t *f = &ff[i];
            if (!f->at_us)
                continue;
            printf("[DEV] KbdFail: which=%s ago_ms=%lu why=%s state=%s "
                   "job=%u abrt_src=0x%lx raw_intr=0x%lx status=0x%lx "
                   "txflr=%u rxflr=%u sda=%u scl=%u sys_mhz=%u\n",
                   i ? "last" : "first",
                   (unsigned long)((now - f->at_us) / 1000u), k_why[f->why],
                   kbd_i2c_state_name(f->state), f->job,
                   (unsigned long)f->abrt_src, (unsigned long)f->raw_intr,
                   (unsigned long)f->status, f->txflr, f->rxflr,
                   f->lines & 1u, (f->lines >> 1) & 1u, f->sys_mhz);
        }
    } else if (strcmp(s_cmd_buf, "kbdstat log") == 0) {
        // The bus engine's recent events, oldest first (kbd_i2c_event_t).
        kbd_i2c_event_t ev[KBD_I2C_TRACE_LEN];
        int n = kbd_i2c_get_trace(ev, KBD_I2C_TRACE_LEN);
        uint32_t now = time_us_32();
        for (int i = 0; i < n; i++)
            printf("[DEV] KbdLog: ago_ms=%lu ev=%c state=%s arg=%u\n",
                   (unsigned long)((now - ev[i].t_us) / 1000u), ev[i].ev,
                   kbd_i2c_state_name(ev[i].state), ev[i].arg);
        printf("[DEV] KbdLog: %d events\n", n);
    } else if (strcmp(s_cmd_buf, "kbdstat hw") == 0) {
        // The I2C block, the keyboard pins and the engine's alarm, with
        // clk_sys measured by the frequency counter (kbd_i2c_hw_t).
        kbd_i2c_hw_t hw;
        kbd_i2c_get_hw(&hw);
        printf("[DEV] KbdHw: fc_sys_khz=%lu hcnt=%lu lcnt=%lu tar=0x%lx "
               "en_status=0x%lx status=0x%lx raw_intr=0x%lx intr_mask=0x%lx "
               "pad_sda=0x%lx pad_scl=0x%lx ctrl_sda=0x%lx ctrl_scl=0x%lx "
               "sda=%u scl=%u irq=%d alarm=%d armed=%d due_in_us=%ld\n",
               (unsigned long)frequency_count_khz(CLOCKS_FC0_SRC_VALUE_CLK_SYS),
               (unsigned long)hw.hcnt, (unsigned long)hw.lcnt,
               (unsigned long)hw.tar, (unsigned long)hw.enable_status,
               (unsigned long)hw.status, (unsigned long)hw.raw_intr,
               (unsigned long)hw.intr_mask, (unsigned long)hw.pad_sda,
               (unsigned long)hw.pad_scl, (unsigned long)hw.ctrl_sda,
               (unsigned long)hw.ctrl_scl, hw.lines & 1u,
               (hw.lines >> 1) & 1u, hw.irq_on, hw.alarm_num, hw.armed,
               (long)hw.due_in_us);
    } else if (strncmp(s_cmd_buf, "mp3bench ", 9) == 0) {
        // `mp3bench <path> [frames] [flags]`: a frame's decode cost, by
        // phase (issue #28; mp3_player_bench, which says what flags do).
        char path[128];
        const char *a = s_cmd_buf + 9;
        size_t n = strcspn(a, " ");
        if (n > 0 && n < sizeof path) {
            memcpy(path, a, n);
            path[n] = '\0';
            char *end;
            unsigned long frames = strtoul(a + n, &end, 10);
            unsigned long flags = strtoul(end, NULL, 10);
            mp3_player_bench(path, frames ? (uint32_t)frames : 200u,
                             (uint32_t)flags);
        }
    } else if (strcmp(s_cmd_buf, "prof") == 0 ||
               strncmp(s_cmd_buf, "prof ", 5) == 0) {
        pc_prof_command(s_cmd_buf + 4);
    } else if (strcmp(s_cmd_buf, "lognoise") == 0 ||
               strncmp(s_cmd_buf, "lognoise ", 9) == 0) {
        dev_lognoise(s_cmd_buf[8] ? s_cmd_buf + 9 : "");
    } else if (strcmp(s_cmd_buf, "xipstat") == 0 ||
               strncmp(s_cmd_buf, "xipstat ", 8) == 0) {
        dev_xipstat(s_cmd_buf[7] ? s_cmd_buf + 8 : "");
    } else if (strncmp(s_cmd_buf, "keypress ", 9) == 0 ||
               strncmp(s_cmd_buf, "keydown ", 8) == 0 ||
               strncmp(s_cmd_buf, "keyup ", 6) == 0) {
        bool is_down = (strncmp(s_cmd_buf, "keydown ", 8) == 0);
        bool is_up   = (strncmp(s_cmd_buf, "keyup ", 6) == 0);
        const char *key = s_cmd_buf + (is_down ? 8 : is_up ? 6 : 9);
        uint32_t buttons = dev_key_name_to_mask(key);
        char ch = 0;

        if (!buttons) {
            if (!is_down && !is_up && strlen(key) == 1 &&
                key[0] >= 0x20 && key[0] < 0x7F) {
                ch = key[0];
            } else {
                printf("[DEV] Unknown key: %s\n", key);
                return;
            }
        }

        if (is_down) {
            kbd_hold_buttons(buttons);
        } else if (is_up) {
            kbd_release_buttons(buttons);
        } else {
            if (buttons) kbd_inject_buttons(buttons);
            if (ch) kbd_inject_char(ch);
        }
        printf("[DEV] Key injected: %s\n", key);
    } else if (strncmp(s_cmd_buf, "pad ", 4) == 0 ||
               strcmp(s_cmd_buf, "pad") == 0) {
        char reply[DEV_OP_REPLY_MAX];
        dev_op_pad(s_cmd_buf + 3, reply, sizeof(reply));
        printf("[DEV] %s\n", reply);
    } else if (strncmp(s_cmd_buf, "bt ", 3) == 0 ||
               strcmp(s_cmd_buf, "bt") == 0) {
        // Bluetooth gamepads (drivers/bt_pad.h): dev_ops.c, shared with
        // the simulator's dev_command RPC.
        char reply[DEV_OP_REPLY_MAX];
        dev_op_bt(s_cmd_buf + 2, reply, sizeof(reply));
        printf("[DEV] %s\n", reply);
    } else if (strncmp(s_cmd_buf, "put ", 4) == 0) {
        const char *args = s_cmd_buf + 4;
        uint32_t size = 0;
        char *size_str = strchr(args, ' ');
        if (size_str) {
            *size_str = '\0';
            size = atoi(size_str + 1);
        }
        if (size == 0 || strlen(args) == 0) {
            printf("[DEV] Usage: put <path> <size>\n");
            return;
        }
        strncpy(s_file_recv_path, args, sizeof(s_file_recv_path) - 1);
        s_file_recv_path[sizeof(s_file_recv_path) - 1] = '\0';
        s_file_recv_handle = sdcard_fopen(s_file_recv_path, "wb");
        if (!s_file_recv_handle) {
            printf("[DEV] Failed to open file for writing: %s\n", args);
            return;
        }
        s_file_recv_expected = size;
        s_file_recv_received = 0;
        s_b64_hash = SERIAL_FNV1A_INIT;
        s_b64_last_rx_us = time_us_64();
        console_gate_close(CONSOLE_GATE_LINES);
        printf("[DEV] Ready to receive %lu bytes for %s\n", (unsigned long)size, args);
    } else if (strncmp(s_cmd_buf, "putb64 ", 7) == 0) {
        char *args = s_cmd_buf + 7;
        uint32_t size = 0;
        char *size_str = strchr(args, ' ');
        if (size_str) {
            *size_str = '\0';
            size = atoi(size_str + 1);
        }
        if (size == 0 || strlen(args) == 0) {
            printf("[DEV] Usage: putb64 <path> <raw_size>\n");
            return;
        }
        strncpy(s_file_recv_path, args, sizeof(s_file_recv_path) - 1);
        s_file_recv_path[sizeof(s_file_recv_path) - 1] = '\0';
        s_file_recv_handle = sdcard_fopen(s_file_recv_path, "wb");
        if (!s_file_recv_handle) {
            printf("[DEV] Failed to open file for writing: %s\n", args);
            return;
        }
        s_file_recv_expected = size;
        s_file_recv_received = 0;
        s_b64_recv_active = true;
        s_b64_group_len = 0;
        s_b64_writelen = 0;
        s_b64_hash = SERIAL_FNV1A_INIT;
        s_b64_last_rx_us = time_us_64();
        console_gate_close(CONSOLE_GATE_LINES);
        printf("[DEV] Ready B64 %lu bytes for %s (chunk<=%u raw, newline-terminated, await ACK)\n",
               (unsigned long)size, args, (unsigned)B64_WRITE_BUF_SIZE);
    } else if (strncmp(s_cmd_buf, "getb64 ", 7) == 0) {
        dev_send_file_b64(s_cmd_buf + 7);
    } else if (strcmp(s_cmd_buf, "screenshot64") == 0) {
        const uint8_t *fb = (const uint8_t *)display_get_screen_buffer();
        console_gate_close(CONSOLE_GATE_LINES);
        printf("[DEV] SCRN64 w=%u h=%u fmt=565\n", (unsigned)FB_WIDTH, (unsigned)FB_HEIGHT);
        uint32_t hash = b64_send_buf(fb, FB_WIDTH * FB_HEIGHT * 2u, SERIAL_FNV1A_INIT);
        printf("[DEV] SCRN64_END fnv1a=%08lx\n", (unsigned long)hash);
        transfer_end();
    } else if (strcmp(s_cmd_buf, "crashlog") == 0) {
        sdfile_t f = sdcard_fopen("/system/crashlog.txt", "rb");
        if (!f) {
            printf("[DEV] No crash log\n");
        } else {
            printf("[DEV] CRASHLOG BEGIN\n");
            char buf[257];
            int nread;
            while ((nread = sdcard_fread(f, buf, sizeof(buf) - 1)) > 0) {
                buf[nread] = '\0';
                printf("%s", buf);
                watchdog_update();
            }
            sdcard_fclose(f);
            printf("\n[DEV] CRASHLOG END\n");
        }
    } else if (strcmp(s_cmd_buf, "crashlog clear") == 0) {
        sdcard_delete("/system/crashlog.txt");
        printf("[DEV] Crash log cleared\n");
    } else if (strncmp(s_cmd_buf, "get ", 4) == 0) {
        const char *path = s_cmd_buf + 4;
        if (strlen(path) == 0) {
            printf("[DEV] Usage: get <path>\n");
            return;
        }
        if (!tud_cdc_connected()) {
            printf("[DEV] Error: CDC not connected — use getb64\n");
            return;
        }
        sdfile_t f = sdcard_fopen(path, "rb");
        if (!f) {
            printf("[DEV] Failed to open file: %s\n", path);
            return;
        }
        int size = sdcard_fsize_handle(f);
        printf("FILE_DATA:\nSIZE:%d\n", size);
        stdio_flush();

        console_gate_close(CONSOLE_GATE_RAW);
        uint8_t buf[256];
        int read;
        bool ok = true;
        uint32_t hash = SERIAL_FNV1A_INIT;
        while (ok && (read = sdcard_fread(f, buf, sizeof(buf))) > 0)
            ok = cdc_write_all(buf, read, &hash);
        sdcard_fclose(f);
        console_gate_raw_flush();
        transfer_end();
        if (!ok)
            printf("[DEV] Error: CDC write stalled\n");
        printf("[DEV] File sent: %s (%d bytes) fnv1a=%08lx\n", path, size,
               (unsigned long)hash);
    } else if (strncmp(s_cmd_buf, "mkdir ", 6) == 0) {
        const char *path = s_cmd_buf + 6;
        if (path[0] != '/') {
            printf("[DEV] Usage: mkdir /absolute/path\n");
            return;
        }
        // Recursive create ("mkdir -p"): make each component in turn.
        char tmp[CMD_BUF_SIZE];
        strncpy(tmp, path, sizeof(tmp) - 1);
        tmp[sizeof(tmp) - 1] = '\0';
        size_t len = strlen(tmp);
        while (len > 1 && tmp[len - 1] == '/')
            tmp[--len] = '\0';
        bool ok = true;
        for (char *p = tmp + 1; *p && ok; p++) {
            if (*p == '/') {
                *p = '\0';
                ok = sdcard_mkdir(tmp);
                *p = '/';
            }
        }
        if (ok) ok = sdcard_mkdir(tmp);
        if (ok)
            printf("[DEV] Created: %s\n", tmp);
        else
            printf("[DEV] Error: mkdir failed: %s\n", tmp);
    } else if (strncmp(s_cmd_buf, "ls ", 3) == 0) {
        const char *path = s_cmd_buf + 3;
        if (strlen(path) == 0) {
            path = "/";
        }
        int count = sdcard_list_dir(path, dev_ls_callback, NULL);
        printf("[DEV] %d items in %s\n", count < 0 ? 0 : count, path);
    } else if (strncmp(s_cmd_buf, "unzip ", 6) == 0) {
        // unzip <zip> <dest> — extract an archive on-device (push_app).
        char reply[DEV_OP_REPLY_MAX];
        dev_op_unzip(s_cmd_buf + 6, reply, sizeof(reply));
        printf("[DEV] %s\n", reply);
    } else if (strncmp(s_cmd_buf, "rm ", 3) == 0) {
        char reply[DEV_OP_REPLY_MAX];
        dev_op_rm(s_cmd_buf + 3, reply, sizeof(reply));
        printf("[DEV] %s\n", reply);
    } else if (strncmp(s_cmd_buf, "mv ", 3) == 0) {
        char reply[DEV_OP_REPLY_MAX];
        dev_op_mv(s_cmd_buf + 3, reply, sizeof(reply));
        printf("[DEV] %s\n", reply);
    } else if (strcmp(s_cmd_buf, "help") == 0) {
        printf("[DEV] Available commands:\n");
        printf("[DEV]   ping           - Check device is responding\n");
        printf("[DEV]   ver            - Show firmware build date/time\n");
        printf("[DEV]   stack          - Main, app and OS-command stack peak use\n");
        printf("[DEV]   splash         - The boot splash's state, frame rate and timings\n");
        printf("[DEV]   alarmpool      - Default alarm pool: free and lost slots (lost re-read 3x)\n");
        printf("[DEV]   kbdstat [reset|fault|log|hw] - Keyboard bus engine counters\n");
        printf("[DEV]   xipstat [reset|off|prio core0|prio none|mp3idle on|mp3idle off] - XIP cache, bus contention and MP3 decode timing\n");
        printf("[DEV]   lognoise [<ms>|off] - Log from Core 1 and a Core 0 interrupt every <ms>; console gate counts\n");
        printf("[DEV]   mp3bench <path> [frames] [flags] - MP3 decode cost per frame, by phase (at the launcher)\n");
        printf("[DEV]   exit           - Signal current app to exit (error if none)\n");
        printf("[DEV]   usb            - Enable USB storage mode\n");
        printf("[DEV]   reboot         - Reboot device\n");
        printf("[DEV]   reboot-flash   - Reboot to BOOTSEL for flashing\n");
        printf("[DEV]   reboot-ota     - Apply staged /system/update.bin (needs .sha256 + .sig)\n");
        printf("[DEV]   launch <arg>   - Launch app by ID or name (also hidden apps under /apps/.test, /apps/.dev)\n");
        printf("[DEV]   list [all]     - List installed apps ('all' adds the hidden ones)\n");
        printf("[DEV]   screenshot     - Capture screen (binary, ends with its FNV-1a)\n");
        printf("[DEV]   keypress <key> - Inject keypress\n");
        printf("[DEV]   keydown <key> / keyup <key> - Hold/release a key (chords)\n");
        printf("[DEV]   pad <btns|none|off> [ms] - Test gamepad: e.g. 'pad up+a', 'pad home 100', 'pad lx=-0.5+rt=1'\n");
        printf("[DEV]   put <path> <size> - Receive file from host (USB CDC only)\n");
        printf("[DEV]   get <path>     - Send file to host (USB CDC only)\n");
        printf("[DEV]   putb64 <path> <size> - Receive file as base64 (any transport)\n");
        printf("[DEV]   getb64 <path>  - Send file as base64 (any transport)\n");
        printf("[DEV]   screenshot64   - Capture screen as base64 (any transport)\n");
        printf("[DEV]   crashlog       - Print /system/crashlog.txt ('crashlog clear' deletes)\n");
        printf("[DEV]   ls <dir>       - List directory contents\n");
        printf("[DEV]   mkdir <path>   - Create directory (recursive)\n");
        printf("[DEV]   unzip <zip> <dest> - Extract a ZIP archive on-device\n");
        printf("[DEV]   rm <path>      - Delete a file or directory (recursive)\n");
        printf("[DEV]   mv <src> <dst> - Move/rename within the card (not /system, never overwrites)\n");
        printf("[DEV]   status         - Show app/uptime/wifi/sd/battery\n");
        printf("[DEV]   help           - Show this help\n");
        printf("[DEV] Valid keys: up, down, left, right, enter, esc, menu, f1-f10, backspace, tab, del, shift, a-z, A-Z, 0-9, punctuation\n");
    } else {
        printf("[DEV] Unknown command: %s\n", s_cmd_buf);
    }
}

bool dev_commands_process(void) {
    if (!s_cmd_ready) return false;

    // At the launcher this pump runs on Core 0's 4 KB main stack (with ~3 KB
    // already used at peak), and `unzip` alone needs ~5 KB — miniz's central-
    // directory read has a 4 KB local buffer — so it overflowed into MSPLIM
    // and rebooted the device. Every command therefore runs on an app stack:
    // a short-lived 32 KB PSRAM stack at the launcher, or inline on the
    // running app's stack when pumped from inside an app.
    if (!app_stack_run_os(dev_command_run, NULL))
        printf("[DEV] Error: no memory for the command stack, dropped: %s\n",
               s_cmd_buf);

    s_cmd_buf[0] = '\0';
    s_cmd_ready = false;
    return true;
}

bool dev_commands_wants_exit(void) {
    return s_cmd_exit;
}

void dev_commands_clear_exit(void) {
    s_cmd_exit = false;
}

void dev_commands_set_exit(void) {
    s_cmd_exit = true;
}

bool dev_commands_wants_usb(void) {
    return s_cmd_usb;
}

void dev_commands_clear_usb(void) {
    s_cmd_usb = false;
}

bool dev_commands_wants_reboot(void) {
    return s_cmd_reboot;
}

bool dev_commands_wants_reboot_flash(void) {
    return s_cmd_reboot_flash;
}

bool dev_commands_wants_reboot_ota(void) {
    return s_cmd_reboot_ota;
}

void dev_commands_clear_reboot_ota(void) {
    s_cmd_reboot_ota = false;
}

bool dev_commands_wants_list(void) {
    return s_cmd_list;
}

bool dev_commands_list_all(void) {
    return s_cmd_list_all;
}

void dev_commands_clear_list(void) {
    s_cmd_list = false;
    s_cmd_list_all = false;
}

const char* dev_commands_get_pending_launch(void) {
    return s_pending_launch;
}

void dev_commands_clear_pending_launch(void) {
    s_pending_launch = NULL;
}
