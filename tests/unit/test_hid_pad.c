// Host unit tests for src/drivers/hid_pad.c: a Bluetooth pad's HID report
// descriptor read into a gamepad layout, and its input reports turned into
// PAD_* state. Descriptors: tests/unit/hid_pad_fixtures.h (captures from
// real pads, and two written from documented layouts, marked as such).
#include "check.h"
#include "hid_pad.h"
#include "hid_pad_fixtures.h"
#include "pad_source.h"
#include "../os/os.h"

#include <stdlib.h>

#define LEN(a) (sizeof(a) / sizeof((a)[0]))

static hid_pad_layout_t s_l;

// Decode one report; 0xDEAD when the decoder refused it.
static uint32_t dec(hid_pad_profile_t p, const uint8_t *r, size_t n) {
  uint32_t st = 0xDEADu;
  if (!hid_pad_decode(&s_l, p, r, n, &st))
    return 0xDEADu;
  return st;
}

static int count_kind(uint8_t id, uint8_t kind) {
  int n = 0;
  for (int i = 0; i < s_l.n; i++)
    if (s_l.f[i].report_id == id && s_l.f[i].kind == kind)
      n++;
  return n;
}

// ── DualShock 4 over Bluetooth: report 0x01 ─────────────────────────────────
// [1] 0x01, [2..5] LX LY RX RY, [6] hat (low nibble) + Square Cross Circle
// Triangle (bits 4-7), [7] L1 R1 L2 R2 Share Options L3 R3, [8] PS, touchpad
// (bits 0-1) + counter, [9..10] L2 R2 analog.

static void ds4(uint8_t *r, uint8_t hat, uint16_t buttons, uint8_t lx,
                uint8_t ly) {
  memset(r, 0, 10);
  r[0] = 0x01;
  r[1] = lx;
  r[2] = ly;
  r[3] = 0x80;
  r[4] = 0x80;
  r[5] = (uint8_t)((hat & 0x0F) | ((buttons & 0x0F) << 4));
  r[6] = (uint8_t)(buttons >> 4);
  r[7] = (uint8_t)((buttons >> 12) & 0x03) | 0xA4; // counter bits set
}

#define DS4_SQUARE (1u << 0)
#define DS4_CROSS (1u << 1)
#define DS4_CIRCLE (1u << 2)
#define DS4_TRIANGLE (1u << 3)
#define DS4_L1 (1u << 4)
#define DS4_R1 (1u << 5)
#define DS4_L2 (1u << 6)
#define DS4_R2 (1u << 7)
#define DS4_SHARE (1u << 8)
#define DS4_OPTIONS (1u << 9)
#define DS4_L3 (1u << 10)
#define DS4_R3 (1u << 11)
#define DS4_PS (1u << 12)
#define DS4_TOUCH (1u << 13)

static void test_ds4_layout(void) {
  CHECK(hid_pad_parse(k_ds4_bt, sizeof(k_ds4_bt), &s_l));
  CHECK(s_l.report_ids);
  CHECK_EQ_INT(count_kind(1, HID_PAD_F_X), 1);
  CHECK_EQ_INT(count_kind(1, HID_PAD_F_Y), 1);
  CHECK_EQ_INT(count_kind(1, HID_PAD_F_HAT), 1);
  CHECK_EQ_INT(count_kind(1, HID_PAD_F_BUTTONS), 1);
  // Only report 1 is a gamepad report (0x11 etc. are vendor-defined).
  for (int i = 0; i < s_l.n; i++)
    CHECK_EQ_INT(s_l.f[i].report_id, 1);
  for (int i = 0; i < s_l.n; i++) {
    const hid_pad_field_t *f = &s_l.f[i];
    if (f->kind == HID_PAD_F_BUTTONS) {
      CHECK_EQ_INT(f->usage, 1);
      CHECK_EQ_INT(f->count, 14);
      CHECK_EQ_INT(f->bit, 36); // after 4 axes and the 4-bit hat
    }
    if (f->kind == HID_PAD_F_HAT) {
      CHECK_EQ_INT(f->bit, 32);
      CHECK_EQ_INT(f->size, 4);
    }
    if (f->kind == HID_PAD_F_X)
      CHECK_EQ_INT(f->lmax, 255);
  }
}

