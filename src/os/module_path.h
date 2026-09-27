#pragma once
#include <stdbool.h>
#include <stddef.h>

// Builds "<base>/<a>/<b>.lua" from the dotted module name "a.b" (name[0..len),
// the caller's byte count, so an embedded NUL is refused). The name is 1..128
// bytes; '.' separates parts; every part must be fs_name_valid (1..64 bytes of
// [A-Za-z0-9_-] here, since '.' cannot appear inside a part). Returns false if
// the name is invalid or the path does not fit in out_size bytes.
bool module_path(const char *name, size_t len, const char *base, char *out,
                 size_t out_size);
