// crypto.rsaVerify / crypto.ecdsaP256Verify (crypto_rsa_verify,
// crypto_ecdsa_p256_verify in src/os/crypto.c) against the same mbedTLS the
// firmware links: good SSH-format vectors verify, a flipped bit does not, and
// malformed blobs return false without reading outside the buffer.  The blob
// parsing itself is tested in test_ssh_blob.c.
#include <stdlib.h>

#include "check.h"
#include "crypto.h"
#include "crypto_vectors.h"
#include "rng.h"

// crypto.c's RNG hooks (src/drivers/rng.c needs the TRNG); the verifiers use none.
bool rng_bytes(void *buf, size_t len) { memset(buf, 0, len); return true; }
int rng_mbedtls_random(void *p, unsigned char *out, size_t len) {
    (void)p; memset(out, 0, len); return 0;
}

static uint8_t *dup(const uint8_t *b, uint32_t n) {
    uint8_t *p = malloc(n ? n : 1);
    memcpy(p, b, n);
    return p;
}

// Verify on exact-size heap copies so ASan flags any over-read.
static bool rsa(const uint8_t *k, uint32_t kl, const uint8_t *s, uint32_t sl,
                const uint8_t *h, uint32_t hl) {
    uint8_t *kc = dup(k, kl), *sc = dup(s, sl), *hc = dup(h, hl);
    bool ok = crypto_rsa_verify(kc, kl, sc, sl, hc, hl);
    free(kc); free(sc); free(hc);
    return ok;
}
static bool ec(const uint8_t *k, uint32_t kl, const uint8_t *s, uint32_t sl,
               const uint8_t *h, uint32_t hl) {
    uint8_t *kc = dup(k, kl), *sc = dup(s, sl), *hc = dup(h, hl);
    bool ok = crypto_ecdsa_p256_verify(kc, kl, sc, sl, hc, hl);
    free(kc); free(sc); free(hc);
    return ok;
}

static void test_ecdsa(void) {
    CHECK(ec(EC_SSH_PUB, sizeof EC_SSH_PUB, EC_SSH_SIG, sizeof EC_SSH_SIG, VEC_HASH, 32));
    for (unsigned i = 0; i < sizeof EC_SSH_SIG; i++) {
        uint8_t *bad = dup(EC_SSH_SIG, sizeof EC_SSH_SIG);
        bad[i] ^= 1;      // a length byte or a r/s bit: never a valid signature
        CHECK(!ec(EC_SSH_PUB, sizeof EC_SSH_PUB, bad, sizeof EC_SSH_SIG, VEC_HASH, 32));
        free(bad);
    }
    uint8_t h2[32]; memcpy(h2, VEC_HASH, 32); h2[0] ^= 1;
    CHECK(!ec(EC_SSH_PUB, sizeof EC_SSH_PUB, EC_SSH_SIG, sizeof EC_SSH_SIG, h2, 32));
    // The hash must be exactly 32 bytes.
    CHECK(!ec(EC_SSH_PUB, sizeof EC_SSH_PUB, EC_SSH_SIG, sizeof EC_SSH_SIG, VEC_HASH, 31));
    // Truncated blobs, a wrapping length, an empty signature.
    for (uint32_t n = 0; n < sizeof EC_SSH_SIG; n++)
        CHECK(!ec(EC_SSH_PUB, sizeof EC_SSH_PUB, EC_SSH_SIG, n, VEC_HASH, 32));
    for (uint32_t n = 0; n < sizeof EC_SSH_PUB; n++)
        CHECK(!ec(EC_SSH_PUB, n, EC_SSH_SIG, sizeof EC_SSH_SIG, VEC_HASH, 32));
    uint8_t wrap[12] = {0xFF, 0xFF, 0xFF, 0xF0};
    CHECK(!ec(EC_SSH_PUB, sizeof EC_SSH_PUB, wrap, sizeof wrap, VEC_HASH, 32));
    uint8_t zero[8] = {0};                      // r = s = ""
    CHECK(!ec(EC_SSH_PUB, sizeof EC_SSH_PUB, zero, sizeof zero, VEC_HASH, 32));
}

static void test_rsa(void) {
    CHECK(rsa(RSA_SSH_PUB, sizeof RSA_SSH_PUB, RSA_SIG, sizeof RSA_SIG, VEC_HASH, 32));
    for (unsigned i = 0; i < sizeof RSA_SIG; i += 7) {
        uint8_t *bad = dup(RSA_SIG, sizeof RSA_SIG);
        bad[i] ^= 1;
        CHECK(!rsa(RSA_SSH_PUB, sizeof RSA_SSH_PUB, bad, sizeof RSA_SIG, VEC_HASH, 32));
        free(bad);
    }
    CHECK(!rsa(RSA_SSH_PUB, sizeof RSA_SSH_PUB, RSA_SIG, sizeof RSA_SIG, VEC_HASH, 20));
    // The signature must be exactly the modulus length.
    CHECK(!rsa(RSA_SSH_PUB, sizeof RSA_SSH_PUB, RSA_SIG, sizeof RSA_SIG - 1, VEC_HASH, 32));
    uint8_t *longsig = calloc(sizeof RSA_SIG + 1, 1);
    memcpy(longsig, RSA_SIG, sizeof RSA_SIG);
    CHECK(!rsa(RSA_SSH_PUB, sizeof RSA_SSH_PUB, longsig, sizeof RSA_SIG + 1, VEC_HASH, 32));
    free(longsig);
    for (uint32_t n = 0; n < sizeof RSA_SSH_PUB; n++)
        CHECK(!rsa(RSA_SSH_PUB, n, RSA_SIG, sizeof RSA_SIG, VEC_HASH, 32));
    CHECK(!rsa((const uint8_t *)"junk", 4, (const uint8_t *)"junk", 4, VEC_HASH, 32));
}

int main(void) {
    test_ecdsa();
    test_rsa();
    return check_report("test_crypto_verify");
}
