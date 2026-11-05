#include "appconfig.h"
#include "../drivers/sdcard.h"
#include "umm_malloc.h"
#include "flat_json.h"
#include "sd_atomic.h"

#include <stdio.h>
#include <string.h>
#include <stdbool.h>

// app_entry_t.id is 80 bytes; the path is built on demand (no static copy)
// and sized for the longest id: "/data/" + 79 + "/config.json".
#define APPCONFIG_ID_MAX   80
#define APPCONFIG_PATH_LEN (6 + APPCONFIG_ID_MAX + 12)

static char s_app_id[APPCONFIG_ID_MAX] = {0};

static void config_path(char *out, size_t n) {
    snprintf(out, n, "/data/%s/config.json", s_app_id);
}

typedef struct {
    char key[APPCONFIG_KEY_MAX];
    char val[APPCONFIG_VAL_MAX];
} appconfig_entry_t;

static appconfig_entry_t *s_entries;  // umm, APPCONFIG_MAX_ENTRIES of them
static int s_count = 0;

// The file's members the store cannot hold (appconfig.h), verbatim:
// `"k":v,"k2":v2`, NUL-terminated (umm). save writes them back.
static char *s_kept;
static size_t s_kept_len;

// The file exists and could not be read (or held, for want of memory): a save
// would delete its keys, so it is refused until a load works or a reset.
static bool s_unread;

static bool entries_ready(void) {
    if (!s_entries)
        s_entries = (appconfig_entry_t *)umm_malloc(APPCONFIG_MAX_ENTRIES *
                                                    sizeof(appconfig_entry_t));
    return s_entries != NULL;
}

static appconfig_entry_t *entry_find(const char *key) {
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_entries[i].key, key) == 0)
            return &s_entries[i];
    }
    return NULL;
}

// A new key or a new value for one (both already known to fit). False, and
// nothing stored, when the store is full or there is no memory for it.
static bool entry_store(const char *key, const char *val) {
    appconfig_entry_t *e = s_entries ? entry_find(key) : NULL;
    if (!e) {
        if (s_count >= APPCONFIG_MAX_ENTRIES || !entries_ready())
            return false;
        e = &s_entries[s_count++];
        strcpy(e->key, key);
    }
    memmove(e->val, val, strlen(val) + 1);  // val may be this entry's own
    return true;
}

// ── The file: one flat JSON object ───────────────────────────────────────────

static const char *skip_ws(const char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
        p++;
    return p;
}

// p is at an opening '"': just past its closing one, NULL if there is none.
static const char *skip_string(const char *p) {
    for (p++; *p; p++) {
        if (*p == '\\') {
            if (!p[1])
                return NULL;
            p++;
        } else if (*p == '"') {
            return p + 1;
        }
    }
    return NULL;
}

// p is at a value: just past it, NULL if it is cut short. Only nesting is
// checked, as this store reads nothing inside a nested value.
static const char *skip_value(const char *p) {
    if (*p == '"')
        return skip_string(p);
    if (*p == '{' || *p == '[') {
        int depth = 0;
        do {
            if (!*p)
                return NULL;
            if (*p == '"') {
                p = skip_string(p);
                if (!p)
                    return NULL;
                continue;
            }
            if (*p == '{' || *p == '[')
                depth++;
            else if (*p == '}' || *p == ']')
                depth--;
            p++;
        } while (depth > 0);
        return p;
    }
    const char *start = p;
    while (*p && *p != ',' && *p != '}' && *p != ']' && *p != ' ' &&
           *p != '\t' && *p != '\r' && *p != '\n')
        p++;
    return p > start ? p : NULL;
}

// p is at a member's key. On success *value is the value's first byte, key is
// the decoded key (cut to key_size - 1) and the result is just past the value.
static const char *member_parse(const char *p, char *key, size_t key_size,
                                const char **value) {
    const char *q = skip_string(p);
    if (!q)
        return NULL;
    flat_json_read_string(p + 1, key, key_size);
    q = skip_ws(q);
    if (*q != ':')
        return NULL;
    q = skip_ws(q + 1);
    *value = q;
    return skip_value(q);
}

// `size` bounds every member of the file together.
static bool kept_append(const char *member, size_t n, size_t size) {
    if (!s_kept) {
        s_kept = (char *)umm_malloc(size + 1);
        if (!s_kept)
            return false;
        s_kept_len = 0;
    }
    if (s_kept_len)
        s_kept[s_kept_len++] = ',';
    memcpy(s_kept + s_kept_len, member, n);
    s_kept_len += n;
    s_kept[s_kept_len] = '\0';
    return true;
}

static void kept_free(void) {
    if (s_kept)
        umm_free(s_kept);
    s_kept = NULL;
    s_kept_len = 0;
}

