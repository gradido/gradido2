/*
 * MBEDTLS_USER_CONFIG_FILE: applied on top of mbedtls' own defaults, so this
 * file only takes things away. What has to stay is decided by exactly one
 * consumer, curl's lib/vtls/mbedtls.c, which uses
 *
 *   mbedtls_ssl_*   the client handshake and record layer
 *   mbedtls_x509_*  parsing and verifying the relay's chain
 *   mbedtls_pk_*    and PK_WRITE_C, for CURLOPT_PINNEDPUBLICKEY
 *   mbedtls_ctr_drbg_* / mbedtls_entropy_*
 *   mbedtls_sha256 + base64, also for pinning
 *   mbedtls_strerror (ERROR_C), FS_IO for CAINFO, PEM_PARSE_C for a PEM bundle
 *
 * Everything below is outside that list. This is an SMTP submission client:
 * it dials out, it verifies a certificate, and it is never a server.
 *
 * IMPORTANT: the same macro has to be defined when *curl* is compiled, not just
 * mbedtls. curl includes mbedtls headers, and mbedtls_ssl_context changes size
 * with this file -- two different views of that struct in one process is not a
 * link error, it is a crash later. build.zig sets it on both artifacts.
 */
#ifndef GRADIDO_MBEDTLS_CONFIG_H
#define GRADIDO_MBEDTLS_CONFIG_H

/* ---- server side -------------------------------------------------------- */
#undef MBEDTLS_SSL_SRV_C
#undef MBEDTLS_SSL_CACHE_C
#undef MBEDTLS_SSL_TICKET_C
#undef MBEDTLS_SSL_COOKIE_C

/* ---- DTLS: SMTP is TCP -------------------------------------------------- */
#undef MBEDTLS_SSL_PROTO_DTLS
#undef MBEDTLS_SSL_DTLS_ANTI_REPLAY
#undef MBEDTLS_SSL_DTLS_HELLO_VERIFY
#undef MBEDTLS_SSL_DTLS_SRTP
#undef MBEDTLS_SSL_DTLS_CLIENT_PORT_REUSE
#undef MBEDTLS_SSL_DTLS_CONNECTION_ID

/* ---- ciphers no public relay will negotiate ------------------------------ */
#undef MBEDTLS_CAMELLIA_C
#undef MBEDTLS_ARIA_C
#undef MBEDTLS_DES_C
#undef MBEDTLS_CCM_C
#undef MBEDTLS_NIST_KW_C
#undef MBEDTLS_CMAC_C
#undef MBEDTLS_RIPEMD160_C
#undef MBEDTLS_SHA3_C
#undef MBEDTLS_SHA512_USE_A64_CRYPTO_IF_PRESENT

/* ---- key exchanges we do not offer -------------------------------------- */
/* Finite-field DH: every relay worth dialling does ECDHE. */
#undef MBEDTLS_DHM_C
#undef MBEDTLS_KEY_EXCHANGE_DHE_RSA_ENABLED
#undef MBEDTLS_KEY_EXCHANGE_DHE_PSK_ENABLED
#undef MBEDTLS_KEY_EXCHANGE_RSA_PSK_ENABLED
#undef MBEDTLS_KEY_EXCHANGE_PSK_ENABLED
#undef MBEDTLS_KEY_EXCHANGE_ECDHE_PSK_ENABLED
#undef MBEDTLS_KEY_EXCHANGE_ECDH_RSA_ENABLED
#undef MBEDTLS_KEY_EXCHANGE_ECDH_ECDSA_ENABLED
#undef MBEDTLS_ECJPAKE_C
#undef MBEDTLS_KEY_EXCHANGE_ECJPAKE_ENABLED

/* ---- curves: P-256, P-384 and X25519 are what the public web uses -------- */
#undef MBEDTLS_ECP_DP_SECP192R1_ENABLED
#undef MBEDTLS_ECP_DP_SECP224R1_ENABLED
#undef MBEDTLS_ECP_DP_SECP192K1_ENABLED
#undef MBEDTLS_ECP_DP_SECP224K1_ENABLED
#undef MBEDTLS_ECP_DP_SECP256K1_ENABLED
#undef MBEDTLS_ECP_DP_BP256R1_ENABLED
#undef MBEDTLS_ECP_DP_BP384R1_ENABLED
#undef MBEDTLS_ECP_DP_BP512R1_ENABLED
#undef MBEDTLS_ECP_DP_CURVE448_ENABLED

/* ---- X.509 we never write, and CRLs we never fetch ---------------------- */
#undef MBEDTLS_X509_CRL_PARSE_C
#undef MBEDTLS_X509_CSR_PARSE_C
#undef MBEDTLS_X509_CREATE_C
#undef MBEDTLS_X509_CRT_WRITE_C
#undef MBEDTLS_X509_CSR_WRITE_C

/* ---- container formats for private keys we never load ------------------- */
#undef MBEDTLS_PKCS7_C
#undef MBEDTLS_PKCS12_C
#undef MBEDTLS_PEM_WRITE_C

/* ---- post-quantum hash signatures, not used by TLS ---------------------- */
#undef MBEDTLS_LMS_C
#undef MBEDTLS_LMS_PRIVATE

/* ---- infrastructure curl brings itself ---------------------------------- */
#undef MBEDTLS_NET_C              /* curl owns the socket */
#undef MBEDTLS_TIMING_C           /* only DTLS needs mbedtls' timer */
#undef MBEDTLS_MEMORY_BUFFER_ALLOC_C
#undef MBEDTLS_SELF_TEST
#undef MBEDTLS_VERSION_FEATURES
#undef MBEDTLS_DEBUG_C            /* curl compiles its trace hook out with it */
#undef MBEDTLS_SSL_RENEGOTIATION  /* refused by everything modern anyway */

/* ---- PSA persistence: there is no key store here ------------------------ */
#undef MBEDTLS_PSA_CRYPTO_STORAGE_C
#undef MBEDTLS_PSA_ITS_FILE_C
#undef MBEDTLS_PSA_CRYPTO_SE_C

/* ---- optional: TLS 1.2 only --------------------------------------------
 * mbedtls 3.6 builds TLS 1.3 on PSA crypto, so dropping 1.3 drops that whole
 * layer with it. Every relay in service today speaks 1.2; this is a size/
 * future-proofing trade, hence a switch rather than a decision. */
#if defined(GE_TLS12_ONLY)
#undef MBEDTLS_SSL_PROTO_TLS1_3
#undef MBEDTLS_SSL_TLS1_3_COMPATIBILITY_MODE
#undef MBEDTLS_SSL_EARLY_DATA
#undef MBEDTLS_SSL_SESSION_TICKETS
#endif

#endif /* GRADIDO_MBEDTLS_CONFIG_H */
