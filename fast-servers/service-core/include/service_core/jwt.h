/*
 * Just enough JWT to verify what gradido's backend issues: HS256, a fixed issuer and audience,
 * one claim of interest. Not a general purpose library -- it accepts exactly one algorithm and
 * says no to everything else, which for a verifier is a feature rather than a limitation.
 *
 * libsodium underneath, and only libsodium. There is no second backend and no macro selecting
 * one: gradido-blockchain-core is a dependency of this build already and brings libsodium with
 * it, so a choice here would be a choice between the library that is present and one that would
 * have to be added. ../h20Test/src/jwt.c is where this came from and still carries the OpenSSL
 * half; that is the version for a project that does not link the core.
 *
 * Everything after the signature check reads attacker-supplied bytes. AGENTS.md section 4
 * requires this parser to be fuzzed before it verifies a token that came from outside.
 */
#ifndef SERVICE_CORE_JWT_H
#define SERVICE_CORE_JWT_H

#include <stddef.h>
#include <stdint.h>

/** Longest claim value copied out, terminator included. users.gradido_id is a uuid. */
#define SC_JWT_MAX_CLAIM 128
/** Longest token sc_jwt_sign_hs256 will write, terminator included. */
#define SC_JWT_MAX_TOKEN 512

typedef enum sc_jwt_result {
    SC_JWT_OK = 0,
    SC_JWT_MALFORMED,     /* not three base64url segments */
    SC_JWT_BAD_ALGORITHM, /* the header does not say HS256 */
    SC_JWT_BAD_SIGNATURE,
    SC_JWT_EXPIRED,
    SC_JWT_BAD_ISSUER,
    SC_JWT_BAD_AUDIENCE,
    SC_JWT_MISSING_CLAIM
} sc_jwt_result;

/*
 * These distinguish what went wrong so a log line can say it. An HTTP response must not: every
 * one of them is 401 and the same body, because telling a caller which half failed tells an
 * attacker the same thing.
 */

typedef struct sc_jwt_config {
    const uint8_t *secret;
    size_t secret_len;
    /* Optional. NULL skips the check rather than accepting an absent claim. */
    const char *issuer;
    const char *audience;
} sc_jwt_config;

/**
 * Initialises libsodium. Call once before the first verify or sign, from one thread.
 *
 * Aborts if libsodium cannot initialise itself: a server that cannot check signatures has no
 * business accepting the request that follows.
 */
void sc_jwt_init(void);

/**
 * Verifies the signature, the expiry, the issuer and the audience -- in that order -- and then
 * copies the value of @p claim into @p out as a NUL-terminated string.
 *
 * @param out  buffer of at least SC_JWT_MAX_CLAIM bytes
 * @param now  unix seconds, passed in so a caller can reuse a timestamp it already has rather
 *             than ask the clock once per token
 * @return SC_JWT_OK, or the first thing found wrong
 */
sc_jwt_result sc_jwt_verify_hs256(const sc_jwt_config *config, const char *token, size_t token_len,
                                  const char *claim, char *out, int64_t now);

/**
 * Issues a fresh token for @p claim, the way gradido's backend does on every authorized
 * request: same algorithm, same issuer and audience, a new `iat` and `exp`. The extra
 * `urn:gradido:claim` member is there because the reference implementation puts it there, and a
 * signature is only comparable over comparable bytes.
 *
 * @param out  buffer of at least SC_JWT_MAX_TOKEN bytes, NUL-terminated on success
 * @param ttl  seconds until the new token expires
 * @return length written, or -1 if it did not fit
 */
int sc_jwt_sign_hs256(const sc_jwt_config *config, const char *claim, const char *value,
                      int64_t now, int64_t ttl, char *out, size_t out_size);

#endif /* SERVICE_CORE_JWT_H */
