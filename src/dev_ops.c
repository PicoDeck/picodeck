#include "dev_ops.h"
#include "core1_stats.h"
#include "dev_commands.h"
#include "drivers/audio.h"
#include "drivers/mp3_player.h"
#include "drivers/sdcard.h"
#include "drivers/sound.h"
#include "os/launcher.h"
#include "os/zip_util.h"
#include "hardware/clocks.h"
#include "hardware/watchdog.h"
#include "pico/time.h"

#include <stdio.h>
#include <string.h>

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
