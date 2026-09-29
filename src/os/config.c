#include "config.h"
#include "../drivers/sdcard.h"
#include "umm_malloc.h"
#include "flat_json.h"
#include "sd_atomic.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

#define CONFIG_PATH "/system/config.json"

// ── In-memory store ───────────────────────────────────────────────────────────
// Keys and values share one pool of "key\0value\0" pairs, back to back, so
// the store's capacity is in bytes: a "1" costs two, a long URL its length.
// Fixed slots of CONFIG_KEY_MAX + CONFIG_VAL_MAX bytes held only ten keys in
// more SRAM than this pool.

static char s_pool[CONFIG_POOL_SIZE];
static int  s_used = 0;  // bytes of s_pool in use; each key appears once

// The pair after the one at p.
static char *pair_next(char *p) {
    p += strlen(p) + 1;
    return p + strlen(p) + 1;
}

static char *pair_find(const char *key) {
    for (char *p = s_pool; p < s_pool + s_used; p = pair_next(p))
        if (strcmp(p, key) == 0)
            return p;
    return NULL;
}

static void pair_remove(char *p) {
    char *next = pair_next(p);
    memmove(p, next, (size_t)(s_pool + s_used - next));
    s_used -= (int)(next - p);
}

// Append key=value (lengths without the NULs). False, and nothing stored, if
// the pool has no room for the pair.
static bool pair_append(const char *key, size_t klen, const char *val,
                        size_t vlen) {
    size_t need = klen + 1 + vlen + 1;
    if (need > (size_t)(CONFIG_POOL_SIZE - s_used))
        return false;
    char *p = s_pool + s_used;
    memcpy(p, key, klen);
    p[klen] = '\0';
    memcpy(p + klen + 1, val, vlen);
    p[klen + 1 + vlen] = '\0';
    s_used += (int)need;
    return true;
}

// ── Public API ─────────────────────────────────────────────────────────────────

bool config_load(void) {
    s_used = 0;

    sd_atomic_recover(CONFIG_PATH);
    int len = 0;
    char *json = sdcard_read_file(CONFIG_PATH, &len);
    if (!json) {
        // File doesn't exist yet — empty config is fine
        return false;
    }

    // Walk the JSON string looking for "key":"value" pairs.
    // We keep a simple cursor that scans for opening quotes.
    const char *p = json;
    int count = 0;
    for (;;) {
        // Find next '"'
        p = strchr(p, '"');
        if (!p) break;
        p++;  // skip opening "

        // Read key (escaped by config_save; over-long keys are truncated
        // and the rest skipped)
        char key[CONFIG_KEY_MAX];
        p = flat_json_read_string(p, key, sizeof(key));
        if (!*p) break;

        // Skip whitespace and ':'
        while (*p == ' ' || *p == '\t' || *p == ':') p++;

        // Expect value '"'
        if (*p != '"') {
            // Not a string value — skip to next ','
            p = strchr(p, ',');
            if (!p) break;
            continue;
        }
        p++;  // skip opening "

        // Read value
        char val[CONFIG_VAL_MAX];
        p = flat_json_read_string(p, val, sizeof(val));

        // Skip internal metadata key; a pair the pool has no room for is
        // dropped (logged), later smaller ones may still fit. A repeated key
        // replaces the earlier copy (the last one wins, as in JSON), so each
        // key is in the pool once and config_get/config_set see one value.
        if (key[0] != '\0') {
            char *dup = pair_find(key);
            if (dup) {
                pair_remove(dup);
                count--;
            }
            if (pair_append(key, strlen(key), val, strlen(val)))
                count++;
            else
                printf("Config: no room for '%s', dropped\n", key);
        }
    }

    umm_free(json);
    printf("Config: loaded %d entries (%d/%d bytes) from %s\n", count, s_used,
           CONFIG_POOL_SIZE, CONFIG_PATH);
    return true;
}

bool config_save(void) {
    // Capacity formula accounts for worst-case JSON escaping:
    //   - Each key or value char can expand to 2 bytes (e.g. '\' → "\\"),
    //     and the pool holds every char plus one NUL each → 2*s_used
    //   - Per-entry overhead: "":""[,]                              → 8 bytes
    //   - Outer braces + null terminator                            → 4 bytes
    // So the estimate is always sufficient for any key/value content.
    int  count = 0;
    for (char *e = s_pool; e < s_pool + s_used; e = pair_next(e))
        count++;
    int  cap = 2 * s_used + count * 8 + 4;
    char *buf = (char *)umm_malloc(cap);
    if (!buf) return false;
    int  pos = 0;

    buf[pos++] = '{';
    for (char *e = s_pool; e < s_pool + s_used; e = pair_next(e)) {
        if (e > s_pool) buf[pos++] = ',';

        buf[pos++] = '"';
        for (const char *k = e; *k; k++) {
            if (*k == '"' || *k == '\\') buf[pos++] = '\\';
            buf[pos++] = *k;
        }
        buf[pos++] = '"';
        buf[pos++] = ':';
        buf[pos++] = '"';
        for (const char *v = e + strlen(e) + 1; *v; v++) {
            if (*v == '"' || *v == '\\') buf[pos++] = '\\';
            else if (*v == '\n') { buf[pos++] = '\\'; buf[pos++] = 'n'; continue; }
            else if (*v == '\t') { buf[pos++] = '\\'; buf[pos++] = 't'; continue; }
            buf[pos++] = *v;
        }
        buf[pos++] = '"';
    }
    buf[pos++] = '}';
    buf[pos]   = '\0';

    // Through config.json.tmp and a rename: a power cut or a failed write
    // never leaves a truncated config (and lost WiFi credentials).
    bool ok = sd_atomic_write(CONFIG_PATH, buf, pos);
    umm_free(buf);
    if (!ok) {
        printf("Config: save to %s failed; previous file kept\n", CONFIG_PATH);
        return false;
    }
    printf("Config: saved %d entries to %s\n", count, CONFIG_PATH);
    return true;
}

const char *config_get(const char *key) {
    char *p = pair_find(key);
    return p ? p + strlen(p) + 1 : NULL;
}

void config_set(const char *key, const char *value) {
    if (!key || !key[0]) return;

    // Copies within the limits (truncated as before); either argument may
    // point into the pool, which the edits below move.
    char k[CONFIG_KEY_MAX], v[CONFIG_VAL_MAX];
    size_t klen = strnlen(key, CONFIG_KEY_MAX - 1);
    memcpy(k, key, klen);
    k[klen] = '\0';
    size_t vlen = value ? strnlen(value, CONFIG_VAL_MAX - 1) : 0;
    memcpy(v, value ? value : "", vlen);
    v[vlen] = '\0';

    char *p = pair_find(k);

    // Remove key if value is NULL or empty
    if (vlen == 0) {
        if (p) pair_remove(p);
        return;
    }

    if (p) {
        char *old = p + klen + 1;
        size_t old_len = strlen(old);
        if (old_len == vlen) {  // same length: in place
            memcpy(old, v, vlen);
            return;
        }
        // No room for the longer value: keep the old one.
        if (vlen > old_len && vlen - old_len > (size_t)(CONFIG_POOL_SIZE - s_used))
            return;
        pair_remove(p);
    }
    pair_append(k, klen, v, vlen);  // a new key on a full pool is dropped
}
