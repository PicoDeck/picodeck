#include "video_player.h"

extern "C" {
#include "sdcard.h"
#include "display.h"
#include "umm_malloc.h"
#include "wifi.h"
#include "mp3_player.h"
#include "pio_psram.h"
#include "avi_index.h"
#include "../os/launcher.h"
#include "../os/config.h"
#include "../os/idle_dim.h"
}

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <atomic>
#include <pico/time.h>
#include <pico/multicore.h>
#include <hardware/watchdog.h>

#include <JPEGDEC.h>

typedef struct {
    uint32_t file_offset;
    uint32_t chunk_size;
} frame_index_entry_t;

typedef struct {
    uint8_t *buffer;
    uint32_t capacity;
    uint32_t size;
    bool in_use;
} buffer_pool_entry_t;

typedef struct {
    sdfile_t file;
    uint32_t movi_offset;
    uint32_t movi_size;
    uint32_t next_chunk_pos;

    JPEGDEC *jpeg;       // points to static s_jpeg_sram (SRAM BSS)
    buffer_pool_entry_t buffer_pool[VIDEO_BUFFER_POOL_SIZE];
    int current_buffer;

    uint64_t start_time_us;
    uint32_t frame_duration_us;

    uint64_t last_frame_times[16];
    int last_frame_idx;

    frame_index_entry_t *frame_index;
    uint32_t frame_index_count;
    uint32_t frame_index_capacity;
    bool     frame_index_capped;   // file has more frames than the index holds

    uint32_t dropped_frames;
    uint32_t consecutive_drops;
    uint32_t adaptive_stride;

    // Deferred flush: DMA starts at the beginning of the next decode,
    // overlapping with the SD read for the next frame.
    bool     pending_flush;
    uint16_t flush_y0;
    uint16_t flush_y1;

    bool     overclocked;       // True if we boosted the clock for playback
    bool     wifi_was_connected; // True if WiFi was connected before overclock

    // Audio track info (parsed from AVI headers)
    bool     has_audio;
    uint16_t audio_format;         // 0x0055 = MP3
    uint32_t audio_sample_rate;
    uint16_t audio_channels;
    uint32_t audio_avg_bytes_sec;
    uint32_t audio_strh_length;      // audio stream header dwLength and
    uint32_t audio_strh_sample_size; // dwSampleSize: size the audio index

    // Audio chunk index (parallel to video frame_index)
    avi_index_t audio_index;

    // Audio playback state
    uint32_t audio_feed_cursor;    // Next audio chunk to feed
    bool     audio_muted;
    bool     audio_active;         // True if fed mode is currently running

    // On-screen display (progress bar + time).  osd_backup holds the clean
    // video rows under the strip so the OSD can be re-rendered (e.g. with the
    // pause glyph) on a frame that already carries it.
    bool      osd_enabled;
    uint64_t  osd_timeout_us;
    uint64_t  osd_hide_at_us;
    uint16_t *osd_backup;      // OSD_STRIP_H * FB_WIDTH pixels (PSRAM)
    bool      osd_on_back;     // back buffer strip currently has the OSD drawn
    bool      osd_on_front;    // front buffer strip currently has the OSD drawn

    // Diagnostics: Core-1 prefetch hits/misses
    uint32_t pf_hits;
    uint32_t pf_misses;
} video_priv_t;

// JPEGDEC in static SRAM BSS: Huffman tables (10KB), MCU buffers, VLC staging,
// and quant tables all get single-cycle SRAM access instead of going through
// the 16KB XIP cache (which thrashes against 24KB of JPEGDEC flash code).
static JPEGDEC s_jpeg_sram;

// --- Adaptive quality: 2x nearest-neighbor upscale callback ---
// When the player falls behind, we decode at half resolution and upscale
// each MCU row directly to the framebuffer via this callback.
static int s_adaptive_x_offset;
static int s_adaptive_y_offset;

static int jpeg_draw_cb(JPEGDRAW *pDraw) {
    display_blit_be(pDraw->x, pDraw->y, (const uint16_t *)pDraw->pPixels,
                    pDraw->iWidth, pDraw->iHeight);
    return 1;
}

static int jpeg_draw_cb_2x(JPEGDRAW *pDraw) {
    uint16_t *fb = display_get_back_buffer();
    for (int row = 0; row < pDraw->iHeight; row++) {
        const uint16_t *src = &pDraw->pPixels[row * pDraw->iWidth];
        int dy = s_adaptive_y_offset + (pDraw->y + row) * 2;
        if (dy < 0 || dy + 1 >= FB_HEIGHT) continue;
        uint16_t *dst0 = &fb[dy * FB_WIDTH];
        uint16_t *dst1 = &fb[(dy + 1) * FB_WIDTH];
        for (int col = 0; col < pDraw->iWidth; col++) {
            int dx = s_adaptive_x_offset + (pDraw->x + col) * 2;
            if (dx < 0 || dx + 1 >= FB_WIDTH) continue;
            uint16_t pixel = src[col];
            dst0[dx]     = pixel;
            dst0[dx + 1] = pixel;
            dst1[dx]     = pixel;
            dst1[dx + 1] = pixel;
        }
    }
    return 1;
}

static uint8_t *buffer_pool_acquire(video_priv_t *priv, uint32_t needed_size) {
    for (int i = 0; i < VIDEO_BUFFER_POOL_SIZE; i++) {
        if (!priv->buffer_pool[i].in_use) {
            if (priv->buffer_pool[i].capacity < needed_size) {
                if (priv->buffer_pool[i].buffer) {
                    umm_free(priv->buffer_pool[i].buffer);
                }
                priv->buffer_pool[i].buffer = (uint8_t *)umm_malloc(needed_size);
                if (!priv->buffer_pool[i].buffer) {
                    priv->buffer_pool[i].capacity = 0;
                    return NULL;
                }
                priv->buffer_pool[i].capacity = needed_size;
            }
            priv->buffer_pool[i].in_use = true;
            priv->buffer_pool[i].size = needed_size;
            priv->current_buffer = i;
            return priv->buffer_pool[i].buffer;
        }
    }
    return NULL;
}

static void buffer_pool_release(video_priv_t *priv, uint8_t *buffer) {
    for (int i = 0; i < VIDEO_BUFFER_POOL_SIZE; i++) {
        if (priv->buffer_pool[i].buffer == buffer) {
            priv->buffer_pool[i].in_use = false;
            return;
        }
    }
}

static void buffer_pool_cleanup(video_priv_t *priv) {
    for (int i = 0; i < VIDEO_BUFFER_POOL_SIZE; i++) {
        if (priv->buffer_pool[i].buffer) {
            umm_free(priv->buffer_pool[i].buffer);
            priv->buffer_pool[i].buffer = NULL;
            priv->buffer_pool[i].capacity = 0;
            priv->buffer_pool[i].in_use = false;
        }
    }
}

// --- Core-1 next-frame prefetch ---------------------------------------------
// While Core 0 decodes frame N (~22ms), Core 1 reads frame N+1 from SD into a
// pool buffer (~9ms), hiding the SD read entirely.  Modeled on image_preload:
// atomic state machine + doorbell wake.  The SD driver's cross-core recursive
// mutex serializes FatFS access; a dedicated read-only file handle avoids
// fseek/fread interleaving with Core 0's audio-feed reads on the main handle
// (FF_FS_LOCK permits duplicate read-mode opens).
//
// Ownership rules:
//  - Pool buffers are acquired/released ONLY on Core 0 (post/consume/cancel).
//  - Core 1 only writes into the buffer while state is VPF_READING.
//  - Cancel is synchronous: it waits for Core 1 to leave VPF_READING before
//    the buffer is released, so Core 1 can never write freed memory.

