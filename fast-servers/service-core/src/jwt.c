/*
 * HS256 over libsodium, which also does the base64url, with arnm reading and writing the JSON.
 *
 * Ported from ../h20Test/src/jwt.c, minus its OpenSSL half. See jwt.h for why there is only one
 * crypto backend here.
 *
 * The JSON goes through arnm/json_reader.h and arnm/json_writer.h rather than through yyjson
 * directly. yyjson is still what runs underneath, but nothing of it reaches those headers -- no
 * type, no constant, no include path -- so this file names one library where it used to name
 * two, and the parser under arnm can change without this file hearing about it.
 *
 * Both halves draw from an arena on this stack frame and nothing else. A document is released
 * where it has to be, but nothing is freed one allocation at a time: the arena goes when the
 * function returns, which is the whole of the memory management here.
 */
#include "service_core/jwt.h"

#include <stdlib.h>
#include <string.h>

#include <sodium.h>

#include "arnm/arena.h"
#include "arnm/json_reader.h"
#include "arnm/json_writer.h"

/** Longest base64url segment decoded. A JWT that needs more than this is not one of ours. */
#define MAX_SEGMENT 1024
/**
 * The arena a reader or a writer draws from, a multiple of 8 as arnm_init_arena_borrow() wants.
 *
 * Generous for two segments of MAX_SEGMENT: a parse behind an arena costs one allocation on the
 * insitu path, and the verify below releases the header's document before the payload's is
 * parsed. Nothing here reaches the host allocator, and nothing is meant to.
 */
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

/** Success, or the one warning that still means the text is complete and correct. */
static int arnm_ok(arnm_result result)
{
    return result == ARNM_SUCCESS || result == ARNM_WARNING_ARENA_MEMORY_NOT_RECLAIMED;
}

