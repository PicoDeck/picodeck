// Host unit tests for src/drivers/bt_pad_store.c: the Bluetooth pads' bonding
// store (BTstack's TLV calls, the paired-pad list, the file image).
#include "check.h"
#include "bt_pad_store.h"

#include <stdlib.h>

static bt_store_t s_s;

#define BTL(i) (((uint32_t)'B' << 24) | ((uint32_t)'T' << 16) | \
                ((uint32_t)'L' << 8) | (uint32_t)(i))

static void test_tlv(void) {
  bt_store_init(&s_s);
  uint8_t key[27];
  for (int i = 0; i < 27; i++)
    key[i] = (uint8_t)(i * 7);
  uint8_t buf[64];
  CHECK_EQ_INT(bt_store_get(&s_s, BTL(0), buf, sizeof(buf)), 0);
  CHECK(!s_s.dirty);
  CHECK(bt_store_put(&s_s, BTL(0), key, sizeof(key)));
  CHECK(s_s.dirty);
  memset(buf, 0, sizeof(buf));
  CHECK_EQ_INT(bt_store_get(&s_s, BTL(0), buf, sizeof(buf)), 27);
  CHECK(memcmp(buf, key, 27) == 0);
  // A short buffer gets a prefix; the length is still the value's.
  memset(buf, 0xEE, sizeof(buf));
  CHECK_EQ_INT(bt_store_get(&s_s, BTL(0), buf, 4), 27);
  CHECK(memcmp(buf, key, 4) == 0 && buf[4] == 0xEE);
  // The same value again is not a change.
  s_s.dirty = false;
  CHECK(bt_store_put(&s_s, BTL(0), key, sizeof(key)));
  CHECK(!s_s.dirty);
  key[3] ^= 1;
  CHECK(bt_store_put(&s_s, BTL(0), key, sizeof(key)));
  CHECK(s_s.dirty);
  CHECK_EQ_INT(s_s.n, 1);
  // Too long, and a full table, are refused.
  uint8_t big[BT_STORE_VALUE_MAX + 1] = {0};
  CHECK(!bt_store_put(&s_s, BTL(1), big, sizeof(big)));
  for (int i = 1; i < BT_STORE_ENTRIES; i++)
    CHECK(bt_store_put(&s_s, BTL(i), key, 1));
  CHECK(!bt_store_put(&s_s, BTL(99), key, 1));
  CHECK(bt_store_put(&s_s, BTL(3), key, 2)); // an existing tag still updates
  // Delete.
  s_s.dirty = false;
  bt_store_del(&s_s, BTL(0));
  CHECK(s_s.dirty);
  CHECK_EQ_INT(bt_store_get(&s_s, BTL(0), buf, sizeof(buf)), 0);
  CHECK_EQ_INT(bt_store_get(&s_s, BTL(3), buf, sizeof(buf)), 2);
  s_s.dirty = false;
  bt_store_del(&s_s, BTL(0)); // gone already: no change
  CHECK(!s_s.dirty);
}

static bt_pad_record_t rec(uint8_t last, const char *name) {
  bt_pad_record_t r;
  memset(&r, 0, sizeof(r));
  uint8_t a[6] = {0x11, 0x22, 0x33, 0x44, 0x55, last};
  memcpy(r.addr, a, 6);
  strncpy(r.name, name, sizeof(r.name) - 1);
  return r;
}

static void test_pads(void) {
  bt_store_init(&s_s);
  bt_pad_record_t out[BT_PAD_PAIRED_MAX];
  CHECK_EQ_INT(bt_store_pads(&s_s, out, BT_PAD_PAIRED_MAX), 0);
  uint8_t ev[6];
  bt_pad_record_t a = rec(1, "Wireless Controller");
  bt_pad_record_t b = rec(2, "Pro Controller");
  CHECK(!bt_store_add_pad(&s_s, &a, ev));
  CHECK(!bt_store_add_pad(&s_s, &b, ev));
  CHECK_EQ_INT(bt_store_pads(&s_s, out, BT_PAD_PAIRED_MAX), 2);
  CHECK_EQ_INT(bt_store_pad_count(&s_s), 2);
  CHECK_STR(out[0].name, "Pro Controller"); // newest first
  CHECK_STR(out[1].name, "Wireless Controller");
  // Re-adding refreshes the name and makes it the newest.
  bt_pad_record_t a2 = rec(1, "DS4");
  CHECK(!bt_store_add_pad(&s_s, &a2, ev));
  CHECK_EQ_INT(bt_store_pads(&s_s, out, BT_PAD_PAIRED_MAX), 2);
  CHECK_STR(out[0].name, "DS4");
  bt_pad_record_t f;
  CHECK(bt_store_find_pad(&s_s, b.addr, &f));
  CHECK_STR(f.name, "Pro Controller");
  // Full: the oldest goes, and its address comes back for its link key.
  bt_pad_record_t c = rec(3, "C"), d = rec(4, "D"), e = rec(5, "E");
  CHECK(!bt_store_add_pad(&s_s, &c, ev));
  CHECK(!bt_store_add_pad(&s_s, &d, ev));
  memset(ev, 0, sizeof(ev));
  CHECK(bt_store_add_pad(&s_s, &e, ev));
  CHECK(memcmp(ev, b.addr, 6) == 0);
  CHECK(!bt_store_find_pad(&s_s, b.addr, NULL));
  CHECK_EQ_INT(bt_store_pads(&s_s, out, BT_PAD_PAIRED_MAX), 4);
  CHECK_STR(out[0].name, "E");
  CHECK_STR(out[3].name, "DS4");
  // A smaller `max`.
  CHECK_EQ_INT(bt_store_pads(&s_s, out, 2), 2);
  CHECK_STR(out[1].name, "D");
  // Remove.
  CHECK(bt_store_remove_pad(&s_s, d.addr));
  CHECK(!bt_store_remove_pad(&s_s, d.addr));
  CHECK_EQ_INT(bt_store_pads(&s_s, out, BT_PAD_PAIRED_MAX), 3);
  CHECK_EQ_INT(bt_store_pad_count(&s_s), 3);
  // A name at the limit is cut, never unterminated.
  bt_pad_record_t lng = rec(6, "");
  memset(lng.name, 'x', sizeof(lng.name));
  CHECK(!bt_store_add_pad(&s_s, &lng, ev));
  CHECK(bt_store_find_pad(&s_s, lng.addr, &f));
  CHECK_EQ_INT(strlen(f.name), BT_PAD_NAME_MAX - 1);
}