typedef enum {
    VPF_IDLE = 0,
    VPF_REQUESTED,   // Core 0 posted a request; Core 1 hasn't picked it up
    VPF_READING,     // Core 1 is reading from SD into s_vpf_buf
    VPF_DONE,        // Frame data ready for Core 0
    VPF_FAILED,      // Read failed; Core 0 falls back to a direct SD read
    VPF_CANCELLED    // Core 0 cancelled mid-read; Core 1 settles to IDLE
} vpf_state_t;

static std::atomic<int> s_vpf_state{VPF_IDLE};
static sdfile_t s_vpf_file = NULL;   // Core 1's dedicated read handle
static uint8_t *s_vpf_buf = NULL;    // destination pool buffer (Core 0 owns pool)
static uint32_t s_vpf_offset;        // absolute file offset of JPEG payload
static uint32_t s_vpf_size;          // payload size
static uint32_t s_vpf_frame;         // frame number being prefetched

// Core 0: post a prefetch request for `frame` if the slot is free.
static void video_prefetch_post(video_priv_t *priv, uint32_t frame) {
    if (!s_vpf_file || !priv->frame_index) return;
    if (frame >= priv->frame_index_count) return;

    if (s_vpf_state.load() != VPF_IDLE) return;  // busy or unclaimed result

    uint32_t size = priv->frame_index[frame].chunk_size;
    if (size == 0) return;

    uint8_t *buf = buffer_pool_acquire(priv, size);
    if (!buf) return;

    s_vpf_buf = buf;
    s_vpf_size = size;
    s_vpf_frame = frame;
    s_vpf_offset = priv->frame_index[frame].file_offset + 8;  // skip chunk header

    int expected = VPF_IDLE;
    if (!s_vpf_state.compare_exchange_strong(expected, VPF_REQUESTED)) {
        buffer_pool_release(priv, buf);
        return;
    }
    multicore_doorbell_set_other_core(WIFI_IPC_DOORBELL);  // wake Core 1 now
}

// Core 0: settle the state machine and reclaim the pool buffer.  Blocks for
// at most one chunked read (~10ms) if Core 1 is mid-read.
static void video_prefetch_cancel(video_priv_t *priv) {
    while (true) {
        int cur = s_vpf_state.load();
        if (cur == VPF_IDLE) return;

        if (cur == VPF_REQUESTED || cur == VPF_DONE || cur == VPF_FAILED) {
            if (s_vpf_state.compare_exchange_strong(cur, VPF_IDLE)) {
                if (priv && s_vpf_buf) buffer_pool_release(priv, s_vpf_buf);
                s_vpf_buf = NULL;
                return;
            }
            continue;  // lost a race with Core 1 — re-read state
        }
        if (cur == VPF_READING) {
            s_vpf_state.compare_exchange_strong(cur, VPF_CANCELLED);
            continue;  // wait for Core 1 to settle
        }
        // VPF_CANCELLED: Core 1 will store IDLE when its read finishes.
        tight_loop_contents();
    }
}

// Core 1 (called from the core1 tick loop): service a pending request.
extern "C" void video_prefetch_update(void) {
    int expected = VPF_REQUESTED;
    if (!s_vpf_state.compare_exchange_strong(expected, VPF_READING)) return;

    sdfile_t f = s_vpf_file;
    bool ok = (f != NULL) && sdcard_fseek(f, s_vpf_offset);

    // Chunked read with an mp3 poll between chunks so the fed-mode audio
    // decoder (also on Core 1) never starves behind a long SD transfer.
    uint32_t done = 0;
    while (ok && done < s_vpf_size) {
        uint32_t n = s_vpf_size - done;
        if (n > 16384) n = 16384;
        if (sdcard_fread(f, s_vpf_buf + done, (int)n) != (int)n) {
            ok = false;
            break;
        }
        done += n;
        if (done < s_vpf_size) mp3_player_update();
    }

    expected = VPF_READING;
    if (!s_vpf_state.compare_exchange_strong(expected,
                                             ok ? VPF_DONE : VPF_FAILED)) {
        // Cancelled mid-read: Core 0 is spinning in cancel and reclaims the
        // buffer once we settle.
        s_vpf_state.store(VPF_IDLE);
    }
}

// Build a complete frame index (one entry per frame) for O(1) seeking
// and instant skip-to-target in the update loop.  Sized from the AVI
// header's frame count (capped at VIDEO_MAX_FRAME_INDEX).  The audio index
// is sized from the audio stream header and grows while the scan finds
// more chunks (avi_index.h): at a low frame rate there are several MP3
// chunks per video frame.
static bool build_frame_index(video_priv_t *priv, video_player_t *player) {
    uint32_t cap = player->frame_count;
    if (cap == 0) cap = 8192;  // header didn't say — scan and find out
    if (cap > VIDEO_MAX_FRAME_INDEX) cap = VIDEO_MAX_FRAME_INDEX;
    priv->frame_index_capacity = cap;
    priv->frame_index_capped = false;

    priv->frame_index = (frame_index_entry_t *)umm_malloc(
        priv->frame_index_capacity * sizeof(frame_index_entry_t));
    if (!priv->frame_index) {
        priv->frame_index_capacity = 0;
        return false;
    }

    // Allocate audio index if audio stream present
    if (priv->has_audio) {
        uint32_t acap = avi_audio_index_capacity(cap, priv->audio_strh_length,
                                                 priv->audio_strh_sample_size);
        priv->audio_index.count = 0;
        priv->audio_index.entries = (avi_index_entry_t *)umm_malloc(
            acap * sizeof(avi_index_entry_t));
        priv->audio_index.capacity = priv->audio_index.entries ? acap : 0;
        // Non-fatal if alloc fails — video still works without audio
        if (!priv->audio_index.entries) {
            printf("[VIDEO] Warning: could not allocate audio index\n");
            priv->has_audio = false;
        }
    }

    uint32_t saved_pos = sdcard_ftell(priv->file);
    sdcard_fseek(priv->file, priv->movi_offset);

    // A bogus LIST size (some streaming muxers write 0) means scan to EOF;
    // the trailing idx1 chunk is then skipped like any other non-frame chunk.
    uint32_t movi_end = (priv->movi_size >= 8)
                            ? priv->movi_offset + priv->movi_size
                            : 0xFFFFFFFFu;
    uint32_t frame_num = 0;
    bool audio_full = false;
    uint8_t chunk[8];

    while (sdcard_ftell(priv->file) < movi_end &&
           sdcard_fread(priv->file, chunk, 8) == 8) {
        uint32_t chunk_pos = sdcard_ftell(priv->file) - 8;
        uint32_t size = *(uint32_t *)(chunk + 4);
        uint32_t next_pos = sdcard_ftell(priv->file) + size;
        if (next_pos & 1) next_pos++;

        if (chunk[2] == 'd' && (chunk[3] == 'b' || chunk[3] == 'c')) {
            if (frame_num >= priv->frame_index_capacity) {
                priv->frame_index_capped = true;
                break;
            }
            priv->frame_index[frame_num].file_offset = chunk_pos;
            priv->frame_index[frame_num].chunk_size = size;
            frame_num++;
            if (frame_num % 100 == 0) watchdog_update();
        } else if (priv->has_audio && priv->audio_index.entries && !audio_full &&
                   chunk[0] == '0' && chunk[1] == '1' &&
                   chunk[2] == 'w' && chunk[3] == 'b') {
            if (!avi_index_push(&priv->audio_index, chunk_pos, size, umm_realloc)) {
                audio_full = true;   // out of memory: the rest of the audio is lost
                printf("[VIDEO] Audio index full at %u chunks\n",
                       (unsigned)priv->audio_index.count);
            }
        }
        sdcard_fseek(priv->file, next_pos);
    }

    priv->frame_index_count = frame_num;
    sdcard_fseek(priv->file, saved_pos);

    // The scan is authoritative when it covered the whole movi list: trust it
    // over a missing or inflated dwTotalFrames so end-of-file and duration
    // are exact.
    if (!priv->frame_index_capped && frame_num > 0) {
        player->frame_count = frame_num;
    } else if (priv->frame_index_capped && player->frame_count < frame_num) {
        player->frame_count = frame_num;
    }

    if (priv->has_audio && priv->audio_index.count > 0) {
        printf("[VIDEO] Audio index: %u chunks\n", (unsigned)priv->audio_index.count);
    }
    if (priv->frame_index_capped) {
        printf("[VIDEO] Frame index capped at %u entries; seeking beyond that is slow\n",
               (unsigned)priv->frame_index_capacity);
    }

    return true;
}

