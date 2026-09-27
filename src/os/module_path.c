#include "module_path.h"
#include "fs_path.h"
#include <string.h>

#define MODULE_NAME_MAX 128
#define MODULE_PART_MAX 64

bool module_path(const char *name, size_t len, const char *base, char *out,
                 size_t out_size) {
  if (!name || !base || !out || len == 0 || len > MODULE_NAME_MAX) return false;
  size_t blen = strlen(base);
  size_t need = blen + 1 + len + 4 + 1;  // base '/' name ".lua" NUL
  if (need > out_size) return false;
  size_t start = 0;
  for (size_t i = 0; i <= len; i++) {
    if (i == len || name[i] == '.') {
      if (!fs_name_valid(name + start, i - start, MODULE_PART_MAX)) return false;
      start = i + 1;
    }
  }
  memcpy(out, base, blen);
  out[blen] = '/';
  for (size_t i = 0; i < len; i++)
    out[blen + 1 + i] = name[i] == '.' ? '/' : name[i];
  memcpy(out + blen + 1 + len, ".lua", 5);
  return true;
}
