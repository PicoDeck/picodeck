// HID gamepad reports -> PAD_* (see hid_pad.h). No hardware dependency: host
// tests in tests/unit/test_hid_pad.c, fuzz target tests/fuzz/fuzz_hid_pad.c.
#include "hid_pad.h"
#include "pad_source.h"   // PAD_SOURCE_HOME
#include "../os/os.h"     // PAD_*

#include <string.h>

// ── Descriptor items (HID 1.11, 6.2.2) ──────────────────────────────────────

#define PAGE_GENERIC_DESKTOP 0x01
#define PAGE_BUTTON 0x09
#define PAGE_CONSUMER 0x0C
#define GD_X 0x30
#define GD_Y 0x31
#define GD_HAT 0x39
#define GD_SYSTEM_MAIN_MENU 0x85
#define CONSUMER_AC_HOME 0x223

#define MAX_USAGES HID_PAD_PARSE_USAGES
#define MAX_REPORTS HID_PAD_PARSE_REPORTS
#define MAX_PUSH HID_PAD_PARSE_PUSH

typedef hid_pad_parse_globals_t globals_t;
typedef hid_pad_parser_t parser_t;

static uint32_t item_u(const uint8_t *d, int n) {
  uint32_t v = 0;
  for (int i = 0; i < n; i++)
    v |= (uint32_t)d[i] << (8 * i);
  return v;
}

static int32_t item_s(const uint8_t *d, int n) {
  uint32_t v = item_u(d, n);
  if (n == 1)
    return (int8_t)v;
  if (n == 2)
    return (int16_t)v;
  return (int32_t)v;
}

// The input bit offset of report `id` (created on first use; NULL when the
// table is full: that report's fields are ignored).
static uint16_t *report_bits(parser_t *p, uint8_t id) {
  for (int i = 0; i < p->n_offs; i++)
    if (p->offs[i].id == id)
      return &p->offs[i].bits;
  if (p->n_offs >= MAX_REPORTS)
    return NULL;
  p->offs[p->n_offs].id = id;
  p->offs[p->n_offs].bits = 0;
  return &p->offs[p->n_offs++].bits;
}

static void clear_locals(parser_t *p) {
  p->n_usages = 0;
  p->has_min = p->has_max = false;
}

// A local usage with no page of its own (1-2 data bytes) takes the page in
// effect where it is declared; a 4-byte one carries its page.
static uint32_t full_usage(const parser_t *p, uint32_t v, int n) {
  return n == 4 ? v : ((uint32_t)p->g.page << 16) | (v & 0xFFFF);
}

static hid_pad_field_t *add_field(parser_t *p, uint8_t kind) {
  hid_pad_layout_t *l = p->out;
  if (l->n >= HID_PAD_MAX_FIELDS)
    return NULL;
  hid_pad_field_t *f = &l->f[l->n++];
  memset(f, 0, sizeof(*f));
  f->kind = kind;
  f->report_id = p->g.report_id;
  f->size = (uint8_t)p->g.size;
  f->lmin = p->g.lmin;
  // Logical Maximum is signed, but a descriptor with a non-negative minimum
  // often writes 255 as the one byte 0xFF: read it unsigned then.
  f->lmax = (p->g.lmin >= 0 && p->g.lmax < 0) ? (int32_t)p->g.lmax_u
                                               : p->g.lmax;
  return f;
}

// The first of `kind` in this report wins (X/Y: the left stick).
static bool have_kind(const parser_t *p, uint8_t kind) {
  const hid_pad_layout_t *l = p->out;
  for (int i = 0; i < l->n; i++)
    if (l->f[i].kind == kind && l->f[i].report_id == p->g.report_id)
      return true;
  return false;
}

// Usage of element i of a variable main item.
static uint32_t element_usage(const parser_t *p, uint32_t i) {
  if (p->n_usages > 0)
    return p->usages[(int)i < p->n_usages ? (int)i : p->n_usages - 1];
  if (p->has_min) {
    uint32_t u = p->umin + i;
    if (p->has_max && u > p->umax)
      u = p->umax;
    return u;
  }
  return 0;
}

