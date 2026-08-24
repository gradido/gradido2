/*
 * HS256 over libsodium, which also does the base64url, with yyjson reading the two segments.
 *
 * Ported from ../h20Test/src/jwt.c, minus its OpenSSL half. See jwt.h for why there is only one
 * crypto backend here.
 *
 * yyjson comes from arnm, which has carried it since 0.7.2 and compiles it into libarnm, which
 * gradido-blockchain-core links. That is deliberate rather than convenient: a second, separately
 * pinned yyjson in this build would put two definitions of every yyjson_* symbol in front of the
 * linker, and the one that wins would depend on link order. This build did pin one, correctly,
 * while the core was at 0.16.0 and carried no parser of its own.
 */
#include "service_core/jwt.h"

#include <stdlib.h>
#include <string.h>

#include <sodium.h>
#include <yyjson.h>

/** Longest base64url segment decoded. A JWT that needs more than this is not one of ours. */
#define MAX_SEGMENT 1024
/** Enough for a document of this size; yyjson never touches malloc here. */
#define JSON_ARENA 8192
/** HS256 and nothing else, so the digest has exactly one size. */
#define JWT_HMAC_LEN 32

void sc_jwt_init(void)
{
    /* Safe to call more than once, which is convenient for a library that does not know who its
     * callers are. A negative return means libsodium could not initialise itself. */
    if (sodium_init() < 0)
        abort();
}

/** @return 0 on success */
static int hmac_sha256(const sc_jwt_config *config, const uint8_t *msg, size_t msg_len,
                       uint8_t out[JWT_HMAC_LEN])
{
    crypto_auth_hmacsha256_state state;

    /* The init/update/final form takes a key of any length; the one-shot
     * crypto_auth_hmacsha256() insists on exactly 32 bytes, and a JWT secret is whatever the
     * operator typed into the environment. */
    if (crypto_auth_hmacsha256_init(&state, config->secret, config->secret_len) != 0)
        return -1;
    crypto_auth_hmacsha256_update(&state, msg, msg_len);
    crypto_auth_hmacsha256_final(&state, out);
    return 0;
}

/** @return non-zero if the two digests are equal, in constant time */
static int hmac_equal(const uint8_t *a, const uint8_t *b)
{
    return sodium_memcmp(a, b, JWT_HMAC_LEN) == 0;
}

/*
 * Base64url, both directions, over libsodium.
 *
 * Not grdu_binary_to_base64() / grdu_binary_from_base64(), although they sit in
 * gradido-blockchain-core and would otherwise be the obvious call: those are pinned to
 * sodium_base64_VARIANT_ORIGINAL, and a JWT is base64url without padding. The two alphabets
 * differ in two characters out of sixty-four and in whether the tail is padded, which is enough
 * that they cannot stand in for each other:
 *
 *   VARIANT_ORIGINAL              +  /  and a trailing '=' or '=='
 *   VARIANT_URLSAFE_NO_PADDING    -  _  and nothing after the last character
 *
 * Decoding a real signature segment with the original variant fails outright -- sodium refuses
 * the '-' -- and it fails only for the tokens that happen to contain one of the two characters,
 * which is most but not all of them. That is the shape of a bug that passes a test and then
 * rejects a share of the users. Encoding with it would produce a token no JWT library accepts.
 *
 * What is left of the hand-written pair these replaced is nothing: sodium does the conversion,
 * the same way the grdu_ wrappers do, with the constant this format asks for. If the core ever
 * grows a url-safe variant, this becomes a call to it.
 */

/**
 * Decodes @p in_len characters of base64url into @p out.
 *
 * JWT segments carry no padding, and the whole input must be base64url -- there is no ignore
 * set and no early stop, so a stray character is a refusal rather than a short read.
 *
 * @return number of bytes written, or -1 if the input is not base64url or does not fit
 */
static int base64url_decode(const char *in, size_t in_len, uint8_t *out, size_t out_size)
{
    size_t written = 0;

    if (sodium_base642bin(out, out_size, in, in_len, NULL, &written, NULL,
                          sodium_base64_VARIANT_URLSAFE_NO_PADDING) != 0)
        return -1;
    return (int)written;
}

/**
 * Encodes @p in_len bytes as base64url without padding, the way a JWT segment wants them.
 *
 * The room is checked before the call and not after: sodium_bin2base64 answers a destination
 * that is too small by calling sodium_misuse(), which aborts the process. The core's header
 * documents the same trap for the same reason.
 *
 * @return characters written, terminator excluded, or -1 if the buffer is too small
 */
