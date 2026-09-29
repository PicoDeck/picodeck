#include "gamepad.h"
#include "gamepad_map.h"
#include "../drivers/keyboard.h"

const char *gamepad_get_label(uint32_t pad_button, int slot) {
  return gamepad_map_label(kbd_get_pad_map(), pad_button, slot);
}
