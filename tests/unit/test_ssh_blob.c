// SSH wire-format parsers behind crypto.rsaVerify / crypto.ecdsaP256Verify
// (src/os/ssh_blob.c): the good vectors parse to the right fields, and crafted
// blobs (huge lengths, one past the end, truncation, zero-length mpints) are
// rejected without reading outside the buffer.  Each blob is copied to an
// exact-size heap block so ASan sees any over-read.  The verify step itself
// needs mbedTLS (crypto.c) and is covered by test_crypto_verify.c.
#include <stdlib.h>

#include "check.h"
#include "crypto_vectors.h"
#include "ssh_blob.h"

static uint8_t *dup(const uint8_t *b, uint32_t n) {
    uint8_t *p = malloc(n ? n : 1);
    memcpy(p, b, n);
    return p;
}

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

// Each parser, on an exact-size copy of the first n bytes of its blob.
static bool rsa_key(const uint8_t *b, uint32_t n) {
    const uint8_t *e, *m; uint32_t el, ml;
    uint8_t *c = dup(b, n);
    bool ok = ssh_rsa_pubkey_parse(c, n, &e, &el, &m, &ml);
    free(c);
    return ok;
}
static bool ec_key(const uint8_t *b, uint32_t n) {
    const uint8_t *q; uint32_t ql;
    uint8_t *c = dup(b, n);
    bool ok = ssh_ecdsa_pubkey_parse(c, n, &q, &ql);
    free(c);
    return ok;
}
static bool ec_sig(const uint8_t *b, uint32_t n) {
    const uint8_t *r, *s; uint32_t rl, sl;
    uint8_t *c = dup(b, n);
    bool ok = ssh_ecdsa_sig_parse(c, n, &r, &rl, &s, &sl);
    free(c);
    return ok;
}

static void test_good_vectors(void) {
    const uint8_t *a, *b; uint32_t al, bl;
    CHECK(ssh_rsa_pubkey_parse(RSA_SSH_PUB, sizeof RSA_SSH_PUB, &a, &al, &b, &bl));
    CHECK_EQ_U32(al, 3);
    CHECK(a[0] == 1 && a[1] == 0 && a[2] == 1);          // e = 65537
    CHECK_EQ_U32(bl, 129);                               // 1024-bit n + 0 pad
    CHECK(b + bl == RSA_SSH_PUB + sizeof RSA_SSH_PUB);

    CHECK(ssh_ecdsa_pubkey_parse(EC_SSH_PUB, sizeof EC_SSH_PUB, &a, &al));
    CHECK_EQ_U32(al, 65);
    CHECK_EQ_U32(a[0], 4);                               // uncompressed point

    CHECK(ssh_ecdsa_sig_parse(EC_SSH_SIG, sizeof EC_SSH_SIG, &a, &al, &b, &bl));
    CHECK_EQ_U32(al, 32);
    CHECK_EQ_U32(bl, 33);
    CHECK(b + bl == EC_SSH_SIG + sizeof EC_SSH_SIG);
}

// Every proper prefix of a good blob is truncated, so every parser refuses it.
static void test_truncation(void) {
    for (uint32_t n = 0; n < sizeof RSA_SSH_PUB; n++) CHECK(!rsa_key(RSA_SSH_PUB, n));
    for (uint32_t n = 0; n < sizeof EC_SSH_PUB; n++) CHECK(!ec_key(EC_SSH_PUB, n));
    for (uint32_t n = 0; n < sizeof EC_SSH_SIG; n++) CHECK(!ec_sig(EC_SSH_SIG, n));
    CHECK(rsa_key(RSA_SSH_PUB, sizeof RSA_SSH_PUB));
    CHECK(ec_key(EC_SSH_PUB, sizeof EC_SSH_PUB));
    CHECK(ec_sig(EC_SSH_SIG, sizeof EC_SSH_SIG));
}

// Overwrite the length at `off` in a copy of the blob and parse it.
static bool with_len(bool (*f)(const uint8_t *, uint32_t), const uint8_t *b,
                     uint32_t n, uint32_t off, uint32_t len) {
    uint8_t *c = dup(b, n);
    put32(c + off, len);
    bool ok = f(c, n);
    free(c);
    return ok;
}

