#pragma once

#include <stdbool.h>

void dev_commands_init(void);

const char* dev_commands_get_device(void);

void dev_commands_poll(void);

bool dev_commands_process(void);

void dev_commands_send_screenshot(void);

bool dev_commands_wants_exit(void);

void dev_commands_clear_exit(void);

void dev_commands_set_exit(void);

// True while a bulk serial transfer (b64 or raw CDC) is streaming.  Async
// log producers (Core 1's mongoose sink in wifi.c) must drop their output
// while this is set, or their lines interleave mid-payload and corrupt the
// transfer.
bool dev_commands_transfer_active(void);

bool dev_commands_wants_usb(void);

void dev_commands_clear_usb(void);

bool dev_commands_wants_list(void);

// True when the pending `list` was `list all` (hidden apps too).
bool dev_commands_list_all(void);

void dev_commands_clear_list(void);

bool dev_commands_wants_reboot(void);

// `reboot-ota`: apply the staged /system/update.bin (launcher context only).
bool dev_commands_wants_reboot_ota(void);
void dev_commands_clear_reboot_ota(void);

bool dev_commands_wants_reboot_flash(void);

const char* dev_commands_get_pending_launch(void);

void dev_commands_clear_pending_launch(void);
