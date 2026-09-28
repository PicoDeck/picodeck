// pico/time.h stub for simulator

#ifndef PICO_TIME_H
#define PICO_TIME_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// Forward declarations of HAL timing functions
extern void hal_sleep_ms(uint32_t ms);
extern void hal_sleep_us(uint64_t us);
extern uint64_t hal_get_time_us(void);

// Use HAL timing functions
#define sleep_ms(ms) hal_sleep_ms(ms)
#define sleep_us(us) hal_sleep_us(us)

// absolute_time_t is just uint64_t microseconds
typedef uint64_t absolute_time_t;

static inline absolute_time_t get_absolute_time(void) {
    return hal_get_time_us();
}

static inline uint64_t time_us_64(void) {
    return hal_get_time_us();
}

static inline uint32_t time_us_32(void) {
    return (uint32_t)hal_get_time_us();
}

static inline uint32_t to_ms_since_boot(absolute_time_t t) {
    return (uint32_t)(t / 1000);
}

static inline uint64_t to_us_since_boot(absolute_time_t t) {
    return (uint64_t)t;
}

static inline int64_t absolute_time_diff_us(absolute_time_t from, absolute_time_t to) {
    return (int64_t)(to - from);
}

static inline bool is_at_the_end_of_time(absolute_time_t t) {
    return t == UINT64_MAX;
}

static inline absolute_time_t at_the_end_of_time(void) {
    return UINT64_MAX;
}

static inline absolute_time_t nil_time(void) {
    return 0;
}

static inline bool is_nil_time(absolute_time_t t) {
    return t == 0;
}

// Alarm pool type (dummy)
typedef struct {
    int dummy;
} alarm_pool_t;

// Alarm-related functions (stubs)
static inline void alarm_pool_init_default(void) {}
static inline alarm_pool_t* alarm_pool_get_default(void) { return NULL; }
static inline int alarm_pool_add_alarm_in_ms(alarm_pool_t* pool, uint32_t ms, void* callback, void* user_data, bool fire_if_past) {
    (void)pool; (void)ms; (void)callback; (void)user_data; (void)fire_if_past;
    return 0;
}
static inline bool alarm_pool_cancel_alarm(alarm_pool_t* pool, int alarm_id) {
    (void)pool; (void)alarm_id;
    return false;
}

// Repeating timer (stub)
struct repeating_timer {
    uint32_t delay_ms;
    void* callback;
    void* user_data;
};

static inline bool add_repeating_timer_ms(int32_t delay_ms, void* callback, void* user_data, struct repeating_timer* out) {
    (void)delay_ms; (void)callback; (void)user_data;
    if (out) {
        out->delay_ms = delay_ms;
        out->callback = callback;
        out->user_data = user_data;
    }
    return true;
}

static inline bool cancel_repeating_timer(struct repeating_timer* timer) {
    (void)timer;
    return true;
}

// busy_wait functions
static inline void busy_wait_ms(uint32_t ms) {
    sleep_ms(ms);
}

static inline void busy_wait_us(uint32_t us) {
    sleep_us(us);
}

static inline void busy_wait_until(absolute_time_t t) {
    while (get_absolute_time() < t) {
        // Busy wait
    }
}

#endif // PICO_TIME_H
