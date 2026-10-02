// The Bluetooth pads' bonding store (see bt_pad_store.h). No hardware
// dependency: host tests in tests/unit/test_bt_pad_store.c.
#include "bt_pad_store.h"

#include <string.h>

#define FILE_MAGIC "PDBT"
#define FILE_VERSION 1

// A paired pad: 'P' 'D' 'P' + slot; value = sequence (LE32), address, name.
#define PAD_TAG(i) (((uint32_t)'P' << 24) | ((uint32_t)'D' << 16) | \
                     ((uint32_t)'P' << 8) | (uint32_t)(i))
#define PAD_VALUE_LEN (4 + 6 + BT_PAD_NAME_MAX)
_Static_assert(PAD_VALUE_LEN <= BT_STORE_VALUE_MAX, "pad record fits a value");

void bt_store_init(bt_store_t *s) { memset(s, 0, sizeof(*s)); }

static int find(const bt_store_t *s, uint32_t tag) {
  for (int i = 0; i < s->n; i++)
    if (s->e[i].tag == tag)
      return i;
  return -1;
}

int bt_store_get(const bt_store_t *s, uint32_t tag, uint8_t *buf,
                 uint32_t size) {
  int i = find(s, tag);
  if (i < 0)
    return 0;
  uint32_t n = s->e[i].len < size ? s->e[i].len : size;
  if (buf && n)
    memcpy(buf, s->e[i].data, n);
  return s->e[i].len;
}

bool bt_store_put(bt_store_t *s, uint32_t tag, const uint8_t *data,
                  uint32_t len) {
  if (len > BT_STORE_VALUE_MAX || (len && !data))
    return false;
  int i = find(s, tag);
  if (i < 0) {
    if (s->n >= BT_STORE_ENTRIES)
      return false;
    i = s->n++;
    s->e[i].tag = tag;
    s->e[i].len = 0xFF; // never equal below: a new entry is always a change
  }
  bt_store_entry_t *e = &s->e[i];
  if (e->len == len && (len == 0 || memcmp(e->data, data, len) == 0))
    return true;
  memset(e->data, 0, sizeof(e->data));
  if (len)
    memcpy(e->data, data, len);
  e->len = (uint8_t)len;
  s->dirty = true;
  return true;
}

void bt_store_del(bt_store_t *s, uint32_t tag) {
  int i = find(s, tag);
  if (i < 0)
    return;
  s->e[i] = s->e[s->n - 1];
  s->n--;
  s->dirty = true;
}

static void put_le32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 24);
}

static uint32_t get_le32(const uint8_t *p) {
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
         (uint32_t)p[3] << 24;
}

size_t bt_store_serialize(const bt_store_t *s, uint8_t *out, size_t cap) {
  size_t need = 6;
  for (int i = 0; i < s->n; i++)
    need += 5u + s->e[i].len;
  if (!out || cap < need)
    return 0;
  memcpy(out, FILE_MAGIC, 4);
  out[4] = FILE_VERSION;
  out[5] = s->n;
  size_t at = 6;
  for (int i = 0; i < s->n; i++) {
    put_le32(out + at, s->e[i].tag);
    out[at + 4] = s->e[i].len;
    memcpy(out + at + 5, s->e[i].data, s->e[i].len);
    at += 5u + s->e[i].len;
  }
  return at;
}

bool bt_store_parse(bt_store_t *s, const uint8_t *in, size_t len) {
  bt_store_init(s);
  if (!in || len < 6 || memcmp(in, FILE_MAGIC, 4) != 0 ||
      in[4] != FILE_VERSION || in[5] > BT_STORE_ENTRIES)
    return false;
  size_t at = 6;
  for (int i = 0; i < in[5]; i++) {
    if (at + 5 > len || in[at + 4] > BT_STORE_VALUE_MAX ||
        at + 5 + in[at + 4] > len) {
      bt_store_init(s);
      return false;
    }
    uint32_t tag = get_le32(in + at);
    if (find(s, tag) >= 0) { // a tag twice: not a file this wrote
      bt_store_init(s);
      return false;
    }
    bt_store_entry_t *e = &s->e[s->n++];
    e->tag = tag;
    e->len = in[at + 4];
    memcpy(e->data, in + at + 5, e->len);
    at += 5u + e->len;
  }
  if (at != len) {
    bt_store_init(s);
    return false;
  }
  return true;
}

// ── Paired pads ─────────────────────────────────────────────────────────────
// bt_pad.c calls these from BTstack callbacks, on the 4 KB main stack: they
// sort small references and read the records in place, with no copies.

typedef struct {
  uint32_t seq;
  const uint8_t *v; // the record's value in the store (PAD_VALUE_LEN bytes)
  int slot;
} pad_ref_t;

