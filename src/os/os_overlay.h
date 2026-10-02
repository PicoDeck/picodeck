#pragma once

// The OS overlay pass: what the OS draws over an app's frame on every present
// — the system toast and the Show FPS counter. Called from all three present
// paths of both the Lua bridge (lua_bridge_display.c) and the native API
// (src/main.c, simulator/unicorn_trampolines.c), just before the present, so
// only while an app runs: the launcher and the system menu present through
// display_flush() directly.

#include <stdbool.h>
#include <stdint.h>

typedef enum {
  OS_PRESENT_FLUSH,   // flush(): the whole frame, then a swap
  OS_PRESENT_ROWS,    // flushRows(y0, y1): rows of the draw buffer, no swap
  OS_PRESENT_REGION,  // flushRegion(y0, y1): rows, then a swap
} os_present_t;

// Show FPS modes, in the order the Settings item cycles through them.
enum {
  OS_FPS_OFF,
  OS_FPS_TOP_RIGHT,
  OS_FPS_TOP_LEFT,
  OS_FPS_BOTTOM_RIGHT,
  OS_FPS_BOTTOM_LEFT,
  OS_FPS_MODES
};

// Count the present and draw the overlays into the back buffer. For
// flush() they are drawn whole. For flushRows/flushRegion (rows y0..y1) they
// are drawn only into those rows, and an overlay that reaches outside them is
// pushed to the panel as a small window write (display_push_rect) when it
// appears, changes or goes away, composed over the app's pixels and then
// removed again, so the app's draw buffer keeps its own pixels there (an
// overlay drawn into rows that were sent stays in the buffer, as with
// flush()). Nothing is drawn while the hardware-scroll offset is non-zero
// (the rows would land at scrolled positions): a partial present then pushes
// the buffer's rows back over what the panel showed of an overlay.
void os_overlay_draw(os_present_t kind, int y0, int y1);

// perf.endFrame() (Lua and native): one logical frame. While an app ticks
// perf, the counter shows these per second instead of presents.
void os_overlay_frame_tick(uint32_t now_ms);

// At every app launch: zero the counter and re-read the setting.
void os_overlay_app_start(void);

// The show_fps setting (/system/config.json): "tr", "tl", "br", "bl"; no
// key (or "0") is Off. os_overlay_fps_mode() reads the config.
// os_overlay_reload() picks up a change and forgets what the panel shows of
// the overlays: the system menu calls it when it closes, having drawn over
// the app. It draws nothing: the screen the menu gives back shows the
// overlays as the app's last present left them (a flush() app's counter at
// the old corner and value, in its restored front buffer), and the new
// setting shows from the app's next present. Nothing repaints them on close:
// the pixels under a counter drawn into a flush() frame are gone, so an old
// box cannot be erased cleanly, and a value read over the menu's pause would
// be wrong anyway.
int os_overlay_fps_mode(void);
void os_overlay_reload(void);
const char *os_overlay_fps_key(int mode);    // NULL (Off: no key), "tr", ...
const char *os_overlay_fps_label(int mode);  // "Off", "Top right", ...

// Whether the running app shows the OS counter: the setting as read at its
// launch and when the system menu last closed. perf.drawFPS draws nothing
// while it does (perf.h), so the screen shows one counter.
bool os_overlay_fps_on(void);