int sc_jwt_sign_hs256(const sc_jwt_config *config, const char *claim, const char *value,
                      int64_t now, int64_t ttl, char *out, size_t out_size)
{
    /* The header is the same twenty bytes every time, so it is spelled out rather than built:
     * {"alg":"HS256"} */
    static const char HEADER_B64[] = "eyJhbGciOiJIUzI1NiJ9";
    /* 8 byte aligned and a multiple of 8, which is what arnm_init_arena_borrow requires. */
    _Alignas(8) uint8_t scratch[JSON_ARENA];
    arnm allocator = {0};
    arnm_json_writer writer;
    arnm_memory_block payload;
    uint8_t signature[JWT_HMAC_LEN];
    size_t written = 0;
    uint32_t payload_len = 0;
    int n;

    if (config == NULL || claim == NULL || value == NULL || out == NULL)
        return -1;
    if (!arnm_ok(arnm_init_arena_borrow(&allocator, scratch, sizeof(scratch))))
        return -1;
    if (!arnm_ok(arnm_json_writer_init(&writer, &allocator, ARNM_JSON_WRITE_DEFAULT)))
        return -1;

    /* Member order follows what the reference implementation produces, so two tokens differ in
     * their timestamps and nothing else -- and a signature is only comparable over comparable
     * bytes. The writer keeps the order fields are added in, which is what makes that hold.
     *
     * No test between the lines: the writer keeps the first error and does nothing after it, so
     * the write below stands in for a check after every one of them. */
    arnm_json_writer_add_string(&writer, claim, value);
    arnm_json_writer_add_bool(&writer, "urn:gradido:claim", true);
    arnm_json_writer_add_int64(&writer, "iat", now);
    if (config->issuer != NULL)
        arnm_json_writer_add_string(&writer, "iss", config->issuer);
    if (config->audience != NULL)
        arnm_json_writer_add_string(&writer, "aud", config->audience);
    arnm_json_writer_add_int64(&writer, "exp", now + ttl);

    if (!arnm_ok(arnm_json_writer_write(&writer, &allocator, &payload, &payload_len)))
        return -1;

    if (sizeof(HEADER_B64) - 1 + 1 > out_size)
        return -1;
    memcpy(out, HEADER_B64, sizeof(HEADER_B64) - 1);
    written = sizeof(HEADER_B64) - 1;
    out[written++] = '.';

    if ((n = base64url_encode(payload.data, payload_len, out + written, out_size - written)) < 0)
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

/** @return non-zero if the member is a string equal to @p expected */
static int member_is(arnm_json_reader *reader, const char *key, const char *expected)
{
    const char *value = arnm_json_reader_get_string(reader, key);
    return value != NULL && strcmp(value, expected) == 0;
}

/**
 * The audience may be one string or a list of them -- the reference implementation writes the
 * first, RFC 7519 allows the second.
 *
 * @return non-zero if @p expected is among them
 */
static int audience_matches(arnm_json_reader *reader, const char *expected)
{
    if (arnm_json_reader_type_of(reader, "aud") == ARNM_JSON_TYPE_ARRAY) {
        /* enter hands back the value it left, and leave puts it back; the walk costs those two
         * lines and no bookkeeping of its own. */
        arnm_json_value *array = arnm_json_reader_enter(reader, "aud");
        const uint32_t count = arnm_json_reader_count(reader);
        int found = 0;
        uint32_t i;

        for (i = 0; i != count && !found; ++i) {
            arnm_json_value *element = arnm_json_reader_enter_at(reader, i);
            /* Look before reading. A NULL key asks about the current value itself, and
             * type_of records nothing -- where a getter on an element that is not a string
             * would record the reader's first error, and from there every later getter
             * answers its empty value. That would end this walk at the element rather than
             * at the match, and it would carry on into the claim read below, which would
             * then come back missing. An audience list with a number in it is malformed
             * either way; it is not this file's job to turn that into a wrong answer about
             * a different field. */
            if (arnm_json_reader_type_of(reader, NULL) == ARNM_JSON_TYPE_STRING) {
                const char *value = arnm_json_reader_get_string(reader, NULL);
                found = value != NULL && strcmp(value, expected) == 0;
            }
            arnm_json_reader_leave(reader, element);
        }
        arnm_json_reader_leave(reader, array);
        return found;
    }
    return member_is(reader, "aud", expected);
}

sc_jwt_result sc_jwt_verify_hs256(const sc_jwt_config *config, const char *token, size_t token_len,
                                  const char *claim, char *out, int64_t now)
{
    const char *dot1;
    const char *dot2;
    const char *sig_b64;
    size_t signed_len, sig_len;
    uint8_t expected[JWT_HMAC_LEN];
    uint8_t actual[JWT_HMAC_LEN];
    int actual_len;
    /* 8 byte aligned and a multiple of 8, as arnm_init_arena_borrow requires. */
    _Alignas(8) uint8_t scratch[JSON_ARENA];
    arnm allocator = {0};
    arnm_json_reader reader;
    /* The insitu parse reads four bytes past the document and writes zeroes there, which is
     * what lets its scanner run without a bounds test. The buffers carry that much slack. */
    char header[MAX_SEGMENT + ARNM_JSON_READER_INSITU_PADDING];
    char payload[MAX_SEGMENT + ARNM_JSON_READER_INSITU_PADDING];
    int header_len, payload_len;
    uint32_t claim_len = 0;
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

    header_len = base64url_decode(token, (size_t)(dot1 - token), (uint8_t *)header, MAX_SEGMENT);
    if (header_len < 0)
        return SC_JWT_MALFORMED;
    payload_len =
        base64url_decode(dot1 + 1, (size_t)(dot2 - dot1 - 1), (uint8_t *)payload, MAX_SEGMENT);
    if (payload_len < 0)
        return SC_JWT_MALFORMED;

    if (!arnm_ok(arnm_init_arena_borrow(&allocator, scratch, sizeof(scratch))))
        return SC_JWT_MALFORMED;
    if (!arnm_ok(arnm_json_reader_init(&reader, &allocator, ARNM_JSON_READ_DEFAULT)))
        return SC_JWT_MALFORMED;

    /* A parse that fails is the reader's first error, which is why it needs no test of its own:
     * the alg that follows answers NULL and the status carries the reason. */
    arnm_json_reader_parse_insitu(&reader, header, (uint32_t)header_len, sizeof(header));
    if (arnm_json_reader_status(&reader) != ARNM_SUCCESS)
        return SC_JWT_MALFORMED;
    if (!member_is(&reader, "alg", "HS256"))
        return SC_JWT_BAD_ALGORITHM;

    /* The header is done with. Its document goes back before the arena is reset, in that order:
     * resetting under a document the reader still holds would leave it pointing into ground the
     * next parse is about to hand out again. */
    arnm_json_reader_release(&reader);
    if (!arnm_ok(arnm_init_arena_borrow(&allocator, scratch, sizeof(scratch))))
        return SC_JWT_MALFORMED;

    arnm_json_reader_parse_insitu(&reader, payload, (uint32_t)payload_len, sizeof(payload));
    if (arnm_json_reader_status(&reader) != ARNM_SUCCESS)
        return SC_JWT_MALFORMED;

    /* Only an expiry that is there and is a number expires a token. A payload without one is
     * not treated as expired -- the behavior this was ported with, and not this file's to
     * change on its own. */
    if (arnm_json_reader_type_of(&reader, "exp") == ARNM_JSON_TYPE_NUMBER &&
        arnm_json_reader_get_int64(&reader, "exp") <= now)
        return SC_JWT_EXPIRED;

    if (config->issuer != NULL && !member_is(&reader, "iss", config->issuer))
        return SC_JWT_BAD_ISSUER;
    if (config->audience != NULL && !audience_matches(&reader, config->audience))
        return SC_JWT_BAD_AUDIENCE;

    claim_str = arnm_json_reader_get_string_length(&reader, claim, &claim_len);
    if (claim_str == NULL)
        return SC_JWT_MISSING_CLAIM;
    if (claim_len == 0 || claim_len >= SC_JWT_MAX_CLAIM)
        return SC_JWT_MISSING_CLAIM;
    memcpy(out, claim_str, claim_len);
    out[claim_len] = '\0';

    return SC_JWT_OK;
}
