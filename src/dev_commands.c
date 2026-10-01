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
#include "tusb.h"
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
static uint32_t s_b64_hash = 2166136261u; // FNV-1a running hash
static uint64_t s_b64_last_rx_us = 0;

// True while a bulk transfer is streaming over serial.  Core 1's log sink
// (wifi.c) checks this and drops its output: an async log line landing
// mid-payload-line defeats the '~' line framing and corrupts the transfer.
// volatile: written on Core 0, read on Core 1.
static volatile bool s_transfer_quiet = false;

bool dev_commands_transfer_active(void) {
    return s_transfer_quiet;
}

// FNV-1a 32-bit — cheap integrity check for serial transfers (not
// cryptographic; OTA does its own SHA-256 before touching flash).
static inline uint32_t fnv1a_update(uint32_t h, const uint8_t *p, uint32_t n) {
    while (n--) { h ^= *p++; h *= 16777619u; }
    return h;
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
    s_pending_launch = NULL;
    s_file_recv_handle = NULL;
    s_file_recv_expected = 0;
    s_file_recv_received = 0;
    s_b64_recv_active = false;
    s_b64_group_len = 0;
    s_b64_writelen = 0;
    s_transfer_quiet = false;
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
            uint32_t got = tud_cdc_read(buf, FILE_RECEIVE_CHUNK_SIZE);
            if (got == 0) break;
            s_b64_last_rx_us = time_us_64();
            int written = sdcard_fwrite(s_file_recv_handle, buf, got);
            if (written < 0) {
                printf("[DEV] Error writing file\n");
                sdcard_fclose(s_file_recv_handle);
                s_file_recv_handle = NULL;
                s_transfer_quiet = false;
                return;
            }
            s_file_recv_received += written;
        }
        if (s_file_recv_received >= s_file_recv_expected) {
            sdcard_fclose(s_file_recv_handle);
            s_file_recv_handle = NULL;
            s_transfer_quiet = false;
            printf("[DEV] File received: %s (%lu bytes)\n", s_file_recv_path, (unsigned long)s_file_recv_received);
        } else if (time_us_64() - s_b64_last_rx_us > B64_RECV_TIMEOUT_US) {
            // Stalled CDC transfer (e.g. host went away): abort so the console
            // does not stay captured in receive mode forever.
            sdcard_fclose(s_file_recv_handle);
            s_file_recv_handle = NULL;
            s_transfer_quiet = false;
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

// Write binary data over CDC, bypassing stdio (for screenshot bulk transfer).
// Caller must call stdio_flush() / fflush(stdout) before this.
// Returns false if CDC is not mounted or the host stops draining: spinning
// unconditionally here starves the watchdog and resets the device when the
// only connection is UART (no CDC host to drain the FIFO).
static bool cdc_write_all(const uint8_t *data, uint32_t len) {
    if (!tud_cdc_connected()) return false;
    uint64_t last_progress_us = time_us_64();
    while (len > 0) {
        uint32_t avail = tud_cdc_write_available();
        if (avail == 0) {
            tud_cdc_write_flush();
            tud_task();
            if (time_us_64() - last_progress_us > 2000000u) return false;
            continue;
        }
        uint32_t chunk = (len < avail) ? len : avail;
        uint32_t written = tud_cdc_write(data, chunk);
        data += written;
        len -= written;
        if (written > 0) {
            last_progress_us = time_us_64();
            watchdog_update();
        }
    }
    tud_cdc_write_flush();
    return true;
}

static void b64_recv_abort(const char *why) {
    if (s_file_recv_handle) {
        sdcard_fclose(s_file_recv_handle);
        s_file_recv_handle = NULL;
    }
    s_b64_recv_active = false;
    s_transfer_quiet = false;
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
    s_b64_hash = fnv1a_update(s_b64_hash, s_b64_writebuf, s_b64_writelen);
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
                s_transfer_quiet = false;
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

    // Header: "SCRN" + width(u16 LE) + height(u16 LE) + format(u16 LE) + pad(2)
    uint8_t header[12];
    header[0] = 'S'; header[1] = 'C'; header[2] = 'R'; header[3] = 'N';
    uint16_t w = FB_WIDTH, h = FB_HEIGHT, fmt = 565;
    memcpy(&header[4], &w, 2);
    memcpy(&header[6], &h, 2);
    memcpy(&header[8], &fmt, 2);
    header[10] = 0; header[11] = 0;

    // Flush stdio so our raw CDC writes don't interleave with printf output
    stdio_flush();

    s_transfer_quiet = true;
    bool ok = cdc_write_all(header, sizeof(header)) &&
              cdc_write_all((const uint8_t *)fb, FB_WIDTH * FB_HEIGHT * sizeof(uint16_t));
    s_transfer_quiet = false;
    if (!ok)
        printf("[DEV] Error: CDC write stalled\n");
}

// Stream a memory buffer as '~'-prefixed base64 lines over stdio.  The '~'
// prefix lets the host discard any log lines Core 1 interleaves into the
// stream; the FNV-1a in the caller's footer catches mid-line interleaving.
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
        hash = fnv1a_update(hash, data + off, n);
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
    s_transfer_quiet = true;
    int size = sdcard_fsize_handle(f);
    printf("[DEV] B64 size=%d\n", size);
    uint8_t buf[B64_LINE_RAW * 4];
    uint32_t hash = 2166136261u;
    int nread;
    while ((nread = sdcard_fread(f, buf, sizeof(buf))) > 0)
        hash = b64_send_buf(buf, (uint32_t)nread, hash);
    sdcard_fclose(f);
    printf("[DEV] B64_END fnv1a=%08lx path=%s\n", (unsigned long)hash, path);
    s_transfer_quiet = false;
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
           "mp3_overran=%lu mp3_frame_us=%lu mp3idle=%d sys_khz=%lu\n",
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
    } else if (strcmp(s_cmd_buf, "list") == 0) {
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
        if (strcmp(s_cmd_buf, "kbdstat reset") == 0)
            kbd_i2c_reset_stats();
        else if (strcmp(s_cmd_buf, "kbdstat fault") == 0)
            kbd_i2c_inject_fault();
        kbd_i2c_stats_t k;
        kbd_i2c_get_stats(&k);
        printf("[DEV] Kbd: state=%s window_ms=%lu reads=%lu items=%lu "
               "dropped=%lu bat_reads=%lu bl_writes=%lu errors=%lu "
               "recoveries=%lu max_gap_us=%lu max_read_us=%lu isr_us=%lu "
               "interval_us=%lu battery=%d sys_khz=%lu\n",
               k.state, (unsigned long)k.window_ms, (unsigned long)k.reads,
               (unsigned long)k.items, (unsigned long)k.dropped,
               (unsigned long)k.bat_reads, (unsigned long)k.bl_writes,
               (unsigned long)k.errors, (unsigned long)k.recoveries,
               (unsigned long)k.max_gap_us, (unsigned long)k.max_read_us,
               (unsigned long)k.isr_us, (unsigned long)k.interval_us,
               k.battery, (unsigned long)(clock_get_hz(clk_sys) / 1000u));
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
        s_b64_last_rx_us = time_us_64();
        s_transfer_quiet = true;
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
        s_b64_hash = 2166136261u;
        s_b64_last_rx_us = time_us_64();
        s_transfer_quiet = true;
        printf("[DEV] Ready B64 %lu bytes for %s (chunk<=%u raw, newline-terminated, await ACK)\n",
               (unsigned long)size, args, (unsigned)B64_WRITE_BUF_SIZE);
    } else if (strncmp(s_cmd_buf, "getb64 ", 7) == 0) {
        dev_send_file_b64(s_cmd_buf + 7);
    } else if (strcmp(s_cmd_buf, "screenshot64") == 0) {
        const uint8_t *fb = (const uint8_t *)display_get_screen_buffer();
        s_transfer_quiet = true;
        printf("[DEV] SCRN64 w=%u h=%u fmt=565\n", (unsigned)FB_WIDTH, (unsigned)FB_HEIGHT);
        uint32_t hash = b64_send_buf(fb, FB_WIDTH * FB_HEIGHT * 2u, 2166136261u);
        printf("[DEV] SCRN64_END fnv1a=%08lx\n", (unsigned long)hash);
        s_transfer_quiet = false;
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

        s_transfer_quiet = true;
        uint8_t buf[256];
        int read;
        while ((read = sdcard_fread(f, buf, sizeof(buf))) > 0) {
            if (!cdc_write_all(buf, read)) {
                printf("[DEV] Error: CDC write stalled\n");
                break;
            }
        }
        sdcard_fclose(f);
        s_transfer_quiet = false;
        printf("[DEV] File sent: %s (%d bytes)\n", path, size);
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
    } else if (strcmp(s_cmd_buf, "help") == 0) {
        printf("[DEV] Available commands:\n");
        printf("[DEV]   ping           - Check device is responding\n");
        printf("[DEV]   ver            - Show firmware build date/time\n");
        printf("[DEV]   stack          - Main, app and OS-command stack peak use\n");
        printf("[DEV]   kbdstat [reset|fault] - Keyboard bus engine counters\n");
        printf("[DEV]   xipstat [reset|off|prio core0|prio none|mp3idle on|mp3idle off] - XIP cache, bus contention and MP3 decode timing\n");
        printf("[DEV]   exit           - Signal current app to exit (error if none)\n");
        printf("[DEV]   usb            - Enable USB storage mode\n");
        printf("[DEV]   reboot         - Reboot device\n");
        printf("[DEV]   reboot-flash   - Reboot to BOOTSEL for flashing\n");
        printf("[DEV]   reboot-ota     - Apply staged /system/update.bin (needs .sha256 + .sig)\n");
        printf("[DEV]   launch <arg>   - Launch app by ID or name\n");
        printf("[DEV]   list           - List installed apps\n");
        printf("[DEV]   screenshot     - Capture screen\n");
        printf("[DEV]   keypress <key> - Inject keypress\n");
        printf("[DEV]   keydown <key> / keyup <key> - Hold/release a key (chords)\n");
        printf("[DEV]   pad <btns|none|off> [ms] - Test gamepad: e.g. 'pad up+a', 'pad home 100'\n");
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

void dev_commands_clear_list(void) {
    s_cmd_list = false;
}

const char* dev_commands_get_pending_launch(void) {
    return s_pending_launch;
}

void dev_commands_clear_pending_launch(void) {
    s_pending_launch = NULL;
}
