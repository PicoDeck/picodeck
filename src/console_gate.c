#include "console_gate.h"

#include "pico.h"
#include "pico/stdio/driver.h"
#include "pico/stdio_uart.h"
#include "pico/stdio_usb.h"

// The SDK's print mutex, held for a whole printf (pico_stdio/stdio.c; in no
// header).
extern bool stdout_serialize_begin(void);
extern void stdout_serialize_end(void);

// volatile: written on Core 0 (or by a fault on either core), read on both.
static volatile console_gate_mode_t s_mode = CONSOLE_GATE_OPEN;

// One writer each (a core), so no lock; a count an interrupt steps on is only a
// count.
static volatile uint32_t s_dropped[2];
static uint32_t s_dropped_at_close;
static uint32_t s_transfers;

static void (*s_usb_out_chars)(const char *buf, int len);
static void (*s_usb_out_flush)(void);
static void (*s_uart_out_chars)(const char *buf, int len);

static uint32_t dropped_total(void) {
    return s_dropped[0] + s_dropped[1];
}

static bool usb_blocked(void) {
    console_gate_mode_t mode = s_mode;
    if (mode == CONSOLE_GATE_OPEN)
        return false;
    return mode == CONSOLE_GATE_RAW || get_core_num() != 0 ||
           __get_current_exception() != 0;
}

static bool uart_blocked(void) {
    return s_mode == CONSOLE_GATE_LINES &&
           (get_core_num() != 0 || __get_current_exception() != 0);
}

static void usb_out_chars(const char *buf, int len) {
    if (usb_blocked()) {
        s_dropped[get_core_num()] += (uint32_t)len;
        return;
    }
    s_usb_out_chars(buf, len);
}

// Every printf flushes all drivers, whatever out_chars did. The SDK's USB flush
// runs tud_task: on Core 1 or in an interrupt that would be TinyUSB under the
// transfer, and it would wait for stdio_usb_mutex for as long as a write holds
// it.
static void usb_out_flush(void) {
    if (!usb_blocked())
        s_usb_out_flush();
}

// Stopped here, not counted: usb_out_chars has already counted the same bytes.
static void uart_out_chars(const char *buf, int len) {
    if (!uart_blocked())
        s_uart_out_chars(buf, len);
}

void console_gate_init(void) {
    s_usb_out_chars = stdio_usb.out_chars;
    s_usb_out_flush = stdio_usb.out_flush;
    s_uart_out_chars = stdio_uart.out_chars;
    stdio_usb.out_chars = usb_out_chars;
    stdio_usb.out_flush = usb_out_flush;
    stdio_uart.out_chars = uart_out_chars;
}

void console_gate_close(console_gate_mode_t mode) {
    bool locked = stdout_serialize_begin();
    s_mode = mode;
    if (locked)
        stdout_serialize_end();
    s_dropped_at_close = dropped_total();
    s_transfers++;
}

uint32_t console_gate_open(void) {
    s_mode = CONSOLE_GATE_OPEN;
    return dropped_total() - s_dropped_at_close;
}

void console_gate_force_open(void) {
    s_mode = CONSOLE_GATE_OPEN;
}

bool console_gate_is_closed(void) {
    return s_mode != CONSOLE_GATE_OPEN;
}

void console_gate_raw_write(const uint8_t *data, uint32_t len) {
    s_usb_out_chars((const char *)data, (int)len);
}

void console_gate_raw_flush(void) {
    s_usb_out_flush();
}

void console_gate_stats(uint32_t *transfers, uint32_t *dropped_bytes) {
    *transfers = s_transfers;
    *dropped_bytes = dropped_total();
}

#ifndef PICODECK_HOST_TEST
#include "pico/mutex.h"
#include "pico/time.h"

// Linked with --wrap (CMakeLists.txt) for the SDK's lock waits. Its version adds
// an alarm to the default pool and cancels it, and each of those forces the
// pool's interrupt, which runs on Core 0. So Core 1 waiting for a lock that
// Core 0's thread holds, which is what a log line from each core at once comes
// to, kept Core 0 in that interrupt and its thread from ever releasing the lock:
// both cores froze for each 1 s lock timeout, and a Core 1 that kept logging ran
// out the 10 s watchdog. Core 1 polls instead, in RAM, so the wait costs Core 0
// nothing. (time.c's own calls, sleep_ms and the like, are not wrapped: they end
// when their time is up, whoever holds what.)
extern bool __real_best_effort_wfe_or_timeout(absolute_time_t timeout_timestamp);
bool __time_critical_func(__wrap_best_effort_wfe_or_timeout)(absolute_time_t timeout_timestamp) {
    if (get_core_num() == 0)
        return __real_best_effort_wfe_or_timeout(timeout_timestamp);
    tight_loop_contents();
    return time_reached(timeout_timestamp);
}

// Linked with --wrap too, for the SDK's print and USB locks (its only callers). In
// an interrupt, take the lock if it is free and never wait: that interrupt may
// have preempted the holder of the USB lock on this core, while Core 1 holds the
// print lock and waits for that USB lock. Then each of the three waits on the
// next, until the 1 s timeouts, and a repeating timer that logs comes straight
// back to wait again. A line that finds a lock taken is lost on USB.
extern bool __real_mutex_try_enter_block_until(mutex_t *mtx, absolute_time_t until);
bool __time_critical_func(__wrap_mutex_try_enter_block_until)(mutex_t *mtx, absolute_time_t until) {
    if (__get_current_exception())
        return mutex_try_enter(mtx, NULL);
    return __real_mutex_try_enter_block_until(mtx, until);
}

// panic(), linked with --wrap=panic (CMakeLists.txt): open the gate, then the
// SDK's own, its arguments untouched. Without this a panic during a transfer
// would lose its message: the bkpt it ends in reaches hardfault_c, which keeps
// the crash record, but not the text.
void __attribute__((naked, noreturn)) __wrap_panic(const char *fmt, ...) {
    __asm volatile("push {r0, r1, r2, r3, r12, lr}\n"
                   "bl   console_gate_force_open\n"
                   "pop  {r0, r1, r2, r3, r12, lr}\n"
                   "b    __real_panic\n");
}
#endif
