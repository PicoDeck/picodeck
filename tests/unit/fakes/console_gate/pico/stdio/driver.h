#pragma once
typedef struct stdio_driver {
    void (*out_chars)(const char *buf, int len);
    void (*out_flush)(void);
} stdio_driver_t;
