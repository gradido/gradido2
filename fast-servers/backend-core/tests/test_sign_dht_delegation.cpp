/*
 * The community vouching for this instance's dht node, against a real database: the signing key
 * is read out of the row the setup wrote. The reference is
 * packages/backend-core/src/domain/community/interactions/sign-dht-delegation.test.ts. What the
 * delegation's bytes are for a given key is contracts/test-vectors/master-seed.json.
 *
 * SQLite, on a file that goes away with the fixture, as in test_register_account.cpp.
 *
 * C++ because googletest is; see the note at the top of service-core/tests/test_cache.cpp.
 */
#include <gtest/gtest.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <unistd.h>

extern "C" {
#include "backend_core/database/migrations.h"
#include "backend_core/domain/community.h"
#include "service_core/dht_delegation.h"
#include "service_core/master_seed.h"
}

namespace
{

class SignDhtDelegation : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        sc_db_config config = {};
        uint32_t from = 0;

        (void)snprintf(path_, sizeof(path_), "gradido_delegation_test_%d.sqlite", (int)getpid());
        config.kind = SC_DB_SQLITE;
        (void)snprintf(config.file, sizeof(config.file), "%s", path_);
        ASSERT_EQ(sc_db_open(&config, &db_), SC_OK);
        ASSERT_EQ(bc_migrations_run(db_, &from), SC_OK);
        std::memset(master_seed_, 7, sizeof(master_seed_));
    }

    void TearDown() override
    {
        sc_db_close(db_);
        const std::string path = path_;
        (void)std::remove(path.c_str());
        (void)std::remove((path + "-wal").c_str());
        (void)std::remove((path + "-shm").c_str());
    }

    bc_home_community create()
    {
        bc_home_community_setup setup = {};
        bc_home_community home = {};
        char error[BC_SQL_ERROR_MAX] = {0};
        (void)snprintf(setup.name, sizeof(setup.name), "%s", "Gradido Test");
        (void)snprintf(setup.url, sizeof(setup.url), "%s", "https://gdd.example.org");
        EXPECT_EQ(bc_create_home_community(db_, &setup, &home, error, sizeof(error)), SC_OK)
            << error;
        return home;
    }

    sc_db *db_ = nullptr;
    char path_[128] = {0};
    uint8_t master_seed_[SC_MASTER_SEED_BYTES] = {0};
};

TEST_F(SignDhtDelegation, TheHomeCommunityVouchesForTheNodeTheMasterSeedDerives)
{
    const bc_home_community home = create();
    uint8_t delegation[SC_DHT_DELEGATION_BYTES];
    uint8_t node_seed[32];
    uint8_t node_key[32];
    char error[BC_SQL_ERROR_MAX] = {0};

    ASSERT_EQ(bc_sign_dht_delegation_for(db_, master_seed_, delegation, error, sizeof(error)), SC_OK)
        << error;
    ASSERT_EQ(sc_master_seed_derive_dht(master_seed_, node_seed, node_key), SC_OK);
    EXPECT_EQ(std::memcmp(delegation, node_key, 32), 0);
    EXPECT_EQ(std::memcmp(delegation + 32, home.public_key, 32), 0);
    for (int i = 64; i != 72; ++i)
        EXPECT_EQ(delegation[i], 0) << "never expires";
}

TEST_F(SignDhtDelegation, SigningTwiceGivesTheSameBytes)
{
    (void)create();
    uint8_t first[SC_DHT_DELEGATION_BYTES];
    uint8_t second[SC_DHT_DELEGATION_BYTES];
    char error[BC_SQL_ERROR_MAX] = {0};

    ASSERT_EQ(bc_sign_dht_delegation_for(db_, master_seed_, first, error, sizeof(error)), SC_OK);
    ASSERT_EQ(bc_sign_dht_delegation_for(db_, master_seed_, second, error, sizeof(error)), SC_OK);
    EXPECT_EQ(std::memcmp(first, second, sizeof(first)), 0);
}

TEST_F(SignDhtDelegation, WithoutAHomeCommunityThereIsNothingToSignWith)
{
    uint8_t delegation[SC_DHT_DELEGATION_BYTES];
    char error[BC_SQL_ERROR_MAX] = {0};

    EXPECT_EQ(bc_sign_dht_delegation_for(db_, master_seed_, delegation, error, sizeof(error)),
              SC_ERR_UNAVAILABLE);
    EXPECT_NE(std::string(error).find("no home community"), std::string::npos) << error;
}

} // namespace

int main(int argc, char **argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
