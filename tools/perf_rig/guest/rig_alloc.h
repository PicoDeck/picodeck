// The stub allocator gfx3d.c is built against (GFX3D_MALLOC), in QMI PSRAM.
#pragma once
#include <stddef.h>
void *rig_malloc(size_t n);
void rig_free(void *p);
