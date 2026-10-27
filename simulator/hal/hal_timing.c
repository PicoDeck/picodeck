// HAL Timing - SDL2 Implementation
//
// Two clocks:
//
// Wall clock (default): hal_get_time_ms() is SDL_GetTicks(); the time
// multiplier only scales hal_sleep_ms()/hal_sleep_us() delays.
//
// Virtual clock (--virtual-time, test mode only; hal_timing_set_virtual()
// before hal_timing_init()). Every thread reads the same monotonic virtual
// clock, but only the clock's owner — the thread that called
// hal_timing_init(), i.e. Core 0 (launcher, Lua VM, native apps) — moves it:
//   - An owner sleep_ms(n)/sleep_us(n) advances the clock by n and then
//     waits n / multiplier real time (default multiplier 50), so a
//     sys.sleep(5000) costs ~100 ms of wall clock and reads back exactly
//     5000 ms of getTimeMs().
//   - A stretch longer than SIM_VT_BUSY_THRESH_US with no owner sleep (a busy
//     loop, a long decode) also advances it, at min(multiplier, 1) x real
//     time, less the threshold — busy loops that poll the clock progress, and
//     the short compute between sleeps adds nothing (so timings are exact).
//   - multiplier 0 pauses the clock: busy stretches add nothing and an owner
//     sleep waits (in <= 10 ms real slices) without advancing it, so loops
//     such as sys.sleep() block until hal_time_step() moves the clock.
//   - hal_time_step(ms) (the step_time RPC) advances it from any thread.
// Other threads (Core 1: audio, network; the socket thread) sleep in real
// time and never move the clock. Code on them that measures time (firmware
// http.c/tcp.c/wifi.c timeouts in the SIM_FIRMWARE_NET build) therefore sees
// virtual time run ahead of its real I/O: virtual time is for tests of app
// timing, not for audio playback or network suites, which keep the wall
// clock (the harness leaves it off unless a test asks for it).

#include "hal_timing.h"
#include <SDL2/SDL.h>
#include <pthread.h>
#include <stdbool.h>
#include <time.h>
#ifdef __EMSCRIPTEN__
#include "web_platform.h"
#endif

static int g_debug_mode = 0;
static float s_time_multiplier = 1.0f;

// ── Virtual clock ───────────────────────────────────────────────────────────

#define SIM_VT_DEFAULT_MULTIPLIER 50.0f
#define SIM_VT_BUSY_THRESH_US 50000ULL
#define SIM_VT_PAUSED_SLICE_US 10000ULL

static bool s_vt_requested = false;  // set before init (--virtual-time)
static bool s_vt_on = false;         // fixed after hal_timing_init()
static pthread_t s_vt_owner;
static pthread_mutex_t s_vt_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_vt_cv = PTHREAD_COND_INITIALIZER;
static uint64_t s_vt_us;       // virtual time at the last sync
static uint64_t s_vt_sync_us;  // real time of the last sync

static uint64_t real_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

// Virtual "now": the synced value plus the busy stretch past the threshold.
static uint64_t vt_now_locked(uint64_t real) {
    if (real <= s_vt_sync_us)  // inside an owner's paced sleep
        return s_vt_us;
    uint64_t gap = real - s_vt_sync_us;
    if (gap <= SIM_VT_BUSY_THRESH_US)
        return s_vt_us;
    float rate = s_time_multiplier < 1.0f ? s_time_multiplier : 1.0f;
    return s_vt_us + (uint64_t)((double)(gap - SIM_VT_BUSY_THRESH_US) * rate);
}

static void vt_sync_locked(uint64_t real) {
    if (real <= s_vt_sync_us)  // inside an owner's paced sleep: nothing to add
        return;
    s_vt_us = vt_now_locked(real);
    s_vt_sync_us = real;
}

static void sleep_real_us(uint64_t us) {
    if (us == 0)
        return;
    struct timespec ts = { (time_t)(us / 1000000ULL),
                           (long)((us % 1000000ULL) * 1000ULL) };
    nanosleep(&ts, NULL);
}

