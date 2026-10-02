#ifndef PICODECK_BTSTACK_CONFIG_H
#define PICODECK_BTSTACK_CONFIG_H

// BTstack configuration for PicoDeck's Bluetooth gamepads (bt_pad.c): a
// Classic HID host on the CYW43, nothing else. ENABLE_CLASSIC comes from the
// SDK's pico_btstack_classic target.
//
// Memory: no HAVE_MALLOC. Every pool BTstack uses is static (MAX_NR_* below)
// and hci_stack_t is hci.c's static one, so nothing is allocated at run time,
// least of all from BTstack's callbacks in Core 0's interrupt: and all of
// that .bss (with the SDK transport's incoming packet buffer and the modules'
// small state) is linked into QMI PSRAM by bt_psram.ld, not into the ~3 KB
// of SRAM the firmware has left. The CYW43 shared-bus driver's firmware
// download buffers come from umm (CMakeLists.txt renames its malloc).

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

// ── Memory (static pools) ───────────────────────────────────────────────────
// One pad at a time: its ACL, HID control + interrupt and the SDP query's
// channel, plus room for a second connection being refused or closing.
// Undefined pools are 0 (btstack_memory.c).
#define MAX_NR_HCI_CONNECTIONS 2
#define MAX_NR_L2CAP_CHANNELS 4
#define MAX_NR_L2CAP_SERVICES 3
#define MAX_NR_HID_HOST_CONNECTIONS 2

// Link keys in the TLV store bt_pad.c keeps on the SD card: the 4 paired
// pads (BT_PAD_PAIRED_MAX) and spare room, so a key bt_pad.c has not
// dropped yet (a pairing that bonded but failed) never evicts a pad's.
#define NVM_NUM_LINK_KEYS 6

// ── HAL ─────────────────────────────────────────────────────────────────────
#define HAVE_EMBEDDED_TIME_MS
#define HAVE_ASSERT
#define HCI_RESET_RESEND_TIMEOUT_MS 1000

#endif // PICODECK_BTSTACK_CONFIG_H