// Flush any deferred frame to the display.
//
// In the steady-state decode loop (video_player_update) the very next decode
// repaints the full flushed region, so the front→back sync memcpy
// (~200KB/frame) done by display_flush_region() is wasted — use the no-copy
// variant there.  Terminal paths (pause/stop/seek/destroy) are followed by
// overlay drawing + full display_flush() from Lua, which alternates buffers:
// without the sync the two buffers hold different video frames and the
// paused screen flickers — so those paths must keep the copy.
static void flush_pending_nocopy(video_priv_t *priv) {
    if (priv->pending_flush) {
        display_flush_region_nocopy(priv->flush_y0, priv->flush_y1);
        priv->pending_flush = false;
        priv->osd_on_front = priv->osd_on_back;
        priv->osd_on_back = false;   // stale rows; always synced before reuse
        // Active playback counts as activity — don't let the idle dimmer
        // darken the screen mid-video.
        idle_dim_note_activity();
    }
}

static void flush_pending(video_priv_t *priv) {
    if (priv->pending_flush) {
        display_flush_region(priv->flush_y0, priv->flush_y1);
        priv->pending_flush = false;
        priv->osd_on_front = priv->osd_on_back;   // copy variant: both equal
    }
}

static void video_boost_clock(video_priv_t *priv);
static void video_restore_clock(video_priv_t *priv);

// --- Position helpers --------------------------------------------------------

// Frame currently on screen: current_frame is the NEXT frame to decode.
static inline uint32_t displayed_frame(const video_player_t *player) {
    return player->current_frame > 0 ? player->current_frame - 1 : 0;
}

static inline uint32_t frame_to_ms(const video_priv_t *priv, uint32_t frame) {
    return (uint32_t)(((uint64_t)frame * priv->frame_duration_us) / 1000);
}

static inline uint32_t ms_to_frame(const video_priv_t *priv, uint32_t ms) {
    if (priv->frame_duration_us == 0) return 0;
    return (uint32_t)(((uint64_t)ms * 1000) / priv->frame_duration_us);
}

// --- On-screen display -------------------------------------------------------
// Drawn into the back buffer after each decode (inside the flush region, so
// the deferred flush carries it) and into the presented frame on pause/seek/
// end.  Geometry sits over the bottom of the visible video rows.

#define OSD_STRIP_H     22
#define OSD_MARGIN_X    8
#define OSD_BAR_H       4
#define OSD_MIN_VIDEO_H 48

static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

static void osd_format_time(char *out, size_t n, uint32_t ms) {
    uint32_t s = ms / 1000;
    uint32_t h = s / 3600;
    uint32_t m = (s / 60) % 60;
    s %= 60;
    if (h) snprintf(out, n, "%u:%02u:%02u", (unsigned)h, (unsigned)m, (unsigned)s);
    else   snprintf(out, n, "%u:%02u", (unsigned)m, (unsigned)s);
}

static bool osd_visible(const video_player_t *player, const video_priv_t *priv) {
    if (!priv->osd_enabled) return false;
    if (player->visible_height < OSD_MIN_VIDEO_H) return false;
    if (player->paused || player->ended || !player->playing) return true;
    return time_us_64() < priv->osd_hide_at_us;
}

static void osd_draw(video_player_t *player, video_priv_t *priv) {
    int y1 = player->y_offset + player->visible_height - 1;
    int y0 = y1 - OSD_STRIP_H + 1;
    if (y0 < 0) return;

    uint16_t *fb = display_get_back_buffer();
    uint16_t *strip = &fb[y0 * FB_WIDTH];
    const size_t strip_bytes = OSD_STRIP_H * FB_WIDTH * sizeof(uint16_t);

    if (priv->osd_backup) {
        if (priv->osd_on_back) memcpy(strip, priv->osd_backup, strip_bytes);  // re-render
        else                   memcpy(priv->osd_backup, strip, strip_bytes);  // remember clean rows
    } else if (priv->osd_on_back) {
        return;  // no backup: can't redraw without double-darkening
    }
    priv->osd_on_back = true;

    // Darken the strip in place (framebuffer holds byte-swapped RGB565).
    for (int y = y0; y <= y1; y++) {
        uint16_t *row = &fb[y * FB_WIDTH];
        for (int x = 0; x < FB_WIDTH; x++) {
            uint16_t p = __builtin_bswap16(row[x]);
            p = (p >> 1) & 0x7BEF;  // halve each channel
            row[x] = __builtin_bswap16(p);
        }
    }

    uint32_t frames   = player->frame_count ? player->frame_count : 1;
    uint32_t cur      = player->ended ? frames : displayed_frame(player);
    if (cur > frames) cur = frames;
    uint32_t pos_ms   = frame_to_ms(priv, cur);
    uint32_t dur_ms   = frame_to_ms(priv, frames);

    const uint16_t white = 0xFFFF;
    const uint16_t track = rgb565(90, 90, 90);
    const uint16_t fill  = rgb565(255, 60, 60);

    // Progress bar along the bottom of the strip
    int bar_x = OSD_MARGIN_X;
    int bar_w = FB_WIDTH - 2 * OSD_MARGIN_X;
    int bar_y = y1 - 3 - OSD_BAR_H + 1;
    display_fill_rect(bar_x, bar_y, bar_w, OSD_BAR_H, track);
    int filled = (int)(((uint64_t)bar_w * cur) / frames);
    if (filled > 0) display_fill_rect(bar_x, bar_y, filled, OSD_BAR_H, fill);
    // Playhead knob
    int knob_x = bar_x + filled - 2;
    if (knob_x < bar_x) knob_x = bar_x;
    if (knob_x > bar_x + bar_w - 4) knob_x = bar_x + bar_w - 4;
    display_fill_rect(knob_x, bar_y - 1, 4, OSD_BAR_H + 2, white);

    // Time row above the bar: elapsed left, total right, state glyph centre.
    // Always the built-in 6x8 font, whatever the app selected.
    int saved_font = display_get_font();
    display_set_font(0);
    int text_y = bar_y - 1 - 8 - 2;
    char buf[16];
    osd_format_time(buf, sizeof buf, pos_ms);
    display_draw_text_transparent(bar_x, text_y, buf, white);
    osd_format_time(buf, sizeof buf, dur_ms);
    int tw = display_text_width(buf);
    display_draw_text_transparent(bar_x + bar_w - tw, text_y, buf, white);

    int cx = FB_WIDTH / 2;
    if (player->ended) {
        display_draw_text_transparent(cx - display_text_width("END") / 2, text_y, "END", white);
    } else if (player->paused) {
        display_fill_rect(cx - 4, text_y, 3, 8, white);   // pause bars
        display_fill_rect(cx + 1, text_y, 3, 8, white);
    }
    display_set_font(saved_font);
}

