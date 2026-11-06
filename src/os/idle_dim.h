#pragma once

#include <stdbool.h>
#include <stdint.h>

// Idle screen dimming — drops the backlight to a very low level after a
// period with no input activity, to prevent burn-in on the IPS panel.
// Any input restores the user's brightness (the waking press is swallowed
// by the keyboard driver so it never reaches the running app). So does an app
// launch (launcher.c), or the app's first key would be spent waking the screen.

// timeout_s: seconds of no input before dimming (0 disables dimming).
void idle_dim_init(uint8_t user_brightness, uint32_t timeout_s);

// Inform the module of a user brightness change (system menu slider) so the
// correct level is restored on wake. It also ends an app's own level, below.
void idle_dim_set_brightness(uint8_t brightness);

// The running app's own level (display.setBrightness, native
// display->setBrightness). It drives the backlight and is what a wake restores,
// until the app exits or the user changes the setting; dimmed, the screen stays
// dim. The user's setting and the saved config do not change.
void idle_dim_set_app_brightness(uint8_t brightness);

// The app is gone (or its error screen must be readable): the user's level.
void idle_dim_clear_app_brightness(void);

// The system menu opened or closed: it shows at the user's level, so an app
// that set 0 cannot hide the way out, and the app's level returns afterwards.
void idle_dim_menu_open(bool open);

// Note input (or other "keep awake") activity. Restores the backlight if
// currently dimmed. Returns true if this call woke the screen — the caller
// should swallow the waking key event.
bool idle_dim_note_activity(void);

// Periodic check; dims the backlight once the idle timeout expires.
// Cheap (one time compare) — call from the input poll path.
void idle_dim_poll(void);

// True while the screen is dimmed.
bool idle_dim_is_dimmed(void);
