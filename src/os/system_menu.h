#pragma once
#include <stdbool.h>
#include "lua.h"

// =============================================================================
// System Menu Overlay
//
// An OS-level menu triggered by the F10 key (BTN_F 0x90).
// When invoked, it pauses the running Lua app, draws a translucent overlay
// with built-in items (Brightness, Battery, WiFi, Reboot, Exit App) plus any
// app-registered custom items, and resumes the app on dismiss.
//
// The menu is triggered via a Lua instruction-count hook installed in
// lua_bridge_register(), so no changes to the app run loop are required.
// =============================================================================

#define SYSMENU_MAX_APP_ITEMS 4

// Call once at OS init (before launcher_run).
void system_menu_init(void);

// Register an app item (called via picocalc.sys.addMenuItem in Lua).
// At most SYSMENU_MAX_APP_ITEMS items; extras are silently ignored.
void system_menu_add_item(const char *label,
                          void (*callback)(void *user), void *user);

// Option rows: "Label: value <>" with Left/Right/Enter stepping the value.
#define SYSMENU_OPT_LABEL_MAX 12
#define SYSMENU_OPT_VALUE_MAX 14
#define SYSMENU_OPT_MAX 16

// Register an option row (shares the SYSMENU_MAX_APP_ITEMS slots). Copies
// label and options. current is 0-based. callback(user, index) runs after the
// menu closes when the row's value changed during that visit. Returns false
// when no slot is free, an argument is out of range, or memory is short.
bool system_menu_add_option(const char *label, const char *const *options,
                            int n, int current,
                            void (*callback)(void *user, int index),
                            void *user);

// Remove all app-registered items.
// Called automatically by launcher.c after each app exits.
void system_menu_clear_items(void);

// Show the menu synchronously. Blocks until the user dismisses or acts.
// Raises exit sentinel (via lua_bridge_raise_exit) when "Exit App" is selected —
// this propagates to lua_pcall in launcher.c exactly like picocalc.sys.exit().
void system_menu_show(lua_State *L);

// Show the menu for native (non-Lua) apps. Blocks until dismissed.
// Returns true if the user selected "Exit App".
bool system_menu_show_for_native(void);

// Returns true if WiFi should auto-disconnect after NTP sync (default: true).
bool system_menu_get_wifi_auto_disconnect(void);