// Take key's member out of what is kept (key fits APPCONFIG_KEY_MAX, so a
// longer key's cut copy in `k` can never equal it).
static void kept_remove(const char *key) {
    if (!s_kept_len)
        return;
    char *p = s_kept;
    while (*p) {
        char k[APPCONFIG_KEY_MAX + 1];
        const char *value;
        const char *q = member_parse(p, k, sizeof(k), &value);
        if (!q)
            return;  // cannot happen: every member was parsed at load
        if (*q == ',')
            q++;
        if (strcmp(k, key) == 0) {
            size_t rest = (size_t)(s_kept + s_kept_len - q);
            memmove(p, q, rest + 1);
            s_kept_len = (size_t)(p - s_kept) + rest;
            if (s_kept_len && s_kept[s_kept_len - 1] == ',')
                s_kept[--s_kept_len] = '\0';
            continue;  // a repeated key: every copy goes
        }
        p = s_kept + (q - s_kept);
    }
}

static void load_members(const char *json, size_t size) {
    const char *p = skip_ws(json);
    if (*p == '{') {
        p = skip_ws(p + 1);
        while (*p == '"') {
            char key[APPCONFIG_KEY_MAX + 1];
            const char *value;
            const char *q = member_parse(p, key, sizeof(key), &value);
            if (!q)
                break;
            bool held = false;
            if (*value == '"' && key[0] && strlen(key) < APPCONFIG_KEY_MAX) {
                char val[APPCONFIG_VAL_MAX + 1];
                flat_json_read_string(value + 1, val, sizeof(val));
                if (strlen(val) < APPCONFIG_VAL_MAX)
                    held = entry_store(key, val);
            }
            if (!held && !kept_append(p, (size_t)(q - p), size)) {
                s_unread = true;
                printf("[APPCONFIG] ERROR: no memory to keep what the file holds\n");
                return;
            }
            // A comma between every two members is also what keeps s_kept, with
            // the commas it adds, within the file's size.
            p = skip_ws(q);
            if (*p != ',')
                break;
            p = skip_ws(p + 1);
        }
        if (*p == '}')
            return;
    }
    printf("[APPCONFIG] WARNING: %s is malformed at byte %d: the rest is ignored\n",
           s_app_id, (int)(p - json));
}

static bool ensure_data_dir(void) {
    char dir_path[APPCONFIG_PATH_LEN];
    snprintf(dir_path, sizeof(dir_path), "/data/%s", s_app_id);
    
    if (!sdcard_fexists(dir_path)) {
        printf("[APPCONFIG] Creating directory: %s\n", dir_path);
        if (!sdcard_mkdir(dir_path)) {
            printf("[APPCONFIG] ERROR: Failed to create directory %s\n", dir_path);
            return false;
        }
    }
    return true;
}

bool appconfig_load(const char *app_id) {
    if (!app_id || !app_id[0]) {
        printf("[APPCONFIG] ERROR: No app_id provided\n");
        return false;
    }

    s_count = 0;
    kept_free();
    s_unread = false;
    if (strlen(app_id) >= sizeof(s_app_id)) {
        // Too long to be a launcher id; never truncate into another path.
        printf("[APPCONFIG] ERROR: app_id too long\n");
        s_app_id[0] = '\0';
        return false;
    }
    strcpy(s_app_id, app_id);

    char path[APPCONFIG_PATH_LEN];
    config_path(path, sizeof(path));

    sd_atomic_recover(path);
    int len = 0;
    char *json = sdcard_read_file(path, &len);
    if (!json) {
        if (sdcard_fexists(path)) {
            s_unread = true;
            printf("[APPCONFIG] ERROR: cannot read %s: saves refused\n", path);
        } else {
            printf("[APPCONFIG] No config file at %s (first run?)\n", path);
        }
        return false;
    }

    load_members(json, (size_t)len);
    umm_free(json);
    printf("[APPCONFIG] Loaded %d entries from %s\n", s_count, path);
    return true;
}

// At most twice s's length.
static size_t put_escaped(char *buf, size_t pos, const char *s) {
    for (; *s; s++) {
        char e = *s == '\n' ? 'n' : *s == '\t' ? 't' : *s == '\r' ? 'r'
                 : *s == '\b' ? 'b' : *s == '\f' ? 'f'
                 : (*s == '"' || *s == '\\') ? *s : 0;
        if (e) {
            buf[pos++] = '\\';
            buf[pos++] = e;
        } else {
            buf[pos++] = *s;
        }
    }
    return pos;
}

