#include "font_registry.h"
#include "font_6x8.h"
#include "font_8x12.h"
#include "font_scientifica.h"
#include "sdcard.h"
#include "umm_malloc.h"
#include <stdio.h>

#define LOADED_SLOTS FONT_REGISTRY_LOADED

// The built-in tables below are one byte per glyph row, so their stride column
// is hardcoded to 1. Widen a built-in font past 8 px and this must be revisited.
_Static_assert((FONT_6X8_WIDTH + 7) / 8 == 1, "font_6x8 no longer has stride 1");
_Static_assert((FONT_8X12_WIDTH + 7) / 8 == 1, "font_8x12 no longer has stride 1");
_Static_assert((FONT_SCI_WIDTH + 7) / 8 == 1, "font_scientifica no longer has stride 1");

static const pc_font_t s_builtin[FONT_REGISTRY_BUILTIN] = {
  { 0x20, 0x7E, FONT_6X8_HEIGHT,  FONT_6X8_WIDTH,  1, NULL, &font_6x8[0][0],             NULL },
  { 0x20, 0x7E, FONT_8X12_HEIGHT, FONT_8X12_WIDTH, 1, NULL, &font_8x12[0][0],            NULL },
  { FONT_SCI_FIRST, FONT_SCI_LAST, FONT_SCI_HEIGHT, FONT_SCI_WIDTH, 1, NULL, &font_scientifica[0][0],      NULL },
  { FONT_SCI_FIRST, FONT_SCI_LAST, FONT_SCI_HEIGHT, FONT_SCI_WIDTH, 1, NULL, &font_scientifica_bold[0][0], NULL },
};

static pc_font_t s_loaded[LOADED_SLOTS];
static bool      s_live[LOADED_SLOTS];

const pc_font_t *font_registry_get(int id) {
  if (id >= 0 && id < FONT_REGISTRY_BUILTIN) return &s_builtin[id];
  int i = id - FONT_REGISTRY_BUILTIN;
  if (i < 0 || i >= LOADED_SLOTS || !s_live[i]) return NULL;
  return &s_loaded[i];
}

int font_registry_load(const char *path) {
  return font_registry_load_ex(path, NULL);
}

int font_registry_load_ex(const char *path, const char **why) {
  const char *dummy;
  if (!why) why = &dummy;
  if (!path) { *why = "no path"; return -1; }
  int slot = -1;
  for (int i = 0; i < LOADED_SLOTS; i++) {
    if (!s_live[i]) { slot = i; break; }
  }
  if (slot < 0) {
    printf("[FONT] WARNING: load %s refused: all %d font slots are in use\n",
           path, LOADED_SLOTS);
    *why = FONT_REGISTRY_WHY_FULL;
    return -1;
  }
  int len = 0;
  char *blob = sdcard_read_file(path, &len);
  if (!blob) {
    printf("[FONT] load %s: cannot read file\n", path);
    *why = "cannot read file";
    return -1;
  }
  pc_font_t f;
  if (!font_from_blob(&f, blob, (size_t)len)) {
    printf("[FONT] load %s: not a valid .pfn (%d bytes)\n", path, len);
    umm_free(blob);
    *why = "not a valid .pfn font";
    return -1;
  }
  f.blob = blob;
  s_loaded[slot] = f;
  s_live[slot] = true;
  // Every .pfn carries a widths table, so a non-NULL `widths` proves nothing:
  // the font is only proportional if some advance differs from the cell width.
  bool proportional = false;
  if (f.widths) {
    for (int i = 0, n = font_glyph_count(&f); i < n; i++) {
      if (f.widths[i] != f.max_width) { proportional = true; break; }
    }
  }
  printf("[FONT] loaded %s -> id %d (%dx%d, %d glyphs%s)\n", path,
         FONT_REGISTRY_BUILTIN + slot, f.max_width, f.height,
         font_glyph_count(&f), proportional ? ", proportional" : "");
  return FONT_REGISTRY_BUILTIN + slot;
}

void font_registry_unload(int id) {
  int i = id - FONT_REGISTRY_BUILTIN;
  if (i < 0 || i >= LOADED_SLOTS || !s_live[i]) return;
  umm_free(s_loaded[i].blob);
  s_loaded[i].blob = NULL;
  s_live[i] = false;
}

void font_registry_unload_all(void) {
  for (int i = 0; i < LOADED_SLOTS; i++)
    font_registry_unload(FONT_REGISTRY_BUILTIN + i);
}
