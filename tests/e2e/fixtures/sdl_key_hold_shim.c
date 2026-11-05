// LD_PRELOAD shim for tests/e2e/test_menu_key_hold.py: feeds the simulator a
// held host key the way SDL reports one on a desktop: a KEYDOWN (repeat 0),
// then KEYDOWN with repeat 1 every 40 ms after 500 ms, then the KEYUP.
//
// KEYHOLD_TRIGGER  path: the hold starts when this file exists
// KEYHOLD_SYM      SDL keycode (default SDLK_F10)
// KEYHOLD_MS       how long the key stays down (default 2000)
// KEYHOLD_LAST_REPEAT  set: one more repeat right before the KEYUP, handed over
//                  in the same drain of the queue, so both land in one poll
//
// <trigger>.down and <trigger>.up appear as the KEYDOWN and the KEYUP are handed
// over, so a test waits for the key itself instead of guessing how long the
// simulator takes to get to it.
//
// Build: cc -shared -fPIC -o shim.so sdl_key_hold_shim.c $(pkg-config --cflags sdl2) -ldl
#define _GNU_SOURCE
#include <SDL2/SDL.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static long now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static int s_state;  // 0 idle, 1 holding, 2 done
static int s_last_repeat_sent;
static long s_start, s_last_ev, s_last_check;

static void make_key(SDL_Event *e, Uint32 type, int repeat, SDL_Keycode sym) {
  memset(e, 0, sizeof(*e));
  e->type = type;
  e->key.state = type == SDL_KEYDOWN ? SDL_PRESSED : SDL_RELEASED;
  e->key.repeat = repeat;
  e->key.keysym.sym = sym;
  e->key.keysym.scancode = SDL_GetScancodeFromKey(sym);
}

static void ack(const char *trigger, const char *what) {
  char path[4096];
  snprintf(path, sizeof(path), "%s.%s", trigger, what);
  int fd = open(path, O_CREAT | O_WRONLY, 0644);
  if (fd >= 0)
    close(fd);
}

int SDL_PollEvent(SDL_Event *event) {
  static int (*real)(SDL_Event *);
  if (!real)
    real = (int (*)(SDL_Event *))dlsym(RTLD_NEXT, "SDL_PollEvent");
  const char *trig = getenv("KEYHOLD_TRIGGER");
  if (trig && event) {
    long t = now_ms();
    long hold_ms = getenv("KEYHOLD_MS") ? atol(getenv("KEYHOLD_MS")) : 2000;
    SDL_Keycode sym = getenv("KEYHOLD_SYM") ? (SDL_Keycode)atol(getenv("KEYHOLD_SYM"))
                                            : SDLK_F10;
    if (s_state == 0 && t - s_last_check >= 20) {
      s_last_check = t;
      if (access(trig, F_OK) == 0) {
        s_state = 1;
        s_start = s_last_ev = t;
        make_key(event, SDL_KEYDOWN, 0, sym);
        ack(trig, "down");
        return 1;
      }
    } else if (s_state == 1) {
      if (t - s_start >= hold_ms) {
        if (getenv("KEYHOLD_LAST_REPEAT") && !s_last_repeat_sent) {
          s_last_repeat_sent = 1;
          make_key(event, SDL_KEYDOWN, 1, sym);
          return 1;
        }
        s_state = 2;
        make_key(event, SDL_KEYUP, 0, sym);
        ack(trig, "up");
        return 1;
      }
      if (t - s_start >= 500 && t - s_last_ev >= 40) {
        s_last_ev = t;
        make_key(event, SDL_KEYDOWN, 1, sym);
        return 1;
      }
    }
  }
  return real(event);
}
