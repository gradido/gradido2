/*
 * A `.env` into the environment, and a secret out of the best source that has it.
 *
 * The resolution order is `contracts/secrets.json`, and the reference path asserts the same
 * cases in `packages/service-core/src/config/secret.test.ts`. When one of these moves the other
 * has to move with it: an operator who has learned the mechanism on one binary has learned it on
 * both, or the mechanism is not one.
 *
 * C++ because googletest is; see the note at the top of test_cache.cpp.
 */
#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <string>

extern "C" {
#include "service_core/env.h"
}

#if defined(_WIN32)
#include <direct.h>
#include <io.h>
#define sc_test_mkdir(p) _mkdir(p)
#define sc_test_unlink _unlink
#define sc_test_rmdir _rmdir
#define sc_test_putenv(name, value) (void)_putenv_s((name), (value))
#define sc_test_unsetenv(name) (void)_putenv_s((name), "")
#else
#include <sys/stat.h>
#include <unistd.h>
#define sc_test_mkdir(p) mkdir((p), 0700)
#define sc_test_unlink unlink
#define sc_test_rmdir rmdir
#define sc_test_putenv(name, value) (void)setenv((name), (value), 1)
#define sc_test_unsetenv(name) (void)unsetenv((name))
#endif

namespace
{

constexpr const char *kVariables[] = {"DB_PASSWORD", "DB_PASSWORD_FILE", "CREDENTIALS_DIRECTORY",
                                      "SC_ENV_TEST_A", "SC_ENV_TEST_B", "SC_ENV_TEST_QUOTED"};

class EnvTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        for (const char *name : kVariables)
            sc_test_unsetenv(name);
        dir_ = std::string("/tmp/sc_env_test_") + std::to_string(
#if defined(_WIN32)
            (long)_getpid()
#else
            (long)getpid()
#endif
        );
        sc_test_mkdir(dir_.c_str());
    }

    void TearDown() override
    {
        for (const std::string &path : written_)
            sc_test_unlink(path.c_str());
        sc_test_rmdir(dir_.c_str());
        for (const char *name : kVariables)
            sc_test_unsetenv(name);
    }

    /** A file with exactly these bytes -- no newline added, so a test says what it means. */
    std::string write(const std::string &name, const std::string &content)
    {
        const std::string path = dir_ + "/" + name;
        std::FILE *file = std::fopen(path.c_str(), "wb");
        if (file != nullptr) {
            (void)std::fwrite(content.data(), 1, content.size(), file);
            std::fclose(file);
        }
        written_.push_back(path);
        return path;
    }

    std::string secret(const char *name = "DB_PASSWORD")
    {
        char out[256] = {0};
        EXPECT_EQ(sc_secret_read(name, out, sizeof(out)), SC_OK);
        return std::string(out);
    }

    std::string dir_;
    std::vector<std::string> written_;
};

/* ------------------------------------------------------------------ The order */

TEST_F(EnvTest, TakesTheVariableWhenNothingElseNamesASource)
{
    sc_test_putenv("DB_PASSWORD", "from-env");
    EXPECT_EQ(secret(), "from-env");
}

TEST_F(EnvTest, AFileTheEnvironmentNamesBeatsTheVariable)
{
    sc_test_putenv("DB_PASSWORD", "from-env");
    sc_test_putenv("DB_PASSWORD_FILE", write("pw", "from-file").c_str());
    EXPECT_EQ(secret(), "from-file");
}

TEST_F(EnvTest, ASystemdCredentialBeatsBoth)
{
    write("DB_PASSWORD", "from-credential");
    sc_test_putenv("DB_PASSWORD", "from-env");
    sc_test_putenv("DB_PASSWORD_FILE", write("pw", "from-file").c_str());
    sc_test_putenv("CREDENTIALS_DIRECTORY", dir_.c_str());
    EXPECT_EQ(secret(), "from-credential");
}

TEST_F(EnvTest, ACredentialsDirectoryWithoutThisCredentialFallsThrough)
{
    /* A unit loads the credentials it needs and no others, so an absent file is ordinary. */
    sc_test_putenv("DB_PASSWORD", "from-env");
    sc_test_putenv("CREDENTIALS_DIRECTORY", dir_.c_str());
    EXPECT_EQ(secret(), "from-env");
}

TEST_F(EnvTest, ACredentialThatIsThereButUnreadableIsFatal)
{
    /* The half of the rule that is easy to miss: fopen reports "no such file" and "not allowed
     * to read it" the same way, so an implementation that only checks for NULL falls silently
     * through a configured source. Absent is ordinary; present and unreadable is not. */
    char out[256] = {0};
    const std::string path = write("DB_PASSWORD", "from-credential");
    ASSERT_EQ(chmod(path.c_str(), 0), 0);

    sc_test_putenv("DB_PASSWORD", "from-env");
    sc_test_putenv("CREDENTIALS_DIRECTORY", dir_.c_str());
    const sc_status status = sc_secret_read("DB_PASSWORD", out, sizeof(out));

    /* Restored before the assertion so a failure still leaves a removable file behind. */
    (void)chmod(path.c_str(), 0600);
    EXPECT_EQ(status, SC_ERR_UNAVAILABLE);
}

