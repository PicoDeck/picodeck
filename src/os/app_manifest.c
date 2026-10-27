#include "app_manifest.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

// All scanning is bounded by [p, end); end is json + len or the first NUL.

static const char *find_bytes(const char *p, const char *end, const char *s,
                              size_t n) {
    for (; p && (size_t)(end - p) >= n; p++) {
        if (memcmp(p, s, n) == 0)
            return p;
    }
    return NULL;
}

// Next occurrence of "key" (with its quotes) at or after p; returns the
// position just past the closing quote, or NULL.
static const char *find_key(const char *p, const char *end, const char *key) {
    char search[64];
    int n = snprintf(search, sizeof(search), "\"%s\"", key);
    if (n <= 0 || (size_t)n >= sizeof(search))
        return NULL;
    const char *hit = find_bytes(p, end, search, (size_t)n);
    return hit ? hit + n : NULL;
}

static const char *skip_sep(const char *q, const char *end) {
    while (q < end && (*q == ' ' || *q == ':' || *q == '\t' || *q == '\r' ||
                       *q == '\n'))
        q++;
    return q;
}

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Decode the string body starting just after its opening quote.
static void read_string(const char *q, const char *end, char *out,
                        size_t out_len) {
    size_t i = 0;
    while (q < end && *q != '"' && i + 1 < out_len) {
        char c = *q++;
        if (c == '\\' && q < end) {
            char e = *q++;
            switch (e) {
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case 'r': c = '\r'; break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case 'u': {
                uint32_t cp = 0;
                int k = 0;
                for (; k < 4 && q < end; k++, q++) {
                    int h = hex_val(*q);
                    if (h < 0)
                        break;
                    cp = cp * 16u + (uint32_t)h;
                }
                c = (k == 4 && cp > 0 && cp < 0x80) ? (char)cp : '?';
                break;
            }
            default: c = e; break;  // \" \\ \/ and anything else literally
            }
        }
        out[i++] = c;
    }
    out[i] = '\0';
}

// String value of "key".  A match whose value is not a string (e.g. the key
// text appearing inside another value) is skipped and the search continues.
static bool json_get_string(const char *json, const char *end,
                            const char *key, char *out, size_t out_len) {
    const char *p = json;
    while ((p = find_key(p, end, key)) != NULL) {
        const char *q = skip_sep(p, end);
        if (q < end && *q == '"') {
            read_string(q + 1, end, out, out_len);
            return true;
        }
    }
    return false;
}

// Integer value of the first "key" (atoi semantics: leading '-' allowed,
// stops at the first non-digit, no digits = 0).
static bool json_get_int(const char *json, const char *end, const char *key,
                         uint32_t *out) {
    const char *p = find_key(json, end, key);
    if (!p)
        return false;
    p = skip_sep(p, end);
    bool neg = false;
    if (p < end && (*p == '-' || *p == '+')) {
        neg = (*p == '-');
        p++;
    }
    uint32_t v = 0;
    while (p < end && *p >= '0' && *p <= '9')
        v = v * 10u + (uint32_t)(*p++ - '0');
    *out = neg ? (uint32_t)(0u - v) : v;
    return true;
}

static bool id_char_ok(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
}

static void fold_lower(char *s) {
    for (; *s; s++) {
        if (*s >= 'A' && *s <= 'Z')
            *s = (char)(*s - 'A' + 'a');
    }
}

// The "requirements" array as a space-separated list of folded names.
// Non-string elements and names outside [a-z0-9._-] are skipped; a name that
// does not fit is dropped whole.  An unterminated or non-array value gives
// an empty list.
static void json_get_requirements(const char *json, const char *end,
                                  char *out, size_t out_len) {
    out[0] = '\0';
    const char *p = find_key(json, end, "requirements");
    if (!p)
        return;
    p = skip_sep(p, end);
    if (p >= end || *p != '[')
        return;
    const char *close = p;
    while (close < end && *close != ']')
        close++;
    if (close >= end)
        return;  // unterminated array

    size_t used = 0;
    for (p++; p < close; p++) {
        if (*p != '"')
            continue;
        const char *q = p + 1;  // find the closing quote, skipping escapes
        while (q < close && *q != '"')
            q += (*q == '\\' && q + 1 < close) ? 2 : 1;
        char name[48];
        read_string(p + 1, q, name, sizeof(name));
        bool truncated = (size_t)(q - (p + 1)) >= sizeof(name);
        p = q;  // loop increment steps past the closing quote
        fold_lower(name);
        size_t n = strlen(name);
        bool ok = n > 0 && !truncated;
        for (size_t i = 0; ok && i < n; i++)
            ok = id_char_ok(name[i]);
        if (!ok || used + (used ? 1 : 0) + n + 1 > out_len)
            continue;
        if (used)
            out[used++] = ' ';
        memcpy(out + used, name, n + 1);
        used += n;
    }
}

bool app_requirements_has(const char *list, const char *name) {
    if (!list || !name || !name[0])
        return false;
    size_t n = strlen(name);
    for (const char *p = list; *p;) {
        const char *sp = strchr(p, ' ');
        size_t len = sp ? (size_t)(sp - p) : strlen(p);
        if (len == n && memcmp(p, name, n) == 0)
            return true;
        if (!sp)
            break;
        p = sp + 1;
    }
    return false;
}

