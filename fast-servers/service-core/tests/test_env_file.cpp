/*
 * The `.env` file. service_core/env_file.h holds the design; this file holds what a test can
 * decide about it.
 *
 * What is worth checking is the agreement, and it has two halves. Reading has to mean what
 * dotenv means -- the three quoting forms, the comment, the `export`, and the rule that the
 * environment wins -- because a file written by the reference implementation's setup is read
 * here. Writing has to leave everything it was given no answer for exactly where it stood,
 * because that is what makes `setup` safe to run twice against a file an operator has
 * commented.
 *
 * The values that go round trip below are the same ones
 * packages/backend/src/setup/envFile.test.ts sends through dotenv, so the two suites are
 * measuring one agreement rather than two implementations.
 *
 * C++ because googletest is; see the note at the top of test_cache.cpp.
 */
#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <string>

extern "C" {
#include "service_core/env_file.h"
#include "service_core/log/log.h"
#include "service_core/log/logger.h"
}

namespace
{

/* One file per test, in the working directory the test binary was started in. */
constexpr const char *kPath = "test_env_file.tmp";

class EnvFileTest : public ::testing::Test
{
  protected:
    void TearDown() override
    {
        (void)std::remove(kPath);
        for (const char *name : {"SC_TEST_ONE", "SC_TEST_TWO", "SC_TEST_QUOTED"})
            unsetenv(name);
    }

    static void given(const std::string &text)
    {
        FILE *file = fopen(kPath, "wb");
        ASSERT_NE(file, nullptr);
        (void)fwrite(text.data(), 1, text.size(), file);
        (void)fclose(file);
    }

    static std::string written()
    {
        FILE *file = fopen(kPath, "rb");
        std::string text;
        char chunk[4096];
        size_t read;

        if (file == nullptr)
            return text;
        while ((read = fread(chunk, 1, sizeof(chunk), file)) != 0)
            text.append(chunk, read);
        (void)fclose(file);
        return text;
    }

