#include "gamepad.h"
#include "gamepad_map.h"
#include "app_identity.h"
#include "app_stack.h"
#include "../drivers/keyboard.h"

#include <stdio.h>

static void apply_on_stack(void *arg) {
  (void)arg;
  kbd_padmap_t map;
  const app_identity_t *me = app_identity_current();
  gamepad_load_effective(&map, me ? me->id : NULL);
  kbd_set_pad_map(&map);
}

void gamepad_apply(void) {
  if (!app_stack_run_os(apply_on_stack, NULL)) {
    // No PSRAM for the stack: the app would not start either, but never
    // leave the previous app's override in place.
    printf("[GAMEPAD] no stack to read the bindings: defaults\n");
    kbd_padmap_t map;
    gamepad_map_defaults(&map);
    kbd_set_pad_map(&map);
  }
}

const char *gamepad_get_label(uint32_t pad_button, int slot) {
  return gamepad_map_label(kbd_get_pad_map(), pad_button, slot);
}
