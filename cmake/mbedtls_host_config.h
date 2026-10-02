// mbedTLS configuration for HOST builds (simulator and tests/unit): what
// src/os/ota_verify.c needs (PEM public-key parsing, ECDSA P-256 verify,
// SHA-256) plus X.509 chain verification for tests/unit/test_ca_bundle.c, and what
// src/os/crypto.c needs (AES-CTR, ECDH, RSA) for tests/unit/test_crypto_verify.c.
// No MBEDTLS_HAVE_TIME_DATE: the chain test checks trust paths and names,
// not dates (so its captured fixtures never expire).  The firmware uses
// src/mbedtls_config.h instead.
#ifndef PICODECK_MBEDTLS_HOST_CONFIG_H
#define PICODECK_MBEDTLS_HOST_CONFIG_H

#define MBEDTLS_BIGNUM_C
#define MBEDTLS_ECP_C
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED
#define MBEDTLS_ECDSA_C
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C
#define MBEDTLS_OID_C
#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C
#define MBEDTLS_PEM_PARSE_C
#define MBEDTLS_BASE64_C
#define MBEDTLS_MD_C
#define MBEDTLS_SHA256_C
#define MBEDTLS_SHA224_C
#define MBEDTLS_SHA384_C
#define MBEDTLS_SHA512_C
#define MBEDTLS_SHA1_C
#define MBEDTLS_ECP_DP_SECP384R1_ENABLED
#define MBEDTLS_RSA_C
#define MBEDTLS_PKCS1_V15
#define MBEDTLS_PKCS1_V21
#define MBEDTLS_AES_C
#define MBEDTLS_CIPHER_MODE_CTR
#define MBEDTLS_ECDH_C
#define MBEDTLS_ECP_DP_CURVE25519_ENABLED
#define MBEDTLS_X509_USE_C
#define MBEDTLS_X509_CRT_PARSE_C

#endif
