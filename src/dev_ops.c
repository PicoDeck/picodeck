#include "dev_ops.h"
#include "core1_stats.h"
#include "dev_commands.h"
#include "drivers/audio.h"
#include "drivers/bt_pad.h"
#include "drivers/wifi.h"
#include "drivers/mp3_player.h"
#include "drivers/pad_source.h"
#include "drivers/sdcard.h"
#include "drivers/sound.h"
#include "os/gamepad_map.h"
#include "os/launcher.h"
#include "os/zip_util.h"
#include "hardware/clocks.h"
#include "hardware/watchdog.h"
#include "pico/time.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

bool dev_op_exit(char *reply, size_t n) {
    dev_commands_set_exit();
    const char *app = launcher_get_running_app_name();
    if (!app || !app[0]) {
        snprintf(reply, n, "Error: exit: no app running");
        return false;
    }
    snprintf(reply, n, "Exit requested: %s", app);
    return true;
}

// Periodic status lines for the host tool and a watchdog feed: extraction of
// a big archive easily outlasts the 10s window and nothing else runs on
// Core 0 while we're in here.
static bool dev_unzip_progress(int done, int total, const char *name,
                               void *user) {
    (void)name; (void)user;
    watchdog_update();
    if (done % 25 == 0 || done == total)
        printf("[DEV] UNZIP %d/%d\n", done, total);
    return true;
}

bool dev_op_unzip(char *args, char *reply, size_t n) {
    // Blocks Core 0 for the duration (same contract as putb64: audio decoded
    // on Core 1 will starve). The engine validates entry names.
    char *zip_path = args;
    char *dest = strchr(zip_path, ' ');
    if (!dest || zip_path[0] != '/' || dest[1] != '/') {
        snprintf(reply, n, "Usage: unzip /path/to.zip /dest/dir");
        return false;
    }
    *dest++ = '\0';
    zip_reader_t zr;
    char err[ZIP_ERR_MAX];
    if (!zip_reader_open(&zr, zip_path, err)) {
        snprintf(reply, n, "Error: unzip failed: %s", err);
        return false;
    }
    zip_extract_result_t result;
    bool ok = zip_reader_extract_all(&zr, dest, NULL, dev_unzip_progress, NULL,
                                     &result, err);
    zip_reader_close(&zr);
    if (!ok) {
        snprintf(reply, n, "Error: unzip failed: %s", err);
        return false;
    }
    snprintf(reply, n, "Unzipped %d files (%d skipped)", result.files_done,
             result.skipped_names);
    return true;
}

bool dev_op_rm(const char *path, char *reply, size_t n) {
    if (path[0] != '/' || strcmp(path, "/") == 0) {
        snprintf(reply, n, "Usage: rm /absolute/path (file or directory)");
        return false;
    }
    if (sdcard_delete(path) || sdcard_delete_recursive(path)) {
        snprintf(reply, n, "Deleted: %s", path);
        return true;
    }
    snprintf(reply, n, "Error: rm failed: %s", path);
    return false;
}

// "up+a" -> PAD_UP | PAD_A; false (with *bad set) on an unknown or empty
// name ("a+", "+").
static bool pad_parse(char *spec, uint32_t *state, const char **bad) {
  uint32_t st = 0;
  for (char *tok = spec;;) {
    char *plus = strchr(tok, '+');
    if (plus)
      *plus = '\0';
    int b = gamepad_button_from_name(tok);
    if (b >= 0) {
      st |= 1u << b;
    } else if (strcasecmp(tok, "home") == 0) {
      st |= PAD_SOURCE_HOME;
    } else {
      *bad = tok;
      return false;
    }
    if (!plus)
      break;
    tok = plus + 1;
  }
  *state = st;
  return true;
}

// The state as the command would name it ("up+a+home").
static void pad_format(uint32_t state, char *out, size_t n) {
  size_t len = 0;
  out[0] = '\0';
  for (int b = 0; b < KBD_PAD_BUTTONS; b++)
    if (state & (1u << b))
      len += (size_t)snprintf(out + len, len < n ? n - len : 0, "%s%s",
                              len ? "+" : "", gamepad_button_name(b));
  if (state & PAD_SOURCE_HOME)
    snprintf(out + len, len < n ? n - len : 0, "%shome", len ? "+" : "");
}