// The slots' records, newest first; returns how many are valid.
static int pad_refs(const bt_store_t *s, pad_ref_t *p) {
  int n = 0;
  for (int i = 0; i < BT_PAD_PAIRED_MAX; i++) {
    int e = find(s, PAD_TAG(i));
    if (e < 0 || s->e[e].len != PAD_VALUE_LEN)
      continue;
    p[n].seq = get_le32(s->e[e].data);
    p[n].v = s->e[e].data;
    p[n].slot = i;
    n++;
  }
  for (int i = 1; i < n; i++) // insertion sort: at most four
    for (int j = i; j > 0 && p[j].seq > p[j - 1].seq; j--) {
      pad_ref_t t = p[j];
      p[j] = p[j - 1];
      p[j - 1] = t;
    }
  return n;
}

static void pad_record(const uint8_t *v, bt_pad_record_t *out) {
  memcpy(out->addr, v + 4, 6);
  memcpy(out->name, v + 10, BT_PAD_NAME_MAX);
  out->name[BT_PAD_NAME_MAX - 1] = '\0';
}

int bt_store_pad_count(const bt_store_t *s) {
  pad_ref_t p[BT_PAD_PAIRED_MAX];
  return pad_refs(s, p);
}

int bt_store_pads(const bt_store_t *s, bt_pad_record_t *out, int max) {
  pad_ref_t p[BT_PAD_PAIRED_MAX];
  int n = pad_refs(s, p);
  if (n > max)
    n = max;
  for (int i = 0; i < n; i++)
    pad_record(p[i].v, &out[i]);
  return n;
}

bool bt_store_find_pad(const bt_store_t *s, const uint8_t addr[6],
                       bt_pad_record_t *out) {
  pad_ref_t p[BT_PAD_PAIRED_MAX];
  int n = pad_refs(s, p);
  for (int i = 0; i < n; i++)
    if (memcmp(p[i].v + 4, addr, 6) == 0) {
      if (out)
        pad_record(p[i].v, out);
      return true;
    }
  return false;
}

bool bt_store_add_pad(bt_store_t *s, const bt_pad_record_t *r,
                      uint8_t evicted[6]) {
  pad_ref_t p[BT_PAD_PAIRED_MAX];
  int n = pad_refs(s, p);
  uint32_t seq = n ? p[0].seq + 1 : 1;
  int slot = -1;
  bool dropped = false;
  for (int i = 0; i < n; i++)
    if (memcmp(p[i].v + 4, r->addr, 6) == 0)
      slot = p[i].slot;
  if (slot < 0) {
    bool used[BT_PAD_PAIRED_MAX] = {false};
    for (int i = 0; i < n; i++)
      used[p[i].slot] = true;
    for (int i = 0; i < BT_PAD_PAIRED_MAX && slot < 0; i++)
      if (!used[i])
        slot = i;
    if (slot < 0) { // full: the oldest goes
      slot = p[n - 1].slot;
      if (evicted)
        memcpy(evicted, p[n - 1].v + 4, 6);
      dropped = true;
    }
  }
  uint8_t v[PAD_VALUE_LEN];
  memset(v, 0, sizeof(v));
  put_le32(v, seq);
  memcpy(v + 4, r->addr, 6);
  strncpy((char *)v + 10, r->name, BT_PAD_NAME_MAX - 1);
  bt_store_put(s, PAD_TAG(slot), v, sizeof(v));
  return dropped;
}

bool bt_store_remove_pad(bt_store_t *s, const uint8_t addr[6]) {
  pad_ref_t p[BT_PAD_PAIRED_MAX];
  int n = pad_refs(s, p);
  for (int i = 0; i < n; i++)
    if (memcmp(p[i].v + 4, addr, 6) == 0) {
      bt_store_del(s, PAD_TAG(p[i].slot));
      return true;
    }
  return false;
}

// ── Addresses ───────────────────────────────────────────────────────────────

void bt_pad_addr_str(const uint8_t addr[6], char out[18]) {
  static const char hex[] = "0123456789ABCDEF";
  for (int i = 0; i < 6; i++) {
    out[i * 3] = hex[addr[i] >> 4];
    out[i * 3 + 1] = hex[addr[i] & 15];
    out[i * 3 + 2] = i < 5 ? ':' : '\0';
  }
}

static int hexval(char c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return -1;
}

bool bt_pad_addr_parse(const char *s, uint8_t addr[6]) {
  if (!s)
    return false;
  for (int i = 0; i < 6; i++) {
    int hi = hexval(s[i * 3]), lo = hi < 0 ? -1 : hexval(s[i * 3 + 1]);
    if (hi < 0 || lo < 0)
      return false;
    char sep = s[i * 3 + 2];
    if (i < 5 ? sep != ':' : sep != '\0')
      return false;
    addr[i] = (uint8_t)(hi << 4 | lo);
  }
  return true;
}

// ── Pairing policy ──────────────────────────────────────────────────────────

bool bt_pad_pairing_allowed(bool pairing, const uint8_t peer[6],
                            const uint8_t addr[6]) {
  return pairing && peer && addr && memcmp(peer, addr, 6) == 0;
}

bool bt_pad_connection_allowed(const bt_store_t *s, bool pairing,
                               const uint8_t peer[6], const uint8_t addr[6]) {
  if (!addr)
    return false;
  return bt_store_find_pad(s, addr, NULL) ||
         bt_pad_pairing_allowed(pairing, peer, addr);
}