// Present the current frame (with OSD if visible) as a stable still: make
// sure the back buffer holds the latest frame, draw the OSD, flush with copy
// so both buffers match.  Used by the pause / seek-while-paused / end-of-
// video paths, after which the app draws its own overlays and calls
// display_flush() (alternating buffers) without flicker.
static void present_still(video_player_t *player, video_priv_t *priv) {
    int y0 = player->y_offset;
    int y1 = player->y_offset + player->visible_height - 1;

    if (!priv->pending_flush) {
        // Latest frame is on the front (already flushed); the back rows are
        // stale from the no-copy steady state.  Pull them across.
        display_sync_back_region(y0, y1);
        priv->osd_on_back = priv->osd_on_front;
    }
    if (osd_visible(player, priv)) osd_draw(player, priv);

    if (priv->pending_flush || player->auto_flush) {
        display_flush_region(y0, y1);   // copy variant: both buffers now equal
        priv->pending_flush = false;
        priv->osd_on_front = priv->osd_on_back;
    }
}

// --- Audio -------------------------------------------------------------------

// Feed audio chunks from the audio index into the MP3 fed ring.
// Called during play (pre-fill) and update (top-up).
static void video_feed_audio(video_priv_t *priv, int max_chunks) {
    if (!priv->has_audio || !priv->audio_index.entries || priv->audio_muted)
        return;

    uint8_t temp[2048];
    int chunks_fed = 0;

    while (chunks_fed < max_chunks &&
           priv->audio_feed_cursor < priv->audio_index.count) {
        uint32_t space = mp3_player_feed_space();
        if (space < 1024) break;

        uint32_t idx = priv->audio_feed_cursor;
        uint32_t size = priv->audio_index.entries[idx].chunk_size;
        if (size > sizeof(temp) || size > space) break;

        sdcard_fseek(priv->file, priv->audio_index.entries[idx].file_offset + 8);
        sdcard_fread(priv->file, temp, size);
        mp3_player_feed(temp, size);

        priv->audio_feed_cursor++;
        chunks_fed++;
    }
}

static void audio_stop(video_priv_t *priv) {
    if (priv->audio_active) {
        mp3_player_stop_fed();
        priv->audio_active = false;
    }
}

// (Re)start fed-mode audio positioned at `frame`.  Pre-fills the compressed
// ring from SD; starts the output immediately unless `defer_dma` (paused —
// resume starts it).  Any previous fed session is torn down first.
static void audio_start_at(video_priv_t *priv, video_player_t *player,
                           uint32_t frame, bool defer_dma) {
    audio_stop(priv);
    if (!priv->has_audio || priv->audio_format != 0x0055 || priv->audio_muted ||
        !priv->audio_index.entries || priv->audio_index.count == 0)
        return;
    if (!mp3_player_start_fed(priv->audio_sample_rate, priv->audio_channels))
        return;
    priv->audio_active = true;
    // Approximate audio cursor from the video frame position
    uint32_t denom = priv->frame_index_count ? priv->frame_index_count : player->frame_count;
    if (denom == 0) denom = 1;
    priv->audio_feed_cursor = (uint32_t)((uint64_t)frame * priv->audio_index.count / denom);
    if (priv->audio_feed_cursor >= priv->audio_index.count)
        priv->audio_feed_cursor = priv->audio_index.count - 1;
    video_feed_audio(priv, 50);            // pre-fill fed ring with compressed data
    if (!defer_dma) mp3_player_start_fed_output();  // decode + start the mixer pulling it
}

// Align the wall-clock frame timer so that frame `current_frame` is due now.
static inline void rebase_clock(video_priv_t *priv, video_player_t *player) {
    priv->start_time_us = time_us_64() - (uint64_t)player->current_frame * priv->frame_duration_us;
}

// --- Lifecycle ---------------------------------------------------------------

bool video_player_init(void) {
    return true;
}

video_player_t *video_player_create(void) {
    video_player_t *player = (video_player_t *)umm_malloc(sizeof(video_player_t));
    if (!player) return NULL;
    memset(player, 0, sizeof(video_player_t));

    video_priv_t *priv = (video_priv_t *)umm_malloc(sizeof(video_priv_t));
    if (!priv) {
        umm_free(player);
        return NULL;
    }
    memset(priv, 0, sizeof(video_priv_t));

    priv->jpeg = &s_jpeg_sram;

    for (int i = 0; i < VIDEO_BUFFER_POOL_SIZE; i++) {
        priv->buffer_pool[i].buffer = (uint8_t *)umm_malloc(VIDEO_MAX_JPEG_SIZE);
        if (priv->buffer_pool[i].buffer) {
            priv->buffer_pool[i].capacity = VIDEO_MAX_JPEG_SIZE;
        }
    }

    priv->adaptive_stride = 1;
    priv->osd_enabled = true;
    priv->osd_timeout_us = 3000 * 1000u;
    priv->osd_backup = (uint16_t *)umm_malloc(OSD_STRIP_H * FB_WIDTH * sizeof(uint16_t));

    player->priv = priv;
    return player;
}

void video_player_destroy(video_player_t *player) {
    if (!player) return;
    video_priv_t *priv = (video_priv_t *)player->priv;
    if (priv) {
        // Must settle Core-1 prefetch BEFORE closing files or freeing the
        // buffer pool it writes into.
        video_prefetch_cancel(priv);
        if (s_vpf_file) {
            sdcard_fclose(s_vpf_file);
            s_vpf_file = NULL;
        }
        audio_stop(priv);
        flush_pending(priv);
        video_restore_clock(priv);
        if (priv->file) sdcard_fclose(priv->file);
        buffer_pool_cleanup(priv);
        if (priv->frame_index) umm_free(priv->frame_index);
        if (priv->audio_index.entries) umm_free(priv->audio_index.entries);
        if (priv->osd_backup) umm_free(priv->osd_backup);
        // priv->jpeg points to static s_jpeg_sram — no free needed
        umm_free(priv);
    }
    umm_free(player);
}

