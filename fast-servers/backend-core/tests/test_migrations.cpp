/*
 * The schema, as contracts/migrations defines it and backend-core runs it.
 *
 * What a test can decide here without a database is everything before the first statement is
 * sent: that the contract in the binary is the one the loader reads, and that a `.sql` file is
 * split into the statements it holds rather than at every semicolon it contains. The second one
 * is shared behaviour -- contracts/migrations/index.json states the rule for both
 * implementations -- so what is asserted is the rule, on inputs the migrations do not yet
 * contain, which is where a splitter goes wrong first.
 *
 * Applying a migration is not tested here. It needs a database, service-core's test_db is where
 * this build opens one, and the round trip that would prove a schema is the integration suite's
 * to make.
 *
 * C++ because googletest is; see the note at the top of service-core/tests/test_cache.cpp.
 */
#include <gtest/gtest.h>

#include <string>
#include <vector>

extern "C" {
#include "backend_core/database/contract_files.h"
#include "backend_core/database/migrations.h"
}

namespace
{

std::vector<std::string> split(const std::string &sql)
{
    std::vector<std::string> statements;
    char buffer[BC_SQL_STATEMENT_MAX];
    size_t pos = 0;

    while (bc_sql_split_next(sql.c_str(), sql.size(), &pos, buffer, sizeof(buffer)) == 1)
        statements.push_back(buffer);
    return statements;
}

TEST(SqlSplit, SeparatesOnSemicolons)
{
    const auto statements = split("CREATE TABLE a (id int);\nCREATE INDEX b ON a (id);\n");

    ASSERT_EQ(statements.size(), 2u);
    EXPECT_EQ(statements[0], "CREATE TABLE a (id int)");
    EXPECT_EQ(statements[1], "CREATE INDEX b ON a (id)");
}

TEST(SqlSplit, DropsCommentLinesAndTheTailAfterTheLastSemicolon)
{
    /* Every migration ends this way: a comment block, then statements, then a newline that is
     * not a statement. The tail is what a naive split on ';' would hand the driver. */
    const auto statements = split("-- what this does\n-- and why\nSELECT 1;\n\n");

    ASSERT_EQ(statements.size(), 1u);
    EXPECT_EQ(statements[0], "SELECT 1");
}

TEST(SqlSplit, KeepsACommentThatFollowsSql)
{
    /* Only a line that *starts* with -- is dropped, which is the rule the reference path
     * applies: a trailing comment is part of the statement's line and goes to the driver. */
    const auto statements = split("SELECT 1 -- why\n;");

    ASSERT_EQ(statements.size(), 1u);
    EXPECT_EQ(statements[0], "SELECT 1 -- why");
}

TEST(SqlSplit, ASemicolonInsideAStringIsNotAnEnd)
{
    const auto statements = split("INSERT INTO a VALUES ('x;y');SELECT 2;");

    ASSERT_EQ(statements.size(), 2u);
    EXPECT_EQ(statements[0], "INSERT INTO a VALUES ('x;y')");
    EXPECT_EQ(statements[1], "SELECT 2");
}

TEST(SqlSplit, AnEscapedQuoteDoesNotEndTheString)
{
    const auto statements = split("INSERT INTO a VALUES ('it''s; fine');SELECT 2;");

    ASSERT_EQ(statements.size(), 2u);
    EXPECT_EQ(statements[0], "INSERT INTO a VALUES ('it''s; fine')");
}

TEST(SqlSplit, ASemicolonInsideADollarQuotedBodyIsNotAnEnd)
{
    const auto statements =
        split("CREATE FUNCTION f() RETURNS int AS $$ BEGIN RETURN 1; END $$ LANGUAGE plpgsql;");

    ASSERT_EQ(statements.size(), 1u);
    EXPECT_NE(statements[0].find("RETURN 1;"), std::string::npos);
}

TEST(SqlSplit, ATaggedDollarBodyIsTheSame)
{
    const auto statements = split("SELECT $tag$ a;b $tag$;SELECT 2;");

    ASSERT_EQ(statements.size(), 2u);
    EXPECT_EQ(statements[0], "SELECT $tag$ a;b $tag$");
}

TEST(SqlSplit, ADollarThatIsNotATagIsOrdinaryText)
{
    /* `$1` is a PostgreSQL placeholder, not the start of a quoted body -- the tag has to begin
     * with a letter or an underscore. Reading it as one would swallow the rest of the file. */
    const auto statements = split("SELECT $1;SELECT 2;");

    ASSERT_EQ(statements.size(), 2u);
    EXPECT_EQ(statements[0], "SELECT $1");
}

TEST(SqlSplit, AStatementThatDoesNotFitIsRefusedRatherThanCut)
{
    const std::string sql(64, 'x');
    char small[16];
    size_t pos = 0;

    EXPECT_EQ(bc_sql_split_next(sql.c_str(), sql.size(), &pos, small, sizeof(small)), -1);
}

TEST(ContractFiles, CarryTheIndexAndTheSqlTheContractNames)
{
    ASSERT_NE(bc_contract_file_find("index.json"), nullptr);
    EXPECT_NE(bc_contract_file_find("0001_communities/up.sqlite.sql"), nullptr);
    EXPECT_EQ(bc_contract_file_find("0001_communities/nothing.sql"), nullptr);
}

/* The loader reads index.json rather than a list of its own, so both dialects have to come out
 * of the same file with the same versions -- which is what a second list would eventually stop
 * doing. */
TEST(Migrations, LoadTheContractForBothDialects)
{
    for (const auto kind : {SC_DB_SQLITE, SC_DB_POSTGRESQL}) {
        bc_migration_set set;
        char error[BC_SQL_ERROR_MAX];

        ASSERT_EQ(bc_migrations_load(kind, &set, error, sizeof(error)), SC_OK) << error;
        ASSERT_GE(set.count, 2u);
        EXPECT_EQ(set.items[0].version, 1u);
        EXPECT_STREQ(set.items[0].name, "0001_communities");
        EXPECT_EQ(set.items[1].version, 2u);
        EXPECT_STREQ(set.items[1].name, "0002_users");
        EXPECT_EQ(bc_migrations_schema_version(&set), set.items[set.count - 1].version);

        for (size_t i = 0; i != set.count; ++i) {
            EXPECT_NE(set.items[i].up, nullptr);
            EXPECT_GT(set.items[i].up_len, 0u);
        }
    }
}

/* The dialects are not interchangeable, and the loader picking the wrong one would produce a
 * database that is subtly not the contracted schema rather than an error. */
TEST(Migrations, PickTheSqlOfTheDialectTheyWereAskedFor)
{
    bc_migration_set sqlite;
    bc_migration_set postgres;
    char error[BC_SQL_ERROR_MAX];

    ASSERT_EQ(bc_migrations_load(SC_DB_SQLITE, &sqlite, error, sizeof(error)), SC_OK);
    ASSERT_EQ(bc_migrations_load(SC_DB_POSTGRESQL, &postgres, error, sizeof(error)), SC_OK);
    EXPECT_NE(sqlite.items[0].up, postgres.items[0].up);
    EXPECT_NE(std::string(sqlite.items[0].up).find("INTEGER PRIMARY KEY AUTOINCREMENT"),
              std::string::npos);
    EXPECT_NE(std::string(postgres.items[0].up).find("GENERATED ALWAYS AS IDENTITY"),
              std::string::npos);
}

/* Every statement of every migration has to come back out of the splitter, on both dialects.
 * The check is cheap and it is the one that notices a migration whose SQL the splitter cannot
 * carry -- a body too long for the buffer, or a quote it walks past the end of. */
TEST(Migrations, SplitIntoStatementsOnBothDialects)
{
    for (const auto kind : {SC_DB_SQLITE, SC_DB_POSTGRESQL}) {
        bc_migration_set set;
        char error[BC_SQL_ERROR_MAX];

        ASSERT_EQ(bc_migrations_load(kind, &set, error, sizeof(error)), SC_OK);
        for (size_t i = 0; i != set.count; ++i) {
            char buffer[BC_SQL_STATEMENT_MAX];
            size_t pos = 0;
            int statements = 0;
            int step;

            while ((step = bc_sql_split_next(set.items[i].up, set.items[i].up_len, &pos, buffer,
                                             sizeof(buffer))) == 1)
                ++statements;
            EXPECT_EQ(step, 0) << set.items[i].name;
            EXPECT_GT(statements, 0) << set.items[i].name;
        }
    }
}

} // namespace

int main(int argc, char **argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
