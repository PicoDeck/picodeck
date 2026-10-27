#pragma once

#include <stdbool.h>
#include <stddef.h>

// The dev `mv <src> <dst>` (dev_op_mv in dev_ops.c; host-tested on a FatFS
// RAM disk in tests/unit/test_mv_op.c). `args` is "<src> <dst>" and is
// modified. Reply "Moved: <src> -> <dst>" or "Error: mv ...".
bool mv_op(char *args, char *reply, size_t n);
