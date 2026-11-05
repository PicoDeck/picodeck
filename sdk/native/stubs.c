// C library support for native apps: the few functions newlib expects an OS to
// provide. Link this file when your app uses malloc, snprintf or anything else
// that needs them, with -lc -lm -lgcc (docs/Native-Loading.md, "C library").
//
// malloc gets its memory from _sbrk below: a BSS array, so it is part of the
// app's image and costs PSRAM whether it is used or not. Choose its size with
// -DPICODECK_C_HEAP_SIZE=<bytes>.
//
// There are no file descriptors. Use api->fs for files; printf prints nowhere,
// so format with snprintf and pass the text to api->sys->log.

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/stat.h>

#ifndef PICODECK_C_HEAP_SIZE
#define PICODECK_C_HEAP_SIZE (256 * 1024)
#endif

static uint8_t s_heap[PICODECK_C_HEAP_SIZE] __attribute__((aligned(8)));
static size_t s_heap_used = 0;

void *_sbrk(ptrdiff_t incr) {
    if (incr > 0 && (size_t)incr > sizeof(s_heap) - s_heap_used) {
        errno = ENOMEM;  // malloc returns NULL
        return (void *)-1;
    }
    void *prev = &s_heap[s_heap_used];
    s_heap_used += (size_t)incr;
    return prev;
}

// Nothing to return to: return from picodeck_main to leave. exit() and abort()
// end up here and stop the app with a fault, which leaves a crash record.
void _exit(int status) {
    (void)status;
    __builtin_trap();
}

int _close(int fd) { (void)fd; errno = EBADF; return -1; }
int _lseek(int fd, int offset, int whence) { (void)fd; (void)offset; (void)whence; errno = EBADF; return -1; }
int _read(int fd, char *buf, int len) { (void)fd; (void)buf; (void)len; return 0; }
int _write(int fd, const char *buf, int len) { (void)fd; (void)buf; return len; }
int _fstat(int fd, struct stat *st) { (void)fd; st->st_mode = S_IFCHR; return 0; }
int _isatty(int fd) { (void)fd; return 1; }
int _getpid(void) { return 1; }
int _kill(int pid, int sig) { (void)pid; (void)sig; errno = EINVAL; return -1; }