bool dev_op_pad(char *args, char *reply, size_t n) {
  static const char usage[] =
      "Usage: pad <none|off|buttons> [hold_ms] (buttons: up down left right "
      "a b x y l r start select home, joined by '+')";
  while (*args == ' ')
    args++;
  char *spec = args;
  char *hold = strchr(args, ' ');
  if (hold) {
    *hold++ = '\0';
    while (*hold == ' ')
      hold++;
  }
  uint32_t hold_ms = 0;
  if (hold && *hold) {
    char *end;
    unsigned long v = strtoul(hold, &end, 10);
    if (*end || v > 600000ul) {
      snprintf(reply, n, "%s", usage);
      return false;
    }
    hold_ms = (uint32_t)v;
  }
  if (!spec[0]) {
    snprintf(reply, n, "%s", usage);
    return false;
  }
  if (strcasecmp(spec, "off") == 0) {
    pad_source_test_off();
    snprintf(reply, n, "Pad: off");
    return true;
  }
  uint32_t state = 0;
  if (strcasecmp(spec, "none") != 0) {
    const char *bad = NULL;
    if (!pad_parse(spec, &state, &bad)) {
      snprintf(reply, n, "Error: unknown pad button: %s", bad);
      return false;
    }
  }
  pad_source_test_set(state, hold_ms);
  char names[96];
  pad_format(state, names, sizeof(names));
  if (hold_ms)
    snprintf(reply, n, "Pad: %s for %lu ms", state ? names : "none",
             (unsigned long)hold_ms);
  else
    snprintf(reply, n, "Pad: %s", state ? names : "none");
  return true;
}

bool dev_op_audiostat(bool reset, char *reply, size_t n) {
    // Core 1 zeroes its tick counters at its next tick (1 ms) and the
    // refill interrupt its own at its next refill (~2.9 ms): give them both
    // time. The stream's and the MP3's counters reset under their locks.
    if (reset) {
        core1_reset_tick_stats();
        audio_output_reset_stats();
        audio_stream_reset_underruns();
        mp3_player_reset_staging_underruns();
        sleep_ms(5);
    }
    core1_tick_stats_t t;
    core1_get_tick_stats(&t);
    audio_output_stats_t o;
    audio_output_get_stats(&o);
    audio_stream_stats_t s;
    audio_stream_get_stats(&s);
    snprintf(reply, n,
             "Audio: window_ms=%lu ticks=%lu tick_over=%lu tick_missed=%lu "
             "tick_max_us=%lu out=%d isr=%lu isr_us=%lu isr_max_us=%lu "
             "stream_underruns=%lu mp3_underruns=%lu voices=%d sys_khz=%lu "
             "stream_starts=%lu stream_loops=%lu stream_gaps=%lu "
             "stream_start_underruns=%lu stream_loop_underruns=%lu "
             "stream_first_ms=%ld stream_last_ms=%ld stream_low_ms=%ld",
             (unsigned long)t.window_ms, (unsigned long)t.ticks,
             (unsigned long)t.over, (unsigned long)t.missed,
             (unsigned long)t.max_us, o.running ? 1 : 0,
             (unsigned long)o.isr_count, (unsigned long)o.isr_us,
             (unsigned long)o.isr_max_us, (unsigned long)s.underruns,
             (unsigned long)mp3_player_staging_underruns(),
             sound_get_playing_source_count(),
             (unsigned long)(clock_get_hz(clk_sys) / 1000),
             (unsigned long)s.starts, (unsigned long)s.loops,
             (unsigned long)s.gaps, (unsigned long)s.start_underruns,
             (unsigned long)s.loop_underruns, (long)s.first_ms,
             (long)s.last_ms, (long)s.low_ms);
    return true;
}

// ── bt ──────────────────────────────────────────────────────────────────────

