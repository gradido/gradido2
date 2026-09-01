/*
 * HS256 over libsodium, which also does the base64url, with arnm reading and writing the JSON.
 *
 * Carried over from the h2o prototype that preceded this repository, minus its OpenSSL half.
 * See jwt.h for why there is only one crypto backend here.
 *
 * The JSON goes through arnm/json_reader.h and arnm/json_writer.h rather than through yyjson
 * directly. yyjson is still what runs underneath, but nothing of it reaches those headers -- no
 * type, no constant, no include path -- so this file names one library where it used to name
 * two, and the parser under arnm can change without this file hearing about it.
 *
 * Reading is a table and not a cursor. arnm 0.7.5 took the cursor away: there is no entering an
 * object, no asking it for a member by name and no getter per value. A shape is described once,
 * as a list of fields -- what a key is called, what it should become, where to put it -- and one
 * walk of the member chain answers all of them at once. Which members arrived comes back as a
 * bit per field, and that mask is what every decision below is made on. See the audience check
 * for the one thing this verifier lost with the cursor.
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
    /* No size hint: the pools are told up front how big a document will be, and this one is six
     * short members whose first chunk already holds it. A hint here would name a number that has
     * to be kept in step with the fields below for nothing to gain. */
    if (!arnm_ok(arnm_json_writer_init(&writer, &allocator, ARNM_JSON_WRITE_DEFAULT, NULL)))
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

/** @return non-zero if @p block holds exactly the characters of @p expected */
static int block_is(const arnm_memory_block *block, const char *expected)
{
    const size_t len = strlen(expected);

    /* For a STRING field arnm fills `size` with the string's length rather than with an
     * allocation, and points `data` into the document. Comparing the length first is what keeps
     * a prefix of the expected value from passing. */
    return block->data != NULL && block->size == (uint32_t)len &&
           memcmp(block->data, expected, len) == 0;
}

/*
 * The tables the two walks are driven by. A bit of the mask arnm hands back names the entry at
 * the same index, so these two runs of constants have to move together with the initialisers
 * below -- which is why they sit here rather than being counted at each call site.
 */
#define HEADER_FIELD_ALG 0u
#define HEADER_FIELD_COUNT 1u

#define PAYLOAD_FIELD_ISS 0u
#define PAYLOAD_FIELD_EXP 1u
#define PAYLOAD_FIELD_COUNT 2u

