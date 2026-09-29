#include "toast.h"
#include "hardware/sync.h"
#include "pico/stdlib.h"
#include "../drivers/display.h"
#include "ui_widgets.h"
#include <string.h>

#define TOAST_QUEUE_SIZE 4
#define TOAST_MSG_MAX    64
#define TOAST_DURATION_MS 3000
#define TOAST_Y 280  // near the bottom of the 320px display, above the footer area

// Background colors per toast style
#define TOAST_COLOR_INFO    RGB565(40,  40,  40)    // Default gray
#define TOAST_COLOR_SUCCESS RGB565(20,  80,  30)    // Green
#define TOAST_COLOR_WARNING RGB565(120, 80,  10)    // Amber
#define TOAST_COLOR_ERROR   RGB565(120, 25,  25)    // Red

static const uint16_t s_toast_colors[] = {
    TOAST_COLOR_INFO,
    TOAST_COLOR_SUCCESS,
    TOAST_COLOR_WARNING,
    TOAST_COLOR_ERROR,
};

typedef struct {
    char     msg[TOAST_MSG_MAX];
    uint8_t  style;
    uint32_t expire_ms;  // 0 = empty slot
} toast_entry_t;

static toast_entry_t s_queue[TOAST_QUEUE_SIZE];
static uint8_t       s_head = 0;
static uint8_t       s_tail = 0;
static spin_lock_t  *s_lock;
static int           s_lock_num;
static toast_entry_t s_active;  // currently displaying (Core 0 only)

void toast_init(void) {
    s_lock_num = spin_lock_claim_unused(true);
    s_lock = spin_lock_instance(s_lock_num);
    memset(s_queue, 0, sizeof(s_queue));
    memset(&s_active, 0, sizeof(s_active));
}

void toast_push(const char *msg, uint8_t style) {
    if (!msg || !msg[0]) return;

    uint32_t save = spin_lock_blocking(s_lock);
    uint8_t next = (s_head + 1) % TOAST_QUEUE_SIZE;
    if (next == s_tail) {
        // Queue full — drop oldest
        s_tail = (s_tail + 1) % TOAST_QUEUE_SIZE;
    }
    strncpy(s_queue[s_head].msg, msg, TOAST_MSG_MAX - 1);
    s_queue[s_head].msg[TOAST_MSG_MAX - 1] = '\0';
    s_queue[s_head].style = style;
    s_queue[s_head].expire_ms = 0;  // set when dequeued for display
    s_head = next;
    spin_unlock(s_lock, save);
}

uint32_t toast_update(void) {
    uint32_t now = to_ms_since_boot(get_absolute_time());

    // If active toast expired, clear it
    if (s_active.expire_ms > 0 && now >= s_active.expire_ms) {
        s_active.expire_ms = 0;
    }

    // If no active toast, try to dequeue one
    if (s_active.expire_ms == 0) {
        uint32_t save = spin_lock_blocking(s_lock);
        if (s_head != s_tail) {
            s_active = s_queue[s_tail];
            s_tail = (s_tail + 1) % TOAST_QUEUE_SIZE;
            s_active.expire_ms = now + TOAST_DURATION_MS;
        }
        spin_unlock(s_lock, save);
    }

    // The expiry time tells one toast from the next.
    return s_active.expire_ms;
}

void toast_rect(int *x, int *y, int *w, int *h) {
    ui_widget_toast_rect(s_active.msg, x, w, h);
    *y = TOAST_Y;
}

void toast_render(void) {
    if (s_active.expire_ms == 0) return;
    uint16_t color = (s_active.style < 4) ? s_toast_colors[s_active.style] : s_toast_colors[0];
    ui_widget_toast(TOAST_Y, s_active.msg, color);
}