bool video_player_load(video_player_t *player, const char *path) {
    video_priv_t *priv = (video_priv_t *)player->priv;

    // Settle any in-flight Core-1 prefetch before touching files/indices.
    video_prefetch_cancel(priv);
    if (s_vpf_file) {
        sdcard_fclose(s_vpf_file);
        s_vpf_file = NULL;
    }

    if (priv->file) {
        sdcard_fclose(priv->file);
        priv->file = NULL;
    }
    if (priv->frame_index) {
        umm_free(priv->frame_index);
        priv->frame_index = NULL;
    }
    priv->frame_index_count = 0;
    priv->frame_index_capacity = 0;
    if (priv->audio_index.entries) umm_free(priv->audio_index.entries);
    priv->audio_index = avi_index_t{};
    audio_stop(priv);

    player->playing = false;
    player->paused = false;
    player->ended = false;
    player->frame_count = 0;
    priv->movi_offset = 0;
    priv->movi_size = 0;

    priv->file = sdcard_fopen(path, "rb");
    if (!priv->file) return false;

    uint8_t header[64];
    sdcard_fread(priv->file, header, 64);

    if (memcmp(header, "RIFF", 4) != 0 || memcmp(header + 8, "AVI ", 4) != 0) {
        printf("[VIDEO] Not a valid AVI file\n");
        return false;
    }

    priv->has_audio = false;
    priv->audio_format = 0;
    priv->audio_sample_rate = 0;
    priv->audio_channels = 0;
    priv->audio_avg_bytes_sec = 0;
    priv->audio_strh_length = 0;
    priv->audio_strh_sample_size = 0;

    sdcard_fseek(priv->file, 12);
    uint8_t chunk[8];
    while (sdcard_fread(priv->file, chunk, 8) == 8) {
        watchdog_update();
        uint32_t size = *(uint32_t *)(chunk + 4);
        if (memcmp(chunk, "LIST", 4) == 0) {
            uint8_t list_type[4];
            sdcard_fread(priv->file, list_type, 4);
            if (memcmp(list_type, "movi", 4) == 0) {
                priv->movi_offset = sdcard_ftell(priv->file);
                priv->movi_size = size - 4;
                break;
            } else if (memcmp(list_type, "strl", 4) == 0) {
                // Parse stream list — look for audio stream header + format
                uint32_t strl_end = sdcard_ftell(priv->file) + size - 4;
                uint8_t sub[8];
                while (sdcard_ftell(priv->file) < strl_end && sdcard_fread(priv->file, sub, 8) == 8) {
                    uint32_t sub_size = *(uint32_t *)(sub + 4);
                    uint32_t sub_start = sdcard_ftell(priv->file);

                    if (memcmp(sub, "strh", 4) == 0 && sub_size >= 56) {
                        uint8_t strh[56];
                        sdcard_fread(priv->file, strh, 56);
                        if (memcmp(strh, "auds", 4) == 0) {
                            // This is an audio stream — read the matching strf
                            uint32_t strh_end = sub_start + sub_size;
                            if (strh_end & 1) strh_end++;
                            sdcard_fseek(priv->file, strh_end);

                            uint8_t strf_hdr[8];
                            if (sdcard_fread(priv->file, strf_hdr, 8) == 8 &&
                                memcmp(strf_hdr, "strf", 4) == 0) {
                                uint32_t strf_size = *(uint32_t *)(strf_hdr + 4);
                                if (strf_size >= 16) {
                                    uint8_t wfx[18];
                                    uint32_t to_read = (strf_size < 18) ? strf_size : 18;
                                    sdcard_fread(priv->file, wfx, to_read);
                                    uint16_t fmt_tag = *(uint16_t *)(wfx + 0);
                                    if (fmt_tag == 0x0055) {  // MP3
                                        priv->has_audio = true;
                                        priv->audio_format = fmt_tag;
                                        priv->audio_strh_length = *(uint32_t *)(strh + 32);
                                        priv->audio_strh_sample_size = *(uint32_t *)(strh + 44);
                                        priv->audio_channels = *(uint16_t *)(wfx + 2);
                                        priv->audio_sample_rate = *(uint32_t *)(wfx + 4);
                                        priv->audio_avg_bytes_sec = *(uint32_t *)(wfx + 8);
                                        printf("[VIDEO] Audio: MP3 %u Hz, %u ch, %u bytes/sec\n",
                                               (unsigned)priv->audio_sample_rate,
                                               (unsigned)priv->audio_channels,
                                               (unsigned)priv->audio_avg_bytes_sec);
                                    }
                                }
                            }
                        }
                    }

                    // Skip to next sub-chunk
                    uint32_t next = sub_start + sub_size;
                    if (next & 1) next++;
                    sdcard_fseek(priv->file, next);
                }
                // Ensure we're at the end of the strl LIST
                sdcard_fseek(priv->file, strl_end);
                if (sdcard_ftell(priv->file) & 1)
                    sdcard_fseek(priv->file, sdcard_ftell(priv->file) + 1);
                continue;  // Skip the alignment check at the bottom
            }
            // Other LIST types: skip remaining content
            // (already consumed 4 bytes for list_type, so skip size-4 more)
        } else if (memcmp(chunk, "avih", 4) == 0) {
            uint8_t avih[56];
            sdcard_fread(priv->file, avih, size < 56 ? size : 56);
            uint32_t microsec_per_frame = *(uint32_t *)avih;
            if (microsec_per_frame == 0) microsec_per_frame = 40000;  // 25 fps fallback
            player->fps_num = 1000000;
            player->fps_den = microsec_per_frame;
            player->width = *(uint32_t *)(avih + 32);
            player->height = *(uint32_t *)(avih + 36);
            player->frame_count = *(uint32_t *)(avih + 16);
            priv->frame_duration_us = microsec_per_frame;
            if (size > 56) sdcard_fseek(priv->file, sdcard_ftell(priv->file) + (size - 56));
        } else {
            sdcard_fseek(priv->file, sdcard_ftell(priv->file) + size);
        }
        if (sdcard_ftell(priv->file) & 1) sdcard_fseek(priv->file, sdcard_ftell(priv->file) + 1);
    }

    if (priv->movi_offset == 0) {
        printf("[VIDEO] Could not find 'movi' list\n");
        return false;
    }
    if (priv->frame_duration_us == 0) {
        printf("[VIDEO] Missing 'avih' header\n");
        return false;
    }

    sdcard_fseek(priv->file, priv->movi_offset);
    priv->next_chunk_pos = priv->movi_offset;
    player->current_frame = 0;

    int vh = (int)player->height;
    int vw = (int)player->width;
    // Account for auto-downscaling in decode_frame_at
    if (vw > 640 || vh > 640) {
        vh /= 4;
    } else if (vw > 320 || vh > 320) {
        vh /= 2;
    }
    if (vh < 1) vh = 1;
    if (vh > 320) vh = 320;
    player->y_offset = (uint16_t)((320 - vh) / 2);
    player->visible_height = (uint16_t)vh;

    priv->dropped_frames = 0;
    priv->consecutive_drops = 0;
    priv->adaptive_stride = 1;
    priv->pending_flush = false;
    priv->osd_hide_at_us = 0;

    if (!build_frame_index(priv, player)) {
        printf("[VIDEO] Warning: could not build frame index, seeking will be slow\n");
    }
    if (player->frame_count == 0) {
        printf("[VIDEO] No video frames found\n");
        return false;
    }

    // Dedicated read handle for the Core-1 prefetcher.  FF_FS_LOCK allows
    // duplicate read-mode opens; a separate FIL means Core 1's fseek/fread
    // never interleaves with Core 0's audio-feed reads on the main handle.
    priv->pf_hits = 0;
    priv->pf_misses = 0;
    s_vpf_file = sdcard_fopen(path, "rb");
    if (!s_vpf_file) {
        printf("[VIDEO] Prefetch: second file handle unavailable, SD reads stay on Core 0\n");
    }

    printf("[VIDEO] Loaded %s: %lux%lu, %u frames, %u fps, %u index entries, %u ms\n",
           path, player->width, player->height, (unsigned)player->frame_count,
           (unsigned)(player->fps_num / player->fps_den),
           (unsigned)priv->frame_index_count,
           (unsigned)frame_to_ms(priv, player->frame_count));

    return true;
}

static void video_boost_clock(video_priv_t *priv) {
    if (priv->overclocked) return;

    // The CYW43 PIO SPI clock divider is set at init (200 MHz) and is NOT
    // updated by launcher_apply_clock(), so WiFi must be disconnected first.
    priv->wifi_was_connected = false;
    if (wifi_is_available()) {
        wifi_status_t wst = wifi_get_status();
        if (wst == WIFI_STATUS_CONNECTED || wst == WIFI_STATUS_CONNECTING ||
            wst == WIFI_STATUS_ONLINE) {
            priv->wifi_was_connected = true;
            wifi_disconnect();
        }
        // Wait for Core 1 to actually finish the hardware disconnect (bounded):
        // changing sysclk while the driver is still talking to the chip
        // produces an "hdr mismatch" error storm on Core 1 that starves the
        // frame prefetcher and the MP3 decoder for the whole playback.
        uint64_t deadline =
            time_us_64() + (uint64_t)WIFI_HW_DISCONNECT_WAIT_MS * 1000;
        while (!wifi_hw_disconnected() && time_us_64() < deadline) {
            sleep_ms(5);
        }
        if (!wifi_hw_disconnected())
            printf("[VIDEO] WiFi not idle after %d ms; boosting anyway\n",
                   WIFI_HW_DISCONNECT_WAIT_MS);
    }

    // Boost to 300 MHz for video playback
    launcher_apply_clock(300000);
    priv->overclocked = true;
}

