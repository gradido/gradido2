/*
 * Registration against a real database: the three ways the two rows can end, and what the
 * interaction does with each.
 *
 * The other user tests need no database and say so; this one needs one because what is under
 * test *is* the database's answer. Nothing here checks a value before writing it -- that is the
 * change these tests exist for -- so every property below is a unique index being asked at the
 * moment of the write, and asking it any other way would test something else.
 *
 * SQLite, on a file that goes away with the fixture. The PostgreSQL branch of the same code is
 * the other half of user_repository.c and is not reachable from here; it is exercised by running
 * the backend against a server, which is how the TypeScript path's equivalent branch was found
 * to be wrong.
 *
 * C++ because googletest is; see the note at the top of service-core/tests/test_cache.cpp.
 */
#include <gtest/gtest.h>

#include <cstdio>
#include <string>

extern "C" {
#include "backend_core/backend_core.h"
#include "backend_core/database/migrations.h"
#include "backend_core/domain/community.h"
#include "backend_core/domain/user.h"
#include "service_core/sql.h"
}

namespace
{

/** A migrated database with a home community, on one connection the tests run everything on. */
class Registration : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        sc_db_config config = {};
        bc_home_community_setup setup = {};
        char error[BC_SQL_ERROR_MAX] = {0};
        uint32_t from = 0;

        path_ = temporary_path();
        config.kind = SC_DB_SQLITE;
        (void)snprintf(config.file, sizeof(config.file), "%s", path_.c_str());
        ASSERT_EQ(sc_db_open(&config, &db_), SC_OK);
        ASSERT_EQ(bc_migrations_run(db_, &from), SC_OK);

        (void)snprintf(setup.name, sizeof(setup.name), "%s", "Gradido Test");
        (void)snprintf(setup.url, sizeof(setup.url), "%s", "https://gdd.example.org");
        ASSERT_EQ(bc_create_home_community(db_, &setup, &context_.home, error, sizeof(error)),
                  SC_OK)
            << error;
    }

    void TearDown() override
    {
        sc_db_close(db_);
        (void)std::remove(path_.c_str());
        (void)std::remove((path_ + "-wal").c_str());
        (void)std::remove((path_ + "-shm").c_str());
    }

    /** An account whose two generated values are the test's rather than the interaction's. */
    bc_new_account account(const char *email, const char *gradido_id, uint64_t code)
    {
        bc_new_account out = {};

        (void)snprintf(out.email, sizeof(out.email), "%s", email);
        (void)snprintf(out.first_name, sizeof(out.first_name), "%s", "Einhorn");
        (void)snprintf(out.last_name, sizeof(out.last_name), "%s", "Immond");
        (void)snprintf(out.language, sizeof(out.language), "%s", "de");
        (void)snprintf(out.gradido_id, sizeof(out.gradido_id), "%s", gradido_id);
        out.community_id = context_.home.id;
        out.email_verification_code = code;
        out.created_at = sc_now_ms();
        return out;
    }

    /** The repository under the rule a unit runs by: a transaction the executor opens, ended
     *  with COMMIT for a written account and ROLLBACK for everything else. */
    struct CreateUnit {
        sc_db_unit unit;
        const bc_new_account *account;
        bc_create_account_result *result;
        char *error;
        sc_status status;
    };

    static sc_db_end create_work(sc_db *db, sc_db_unit *unit)
    {
        CreateUnit *self = reinterpret_cast<CreateUnit *>(unit);

        self->status =
            bc_user_create_account(db, self->account, self->result, self->error, BC_SQL_ERROR_MAX);
        return self->status == SC_OK && self->result->outcome == BC_ACCOUNT_CREATED
                   ? SC_DB_COMMIT
                   : SC_DB_ROLLBACK;
    }

    sc_status create(const bc_new_account *account, bc_create_account_result *result, char *error)
    {
        CreateUnit u{};

        u.unit.access = SC_DB_WRITE;
        u.unit.work = create_work;
        u.account = account;
        u.result = result;
        u.error = error;
        EXPECT_EQ(sc_db_run(db_, &u.unit), SC_OK) << u.unit.error.message;
        return u.status;
    }

    /** Every member row, deleted or not -- the one thing a rollback that failed would leave. */
    int64_t member_rows()
    {
        static sc_sql_statement kCount = SC_SQL_STATEMENT(
            "test.users.count", "SELECT count(*) FROM users", "SELECT count(*) FROM users");
        sc_sql_rows rows{};
        sc_sql_error failure{};
        int64_t count = -1;

        EXPECT_EQ(sc_sql_query(db_, &kCount, nullptr, 0, &rows, &failure), SC_OK)
            << failure.message;
        if (sc_sql_next(&rows))
            count = sc_sql_col_int(&rows, 0);
        EXPECT_EQ(sc_sql_close(&rows), SC_OK) << failure.message;
        return count;
    }

    /** Whose an address is, or 0 when nobody's. */
    uint64_t owner_of(const char *email)
    {
        bc_address_owner owner = {};
        char error[BC_SQL_ERROR_MAX] = {0};
        int found = 0;

        EXPECT_EQ(bc_user_find_address_owner(db_, email, &owner, &found, error, sizeof(error)),
                  SC_OK)
            << error;
        return found ? owner.id : 0;
    }

    sc_status register_one(const char *email)
    {
        char error[BC_SQL_ERROR_MAX] = {0};

        last_error_ = "";
        /* The whole interaction under the executor's rules -- the transaction, AGAIN on a
         * collision, the report -- run right here on the test's connection, the way the setup
         * command and every tool without a server run a unit. */
        const sc_status status = bc_register_account_on(db_, &context_.home, "Einhorn", "Immond",
                                                        email, "de", error, sizeof(error));
        last_error_ = error;
        return status;
    }

    static std::string temporary_path()
    {
        static int counter = 0;
        char name[256];

        (void)snprintf(name, sizeof(name), "gradido_register_test_%d_%d.sqlite", (int)getpid(),
                       counter++);
        return name;
    }

    sc_db *db_ = nullptr;
    bc_context context_ = {};
    std::string path_;
    std::string last_error_;
};