// "local.<dir>" with every byte outside [A-Za-z0-9._-] (and the second dot
// of a "..") replaced by '_', then folded: always a valid id.
static void default_id(const char *dir_name, char *out, size_t out_len) {
    int n = snprintf(out, out_len, "local.%s", dir_name ? dir_name : "");
    if (n < 0)
        out[0] = '\0';
    for (size_t i = 6; out[i]; i++) {
        if (!id_char_ok(out[i]) || (out[i] == '.' && out[i - 1] == '.'))
            out[i] = '_';
    }
    // FatFS strips a trailing dot (see app_manifest_id_valid).
    size_t len = strlen(out);
    if (len > 6 && out[len - 1] == '.')
        out[len - 1] = '_';
    fold_lower(out);
}

static size_t bounded_len(const char *s, size_t max) {
    size_t n = 0;
    while (n < max && s[n])
        n++;
    return n;
}

static void copy_str(char *dst, size_t n, const char *src) {
    snprintf(dst, n, "%s", src ? src : "");
}

// The clocks launcher_apply_clock has been validated at (VREG step, PLL
// solution, QMI/PIO PSRAM and LCD timing), plus 0 = the OS default.
bool app_manifest_clock_valid(uint32_t khz) {
    static const uint32_t k_clocks[] = {0, 125000, 150000, 200000, 250000,
                                        300000};
    for (size_t i = 0; i < sizeof(k_clocks) / sizeof(k_clocks[0]); i++)
        if (khz == k_clocks[i])
            return true;
    return false;
}

void app_manifest_parse(const char *json, size_t len, const char *dir_name,
                        app_entry_t *app) {
    const char *end = json ? json + bounded_len(json, len) : json;

    if (!json || !json_get_string(json, end, "id", app->id, sizeof(app->id)))
        default_id(dir_name, app->id, sizeof(app->id));
    fold_lower(app->id);  // FatFS is case-insensitive: one spelling per app
    if (!json || !json_get_string(json, end, "name", app->name, sizeof(app->name)))
        copy_str(app->name, sizeof(app->name), dir_name);
    if (!json || !json_get_string(json, end, "description", app->description,
                                  sizeof(app->description)))
        app->description[0] = '\0';
    if (!json || !json_get_string(json, end, "version", app->version,
                                  sizeof(app->version)))
        copy_str(app->version, sizeof(app->version), "1.0");

    if (json)
        json_get_requirements(json, end, app->requirements,
                              sizeof(app->requirements));
    else
        app->requirements[0] = '\0';
    app->has_root_filesystem =
        app_requirements_has(app->requirements, "root-filesystem");
    app->has_http = app_requirements_has(app->requirements, "http");
    app->has_audio = app_requirements_has(app->requirements, "audio");
    app->system_clock_khz = 0;
    app->min_psram_kb = 0;
    if (json) {
        json_get_int(json, end, "system_clock_khz", &app->system_clock_khz);
        json_get_int(json, end, "min_psram_kb", &app->min_psram_kb);
    }
    if (!app_manifest_clock_valid(app->system_clock_khz)) {
        printf("[MANIFEST] %s: system_clock_khz %ld is not a supported clock "
               "(125000/150000/200000/250000/300000); using the default\n",
               dir_name ? dir_name : "?", (long)(int32_t)app->system_clock_khz);
        app->system_clock_khz = 0;
    }
    if (!json || !json_get_string(json, end, "category", app->category,
                                  sizeof(app->category)))
        app->category[0] = '\0';
}

void app_manifest_defaults(const char *dir_name, app_entry_t *app) {
    default_id(dir_name, app->id, sizeof(app->id));
    app->requirements[0] = '\0';
    copy_str(app->name, sizeof(app->name), dir_name);
    app->description[0] = '\0';
    copy_str(app->version, sizeof(app->version), "?");
    app->category[0] = '\0';
    app->has_root_filesystem = false;
    app->has_http = false;
    app->has_audio = false;
    app->system_clock_khz = 0;
    app->min_psram_kb = 0;
}

bool app_manifest_id_valid(const char *id) {
    if (!id || !id[0])
        return false;
    size_t n = bounded_len(id, sizeof(((app_entry_t *)0)->id));
    if (n >= sizeof(((app_entry_t *)0)->id))
        return false;
    for (size_t i = 0; i < n; i++) {
        if (!id_char_ok(id[i]))
            return false;
    }
    if (strcmp(id, ".") == 0 || strstr(id, ".."))
        return false;
    // FatFS strips trailing dots from a path component, so "com.victim."
    // would name /data/com.victim — another app's data.
    if (id[n - 1] == '.')
        return false;
    return true;
}

bool app_hidden_name_valid(const char *name) {
    if (!name || !name[0] || name[0] == '.')
        return false;
    size_t n = bounded_len(name, APP_HIDDEN_NAME_MAX + 1);
    if (n > APP_HIDDEN_NAME_MAX)
        return false;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c == '/' || c == '\\' || c < 0x20 || c == 0x7f)
            return false;
    }
    return strstr(name, "..") == NULL && name[n - 1] != '.';
}
