// Dev commands stub for simulator
// Provides simulator-compatible implementations of dev command functions

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>
#include "../../src/os/screenshot.h"
#include "../../src/os/lua_bridge.h"
#include "../../src/os/app_stack.h"
#include "../../src/dev_ops.h"

// Command state
static bool s_cmd_exit = false;
static bool s_cmd_usb = false;
static bool s_cmd_reboot = false;
static bool s_cmd_reboot_flash = false;
static bool s_cmd_list = false;
static const char* s_pending_launch = NULL;

void dev_commands_init(void) {
    if (s_pending_launch) {
        free((void*)s_pending_launch);
        s_pending_launch = NULL;
    }
    s_cmd_exit = false;
    s_cmd_usb = false;
    s_cmd_reboot = false;
    s_cmd_reboot_flash = false;
    s_cmd_list = false;
    
    // Set stdin to non-blocking mode so getchar() doesn't block
    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    if (flags != -1) {
        fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
    }
}

const char* dev_commands_get_device(void) {
    return "/dev/ttyACM0";
}

// Forward declaration
void dev_commands_send_screenshot(void);

void dev_commands_poll(void) {
    // Check for commands from stdin (non-blocking)
    // In real hardware this comes from USB CDC serial
    static char s_cmd_buf[128];
    static int s_cmd_len = 0;
    
    int ch = fgetc(stdin);
    if (ch != EOF) {
        if (ch == '\n' || ch == '\r') {
            if (s_cmd_len > 0) {
                s_cmd_buf[s_cmd_len] = '\0';
                
                // Process command
                if (strcmp(s_cmd_buf, "exit") == 0) {
                    s_cmd_exit = true;
                } else if (strcmp(s_cmd_buf, "list") == 0) {
                    s_cmd_list = true;
                } else if (strcmp(s_cmd_buf, "screenshot") == 0) {
                    dev_commands_send_screenshot();
                } else if (strncmp(s_cmd_buf, "launch ", 7) == 0) {
                    s_pending_launch = strdup(s_cmd_buf + 7);
                    s_cmd_exit = true;  // Exit launcher to prepare for launch
                }
                
                s_cmd_len = 0;
            }
        } else if (s_cmd_len < sizeof(s_cmd_buf) - 1) {
            s_cmd_buf[s_cmd_len++] = (char)ch;
        }
    }
}

// ── Control-channel dev commands (the `dev_command` RPC) ─────────────────────
// The socket thread queues one command line; Core 0 runs it from
// dev_commands_process() — the launcher loop, or a running app's Lua hook /
// sys.sleep pump — through app_stack_run_os() and the shared dev_ops
// handlers, like a line typed on the firmware's serial console. One command
// is in flight at a time (the socket thread dispatches serially).

typedef enum { DC_IDLE, DC_QUEUED, DC_RUNNING, DC_DONE, DC_ABANDONED } dc_state_t;

static pthread_mutex_t s_dc_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_dc_cond = PTHREAD_COND_INITIALIZER;
static dc_state_t s_dc_state = DC_IDLE;
static char s_dc_cmd[300];
static char s_dc_reply[DEV_OP_REPLY_MAX];
static bool s_dc_ok = false;

typedef struct {
    char cmd[300];
    char reply[DEV_OP_REPLY_MAX];
    bool ok;
} dc_job_t;

static void dc_run(void *arg) {
    dc_job_t *job = (dc_job_t *)arg;
    printf("[DEV] Command: %s\n", job->cmd);
    if (strcmp(job->cmd, "ping") == 0) {
        snprintf(job->reply, sizeof(job->reply), "pong");
        job->ok = true;
    } else if (strcmp(job->cmd, "exit") == 0) {
        job->ok = dev_op_exit(job->reply, sizeof(job->reply));
    } else if (strncmp(job->cmd, "unzip ", 6) == 0) {
        job->ok = dev_op_unzip(job->cmd + 6, job->reply, sizeof(job->reply));
    } else if (strncmp(job->cmd, "rm ", 3) == 0) {
        job->ok = dev_op_rm(job->cmd + 3, job->reply, sizeof(job->reply));
    } else if (strncmp(job->cmd, "pad ", 4) == 0 || strcmp(job->cmd, "pad") == 0) {
        job->ok = dev_op_pad(job->cmd + 3, job->reply, sizeof(job->reply));
    } else if (strncmp(job->cmd, "bt ", 3) == 0 || strcmp(job->cmd, "bt") == 0) {
        job->ok = dev_op_bt(job->cmd + 2, job->reply, sizeof(job->reply));
    } else if (strcmp(job->cmd, "audiostat") == 0 ||
               strcmp(job->cmd, "audiostat reset") == 0) {
        job->ok = dev_op_audiostat(strcmp(job->cmd, "audiostat reset") == 0,
                                   job->reply, sizeof(job->reply));
    } else {
        snprintf(job->reply, sizeof(job->reply),
                 "Unknown command: %s (simulator supports ping, exit, unzip, "
                 "rm, pad, bt, audiostat)", job->cmd);
        job->ok = false;
    }
    printf("[DEV] %s\n", job->reply);
    fflush(stdout);
}

