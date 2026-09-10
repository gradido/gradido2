/*
 * Statements: every value type through both databases and back, the refusals a repository
 * decides on, and a cursor over more than one row.
 *
 * Each test runs against SQLite always and against PostgreSQL when SC_DB_TEST_PG_HOST names one
 * (test_db.cpp documents the variables). The tables are TEMP, so a test touches nothing but its
 * own session -- and the session is a fresh connection per test, so no prepared statement
 * survives from one test into the next unless a test keeps its connection on purpose.
 *
 * C++ because googletest is; see the note at the top of test_cache.cpp.
 */
#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <unistd.h>

extern "C" {
#include "service_core/db.h"
#include "service_core/log/log.h"
#include "service_core/log/logger.h"
#include "service_core/sql.h"
}

namespace
{

sc_sql_statement kCreatePg = SC_SQL_STATEMENT("test.create", nullptr, nullptr);

/* The same table in both dialects: the types the six value kinds are for. */
const char *kTablePostgresql = "CREATE TEMP TABLE value_types (i bigint, s text, b boolean, "
                               "ts timestamptz(3), bin bytea, n text, u text UNIQUE)";
const char *kTableSqlite = "CREATE TEMP TABLE value_types (i INTEGER, s TEXT, b INTEGER, "
                           "ts INTEGER, bin BLOB, n TEXT, u TEXT UNIQUE)";

sc_sql_statement kInsert = SC_SQL_STATEMENT("test.insert",
                                            "INSERT INTO value_types (i, s, b, ts, bin, n, u) "
                                            "VALUES ($1, $2, $3, $4, $5, $6, $7)",
                                            "INSERT INTO value_types (i, s, b, ts, bin, n, u) "
                                            "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7)");
sc_sql_statement kSelect =
    SC_SQL_STATEMENT("test.select", "SELECT i, s, b, ts, bin, n FROM value_types ORDER BY i",
                     "SELECT i, s, b, ts, bin, n FROM value_types ORDER BY i");
sc_sql_statement kUpdate =
    SC_SQL_STATEMENT("test.update", "UPDATE value_types SET n = $1 WHERE i >= $2",
                     "UPDATE value_types SET n = ?1 WHERE i >= ?2");

enum class Db { Sqlite, Postgresql };

std::string name_of(const ::testing::TestParamInfo<Db> &info)
{
    return info.param == Db::Sqlite ? "Sqlite" : "Postgresql";
}

class Sql : public ::testing::TestWithParam<Db>
{
  protected:
    void SetUp() override
    {
        sc_db_config config{};
        sc_sql_error error{};

        if (GetParam() == Db::Sqlite) {
            static int counter = 0;
            char name[256];

            (void)std::snprintf(name, sizeof(name), "gradido_sql_test_%d_%d.sqlite", (int)getpid(),
                                counter++);
            path_ = name;
            config.kind = SC_DB_SQLITE;
            (void)std::snprintf(config.file, sizeof(config.file), "%s", path_.c_str());
        } else {
            const char *host = std::getenv("SC_DB_TEST_PG_HOST");
            const char *port = std::getenv("SC_DB_TEST_PG_PORT");
            const char *user = std::getenv("SC_DB_TEST_PG_USER");
            const char *password = std::getenv("SC_DB_TEST_PG_PASSWORD");
            const char *database = std::getenv("SC_DB_TEST_PG_DATABASE");

            if (host == nullptr || !sc_db_kind_available(SC_DB_POSTGRESQL))
                GTEST_SKIP() << "set SC_DB_TEST_PG_HOST to a reachable PostgreSQL";
            config.kind = SC_DB_POSTGRESQL;
            (void)std::snprintf(config.host, sizeof(config.host), "%s", host);
            config.port = static_cast<uint16_t>(port != nullptr ? std::atoi(port) : 5432);
            (void)std::snprintf(config.user, sizeof(config.user), "%s",
                                user != nullptr ? user : "gradido");
            (void)std::snprintf(config.password, sizeof(config.password), "%s",
                                password != nullptr ? password : "");
            (void)std::snprintf(config.database, sizeof(config.database), "%s",
                                database != nullptr ? database : "postgres");
        }
        ASSERT_EQ(sc_db_open(&config, &db_), SC_OK);
        ASSERT_EQ(
            sc_sql_simple(db_, GetParam() == Db::Sqlite ? kTableSqlite : kTablePostgresql, &error),
            SC_OK)
            << error.message;
    }

