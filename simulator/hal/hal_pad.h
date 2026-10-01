// HAL Pad - the host's SDL game controllers as a gamepad source.
//
// Every open controller is ORed into one source, PAD_SOURCE_HOST
// (src/drivers/pad_source.h), published whole at every controller event on
// Core 0 (the keyboard stub's SDL event pump). Hotplug: a controller plugged
// in is opened, and the source is disconnected (its buttons released) when
// the last one is unplugged.
//
// Mapping (SDL's positional names: A is the bottom face button):
//   D-pad, or the left stick past half way  -> Up / Down / Left / Right
//   A / B / X / Y                           -> A / B / X / Y
//   left / right shoulder                   -> L / R
//   Start / Back                            -> Start / Select
//   Guide (Home)                            -> the system menu
// Triggers and the right stick are not mapped (analog input is out of scope).

#ifndef HAL_PAD_H
#define HAL_PAD_H

#include <SDL2/SDL.h>
#include <stdbool.h>
#include <stdint.h>

// Start the SDL game controller subsystem when `enable` (the simulator
// passes false under --test-mode or --no-gamepad, so a test never sees the
// host's controllers, and in the web build). Controllers already plugged in
// arrive as SDL_CONTROLLERDEVICEADDED events.
void hal_pad_init(bool enable);
void hal_pad_shutdown(void);

// Handle a controller event (device added / removed, button, axis). Returns
// true when the event was one; hal_input_handle_event calls it first.
bool hal_pad_handle_event(const SDL_Event *event);

#endif // HAL_PAD_H