// An owner sleep of `us` virtual microseconds.
static void vt_owner_sleep(uint64_t us) {
    pthread_mutex_lock(&s_vt_mu);
    vt_sync_locked(real_us());
    float m = s_time_multiplier;
    if (m <= 0.0f) {
        // Paused: wait for a step (or a short slice) without advancing.
        uint64_t wait = us < SIM_VT_PAUSED_SLICE_US ? us : SIM_VT_PAUSED_SLICE_US;
        if (wait == 0)
            wait = 1000;
        struct timespec dl;
        clock_gettime(CLOCK_REALTIME, &dl);
        uint64_t ns = (uint64_t)dl.tv_nsec + wait * 1000ULL;
        dl.tv_sec += (time_t)(ns / 1000000000ULL);
        dl.tv_nsec = (long)(ns % 1000000000ULL);
        pthread_cond_timedwait(&s_vt_cv, &s_vt_mu, &dl);
        vt_sync_locked(real_us());  // adds nothing while paused
        pthread_mutex_unlock(&s_vt_mu);
        return;
    }
    s_vt_us += us;
    // The paced wait is not busy time: start the next busy stretch where the
    // wait is due to end (readers see no busy gain until then).
    uint64_t wait_us = (uint64_t)((double)us / m);
    s_vt_sync_us += wait_us;
    pthread_mutex_unlock(&s_vt_mu);
    sleep_real_us(wait_us);
}

static bool vt_is_owner(void) {
    return pthread_equal(pthread_self(), s_vt_owner);
}

void hal_timing_set_virtual(bool on) { s_vt_requested = on; }
bool hal_timing_is_virtual(void) { return s_vt_on; }

bool hal_time_step(uint32_t ms, uint64_t *now_ms) {
    if (!s_vt_on)
        return false;
    pthread_mutex_lock(&s_vt_mu);
    vt_sync_locked(real_us());
    s_vt_us += (uint64_t)ms * 1000ULL;
    if (now_ms)
        *now_ms = s_vt_us / 1000ULL;
    pthread_cond_broadcast(&s_vt_cv);
    pthread_mutex_unlock(&s_vt_mu);
    return true;
}

// ── Public API ──────────────────────────────────────────────────────────────

void hal_set_time_multiplier(float m) {
    if (m < 0.0f)
        m = 0.0f;
    if (!s_vt_on) {
        s_time_multiplier = m;
        return;
    }
    // Close the current stretch at the old rate before changing it.
    pthread_mutex_lock(&s_vt_mu);
    vt_sync_locked(real_us());
    s_time_multiplier = m;
    pthread_cond_broadcast(&s_vt_cv);
    pthread_mutex_unlock(&s_vt_mu);
}

float hal_get_time_multiplier(void) { return s_time_multiplier; }

void hal_timing_init(void) {
    // SDL timer is initialized with SDL_Init
    if (s_vt_requested && !s_vt_on) {
        s_vt_owner = pthread_self();
        // Continue from the wall clock so time never runs backwards.
        s_vt_us = (uint64_t)SDL_GetTicks() * 1000ULL;
        s_vt_sync_us = real_us();
        s_time_multiplier = SIM_VT_DEFAULT_MULTIPLIER;
        s_vt_on = true;
    }
}

uint32_t hal_get_time_ms(void) {
    return (uint32_t)(hal_get_time_us() / 1000ULL);
}

uint64_t hal_get_time_us(void) {
    if (!s_vt_on)
        return (uint64_t)SDL_GetTicks() * 1000ULL;
    pthread_mutex_lock(&s_vt_mu);
    uint64_t now = vt_now_locked(real_us());
    pthread_mutex_unlock(&s_vt_mu);
    return now;
}

void hal_sleep_ms(uint32_t ms) {
    if (s_vt_on) {
        if (vt_is_owner())
            vt_owner_sleep((uint64_t)ms * 1000ULL);
        else
            SDL_Delay(ms);  // Core 1 / helper threads: real time
        return;
    }
    if (s_time_multiplier <= 0.0f) return;  // 0 = paused (skip delay)
#ifdef __EMSCRIPTEN__
    web_yield((uint32_t)(ms / s_time_multiplier));
#else
    SDL_Delay((uint32_t)(ms / s_time_multiplier));
#endif
}

void hal_sleep_us(uint64_t us) {
    if (s_vt_on && vt_is_owner()) {
        vt_owner_sleep(us);
        return;
    }
#ifdef __EMSCRIPTEN__
    // No busy-waiting in a browser tab: short waits only yield when overdue.
    if (us < 1000) web_yield_if_due();
    else web_yield((uint32_t)(us / 1000));
    return;
#endif
    if (us < 100) {
        // Busy-wait for sub-100μs precision (max 99μs spin)
        uint64_t start = SDL_GetPerformanceCounter();
        uint64_t freq = SDL_GetPerformanceFrequency();
        uint64_t target = start + (us * freq) / 1000000ULL;
        while (SDL_GetPerformanceCounter() < target) {
            // Busy wait
        }
    } else {
        SDL_Delay((uint32_t)(us / 1000));
    }
}

void hal_set_debug_mode(int enabled) {
    g_debug_mode = enabled;
}

int hal_get_debug_mode(void) {
    return g_debug_mode;
}
