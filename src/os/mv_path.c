#include "mv_path.h"

#include <string.h>
#include <strings.h>

bool mv_path_canon(const char *in, char *out, size_t n) {
    if (!in || in[0] != '/' || n < 2)
        return false;
    size_t len = 0;
    const char *p = in;
    while (*p) {
        while (*p == '/' || *p == '\\')
            p++;
        if (!*p)
            break;
        const char *start = p;
        while (*p && *p != '/' && *p != '\\') {
            unsigned char c = (unsigned char)*p;
            if (c < 0x20 || c == 0x7f || strchr("\"*:<>?|", c))
                return false;
            p++;
        }
        size_t clen = (size_t)(p - start);
        if ((clen == 1 && start[0] == '.') ||
            (clen == 2 && start[0] == '.' && start[1] == '.') ||
            start[clen - 1] == '.' || start[clen - 1] == ' ')
            return false;
        if (len + 1 + clen + 1 > n)
            return false;
        out[len++] = '/';
        memcpy(out + len, start, clen);
        len += clen;
    }
    if (len == 0)
        return false;
    out[len] = '\0';
    return true;
}

bool mv_path_is_root_dir(const char *canon) {
    return strcmp(canon, "/") == 0 || strcasecmp(canon, "/apps") == 0 ||
           strcasecmp(canon, "/data") == 0 || strcasecmp(canon, "/system") == 0;
}
