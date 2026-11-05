#include "idle_dim.h"
#include "../drivers/keyboard.h"

#include "pico/time.h"

// Backlight level while dimmed. Low enough to stop burn-in but the screen
// stays faintly readable so the device doesn't look switched off.
#define IDLE_DIM_LEVEL 10

static uint32_t s_timeout_ms = 60000; // 0 = disabled
static uint32_t s_last_activity_ms = 0;
static uint8_t s_user_brightness = 128;
static int16_t s_app_brightness = -1; // the running app's level, -1 = none
static bool s_menu_open = false;      // the system menu shows at the user's level
static bool s_dimmed = false;

// The level the panel has when it is not dimmed.
static uint8_t awake_level(void) {
  return (s_app_brightness >= 0 && !s_menu_open) ? (uint8_t)s_app_brightness
                                                 : s_user_brightness;
}

static uint8_t dim_level(void) {
  uint8_t level = awake_level();
  return level < IDLE_DIM_LEVEL ? level : IDLE_DIM_LEVEL;
}

static void apply_level(void) {
  kbd_set_backlight(s_dimmed ? dim_level() : awake_level());
}

void idle_dim_init(uint8_t user_brightness, uint32_t timeout_s) {
  s_user_brightness = user_brightness;
  s_app_brightness = -1;
  s_menu_open = false;
  s_timeout_ms = timeout_s * 1000u;
  s_last_activity_ms = to_ms_since_boot(get_absolute_time());
  s_dimmed = false;
}

void idle_dim_set_brightness(uint8_t brightness) {
  s_user_brightness = brightness;
  s_app_brightness = -1; // the user's choice wins over the app's
  s_dimmed = false; // the user is actively adjusting — screen is awake
}

void idle_dim_set_app_brightness(uint8_t brightness) {
  s_app_brightness = brightness;
  apply_level();
}

void idle_dim_clear_app_brightness(void) {
  if (s_app_brightness < 0)
    return;
  s_app_brightness = -1;
  apply_level();
}

void idle_dim_menu_open(bool open) {
  s_menu_open = open;
  if (s_app_brightness >= 0)
    apply_level();
}

bool idle_dim_note_activity(void) {
  s_last_activity_ms = to_ms_since_boot(get_absolute_time());
  if (s_dimmed) {
    s_dimmed = false;
    kbd_set_backlight(awake_level());
    return true;
  }
  return false;
}

void idle_dim_poll(void) {
  if (s_dimmed || s_timeout_ms == 0)
    return;
  uint32_t now = to_ms_since_boot(get_absolute_time());
  if (now - s_last_activity_ms >= s_timeout_ms) {
    s_dimmed = true;
    kbd_set_backlight(dim_level());
  }
}

bool idle_dim_is_dimmed(void) { return s_dimmed; }
