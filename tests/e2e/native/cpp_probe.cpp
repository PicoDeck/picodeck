// native_cpp_probe — what the docs say works in C++ (docs/Native-Loading.md, "C++"):
// classes, virtual calls, templates and a supplied operator new, with no static
// constructors. tests/e2e/test_native_sdk.py compares what it logs.
//
// Built by tests/e2e/native/Makefile into tests/e2e/fixtures/native_cpp/main.elf
// (committed, so the E2E job needs no ARM toolchain).
#include <stddef.h>

#include "app_abi.h"
#include "os.h"

static const PicoCalcAPI *g_api_ptr;

void *operator new(size_t n) { return g_api_ptr->psram->qmiAlloc(n); }
void *operator new[](size_t n) { return g_api_ptr->psram->qmiAlloc(n); }
void operator delete(void *p) noexcept { g_api_ptr->psram->qmiFree(p); }
void operator delete[](void *p) noexcept { g_api_ptr->psram->qmiFree(p); }
void operator delete(void *p, size_t) noexcept { g_api_ptr->psram->qmiFree(p); }
void operator delete[](void *p, size_t) noexcept { g_api_ptr->psram->qmiFree(p); }

class Shape {
public:
    virtual ~Shape() {}
    virtual int area() const = 0;
};

class Square : public Shape {
public:
    explicit Square(int side) : side_(side) {}
    int area() const override { return side_ * side_; }
private:
    int side_;
};

template <typename T>
static T twice(T v) { return v + v; }

struct Constructed {
    int v;
    Constructed() : v(42) {}
};
// Its constructor is in .init_array, which the loader does not walk.
static Constructed s_global;

// No extern "C" here: app_abi.h declares it with C linkage for C++.
void picodeck_main(const PicoCalcAPI *api, const char *, const char *, const char *) {
    g_api_ptr = api;
    Shape *s = new Square(6);
    api->sys->log("CPP virtual %d template %d", s->area(), twice(21));
    delete s;
    int *many = new int[10];
    many[9] = 5;
    api->sys->log("CPP array %d", many[9]);
    delete[] many;
    api->sys->log("CPP static constructor ran %d", s_global.v == 42);
    api->sys->log("CPP DONE");
}