static void add_variable(parser_t *p, uint16_t bit) {
  uint32_t size = p->g.size;
  for (uint32_t i = 0; i < p->g.count; i++) {
    uint32_t u = element_usage(p, i);
    uint16_t page = (uint16_t)(u >> 16), id = (uint16_t)u;
    uint16_t at = (uint16_t)(bit + i * size);
    if (page == PAGE_BUTTON && id >= 1 && id <= 0xFF) {
      // Extend the previous run when this one carries on from it.
      hid_pad_layout_t *l = p->out;
      hid_pad_field_t *last = l->n ? &l->f[l->n - 1] : NULL;
      if (last && last->kind == HID_PAD_F_BUTTONS &&
          last->report_id == p->g.report_id && last->size == size &&
          last->count < 255 && last->bit + last->count * size == at &&
          last->usage + last->count == id) {
        last->count++;
        continue;
      }
      hid_pad_field_t *f = add_field(p, HID_PAD_F_BUTTONS);
      if (f) {
        f->bit = at;
        f->usage = id;
        f->count = 1;
      }
      continue;
    }
    uint8_t kind = 0;
    if (page == PAGE_GENERIC_DESKTOP && id == GD_X)
      kind = HID_PAD_F_X;
    else if (page == PAGE_GENERIC_DESKTOP && id == GD_Y)
      kind = HID_PAD_F_Y;
    else if (page == PAGE_GENERIC_DESKTOP && id == GD_HAT)
      kind = HID_PAD_F_HAT;
    else if ((page == PAGE_GENERIC_DESKTOP && id == GD_SYSTEM_MAIN_MENU) ||
             (page == PAGE_CONSUMER && id == CONSUMER_AC_HOME))
      kind = HID_PAD_F_HOME;
    if (!kind || have_kind(p, kind))
      continue;
    hid_pad_field_t *f = add_field(p, kind);
    if (f) {
      f->bit = at;
      f->count = 1;
    }
  }
}

// An array of button numbers (Usage Minimum/Maximum on the Button page).
static void add_array(parser_t *p, uint16_t bit) {
  if (!p->has_min || (p->umin >> 16) != PAGE_BUTTON || p->g.count > 255)
    return;
  hid_pad_field_t *f = add_field(p, HID_PAD_F_BUTTON_ARRAY);
  if (f) {
    f->bit = bit;
    f->count = (uint8_t)p->g.count;
    f->usage = (uint16_t)p->umin;
  }
}

static void main_input(parser_t *p, uint32_t flags) {
  uint16_t *bits = report_bits(p, p->g.report_id);
  if (!bits)
    return;
  uint32_t size = p->g.size, count = p->g.count;
  uint32_t total = (size <= 32 && count <= 0x8000) ? size * count : 0x10000;
  uint32_t at = *bits;
  if (at + total > 0xFFFF) { // past any real report: the rest is ignored
    *bits = 0xFFFF;
    return;
  }
  *bits = (uint16_t)(at + total);
  if (size == 0 || count == 0 || (flags & 0x01)) // Constant: padding
    return;
  if (flags & 0x02)
    add_variable(p, at);
  else
    add_array(p, at);
}

bool hid_pad_parse(const uint8_t *desc, size_t len, hid_pad_layout_t *out) {
  parser_t p;
  return hid_pad_parse_with(&p, desc, len, out);
}

bool hid_pad_parse_with(hid_pad_parser_t *scratch, const uint8_t *desc,
                        size_t len, hid_pad_layout_t *out) {
  static const uint8_t k_bytes[4] = {0, 1, 2, 4};
  parser_t *p = scratch;
  memset(p, 0, sizeof(*p));
  memset(out, 0, sizeof(*out));
  p->out = out;

  size_t i = 0;
  while (desc && i < len) {
    uint8_t b = desc[i++];
    if (b == 0xFE) { // long item: size, tag, data; nothing we read
      if (i + 2 > len)
        break;
      i += 2u + desc[i];
      continue;
    }
    int n = k_bytes[b & 3];
    if (i + (size_t)n > len)
      break;
    const uint8_t *d = desc + i;
    i += (size_t)n;
    uint32_t u = item_u(d, n);
    switch (b & 0xFC) {
    // Main
    case 0x80: // Input
      main_input(p, u);
      clear_locals(p);
      break;
    case 0x90: // Output
    case 0xB0: // Feature
    case 0xA0: // Collection
    case 0xC0: // End Collection
      clear_locals(p);
      break;
    // Global
    case 0x04:
      p->g.page = (uint16_t)u;
      break;
    case 0x14:
      p->g.lmin = item_s(d, n);
      break;
    case 0x24:
      p->g.lmax = item_s(d, n);
      p->g.lmax_u = u;
      break;
    case 0x74:
      p->g.size = u;
      break;
    case 0x84:
      p->g.report_id = (uint8_t)u;
      out->report_ids = true;
      break;
    case 0x94:
      p->g.count = u;
      break;
    case 0xA4: // Push
      if (p->depth < MAX_PUSH)
        p->stack[p->depth++] = p->g;
      break;
    case 0xB4: // Pop
      if (p->depth > 0)
        p->g = p->stack[--p->depth];
      break;
    // Local
    case 0x08:
      if (p->n_usages < MAX_USAGES)
        p->usages[p->n_usages++] = full_usage(p, u, n);
      break;
    case 0x18:
      p->umin = full_usage(p, u, n);
      p->has_min = true;
      break;
    case 0x28:
      p->umax = full_usage(p, u, n);
      p->has_max = true;
      break;
    default: // physical range, units, strings, delimiters: not needed
      break;
    }
  }
  return out->n > 0;
}

