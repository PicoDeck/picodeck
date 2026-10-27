#include "ssh_blob.h"

// Read the length-prefixed field at buf[*off]; on success *off moves past it.
// Offsets, not pointers, and each test is written so it cannot overflow
// uint32_t (`*off + 4 + n > len` wraps for n near 2^32, and a pointer sum
// wraps on the 32-bit target but not on a 64-bit host).
static bool next_field(const uint8_t *buf, uint32_t len, uint32_t *off,
                       const uint8_t **data, uint32_t *flen) {
    if (*off > len || len - *off < 4) return false;
    const uint8_t *q = buf + *off;
    uint32_t n = ((uint32_t)q[0] << 24) | ((uint32_t)q[1] << 16) |
                 ((uint32_t)q[2] << 8) | q[3];
    if (n > len - *off - 4) return false;
    *data = q + 4;
    *flen = n;
    *off += 4 + n;
    return true;
}

bool ssh_rsa_pubkey_parse(const uint8_t *blob, uint32_t len,
                          const uint8_t **e, uint32_t *e_len,
                          const uint8_t **n, uint32_t *n_len) {
    const uint8_t *skip;
    uint32_t off = 0;
    uint32_t skip_len;
    return next_field(blob, len, &off, &skip, &skip_len) &&
           next_field(blob, len, &off, e, e_len) &&
           next_field(blob, len, &off, n, n_len);
}

bool ssh_ecdsa_pubkey_parse(const uint8_t *blob, uint32_t len,
                            const uint8_t **q, uint32_t *q_len) {
    const uint8_t *skip;
    uint32_t off = 0;
    uint32_t skip_len;
    return next_field(blob, len, &off, &skip, &skip_len) &&
           next_field(blob, len, &off, &skip, &skip_len) &&
           next_field(blob, len, &off, q, q_len);
}

bool ssh_ecdsa_sig_parse(const uint8_t *sig, uint32_t len,
                         const uint8_t **r, uint32_t *r_len,
                         const uint8_t **s, uint32_t *s_len) {
    uint32_t off = 0;
    return next_field(sig, len, &off, r, r_len) && next_field(sig, len, &off, s, s_len);
}