static void test_bad_lengths(void) {
    static const uint32_t bad[] = { 0xFFFFFFFFu, 0xFFFFFFF0u, 0xFFFFFFFCu,
                                    0x80000000u, 0x7FFFFFFFu };
    // Offsets of each length prefix: sig r=0, s=36; ec key 0, 23, 35;
    // rsa key 0, 11, 18.
    static const uint32_t sig_off[] = { 0, 36 };
    static const uint32_t ec_off[] = { 0, 23, 35 };
    static const uint32_t rsa_off[] = { 0, 11, 18 };
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        for (unsigned j = 0; j < 2; j++)
            CHECK(!with_len(ec_sig, EC_SSH_SIG, sizeof EC_SSH_SIG, sig_off[j], bad[i]));
        for (unsigned j = 0; j < 3; j++) {
            CHECK(!with_len(ec_key, EC_SSH_PUB, sizeof EC_SSH_PUB, ec_off[j], bad[i]));
            CHECK(!with_len(rsa_key, RSA_SSH_PUB, sizeof RSA_SSH_PUB, rsa_off[j], bad[i]));
        }
    }
    // One past the end: r's length claims a byte more than remains.
    CHECK(!with_len(ec_sig, EC_SSH_SIG, sizeof EC_SSH_SIG, 0,
                    (uint32_t)sizeof EC_SSH_SIG - 4 + 1));
    CHECK(!with_len(ec_sig, EC_SSH_SIG, sizeof EC_SSH_SIG, 36, 33 + 1));
    CHECK(with_len(ec_sig, EC_SSH_SIG, sizeof EC_SSH_SIG, 36, 33));
    // The Q length: exact fit parses, one more does not.
    CHECK(!with_len(ec_key, EC_SSH_PUB, sizeof EC_SSH_PUB, 35, 66));
    CHECK(!with_len(rsa_key, RSA_SSH_PUB, sizeof RSA_SSH_PUB, 18, 130));
}

// The wrap case the old `sp + r_len > send` check missed: on the 32-bit
// target r_len = 0xFFFFFFF0 puts the pointer 12 bytes before the buffer, and
// only 0xFFFFFFFC lands back at blob+0, where a second length read follows.
// The parser works on uint32_t offsets, so these fail the same way on a
// 64-bit host.
static void test_wrap_sig(void) {
    uint8_t blob[16] = {0};
    put32(blob, 0xFFFFFFF0u);
    CHECK(!ec_sig(blob, sizeof blob));
    put32(blob, 0xFFFFFFFCu);
    CHECK(!ec_sig(blob, sizeof blob));
}

// The regressed shape `off + 4 + n > len` wraps in uint32_t for n >= 2^32 - 4
// - off: a wrapped sum is small, so it would accept.  Hit it at every field
// position (offsets 0, 4 + r, ...) of a signature and of the keys.
static void test_wrap_offset(void) {
    static const uint32_t sig_off[] = { 0, 36 };
    static const uint32_t ec_off[] = { 0, 23, 35 };
    static const uint32_t rsa_off[] = { 0, 11, 18 };
    for (unsigned i = 0; i < 3; i++) {
        // 2^32 - 4 - off, +1, +2: the sum off + 4 + n wraps to 0, 1, 2.
        for (unsigned j = 0; j < 2; j++) {
            uint32_t w = 0u - 4u - sig_off[j];
            CHECK(!with_len(ec_sig, EC_SSH_SIG, sizeof EC_SSH_SIG, sig_off[j], w + i));
        }
        for (unsigned j = 0; j < 3; j++) {
            CHECK(!with_len(ec_key, EC_SSH_PUB, sizeof EC_SSH_PUB, ec_off[j],
                            0u - 4u - ec_off[j] + i));
            CHECK(!with_len(rsa_key, RSA_SSH_PUB, sizeof RSA_SSH_PUB, rsa_off[j],
                            0u - 4u - rsa_off[j] + i));
        }
    }
}

// Zero-length mpints parse (the verifier then rejects the zero value).
static void test_zero_length(void) {
    uint8_t sig[8] = {0};                         // r = "", s = ""
    const uint8_t *r, *s; uint32_t rl, sl;
    uint8_t *c = dup(sig, sizeof sig);
    CHECK(ssh_ecdsa_sig_parse(c, sizeof sig, &r, &rl, &s, &sl));
    CHECK_EQ_U32(rl, 0);
    CHECK_EQ_U32(sl, 0);
    free(c);
    CHECK(!ec_sig(sig, 7));
    CHECK(!ec_sig(sig, 4));
    CHECK(!ec_sig(sig, 0));

    uint8_t rsa[4 + 7 + 4 + 4] = {0};             // type "ssh-rsa", e = "", n = ""
    put32(rsa, 7);
    memcpy(rsa + 4, "ssh-rsa", 7);
    CHECK(rsa_key(rsa, sizeof rsa));
    CHECK(!rsa_key(rsa, sizeof rsa - 1));
}

int main(void) {
    test_good_vectors();
    test_truncation();
    test_bad_lengths();
    test_wrap_sig();
    test_wrap_offset();
    test_zero_length();
    return check_report("test_ssh_blob");
}