TEST_F(EnvTest, OneTrailingLineEndingGoesAndNothingElse)
{
    sc_test_putenv("DB_PASSWORD_FILE", write("a", "secret\n").c_str());
    EXPECT_EQ(secret(), "secret");
    sc_test_putenv("DB_PASSWORD_FILE", write("b", "secret\r\n").c_str());
    EXPECT_EQ(secret(), "secret");
    /* Only one, and only at the end: a password may legitimately hold or end in whitespace, and
     * one nobody can express is worse than one that needs a careful printf. */
    sc_test_putenv("DB_PASSWORD_FILE", write("c", "secret\n\n").c_str());
    EXPECT_EQ(secret(), "secret\n");
    sc_test_putenv("DB_PASSWORD_FILE", write("d", " secret ").c_str());
    EXPECT_EQ(secret(), " secret ");
}

TEST_F(EnvTest, AnEmptyFileIsAnAnswerNotAnAbsence)
{
    sc_test_putenv("DB_PASSWORD", "from-env");
    sc_test_putenv("DB_PASSWORD_FILE", write("e", "").c_str());
    EXPECT_EQ(secret(), "");
}

TEST_F(EnvTest, ANamedFileThatCannotBeReadIsFatalRatherThanAFallback)
{
    /* The rule the whole order stands on: a silent fallback turns an unreadable secret into an
     * empty one, and an empty password is how a process connects as somebody else. */
    char out[256] = {0};
    sc_test_putenv("DB_PASSWORD", "from-env");
    sc_test_putenv("DB_PASSWORD_FILE", (dir_ + "/nope").c_str());
    EXPECT_EQ(sc_secret_read("DB_PASSWORD", out, sizeof(out)), SC_ERR_UNAVAILABLE);
}

TEST_F(EnvTest, ASecretNobodyConfiguredIsEmptyAndNotAnError)
{
    char out[256] = {'x', '\0'};
    EXPECT_EQ(sc_secret_read("DB_PASSWORD", out, sizeof(out)), SC_OK);
    EXPECT_STREQ(out, "");
}

TEST_F(EnvTest, AValueThatDoesNotFitIsRefusedRatherThanCut)
{
    char out[8] = {0};
    sc_test_putenv("DB_PASSWORD", "much longer than eight");
    EXPECT_EQ(sc_secret_read("DB_PASSWORD", out, sizeof(out)), SC_ERR_TOO_LONG);
}

/* ------------------------------------------------------------------ The .env file */

TEST_F(EnvTest, ReadsKeyValueLinesWithoutOverridingWhatIsSet)
{
    /* Not overriding is the whole behaviour, and the same rule bun applies: an exported
     * variable beats a stale line in a checkout. */
    sc_test_putenv("SC_ENV_TEST_A", "from-environment");
    const std::string path = write("dotenv", "SC_ENV_TEST_A=from-file\nSC_ENV_TEST_B=b\n");

    EXPECT_EQ(sc_env_load_file(path.c_str()), SC_OK);
    EXPECT_STREQ(getenv("SC_ENV_TEST_A"), "from-environment");
    EXPECT_STREQ(getenv("SC_ENV_TEST_B"), "b");
}

TEST_F(EnvTest, SkipsBlanksAndCommentsAndUnwrapsQuotes)
{
    const std::string path = write("dotenv2",
                                   "\n"
                                   "  # a comment\n"
                                   "export SC_ENV_TEST_A = spaced\n"
                                   "SC_ENV_TEST_QUOTED=\"quoted # not a comment\"\n");

    EXPECT_EQ(sc_env_load_file(path.c_str()), SC_OK);
    EXPECT_STREQ(getenv("SC_ENV_TEST_A"), "spaced");
    /* A '#' inside a value is a character: this is a list of values, not a shell. */
    EXPECT_STREQ(getenv("SC_ENV_TEST_QUOTED"), "quoted # not a comment");
}

TEST_F(EnvTest, AnAbsentFileIsOrdinaryAndAMalformedLineIsNot)
{
    EXPECT_EQ(sc_env_load_file((dir_ + "/no-such-file").c_str()), SC_ERR_UNAVAILABLE);
    EXPECT_EQ(sc_env_load_file(write("bad", "this is not an assignment\n").c_str()),
              SC_ERR_MALFORMED);
}

} // namespace

int main(int argc, char **argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
