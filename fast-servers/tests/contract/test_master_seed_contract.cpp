/*
 * `contracts/test-vectors/master-seed.json`, run against service_core/master_seed.h and
 * service_core/dht_delegation.h.
 *
 * The other half is `packages/contract-tests/src/master-seed.test.ts`. The derivation is the same
 * C function underneath both, so what this pins is the index and the byte layout around it; the
 * delegation is signed by libsodium here and by node:crypto there, and the file's values were
 * signed by libp2p-ffi itself.
 *
 * C++ because googletest is; see the note at the top of service-core/tests/test_cache.cpp.
 */
#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

extern "C" {
#include "service_core/dht_delegation.h"
#include "service_core/master_seed.h"
}

#include "vectors.hpp"

namespace
{

const contract::VectorFile &vectorFile()
{
    static const contract::VectorFile file("master-seed");
    return file;
}

std::vector<uint8_t> bytes(const std::string &hex)
{
    std::vector<uint8_t> out(hex.size() / 2);
    for (size_t i = 0; i != out.size(); ++i)
        out[i] = static_cast<uint8_t>(std::stoul(hex.substr(2 * i, 2), nullptr, 16));
    return out;
}

std::string hex(const uint8_t *data, size_t size)
{
    static const char digits[] = "0123456789abcdef";
    std::string out;
    for (size_t i = 0; i != size; ++i) {
        out.push_back(digits[data[i] >> 4]);
        out.push_back(digits[data[i] & 0xf]);
    }
    return out;
}

/** A rule's value, which the file spells as { "type", "value" }. */
int64_t rule(const char *name)
{
    arnm_json_value *rules = contract::requiredObject(vectorFile().root(), "rules");
    return contract::requiredDecimal(contract::requiredObject(rules, name), "value");
}

} // namespace

TEST(MasterSeedContract, TheConstantsAreTheOnesTheFileWasWrittenAgainst)
{
    EXPECT_EQ(rule("pathDht"), static_cast<int64_t>(SC_MASTER_SEED_PATH_DHT));
    EXPECT_EQ(rule("seedBytes"), static_cast<int64_t>(SC_MASTER_SEED_BYTES));
}

TEST(MasterSeedContract, EveryVector)
{
    ASSERT_FALSE(vectorFile().vectors().empty());
    for (arnm_json_value *value : vectorFile().vectors()) {
        const std::string id = contract::requiredString(value, "id");
        const std::string kind = contract::requiredString(value, "kind");
        arnm_json_value *expect = contract::requiredObject(value, "expect");
        SCOPED_TRACE(id);

        if (kind == "parse") {
            uint8_t seed[SC_MASTER_SEED_BYTES];
            const std::string text = contract::requiredString(value, "text");
            EXPECT_EQ(sc_master_seed_parse(text.c_str(), seed) != 0,
                      contract::requiredBool(expect, "accepted"));
        } else if (kind == "derive" || kind == "delegate") {
            const std::vector<uint8_t> master = bytes(contract::requiredString(value, "masterSeed"));
            uint8_t node_seed[32];
            uint8_t node_key[32];
            ASSERT_EQ(master.size(), SC_MASTER_SEED_BYTES);
            ASSERT_EQ(sc_master_seed_derive_dht(master.data(), node_seed, node_key), SC_OK);
            EXPECT_EQ(hex(node_key, 32), contract::requiredString(expect, "dhtNodeKey"));

            if (kind == "derive") {
                EXPECT_EQ(hex(node_seed, 32), contract::requiredString(expect, "dhtNodeSeed"));
            } else {
                /* The column's 64 bytes: the community's seed, then its public key. */
                std::vector<uint8_t> privateKey = bytes(contract::requiredString(value, "communitySeed"));
                const std::vector<uint8_t> communityKey =
                    bytes(contract::requiredString(expect, "communityKey"));
                privateKey.insert(privateKey.end(), communityKey.begin(), communityKey.end());
                uint8_t delegation[SC_DHT_DELEGATION_BYTES];
                ASSERT_EQ(sc_dht_delegation_sign(
                              privateKey.data(), node_key,
                              static_cast<uint64_t>(contract::requiredDecimal(value, "expiresMs")),
                              delegation),
                          SC_OK);
                EXPECT_EQ(hex(delegation, sizeof(delegation)),
                          contract::requiredString(expect, "delegation"));
            }
        } else {
            ADD_FAILURE() << "no such kind: " << kind;
        }
    }
}

int main(int argc, char **argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
