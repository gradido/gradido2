/*
 * The user domain's logic, which is the half of it that needs no database.
 *
 * What each of these is for is on the declarations in backend_core/domain/user.h; what is
 * asserted here is the part a reader of that header would have to take on trust: that the
 * verification code stays inside the bound both databases can hold, that the gradido id ladder
 * gives up rather than spins, and that an address is normalized the one way every lookup and
 * every write agrees on.
 *
 * C++ because googletest is; see the note at the top of service-core/tests/test_cache.cpp.
 */
#include <gtest/gtest.h>

#include <set>
#include <string>

extern "C" {
#include "backend_core/domain/community.h"
#include "backend_core/domain/user.h"
#include "backend_core/language.h"
#include "backend_core/uuid.h"
}

namespace
{

std::string normalized(const char *input)
{
    char out[BC_EMAIL_MAX];
    EXPECT_TRUE(bc_normalize_email(input, out, sizeof(out)));
    return out;
}

TEST(NormalizeEmail, TrimsAndLowercases)
{
    EXPECT_EQ(normalized("  Max.Mustermann@Example.ORG \t"), "max.mustermann@example.org");
    EXPECT_EQ(normalized("already@lower.tld"), "already@lower.tld");
}

TEST(NormalizeEmail, RefusesAnAddressThatWouldNotFit)
{
    const std::string address(64, 'a');
    char out[16];

    EXPECT_FALSE(bc_normalize_email(address.c_str(), out, sizeof(out)));
}

/*
 * 53 bits, never zero. The width is contracts/db/user_contacts.json's, and it is there because
 * SQLite hands an INTEGER to JavaScript as a double: a wider code would come back changed on the
 * reference path, with nothing failing anywhere. This implementation would not round it, which
 * is exactly why the bound has to be asserted here rather than assumed to be visible.
 */
TEST(VerificationCode, StaysInsideTheContractedRange)
{
    std::set<uint64_t> seen;

    for (int i = 0; i != 2000; ++i) {
        const uint64_t code = bc_new_email_verification_code();

        ASSERT_NE(code, 0u);
        ASSERT_LE(code, (1ull << 53) - 1);
        seen.insert(code);
    }
    /* Not a statistical test: 2000 draws out of 2^53 collide with probability around 2e-13, so a
     * repeat here means the draw is not random rather than that we were unlucky. */
    EXPECT_EQ(seen.size(), 2000u);
}

int never_taken(const char *, void *) { return 0; }
int always_taken(const char *, void *) { return 1; }
int cannot_tell(const char *, void *) { return -1; }

int taken_once(const char *, void *user_data)
{
    int *calls = static_cast<int *>(user_data);
    return (*calls)++ == 0 ? 1 : 0;
}

TEST(GradidoId, DrawsAUuidNobodyHolds)
{
    char id[BC_UUID_TEXT_MAX];

    ASSERT_EQ(bc_new_gradido_id(never_taken, nullptr, id), SC_OK);
    EXPECT_EQ(std::string(id).size(), 36u);
    /* v4 and the RFC 9562 variant, in the two places they live. */
    EXPECT_EQ(id[14], '4');
    EXPECT_NE(std::string("89ab").find(id[19]), std::string::npos);
}

TEST(GradidoId, DrawsAgainWhenTheFirstIsTaken)
{
    char first[BC_UUID_TEXT_MAX];
    char second[BC_UUID_TEXT_MAX];
    int calls = 0;

    ASSERT_EQ(bc_new_gradido_id(taken_once, &calls, first), SC_OK);
    EXPECT_EQ(calls, 2);
    ASSERT_EQ(bc_new_gradido_id(never_taken, nullptr, second), SC_OK);
    EXPECT_STRNE(first, second);
}

/* Five draws in a row colliding is not a number that happens at 122 random bits -- it is the
 * lookup answering yes for reasons of its own, and a loop that would spin on it forever is worse
 * than an error that says so. */
TEST(GradidoId, GivesUpRatherThanSpinning)
{
    char id[BC_UUID_TEXT_MAX];

    EXPECT_EQ(bc_new_gradido_id(always_taken, nullptr, id), SC_ERR_UNAVAILABLE);
}

/* A lookup that failed is not a free uuid. Treating it as one would write a row against an index
 * nobody checked. */
TEST(GradidoId, StopsWhenTheLookupCannotTell)
{
    char id[BC_UUID_TEXT_MAX];

    EXPECT_EQ(bc_new_gradido_id(cannot_tell, nullptr, id), SC_ERR_INVALID_ARGUMENT);
}

TEST(Language, KnowsTheContractedSetAndDefaultsToGerman)
{
    EXPECT_TRUE(bc_language_is_known("de"));
    EXPECT_TRUE(bc_language_is_known("el"));
    EXPECT_FALSE(bc_language_is_known("de-AT"));
    EXPECT_FALSE(bc_language_is_known(""));
    EXPECT_FALSE(bc_language_is_known(nullptr));

    EXPECT_STREQ(bc_language_or_default("fr"), "fr");
    /* ignore_and_warn: the value comes from a browser's locale, which nobody typed and nobody
     * can correct from the form, so an unknown one is the default rather than a refusal. */
    EXPECT_STREQ(bc_language_or_default("de-AT"), BC_LANGUAGE_DEFAULT);
    EXPECT_STREQ(bc_language_or_default(nullptr), BC_LANGUAGE_DEFAULT);
}

/* The layout is libsodium's and legacy's dht-node's: the secret key is the seed followed by the
 * public key. A build whose halves disagreed about that would write a private_key nothing could
 * verify a signature against. */
TEST(CommunityKeys, PutThePublicKeyInsideTheSecretOne)
{
    uint8_t first_public[BC_PUBLIC_KEY_SIZE];
    uint8_t first_private[BC_PRIVATE_KEY_SIZE];
    uint8_t second_public[BC_PUBLIC_KEY_SIZE];
    uint8_t second_private[BC_PRIVATE_KEY_SIZE];

    bc_community_new_keys(first_public, first_private);
    bc_community_new_keys(second_public, second_private);

    EXPECT_EQ(memcmp(first_private + 32, first_public, BC_PUBLIC_KEY_SIZE), 0);
    EXPECT_NE(memcmp(first_public, second_public, BC_PUBLIC_KEY_SIZE), 0);
}

} // namespace

int main(int argc, char **argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