    void TearDown() override
    {
        sc_db_close(db_);
        if (!path_.empty())
            for (const char *suffix : {"", "-wal", "-shm"})
                (void)std::remove((path_ + suffix).c_str());
    }

    sc_status insert(int64_t i, const char *unique, sc_sql_error *error)
    {
        const uint8_t bytes[4] = {0, 1, 0x7f, 0xff};
        sc_sql_param params[7] = {sc_sql_int(i),
                                  sc_sql_text("Ümlaut ✓"),
                                  sc_sql_bool(1),
                                  sc_sql_time(1789024070494),
                                  sc_sql_bytes(bytes, sizeof(bytes)),
                                  sc_sql_null(),
                                  sc_sql_text(unique)};

        return sc_sql_exec(db_, &kInsert, params, 7, nullptr, error);
    }

    sc_db *db_ = nullptr;
    std::string path_;
};

/* Every value kind goes in as what it is and comes out as the same value -- whatever the two
 * databases store it as underneath. The integer is past 2^53, which is where a double would
 * have started rounding it. */
TEST_P(Sql, EveryValueComesBackAsItWent)
{
    sc_sql_error error{};
    sc_sql_rows rows{};
    uint8_t bytes[8] = {};
    const int64_t big = (int64_t{1} << 53) + 7;

    ASSERT_EQ(insert(big, "a", &error), SC_OK) << error.message;
    ASSERT_EQ(sc_sql_query(db_, &kSelect, nullptr, 0, &rows, &error), SC_OK) << error.message;
    ASSERT_TRUE(sc_sql_next(&rows));

    EXPECT_EQ(sc_sql_col_int(&rows, 0), big);
    uint32_t size = 0;
    const char *text = sc_sql_col_text(&rows, 1, &size);
    EXPECT_EQ(std::string(text, size), "Ümlaut ✓");
    EXPECT_EQ(sc_sql_col_bool(&rows, 2), 1);
    EXPECT_EQ(sc_sql_col_time(&rows, 3), 1789024070494);
    ASSERT_EQ(sc_sql_col_bytes(&rows, 4, bytes, sizeof(bytes)), 4);
    EXPECT_EQ(bytes[0], 0);
    EXPECT_EQ(bytes[2], 0x7f);
    EXPECT_EQ(bytes[3], 0xff);
    EXPECT_TRUE(sc_sql_col_is_null(&rows, 5));
    EXPECT_FALSE(sc_sql_col_is_null(&rows, 1));

    EXPECT_FALSE(sc_sql_next(&rows));
    EXPECT_EQ(sc_sql_close(&rows), SC_OK);
}

/* A slice of a larger buffer is its own length, not the rest of the buffer -- which is what a
 * value cut out of a request body looks like. */
TEST_P(Sql, AnUnterminatedTextIsItsLengthAndNoMore)
{
    const char buffer[] = "gradido-and-then-some";
    sc_sql_param params[7] = {sc_sql_int(1),       sc_sql_textn(buffer, 7), sc_sql_bool(0),
                              sc_sql_time(0),      sc_sql_null(),           sc_sql_null(),
                              sc_sql_text("slice")};
    sc_sql_error error{};
    sc_sql_rows rows{};
    uint32_t size = 0;

    ASSERT_EQ(sc_sql_exec(db_, &kInsert, params, 7, nullptr, &error), SC_OK) << error.message;
    ASSERT_EQ(sc_sql_query(db_, &kSelect, nullptr, 0, &rows, &error), SC_OK);
    ASSERT_TRUE(sc_sql_next(&rows));
    const char *text = sc_sql_col_text(&rows, 1, &size);
    EXPECT_EQ(std::string(text, size), "gradido");
    EXPECT_EQ(sc_sql_col_bool(&rows, 2), 0);
    EXPECT_EQ(sc_sql_close(&rows), SC_OK);
}

/* The refusal a repository decides on, and the same kind on both databases. PostgreSQL names
 * the constraint and SQLite the column, and both spellings contain the column's name. */
TEST_P(Sql, AUniqueViolationSaysSoAndNamesTheColumn)
{
    sc_sql_error error{};

    ASSERT_EQ(insert(1, "same", &error), SC_OK) << error.message;
    EXPECT_EQ(insert(2, "same", &error), SC_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(error.kind, SC_SQL_ERROR_UNIQUE);
    EXPECT_NE(std::string(error.constraint).find('u'), std::string::npos) << error.constraint;
    EXPECT_NE(error.message[0], '\0');

    /* Refused and not broken: the next statement on the same connection works. On PostgreSQL
     * outside a transaction there is nothing to roll back; on SQLite the statement was reset. */
    EXPECT_EQ(insert(3, "other", &error), SC_OK) << error.message;
}

TEST_P(Sql, CountsWhatAStatementChanged)
{
    sc_sql_error error{};
    int64_t changes = -1;

    for (int64_t i = 1; i <= 5; ++i)
        ASSERT_EQ(insert(i, std::to_string(i).c_str(), &error), SC_OK) << error.message;

    sc_sql_param params[2] = {sc_sql_text("touched"), sc_sql_int(3)};
    ASSERT_EQ(sc_sql_exec(db_, &kUpdate, params, 2, &changes, &error), SC_OK) << error.message;
    EXPECT_EQ(changes, 3);
}

/* Several rows, in order, and the statement is usable again once the cursor is closed -- and
 * again while one is left open, which ends the open one rather than failing. */
TEST_P(Sql, WalksEveryRowAndRunsAgainAfterwards)
{
    sc_sql_error error{};
    sc_sql_rows rows{};
    std::vector<int64_t> seen;

    for (int64_t i = 1; i <= 4; ++i)
        ASSERT_EQ(insert(i, std::to_string(i).c_str(), &error), SC_OK) << error.message;

    ASSERT_EQ(sc_sql_query(db_, &kSelect, nullptr, 0, &rows, &error), SC_OK);
    while (sc_sql_next(&rows))
        seen.push_back(sc_sql_col_int(&rows, 0));
    EXPECT_EQ(sc_sql_close(&rows), SC_OK);
    EXPECT_EQ(seen, (std::vector<int64_t>{1, 2, 3, 4}));

    ASSERT_EQ(sc_sql_query(db_, &kSelect, nullptr, 0, &rows, &error), SC_OK);
    ASSERT_TRUE(sc_sql_next(&rows));
    sc_sql_rows again{};
    ASSERT_EQ(sc_sql_query(db_, &kSelect, nullptr, 0, &again, &error), SC_OK) << error.message;
    ASSERT_TRUE(sc_sql_next(&again));
    EXPECT_EQ(sc_sql_col_int(&again, 0), 1);
    EXPECT_EQ(sc_sql_close(&again), SC_OK);
    EXPECT_EQ(sc_sql_close(&rows), SC_OK);
}

/* A statement is prepared once per session and then only executed. On PostgreSQL the session
 * can be asked what it has prepared: one entry for a statement run a thousand times. */
TEST_P(Sql, PreparesOnceAndRunsMany)
{
    static sc_sql_statement kCount =
        SC_SQL_STATEMENT("test.count", "SELECT count(*) FROM value_types WHERE i > $1",
                         "SELECT count(*) FROM value_types WHERE i > ?1");
    static sc_sql_statement kPrepared = SC_SQL_STATEMENT(
        "test.prepared", "SELECT count(*) FROM pg_prepared_statements WHERE statement LIKE $1",
        nullptr);
    sc_sql_error error{};

    for (int i = 0; i != 1000; ++i) {
        sc_sql_param params[1] = {sc_sql_int(i)};
        sc_sql_rows rows{};

        ASSERT_EQ(sc_sql_query(db_, &kCount, params, 1, &rows, &error), SC_OK) << error.message;
        ASSERT_TRUE(sc_sql_next(&rows));
        EXPECT_EQ(sc_sql_close(&rows), SC_OK);
    }
    if (GetParam() != Db::Postgresql)
        return;

    sc_sql_param like[1] = {sc_sql_text("SELECT count(*) FROM value_types WHERE i > %")};
    sc_sql_rows rows{};
    ASSERT_EQ(sc_sql_query(db_, &kPrepared, like, 1, &rows, &error), SC_OK) << error.message;
    ASSERT_TRUE(sc_sql_next(&rows));
    EXPECT_EQ(sc_sql_col_int(&rows, 0), 1);
    EXPECT_EQ(sc_sql_close(&rows), SC_OK);
}

/*
 * A cursor that has answered "no more rows" keeps answering it. SQLite would otherwise start a
 * finished statement from the beginning on the next step and hand out its rows a second time --
 * a difference between the two databases that only shows in the one caller that asks twice.
 */
TEST_P(Sql, ACursorAtItsEndStaysThere)
{
    static sc_sql_statement kNone =
        SC_SQL_STATEMENT("test.none", "SELECT i FROM value_types WHERE i < 0",
                         "SELECT i FROM value_types WHERE i < 0");
    sc_sql_error error{};
    sc_sql_rows rows{};

    ASSERT_EQ(insert(1, "only", &error), SC_OK) << error.message;

    ASSERT_EQ(sc_sql_query(db_, &kSelect, nullptr, 0, &rows, &error), SC_OK) << error.message;
    ASSERT_TRUE(sc_sql_next(&rows));
    EXPECT_FALSE(sc_sql_next(&rows));
    EXPECT_FALSE(sc_sql_next(&rows)) << "the one row came round again";
    EXPECT_FALSE(sc_sql_next(&rows));
    EXPECT_EQ(sc_sql_close(&rows), SC_OK);

    /* No rows at all: the end is the first answer, and it stays the answer. */
    ASSERT_EQ(sc_sql_query(db_, &kNone, nullptr, 0, &rows, &error), SC_OK) << error.message;
    EXPECT_FALSE(sc_sql_next(&rows));
    EXPECT_FALSE(sc_sql_next(&rows));
    EXPECT_EQ(sc_sql_close(&rows), SC_OK);
}

/*
 * The failure the cursor must not hide: a statement whose rows are fine until one is not.
 *
 * abs() of the smallest bigint does not fit a bigint, on either database. SQLite produces rows as
 * they are stepped, so the first row arrives and the second fails -- and sc_sql_next answers 0
 * for that exactly as it would for the end. sc_sql_close is what tells them apart, and it has to:
 * a caller that took the one row it saw for the whole result is the bug this exists to prevent.
 * PostgreSQL computes the result before handing any of it over and says so at sc_sql_query.
 */
TEST_P(Sql, ARowThatFailsPartWayIsNotTheEndOfTheRows)
{
    static sc_sql_statement kAbs = SC_SQL_STATEMENT("test.abs", "SELECT abs(i) FROM value_types",
                                                    "SELECT abs(i) FROM value_types");
    sc_sql_error error{};
    sc_sql_rows rows{};

    ASSERT_EQ(insert(1, "fine", &error), SC_OK) << error.message;
    ASSERT_EQ(insert(INT64_MIN, "overflows", &error), SC_OK) << error.message;

    const sc_status status = sc_sql_query(db_, &kAbs, nullptr, 0, &rows, &error);
    if (GetParam() == Db::Postgresql) {
        EXPECT_NE(status, SC_OK);
        EXPECT_NE(error.message[0], '\0');
        return;
    }
    ASSERT_EQ(status, SC_OK) << error.message;
    ASSERT_TRUE(sc_sql_next(&rows));
    EXPECT_EQ(sc_sql_col_int(&rows, 0), 1);
    EXPECT_FALSE(sc_sql_next(&rows));
    EXPECT_FALSE(sc_sql_next(&rows)) << "a failed cursor answers no more rows";
    EXPECT_EQ(sc_sql_close(&rows), SC_ERR_INVALID_ARGUMENT);
    EXPECT_NE(std::string(error.message).find("overflow"), std::string::npos) << error.message;
}

/* A session that forgot its prepared statements, outside a transaction: nothing is broken but
 * the name, and the statement is prepared again and run, at once. */
TEST_P(Sql, ASessionThatForgotItsStatementsPreparesThemAgain)
{
    sc_sql_error error{};
    sc_sql_rows rows{};

    if (GetParam() != Db::Postgresql)
        GTEST_SKIP() << "only PostgreSQL sessions can be told to forget their statements";
    ASSERT_EQ(sc_sql_query(db_, &kSelect, nullptr, 0, &rows, &error), SC_OK) << error.message;
    EXPECT_EQ(sc_sql_close(&rows), SC_OK);
    ASSERT_EQ(sc_sql_simple(db_, "DEALLOCATE ALL", &error), SC_OK) << error.message;
    ASSERT_EQ(sc_sql_query(db_, &kSelect, nullptr, 0, &rows, &error), SC_OK) << error.message;
    EXPECT_EQ(sc_sql_close(&rows), SC_OK);
}

/*
 * A session that lost one statement and kept the others -- a DEALLOCATE of one name, a pooler
 * whose server connection knows some of them. Only the lost one is prepared again: preparing a
 * name the session still has is refused as a duplicate, and a table that had forgotten every
 * name would leave the survivors unusable on this connection for good.
 */
TEST_P(Sql, ASessionThatLostOneStatementKeepsTheOthers)
{
    static sc_sql_statement kNameOf = SC_SQL_STATEMENT(
        "test.name_of", "SELECT name FROM pg_prepared_statements WHERE statement = $1", nullptr);
    sc_sql_error error{};
    sc_sql_rows rows{};
    char name[64] = {};

    if (GetParam() != Db::Postgresql)
        GTEST_SKIP() << "only PostgreSQL sessions can be told to forget a statement";
    ASSERT_EQ(insert(1, "one", &error), SC_OK) << error.message;
    ASSERT_EQ(sc_sql_query(db_, &kSelect, nullptr, 0, &rows, &error), SC_OK) << error.message;
    EXPECT_EQ(sc_sql_close(&rows), SC_OK);

    /* Which name the session gave kSelect, and then that name alone taken away. */
    sc_sql_param text[1] = {sc_sql_text(kSelect.postgresql)};
    ASSERT_EQ(sc_sql_query(db_, &kNameOf, text, 1, &rows, &error), SC_OK) << error.message;
    ASSERT_TRUE(sc_sql_next(&rows));
    ASSERT_TRUE(sc_sql_col_copy(&rows, 0, name, sizeof(name)));
    EXPECT_EQ(sc_sql_close(&rows), SC_OK);
    ASSERT_EQ(sc_sql_simple(db_, (std::string("DEALLOCATE ") + name).c_str(), &error), SC_OK)
        << error.message;

    /* The lost one comes back; the one that was never lost -- the insert -- still works. */
    ASSERT_EQ(sc_sql_query(db_, &kSelect, nullptr, 0, &rows, &error), SC_OK) << error.message;
    EXPECT_EQ(sc_sql_close(&rows), SC_OK);
    EXPECT_EQ(insert(2, "two", &error), SC_OK) << error.message;
    EXPECT_EQ(insert(3, "three", &error), SC_OK) << error.message;
}

/* A statement with no text for the database it is run against is refused, not sent: a
 * repository that forgot one dialect finds out on its first run, with the statement named. */
TEST_P(Sql, AStatementWithoutThisDialectIsRefused)
{
    sc_sql_error error{};
    sc_sql_rows rows{};

    EXPECT_NE(sc_sql_query(db_, &kCreatePg, nullptr, 0, &rows, &error), SC_OK);
    EXPECT_EQ(error.kind, SC_SQL_ERROR_OTHER);
}

INSTANTIATE_TEST_SUITE_P(BothDatabases, Sql, ::testing::Values(Db::Sqlite, Db::Postgresql),
                         name_of);

} // namespace

int main(int argc, char **argv)
{
    sc_log_config log_cfg;
    int result;

    sc_log_default_config(&log_cfg);
    log_cfg.min_level = std::getenv("SC_DB_TEST_LOG") != nullptr ? SC_LOG_DEBUG : SC_LOG_FATAL;
    sc_log_init(&log_cfg);
    sc_log_thread_join();

    ::testing::InitGoogleTest(&argc, argv);
    result = RUN_ALL_TESTS();

    sc_log_thread_leave();
    sc_log_shutdown();
    return result;
}