static void test_ds4_buttons(void) {
  hid_pad_parse(k_ds4_bt, sizeof(k_ds4_bt), &s_l);
  hid_pad_profile_t p = HID_PAD_PROFILE_SONY;
  uint8_t r[10];
  ds4(r, 8, 0, 0x80, 0x80); // centred, nothing held
  CHECK_EQ_U32(dec(p, r, sizeof(r)), 0);
  static const struct {
    uint16_t ds4;
    uint32_t pad;
  } k[] = {
      {DS4_CROSS, PAD_A},       {DS4_CIRCLE, PAD_B},
      {DS4_SQUARE, PAD_X},      {DS4_TRIANGLE, PAD_Y},
      {DS4_L1, PAD_L},          {DS4_R1, PAD_R},
      {DS4_SHARE, PAD_SELECT},  {DS4_OPTIONS, PAD_START},
      {DS4_PS, PAD_SOURCE_HOME}, {DS4_L2, 0},
      {DS4_R2, 0},              {DS4_L3, 0},
      {DS4_R3, 0},              {DS4_TOUCH, 0},
  };
  for (size_t i = 0; i < LEN(k); i++) {
    ds4(r, 8, k[i].ds4, 0x80, 0x80);
    CHECK_EQ_U32(dec(p, r, sizeof(r)), k[i].pad);
  }
  ds4(r, 8, DS4_CROSS | DS4_L1 | DS4_OPTIONS, 0x80, 0x80);
  CHECK_EQ_U32(dec(p, r, sizeof(r)), PAD_A | PAD_L | PAD_START);
}

static void test_ds4_hat_and_stick(void) {
  hid_pad_parse(k_ds4_bt, sizeof(k_ds4_bt), &s_l);
  hid_pad_profile_t p = HID_PAD_PROFILE_SONY;
  uint8_t r[10];
  static const uint32_t hat[9] = {
      PAD_UP,   PAD_UP | PAD_RIGHT,  PAD_RIGHT, PAD_DOWN | PAD_RIGHT,
      PAD_DOWN, PAD_DOWN | PAD_LEFT, PAD_LEFT,  PAD_UP | PAD_LEFT, 0};
  for (int h = 0; h <= 8; h++) {
    ds4(r, (uint8_t)h, 0, 0x80, 0x80);
    CHECK_EQ_U32(dec(p, r, sizeof(r)), hat[h]);
  }
  ds4(r, 15, 0, 0x80, 0x80); // any other null value: centred
  CHECK_EQ_U32(dec(p, r, sizeof(r)), 0);
  // The left stick, past half way from the centre (127.5 +- 63.75).
  ds4(r, 8, 0, 0, 0x80);
  CHECK_EQ_U32(dec(p, r, sizeof(r)), PAD_LEFT);
  ds4(r, 8, 0, 255, 0x80);
  CHECK_EQ_U32(dec(p, r, sizeof(r)), PAD_RIGHT);
  ds4(r, 8, 0, 0x80, 0);
  CHECK_EQ_U32(dec(p, r, sizeof(r)), PAD_UP);
  ds4(r, 8, 0, 0x80, 255);
  CHECK_EQ_U32(dec(p, r, sizeof(r)), PAD_DOWN);
  ds4(r, 8, 0, 63, 192);
  CHECK_EQ_U32(dec(p, r, sizeof(r)), PAD_LEFT | PAD_DOWN);
  ds4(r, 8, 0, 64, 191); // inside the dead zone
  CHECK_EQ_U32(dec(p, r, sizeof(r)), 0);
  ds4(r, 8, 0, 100, 160);
  CHECK_EQ_U32(dec(p, r, sizeof(r)), 0);
  // The right stick (Z/Rz) is not the D-pad.
  ds4(r, 8, 0, 0x80, 0x80);
  r[3] = 0;
  r[4] = 255;
  CHECK_EQ_U32(dec(p, r, sizeof(r)), 0);
  // Hat and stick together are ORed.
  ds4(r, 2, DS4_CROSS, 0x80, 0);
  CHECK_EQ_U32(dec(p, r, sizeof(r)), PAD_RIGHT | PAD_UP | PAD_A);
}

