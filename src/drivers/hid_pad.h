#pragma once

// =============================================================================
// HID gamepad reports -> PAD_* (pure; host-tested in tests/unit/test_hid_pad.c,
// fuzzed by tests/fuzz/fuzz_hid_pad.c).
//
// A Bluetooth HID pad (bt_pad.c) describes its reports with a HID report
// descriptor (from SDP). hid_pad_parse() reads the descriptor once, at
// connect, into a small table of the Input fields a gamepad needs: the
// buttons (Button page, as ranges or lists), the hat switch, the left stick
// (Generic Desktop X and Y, the first pair in a report) and a Home button
// (Generic Desktop System Main Menu, or Consumer AC Home). hid_pad_decode()
// then turns each input report into the source's PAD_* state
// (pad_source.h: PAD_* | PAD_SOURCE_HOME).
//
// Which HID button is which PAD_* button depends on the pad (the descriptor
// only numbers them), so a profile picks the table, by the pad's Bluetooth
// name (hid_pad_profile_for_name). The face buttons are mapped by position,
// as on an Xbox pad and as the simulator's SDL controllers are: PAD_A is the
// bottom button, PAD_B the right one, PAD_X the left one, PAD_Y the top
// one. On a Nintendo-layout pad that means PAD_A is the button labelled B.
//
// The stick drives the D-pad past half way from the centre (the simulator's
// threshold too); the hat and the stick are ORed.
// =============================================================================

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HID_PAD_MAX_FIELDS 24

typedef enum {
  HID_PAD_F_BUTTONS = 1, // `count` 1-element buttons from button `usage` up
  HID_PAD_F_BUTTON_ARRAY,// `count` slots holding button numbers
  HID_PAD_F_HAT,         // hat switch (8 or 4 positions)
  HID_PAD_F_X,           // left stick X
  HID_PAD_F_Y,           // left stick Y
  HID_PAD_F_HOME,        // Home / guide / System Main Menu (1 bit)
} hid_pad_kind_t;

typedef struct {
  uint8_t kind;      // hid_pad_kind_t
  uint8_t report_id; // 0 when the descriptor uses no report IDs
  uint8_t size;      // bits per element (1-32)
  uint8_t count;     // elements
  uint16_t bit;      // first bit, in the report after its ID byte
  uint16_t usage;    // buttons: first button number; array: the range's first
  int32_t lmin, lmax;
} hid_pad_field_t;

typedef struct {
  hid_pad_field_t f[HID_PAD_MAX_FIELDS];
  uint8_t n;
  bool report_ids; // every report starts with its ID byte
} hid_pad_layout_t;

// Read a report descriptor. False when it holds no gamepad field (no
// buttons, hat or stick in any input report): not a pad this can read.
bool hid_pad_parse(const uint8_t *desc, size_t len, hid_pad_layout_t *out);

// The parser's working state (~330 B): hid_pad_parse keeps it on the stack;
// hid_pad_parse_with takes it from the caller (bt_pad.c parses inside a
// BTstack callback, on the 4 KB main stack, so its copy is in PSRAM).
#define HID_PAD_PARSE_USAGES 32  // local Usage items before one main item
#define HID_PAD_PARSE_REPORTS 16 // report IDs whose bit offsets are tracked
#define HID_PAD_PARSE_PUSH 4

typedef struct {
  uint16_t page;
  int32_t lmin, lmax;
  uint32_t lmax_u; // Logical Maximum read unsigned
  uint32_t size, count;
  uint8_t report_id;
} hid_pad_parse_globals_t;

typedef struct {
  hid_pad_parse_globals_t g, stack[HID_PAD_PARSE_PUSH];
  int depth;
  uint32_t usages[HID_PAD_PARSE_USAGES]; // page << 16 | id, bound when declared
  int n_usages;
  uint32_t umin, umax;
  bool has_min, has_max;
  struct {
    uint8_t id;
    uint16_t bits;
  } offs[HID_PAD_PARSE_REPORTS];
  int n_offs;
  hid_pad_layout_t *out;
} hid_pad_parser_t;

bool hid_pad_parse_with(hid_pad_parser_t *scratch, const uint8_t *desc,
                        size_t len, hid_pad_layout_t *out);

typedef enum {
  HID_PAD_PROFILE_GENERIC = 0, // Android / Xbox numbering (1 A, 2 B, 4 X, 5 Y)
  HID_PAD_PROFILE_SONY,        // DualShock 4, DualSense
  HID_PAD_PROFILE_NINTENDO,    // Switch Pro Controller (simple HID mode)
  HID_PAD_PROFILE_8BITDO,      // 8BitDo in D-input / Android mode
  HID_PAD_PROFILE_COUNT
} hid_pad_profile_t;

// The profile for a pad's Bluetooth name ("Wireless Controller", "Pro
// Controller", "8BitDo SN30 Pro", ...); GENERIC when nothing matches.
hid_pad_profile_t hid_pad_profile_for_name(const char *name);

const char *hid_pad_profile_label(hid_pad_profile_t p);

// The PAD_* bit (or PAD_SOURCE_HOME) HID button `button` (1-based) stands for
// in profile `p`; 0 when it has none.
uint32_t hid_pad_button_bit(hid_pad_profile_t p, unsigned button);

// One input report, as the pad sent it (its ID byte first when the layout
// uses report IDs). Writes the source state and returns true when the report
// holds any of the layout's fields; false (state untouched) for a report
// that is not the gamepad's (a vendor report, a short or unknown one).
bool hid_pad_decode(const hid_pad_layout_t *l, hid_pad_profile_t p,
                    const uint8_t *report, size_t len, uint32_t *state);
