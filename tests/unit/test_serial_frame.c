// Host unit tests for src/os/serial_frame.h: the FNV-1a every dev-console
// transfer reports (published vectors) and the `screenshot` stream's header and
// trailer, byte for byte. tests/e2e/test_hw_target_protocol.py holds the host's
// reader (tools/scrn_frame.py) to the same format, and
// tests/e2e/test_screenshot_frame.py runs one against the other.
#include "check.h"
#include "serial_frame.h"

static uint32_t fnv(const char *s) {
    return serial_fnv1a_update(SERIAL_FNV1A_INIT, (const uint8_t *)s,
                               (uint32_t)strlen(s));
}

static void test_fnv1a_vectors(void) {
    CHECK_EQ_U32(fnv(""), 0x811c9dc5u);
    CHECK_EQ_U32(fnv("a"), 0xe40c292cu);
    CHECK_EQ_U32(fnv("foobar"), 0xbf9cf968u);
}

static void test_fnv1a_in_chunks(void) {
    const uint8_t *p = (const uint8_t *)"foobar";
    uint32_t h = serial_fnv1a_update(SERIAL_FNV1A_INIT, p, 2);
    h = serial_fnv1a_update(h, p + 2, 4);
    CHECK_EQ_U32(h, 0xbf9cf968u);
}

static void test_header_bytes(void) {
    uint8_t h[SCRN_HEADER_LEN];
    scrn_frame_header(h, 320, 300, SCRN_FORMAT_RGB565, SCRN_FLAG_TRAILER);
    static const uint8_t want[SCRN_HEADER_LEN] = {
        'S', 'C', 'R', 'N',
        0x40, 0x01,  // 320, little-endian
        0x2C, 0x01,  // 300
        0x35, 0x02,  // 565
        0x01, 0x00,  // the trailer flag, where the padding was
    };
    CHECK(memcmp(h, want, sizeof want) == 0);
}

static void test_trailer_bytes(void) {
    char t[SCRN_TRAILER_LEN + 1];
    memset(t, 0x55, sizeof t);
    scrn_frame_trailer(t, 0x0123abcdu);
    CHECK(strcmp(t, "\n[DEV] SCRN_END fnv1a=0123abcd\n") == 0);
    CHECK_EQ_INT((int)strlen(t), SCRN_TRAILER_LEN);
    scrn_frame_trailer(t, 0xfffffffeu);
    CHECK(strcmp(t, "\n[DEV] SCRN_END fnv1a=fffffffe\n") == 0);
    scrn_frame_trailer(t, 0u); // leading zeroes keep the length fixed
    CHECK(strcmp(t, "\n[DEV] SCRN_END fnv1a=00000000\n") == 0);
}

int main(void) {
    test_fnv1a_vectors();
    test_fnv1a_in_chunks();
    test_header_bytes();
    test_trailer_bytes();
    return check_report("test_serial_frame");
}
