/*
 * The database connection. service_core/db.h holds the design; this file holds the parts of it
 * a test can decide without a database.
 *
 * Most of that is the configuration, and it is worth more than it looks: the defaults here are
 * not chosen, they are the TypeScript path's defaults in
 * `packages/backend-core/src/database/schema.ts`, and backend and federation reaching the same
 * database from two implementations is exactly what a drifting default would end. So the
 * values are written out literally below rather than compared against a constant -- a constant
 * that moved would move the test with it and prove nothing.
 *
 * The rest is refusal, which needs no database either: a port nothing listens on refuses a
 * connection in microseconds, which is what makes the "not yet" / "not like this" decision
 * testable offline. SQLite is a file and an in-memory database, so it is tested for real.
 *
 * What no test here decides is whether a statement against either database returns what it
 * should. There is no statement yet -- see db.h, *What this surface is not* -- and when there
 * is, it arrives with the generated row mapping and is tested against both databases, which is
 * `../../Architecture.md`, *DB*: tests run against both database modes.
 *
 * C++ because googletest is; see the note at the top of test_cache.cpp.
 */
#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

extern "C" {
#include "service_core/db.h"
#include "service_core/log.h"
}

namespace
{

/* A port nothing listens on, so a connection fails at connect rather than at a timeout. The
 * mail tests use the same address for the same reason. */
constexpr const char *kDeadHost = "127.0.0.1";
constexpr const char *kDeadPort = "1";

/* Every variable sc_db_config_load reads, plus the one it consults for the production rule. */
constexpr const char *kVariables[] = {"DB_TYPE",     "DB_HOST",     "DB_PORT", "DB_USER",
                                      "DB_PASSWORD", "DB_DATABASE", "DB_FILE", "NODE_ENV"};

void set_env(const char *name, const char *value)
{
#if defined(_WIN32)
    (void)_putenv_s(name, value);
#else
    (void)setenv(name, value, 1);
#endif
}

void clear_env(const char *name)
{
#if defined(_WIN32)
    (void)_putenv_s(name, "");
#else
    (void)unsetenv(name);
#endif
}

class DbConfigTest : public ::testing::Test
{
  protected:
    /* The environment is process-wide and these tests write to it, so each of them starts from
     * a known one rather than from whatever the previous test left. */
    void SetUp() override
    {
        for (const char *name : kVariables)
            clear_env(name);
    }