static void video_restore_clock(video_priv_t *priv) {
    if (!priv->overclocked) return;

    launcher_apply_clock(200000);
    priv->overclocked = false;

    // Reconnect WiFi if it was connected before
    if (priv->wifi_was_connected && wifi_is_available()) {
        const char *ssid = config_get("wifi_ssid");
        const char *pass = config_get("wifi_pass");
        if (ssid && ssid[0])
            wifi_connect(ssid, pass ? pass : "");
        priv->wifi_was_connected = false;
    }
}

void video_player_play(video_player_t *player) {
    video_priv_t *priv = (video_priv_t *)player->priv;

    // Clear both framebuffers to black (double-buffered: must clear twice)
    display_clear(0x0000);
    display_flush();
    display_wait_for_flush();
    display_clear(0x0000);  // Now the swapped-in back buffer is also black

    video_boost_clock(priv);

    // Replay after the end (or after a seek to the last frame) restarts.
    if (player->ended || player->current_frame >= player->frame_count) {
        video_prefetch_cancel(priv);
        player->current_frame = 0;
        if (priv->frame_index) priv->next_chunk_pos = priv->frame_index[0].file_offset;
        else priv->next_chunk_pos = priv->movi_offset;
    }
    player->playing = true;
    player->paused = false;
    player->ended = false;
    priv->osd_hide_at_us = time_us_64() + priv->osd_timeout_us;

    // Start audio if available (before capturing start_time_us so the clock
    // starts only after the audio is queued, keeping A/V in sync from frame 0)
    audio_start_at(priv, player, player->current_frame, false);

    // Capture start time after audio setup so the video clock aligns with
    // audio sample 0 (video_feed_audio + start_fed_output take ~50ms of SD I/O)
    rebase_clock(priv, player);
}

void video_player_stop(video_player_t *player) {
    video_priv_t *priv = (video_priv_t *)player->priv;
    video_prefetch_cancel(priv);
    flush_pending(priv);
    audio_stop(priv);
    player->playing = false;
    player->paused = false;
    video_restore_clock(priv);
}

void video_player_pause(video_player_t *player) {
    video_priv_t *priv = (video_priv_t *)player->priv;
    if (!player->playing || player->paused) return;
    player->paused = true;
    present_still(player, priv);
    if (priv->audio_active) {
        mp3_player_pause(mp3_player_create());
    }
}

void video_player_resume(video_player_t *player) {
    video_priv_t *priv = (video_priv_t *)player->priv;
    if (player->ended || !player->playing) {
        // Resume after the end (or before play) == play
        video_player_play(player);
        return;
    }
    player->paused = false;
    priv->osd_hide_at_us = time_us_64() + priv->osd_timeout_us;
    if (priv->audio_active) {
        mp3_player_t *mp3 = mp3_player_create();
        if (mp3_player_is_playing(mp3)) {
            // Already playing (e.g. a seek restarted it while paused)
        } else if (mp3->playing && mp3->paused) {
            // Normal resume from pause
            mp3_player_resume(mp3);
        } else {
            // Fed mode was restarted (e.g. seek while paused) — start it
            video_feed_audio(priv, 50);
            mp3_player_start_fed_output();
        }
    }
    // Capture start time after any audio work so clock aligns with audio
    rebase_clock(priv, player);
}

static bool decode_frame_at(video_player_t *player, video_priv_t *priv,
                            uint32_t target_frame, uint32_t chunk_pos, uint32_t size) {
    uint8_t *jpeg_buf = NULL;
    bool from_prefetch = false;   // Core-1 prefetch (pool buffer, release after)

    // Check Core-1 prefetch slot (claim any settled result; use it only if
    // it is exactly the frame we need, otherwise discard it)
    {
        int cur = s_vpf_state.load();
        if (cur == VPF_DONE || cur == VPF_FAILED) {
            uint8_t *pbuf = s_vpf_buf;
            uint32_t pframe = s_vpf_frame;
            uint32_t psize = s_vpf_size;
            if (s_vpf_state.compare_exchange_strong(cur, VPF_IDLE)) {
                s_vpf_buf = NULL;
                if (cur == VPF_DONE && pframe == target_frame) {
                    jpeg_buf = pbuf;
                    size = psize;
                    from_prefetch = true;
                    priv->pf_hits++;
                } else if (pbuf) {
                    buffer_pool_release(priv, pbuf);
                }
            }
        }
    }

    // Fallback: read from SD
    if (!from_prefetch) {
        priv->pf_misses++;
        jpeg_buf = buffer_pool_acquire(priv, size);
        if (!jpeg_buf) {
            return false;
        }
    }

    uint64_t t_sd_start = time_us_64();
    uint64_t t_sd_end = t_sd_start;

    if (!from_prefetch) {
        // Read JPEG data from SD (SPI0).
        // Previous frame's flush was already issued at the top of video_player_update(),
        // and DMA (PIO0) likely finished during the target-frame calculation + SD seek.
        sdcard_fseek(priv->file, chunk_pos + 8);
        sdcard_fread(priv->file, jpeg_buf, size);
        t_sd_end = time_us_64();
    }

    // Kick off the Core-1 prefetch for the next frame now, so it overlaps
    // with the JPEG decode below.  Core 0 makes no further SD calls until
    // the next update's audio feed, so the SD bus is free for Core 1.
    video_prefetch_post(priv, target_frame + 1);

    // No DMA wait needed: the display is double-buffered — the in-flight DMA
    // (started by flush_pending at the top of update) reads the *front*
    // buffer while every decode path below writes the *back* buffer
    // (setFramebuffer/display_blit_be/jpeg_draw_cb_2x all target
    // display_get_back_buffer()/s_framebuffer).  display_flush_region()
    // itself waits for the previous transfer before swapping, so buffers can
    // never alias.  Removing the wait here overlaps decode (~22ms) with the
    // panel DMA (~21ms at 75MHz SPI) instead of serializing them.
    uint64_t t_flush_wait = time_us_64();

    bool success = false;
    bool use_adaptive = (priv->adaptive_stride > 1);

    JPEG_DRAW_CALLBACK *draw_cb = use_adaptive ? jpeg_draw_cb_2x : jpeg_draw_cb;

    uint64_t t_open_start = time_us_64();
    if (priv->jpeg->openRAM(jpeg_buf, (int)size, draw_cb)) {
        uint64_t t_setup_start = time_us_64();
        priv->jpeg->setPixelType(RGB565_BIG_ENDIAN);
        int vw = priv->jpeg->getWidth();
        int vh = priv->jpeg->getHeight();
        int scale = 0;
        if (vw > 640 || vh > 640) {
            scale = JPEG_SCALE_QUARTER;
            vw /= 4; vh /= 4;
        } else if (vw > 320 || vh > 320) {
            scale = JPEG_SCALE_HALF;
            vw /= 2; vh /= 2;
        }

        // Step 5: Adaptive quality — decode at one extra level of downscale
        // and use the 2x upscale callback.  Drops decode time ~60%.
        if (use_adaptive && scale < JPEG_SCALE_QUARTER) {
            if (scale == 0) {
                scale = JPEG_SCALE_HALF;
                vw /= 2; vh /= 2;
            } else if (scale == JPEG_SCALE_HALF) {
                scale = JPEG_SCALE_QUARTER;
                vw /= 2; vh /= 2;
            }
            // Offsets for the 2x upscale callback to center the output
            s_adaptive_x_offset = (320 - vw * 2) / 2;
            s_adaptive_y_offset = (320 - vh * 2) / 2;
            priv->jpeg->decode(0, 0, scale);
        }
        // Step 4: Direct framebuffer decode for 320-wide videos.
        // JPEGDEC pitch = iCropCX = decoded width, which matches FB_WIDTH
        // when vw == 320, so setFramebuffer works correctly.
        else if (vw == 320 && scale == 0) {
            uint16_t *back_buf = display_get_back_buffer();
            int y = (320 - vh) / 2;
            priv->jpeg->setFramebuffer(back_buf + y * FB_WIDTH);
            priv->jpeg->decode(0, 0, scale);
        }
        // Standard callback path for non-320-wide videos
        else {
            int x = (320 - vw) / 2;
            int y = (320 - vh) / 2;
            priv->jpeg->decode(x, y, scale);
        }

        uint64_t t_dec_end = time_us_64();
        priv->jpeg->close();
        uint64_t t_close = time_us_64();
        success = true;

        // Print timing every 30 frames — split decode into open (header/Huffman parse)
        // vs decode (IDCT + pixel conversion) to identify the hot spot.
        if (target_frame % 30 == 0) {
            printf("[VIDEO] f=%u sd=%ums flush=%ums open=%ums dec=%ums close=%ums total=%ums stride=%u src=%s pf=%u/%u cfps=%u\n",
                   (unsigned)target_frame,
                   (unsigned)((t_sd_end - t_sd_start) / 1000),
                   (unsigned)((t_flush_wait - t_sd_end) / 1000),
                   (unsigned)((t_setup_start - t_open_start) / 1000),
                   (unsigned)((t_dec_end - t_setup_start) / 1000),
                   (unsigned)((t_close - t_dec_end) / 1000),
                   (unsigned)((t_close - t_sd_start) / 1000),
                   (unsigned)priv->adaptive_stride,
                   from_prefetch ? "pf" : "sd",
                   (unsigned)priv->pf_hits,
                   (unsigned)(priv->pf_hits + priv->pf_misses),
                   (unsigned)(priv->frame_duration_us ? 1000000u / priv->frame_duration_us : 0));
        }

        priv->osd_on_back = false;   // fresh pixels under the strip

        // Defer the flush — it will be issued at the start of the next
        // video_player_update(), hiding DMA wait behind target-frame calc + SD read.
        if (player->auto_flush) {
            priv->pending_flush = true;
            priv->flush_y0 = player->y_offset;
            priv->flush_y1 = player->y_offset + player->visible_height - 1;
        }

        priv->last_frame_times[priv->last_frame_idx] = time_us_64();
        priv->last_frame_idx = (priv->last_frame_idx + 1) % 16;
    }

    // Covers both the direct-SD pool buffer and a consumed prefetch buffer.
    buffer_pool_release(priv, jpeg_buf);
    return success;
}