// ── Profiles ────────────────────────────────────────────────────────────────

// HID button number (index) -> PAD_* bit, per profile. Taken from the SDL
// GameControllerDB's Bluetooth mappings for these pads (joystick button
// N = HID button N + 1) and the pads' own reports: by position, PAD_A the
// bottom face button, PAD_B the right one, PAD_X the left, PAD_Y the top.
#define HB_MAX 16
static const uint32_t k_profile[HID_PAD_PROFILE_COUNT][HB_MAX + 1] = {
    // Android / Xbox: 1 A, 2 B, 4 X, 5 Y, 7 L1, 8 R1, 11 Select, 12 Start,
    // 13 Home (3, 6, 9, 10 are C, Z, L2, R2).
    [HID_PAD_PROFILE_GENERIC] = {[1] = PAD_A, [2] = PAD_B, [4] = PAD_X,
                                 [5] = PAD_Y, [7] = PAD_L, [8] = PAD_R,
                                 [11] = PAD_SELECT, [12] = PAD_START,
                                 [13] = PAD_SOURCE_HOME},
    // Sony: 1 Square, 2 Cross, 3 Circle, 4 Triangle, 5 L1, 6 R1, 9 Share /
    // Create, 10 Options, 13 PS (7, 8 L2/R2, 11, 12 L3/R3, 14 touchpad).
    [HID_PAD_PROFILE_SONY] = {[1] = PAD_X, [2] = PAD_A, [3] = PAD_B,
                              [4] = PAD_Y, [5] = PAD_L, [6] = PAD_R,
                              [9] = PAD_SELECT, [10] = PAD_START,
                              [13] = PAD_SOURCE_HOME},
    // Switch Pro (report 0x3F): 1 B, 2 A, 3 Y, 4 X, 5 L, 6 R, 9 Minus,
    // 10 Plus, 13 Home (7, 8 ZL/ZR, 11, 12 stick clicks, 14 Capture).
    [HID_PAD_PROFILE_NINTENDO] = {[1] = PAD_A, [2] = PAD_B, [3] = PAD_X,
                                  [4] = PAD_Y, [5] = PAD_L, [6] = PAD_R,
                                  [9] = PAD_SELECT, [10] = PAD_START,
                                  [13] = PAD_SOURCE_HOME},
    // 8BitDo D-input: Android numbering by label on a Nintendo layout: 1 A
    // (right), 2 B (bottom), 4 X (top), 5 Y (left).
    [HID_PAD_PROFILE_8BITDO] = {[1] = PAD_B, [2] = PAD_A, [4] = PAD_Y,
                                [5] = PAD_X, [7] = PAD_L, [8] = PAD_R,
                                [11] = PAD_SELECT, [12] = PAD_START,
                                [13] = PAD_SOURCE_HOME},
};

uint32_t hid_pad_button_bit(hid_pad_profile_t p, unsigned button) {
  if ((unsigned)p >= HID_PAD_PROFILE_COUNT || button == 0 || button > HB_MAX)
    return 0;
  return k_profile[p][button];
}

static bool name_has(const char *name, const char *what) {
  size_t n = strlen(what);
  for (const char *s = name; *s; s++) {
    size_t i = 0;
    while (i < n && s[i]) {
      char a = s[i], b = what[i];
      if (a >= 'A' && a <= 'Z')
        a = (char)(a - 'A' + 'a');
      if (b >= 'A' && b <= 'Z')
        b = (char)(b - 'A' + 'a');
      if (a != b)
        break;
      i++;
    }
    if (i == n)
      return true;
  }
  return false;
}

hid_pad_profile_t hid_pad_profile_for_name(const char *name) {
  if (!name || !name[0])
    return HID_PAD_PROFILE_GENERIC;
  // "Xbox Wireless Controller" before Sony's plain "Wireless Controller".
  if (name_has(name, "xbox"))
    return HID_PAD_PROFILE_GENERIC;
  if (name_has(name, "8bitdo"))
    return HID_PAD_PROFILE_8BITDO;
  if (strcmp(name, "Wireless Controller") == 0 ||
      name_has(name, "dualsense") || name_has(name, "dualshock"))
    return HID_PAD_PROFILE_SONY;
  if (name_has(name, "pro controller"))
    return HID_PAD_PROFILE_NINTENDO;
  return HID_PAD_PROFILE_GENERIC;
}

const char *hid_pad_profile_label(hid_pad_profile_t p) {
  switch (p) {
  case HID_PAD_PROFILE_SONY:
    return "PlayStation";
  case HID_PAD_PROFILE_NINTENDO:
    return "Nintendo";
  case HID_PAD_PROFILE_8BITDO:
    return "8BitDo";
  default:
    return "generic";
  }
}