static void test_file(void) {
  bt_store_init(&s_s);
  uint8_t key[27] = {1, 2, 3};
  bt_store_put(&s_s, BTL(0), key, sizeof(key));
  bt_pad_record_t a = rec(1, "Wireless Controller");
  uint8_t ev[6];
  bt_store_add_pad(&s_s, &a, ev);
  uint8_t img[BT_STORE_FILE_MAX];
  size_t n = bt_store_serialize(&s_s, img, sizeof(img));
  CHECK(n > 6);
  CHECK(memcmp(img, "PDBT", 4) == 0);
  CHECK_EQ_INT(bt_store_serialize(&s_s, img, n - 1), 0); // too small
  bt_store_t t;
  CHECK(bt_store_parse(&t, img, n));
  CHECK(!t.dirty);
  CHECK_EQ_INT(t.n, s_s.n);
  uint8_t buf[32];
  CHECK_EQ_INT(bt_store_get(&t, BTL(0), buf, sizeof(buf)), 27);
  CHECK(buf[2] == 3);
  bt_pad_record_t f;
  CHECK(bt_store_find_pad(&t, a.addr, &f));
  CHECK_STR(f.name, "Wireless Controller");
  // A full store fits BT_STORE_FILE_MAX.
  bt_store_init(&t);
  uint8_t v[BT_STORE_VALUE_MAX];
  memset(v, 0xAB, sizeof(v));
  for (int i = 0; i < BT_STORE_ENTRIES; i++)
    CHECK(bt_store_put(&t, BTL(i), v, sizeof(v)));
  CHECK_EQ_INT(bt_store_serialize(&t, img, sizeof(img)), BT_STORE_FILE_MAX);
  // Damage of every kind reads as an empty store.
  n = bt_store_serialize(&s_s, img, sizeof(img));
  for (size_t cut = 0; cut < n; cut++)
    CHECK(!bt_store_parse(&t, img, cut) && t.n == 0);
  uint8_t bad[BT_STORE_FILE_MAX];
  memcpy(bad, img, n);
  bad[0] = 'X';
  CHECK(!bt_store_parse(&t, bad, n));
  memcpy(bad, img, n);
  bad[4] = 9; // version
  CHECK(!bt_store_parse(&t, bad, n));
  memcpy(bad, img, n);
  bad[5] = BT_STORE_ENTRIES + 1;
  CHECK(!bt_store_parse(&t, bad, n));
  memcpy(bad, img, n);
  bad[6 + 4] = BT_STORE_VALUE_MAX + 1; // first entry's length
  CHECK(!bt_store_parse(&t, bad, n));
  CHECK(!bt_store_parse(&t, img, n + 1)); // trailing byte (img has room)
  // The same tag twice.
  bt_store_t two;
  bt_store_init(&two);
  bt_store_put(&two, BTL(0), key, 1);
  bt_store_put(&two, BTL(1), key, 1);
  n = bt_store_serialize(&two, img, sizeof(img));
  img[6 + 6] = img[6]; // second tag := first
  CHECK(!bt_store_parse(&t, img, n));
  CHECK(!bt_store_parse(&t, NULL, 10));
  // Random bytes never crash and never yield a half-read store.
  srand(7);
  for (int k = 0; k < 2000; k++) {
    size_t len = (size_t)(rand() % 64);
    uint8_t r[64];
    for (size_t i = 0; i < len; i++)
      r[i] = (uint8_t)rand();
    if (len >= 6 && (k & 1)) {
      memcpy(r, "PDBT", 4);
      r[4] = 1;
      r[5] = (uint8_t)(rand() % 4);
    }
    bool ok = bt_store_parse(&t, r, len);
    CHECK(ok || t.n == 0);
  }
}

static void test_addr(void) {
  uint8_t a[6] = {0x00, 0x1A, 0x7D, 0xDA, 0x71, 0xFF}, b[6];
  char s[18];
  bt_pad_addr_str(a, s);
  CHECK_STR(s, "00:1A:7D:DA:71:FF");
  CHECK(bt_pad_addr_parse(s, b) && memcmp(a, b, 6) == 0);
  CHECK(bt_pad_addr_parse("00:1a:7d:da:71:ff", b) && memcmp(a, b, 6) == 0);
  CHECK(!bt_pad_addr_parse("00:1A:7D:DA:71", b));
  CHECK(!bt_pad_addr_parse("00:1A:7D:DA:71:FF:", b));
  CHECK(!bt_pad_addr_parse("00-1A-7D-DA-71-FF", b));
  CHECK(!bt_pad_addr_parse("0G:1A:7D:DA:71:FF", b));
  CHECK(!bt_pad_addr_parse("", b));
  CHECK(!bt_pad_addr_parse(NULL, b));
}

int main(void) {
  test_tlv();
  test_pads();
  test_file();
  test_addr();
  return check_report("test_bt_pad_store");
}