// Decode frame `frame` via the index (or the sequential fallback) and advance
// current_frame / next_chunk_pos past it.  Returns false if nothing decoded.
static bool decode_frame_number(video_player_t *player, video_priv_t *priv, uint32_t frame) {
    if (priv->frame_index && frame < priv->frame_index_count) {
        uint32_t chunk_pos = priv->frame_index[frame].file_offset;
        uint32_t size = priv->frame_index[frame].chunk_size;
        bool ok = decode_frame_at(player, priv, frame, chunk_pos, size);
        player->current_frame = frame + 1;
        priv->next_chunk_pos = chunk_pos + 8 + size;
        if (priv->next_chunk_pos & 1) priv->next_chunk_pos++;
        return ok;
    }

    // Slow fallback: sequential chunk reading from next_chunk_pos, which
    // must currently point at (or before) the chunk holding `frame`.
    bool decoded = false;
    while (player->current_frame <= frame) {
        sdcard_fseek(priv->file, priv->next_chunk_pos);
        uint8_t chunk[8];
        if (sdcard_fread(priv->file, chunk, 8) != 8) return decoded;

        uint32_t size = *(uint32_t *)(chunk + 4);
        priv->next_chunk_pos = sdcard_ftell(priv->file) + size;
        if (priv->next_chunk_pos & 1) priv->next_chunk_pos++;

        if (chunk[2] == 'd' && (chunk[3] == 'b' || chunk[3] == 'c')) {
            if (player->current_frame == frame) {
                decoded = decode_frame_at(player, priv, frame, sdcard_ftell(priv->file) - 8, size);
            } else {
                priv->dropped_frames++;
            }
            player->current_frame++;
        }
    }
    return decoded;
}

static void video_end_reached(video_player_t *player, video_priv_t *priv) {
    if (player->loop) {
        flush_pending(priv);
        video_player_seek(player, 0);
        return;
    }
    player->ended = true;
    player->playing = false;
    player->paused = false;
    player->current_frame = player->frame_count;
    audio_stop(priv);
    video_prefetch_cancel(priv);
    present_still(player, priv);   // hold the last frame, OSD at 100%
}

bool video_player_update(video_player_t *player) {
    if (!player->playing || player->paused) return false;

    video_priv_t *priv = (video_priv_t *)player->priv;

    // Flush any deferred frame from the previous update.
    // This fires early so DMA runs during target-frame calculation,
    // and prevents double-swap jitter if Lua also called disp.flush().
    // No-copy: the decode below repaints the entire region.
    flush_pending_nocopy(priv);

    uint64_t now = time_us_64();
    uint32_t target_frame = (uint32_t)((now - priv->start_time_us) / priv->frame_duration_us);

    if (target_frame < player->current_frame) {
        // Wall-clock says we should be showing a frame we already decoded.
        // Wait for the next frame (current_frame) to become due.
        uint64_t next_us = priv->start_time_us + (uint64_t)player->current_frame * priv->frame_duration_us;
        int64_t wait = (int64_t)(next_us - now);
        if (wait > 0 && wait <= 5000) {
            busy_wait_us_32((uint32_t)wait);
            target_frame = player->current_frame;
        } else {
            return false;
        }
    }

    // Handle end of video
    if (target_frame >= player->frame_count) {
        video_end_reached(player, priv);
        return false;
    }

    // Track frame drops and adapt stride
    int32_t frames_behind = (int32_t)(target_frame - player->current_frame);
    if (frames_behind > 3) {
        priv->consecutive_drops++;
        if (priv->consecutive_drops > 2 && priv->adaptive_stride < 4) {
            priv->adaptive_stride++;
            printf("[VIDEO] Increasing stride to %u (behind by %d)\n",
                   (unsigned)priv->adaptive_stride, (int)frames_behind);
        }
        priv->dropped_frames += (uint32_t)frames_behind;
    } else {
        if (priv->consecutive_drops > 0) {
            priv->consecutive_drops--;
            if (priv->consecutive_drops == 0 && priv->adaptive_stride > 1) {
                priv->adaptive_stride--;
                printf("[VIDEO] Decreasing stride to %u\n", (unsigned)priv->adaptive_stride);
            }
        }
    }

    // Feed audio chunks to keep the compressed ring topped up
    if (priv->audio_active) {
        video_feed_audio(priv, 20);
    }

    bool decoded = decode_frame_number(player, priv, target_frame);

    if (!decoded && player->current_frame <= target_frame) {
        // Sequential fallback hit EOF before reaching the target: the file
        // holds fewer frames than the header claimed.
        player->frame_count = player->current_frame;
        video_end_reached(player, priv);
        return false;
    }

    if (decoded && osd_visible(player, priv)) {
        osd_draw(player, priv);
    }
    return decoded;
}

