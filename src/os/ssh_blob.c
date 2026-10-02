#include "ssh_blob.h"

// Read one length-prefixed field at *p; on success *p moves past it.
static bool next_field(const uint8_t **p, const uint8_t *end,
                       const uint8_t **data, uint32_t *flen) {
    if ((uint32_t)(end - *p) < 4) return false;
    const uint8_t *q = *p;
    uint32_t n = ((uint32_t)q[0] << 24) | ((uint32_t)q[1] << 16) |
                 ((uint32_t)q[2] << 8) | q[3];
    if (n > (uint32_t)(end - q - 4)) return false;
    *data = q + 4;
    *flen = n;
    *p = q + 4 + n;
    return true;
}

bool ssh_rsa_pubkey_parse(const uint8_t *blob, uint32_t len,
                          const uint8_t **e, uint32_t *e_len,
                          const uint8_t **n, uint32_t *n_len) {
    const uint8_t *p = blob, *end = blob + len, *skip;
    uint32_t skip_len;
    return next_field(&p, end, &skip, &skip_len) &&
           next_field(&p, end, e, e_len) &&
           next_field(&p, end, n, n_len);
}

bool ssh_ecdsa_pubkey_parse(const uint8_t *blob, uint32_t len,
                            const uint8_t **q, uint32_t *q_len) {
    const uint8_t *p = blob, *end = blob + len, *skip;
    uint32_t skip_len;
    return next_field(&p, end, &skip, &skip_len) &&
           next_field(&p, end, &skip, &skip_len) &&
           next_field(&p, end, q, q_len);
}

bool ssh_ecdsa_sig_parse(const uint8_t *sig, uint32_t len,
                         const uint8_t **r, uint32_t *r_len,
                         const uint8_t **s, uint32_t *s_len) {
    const uint8_t *p = sig, *end = sig + len;
    return next_field(&p, end, r, r_len) && next_field(&p, end, s, s_len);
}
