// HAL Pad - the host's SDL game controllers as a gamepad source (hal_pad.h).

#include "hal_pad.h"
#include "../../src/drivers/pad_source.h"
#include "../../src/os/os.h" // PAD_*

#include <stdio.h>

#ifndef __EMSCRIPTEN__

// Controllers open at once (more are ignored until one goes).
#define HAL_PAD_MAX 4
// The left stick drives the D-pad past half way.
#define HAL_PAD_STICK_DEADZONE 16384

static bool s_enabled;
static SDL_GameController *s_pads[HAL_PAD_MAX];

static const struct {
  SDL_GameControllerButton sdl;
  uint32_t pad;
} k_buttons[] = {
    {SDL_CONTROLLER_BUTTON_DPAD_UP, PAD_UP},
    {SDL_CONTROLLER_BUTTON_DPAD_DOWN, PAD_DOWN},
    {SDL_CONTROLLER_BUTTON_DPAD_LEFT, PAD_LEFT},
    {SDL_CONTROLLER_BUTTON_DPAD_RIGHT, PAD_RIGHT},
    {SDL_CONTROLLER_BUTTON_A, PAD_A},
    {SDL_CONTROLLER_BUTTON_B, PAD_B},
    {SDL_CONTROLLER_BUTTON_X, PAD_X},
    {SDL_CONTROLLER_BUTTON_Y, PAD_Y},
    {SDL_CONTROLLER_BUTTON_LEFTSHOULDER, PAD_L},
    {SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, PAD_R},
    {SDL_CONTROLLER_BUTTON_START, PAD_START},
    {SDL_CONTROLLER_BUTTON_BACK, PAD_SELECT},
    {SDL_CONTROLLER_BUTTON_GUIDE, PAD_SOURCE_HOME},
};

static uint32_t pad_state(SDL_GameController *gc) {
  uint32_t st = 0;
  for (size_t i = 0; i < sizeof(k_buttons) / sizeof(k_buttons[0]); i++)
    if (SDL_GameControllerGetButton(gc, k_buttons[i].sdl))
      st |= k_buttons[i].pad;
  int x = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTX);
  int y = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTY);
  if (x <= -HAL_PAD_STICK_DEADZONE)
    st |= PAD_LEFT;
  if (x >= HAL_PAD_STICK_DEADZONE)
    st |= PAD_RIGHT;
  if (y <= -HAL_PAD_STICK_DEADZONE)
    st |= PAD_UP;
  if (y >= HAL_PAD_STICK_DEADZONE)
    st |= PAD_DOWN;
  return st;
}

// Publish every open controller's state as one, or disconnect the source
// when none is open. The state is read from SDL at the time of the event (the
// events say only that something changed). A controller being unplugged is
// skipped: SDL queues its "recentering" events ahead of the removal, and by
// the time they are handled its state can read as a stray press.
static void publish(void) {
  bool any = false;
  uint32_t st = 0;
  for (int i = 0; i < HAL_PAD_MAX; i++) {
    if (!s_pads[i])
      continue;
    any = true;
    if (SDL_GameControllerGetAttached(s_pads[i]))
      st |= pad_state(s_pads[i]);
  }
  if (any)
    pad_source_publish(PAD_SOURCE_HOST, st);
  else
    pad_source_disconnect(PAD_SOURCE_HOST);
}

static int slot_of(SDL_JoystickID id) {
  for (int i = 0; i < HAL_PAD_MAX; i++) {
    if (!s_pads[i])
      continue;
    SDL_Joystick *j = SDL_GameControllerGetJoystick(s_pads[i]);
    if (j && SDL_JoystickInstanceID(j) == id)
      return i;
  }
  return -1;
}

static void open_pad(int device_index) {
  if (!SDL_IsGameController(device_index))
    return;
  SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(device_index);
  if (slot_of(id) >= 0)
    return; // already open (SDL reports the boot-time devices once more)
  for (int i = 0; i < HAL_PAD_MAX; i++) {
    if (s_pads[i])
      continue;
    s_pads[i] = SDL_GameControllerOpen(device_index);
    if (!s_pads[i]) {
      printf("[PAD] Could not open controller %d: %s\n", device_index,
             SDL_GetError());
      return;
    }
    const char *name = SDL_GameControllerName(s_pads[i]);
    printf("[PAD] Connected: %s\n", name ? name : "(unnamed controller)");
    fflush(stdout);
    publish();
    return;
  }
  printf("[PAD] Ignoring controller %d: %d already open\n", device_index,
         HAL_PAD_MAX);
}

static void close_pad(SDL_JoystickID id) {
  int i = slot_of(id);
  if (i < 0)
    return;
  SDL_GameControllerClose(s_pads[i]);
  s_pads[i] = NULL;
  printf("[PAD] Disconnected\n");
  fflush(stdout);
  publish(); // its buttons are released; the source goes with the last pad
}

void hal_pad_init(bool enable) {
  s_enabled = false;
  if (!enable) {
    printf("[PAD] Host game controllers off\n");
    return;
  }
  if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) < 0) {
    printf("[PAD] SDL game controller init failed: %s\n", SDL_GetError());
    return;
  }
  s_enabled = true;
  printf("[PAD] Host game controllers on\n");
}

void hal_pad_shutdown(void) {
  if (!s_enabled)
    return;
  for (int i = 0; i < HAL_PAD_MAX; i++) {
    if (s_pads[i])
      SDL_GameControllerClose(s_pads[i]);
    s_pads[i] = NULL;
  }
  pad_source_disconnect(PAD_SOURCE_HOST);
  SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER);
  s_enabled = false;
}

bool hal_pad_handle_event(const SDL_Event *event) {
  switch (event->type) {
  case SDL_CONTROLLERDEVICEADDED:
    if (s_enabled)
      open_pad(event->cdevice.which);
    return true;
  case SDL_CONTROLLERDEVICEREMOVED:
    if (s_enabled)
      close_pad(event->cdevice.which);
    return true;
  case SDL_CONTROLLERBUTTONDOWN:
  case SDL_CONTROLLERBUTTONUP:
  case SDL_CONTROLLERAXISMOTION:
    if (s_enabled)
      publish();
    return true;
  default:
    return false;
  }
}

#else // __EMSCRIPTEN__: the web demo has no controller support.

void hal_pad_init(bool enable) { (void)enable; }
void hal_pad_shutdown(void) {}
bool hal_pad_handle_event(const SDL_Event *event) {
  (void)event;
  return false;
}

#endif
