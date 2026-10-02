#ifndef PICODECK_BTSTACK_CONFIG_H
#define PICODECK_BTSTACK_CONFIG_H

// BTstack configuration for PicoDeck's Bluetooth gamepads (bt_pad.c): a
// Classic HID host on the CYW43, nothing else. ENABLE_CLASSIC comes from the
// SDK's pico_btstack_classic target.
//
// Memory: HAVE_MALLOC, and CMakeLists.txt builds hci.c and btstack_memory.c
// with malloc/free renamed to bt_pad_malloc/bt_pad_free (umm_malloc, QMI
// PSRAM): hci_stack_t (with its packet buffer) and every pooled object
// (connections, L2CAP channels, the HID host connection) live in PSRAM, not
// in the ~3 KB of SRAM the firmware has left. What stays static in SRAM is
// the SDK transport's incoming packet buffer (HCI_INCOMING_PACKET_BUFFER_SIZE,
// from HCI_ACL_PAYLOAD_SIZE below) and the modules' small state.

// ── Features ────────────────────────────────────────────────────────────────
#define ENABLE_LOG_ERROR
#define ENABLE_PRINTF_HEXDUMP

// ── Buffers ─────────────────────────────────────────────────────────────────
// The cyw43 transport puts a 4-byte header in front of every packet it sends
// and wants word-aligned ACL fragments (btstack_hci_transport_cyw43.c).
#define HCI_OUTGOING_PRE_BUFFER_SIZE 4
#define HCI_ACL_CHUNK_SIZE_ALIGNMENT 4
// Smallest that still takes a whole HCI event (2 + 255 bytes); L2CAP's MTU
// is 4 less (255): HID input reports are 10-79 bytes, SDP answers come in
// continuations.
#define HCI_ACL_PAYLOAD_SIZE (255 + 4)

// Limit the ACL buffers the stack uses, and turn on controller-to-host flow
// control, so the cyw43 shared bus never overruns (the SDK's settings).
#define MAX_NR_CONTROLLER_ACL_BUFFERS 3
#define MAX_NR_CONTROLLER_SCO_PACKETS 3
#define ENABLE_HCI_CONTROLLER_TO_HOST_FLOW_CONTROL
#define HCI_HOST_ACL_PACKET_LEN HCI_ACL_PAYLOAD_SIZE
#define HCI_HOST_ACL_PACKET_NUM 3
#define HCI_HOST_SCO_PACKET_LEN 120
#define HCI_HOST_SCO_PACKET_NUM 3

// ── Memory ──────────────────────────────────────────────────────────────────
#define HAVE_MALLOC

// Bonded pads (link keys in the TLV store bt_pad.c keeps on the SD card).
#define NVM_NUM_LINK_KEYS 4

// ── HAL ─────────────────────────────────────────────────────────────────────
#define HAVE_EMBEDDED_TIME_MS
#define HAVE_ASSERT
#define HCI_RESET_RESEND_TIMEOUT_MS 1000

#endif // PICODECK_BTSTACK_CONFIG_H