static int base64url_encode(const uint8_t *in, size_t in_len, char *out, size_t out_size)
{
    /* sodium_base64_encoded_len counts the terminator sodium writes; the caller wants the
     * length of the segment, which is one less. */
    const size_t needed =
        sodium_base64_encoded_len(in_len, sodium_base64_VARIANT_URLSAFE_NO_PADDING);

    if (out_size < needed)
        return -1;
    sodium_bin2base64(out, out_size, in, in_len, sodium_base64_VARIANT_URLSAFE_NO_PADDING);
    return (int)(needed - 1);
}

int sc_jwt_sign_hs256(const sc_jwt_config *config, const char *claim, const char *value,
                      int64_t now, int64_t ttl, char *out, size_t out_size)
{
    /* The header is the same twenty bytes every time, so it is spelled out rather than built:
     * {"alg":"HS256"} */
    static const char HEADER_B64[] = "eyJhbGciOiJIUzI1NiJ9";
    char arena[JSON_ARENA];
    yyjson_alc alc;
    yyjson_mut_doc *doc;
    yyjson_mut_val *root;
    char payload[MAX_SEGMENT];
    uint8_t signature[JWT_HMAC_LEN];
    size_t payload_len = 0, written = 0;
    char *payload_json;
    int n;

    yyjson_alc_pool_init(&alc, arena, sizeof(arena));
    doc = yyjson_mut_doc_new(&alc);
    root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    /* Member order follows what the reference implementation produces, so two tokens differ in
     * their timestamps and nothing else. */
    yyjson_mut_obj_add_str(doc, root, claim, value);
    yyjson_mut_obj_add_bool(doc, root, "urn:gradido:claim", true);
    yyjson_mut_obj_add_sint(doc, root, "iat", now);
    if (config->issuer != NULL)
        yyjson_mut_obj_add_str(doc, root, "iss", config->issuer);
    if (config->audience != NULL)
        yyjson_mut_obj_add_str(doc, root, "aud", config->audience);
    yyjson_mut_obj_add_sint(doc, root, "exp", now + ttl);

    payload_json = yyjson_mut_write_opts(doc, 0, &alc, &payload_len, NULL);
    if (payload_json == NULL || payload_len > sizeof(payload))
        return -1;
    memcpy(payload, payload_json, payload_len);

    if (sizeof(HEADER_B64) - 1 + 1 > out_size)
        return -1;
    memcpy(out, HEADER_B64, sizeof(HEADER_B64) - 1);
    written = sizeof(HEADER_B64) - 1;
    out[written++] = '.';

    if ((n = base64url_encode((const uint8_t *)payload, payload_len, out + written,
                              out_size - written)) < 0)
        return -1;
    written += (size_t)n;
    if (written + 1 > out_size)
        return -1;
    out[written++] = '.';

    if (hmac_sha256(config, (const uint8_t *)out, written - 1, signature) != 0)
        return -1;
    if ((n = base64url_encode(signature, sizeof(signature), out + written, out_size - written)) < 0)
        return -1;
    written += (size_t)n;

    if (written + 1 > out_size)
        return -1;
    out[written] = '\0';
    return (int)written;
}

/**
 * Parses a decoded segment in place. Insitu spares the copy: the buffer is ours, it is padded,
 * and nobody looks at it again afterwards.
 *
 * @return the root object, or NULL if the segment is not one
 */
static yyjson_val *parse_segment(char *json, size_t json_len, yyjson_alc *alc, yyjson_doc **doc)
{
    yyjson_val *root;

    *doc = yyjson_read_opts(json, json_len, YYJSON_READ_INSITU, alc, NULL);
    if (*doc == NULL)
        return NULL;

    root = yyjson_doc_get_root(*doc);
    return yyjson_is_obj(root) ? root : NULL;
}

/** @return non-zero if the member is the expected string */
static int member_is(yyjson_val *obj, const char *key, const char *expected)
{
    const char *value = yyjson_get_str(yyjson_obj_get(obj, key));
    return value != NULL && strcmp(value, expected) == 0;
}

/**
 * The audience may be one string or a list of them -- the reference implementation writes the
 * first, RFC 7519 allows the second.
 *
 * @return non-zero if @p expected is among them
 */