#ifdef PICODECK_SIMULATOR
// The simulator's scripted radio (simulator/sim_bt.c).
bool bt_pad_sim_add(const uint8_t addr[6], uint32_t cod, const char *name);
void bt_pad_sim_clear(void);
bool bt_pad_sim_report(uint32_t state);
bool bt_pad_sim_drop(void);
bool bt_pad_sim_wake(const uint8_t addr[6]);
bool bt_pad_sim_spoof(const uint8_t addr[6]);

// "bt sim add <addr> <cod hex> <name>" (a device in pairing mode in range),
// "bt sim clear", "bt sim press <buttons|none>" (the connected pad's state,
// as `pad` names it), "bt sim drop" (link loss), "bt sim wake <addr>" (a
// bonded pad reconnects), "bt sim spoof <addr>" (a device with that address
// and no link key asks to pair: replies "refused" or "bonded").
static bool pad_parse(char *spec, uint32_t *state, const char **bad);

static bool dev_op_bt_sim(char *args, char *reply, size_t n) {
  uint8_t addr[6];
  if (strncmp(args, "add ", 4) == 0) {
    char *as = args + 4, *cod = strchr(as, ' ');
    char *name = cod ? strchr(cod + 1, ' ') : NULL;
    if (cod)
      *cod++ = '\0';
    if (name)
      *name++ = '\0';
    if (!cod || !bt_pad_addr_parse(as, addr)) {
      snprintf(reply, n, "Usage: bt sim add <addr> <cod hex> [name]");
      return false;
    }
    bool ok = bt_pad_sim_add(addr, (uint32_t)strtoul(cod, NULL, 16),
                             name ? name : "");
    snprintf(reply, n, ok ? "BT sim: added %s" : "Error: bt sim: full", as);
    return ok;
  }
  if (strcmp(args, "clear") == 0) {
    bt_pad_sim_clear();
    snprintf(reply, n, "BT sim: cleared");
    return true;
  }
  if (strncmp(args, "press ", 6) == 0) {
    uint32_t st = 0;
    const char *bad = NULL;
    if (strcasecmp(args + 6, "none") != 0 && !pad_parse(args + 6, &st, &bad)) {
      snprintf(reply, n, "Error: unknown pad button: %s", bad);
      return false;
    }
    bool ok = bt_pad_sim_report(st);
    snprintf(reply, n, ok ? "BT sim: report" : "Error: bt sim: no pad");
    return ok;
  }
  if (strcmp(args, "drop") == 0) {
    bool ok = bt_pad_sim_drop();
    snprintf(reply, n, ok ? "BT sim: dropped" : "Error: bt sim: no link");
    return ok;
  }
  if (strncmp(args, "spoof ", 6) == 0 && bt_pad_addr_parse(args + 6, addr)) {
    bool bonded = bt_pad_sim_spoof(addr);
    snprintf(reply, n, bonded ? "BT sim: bonded" : "BT sim: refused");
    return true;
  }
  if (strncmp(args, "wake ", 5) == 0 && bt_pad_addr_parse(args + 5, addr)) {
    bool ok = bt_pad_sim_wake(addr);
    snprintf(reply, n, ok ? "BT sim: waking" : "Error: bt sim: refused");
    return ok;
  }
  snprintf(reply, n, "Usage: bt sim add|clear|press|drop|wake|spoof");
  return false;
}
#endif

static const char *bt_power_name(bt_pad_power_t p) {
  static const char *const k[] = {"off", "starting", "on", "failed"};
  return (unsigned)p < 4 ? k[p] : "?";
}

static const char *bt_link_name(bt_pad_link_t l) {
  static const char *const k[] = {"none", "connecting", "connected"};
  return (unsigned)l < 3 ? k[l] : "?";
}

// The name a found or paired pad goes by, for `bt connect <addr>`.
static const char *bt_known_name(const bt_pad_status_t *st,
                                 const uint8_t addr[6]) {
  for (int i = 0; i < st->n_found; i++)
    if (memcmp(st->found[i].addr, addr, 6) == 0)
      return st->found[i].name;
  for (int i = 0; i < st->n_paired; i++)
    if (memcmp(st->paired[i].addr, addr, 6) == 0)
      return st->paired[i].name;
  return "";
}

