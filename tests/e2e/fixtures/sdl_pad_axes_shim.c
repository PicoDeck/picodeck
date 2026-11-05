// LD_PRELOAD shim for tests/e2e/test_gamepad_axes.py: one fake SDL game
// controller, so hal_pad.c's real path (SDL_GameControllerGetAxis -> the host
// pad source) runs with no controller plugged in.
//
// PADAXES_FILE  path: "lx ly rx ry lt rt", SDL's raw values. The controller is
//               plugged in when the file first exists, and every change of
//               its contents is an axis motion event.
//
// Build: cc -shared -fPIC -o shim.so sdl_pad_axes_shim.c $(pkg-config --cflags sdl2) -ldl
#define _GNU_SOURCE
#include <SDL2/SDL.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SHIM_INSTANCE 77

static int s_fake; // what the handles point at: never read
static int s_attached, s_values[6];
static long s_last_check;
static char s_last_text[128];

static long now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

SDL_bool SDL_IsGameController(int device_index) {
  (void)device_index;
  return SDL_TRUE;
}

SDL_JoystickID SDL_JoystickGetDeviceInstanceID(int device_index) {
  (void)device_index;
  return SHIM_INSTANCE;
}

SDL_GameController *SDL_GameControllerOpen(int device_index) {
  (void)device_index;
  return (SDL_GameController *)&s_fake;
}

const char *SDL_GameControllerName(SDL_GameController *gc) {
  (void)gc;
  return "Shim Pad";
}

SDL_bool SDL_GameControllerGetAttached(SDL_GameController *gc) {
  (void)gc;
  return SDL_TRUE;
}

SDL_Joystick *SDL_GameControllerGetJoystick(SDL_GameController *gc) {
  (void)gc;
  return (SDL_Joystick *)&s_fake;
}

SDL_JoystickID SDL_JoystickInstanceID(SDL_Joystick *j) {
  (void)j;
  return SHIM_INSTANCE;
}

void SDL_GameControllerClose(SDL_GameController *gc) { (void)gc; }

Uint8 SDL_GameControllerGetButton(SDL_GameController *gc,
                                  SDL_GameControllerButton b) {
  (void)gc;
  (void)b;
  return 0;
}

Sint16 SDL_GameControllerGetAxis(SDL_GameController *gc,
                                 SDL_GameControllerAxis axis) {
  (void)gc;
  switch (axis) {
  case SDL_CONTROLLER_AXIS_LEFTX:
    return (Sint16)s_values[0];
  case SDL_CONTROLLER_AXIS_LEFTY:
    return (Sint16)s_values[1];
  case SDL_CONTROLLER_AXIS_RIGHTX:
    return (Sint16)s_values[2];
  case SDL_CONTROLLER_AXIS_RIGHTY:
    return (Sint16)s_values[3];
  case SDL_CONTROLLER_AXIS_TRIGGERLEFT:
    return (Sint16)s_values[4];
  case SDL_CONTROLLER_AXIS_TRIGGERRIGHT:
    return (Sint16)s_values[5];
  default:
    return 0;
  }
}

int SDL_PollEvent(SDL_Event *event) {
  static int (*real)(SDL_Event *);
  if (!real)
    real = (int (*)(SDL_Event *))dlsym(RTLD_NEXT, "SDL_PollEvent");
  const char *path = getenv("PADAXES_FILE");
  long t = now_ms();
  if (path && event && t - s_last_check >= 10) {
    s_last_check = t;
    char text[128] = "";
    FILE *f = fopen(path, "r");
    if (f) {
      size_t n = fread(text, 1, sizeof(text) - 1, f);
      text[n] = '\0';
      fclose(f);
      if (text[0] && strcmp(text, s_last_text) != 0) {
        strcpy(s_last_text, text);
        sscanf(text, "%d %d %d %d %d %d", &s_values[0], &s_values[1],
               &s_values[2], &s_values[3], &s_values[4], &s_values[5]);
        memset(event, 0, sizeof(*event));
        if (!s_attached) {
          s_attached = 1;
          event->type = SDL_CONTROLLERDEVICEADDED;
          event->cdevice.which = 0;
        } else {
          event->type = SDL_CONTROLLERAXISMOTION;
          event->caxis.which = SHIM_INSTANCE;
        }
        return 1;
      }
    }
  }
  return real(event);
}
