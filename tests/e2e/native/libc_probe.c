// native_libc_probe — the docs' C library recipe (docs/Native-Loading.md, "C
// library"): sdk/native/stubs.c with -lc -lm -lgcc. Exercises what does not link
// without it and logs the results, which tests/e2e/test_native_sdk.py compares.
//
// Built by tests/e2e/native/Makefile into tests/e2e/fixtures/native_libc/main.elf
// (committed, so the E2E job needs no ARM toolchain).
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_abi.h"
#include "os.h"

typedef struct { char text[64]; int n; } big_t;

// volatile: the compiler must not work these out at build time.
static volatile float s_two = 2.0f;
static volatile uint64_t s_big = 10000000000ull;
static volatile uint32_t s_div = 3;

void picodeck_main(const PicoCalcAPI *api, const char *app_dir,
                   const char *app_id, const char *app_name) {
    (void)app_dir; (void)app_id;
    char buf[80];

    // memset and memcpy, which the compiler calls by itself.
    big_t a = {0};
    a.n = 7;
    big_t b = a;
    strcpy(b.text, app_name);
    api->sys->log("LIBC copy %d len %d", b.n, (int)strlen(b.text));

    // Soft-float and 64-bit helpers from libgcc.
    uint64_t q = s_big / s_div;
    api->sys->log("LIBC float %d div %d", (int)(s_two * 21.5f), (int)(q / 1000000));

    // libm.
    api->sys->log("LIBC sqrt %d sin %d", (int)sqrtf(s_two * 8.0f),
                  (int)(1000.0f * sinf(s_two)));

    // printf-family formatting.
    snprintf(buf, sizeof(buf), "%d|%5s|%04x", 42, "ab", 0xbeef);
    api->sys->log("LIBC snprintf %s", buf);
    snprintf(buf, sizeof(buf), "%.2f", 3.14159);
    api->sys->log("LIBC float format [%s]", buf);

    // malloc, from the _sbrk heap, until it says no.
    char *p = malloc(100 * 1024);
    if (p) { p[0] = 1; p[100 * 1024 - 1] = 2; }
    api->sys->log("LIBC malloc %d", p != NULL);
    free(p);
    void *huge = malloc(1024 * 1024);
    api->sys->log("LIBC malloc over the heap %d", huge != NULL);

    api->sys->log("LIBC DONE");
}