static void test_ds4_other_reports(void) {
  hid_pad_parse(k_ds4_bt, sizeof(k_ds4_bt), &s_l);
  uint8_t r[79];
  memset(r, 0, sizeof(r));
  r[0] = 0x11; // the full report, vendor-defined in this descriptor
  CHECK_EQ_U32(dec(HID_PAD_PROFILE_SONY, r, sizeof(r)), 0xDEADu);
  // Report 1 cut short is refused whole: no button reads released.
  uint8_t s[10];
  ds4(s, 8, DS4_CROSS, 0x80, 0x80);
  for (size_t n = 0; n < 8; n++)
    CHECK_EQ_U32(dec(HID_PAD_PROFILE_SONY, s, n), 0xDEADu);
  CHECK_EQ_U32(dec(HID_PAD_PROFILE_SONY, s, 8), PAD_A); // ends after buttons
  // An empty report, and no report at all.
  CHECK_EQ_U32(dec(HID_PAD_PROFILE_SONY, s, 0), 0xDEADu);
  uint32_t st = 7;
  CHECK(!hid_pad_decode(&s_l, HID_PAD_PROFILE_SONY, NULL, 10, &st));
  CHECK_EQ_U32(st, 7);
}

// ── DualSense over Bluetooth: report 0x01 has the DS4's layout ──────────────

static void test_dualsense(void) {
  CHECK(hid_pad_parse(k_dualsense_bt, sizeof(k_dualsense_bt), &s_l));
  CHECK_EQ_INT(count_kind(1, HID_PAD_F_BUTTONS), 1);
  CHECK_EQ_INT(count_kind(0x31, HID_PAD_F_BUTTONS), 0);
  uint8_t r[10];
  ds4(r, 4, DS4_CIRCLE | DS4_PS, 0x80, 0x80);
  CHECK_EQ_U32(dec(HID_PAD_PROFILE_SONY, r, sizeof(r)),
               PAD_DOWN | PAD_B | PAD_SOURCE_HOME);
  uint8_t full[78] = {0x31};
  CHECK_EQ_U32(dec(HID_PAD_PROFILE_SONY, full, sizeof(full)), 0xDEADu);
}

// ── An Android-layout pad: a list of buttons, Consumer AC Home ──────────────
// Report 1: 11 bits = buttons 1 2 4 5 7 8 14 15 13, AC Back, AC Home; one
// padding bit; the hat (4 bits); X Y Z Rz Brake Accelerator (8 bits each).

static void asus(uint8_t *r, uint16_t bits11, uint8_t hat, uint8_t x,
                 uint8_t y) {
  memset(r, 0, 9);
  r[0] = 0x01;
  r[1] = (uint8_t)bits11;
  r[2] = (uint8_t)(((bits11 >> 8) & 0x07) | ((hat & 0x0F) << 4));
  r[3] = x;
  r[4] = y;
  r[5] = r[6] = 0x80;
}

static void test_android_list_and_home(void) {
  CHECK(hid_pad_parse(k_asus_gamepad, sizeof(k_asus_gamepad), &s_l));
  CHECK_EQ_INT(count_kind(1, HID_PAD_F_HOME), 1); // AC Home, not AC Back
  hid_pad_profile_t p = HID_PAD_PROFILE_GENERIC;
  uint8_t r[9];
  asus(r, 0, 8, 0x80, 0x80);
  CHECK_EQ_U32(dec(p, r, sizeof(r)), 0);
  static const struct {
    uint16_t bit;
    uint32_t pad;
  } k[] = {
      {1u << 0, PAD_A}, {1u << 1, PAD_B},  {1u << 2, PAD_X},
      {1u << 3, PAD_Y}, {1u << 4, PAD_L},  {1u << 5, PAD_R},
      {1u << 6, 0},     {1u << 7, 0},      // 14, 15: stick clicks
      {1u << 8, PAD_SOURCE_HOME},           // button 13
      {1u << 9, 0},                         // AC Back
      {1u << 10, PAD_SOURCE_HOME},          // AC Home
  };
  for (size_t i = 0; i < LEN(k); i++) {
    asus(r, k[i].bit, 8, 0x80, 0x80);
    CHECK_EQ_U32(dec(p, r, sizeof(r)), k[i].pad);
  }
  asus(r, 0, 6, 0x80, 0xFF);
  CHECK_EQ_U32(dec(p, r, sizeof(r)), PAD_LEFT | PAD_DOWN);
  // Report 3 (battery, vendor) carries none of the pad's fields.
  uint8_t bat[8] = {0x03, 90};
  CHECK_EQ_U32(dec(p, bat, sizeof(bat)), 0xDEADu);
}

