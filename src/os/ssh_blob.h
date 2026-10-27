#pragma once

// SSH wire-format blob parsing for crypto.c's signature verifiers (pure; no
// mbedTLS).  Host-tested in tests/unit/test_ssh_blob.c.  Every length is a
// big-endian uint32 checked against the bytes left (never as `p + len > end`,
// which wraps on the 32-bit target); the outputs point into the input.

#include <stdbool.h>
#include <stdint.h>

// string("ssh-rsa") + mpint(e) + mpint(n)
bool ssh_rsa_pubkey_parse(const uint8_t *blob, uint32_t len,
                          const uint8_t **e, uint32_t *e_len,
                          const uint8_t **n, uint32_t *n_len);

// string("ecdsa-sha2-nistp256") + string("nistp256") + string(Q)
bool ssh_ecdsa_pubkey_parse(const uint8_t *blob, uint32_t len,
                            const uint8_t **q, uint32_t *q_len);

// mpint(r) + mpint(s); bytes after s are ignored.
bool ssh_ecdsa_sig_parse(const uint8_t *sig, uint32_t len,
                         const uint8_t **r, uint32_t *r_len,
                         const uint8_t **s, uint32_t *s_len);
