#pragma once

// ======================================================================
// The console gate: keeps log output out of a serial transfer.
//
// A dev-console transfer shares its port with the log. A printf landing inside
// the binary `screenshot` stream shifts every later pixel, and one landing
// inside a base64 line loses the line. Those printfs come from everywhere: Core
// 1 (the WiFi, HTTP and TCP drivers, Mongoose, the audio players), Core 0's
// interrupts (BTstack and the CYW43 driver run there), and a crash on either
// core. All of them reach the port through one place, pico_stdio's driver
// list, so the gate sits there: console_gate_init wraps the out_chars and
// out_flush of the SDK's stdio_usb and stdio_uart drivers. A log call never
// waits for a transfer: one the gate stops returns at once.
//
// While a transfer runs, output it stops is DROPPED and counted, not held: a
// held line would need a buffer on a core that may be in an interrupt, and SRAM
// is full. console_gate_open returns how many bytes went, and dev_commands.c
// reports them in a "[DEV] Console: ..." line when the transfer ends, so a gap
// in the log is visible. The log is advisory (the harness reads results from
// files); the record of a crash is not, and is kept by these rules:
//   - HardFault and panic() open the gate first (console_gate_force_open), so
//     a crash report prints even if that tears the frame being sent: the host's
//     checksum catches the frame, and the device is rebooting anyway.
//   - /system/crashlog.txt comes from the scratch registers (main.c hardfault_c)
//     and /system/error.log from Core 0's thread, and neither passes through
//     here.
//
// The SDK's own print and USB locks are a separate hazard, and console_gate.c
// wraps how it waits for them, for any log line, transfer or none: Core 1 polls
// (the SDK's wait keeps Core 0 in its alarm interrupt), and an interrupt doesn't
// wait at all, so its line is lost on USB if another core is printing.
// ======================================================================

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    CONSOLE_GATE_OPEN = 0,
    // The transfer's own printfs carry it (screenshot64, getb64, the ACKs of
    // putb64), on both USB and UART: it cannot know which one the host is on.
    // Only Core 0's thread mode writes, which is the transfer: during a send
    // that thread is inside it, and during a receive the replies and a running
    // app's log both pass, at printf granularity, for a host that reads lines.
    CONSOLE_GATE_LINES,
    // Raw bytes on CDC carry it (screenshot, get), written with
    // console_gate_raw_write. Nothing else touches USB, not even Core 0's
    // thread; UART is not carrying the transfer and keeps logging.
    CONSOLE_GATE_RAW,
} console_gate_mode_t;

// Wraps the drivers. Call right after stdio_init_all, before Core 1 starts.
void console_gate_init(void);

// Closes the gate for a transfer (Core 0 thread mode only). Waits for a printf
// already in flight on the other core, by taking the SDK's print mutex (bounded
// by its 1 s deadlock timeout), so the transfer's first byte follows the whole
// of that line and none of it comes after.
void console_gate_close(console_gate_mode_t mode);

// Opens the gate and returns the bytes dropped since it closed.
uint32_t console_gate_open(void);

// Opens the gate from a fault or a panic, on either core: just the store.
void console_gate_force_open(void);

bool console_gate_is_closed(void);

// Bytes for a RAW transfer, through the SDK's own USB write. That write holds
// stdio_usb_mutex, which is what keeps TinyUSB's background task (an interrupt
// on Core 0) out of these calls. Called from thread mode without it, TinyUSB can
// be interrupted holding one of its mutexes, and the interrupt then waits for
// that mutex on the core that holds it, for good. A host that stops reading
// makes the write give up after PICO_STDIO_USB_STDOUT_TIMEOUT_US and drop the
// rest of the buffer; a closed port drops it at once.
void console_gate_raw_write(const uint8_t *data, uint32_t len);
void console_gate_raw_flush(void);

// Since boot: transfers the gate has closed for, bytes it has dropped.
void console_gate_stats(uint32_t *transfers, uint32_t *dropped_bytes);
