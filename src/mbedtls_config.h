#ifndef MBEDTLS_CONFIG_H
#define MBEDTLS_CONFIG_H

#include <stddef.h>
extern void *umm_malloc(size_t size);
extern void *umm_calloc(size_t num, size_t size);
extern void umm_free(void *ptr);

// System support
#define MBEDTLS_PLATFORM_C
#define MBEDTLS_PLATFORM_MEMORY
#define MBEDTLS_PLATFORM_FREE_MACRO        umm_free
#define MBEDTLS_PLATFORM_MALLOC_MACRO      umm_malloc
#define MBEDTLS_PLATFORM_CALLOC_MACRO      umm_calloc
#define MBEDTLS_PLATFORM_EXIT_ALT
#define MBEDTLS_NO_PLATFORM_ENTROPY        // disable /dev/urandom etc.
// No MBEDTLS_ENTROPY_HARDWARE_ALT: pico_mbedtls's mbedtls_hardware_poll is
// get_rand_64 (xoroshiro).  src/drivers/rng.c adds the RP2350 TRNG (health
// tests on) as the entropy source of every DRBG it seeds.
#define MBEDTLS_ENTROPY_C
#define MBEDTLS_CTR_DRBG_C

// ── THE HACK ────────────────────────────────────────────────────────────────
#define MBEDTLS_ALLOW_PRIVATE_ACCESS
// ────────────────────────────────────────────────────────────────────────────

// TLS features
#define MBEDTLS_SSL_TLS_C
#define MBEDTLS_SSL_CLI_C
#define MBEDTLS_SSL_PROTO_TLS1_2
#define MBEDTLS_SSL_KEEP_PEER_CERTIFICATE

// Key exchange
#define MBEDTLS_KEY_EXCHANGE_ECDHE_RSA_ENABLED
#define MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA_ENABLED
#define MBEDTLS_KEY_EXCHANGE_RSA_ENABLED

// Cipher suites & algorithms
#define MBEDTLS_ECP_C
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED
#define MBEDTLS_ECP_DP_SECP384R1_ENABLED   // P-384: used by Let's Encrypt E5/E6 and Wikipedia CDN
#define MBEDTLS_ECP_DP_CURVE25519_ENABLED  // X25519 for SSH curve25519-sha256 kex
#define MBEDTLS_ECDH_C
#define MBEDTLS_ECDSA_C
#define MBEDTLS_X509_CRT_PARSE_C
#define MBEDTLS_X509_USE_C
#define MBEDTLS_ASN1_PARSE_C               // Required by X509_USE_C for certificate parsing
#define MBEDTLS_ASN1_WRITE_C               // Required for ECDSA signature encoding
#define MBEDTLS_OID_C
#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C
#define MBEDTLS_PEM_PARSE_C                // PEM CA bundle + OTA update public key
#define MBEDTLS_BASE64_C                   // required by PEM_PARSE_C
#define MBEDTLS_SHA224_C                   // SHA-224 (often needed alongside SHA-256)
#define MBEDTLS_SHA256_C
#define MBEDTLS_SHA384_C                   // Required for P-384 curve (Wikipedia)
#define MBEDTLS_SHA512_C
#define MBEDTLS_SHA1_C
#define MBEDTLS_MD5_C
#define MBEDTLS_AES_C
// One forward and one reverse round table (2 KB of SRAM) instead of four
// each (8 KB): the other three are rotations of the first, computed per
// lookup (a rotate folded into the EOR on Cortex-M33). The 6 KB went to
// the MP3 decoder's hot state (issue #28, drivers/mp3_player.c).
#define MBEDTLS_AES_FEWER_TABLES
#define MBEDTLS_GCM_C
#define MBEDTLS_CIPHER_C
#define MBEDTLS_CIPHER_MODE_CBC
#define MBEDTLS_CIPHER_MODE_CTR            // AES-CTR for SSH transport
#define MBEDTLS_MD_C
#define MBEDTLS_ERROR_C

// RSA / PKCS1
#define MBEDTLS_RSA_C
#define MBEDTLS_PKCS1_V15
#define MBEDTLS_BIGNUM_C

// TLS SNI — sends server_name extension in ClientHello (required for shared-IP HTTPS hosts)
#define MBEDTLS_SSL_SERVER_NAME_INDICATION

// Time support.  HAVE_TIME_DATE makes X.509 verification check validity
// dates, against time() — overridden in wifi.c to return the SNTP clock.
// wifi.c refuses verifying handshakes until SNTP has set it.
#define MBEDTLS_HAVE_TIME
#define MBEDTLS_HAVE_TIME_DATE
#define MBEDTLS_PLATFORM_MS_TIME_ALT

// Memory tuning
#define MBEDTLS_SSL_IN_CONTENT_LEN  16384
#define MBEDTLS_SSL_OUT_CONTENT_LEN 8192
#define MBEDTLS_SSL_MAX_CONTENT_LEN 16384

#endif /* MBEDTLS_CONFIG_H */