// ── Reports ─────────────────────────────────────────────────────────────────

// `size` bits (1-32) from bit `bit` of `d` (little-endian, LSB first).
static uint32_t get_bits(const uint8_t *d, uint32_t bit, uint32_t size) {
  uint32_t v = 0;
  for (uint32_t i = 0; i < size; i++) {
    uint32_t b = bit + i;
    if (d[b >> 3] & (1u << (b & 7)))
      v |= 1u << i;
  }
  return v;
}

static int64_t field_value(const hid_pad_field_t *f, uint32_t raw) {
  if (f->lmin < 0 && f->size < 32 && (raw & (1u << (f->size - 1))))
    return (int64_t)raw - ((int64_t)1 << f->size);
  if (f->lmin < 0 && f->size == 32)
    return (int32_t)raw;
  return raw;
}

// The stick's direction on one axis: -1, 0 or +1, past half way from the
// centre of the logical range.
static int axis_dir(const hid_pad_field_t *f, int64_t v) {
  int64_t lo = f->lmin, hi = f->lmax;
  if (hi <= lo)
    return 0;
  int64_t span = hi - lo;           // full range
  int64_t off = 2 * (v - lo) - span; // 2 x distance from the centre
  if (2 * off <= -span)
    return -1;
  if (2 * off >= span)
    return 1;
  return 0;
}

static uint32_t hat_bits(const hid_pad_field_t *f, int64_t v) {
  static const uint32_t k8[8] = {
      PAD_UP,   PAD_UP | PAD_RIGHT,  PAD_RIGHT, PAD_DOWN | PAD_RIGHT,
      PAD_DOWN, PAD_DOWN | PAD_LEFT, PAD_LEFT,  PAD_UP | PAD_LEFT};
  static const uint32_t k4[4] = {PAD_UP, PAD_RIGHT, PAD_DOWN, PAD_LEFT};
  int64_t h = v - f->lmin, positions = (int64_t)f->lmax - f->lmin + 1;
  if (h < 0 || h >= positions) // the null state: centred
    return 0;
  if (positions == 8)
    return k8[h];
  if (positions == 4)
    return k4[h];
  return 0;
}

bool hid_pad_decode(const hid_pad_layout_t *l, hid_pad_profile_t p,
                    const uint8_t *report, size_t len, uint32_t *state) {
  if (!l || !report)
    return false;
  uint8_t id = 0;
  if (l->report_ids) {
    if (len < 1)
      return false;
    id = report[0];
    report++;
    len--;
  }
  // A report shorter than its fields is not read at all: half of one would
  // release the buttons it cut off.
  uint64_t have = (uint64_t)len * 8;
  bool matched = false;
  for (int i = 0; i < l->n; i++) {
    const hid_pad_field_t *f = &l->f[i];
    if (f->report_id != id)
      continue;
    if (f->size == 0 || f->size > 32 ||
        (uint64_t)f->bit + (uint64_t)f->size * f->count > have)
      return false;
    matched = true;
  }
  if (!matched)
    return false;
  uint32_t st = 0;
  int x = 0, y = 0;
  for (int i = 0; i < l->n; i++) {
    const hid_pad_field_t *f = &l->f[i];
    if (f->report_id != id)
      continue;
    switch (f->kind) {
    case HID_PAD_F_BUTTONS:
      for (unsigned k = 0; k < f->count; k++)
        if (get_bits(report, f->bit + k * f->size, f->size))
          st |= hid_pad_button_bit(p, f->usage + k);
      break;
    case HID_PAD_F_BUTTON_ARRAY:
      for (unsigned k = 0; k < f->count; k++) {
        int64_t v = field_value(f, get_bits(report, f->bit + k * f->size,
                                            f->size));
        if (v >= f->lmin && v <= f->lmax)
          st |= hid_pad_button_bit(p, (unsigned)(f->usage + (v - f->lmin)));
      }
      break;
    case HID_PAD_F_HAT:
      st |= hat_bits(f, field_value(f, get_bits(report, f->bit, f->size)));
      break;
    case HID_PAD_F_X:
      x = axis_dir(f, field_value(f, get_bits(report, f->bit, f->size)));
      break;
    case HID_PAD_F_Y:
      y = axis_dir(f, field_value(f, get_bits(report, f->bit, f->size)));
      break;
    case HID_PAD_F_HOME:
      if (get_bits(report, f->bit, f->size))
        st |= PAD_SOURCE_HOME;
      break;
    default:
      break;
    }
  }
  if (x < 0)
    st |= PAD_LEFT;
  if (x > 0)
    st |= PAD_RIGHT;
  if (y < 0)
    st |= PAD_UP;
  if (y > 0)
    st |= PAD_DOWN;
  *state = st;
  return true;
}