    void TearDown() override
    {
        for (const char *name : kVariables)
            clear_env(name);
    }
};

TEST_F(DbConfigTest, DefaultsAreTheTypeScriptDefaults)
{
    sc_db_config config{};

    ASSERT_EQ(sc_db_config_load(&config), SC_OK);
    EXPECT_EQ(config.kind, SC_DB_POSTGRESQL);
    EXPECT_STREQ(config.host, "localhost");
    EXPECT_EQ(config.port, 5432);
    EXPECT_STREQ(config.user, "gradido");
    EXPECT_STREQ(config.password, "");
    EXPECT_STREQ(config.database, "gradido_community");
    EXPECT_STREQ(config.file, "./gradido_community.sqlite");
}

TEST_F(DbConfigTest, ReadsEveryVariable)
{
    sc_db_config config{};

    set_env("DB_TYPE", "sqlite");
    set_env("DB_HOST", "/var/run/postgresql");
    set_env("DB_PORT", "6543");
    set_env("DB_USER", "someone");
    set_env("DB_PASSWORD", "a password with spaces");
    set_env("DB_DATABASE", "another");
    set_env("DB_FILE", "/tmp/somewhere.sqlite");

    ASSERT_EQ(sc_db_config_load(&config), SC_OK);
    EXPECT_EQ(config.kind, SC_DB_SQLITE);
    EXPECT_STREQ(config.host, "/var/run/postgresql");
    EXPECT_EQ(config.port, 6543);
    EXPECT_STREQ(config.user, "someone");
    EXPECT_STREQ(config.password, "a password with spaces");
    EXPECT_STREQ(config.database, "another");
    EXPECT_STREQ(config.file, "/tmp/somewhere.sqlite");
}

TEST_F(DbConfigTest, AnEmptyTypeIsTheDefault)
{
    sc_db_config config{};

    set_env("DB_TYPE", "");
    ASSERT_EQ(sc_db_config_load(&config), SC_OK);
    EXPECT_EQ(config.kind, SC_DB_POSTGRESQL);
}

TEST_F(DbConfigTest, AThirdDatabaseIsRefused)
{
    sc_db_config config{};

    /* Not "close enough": a name that is neither of the two is a configuration that would
     * otherwise start against the wrong database. */
    set_env("DB_TYPE", "mysql");
    EXPECT_EQ(sc_db_config_load(&config), SC_ERR_MALFORMED);

    set_env("DB_TYPE", "PostgreSQL");
    EXPECT_EQ(sc_db_config_load(&config), SC_ERR_MALFORMED);
}

TEST_F(DbConfigTest, APortThatIsNotAPortIsRefused)
{
    sc_db_config config{};

    set_env("DB_PORT", "5432x");
    EXPECT_EQ(sc_db_config_load(&config), SC_ERR_MALFORMED);
    set_env("DB_PORT", "0");
    EXPECT_EQ(sc_db_config_load(&config), SC_ERR_MALFORMED);
    set_env("DB_PORT", "70000");
    EXPECT_EQ(sc_db_config_load(&config), SC_ERR_MALFORMED);
}

TEST_F(DbConfigTest, AValueThatWouldNotFitIsRefusedRatherThanTruncated)
{
    sc_db_config config{};

    /* Half a host name is a host name, and it is the wrong one. */
    set_env("DB_HOST", std::string(SC_DB_HOST_MAX, 'h').c_str());
    EXPECT_EQ(sc_db_config_load(&config), SC_ERR_TOO_LONG);
}

TEST_F(DbConfigTest, AnEmptyPasswordIsRefusedInProduction)
{
    sc_db_config config{};

    /* The same rule, from the same variable, as the TypeScript path: "an empty database
     * password is not acceptable in production". */
    set_env("NODE_ENV", "production");
    EXPECT_EQ(sc_db_config_load(&config), SC_ERR_MALFORMED);

    set_env("DB_PASSWORD", "something");
    EXPECT_EQ(sc_db_config_load(&config), SC_OK);
}

TEST_F(DbConfigTest, AnEmptyPasswordIsFineOutsideProductionAndForSqlite)
{
    sc_db_config config{};

    EXPECT_EQ(sc_db_config_load(&config), SC_OK);

    set_env("NODE_ENV", "development");
    EXPECT_EQ(sc_db_config_load(&config), SC_OK);

    /* SQLite has no password to be empty. */
    set_env("NODE_ENV", "production");
    set_env("DB_TYPE", "sqlite");
    EXPECT_EQ(sc_db_config_load(&config), SC_OK);
}

TEST(DbKindTest, NamesAreTheContractSpelling)
{
    /* contracts/logging.json, startup.server.started: "postgresql | sqlite". These strings are
     * compared across implementations, so they are not free to be prettier. */
    EXPECT_STREQ(sc_db_kind_name(SC_DB_POSTGRESQL), "postgresql");
    EXPECT_STREQ(sc_db_kind_name(SC_DB_SQLITE), "sqlite");
}

TEST(DbKindTest, DriversAgreeWithWhatIsAvailable)
{
    const std::string drivers = sc_db_drivers();
    const bool postgres = sc_db_kind_available(SC_DB_POSTGRESQL) != 0;
    const bool sqlite = sc_db_kind_available(SC_DB_SQLITE) != 0;

    EXPECT_EQ(drivers.find("postgresql") != std::string::npos, postgres);
    EXPECT_EQ(drivers.find("sqlite") != std::string::npos, sqlite);
    EXPECT_EQ(drivers == "none", !postgres && !sqlite);
}

/** Whichever driver this build left out has to say so rather than fail somewhere else. */
TEST(DbOpenTest, ADriverThisBuildLacksAnswersUnavailable)
{
    sc_db_config config{};
    sc_db *db = nullptr;

    config.kind = SC_DB_POSTGRESQL;
    if (sc_db_kind_available(config.kind)) {
        config.kind = SC_DB_SQLITE;
    }
    if (sc_db_kind_available(config.kind)) {
        GTEST_SKIP() << "this build has both drivers; build with -Dpostgres=false to cover this";
    }
    EXPECT_EQ(sc_db_open(&config, &db), SC_ERR_UNAVAILABLE);
    EXPECT_EQ(db, nullptr);
}

class SqliteTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        if (!sc_db_kind_available(SC_DB_SQLITE))
            GTEST_SKIP() << "this build has no SQLite driver";
    }

    static sc_db_config in_memory()
    {
        sc_db_config config{};
        config.kind = SC_DB_SQLITE;
        std::snprintf(config.file, sizeof(config.file), ":memory:");
        return config;
    }
};