void video_player_seek(video_player_t *player, uint32_t frame) {
    video_priv_t *priv = (video_priv_t *)player->priv;
    if (!priv->file || player->frame_count == 0) return;

    video_prefetch_cancel(priv);  // in-flight frame is stale after a seek
    flush_pending(priv);

    // Clamp — never wrap.  Reaching the last frame ends the video naturally
    // on the next update (loop or hold), exactly as if playback got there.
    if (frame >= player->frame_count) frame = player->frame_count - 1;

    // Fast path: direct index lookup
    if (priv->frame_index && frame < priv->frame_index_count) {
        priv->next_chunk_pos = priv->frame_index[frame].file_offset;
        player->current_frame = frame;
    } else {
        // Slow fallback: scan chunk headers.  Continue forward from the
        // current position when the target is ahead of it; otherwise restart
        // from the last indexed frame (or the start of movi).
        if (frame < player->current_frame || player->current_frame >= player->frame_count) {
            if (priv->frame_index && priv->frame_index_count > 0) {
                uint32_t last = priv->frame_index_count - 1;
                priv->next_chunk_pos = priv->frame_index[last].file_offset;
                player->current_frame = last;
            } else {
                priv->next_chunk_pos = priv->movi_offset;
                player->current_frame = 0;
            }
        }

        // Scan until next_chunk_pos sits at (or just before) the target's
        // chunk: decode_frame_number's sequential path takes it from there.
        while (player->current_frame < frame) {
            sdcard_fseek(priv->file, priv->next_chunk_pos);
            uint8_t chunk[8];
            if (sdcard_fread(priv->file, chunk, 8) != 8) break;
            uint32_t size = *(uint32_t *)(chunk + 4);
            priv->next_chunk_pos = sdcard_ftell(priv->file) + size;
            if (priv->next_chunk_pos & 1) priv->next_chunk_pos++;
            if (chunk[2] == 'd' && (chunk[3] == 'b' || chunk[3] == 'c')) {
                player->current_frame++;
                if (player->current_frame % 100 == 0) watchdog_update();
            }
        }
    }

    priv->adaptive_stride = 1;
    priv->consecutive_drops = 0;
    priv->osd_hide_at_us = time_us_64() + priv->osd_timeout_us;

    bool was_ended = player->ended;
    if (was_ended) {
        // Seeking an ended player restarts playback from the target.
        player->ended = false;
        player->playing = true;
        player->paused = false;
    }

    // Reposition audio: flush and restart the fed ring from the proportional
    // audio chunk for the target frame.  Do this before capturing start_time_us
    // so the video clock starts only after audio pre-fill (SD I/O) completes.
    if (priv->audio_active || was_ended) {
        audio_start_at(priv, player, player->current_frame, player->paused);
    }

    if (player->paused || !player->playing) {
        // Present the target frame right away so scrubbing gives feedback.
        // (The clock is rebased on resume.)
        decode_frame_number(player, priv, frame);
        present_still(player, priv);
        return;
    }

    // Capture start time after audio setup so clock aligns with restarted audio
    rebase_clock(priv, player);
}

void video_player_seek_ms(video_player_t *player, uint32_t ms) {
    video_priv_t *priv = (video_priv_t *)player->priv;
    video_player_seek(player, ms_to_frame(priv, ms));
}

void video_player_seek_relative_ms(video_player_t *player, int32_t delta_ms) {
    video_priv_t *priv = (video_priv_t *)player->priv;
    int64_t pos = (int64_t)frame_to_ms(priv, displayed_frame(player)) + delta_ms;
    if (pos < 0) pos = 0;
    video_player_seek_ms(player, (uint32_t)pos);
}

float video_player_get_fps(video_player_t *player) {
    video_priv_t *priv = (video_priv_t *)player->priv;
    int prev_idx = (priv->last_frame_idx + 15) % 16;
    uint64_t dt = priv->last_frame_times[prev_idx] - priv->last_frame_times[priv->last_frame_idx];
    if (dt == 0) return 0;
    return 15000000.0f / (float)dt;
}

uint32_t video_player_get_frame_count(video_player_t *player) {
    return player ? player->frame_count : 0;
}

uint32_t video_player_get_duration_ms(video_player_t *player) {
    if (!player || !player->priv) return 0;
    return frame_to_ms((video_priv_t *)player->priv, player->frame_count);
}

uint32_t video_player_get_position_ms(video_player_t *player) {
    if (!player || !player->priv) return 0;
    video_priv_t *priv = (video_priv_t *)player->priv;
    uint32_t f = player->ended ? player->frame_count : displayed_frame(player);
    return frame_to_ms(priv, f);
}

bool video_player_has_ended(video_player_t *player) {
    return player ? player->ended : false;
}

void video_player_set_osd(video_player_t *player, bool enabled) {
    if (!player || !player->priv) return;
    ((video_priv_t *)player->priv)->osd_enabled = enabled;
}

void video_player_show_osd(video_player_t *player) {
    if (!player || !player->priv) return;
    video_priv_t *priv = (video_priv_t *)player->priv;
    priv->osd_hide_at_us = time_us_64() + priv->osd_timeout_us;
    if (player->paused || player->ended) {
        // Nothing is being decoded — repaint the still with the OSD on it.
        present_still(player, priv);
    }
}

void video_player_set_osd_timeout(video_player_t *player, uint32_t ms) {
    if (!player || !player->priv) return;
    ((video_priv_t *)player->priv)->osd_timeout_us = (uint64_t)ms * 1000;
}

uint32_t video_player_get_dropped_frames(video_player_t *player) {
    video_priv_t *priv = (video_priv_t *)player->priv;
    return priv->dropped_frames;
}

void video_player_reset_stats(video_player_t *player) {
    video_priv_t *priv = (video_priv_t *)player->priv;
    priv->dropped_frames = 0;
    priv->consecutive_drops = 0;
    priv->adaptive_stride = 1;
}

bool video_player_has_audio(video_player_t *player) {
    if (!player || !player->priv) return false;
    video_priv_t *priv = (video_priv_t *)player->priv;
    return priv->has_audio;
}

void video_player_set_audio_volume(video_player_t *player, uint8_t volume) {
    if (!player || !player->priv) return;
    video_priv_t *priv = (video_priv_t *)player->priv;
    if (priv->audio_active) {
        mp3_player_set_volume(mp3_player_create(), volume);
    }
}

uint8_t video_player_get_audio_volume(video_player_t *player) {
    if (!player || !player->priv) return 0;
    video_priv_t *priv = (video_priv_t *)player->priv;
    if (priv->audio_active) {
        return mp3_player_get_volume(mp3_player_create());
    }
    return 100;  // default
}

void video_player_set_audio_muted(video_player_t *player, bool muted) {
    if (!player || !player->priv) return;
    video_priv_t *priv = (video_priv_t *)player->priv;
    priv->audio_muted = muted;

    if (muted) {
        audio_stop(priv);
    } else if (!priv->audio_active && player->playing && !player->paused) {
        // Unmute: start audio from the current position
        audio_start_at(priv, player, player->current_frame, false);
    }
}

bool video_player_get_audio_muted(video_player_t *player) {
    if (!player || !player->priv) return false;
    video_priv_t *priv = (video_priv_t *)player->priv;
    return priv->audio_muted;
}