bool appconfig_save(void) {
    if (!s_app_id[0]) {
        printf("[APPCONFIG] ERROR: No app_id set, cannot save\n");
        return false;
    }
    if (s_unread) {
        printf("[APPCONFIG] ERROR: the file could not be read: not saving over it\n");
        return false;
    }

    if (!ensure_data_dir()) {
        return false;
    }
    
    size_t cap = s_kept_len + 4;  // the braces, the comma before it, the NUL
    for (int i = 0; i < s_count; i++)
        cap += 2 * (strlen(s_entries[i].key) + strlen(s_entries[i].val)) + 6;
    char *buf = (char *)umm_malloc(cap);
    if (!buf) {
        printf("[APPCONFIG] ERROR: OOM during save\n");
        return false;
    }

    size_t pos = 0;
    buf[pos++] = '{';
    for (int i = 0; i < s_count; i++) {
        if (i > 0)
            buf[pos++] = ',';
        buf[pos++] = '"';
        pos = put_escaped(buf, pos, s_entries[i].key);
        buf[pos++] = '"';
        buf[pos++] = ':';
        buf[pos++] = '"';
        pos = put_escaped(buf, pos, s_entries[i].val);
        buf[pos++] = '"';
    }
    if (s_kept_len) {
        if (s_count)
            buf[pos++] = ',';
        memcpy(buf + pos, s_kept, s_kept_len);
        pos += s_kept_len;
    }
    buf[pos++] = '}';
    buf[pos] = '\0';

    char path[APPCONFIG_PATH_LEN];
    config_path(path, sizeof(path));
    bool ok = sd_atomic_write(path, buf, (int)pos);  // .tmp + rename
    umm_free(buf);
    if (!ok) {
        printf("[APPCONFIG] ERROR: save to %s failed; previous file kept\n", path);
        return false;
    }
    printf("[APPCONFIG] Saved %d entries to %s\n", s_count, path);
    return true;
}

const char *appconfig_get(const char *key, const char *fallback) {
    if (!key || !s_entries)
        return fallback;
    appconfig_entry_t *e = entry_find(key);
    return e ? e->val : fallback;
}

appconfig_status_t appconfig_put(const char *key, const char *value) {
    if (!key || !key[0])
        return APPCONFIG_BAD_KEY;
    if (strlen(key) >= APPCONFIG_KEY_MAX)
        return APPCONFIG_KEY_TOO_LONG;

    if (!value || !value[0]) {
        appconfig_entry_t *e = s_entries ? entry_find(key) : NULL;
        if (e) {
            for (appconfig_entry_t *last = &s_entries[--s_count]; e < last; e++)
                e[0] = e[1];
        }
        kept_remove(key);
        return APPCONFIG_OK;
    }

    if (strlen(value) >= APPCONFIG_VAL_MAX)
        return APPCONFIG_VALUE_TOO_LONG;
    if (!(s_entries && entry_find(key)) && s_count >= APPCONFIG_MAX_ENTRIES)
        return APPCONFIG_FULL;
    if (!entry_store(key, value))
        return APPCONFIG_NO_MEMORY;
    kept_remove(key);
    return APPCONFIG_OK;
}

const char *appconfig_status_text(appconfig_status_t status) {
    switch (status) {
    case APPCONFIG_OK: return "ok";
    case APPCONFIG_FULL: return "config full";
    case APPCONFIG_KEY_TOO_LONG: return "key too long";
    case APPCONFIG_VALUE_TOO_LONG: return "value too long";
    case APPCONFIG_BAD_KEY: return "empty key";
    default: return "out of memory";
    }
}

void appconfig_set(const char *key, const char *value) {
    appconfig_status_t st = appconfig_put(key, value);
    if (st != APPCONFIG_OK)
        printf("[APPCONFIG] WARNING: not storing '%s': %s\n", key ? key : "(null)",
               appconfig_status_text(st));
}

void appconfig_clear(void) {
    s_count = 0;
    kept_free();
    printf("[APPCONFIG] Cleared in-memory config\n");
}

bool appconfig_reset(void) {
    if (!s_app_id[0]) {
        printf("[APPCONFIG] ERROR: No app_id set, cannot reset\n");
        return false;
    }

    char path[APPCONFIG_PATH_LEN];
    config_path(path, sizeof(path));
    if (sdcard_fexists(path)) {
        sdcard_delete(path);
        printf("[APPCONFIG] Deleted config file: %s\n", path);
    }
    // A .bak left by an interrupted save would otherwise come back as the
    // config at the next load (sd_atomic_recover).
    char tmp[SD_ATOMIC_PATH_MAX], bak[SD_ATOMIC_PATH_MAX];
    if (sd_atomic_names(path, tmp, bak) && sdcard_fexists(bak))
        sdcard_delete(bak);

    s_count = 0;
    kept_free();
    s_unread = false;
    return true;
}

void appconfig_unbind(void) {
    s_app_id[0] = '\0';
    s_count = 0;
    kept_free();
    s_unread = false;
    if (s_entries)
        umm_free(s_entries);
    s_entries = NULL;
}

const char *appconfig_get_app_id(void) {
    return s_app_id[0] ? s_app_id : NULL;
}
