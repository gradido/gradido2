/*
 * jwt.c, from the outside: `service_core/jwt.h` and nothing else on the include path.
 *
 * What this file is *not* is the place where "which payloads are refused" is decided. That is
 * shared behaviour -- the TypeScript path has to refuse the same ones -- so it lives in
 * `contracts/test-vectors/jwt.json` and is run from both sides, here by
 * `../../tests/contract/test_jwt_contract.cpp` and there by
 * `packages/contract-tests/src/jwt.test.ts`. Duplicating a vector into this file would give the
 * project two answers to one question, which is the failure `contracts/AGENTS.md` exists to
 * prevent.
 *
 * What is left for this file is everything a contract cannot express, and all of it is
 * C-shaped:
 *
 * - **null arguments**, which a language with no null pointer has no vector for.
 * - **the buffers**, at their bounds and one past them. `out_size` on the signer and
 *   SC_JWT_MAX_CLAIM on the verifier are this implementation's, not the contract's.
 * - **the round trip**, which is the one thing the vector file structurally cannot do: every
 *   token in it was written by something else, so nothing there proves that what this signer
 *   writes is what this verifier reads.
 * - **the shape the signer produces**, which is a promise to whoever verifies the token next --
 *   including the reference implementation, which is why the header is pinned byte for byte.
 *
 * Everything here uses one secret and one clock, both handed in, so no test depends on the
 * machine it runs on.
 *
 * C++ because googletest is; see the note at the top of test_cache.cpp.
 */
#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include "service_core/jwt.h"
}

namespace
{

constexpr const char *kSecret = "a-secret-that-is-not-a-real-one";
constexpr const char *kOtherSecret = "a-different-secret-of-the-same-kind";
constexpr const char *kClaim = "gradidoID";
constexpr const char *kUuid = "3fa85f64-5717-4562-b3fc-2c963f66afa6";
/* Unix seconds, and a fixed one: a test that reads the clock is a test that fails at midnight. */
constexpr int64_t kNow = 1700000000;
constexpr int64_t kTtl = 600;

sc_jwt_config configWith(const char *secret, const char *issuer, const char *audience)
{
    sc_jwt_config config {};
    config.secret = reinterpret_cast<const uint8_t *>(secret);
    config.secret_len = std::strlen(secret);
    config.issuer = issuer;
    config.audience = audience;
    return config;
}

class JwtTest : public testing::Test
{
  protected:
    /* Poisoned rather than zeroed, so a claim buffer nobody wrote reads as poison instead of as
     * an empty string -- which is a value something downstream would act on. */
    void SetUp() override { std::memset(claim_, '?', sizeof(claim_)); }

