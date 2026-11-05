// Host unit tests for src/console_gate.c, over fakes of the SDK's stdio drivers
// and print mutex (fakes/console_gate): who may write in each mode, that a
// write in flight finishes before the transfer's first byte, and the counts.
#include "check.h"
#include "console_gate.h"
#include "pico/stdio_uart.h"
#include "pico/stdio_usb.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <unistd.h>

_Thread_local unsigned g_fake_core = 0;
_Thread_local unsigned g_fake_exception = 0;

static int s_usb_bytes, s_usb_flushes, s_uart_bytes;

static void real_usb_out(const char *buf, int len) { (void)buf; s_usb_bytes += len; }
static void real_usb_flush(void) { s_usb_flushes++; }
static void real_uart_out(const char *buf, int len) { (void)buf; s_uart_bytes += len; }

stdio_driver_t stdio_usb = {real_usb_out, real_usb_flush};
stdio_driver_t stdio_uart = {real_uart_out, NULL};

// The SDK's print mutex.
static pthread_mutex_t s_print_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool s_print_mutex_times_out;
static int s_print_mutex_takes;

bool stdout_serialize_begin(void) {
    if (s_print_mutex_times_out)
        return false;
    pthread_mutex_lock(&s_print_mutex);
    s_print_mutex_takes++;
    return true;
}

void stdout_serialize_end(void) {
    pthread_mutex_unlock(&s_print_mutex);
}

static void reset_counts(void) {
    s_usb_bytes = s_usb_flushes = s_uart_bytes = 0;
}

// One printf as the SDK runs it: all drivers, then all flushes.
static void console_write(unsigned core, unsigned exception, int len) {
    g_fake_core = core;
    g_fake_exception = exception;
    stdio_usb.out_chars("x", len);
    stdio_uart.out_chars("x", len);
    stdio_usb.out_flush();
    g_fake_core = 0;
    g_fake_exception = 0;
}

static void expect(int usb, int flushes, int uart) {
    CHECK_EQ_INT(s_usb_bytes, usb);
    CHECK_EQ_INT(s_usb_flushes, flushes);
    CHECK_EQ_INT(s_uart_bytes, uart);
    reset_counts();
}

static void test_open_passes_everyone(void) {
    CHECK(!console_gate_is_closed());
    console_write(0, 0, 3);
    expect(3, 1, 3);
    console_write(1, 0, 3);
    expect(3, 1, 3);
    console_write(0, 15, 3);
    expect(3, 1, 3);
}

static void test_lines_passes_only_core0_thread(void) {
    console_gate_close(CONSOLE_GATE_LINES);
    CHECK(console_gate_is_closed());
    console_write(0, 0, 5); // the transfer's own line
    expect(5, 1, 5);
    console_write(1, 0, 7);  // Core 1
    expect(0, 0, 0);
    console_write(0, 31, 11); // a Core 0 interrupt: BTstack, the CYW43 driver
    expect(0, 0, 0);
    console_write(1, 16, 13); // a Core 1 interrupt
    expect(0, 0, 0);
    CHECK_EQ_INT((int)console_gate_open(), 7 + 11 + 13); // USB's count, not UART's too
    CHECK(!console_gate_is_closed());
    console_write(1, 0, 2);
    expect(2, 1, 2);
}

static void test_raw_keeps_everyone_off_usb(void) {
    console_gate_close(CONSOLE_GATE_RAW);
    console_write(0, 0, 5); // not even Core 0's thread: an SD error mid-`get`
    expect(0, 0, 5);
    console_write(1, 0, 7);
    expect(0, 0, 7); // UART is not carrying the transfer: it keeps the log
    console_write(0, 31, 11);
    expect(0, 0, 11);

    console_gate_raw_write((const uint8_t *)"abcd", 4); // the transfer itself
    console_gate_raw_flush();
    expect(4, 1, 0);

    CHECK_EQ_INT((int)console_gate_open(), 5 + 7 + 11);
}