TEST_F(SqliteTest, OpensAndAnswers)
{
    sc_db_config config = in_memory();
    sc_db *db = nullptr;

    ASSERT_EQ(sc_db_open(&config, &db), SC_OK);
    ASSERT_NE(db, nullptr);
    EXPECT_EQ(sc_db_kind_of(db), SC_DB_SQLITE);
    /* The sqlite3 * a repository will cast and write statements against. */
    EXPECT_NE(sc_db_native(db), nullptr);
    EXPECT_STREQ(sc_db_error(db), "");
    EXPECT_EQ(sc_db_probe(db), SC_OK);
    sc_db_close(db);
}

TEST_F(SqliteTest, CreatesTheFileAndTakesASecondConnection)
{
    const std::string path = std::string(::testing::TempDir()) + "fs_test_db.sqlite";
    sc_db_config config{};
    sc_db *first = nullptr;
    sc_db *second = nullptr;

    std::remove(path.c_str());
    std::remove((path + "-wal").c_str());
    std::remove((path + "-shm").c_str());

    config.kind = SC_DB_SQLITE;
    std::snprintf(config.file, sizeof(config.file), "%s", path.c_str());

    /* A community's first start has no file yet, which is what download-and-start means. */
    ASSERT_EQ(sc_db_open(&config, &first), SC_OK);
    /* And WAL is what lets the second one read while the first writes. */
    ASSERT_EQ(sc_db_open(&config, &second), SC_OK);
    EXPECT_EQ(sc_db_probe(first), SC_OK);
    EXPECT_EQ(sc_db_probe(second), SC_OK);

    sc_db_close(second);
    sc_db_close(first);

    EXPECT_EQ(std::remove(path.c_str()), 0) << "the database file was not created at " << path;
    std::remove((path + "-wal").c_str());
    std::remove((path + "-shm").c_str());
}

TEST_F(SqliteTest, ADirectoryThatDoesNotExistIsRefusedWithoutRetrying)
{
    sc_db_config config{};
    sc_db *db = nullptr;

    config.kind = SC_DB_SQLITE;
    std::snprintf(config.file, sizeof(config.file), "/nonexistent-fs-test/somewhere.sqlite");
    /* Not SC_ERR_NETWORK: there is no second party, so nothing here gets better by waiting --
     * which is also why sc_db_open_waiting must come back at once rather than after 30 s. */
    EXPECT_EQ(sc_db_open(&config, &db), SC_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(db, nullptr);

    config.connect_attempts = 30;
    config.connect_delay_ms = 1000;
    const auto started = std::chrono::steady_clock::now();
    EXPECT_EQ(sc_db_open_waiting(&config, nullptr, &db), SC_ERR_INVALID_ARGUMENT);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 1000);
}

class PostgresTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        if (!sc_db_kind_available(SC_DB_POSTGRESQL))
            GTEST_SKIP() << "this build has no PostgreSQL driver";
    }

    static sc_db_config unreachable()
    {
        sc_db_config config{};
        config.kind = SC_DB_POSTGRESQL;
        std::snprintf(config.host, sizeof(config.host), "%s", kDeadHost);
        config.port = static_cast<uint16_t>(std::atoi(kDeadPort));
        std::snprintf(config.user, sizeof(config.user), "gradido");
        std::snprintf(config.database, sizeof(config.database), "gradido_community");
        return config;
    }
};

/**
 * The one test here that talks to a database, and it skips rather than passes without one --
 * a green test that connected to nothing says nothing. Point it at a server:
 *
 *   SC_DB_TEST_PG_HOST=/var/run/postgresql SC_DB_TEST_PG_DATABASE=gradido \
 *       ./zig-out/bin/test_db
 *
 * A socket directory rather than a host name is the deployment Architecture.md prescribes, and
 * the case worth covering: it is also the one where an empty password is correct, because peer
 * authentication has already established who is asking.
 */
