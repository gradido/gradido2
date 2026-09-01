/*
 * What sc_jwt_sign_hs256 and sc_jwt_verify_hs256 cost, and where the time goes.
 *
 * Built by the same build as the server and linked against the same libsodium, which is the
 * point: the answer depends on which SHA-256 is underneath, and the one in the cache is not
 * necessarily the one on the system.
 *
 * The breakdown does not call jwt.c's static helpers -- they are not exported -- but repeats
 * the same work with the same library calls over the same sizes. It says where the time goes,
 * not what jwt.c does to the byte.
 *
 * Numbers move with the machine. Report the CPU beside them or they mean nothing.
 */
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <sodium.h>

#include "arnm/arena.h"
#include "arnm/json_reader.h"
#include "arnm/json_writer.h"
#include "service_core/jwt.h"

#define ROUNDS 200000

static double now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}

static void report(const char *label, double elapsed_ns, long rounds)
{
    const double per = elapsed_ns / (double)rounds;
    printf("  %-34s %8.0f ns   %10.0f /s\n", label, per, 1e9 / per);
}

static const uint8_t SECRET[] = "a-secret-of-arbitrary-length";
static const char CLAIM_VALUE[] = "8b9a1e2c-0000-4000-8000-000000000001";

int main(void)
{
    sc_jwt_config cfg = {SECRET, sizeof(SECRET) - 1, "gradido", "gradido-backend"};
    char token[SC_JWT_MAX_TOKEN];
    char claim[SC_JWT_MAX_CLAIM];
    const int64_t now = 1787572000;
    double t0;
    long i;
    uint64_t sink = 0;
    int token_len;

    if (sodium_init() < 0)
        return 1;
    sc_jwt_init();

    token_len = sc_jwt_sign_hs256(&cfg, "gradidoID", CLAIM_VALUE, now, 600, token, sizeof(token));
    if (token_len <= 0)
        return 1;
    printf("token: %d bytes, %d rounds each\n\n", token_len, ROUNDS);

    /* warm up */
    for (i = 0; i < 5000; ++i) {
        char scratch[SC_JWT_MAX_TOKEN];
        sink += (uint64_t)sc_jwt_sign_hs256(&cfg, "gradidoID", CLAIM_VALUE, now, 600, scratch,
                                            sizeof(scratch));
        sink +=
            (uint64_t)sc_jwt_verify_hs256(&cfg, token, (size_t)token_len, "gradidoID", claim, now);
    }

    puts("whole calls:");
    t0 = now_ns();
    for (i = 0; i < ROUNDS; ++i) {
        char scratch[SC_JWT_MAX_TOKEN];
        sink += (uint64_t)sc_jwt_sign_hs256(&cfg, "gradidoID", CLAIM_VALUE, now, 600, scratch,
                                            sizeof(scratch));
    }
    report("sc_jwt_sign_hs256", now_ns() - t0, ROUNDS);

    t0 = now_ns();
    for (i = 0; i < ROUNDS; ++i) {
        sink +=
            (uint64_t)sc_jwt_verify_hs256(&cfg, token, (size_t)token_len, "gradidoID", claim, now);
    }
    report("sc_jwt_verify_hs256", now_ns() - t0, ROUNDS);

    /* ---- where the time goes ---------------------------------------------------------- */
    {
        const char *dot2 = strrchr(token, '.');
        const size_t signed_len = (size_t)(dot2 - token);
        const char *payload_b64 = strchr(token, '.') + 1;
        const size_t payload_b64_len = (size_t)(dot2 - payload_b64);
        uint8_t digest[32];
        char payload[1024 + ARNM_JSON_READER_INSITU_PADDING];
        size_t payload_len = 0;

        sodium_base642bin((uint8_t *)payload, sizeof(payload), payload_b64, payload_b64_len, NULL,
                          &payload_len, NULL, sodium_base64_VARIANT_URLSAFE_NO_PADDING);

        puts("\nwhere the time goes:");

        t0 = now_ns();
        for (i = 0; i < ROUNDS; ++i) {
            crypto_auth_hmacsha256_state st;
            crypto_auth_hmacsha256_init(&st, SECRET, sizeof(SECRET) - 1);
            crypto_auth_hmacsha256_update(&st, (const uint8_t *)token, signed_len);
            crypto_auth_hmacsha256_final(&st, digest);
            sink += digest[0];
        }
        report("HMAC-SHA256 over the token", now_ns() - t0, ROUNDS);

        t0 = now_ns();
        for (i = 0; i < ROUNDS; ++i) {
            uint8_t out[32];
            size_t n = 0;
            sodium_base642bin(out, sizeof(out), dot2 + 1, strlen(dot2 + 1), NULL, &n, NULL,
                              sodium_base64_VARIANT_URLSAFE_NO_PADDING);
            sink += n;
        }
        report("base64url, the signature (32 B)", now_ns() - t0, ROUNDS);

        t0 = now_ns();
        for (i = 0; i < ROUNDS; ++i) {
            char buf[1024 + ARNM_JSON_READER_INSITU_PADDING];
            _Alignas(8) uint8_t scratch[8192];
            arnm alloc = {0};
            arnm_json_reader reader;
            arnm_json_value *root = NULL;
            arnm_memory_block gradido_id = {0};
            int64_t exp = 0;
            /* The same two fields jwt.c reads, in one walk -- which is the only way arnm 0.7.5
             * reads them, so the row measures what the verifier really pays. */
            arnm_json_field fields[] = {ARNM_JSON_FIELD_STRING("gradidoID", &gradido_id),
                                        ARNM_JSON_FIELD_INT64("exp", &exp)};
            memcpy(buf, payload, payload_len);
            arnm_init_arena_borrow(&alloc, scratch, sizeof(scratch));
            arnm_json_reader_init(&reader, &alloc);
            arnm_json_reader_parse_insitu(&reader, buf, (uint32_t)payload_len, sizeof(buf), false,
                                          &root);
            arnm_json_read_object(root, fields, 2, NULL);
            sink += (uint64_t)exp;
            sink += (uintptr_t)gradido_id.data;
            arnm_json_reader_release(&reader);
        }
        report("arnm json: parse + 2 fields", now_ns() - t0, ROUNDS);

        t0 = now_ns();
        for (i = 0; i < ROUNDS; ++i) {
            _Alignas(8) uint8_t scratch[8192];
            arnm alloc = {0};
            arnm_json_writer writer;
            arnm_memory_block text;
            uint32_t len = 0;
            arnm_init_arena_borrow(&alloc, scratch, sizeof(scratch));
            arnm_json_writer_init(&writer, &alloc, ARNM_JSON_WRITE_DEFAULT, NULL);
            arnm_json_writer_add_string(&writer, "gradidoID", CLAIM_VALUE);
            arnm_json_writer_add_bool(&writer, "urn:gradido:claim", true);
            arnm_json_writer_add_int64(&writer, "iat", now);
            arnm_json_writer_add_string(&writer, "iss", "gradido");
            arnm_json_writer_add_string(&writer, "aud", "gradido-backend");
            arnm_json_writer_add_int64(&writer, "exp", now + 600);
            arnm_json_writer_write(&writer, &alloc, &text, &len);
            sink += len;
        }
        report("arnm json: write 6 fields", now_ns() - t0, ROUNDS);
    }

    printf("\n(sink %" PRIu64 ")\n", sink);
    return 0;
}