#define FOUND(mask, field) (((mask) & (1ull << (field))) != 0)

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
    arnm_json_value *root = NULL;
    /* The insitu parse reads four bytes past the document and writes zeroes there, which is
     * what lets its scanner run without a bounds test. The buffers carry that much slack. */
    char header[MAX_SEGMENT + ARNM_JSON_READER_INSITU_PADDING];
    char payload[MAX_SEGMENT + ARNM_JSON_READER_INSITU_PADDING];
    int header_len, payload_len;
    arnm_memory_block alg = {0};
    arnm_memory_block issuer = {0};
    arnm_memory_block audience = {0};
    arnm_memory_block claim_value = {0};
    int64_t expires_at = 0;
    uint64_t found = 0;
    int claim_present = 0;

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
    if (header_len <= 0)
        return SC_JWT_MALFORMED;
    payload_len =
        base64url_decode(dot1 + 1, (size_t)(dot2 - dot1 - 1), (uint8_t *)payload, MAX_SEGMENT);
    if (payload_len <= 0)
        return SC_JWT_MALFORMED;

    if (!arnm_ok(arnm_init_arena_borrow(&allocator, scratch, sizeof(scratch))))
        return SC_JWT_MALFORMED;
    if (!arnm_ok(arnm_json_reader_init(&reader, &allocator)))
        return SC_JWT_MALFORMED;

    /* The header, and the one member of it this verifier has an opinion about. `typ` and
     * anything else a signer put there is passed over: a table names what it wants and a
     * document is allowed to carry more. */
    {
        arnm_json_field header_fields[HEADER_FIELD_COUNT] = {
            ARNM_JSON_FIELD_STRING("alg", &alg)};

        if (arnm_json_reader_parse_insitu(&reader, header, (uint32_t)header_len, sizeof(header),
                                          false, &root) != ARNM_SUCCESS)
            return SC_JWT_MALFORMED;

        /* The walk's own result is not read, here or below. It says why the walk stopped, and
         * that is a question about the document rather than about the token: what decides this
         * function is whether the member arrived, which is what the mask answers. A header
         * whose `alg` is a number, one that is no object at all, and one with no `alg` in it
         * are the same answer -- not HS256 -- and each is refused on the same line. */
        arnm_json_read_object(root, header_fields, HEADER_FIELD_COUNT, &found);
        if (!FOUND(found, HEADER_FIELD_ALG) || !block_is(&alg, "HS256"))
            return SC_JWT_BAD_ALGORITHM;
    }

    /* The header is done with. Its document goes back before the arena is reset, in that order:
     * resetting under a document the reader still holds would leave it pointing into ground the
     * next parse is about to hand out again. */
    arnm_json_reader_release(&reader);
    if (!arnm_ok(arnm_init_arena_borrow(&allocator, scratch, sizeof(scratch))))
        return SC_JWT_MALFORMED;

    if (arnm_json_reader_parse_insitu(&reader, payload, (uint32_t)payload_len, sizeof(payload),
                                      false, &root) != ARNM_SUCCESS)
        return SC_JWT_MALFORMED;

    /*
     * The two members this verifier always wants, in one walk of the payload's member chain.
     *
     * The table is in the order sc_jwt_sign_hs256() writes them, which is the order a token from
     * the reference implementation carries them in as well, so the walk meets each entry at the
     * first key it compares against. `urn:gradido:claim` and `iat` are members this verifier has
     * no opinion about; a table names what it wants and the rest is passed over.
     */
    {
        arnm_json_field payload_fields[PAYLOAD_FIELD_COUNT] = {
            ARNM_JSON_FIELD_STRING("iss", &issuer),
            ARNM_JSON_FIELD_INT64("exp", &expires_at)};

        arnm_json_read_object(root, payload_fields, PAYLOAD_FIELD_COUNT, &found);
    }

    /*
     * The claim the caller asked for, in a walk of its own -- and it has to be its own, because
     * its key is an argument rather than a literal.
     *
     * A table stops at the first entry that matches a key, starting from the lowest one it has
     * not filled. Two entries spelled the same are therefore not two questions: the first takes
     * every match and the second is never filled. `claim` may be *any* name, `"iss"` and
     * `"exp"` included, so an entry for it beside those two would silently answer the wrong
     * question -- ask for the claim `iss` and the issuer check would read nothing and refuse a
     * token that is fine. It fails closed either way, which is why it is a correctness bug and
     * not a hole; it is still a wrong answer, and a separate walk is what removes the whole
     * class rather than the two spellings anyone thought of.
     *
     * The entry is built rather than written with ARNM_JSON_FIELD_STRING(): that macro takes the
     * key's length from a literal with sizeof. The struct is the macro's whole output, so
     * spelling it out costs the strlen and nothing else.
     */
    {
        arnm_json_field claim_field = {claim, (uint32_t)strlen(claim),
                                       ARNM_JSON_FIELD_TYPE_STRING, &claim_value};
        uint64_t claim_found = 0;

        arnm_json_read_object(root, &claim_field, 1, &claim_found);
        claim_present = claim_found != 0;
    }

    /* Require the claim, then check it. A claim that is absent is not a claim that passed:
     * the prototype this was carried over from checks exp only where it is present and numeric,
     * so a correctly signed token without exp -- or with "exp": null, or with it as a string --
     * never expires. All four cases were reproduced and accepted; Architecture.md, *Safety net*,
     * records it. The hard session timeout is only as hard as this line.
     *
     * Reading the mask rather than the walk's result is what keeps that true through arnm's
     * table: a member of the wrong type stops the walk and leaves its bit clear, which is the
     * same "not there" as a member nobody wrote -- and the members after it are unread and
     * therefore unset as well. Everything below is a refusal unless a bit says otherwise. */
    if (!FOUND(found, PAYLOAD_FIELD_EXP))
        return SC_JWT_MISSING_CLAIM;
    if (expires_at <= now)
        return SC_JWT_EXPIRED;

    if (config->issuer != NULL &&
        (!FOUND(found, PAYLOAD_FIELD_ISS) || !block_is(&issuer, config->issuer)))
        return SC_JWT_BAD_ISSUER;

    /*
     * `aud` in a walk of its own, and only where there is something to check it against.
     *
     * It is not in the table above because a table converts a member where it stands and stops
     * the whole walk at one it cannot: an `aud` this verifier will not read would take `exp` and
     * the claim down with it, and the token would be refused for the wrong reason. On its own it
     * costs one more pass over the member chain, stopping at the key it wants, and only for a
     * configuration that asked for the check.
     *
     * **A list-valued audience is refused.** RFC 7519 allows `aud` to be an array of strings and
     * this verifier used to walk one. arnm 0.7.5 hands a value out only as a handle, and the
     * only two calls that take a handle are the object walk and arnm_json_read_array() -- so an
     * array's elements can be counted but a string among them can never be read. What is left is
     * the single-string form, which is what gradido's reference implementation writes and what
     * sc_jwt_sign_hs256() writes beside it. A token whose `aud` is an array is answered with
     * SC_JWT_BAD_AUDIENCE, which is the honest code for it: the audience was not confirmed.
     */
    if (config->audience != NULL) {
        arnm_json_field audience_fields[1] = {ARNM_JSON_FIELD_STRING("aud", &audience)};
        uint64_t audience_found = 0;

        arnm_json_read_object(root, audience_fields, 1, &audience_found);
        if (audience_found == 0 || !block_is(&audience, config->audience))
            return SC_JWT_BAD_AUDIENCE;
    }

    if (!claim_present)
        return SC_JWT_MISSING_CLAIM;
    if (claim_value.size == 0 || claim_value.size >= SC_JWT_MAX_CLAIM)
        return SC_JWT_MISSING_CLAIM;
    memcpy(out, claim_value.data, claim_value.size);
    out[claim_value.size] = '\0';

    return SC_JWT_OK;
}
