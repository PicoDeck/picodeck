// mbedTLS for tests/unit/test_mg_tls_resend.c: the firmware's own configuration,
// so its record and buffer sizes, plus the server side, which the firmware has no
// use for and the test's peer needs. umm_* are the test's counting wrappers.
#ifndef PICODECK_MBEDTLS_TLS_TEST_CONFIG_H
#define PICODECK_MBEDTLS_TLS_TEST_CONFIG_H
#include "../../../src/mbedtls_config.h"
#define MBEDTLS_SSL_SRV_C
#endif