    static std::string value(const char *name)
    {
        const char *found = getenv(name);
        return found == nullptr ? std::string("(unset)") : std::string(found);
    }
};

/* ---------------------------------------------------------------- *
 * reading
 * ---------------------------------------------------------------- */

TEST_F(EnvFileTest, NoFileIsNotAFailure)
{
    /* Not being configured by a file is an ordinary way for a deployment to be configured. */
    EXPECT_EQ(sc_env_file_load("no-such-file.env", nullptr, 0), SC_OK);
}

TEST_F(EnvFileTest, ReadsTheThreeFormsTheWriterWrites)
{
    given("SC_TEST_ONE=plain\n"
          "SC_TEST_TWO='a # b \"c\"'\n"
          "SC_TEST_QUOTED=\"line one\\nline two\"\n");

    ASSERT_EQ(sc_env_file_load(kPath, nullptr, 0), SC_OK);
    EXPECT_EQ(value("SC_TEST_ONE"), "plain");
    /* Nothing is unescaped inside single quotes, which is what lets them carry a `"`. */
    EXPECT_EQ(value("SC_TEST_TWO"), "a # b \"c\"");
    /* And only \n and \r inside double quotes, which is all dotenv unescapes there. */
    EXPECT_EQ(value("SC_TEST_QUOTED"), "line one\nline two");
}

TEST_F(EnvFileTest, IgnoresComments_BlankLines_AndTheExportPrefix)
{
    given("# a comment\n"
          "\n"
          "export SC_TEST_ONE=exported\n"
          "SC_TEST_TWO=bare # and a trailing comment\n");

    ASSERT_EQ(sc_env_file_load(kPath, nullptr, 0), SC_OK);
    EXPECT_EQ(value("SC_TEST_ONE"), "exported");
    EXPECT_EQ(value("SC_TEST_TWO"), "bare");
}

TEST_F(EnvFileTest, TheEnvironmentWins)
{
    /* systemd, docker and a shell all set variables that a file in the working directory must
     * not be able to override. */
    setenv("SC_TEST_ONE", "from the environment", 1);
    given("SC_TEST_ONE=from the file\n");

    ASSERT_EQ(sc_env_file_load(kPath, nullptr, 0), SC_OK);
    EXPECT_EQ(value("SC_TEST_ONE"), "from the environment");
}

TEST_F(EnvFileTest, RefusesALineThatIsNotAnAssignment)
{
    char error[256] = {0};

    given("SC_TEST_ONE=fine\nthis is not an assignment\n");

    EXPECT_EQ(sc_env_file_load(kPath, error, sizeof(error)), SC_ERR_MALFORMED);
    EXPECT_NE(error[0], '\0');
}

/* ---------------------------------------------------------------- *
 * writing
 * ---------------------------------------------------------------- */

TEST_F(EnvFileTest, WritesAFileThatWasNotThere)
{
    const sc_env_entry entries[] = {{"DB_TYPE", "sqlite"}, {"EMAIL", "false"}};

    ASSERT_EQ(sc_env_file_write(kPath, entries, 2, nullptr, 0), SC_OK);
    EXPECT_EQ(written(), "# written by the setup command\nDB_TYPE=sqlite\nEMAIL=false\n");
}

TEST_F(EnvFileTest, ReplacesAVariableWhereItStands)
{
    const sc_env_entry entries[] = {{"DB_TYPE", "postgresql"}};

    given("# the database\nDB_TYPE=sqlite\nBACKEND_PORT=4000\n");

    ASSERT_EQ(sc_env_file_write(kPath, entries, 1, nullptr, 0), SC_OK);
    /* The comment an operator wrote above it stays attached to it, and the variable this
     * command had no answer for is untouched. */
    EXPECT_EQ(written(), "# the database\nDB_TYPE=postgresql\nBACKEND_PORT=4000\n");
}

TEST_F(EnvFileTest, ACommentedOutVariableIsNotTheOneThatGetsTheAnswer)
{
    const sc_env_entry entries[] = {{"EMAIL", "false"}};

    given("# EMAIL=true\n");

    ASSERT_EQ(sc_env_file_write(kPath, entries, 1, nullptr, 0), SC_OK);
    EXPECT_EQ(written(), "# EMAIL=true\n\n# written by the setup command\nEMAIL=false\n");
}

TEST_F(EnvFileTest, QuotesAValueTheUnquotedFormWouldChange)
{
    const sc_env_entry entries[] = {{"EMAIL_PASSWORD", "a # b \"c\""},
                                    {"EMAIL_SENDER", "dev@gradido.localhost"}};

    ASSERT_EQ(sc_env_file_write(kPath, entries, 2, nullptr, 0), SC_OK);
    EXPECT_NE(written().find("EMAIL_PASSWORD='a # b \"c\"'"), std::string::npos);
    /* An ordinary value stays bare, because that is what every line of .env.dist looks like. */
    EXPECT_NE(written().find("EMAIL_SENDER=dev@gradido.localhost"), std::string::npos);
}

TEST_F(EnvFileTest, RefusesAValueNoQuotingCarries)
{
    const sc_env_entry entries[] = {{"EMAIL_PASSWORD", "both ' and \""}};
    char error[256] = {0};

    EXPECT_EQ(sc_env_file_write(kPath, entries, 1, error, sizeof(error)), SC_ERR_MALFORMED);
    EXPECT_NE(std::string(error).find("EMAIL_PASSWORD"), std::string::npos);
}

TEST_F(EnvFileTest, EveryValueSetupCanProduceSurvivesTheRoundTrip)
{
    static const char *const kValues[] = {"a # b",
                                          "  padded  ",
                                          "back\\slash",
                                          "literal \\n, not a newline",
                                          "a \"quoted\" word",
                                          "it's mine",
                                          "line one\nline two",
                                          "",
                                          "plain"};

    for (const char *original : kValues) {
        const sc_env_entry entries[] = {{"SC_TEST_QUOTED", original}};

        (void)std::remove(kPath);
        unsetenv("SC_TEST_QUOTED");
        ASSERT_EQ(sc_env_file_write(kPath, entries, 1, nullptr, 0), SC_OK) << original;
        ASSERT_EQ(sc_env_file_load(kPath, nullptr, 0), SC_OK) << original;
        EXPECT_EQ(value("SC_TEST_QUOTED"), std::string(original)) << "for " << original;
    }
}

} // namespace

int main(int argc, char **argv)
{
    /* Nothing here logs, and the logger is still started: sc_env_file_load's caller reports what
     * it refused, so a line arriving from anywhere under this would have nowhere to go. */
    sc_log_config log_cfg;
    int result;

    sc_log_default_config(&log_cfg);
    log_cfg.min_level = SC_LOG_ERROR;
    sc_log_init(&log_cfg);
    sc_log_thread_join();

    ::testing::InitGoogleTest(&argc, argv);
    result = RUN_ALL_TESTS();

    sc_log_thread_leave();
    sc_log_shutdown();
    return result;
}
