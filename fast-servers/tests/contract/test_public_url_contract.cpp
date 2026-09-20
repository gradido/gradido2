/*
 * `contracts/test-vectors/public-url.json`, run against service_core/public_url.h. The other half
 * is `packages/contract-tests/src/public-url.test.ts`.
 *
 * C++ because googletest is; see the note at the top of service-core/tests/test_cache.cpp.
 */
#include <gtest/gtest.h>

#include <string>

extern "C" {
#include "service_core/public_url.h"
}

#include "vectors.hpp"

TEST(PublicUrlContract, EveryVector)
{
    static const contract::VectorFile file("public-url");
    ASSERT_FALSE(file.vectors().empty());
    for (arnm_json_value *value : file.vectors()) {
        const std::string url = contract::requiredString(value, "url");
        SCOPED_TRACE(contract::requiredString(value, "id") + " " + url);
        EXPECT_EQ(sc_url_is_public(url.c_str()) != 0,
                  contract::requiredBool(contract::requiredObject(value, "expect"), "public"));
    }
}

int main(int argc, char **argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
