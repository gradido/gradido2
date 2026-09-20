/*
 * `contracts/test-vectors/shard.json`, run against service_core/shard.h. The other half is
 * `packages/contract-tests/src/shard.test.ts`.
 *
 * C++ because googletest is; see the note at the top of service-core/tests/test_cache.cpp.
 */
#include <gtest/gtest.h>

#include <string>
#include <vector>

extern "C" {
#include "service_core/shard.h"
}

#include "vectors.hpp"

namespace
{

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

} // namespace

TEST(ShardContract, TheShardCountIsTheOneTheFileWasWrittenAgainst)
{
    static const contract::VectorFile file("shard");
    arnm_json_value *rules = contract::requiredObject(file.root(), "rules");
    EXPECT_EQ(contract::requiredDecimal(contract::requiredObject(rules, "shardCount"), "value"),
              static_cast<int64_t>(SC_SHARD_COUNT));
}

TEST(ShardContract, EveryVector)
{
    static const contract::VectorFile file("shard");
    ASSERT_FALSE(file.vectors().empty());
    for (arnm_json_value *value : file.vectors()) {
        const std::string kind = contract::requiredString(value, "kind");
        arnm_json_value *expect = contract::requiredObject(value, "expect");
        uint8_t key[32];
        SCOPED_TRACE(contract::requiredString(value, "id"));

        if (kind == "community") {
            const std::vector<uint8_t> community = bytes(contract::requiredString(value, "communityKey"));
            ASSERT_EQ(community.size(), 32u);
            const uint32_t shard = sc_shard_of(community.data());
            EXPECT_EQ(static_cast<int64_t>(shard), contract::requiredDecimal(expect, "shard"));
            sc_shard_topic_key(shard, key);
            EXPECT_EQ(hex(key, 32), contract::requiredString(expect, "shardTopicKey"));
            sc_community_topic_key(community.data(), key);
            EXPECT_EQ(hex(key, 32), contract::requiredString(expect, "communityTopicKey"));
        } else if (kind == "topic") {
            sc_shard_topic_key(static_cast<uint32_t>(contract::requiredDecimal(value, "shard")), key);
            EXPECT_EQ(hex(key, 32), contract::requiredString(expect, "shardTopicKey"));
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
