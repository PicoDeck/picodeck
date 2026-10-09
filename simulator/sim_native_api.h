#ifndef SIM_NATIVE_API_H
#define SIM_NATIVE_API_H

// The simulator's native-app state, shared by the two ways it runs a native
// app: Unicorn on the desktop (unicorn_trampolines.c) and, in the web build,
// a WebAssembly side module that calls g_api directly (PicoDeck/web-sim's
// platform/web_native.c).

#include <stdbool.h>

// A native app's sys->poll(): poll the keyboard, run a queued dev command,
// then take an exit request (the system menu's Exit App, or the dev `exit`
// command).
void sim_native_poll(void);

// A native app's sys->shouldExit(): true once per exit request (it clears
// the request), as on firmware.
bool sim_native_should_exit(void);

// Around one native app run: each clears the exit request.
void sim_native_app_begin(void);
void sim_native_app_end(void);

#endif