// ── Switch Pro (report 0x3F) and 8BitDo (D-input) ───────────────────────────

static void sw(uint8_t *r, uint16_t buttons, uint8_t hat, uint16_t lx,
               uint16_t ly) {
  memset(r, 0, 12);
  r[0] = 0x3F;
  r[1] = (uint8_t)buttons;
  r[2] = (uint8_t)(buttons >> 8);
  r[3] = hat;
  r[4] = (uint8_t)lx;
  r[5] = (uint8_t)(lx >> 8);
  r[6] = (uint8_t)ly;
  r[7] = (uint8_t)(ly >> 8);
  r[8] = r[10] = 0x00;
  r[9] = r[11] = 0x80;
}

static void test_switch_pro(void) {
  CHECK(hid_pad_parse(k_switch_pro_bt, sizeof(k_switch_pro_bt), &s_l));
  hid_pad_profile_t p = hid_pad_profile_for_name("Pro Controller");
  CHECK_EQ_INT(p, HID_PAD_PROFILE_NINTENDO);
  uint8_t r[12];
  sw(r, 0, 8, 0x8000, 0x8000);
  CHECK_EQ_U32(dec(p, r, sizeof(r)), 0);
  // 1 B (bottom), 2 A (right), 3 Y (left), 4 X (top): by position.
  static const uint32_t want[16] = {
      PAD_A, PAD_B, PAD_X, PAD_Y, PAD_L, PAD_R, 0, 0,
      PAD_SELECT, PAD_START, 0, 0, PAD_SOURCE_HOME, 0, 0, 0};
  for (int b = 0; b < 16; b++) {
    sw(r, (uint16_t)(1u << b), 8, 0x8000, 0x8000);
    CHECK_EQ_U32(dec(p, r, sizeof(r)), want[b]);
  }
  // 16-bit sticks: 0 .. 65535 around 0x8000.
  sw(r, 0, 8, 0x0000, 0x8000);
  CHECK_EQ_U32(dec(p, r, sizeof(r)), PAD_LEFT);
  sw(r, 0, 8, 0xFFFF, 0xFFFF);
  CHECK_EQ_U32(dec(p, r, sizeof(r)), PAD_RIGHT | PAD_DOWN);
  sw(r, 0, 8, 0x4100, 0xBEFF); // inside the dead zone
  CHECK_EQ_U32(dec(p, r, sizeof(r)), 0);
  sw(r, 0, 0, 0x8000, 0x8000);
  CHECK_EQ_U32(dec(p, r, sizeof(r)), PAD_UP);
}

static void test_8bitdo(void) {
  CHECK(hid_pad_parse(k_8bitdo_dinput, sizeof(k_8bitdo_dinput), &s_l));
  hid_pad_profile_t p = hid_pad_profile_for_name("8BitDo SN30 Pro");
  CHECK_EQ_INT(p, HID_PAD_PROFILE_8BITDO);
  uint8_t r[10] = {0x03, 0, 0, 0x08, 0x80, 0x80, 0x80, 0x80, 0, 0};
  CHECK_EQ_U32(dec(p, r, sizeof(r)), 0);
  // 1 A (right), 2 B (bottom), 4 X (top), 5 Y (left).
  static const uint32_t want[16] = {
      PAD_B, PAD_A, 0, PAD_Y, PAD_X, 0, PAD_L, PAD_R,
      0, 0, PAD_SELECT, PAD_START, PAD_SOURCE_HOME, 0, 0, 0};
  for (int b = 0; b < 16; b++) {
    r[1] = (uint8_t)(1u << b);
    r[2] = (uint8_t)((1u << b) >> 8);
    CHECK_EQ_U32(dec(p, r, sizeof(r)), want[b]);
  }
}

// ── Profiles ────────────────────────────────────────────────────────────────