bool dev_commands_process(void) {
    pthread_mutex_lock(&s_dc_mutex);
    if (s_dc_state != DC_QUEUED) {
        pthread_mutex_unlock(&s_dc_mutex);
        return false;
    }
    s_dc_state = DC_RUNNING;
    static dc_job_t job;  // Core 0 only; not re-entered (commands don't pump)
    snprintf(job.cmd, sizeof(job.cmd), "%s", s_dc_cmd);
    pthread_mutex_unlock(&s_dc_mutex);

    job.reply[0] = '\0';
    job.ok = false;
    if (!app_stack_run_os(dc_run, &job))
        snprintf(job.reply, sizeof(job.reply),
                 "Error: no memory for the command stack");

    pthread_mutex_lock(&s_dc_mutex);
    if (s_dc_state == DC_RUNNING) {
        snprintf(s_dc_reply, sizeof(s_dc_reply), "%s", job.reply);
        s_dc_ok = job.ok;
        s_dc_state = DC_DONE;
        pthread_cond_broadcast(&s_dc_cond);
    } else {
        s_dc_state = DC_IDLE;  // the RPC gave up waiting
    }
    pthread_mutex_unlock(&s_dc_mutex);
    return true;
}

bool dev_commands_sim_run(const char *cmd, int timeout_ms, char *reply,
                          size_t reply_len, bool *ok) {
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += timeout_ms / 1000;
    deadline.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }

    pthread_mutex_lock(&s_dc_mutex);
    if (s_dc_state != DC_IDLE) {
        pthread_mutex_unlock(&s_dc_mutex);
        snprintf(reply, reply_len, "Error: a previous command is still running");
        *ok = false;
        return false;
    }
    snprintf(s_dc_cmd, sizeof(s_dc_cmd), "%s", cmd);
    s_dc_state = DC_QUEUED;
    lua_bridge_request_service();  // a running app's next hook pumps it
    int rc = 0;
    while (s_dc_state != DC_DONE && rc != ETIMEDOUT)
        rc = pthread_cond_timedwait(&s_dc_cond, &s_dc_mutex, &deadline);
    bool done = (s_dc_state == DC_DONE);
    if (done) {
        snprintf(reply, reply_len, "%s", s_dc_reply);
        *ok = s_dc_ok;
        s_dc_state = DC_IDLE;
    } else {
        // Not picked up (nothing is pumping dev commands) or still running:
        // withdraw it, or let the runner drop the result when it finishes.
        s_dc_state = (s_dc_state == DC_QUEUED) ? DC_IDLE : DC_ABANDONED;
        snprintf(reply, reply_len, "Error: timed out waiting for Core 0");
        *ok = false;
    }
    pthread_mutex_unlock(&s_dc_mutex);
    return done;
}

void dev_commands_send_screenshot(void) {
    // Screenshot implementation for simulator - saves BMP to host filesystem
    extern void screenshot_save(void);
    screenshot_save();
}

// Exit command
bool dev_commands_wants_exit(void) {
    return s_cmd_exit;
}

void dev_commands_clear_exit(void) {
    s_cmd_exit = false;
}

void dev_commands_set_exit(void) {
    s_cmd_exit = true;
}

// Bulk-transfer quiet flag — no async log producers in the simulator.
bool dev_commands_transfer_active(void) {
    return false;
}

// USB command
bool dev_commands_wants_usb(void) {
    return s_cmd_usb;
}

void dev_commands_clear_usb(void) {
    s_cmd_usb = false;
}

// List command
bool dev_commands_wants_list(void) {
    return s_cmd_list;
}

void dev_commands_clear_list(void) {
    s_cmd_list = false;
}

// Reboot commands
bool dev_commands_wants_reboot(void) {
    return s_cmd_reboot;
}

bool dev_commands_wants_reboot_flash(void) {
    return s_cmd_reboot_flash;
}

bool dev_commands_wants_reboot_ota(void) {
    return false;  // OTA flashing is firmware-only
}

void dev_commands_clear_reboot_ota(void) {}

// Launch command
const char* dev_commands_get_pending_launch(void) {
    return s_pending_launch;
}

void dev_commands_clear_pending_launch(void) {
    if (s_pending_launch) {
        free((void*)s_pending_launch);
        s_pending_launch = NULL;
    }
}

// Additional functions referenced by launcher
void dev_commands_send_keypress(const char* key) {
    (void)key;
    // Key injection handled via SDL in keyboard_stub.c
}