TEST_F(Registration, WritesTheMemberAndTheirAddress)
{
    ASSERT_EQ(register_one("einhorn@gradido.net"), SC_OK) << last_error_;
    EXPECT_NE(owner_of("einhorn@gradido.net"), 0u);
}

/*
 * The silence rule, and the property that replaced the pre-check: nothing reads the address
 * before writing it, so what makes the second registration silent is `user_contacts_email_key`.
 * The answer is SC_OK either way -- the caller cannot tell, which is the whole point -- and the
 * member row the second attempt wrote before finding out goes away with the rollback.
 */
TEST_F(Registration, AnswersAnAddressThatIsTakenExactlyLikeANewOne)
{
    ASSERT_EQ(register_one("einhorn@gradido.net"), SC_OK) << last_error_;
    const uint64_t first = owner_of("einhorn@gradido.net");

    ASSERT_EQ(register_one("einhorn@gradido.net"), SC_OK) << last_error_;

    EXPECT_EQ(owner_of("einhorn@gradido.net"), first);
}

TEST_F(Registration, TellsTheAddressApartFromACollision)
{
    char error[BC_SQL_ERROR_MAX] = {0};
    bc_create_account_result result = {};
    bc_new_account first = account("erste@gradido.net", "11111111-1111-4111-8111-111111111111", 42);

    ASSERT_EQ(create(&first, &result, error), SC_OK) << error;
    ASSERT_EQ(result.outcome, BC_ACCOUNT_CREATED);
    const uint64_t owner = result.user_id;

    /* Same address, everything else fresh: an answer to give, not a draw to repeat. */
    bc_new_account again = account("erste@gradido.net", "22222222-2222-4222-8222-222222222222", 43);
    ASSERT_EQ(create(&again, &result, error), SC_OK) << error;
    EXPECT_EQ(result.outcome, BC_ACCOUNT_ADDRESS_TAKEN);
    EXPECT_EQ(result.taken_by, owner);
}

/*
 * The two generated values, each landing on one that exists. This is the branch that cannot be
 * reached by drawing -- 122 bits and 53 bits do not repeat -- so it is reached by handing the
 * repository a value it has already stored, which is the same thing the index sees.
 */
TEST_F(Registration, SaysWhichGeneratedValueCollided)
{
    char error[BC_SQL_ERROR_MAX] = {0};
    bc_create_account_result result = {};
    bc_new_account first = account("erste@gradido.net", "11111111-1111-4111-8111-111111111111", 42);

    ASSERT_EQ(create(&first, &result, error), SC_OK) << error;
    ASSERT_EQ(result.outcome, BC_ACCOUNT_CREATED);

    bc_new_account same_code =
        account("zweite@gradido.net", "22222222-2222-4222-8222-222222222222", 42);
    ASSERT_EQ(create(&same_code, &result, error), SC_OK) << error;
    EXPECT_EQ(result.outcome, BC_ACCOUNT_COLLIDED);
    EXPECT_NE(std::string(result.constraint).find("email_verification_code"), std::string::npos)
        << result.constraint;
    /* Rolled back whole: a collision leaves no member behind -- the users row the first INSERT
     * wrote goes with the transaction, not only the contact row that failed. */
    EXPECT_EQ(owner_of("zweite@gradido.net"), 0u);
    EXPECT_EQ(member_rows(), 1);

    bc_new_account same_id =
        account("dritte@gradido.net", "11111111-1111-4111-8111-111111111111", 44);
    ASSERT_EQ(create(&same_id, &result, error), SC_OK) << error;
    EXPECT_EQ(result.outcome, BC_ACCOUNT_COLLIDED);
    EXPECT_NE(std::string(result.constraint).find("gradido_id"), std::string::npos)
        << result.constraint;
}

/*
 * The retry loop, and the end of it.
 *
 * A test cannot make the CSPRNG repeat itself, so it makes the *index* refuse instead: one more
 * unique index over a column every registration fills the same way, and every draw is turned
 * down however fresh it is. That is what the interaction sees when a generator has stopped
 * generating, and it must stop rather than spin -- with an error, because a silent SC_OK would
 * tell somebody their registration went through.
 */
TEST_F(Registration, GivesUpRatherThanSpinningWhenEveryDrawIsRefused)
{
    char error[BC_SQL_ERROR_MAX] = {0};

    ASSERT_EQ(register_one("einhorn@gradido.net"), SC_OK) << last_error_;
    ASSERT_EQ(bc_sql_exec(db_, "CREATE UNIQUE INDEX one_member ON users (community_id)", error,
                          sizeof(error)),
              SC_OK)
        << error;

    EXPECT_EQ(register_one("zweite@gradido.net"), SC_ERR_UNAVAILABLE);
    EXPECT_NE(last_error_.find("no free generated values"), std::string::npos) << last_error_;
    EXPECT_EQ(owner_of("zweite@gradido.net"), 0u);
}

} // namespace

int main(int argc, char **argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