static void test_profiles(void) {
  CHECK_EQ_INT(hid_pad_profile_for_name("Wireless Controller"),
               HID_PAD_PROFILE_SONY);
  CHECK_EQ_INT(hid_pad_profile_for_name("DualSense Wireless Controller"),
               HID_PAD_PROFILE_SONY);
  CHECK_EQ_INT(hid_pad_profile_for_name("Xbox Wireless Controller"),
               HID_PAD_PROFILE_GENERIC);
  CHECK_EQ_INT(hid_pad_profile_for_name("Pro Controller"),
               HID_PAD_PROFILE_NINTENDO);
  CHECK_EQ_INT(hid_pad_profile_for_name("8Bitdo N30 Pro 2"),
               HID_PAD_PROFILE_8BITDO);
  CHECK_EQ_INT(hid_pad_profile_for_name("Gamepad"), HID_PAD_PROFILE_GENERIC);
  CHECK_EQ_INT(hid_pad_profile_for_name(""), HID_PAD_PROFILE_GENERIC);
  CHECK_EQ_INT(hid_pad_profile_for_name(NULL), HID_PAD_PROFILE_GENERIC);
  CHECK_EQ_U32(hid_pad_button_bit(HID_PAD_PROFILE_SONY, 0), 0);
  CHECK_EQ_U32(hid_pad_button_bit(HID_PAD_PROFILE_SONY, 17), 0);
  CHECK_EQ_U32(hid_pad_button_bit(HID_PAD_PROFILE_COUNT, 1), 0);
  CHECK_STR(hid_pad_profile_label(HID_PAD_PROFILE_NINTENDO), "Nintendo");
  // Every profile maps each PAD_* button and Home exactly once.
  for (int p = 0; p < HID_PAD_PROFILE_COUNT; p++) {
    uint32_t seen = 0;
    int n = 0;
    for (unsigned b = 1; b <= 16; b++) {
      uint32_t bit = hid_pad_button_bit((hid_pad_profile_t)p, b);
      if (!bit)
        continue;
      CHECK((seen & bit) == 0);
      seen |= bit;
      n++;
    }
    CHECK_EQ_U32(seen, PAD_A | PAD_B | PAD_X | PAD_Y | PAD_L | PAD_R |
                           PAD_START | PAD_SELECT | PAD_SOURCE_HOME);
    CHECK_EQ_INT(n, 9);
  }
}

// ── Descriptor shapes the captures do not cover ─────────────────────────────

static void test_no_report_ids_signed_axes(void) {
  static const uint8_t d[] = {
      0x05, 0x01, 0x09, 0x05, 0xa1, 0x01,
      0x09, 0x30, 0x09, 0x31, 0x15, 0x81, 0x25, 0x7f, // -127 .. 127
      0x75, 0x08, 0x95, 0x02, 0x81, 0x02,
      0x05, 0x09, 0x19, 0x01, 0x29, 0x08, 0x15, 0x00, 0x25, 0x01,
      0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
      0x05, 0x01, 0x09, 0x85, 0x75, 0x01, 0x95, 0x01, 0x81, 0x02, // Home
      0x75, 0x07, 0x95, 0x01, 0x81, 0x03,
      0xc0};
  CHECK(hid_pad_parse(d, sizeof(d), &s_l));
  CHECK(!s_l.report_ids);
  hid_pad_profile_t p = HID_PAD_PROFILE_GENERIC;
  uint8_t r[4] = {0, 0, 0, 0};
  CHECK_EQ_U32(dec(p, r, sizeof(r)), 0);
  r[0] = 0x81; // -127
  r[1] = 0x7f;
  CHECK_EQ_U32(dec(p, r, sizeof(r)), PAD_LEFT | PAD_DOWN);
  r[0] = 0xC1; // -63: inside
  r[1] = 0x40; // 64: outside
  CHECK_EQ_U32(dec(p, r, sizeof(r)), PAD_DOWN);
  r[0] = r[1] = 0;
  r[2] = 0x01; // button 1
  r[3] = 0x01; // System Main Menu
  CHECK_EQ_U32(dec(p, r, sizeof(r)), PAD_A | PAD_SOURCE_HOME);
  CHECK_EQ_U32(dec(p, r, 3), 0xDEADu);
}

