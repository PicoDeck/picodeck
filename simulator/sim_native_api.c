// sim_native_api.c: native-app state the Unicorn trampolines and the web
// build's compiled-in apps share (see sim_native_api.h).

#include "sim_native_api.h"

#include <stdbool.h>

// The simulator's drivers, declared as unicorn_trampolines.c declares them
// (their headers pull in firmware-only code).
extern void kbd_poll(void);
extern bool kbd_consume_menu_press(void);
extern bool system_menu_show_for_native(void);
extern void dev_commands_poll(void);
extern bool dev_commands_process(void);
extern bool dev_commands_wants_exit(void);
extern void dev_commands_clear_exit(void);

// Set by the system menu's Exit App or the dev `exit` command; read, and
// cleared, by the app's shouldExit(), as firmware's sys_shouldExit()
// (src/main.c) does.
static volatile bool s_exit;

void sim_native_poll(void) {
    kbd_poll();
    if (kbd_consume_menu_press() && system_menu_show_for_native())
        s_exit = true;
    // Run a queued control-channel command (`dev_command` RPC) on this
    // thread, as the firmware's sys_poll() runs the serial console's. The
    // simulator's dev handlers have no reboot flags, so an `exit` is all that
    // can come out of one.
    dev_commands_poll();
    dev_commands_process();
    if (dev_commands_wants_exit()) {
        s_exit = true;
        dev_commands_clear_exit();
    }
}

bool sim_native_should_exit(void) {
    bool v = s_exit;
    s_exit = false;
    return v;
}

void sim_native_app_begin(void) { s_exit = false; }

void sim_native_app_end(void) { s_exit = false; }
