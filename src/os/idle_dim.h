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
// correct level is restored on wake.
void idle_dim_set_brightness(uint8_t brightness);

// Note input (or other "keep awake") activity. Restores the backlight if
// currently dimmed. Returns true if this call woke the screen — the caller
// should swallow the waking key event.
bool idle_dim_note_activity(void);

// Periodic check; dims the backlight once the idle timeout expires.
// Cheap (one time compare) — call from the input poll path.
void idle_dim_poll(void);

// True while the screen is dimmed.
bool idle_dim_is_dimmed(void);