TEST_F(PostgresTest, ConnectsToARealDatabase)
{
    const char *host = std::getenv("SC_DB_TEST_PG_HOST");
    if (host == nullptr)
        GTEST_SKIP() << "set SC_DB_TEST_PG_HOST to a reachable PostgreSQL";

    const char *port = std::getenv("SC_DB_TEST_PG_PORT");
    const char *user = std::getenv("SC_DB_TEST_PG_USER");
    const char *password = std::getenv("SC_DB_TEST_PG_PASSWORD");
    const char *database = std::getenv("SC_DB_TEST_PG_DATABASE");

    sc_db_config config{};
    sc_db *db = nullptr;

    config.kind = SC_DB_POSTGRESQL;
    std::snprintf(config.host, sizeof(config.host), "%s", host);
    config.port = static_cast<uint16_t>(port != nullptr ? std::atoi(port) : 5432);
    std::snprintf(config.user, sizeof(config.user), "%s",
                  user != nullptr                  ? user
                  : std::getenv("USER") != nullptr ? std::getenv("USER")
                                                   : "gradido");
    std::snprintf(config.password, sizeof(config.password), "%s",
                  password != nullptr ? password : "");
    std::snprintf(config.database, sizeof(config.database), "%s",
                  database != nullptr ? database : "postgres");

    ASSERT_EQ(sc_db_open_waiting(&config, nullptr, &db), SC_OK) << "connecting to " << host;
    ASSERT_NE(db, nullptr);
    EXPECT_EQ(sc_db_kind_of(db), SC_DB_POSTGRESQL);
    /* The PGconn * a repository will cast and write statements against. */
    EXPECT_NE(sc_db_native(db), nullptr);
    EXPECT_EQ(sc_db_probe(db), SC_OK) << sc_db_error(db);
    sc_db_close(db);
}

/**
 * The other half of the classification, and the half only a real server can answer: a database
 * that is *there* and refuses is not worth waiting for.
 *
 * libpq exposes no SQLSTATE for a connection failure, so db_postgres.c asks PQping instead --
 * see classify_failure() there. This is the test that the substitution actually holds: the
 * server is up, the database does not exist, and the answer must be the permanent one rather
 * than half a minute of retries that could not have helped.
 */
TEST_F(PostgresTest, ADatabaseThatDoesNotExistIsNotWaitedFor)
{
    const char *host = std::getenv("SC_DB_TEST_PG_HOST");
    if (host == nullptr)
        GTEST_SKIP() << "set SC_DB_TEST_PG_HOST to a reachable PostgreSQL";

    const char *port = std::getenv("SC_DB_TEST_PG_PORT");
    sc_db_config config{};
    sc_db *db = nullptr;

    config.kind = SC_DB_POSTGRESQL;
    std::snprintf(config.host, sizeof(config.host), "%s", host);
    config.port = static_cast<uint16_t>(port != nullptr ? std::atoi(port) : 5432);
    std::snprintf(config.user, sizeof(config.user), "%s",
                  std::getenv("USER") != nullptr ? std::getenv("USER") : "gradido");
    std::snprintf(config.database, sizeof(config.database), "no_such_database_fs_test");

    config.connect_attempts = 30;
    config.connect_delay_ms = 1000;
    const auto started = std::chrono::steady_clock::now();
    EXPECT_EQ(sc_db_open_waiting(&config, nullptr, &db), SC_ERR_INVALID_ARGUMENT);
    const auto elapsed = std::chrono::steady_clock::now() - started;

    EXPECT_EQ(db, nullptr);
    EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 1000)
        << "a database that does not exist was retried";
}

TEST_F(PostgresTest, ADatabaseThatDoesNotAnswerIsNotYet)
{
    sc_db_config config = unreachable();
    sc_db *db = nullptr;

    /* Nothing is listening, so this is the case waiting is for -- SC_ERR_NETWORK is what
     * sc_db_open_waiting retries on, and everything else is what it gives up on. */
    EXPECT_EQ(sc_db_open(&config, &db), SC_ERR_NETWORK);
    EXPECT_EQ(db, nullptr);
}

TEST_F(PostgresTest, WaitingGivesUpAfterTheAttemptsItWasGiven)
{
    sc_db_config config = unreachable();
    sc_db *db = nullptr;

    config.connect_attempts = 3;
    config.connect_delay_ms = 10;

    const auto started = std::chrono::steady_clock::now();
    EXPECT_EQ(sc_db_open_waiting(&config, nullptr, &db), SC_ERR_NETWORK);
    const auto elapsed = std::chrono::steady_clock::now() - started;

    EXPECT_EQ(db, nullptr);
    /* Three attempts and two pauses. The upper bound is loose because a refused connection is
     * fast but not instant; what it is here to catch is a loop that ignored connect_attempts. */
    EXPECT_LT(std::chrono::duration_cast<std::chrono::seconds>(elapsed).count(), 10);
}

} // namespace

int main(int argc, char **argv)
{
    /*
     * Quiet, for the reason test_mail gives: the refusal paths log a line each and would bury
     * the one line that says which assertion failed. The configuration failures are logged at
     * fatal and still appear -- they are refusals of a startup, and this is the level at which
     * that is not allowed to be silent. SC_DB_TEST_LOG turns the rest back on.
     */
    sc_log_init(std::getenv("SC_DB_TEST_LOG") != nullptr ? SC_LOG_DEBUG : SC_LOG_FATAL);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