static int audience_matches(yyjson_val *payload, const char *expected)
{
    yyjson_val *aud = yyjson_obj_get(payload, "aud");
    const char *value;

    if (yyjson_is_arr(aud)) {
        yyjson_val *item;
        yyjson_arr_iter iter;
        yyjson_arr_iter_init(aud, &iter);
        while ((item = yyjson_arr_iter_next(&iter)) != NULL) {
            value = yyjson_get_str(item);
            if (value != NULL && strcmp(value, expected) == 0)
                return 1;
        }
        return 0;
    }

    value = yyjson_get_str(aud);
    return value != NULL && strcmp(value, expected) == 0;
}

sc_jwt_result sc_jwt_verify_hs256(const sc_jwt_config *config, const char *token, size_t token_len,
                                  const char *claim, char *out, int64_t now)
{
    const char *dot1;
    const char *dot2;
    const char *sig_b64;
    size_t signed_len, sig_len, claim_len;
    uint8_t expected[JWT_HMAC_LEN];
    uint8_t actual[JWT_HMAC_LEN];
    int actual_len;
    char arena[JSON_ARENA];
    char header[MAX_SEGMENT + YYJSON_PADDING_SIZE];
    char payload[MAX_SEGMENT + YYJSON_PADDING_SIZE];
    int header_len, payload_len;
    yyjson_alc alc;
    yyjson_doc *doc;
    yyjson_val *root;
    yyjson_val *exp;
    yyjson_val *value;
    const char *claim_str;

    if (config == NULL || token == NULL || claim == NULL || out == NULL)
        return SC_JWT_MALFORMED;

    dot1 = memchr(token, '.', token_len);
    if (dot1 == NULL)
        return SC_JWT_MALFORMED;
    dot2 = memchr(dot1 + 1, '.', token_len - (size_t)(dot1 + 1 - token));
    if (dot2 == NULL)
        return SC_JWT_MALFORMED;

    signed_len = (size_t)(dot2 - token);
    sig_b64 = dot2 + 1;
    sig_len = token_len - signed_len - 1;

    /* Signature first: everything after it reads attacker-supplied bytes, and there is no
     * reason to look at those before they have been vouched for. */
    if (hmac_sha256(config, (const uint8_t *)token, signed_len, expected) != 0)
        return SC_JWT_BAD_SIGNATURE;

    /* A signature that does not fit is a signature that cannot match, so the decoder's refusal
     * to overflow doubles as the length check. */
    actual_len = base64url_decode(sig_b64, sig_len, actual, sizeof(actual));
    if (actual_len != JWT_HMAC_LEN || !hmac_equal(actual, expected))
        return SC_JWT_BAD_SIGNATURE;

    /* Both segments are parsed in place, so they carry the padding yyjson wants at the end. The
     * arena lives on this stack frame and is gone when the function returns, which is also why
     * no document is ever freed here. */
    header_len = base64url_decode(token, (size_t)(dot1 - token), (uint8_t *)header, MAX_SEGMENT);
    if (header_len < 0)
        return SC_JWT_MALFORMED;

    yyjson_alc_pool_init(&alc, arena, sizeof(arena));
    if ((root = parse_segment(header, (size_t)header_len, &alc, &doc)) == NULL)
        return SC_JWT_MALFORMED;
    if (!member_is(root, "alg", "HS256"))
        return SC_JWT_BAD_ALGORITHM;

    payload_len =
        base64url_decode(dot1 + 1, (size_t)(dot2 - dot1 - 1), (uint8_t *)payload, MAX_SEGMENT);
    if (payload_len < 0)
        return SC_JWT_MALFORMED;

    /* The header is done with; the arena starts over rather than grow. */
    yyjson_alc_pool_init(&alc, arena, sizeof(arena));
    if ((root = parse_segment(payload, (size_t)payload_len, &alc, &doc)) == NULL)
        return SC_JWT_MALFORMED;

    exp = yyjson_obj_get(root, "exp");
    if (yyjson_is_num(exp) && (int64_t)yyjson_get_num(exp) <= now)
        return SC_JWT_EXPIRED;

    if (config->issuer != NULL && !member_is(root, "iss", config->issuer))
        return SC_JWT_BAD_ISSUER;
    if (config->audience != NULL && !audience_matches(root, config->audience))
        return SC_JWT_BAD_AUDIENCE;

    value = yyjson_obj_get(root, claim);
    claim_str = yyjson_get_str(value);
    if (claim_str == NULL)
        return SC_JWT_MISSING_CLAIM;

    claim_len = yyjson_get_len(value);
    if (claim_len == 0 || claim_len >= SC_JWT_MAX_CLAIM)
        return SC_JWT_MISSING_CLAIM;
    memcpy(out, claim_str, claim_len);
    out[claim_len] = '\0';

    return SC_JWT_OK;
}
