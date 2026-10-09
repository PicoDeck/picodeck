#pragma once
// test_charshader's allocator for the chargrid and shader code under test
// (included into each of its sources by tests/unit/CMakeLists.txt): malloc,
// counted, and refused on demand (test_charshader.c defines it).
#include <stddef.h>
#include <stdlib.h>

void *cg_test_malloc(size_t n);
#define CG_MALLOC cg_test_malloc
#define CG_FREE free
