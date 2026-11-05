// The guest's doorbells to the host (tools/perf_rig/rig.c): writes to this page
// are seen by the memory hook and cost nothing in the model.
#pragma once
#include <stdint.h>
#define RIG_BASE 0x4FF00000u
#define RIG_REG(off) (*(volatile uint32_t *)(RIG_BASE + (off)))
#define RIG_PHASE(p) (RIG_REG(0x00) = (p))  // 0 idle, 1 geometry, 2 raster
#define RIG_DONE() (RIG_REG(0x04) = 1)
#define RIG_FRAME_START() (RIG_REG(0x08) = 1)  // the host models what ran between frames
#define RIG_FRAME_END(fb) (RIG_REG(0x0C) = (uint32_t)(fb))
#define RIG_STAT(i, v) (RIG_REG(0x20 + 4 * (i)) = (v))
enum { PHASE_IDLE, PHASE_GEOM, PHASE_RASTER };