static void test_one_byte_max_hat_and_array(void) {
  static const uint8_t d[] = {
      0x05, 0x01, 0x09, 0x05, 0xa1, 0x01, 0x85, 0x05,
      // X/Y 0..255 written as the one byte 0xFF (signed -1): read unsigned
      0x09, 0x30, 0x09, 0x31, 0x15, 0x00, 0x25, 0xff,
      0x75, 0x08, 0x95, 0x02, 0x81, 0x02,
      // a 1-based 4-position hat: 1 N, 2 E, 3 S, 4 W, 0 centred
      0x09, 0x39, 0x15, 0x01, 0x25, 0x04, 0x75, 0x04, 0x95, 0x01, 0x81, 0x42,
      0x75, 0x04, 0x95, 0x01, 0x81, 0x01,
      // two slots of button numbers 1..16, 0 = none
      0x05, 0x09, 0x19, 0x01, 0x29, 0x10, 0x15, 0x01, 0x25, 0x10,
      0x75, 0x08, 0x95, 0x02, 0x81, 0x00,
      0xc0};
  CHECK(hid_pad_parse(d, sizeof(d), &s_l));
  hid_pad_profile_t p = HID_PAD_PROFILE_GENERIC;
  uint8_t r[6] = {0x05, 0x80, 0x80, 0x00, 0, 0};
  CHECK_EQ_U32(dec(p, r, sizeof(r)), 0);
  r[1] = 0xF0;
  CHECK_EQ_U32(dec(p, r, sizeof(r)), PAD_RIGHT);
  r[1] = 0x80;
  static const uint32_t hat[6] = {0, PAD_UP, PAD_RIGHT, PAD_DOWN, PAD_LEFT, 0};
  for (int h = 0; h < 6; h++) {
    r[3] = (uint8_t)h;
    CHECK_EQ_U32(dec(p, r, sizeof(r)), hat[h]);
  }
  r[3] = 0;
  r[4] = 2;  // B
  r[5] = 12; // Start
  CHECK_EQ_U32(dec(p, r, sizeof(r)), PAD_B | PAD_START);
  r[4] = 0;
  r[5] = 17; // out of range: none
  CHECK_EQ_U32(dec(p, r, sizeof(r)), 0);
  r[0] = 0x06; // another report ID
  CHECK_EQ_U32(dec(p, r, sizeof(r)), 0xDEADu);
}

static void test_not_a_pad_and_garbage(void) {
  // A keyboard: no buttons, hat or stick.
  static const uint8_t kbd[] = {
      0x05, 0x01, 0x09, 0x06, 0xa1, 0x01, 0x05, 0x07, 0x19, 0xe0, 0x29, 0xe7,
      0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0xc0};
  CHECK(!hid_pad_parse(kbd, sizeof(kbd), &s_l));
  CHECK(!hid_pad_parse(NULL, 10, &s_l));
  CHECK(!hid_pad_parse(kbd, 0, &s_l));
  // Every prefix of a real descriptor parses without reading past it.
  for (size_t n = 0; n <= sizeof(k_ds4_bt); n++) {
    uint8_t *copy = (uint8_t *)malloc(n ? n : 1);
    memcpy(copy, k_ds4_bt, n);
    hid_pad_parse(copy, n, &s_l);
    free(copy);
  }
  // Huge counts and sizes are skipped, and later fields stay addressable.
  static const uint8_t big[] = {
      0x05, 0x01, 0x09, 0x05, 0xa1, 0x01,
      0x75, 0x08, 0x97, 0xff, 0xff, 0xff, 0x7f, 0x81, 0x03, // 2^31 bytes
      0x05, 0x09, 0x19, 0x01, 0x29, 0x02, 0x15, 0x00, 0x25, 0x01,
      0x75, 0x01, 0x95, 0x02, 0x81, 0x02, 0xc0};
  hid_pad_parse(big, sizeof(big), &s_l);
  uint8_t r[8] = {0xff};
  CHECK_EQ_U32(dec(HID_PAD_PROFILE_GENERIC, r, sizeof(r)), 0xDEADu);
  // Long items are stepped over.
  static const uint8_t lng[] = {
      0xfe, 0x02, 0x10, 0xaa, 0xbb,
      0x05, 0x09, 0x19, 0x01, 0x29, 0x01, 0x15, 0x00, 0x25, 0x01,
      0x75, 0x01, 0x95, 0x01, 0x81, 0x02};
  CHECK(hid_pad_parse(lng, sizeof(lng), &s_l));
  uint8_t one[1] = {1};
  CHECK_EQ_U32(dec(HID_PAD_PROFILE_GENERIC, one, 1), PAD_A);
}

int main(void) {
  test_ds4_layout();
  test_ds4_buttons();
  test_ds4_hat_and_stick();
  test_ds4_other_reports();
  test_dualsense();
  test_android_list_and_home();
  test_switch_pro();
  test_8bitdo();
  test_profiles();
  test_no_report_ids_signed_axes();
  test_one_byte_max_hat_and_array();
  test_not_a_pad_and_garbage();
  return check_report("test_hid_pad");
}
