#ifndef LEAF_STACK_H
#define LEAF_STACK_H

#include <stdbool.h>
#include <stdint.h>

#define LEAF_STACK_BYTES 4096u

// Experiment (CMake PICODECK_SRAM_LEAF_STACK, off by default; firmware only):
// run a hot C function on a small SRAM stack instead of the app's PSRAM stack,
// so its frames neither take XIP cache lines nor write back to the QMI.
//
// fn must not raise: no luaL_error, nothing that can allocate on the Lua heap
// and fail, no yield. A longjmp out of it restores SP to the PSRAM stack and
// leaves PSPLIM on the SRAM stack, and the next push is a STKOF fault. It must
// not call back into Lua code either, and runs on Core 0 only.
void leaf_stack_call(void (*fn)(void *), void *arg);

// True if psp is on the leaf stack (pc_prof reads frames from it).
bool leaf_stack_contains(uint32_t psp, uint32_t bytes);

// Peak bytes used since the first call, 0 before it.
uint32_t leaf_stack_peak(void);

#endif
