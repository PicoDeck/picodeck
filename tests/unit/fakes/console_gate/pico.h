// The little of the SDK src/console_gate.c uses, for tests/unit/test_console_gate.c:
// which core and which context "this thread" is pretending to be.
#pragma once
extern _Thread_local unsigned g_fake_core;
extern _Thread_local unsigned g_fake_exception;
static inline unsigned get_core_num(void) { return g_fake_core; }
static inline unsigned __get_current_exception(void) { return g_fake_exception; }