static void test_dropped_counts_one_window_and_the_total(void) {
    uint32_t transfers0, dropped0, transfers, dropped;
    console_gate_stats(&transfers0, &dropped0);
    console_gate_close(CONSOLE_GATE_RAW);
    console_write(1, 0, 10);
    CHECK_EQ_INT((int)console_gate_open(), 10);
    reset_counts();
    console_gate_close(CONSOLE_GATE_RAW);
    CHECK_EQ_INT((int)console_gate_open(), 0);
    console_gate_stats(&transfers, &dropped);
    CHECK_EQ_INT((int)(transfers - transfers0), 2);
    CHECK_EQ_INT((int)(dropped - dropped0), 10);
}

static void test_force_open_lets_a_crash_report_out(void) {
    console_gate_close(CONSOLE_GATE_RAW);
    g_fake_core = 1;
    g_fake_exception = 3; // HardFault
    console_gate_force_open();
    CHECK(!console_gate_is_closed());
    console_write(1, 3, 9);
    expect(9, 1, 9);
    console_gate_close(CONSOLE_GATE_LINES);
    console_gate_force_open();
    console_write(1, 3, 9);
    expect(9, 1, 9);
    (void)console_gate_open();
}

static void test_close_goes_through_the_print_mutex(void) {
    s_print_mutex_takes = 0;
    console_gate_close(CONSOLE_GATE_RAW);
    CHECK_EQ_INT(s_print_mutex_takes, 1);
    (void)console_gate_open();
    CHECK_EQ_INT(pthread_mutex_trylock(&s_print_mutex), 0); // released
    pthread_mutex_unlock(&s_print_mutex);
    // The SDK's deadlock timeout: the gate still closes, and releases nothing.
    s_print_mutex_times_out = true;
    console_gate_close(CONSOLE_GATE_RAW);
    CHECK(console_gate_is_closed());
    (void)console_gate_open();
    s_print_mutex_times_out = false;
}

// A real "Core 1": printf after printf, each one slow, under the print mutex as
// the SDK holds it. Nothing of it may reach USB between close and open. Without
// the wait for the mutex, a write in flight when the gate closes lands inside.
static atomic_bool s_stop, s_in_transfer;
static atomic_int s_writes, s_torn;

static void slow_usb_out(const char *buf, int len) {
    (void)buf;
    (void)len;
    usleep(30);
    if (atomic_load(&s_in_transfer))
        atomic_fetch_add(&s_torn, 1);
    atomic_fetch_add(&s_writes, 1);
}

static void noop_flush(void) {}

static void *core1_thread(void *arg) {
    (void)arg;
    g_fake_core = 1;
    while (!atomic_load(&s_stop)) {
        stdout_serialize_begin();
        stdio_usb.out_chars("x", 1);
        stdio_usb.out_flush();
        stdout_serialize_end();
        usleep(1);
    }
    return NULL;
}

static void test_a_write_in_flight_finishes_before_the_transfer(void) {
    pthread_t t;
    atomic_store(&s_stop, false);
    atomic_store(&s_in_transfer, false);
    atomic_store(&s_writes, 0);
    atomic_store(&s_torn, 0);
    pthread_create(&t, NULL, core1_thread, NULL);
    for (int i = 0; i < 300; i++) {
        usleep(50);
        console_gate_close(CONSOLE_GATE_RAW);
        atomic_store(&s_in_transfer, true);
        usleep(100); // what would land here lands inside the frame
        atomic_store(&s_in_transfer, false);
        (void)console_gate_open();
    }
    atomic_store(&s_stop, true);
    pthread_join(t, NULL);
    CHECK_EQ_INT(atomic_load(&s_torn), 0);
    CHECK(atomic_load(&s_writes) > 0);
}

int main(void) {
    console_gate_init();
    test_open_passes_everyone();
    reset_counts();
    test_lines_passes_only_core0_thread();
    test_raw_keeps_everyone_off_usb();
    test_dropped_counts_one_window_and_the_total();
    test_force_open_lets_a_crash_report_out();
    test_close_goes_through_the_print_mutex();

    // A second init wraps these, as the first wrapped the counting ones.
    stdio_usb.out_chars = slow_usb_out;
    stdio_usb.out_flush = noop_flush;
    console_gate_init();
    test_a_write_in_flight_finishes_before_the_transfer();
    return check_report("test_console_gate");
}