    sc_jwt_config config_ = configWith(kSecret, "gradido", "gradido-backend");
    char token_[SC_JWT_MAX_TOKEN] {};
    char claim_[SC_JWT_MAX_CLAIM] {};
};

/* --- what the signer writes ----------------------------------------------------------- */

/* The header is spelled out in jwt.c rather than built, so this pins that the constant is the
 * base64url of {"alg":"HS256"} and has not been mistyped. A token whose header segment is
 * anything else is one no other library accepts. */
TEST_F(JwtTest, TheHeaderSegmentIsTheOneEveryTokenCarries)
{
    const int written = sc_jwt_sign_hs256(&config_, kClaim, kUuid, kNow, kTtl, token_,
                                          sizeof(token_));

    ASSERT_GT(written, 0);
    EXPECT_EQ(std::string(token_).substr(0, 20), "eyJhbGciOiJIUzI1NiJ9");
    EXPECT_EQ(token_[20], '.');
}

TEST_F(JwtTest, ATokenIsThreeSegmentsAndTheLengthItReports)
{
    const int written = sc_jwt_sign_hs256(&config_, kClaim, kUuid, kNow, kTtl, token_,
                                          sizeof(token_));
    ASSERT_GT(written, 0);

    const std::string token(token_);
    EXPECT_EQ(token.size(), static_cast<size_t>(written)) << "the return is the string length";
    EXPECT_EQ(std::count(token.begin(), token.end(), '.'), 2);
    EXPECT_EQ(token.find(".."), std::string::npos) << "no segment is empty";
    EXPECT_EQ(token.find_first_of("+/="), std::string::npos)
        << "base64url, not base64: a '+' or a '/' is a token no JWT library reads";
}

/* Two calls with the same arguments have to produce the same bytes. Not a property anyone needs
 * at runtime -- it is what makes every other test here able to compare a token at all, and it
 * would stop holding the moment something random or clock-derived crept into the payload. */
TEST_F(JwtTest, SigningTwiceWritesTheSameToken)
{
    char again[SC_JWT_MAX_TOKEN];

    ASSERT_GT(sc_jwt_sign_hs256(&config_, kClaim, kUuid, kNow, kTtl, token_, sizeof(token_)), 0);
    ASSERT_GT(sc_jwt_sign_hs256(&config_, kClaim, kUuid, kNow, kTtl, again, sizeof(again)), 0);
    EXPECT_STREQ(token_, again);
}

TEST_F(JwtTest, ADifferentSecretWritesADifferentSignature)
{
    sc_jwt_config other = configWith(kOtherSecret, "gradido", "gradido-backend");
    char with_other[SC_JWT_MAX_TOKEN];

    ASSERT_GT(sc_jwt_sign_hs256(&config_, kClaim, kUuid, kNow, kTtl, token_, sizeof(token_)), 0);
    ASSERT_GT(sc_jwt_sign_hs256(&other, kClaim, kUuid, kNow, kTtl, with_other, sizeof(with_other)),
              0);

    const std::string a(token_), b(with_other);
    EXPECT_EQ(a.substr(0, a.rfind('.')), b.substr(0, b.rfind('.'))) << "same signed input";
    EXPECT_NE(a, b) << "and a different signature over it";
}

/* --- the round trip ------------------------------------------------------------------- */

TEST_F(JwtTest, WhatThisSignerWritesThisVerifierReads)
{
    const int written = sc_jwt_sign_hs256(&config_, kClaim, kUuid, kNow, kTtl, token_,
                                          sizeof(token_));
    ASSERT_GT(written, 0);

    EXPECT_EQ(sc_jwt_verify_hs256(&config_, token_, static_cast<size_t>(written), kClaim, claim_,
                                  kNow),
              SC_JWT_OK);
    EXPECT_STREQ(claim_, kUuid);
}

TEST_F(JwtTest, ATokenIsGoodUntilItsTtlAndNotOneSecondLonger)
{
    const int written = sc_jwt_sign_hs256(&config_, kClaim, kUuid, kNow, kTtl, token_,
                                          sizeof(token_));
    ASSERT_GT(written, 0);
    const size_t length = static_cast<size_t>(written);

    EXPECT_EQ(sc_jwt_verify_hs256(&config_, token_, length, kClaim, claim_, kNow + kTtl - 1),
              SC_JWT_OK);
    /* exp is written as now + ttl and the verifier refuses `exp <= now`, so the last instant the
     * token is good at is one second before its expiry. contracts/test-vectors/jwt.json pins
     * that boundary for both implementations; this pins that the signer sits on the same side
     * of it. */
    EXPECT_EQ(sc_jwt_verify_hs256(&config_, token_, length, kClaim, claim_, kNow + kTtl),
              SC_JWT_EXPIRED);
}

TEST_F(JwtTest, AVerifierWithAnotherSecretRefusesTheSignature)
{
    sc_jwt_config other = configWith(kOtherSecret, "gradido", "gradido-backend");
    const int written = sc_jwt_sign_hs256(&config_, kClaim, kUuid, kNow, kTtl, token_,
                                          sizeof(token_));
    ASSERT_GT(written, 0);

    EXPECT_EQ(sc_jwt_verify_hs256(&other, token_, static_cast<size_t>(written), kClaim, claim_,
                                  kNow),
              SC_JWT_BAD_SIGNATURE);
}

/* A signer that was given no issuer writes none, so a verifier that wants one refuses the token.
 * The pair matters because "not configured" and "absent from the token" are different states and
 * only one of them is a refusal. */
TEST_F(JwtTest, AnUnconfiguredSignerWritesNeitherIssuerNorAudience)
{
    sc_jwt_config bare = configWith(kSecret, nullptr, nullptr);
    const int written = sc_jwt_sign_hs256(&bare, kClaim, kUuid, kNow, kTtl, token_,
                                          sizeof(token_));
    ASSERT_GT(written, 0);
    const size_t length = static_cast<size_t>(written);

    EXPECT_EQ(sc_jwt_verify_hs256(&bare, token_, length, kClaim, claim_, kNow), SC_JWT_OK);
    EXPECT_EQ(sc_jwt_verify_hs256(&config_, token_, length, kClaim, claim_, kNow),
              SC_JWT_BAD_ISSUER);
}

/* The claim is asked for by name, and the name is the caller's argument rather than a literal --
 * which is the one place jwt.c builds a field table by hand instead of through arnm's macro. A
 * name nobody wrote is a miss, and it must not read as the claim that is there. */
TEST_F(JwtTest, TheClaimIsTheOneAskedForByName)
{
    const int written = sc_jwt_sign_hs256(&config_, kClaim, kUuid, kNow, kTtl, token_,
                                          sizeof(token_));
    ASSERT_GT(written, 0);
    const size_t length = static_cast<size_t>(written);

    EXPECT_EQ(sc_jwt_verify_hs256(&config_, token_, length, "iss", claim_, kNow), SC_JWT_OK);
    EXPECT_STREQ(claim_, "gradido");
    EXPECT_EQ(sc_jwt_verify_hs256(&config_, token_, length, "gradidoId", claim_, kNow),
              SC_JWT_MISSING_CLAIM)
        << "a claim name is compared, not matched case-insensitively";
    EXPECT_EQ(sc_jwt_verify_hs256(&config_, token_, length, "gradido", claim_, kNow),
              SC_JWT_MISSING_CLAIM)
        << "nor by prefix";
}

/* --- the buffers ---------------------------------------------------------------------- */

/* The signer measures before it writes, at every one of the four places it appends. Walking the
 * size down one byte at a time is what proves it does not write past the end at any of them:
 * with an ASan build (see AGENTS.md section 4) an off-by-one anywhere in that sequence is a
 * report rather than a silent pass. */
TEST_F(JwtTest, TheSignerRefusesEveryBufferThatIsTooSmallRatherThanOverrunningIt)
{
    const int needed = sc_jwt_sign_hs256(&config_, kClaim, kUuid, kNow, kTtl, token_,
                                         sizeof(token_));
    ASSERT_GT(needed, 0);
    const size_t length = static_cast<size_t>(needed);

    /* Exactly enough is enough: the return counts the characters and the terminator needs one
     * more. */
    std::vector<char> exact(length + 1, '\0');
    EXPECT_EQ(sc_jwt_sign_hs256(&config_, kClaim, kUuid, kNow, kTtl, exact.data(), exact.size()),
              needed);
    EXPECT_STREQ(exact.data(), token_);

    for (size_t size = 0; size <= length; ++size) {
        std::vector<char> tight(size + 1, '\0');
        /* One byte of headroom past `size`, so an overrun of one lands in the guard rather than
         * in the allocator's bookkeeping and is visible without a sanitizer as well. */
        tight[size] = '\x7f';
        EXPECT_EQ(sc_jwt_sign_hs256(&config_, kClaim, kUuid, kNow, kTtl, tight.data(), size), -1)
            << "signed into " << size << " bytes";
        EXPECT_EQ(tight[size], '\x7f') << "wrote past " << size << " bytes";
    }
}

/* SC_JWT_MAX_CLAIM is the verifier's buffer and therefore the longest claim it can hand back.
 * The two vectors in contracts/test-vectors/jwt.json pin the bound as shared behaviour; this
 * pins that the signer and the verifier agree about it, which is a round trip and not a vector. */
TEST_F(JwtTest, TheLongestClaimThatFitsRoundTripsAndOneMoreDoesNot)
{
    const std::string longest(SC_JWT_MAX_CLAIM - 1, 'u');
    const std::string one_more(SC_JWT_MAX_CLAIM, 'u');
    char big[SC_JWT_MAX_TOKEN * 2];

    int written = sc_jwt_sign_hs256(&config_, kClaim, longest.c_str(), kNow, kTtl, big,
                                    sizeof(big));
    ASSERT_GT(written, 0);
    EXPECT_EQ(sc_jwt_verify_hs256(&config_, big, static_cast<size_t>(written), kClaim, claim_,
                                  kNow),
              SC_JWT_OK);
    EXPECT_EQ(std::string(claim_), longest);

    written = sc_jwt_sign_hs256(&config_, kClaim, one_more.c_str(), kNow, kTtl, big, sizeof(big));
    ASSERT_GT(written, 0) << "the signer has no such bound; only the verifier does";
    std::memset(claim_, '?', sizeof(claim_));
    EXPECT_EQ(sc_jwt_verify_hs256(&config_, big, static_cast<size_t>(written), kClaim, claim_,
                                  kNow),
              SC_JWT_MISSING_CLAIM);
    EXPECT_EQ(claim_[0], '?') << "a refusal must leave the caller's buffer alone";
}

/* The signer builds its payload in an arena on its own stack frame, so what it can write is
 * bounded by that arena and not by `out_size`. Where that bound falls is an implementation
 * detail and this does not pin it; what it pins is which side of the line the failure comes out
 * on -- a refusal, not a truncated token and not a write past the arena. */
TEST_F(JwtTest, AClaimTooLargeForTheSignersArenaIsRefusedRatherThanTruncated)
{
    const std::string enormous(4096, 'u');
    char big[65536];

    EXPECT_EQ(sc_jwt_sign_hs256(&config_, kClaim, enormous.c_str(), kNow, kTtl, big, sizeof(big)),
              -1);
}

/* A payload longer than the verifier's decode buffer is refused rather than truncated into one.
 * MAX_SEGMENT is not in the header, so this reaches it the only way a caller can: by signing a
 * claim big enough that the payload passes it. */
TEST_F(JwtTest, APayloadTooLongToDecodeIsRefused)
{
    /* Long enough that the payload passes MAX_SEGMENT once the other members are beside it, and
     * short enough that the signer's own arena still holds the document it builds. */
    const std::string oversized(980, 'u');
    char big[8192];

    const int written = sc_jwt_sign_hs256(&config_, kClaim, oversized.c_str(), kNow, kTtl, big,
                                          sizeof(big));
    ASSERT_GT(written, 0);
    EXPECT_EQ(sc_jwt_verify_hs256(&config_, big, static_cast<size_t>(written), kClaim, claim_,
                                  kNow),
              SC_JWT_MALFORMED);
}

/* --- null, and nothing at all --------------------------------------------------------- */

TEST_F(JwtTest, NullArgumentsAreRefusedRatherThanDereferenced)
{
    ASSERT_GT(sc_jwt_sign_hs256(&config_, kClaim, kUuid, kNow, kTtl, token_, sizeof(token_)), 0);
    const size_t length = std::strlen(token_);

    EXPECT_EQ(sc_jwt_verify_hs256(nullptr, token_, length, kClaim, claim_, kNow),
              SC_JWT_MALFORMED);
    EXPECT_EQ(sc_jwt_verify_hs256(&config_, nullptr, length, kClaim, claim_, kNow),
              SC_JWT_MALFORMED);
    EXPECT_EQ(sc_jwt_verify_hs256(&config_, token_, length, nullptr, claim_, kNow),
              SC_JWT_MALFORMED);
    EXPECT_EQ(sc_jwt_verify_hs256(&config_, token_, length, kClaim, nullptr, kNow),
              SC_JWT_MALFORMED);

    EXPECT_EQ(sc_jwt_sign_hs256(nullptr, kClaim, kUuid, kNow, kTtl, token_, sizeof(token_)), -1);
    EXPECT_EQ(sc_jwt_sign_hs256(&config_, nullptr, kUuid, kNow, kTtl, token_, sizeof(token_)), -1);
    EXPECT_EQ(sc_jwt_sign_hs256(&config_, kClaim, nullptr, kNow, kTtl, token_, sizeof(token_)),
              -1);
    EXPECT_EQ(sc_jwt_sign_hs256(&config_, kClaim, kUuid, kNow, kTtl, nullptr, sizeof(token_)), -1);
}

TEST_F(JwtTest, AnEmptyTokenIsRefused)
{
    EXPECT_EQ(sc_jwt_verify_hs256(&config_, "", 0, kClaim, claim_, kNow), SC_JWT_MALFORMED);
}

/* The header says it is safe to call more than once and from one thread, and callers of a
 * library that does not know who they are will. */
TEST_F(JwtTest, InitIsSafeToCallAgain)
{
    sc_jwt_init();
    sc_jwt_init();
    ASSERT_GT(sc_jwt_sign_hs256(&config_, kClaim, kUuid, kNow, kTtl, token_, sizeof(token_)), 0);
}

} // namespace

int main(int argc, char **argv)
{
    /* Before any test runs rather than in a fixture: libsodium's initialisation is not thread
     * safe and gtest may shard, and a verifier that could not initialise has no business
     * reporting a refusal as a test result. */
    sc_jwt_init();
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