bool dev_op_bt(char *args, char *reply, size_t n) {
  static const char usage[] =
      "Usage: bt [status|on|off|scan|scan stop|found|paired|connect <addr>|"
      "disconnect|forget <addr>]";
  while (*args == ' ')
    args++;
  bt_pad_status_t st;
  bt_pad_get_status(&st);
  if (!st.available && args[0] && strcmp(args, "status") != 0) {
    snprintf(reply, n, "Error: Bluetooth is not available");
    return false;
  }
  uint8_t addr[6];
  char a[18];
  if (!args[0] || strcmp(args, "status") == 0) {
    bt_pad_addr_str(st.peer.addr, a);
    snprintf(reply, n,
             "BT: available=%d enabled=%d power=%s scanning=%d link=%s "
             "peer=%s ready=%d profile=%s reports=%lu paired=%u found=%u "
             "radio_in_use=%d bus_errors=%lu note=\"%s\"",
             st.available, st.enabled, bt_power_name(st.power), st.scanning,
             bt_link_name(st.link), st.link ? a : "-", st.pad_ready,
             st.profile ? st.profile : "-", (unsigned long)st.reports,
             (unsigned)st.n_paired, (unsigned)st.n_found, st.radio_in_use,
             (unsigned long)wifi_bus_errors(), st.note);
    return true;
  }
  if (strcmp(args, "on") == 0 || strcmp(args, "off") == 0) {
    bool on = strcmp(args, "on") == 0;
    bool ok = bt_pad_set_enabled(on);
    snprintf(reply, n, ok ? "BT: %s" : "Error: bt %s failed", args);
    return ok;
  }
  if (strcmp(args, "scan") == 0 || strcmp(args, "scan stop") == 0) {
    bool on = strcmp(args, "scan") == 0;
    bool ok = bt_pad_scan(on);
    snprintf(reply, n, ok ? "BT: %s" : "Error: bt %s: Bluetooth is not on",
             on ? "scanning" : "scan stopped");
    return ok;
  }
  if (strcmp(args, "found") == 0 || strcmp(args, "paired") == 0) {
    bool found = args[0] == 'f';
    int cnt = found ? st.n_found : st.n_paired;
    int at = snprintf(reply, n, "BT %s: n=%d", args, cnt);
    for (int i = 0; i < cnt && at > 0 && (size_t)at < n; i++) {
      const uint8_t *ad = found ? st.found[i].addr : st.paired[i].addr;
      const char *nm = found ? st.found[i].name : st.paired[i].name;
      bt_pad_addr_str(ad, a);
      if (found)
        at += snprintf(reply + at, n - (size_t)at, "; %s %06lx %s", a,
                       (unsigned long)st.found[i].cod, nm[0] ? nm : "?");
      else
        at += snprintf(reply + at, n - (size_t)at, "; %s %s", a,
                       nm[0] ? nm : "?");
    }
    return true;
  }
  if (strncmp(args, "connect ", 8) == 0 || strncmp(args, "forget ", 7) == 0) {
    bool connect = args[0] == 'c';
    const char *as = args + (connect ? 8 : 7);
    if (!bt_pad_addr_parse(as, addr)) {
      snprintf(reply, n, "%s", usage);
      return false;
    }
    bool ok = connect ? bt_pad_connect(addr, bt_known_name(&st, addr))
                      : bt_pad_forget(addr);
    if (!connect)
      bt_pad_service(); // the bond goes from the SD card now
    snprintf(reply, n, ok ? "BT: %s %s" : "Error: bt %s %s failed",
             connect ? "connecting to" : "forgot", as);
    return ok;
  }
  if (strcmp(args, "disconnect") == 0) {
    bt_pad_disconnect();
    snprintf(reply, n, "BT: disconnecting");
    return true;
  }
#ifdef PICODECK_SIMULATOR
  if (strncmp(args, "sim ", 4) == 0)
    return dev_op_bt_sim(args + 4, reply, n);
#endif
  snprintf(reply, n, "%s", usage);
  return false;
}
