/*
 * What the contract vectors cannot say about service_core/master_seed.h and dht_delegation.h:
 * that a new seed is random, and how the functions treat arguments they cannot work with. What a
 * seed derives and what a delegation's bytes are is contracts/test-vectors/master-seed.json, run
 * by tests/contract/test_master_seed_contract.cpp.
 *
 * C++ because googletest is; see the note at the top of test_cache.cpp.
 */
#include <gtest/gtest.h>

#include <cstring>

extern "C" {
#include "service_core/dht_delegation.h"
#include "service_core/master_seed.h"
}

TEST(MasterSeed, ANewOneIsDifferentEveryTimeWhateverWasTyped)
{
    uint8_t a[SC_MASTER_SEED_BYTES];
    uint8_t b[SC_MASTER_SEED_BYTES];
    ASSERT_EQ(sc_master_seed_new("same", a), SC_OK);
    ASSERT_EQ(sc_master_seed_new("same", b), SC_OK);
    EXPECT_NE(std::memcmp(a, b, sizeof(a)), 0);
    ASSERT_EQ(sc_master_seed_new(nullptr, a), SC_OK);
}

TEST(MasterSeed, RoundTripsThroughTheHexSetupWrites)
{
    uint8_t seed[SC_MASTER_SEED_BYTES];
    uint8_t back[SC_MASTER_SEED_BYTES];
    char text[SC_MASTER_SEED_HEX_SIZE];
    ASSERT_EQ(sc_master_seed_new("keys", seed), SC_OK);
    sc_master_seed_to_hex(seed, text);
    EXPECT_EQ(std::strlen(text), 2u * SC_MASTER_SEED_BYTES);
    ASSERT_EQ(sc_master_seed_parse(text, back), 1);
    EXPECT_EQ(std::memcmp(seed, back, sizeof(seed)), 0);
}

TEST(MasterSeed, RefusesWhatItCannotWorkWith)
{
    uint8_t seed[SC_MASTER_SEED_BYTES] = {0};
    uint8_t node_seed[32];
    uint8_t node_key[32];
    uint8_t delegation[SC_DHT_DELEGATION_BYTES];
    EXPECT_EQ(sc_master_seed_parse(nullptr, seed), 0);
    EXPECT_EQ(sc_master_seed_new("", nullptr), SC_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(sc_master_seed_derive_dht(nullptr, node_seed, node_key), SC_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(sc_dht_delegation_sign(nullptr, node_key, 0, delegation), SC_ERR_INVALID_ARGUMENT);
}

TEST(DhtDelegation, NamesTheNodeTheCommunityAndTheExpiryInOrder)
{
    uint8_t private_key[64];
    uint8_t node_key[32];
    uint8_t delegation[SC_DHT_DELEGATION_BYTES];
    const uint8_t expires[8] = {0, 0, 0x01, 0x9a, 0x77, 0x2b, 0x4c, 0xd8};
    std::memset(private_key, 0xa0, 32);
    std::memset(private_key + 32, 0xb5, 32);
    std::memset(node_key, 0x6d, sizeof(node_key));

    ASSERT_EQ(sc_dht_delegation_sign(private_key, node_key, 0x0000019a772b4cd8ull, delegation), SC_OK);
    EXPECT_EQ(std::memcmp(delegation, node_key, 32), 0);
    EXPECT_EQ(std::memcmp(delegation + 32, private_key + 32, 32), 0);
    EXPECT_EQ(std::memcmp(delegation + 64, expires, 8), 0);
}

int main(int argc, char **argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
