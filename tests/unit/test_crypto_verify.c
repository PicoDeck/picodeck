// crypto.rsaVerify / crypto.ecdsaP256Verify (crypto_rsa_verify,
// crypto_ecdsa_p256_verify in src/os/crypto.c) against the same mbedTLS the
// firmware links: good SSH-format vectors verify, a flipped bit does not, and
// malformed blobs return false without reading outside the buffer.  The blob
// parsing itself is tested in test_ssh_blob.c, and the app's use of these
// verifiers in test_ssh_transport.c.
#include <stdlib.h>

#include "check.h"
#include "fakes/sdcard_fake.h"
#include "crypto.h"
#include "crypto_vectors.h"
#include "rng.h"
#include "ssh_handshake_vectors.h"

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

// A hex field of a recorded vector on an exact-size heap block.
static uint8_t *unhex(const char *hex, uint32_t *len) {
    *len = (uint32_t)(strlen(hex) / 2);
    uint8_t *b = malloc(*len ? *len : 1);
    for (uint32_t i = 0; i < *len; i++) {
        unsigned v;
        sscanf(hex + 2 * i, "%2x", &v);
        b[i] = (uint8_t)v;
    }
    return b;
}

// The host's signature over the exchange hash H, as OpenSSH sends it. A host
// signs H with the scheme's usual hash, so what verifies is SHA-256(H), never H.
static void check_host_signature(const char *algo, const char *k_s_hex, const char *h_hex,
                                 const char *sig_blob_hex) {
    uint32_t kl, hl, bl;
    uint8_t *k = unhex(k_s_hex, &kl), *h = unhex(h_hex, &hl), *blob = unhex(sig_blob_hex, &bl);
    bool is_rsa = strcmp(algo, "rsa-sha2-256") == 0;
    bool (*verify)(const uint8_t *, uint32_t, const uint8_t *, uint32_t,
                   const uint8_t *, uint32_t) = is_rsa ? rsa : ec;

    // string(algorithm name) + string(signature)
    uint32_t name_len = ((uint32_t)blob[0] << 24) | ((uint32_t)blob[1] << 16) |
                        ((uint32_t)blob[2] << 8) | blob[3];
    CHECK(name_len == strlen(algo) && memcmp(blob + 4, algo, name_len) == 0);
    const uint8_t *sig = blob + 4 + name_len + 4;
    uint32_t sig_len = bl - name_len - 8;

    uint8_t digest[32];
    crypto_sha256(h, hl, digest);
    CHECK_EQ_U32(hl, 32);
    CHECK(verify(k, kl, sig, sig_len, digest, 32));
    CHECK(!verify(k, kl, sig, sig_len, h, hl));
    free(k); free(h); free(blob);
}

static void test_recorded_handshakes(void) {
    for (size_t i = 0; i < sizeof SSH_HANDSHAKES / sizeof SSH_HANDSHAKES[0]; i++) {
        const ssh_handshake_t *v = &SSH_HANDSHAKES[i];
        check_host_signature(v->hostkey_algo, v->K_S, v->H, v->sig_blob);
    }
    for (size_t i = 0; i < sizeof SSH_SIGNATURES / sizeof SSH_SIGNATURES[0]; i++) {
        const ssh_signature_t *v = &SSH_SIGNATURES[i];
        check_host_signature(v->hostkey_algo, v->K_S, v->H, v->sig_blob);
    }
}

// sha256File must not hand back the hash of what it read before an error.
static void test_sha256_file_read_error(void) {
    uint8_t h[32];
    sdfake_reset();
    sdfake_put("/f.bin", "abc", 3);
    CHECK(crypto_sha256_file("/f.bin", h));
    static const uint8_t abc[4] = {0xba, 0x78, 0x16, 0xbf};
    CHECK(memcmp(h, abc, sizeof(abc)) == 0);
    sdfake_fail_reads(true);
    CHECK(!crypto_sha256_file("/f.bin", h));
    sdfake_fail_reads(false);
    CHECK(!crypto_sha256_file("/absent.bin", h));
}

int main(void) {
    test_ecdsa();
    test_rsa();
    test_recorded_handshakes();
    test_sha256_file_read_error();
    return check_report("test_crypto_verify");
}
