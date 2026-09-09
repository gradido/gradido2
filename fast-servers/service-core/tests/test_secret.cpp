/*
 * A secret out of the best source that has it.
 *
 * The resolution order is `contracts/secrets.json`, and the reference path asserts the same
 * cases in `packages/service-core/src/config/secret.test.ts`. When one of these moves the other
 * has to move with it: an operator who has learned the mechanism on one binary has learned it on
 * both, or the mechanism is not one.
 *
 * The `.env` file is a different question and is tested in test_env_file.cpp -- what it provides
 * is the last of the three sources here and never overrides a real environment entry.
 *
 * C++ because googletest is; see the note at the top of test_cache.cpp.
 */
#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <string>

extern "C" {
#include "service_core/secret.h"
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

constexpr const char *kVariables[] = {"DB_PASSWORD",     "DB_PASSWORD_FILE",
                                      "EMAIL_PASSWORD",  "EMAIL_PASSWORD_FILE",
                                      "CREDENTIALS_DIRECTORY", "SC_SECRET_TEST"};

class SecretTest : public ::testing::Test
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

TEST_F(SecretTest, TakesTheVariableWhenNothingElseNamesASource)
{
    sc_test_putenv("DB_PASSWORD", "from-env");
    EXPECT_EQ(secret(), "from-env");
}

TEST_F(SecretTest, AFileTheEnvironmentNamesBeatsTheVariable)
{
    sc_test_putenv("DB_PASSWORD", "from-env");
    sc_test_putenv("DB_PASSWORD_FILE", write("pw", "from-file").c_str());
    EXPECT_EQ(secret(), "from-file");
}

TEST_F(SecretTest, ASystemdCredentialBeatsBoth)
{
    write("DB_PASSWORD", "from-credential");
    sc_test_putenv("DB_PASSWORD", "from-env");
    sc_test_putenv("DB_PASSWORD_FILE", write("pw", "from-file").c_str());
    sc_test_putenv("CREDENTIALS_DIRECTORY", dir_.c_str());
    EXPECT_EQ(secret(), "from-credential");
}

TEST_F(SecretTest, ACredentialsDirectoryWithoutThisCredentialFallsThrough)
{
    /* A unit loads the credentials it needs and no others, so an absent file is ordinary. */
    sc_test_putenv("DB_PASSWORD", "from-env");
    sc_test_putenv("CREDENTIALS_DIRECTORY", dir_.c_str());
    EXPECT_EQ(secret(), "from-env");
}

TEST_F(SecretTest, EveryDeclaredSecretResolvesTheSameWay)
{
    /* The mechanism is one mechanism or it is not one: an operator who has learned it on
     * DB_PASSWORD has learned it on EMAIL_PASSWORD. contracts/secrets.json lists both, and this
     * walks the same three sources for each of them rather than trusting that a second caller
     * was wired the same way as the first. */
    for (const char *name : {"DB_PASSWORD", "EMAIL_PASSWORD"}) {
        const std::string file_variable = std::string(name) + "_FILE";

        sc_test_putenv(name, "from-env");
        EXPECT_EQ(secret(name), "from-env") << name;

        sc_test_putenv(file_variable.c_str(),
                       write(std::string(name) + ".pw", "from-file").c_str());
        EXPECT_EQ(secret(name), "from-file") << name;

        write(name, "from-credential");
        sc_test_putenv("CREDENTIALS_DIRECTORY", dir_.c_str());
        EXPECT_EQ(secret(name), "from-credential") << name;

        sc_test_unsetenv("CREDENTIALS_DIRECTORY");
        sc_test_unsetenv(file_variable.c_str());
        sc_test_unsetenv(name);
    }
}

TEST_F(SecretTest, SaysWhichSourceAnsweredWithoutReadingIt)
{
    /* `setup` writes the answers it collects into the `.env`. For a secret that comes from
     * anywhere but the environment that would be wrong twice: the prompt has no value to offer,
     * and the empty line it writes becomes the password once the credential is taken away. So
     * setup asks this first and leaves the stronger two alone. */
    EXPECT_EQ(sc_secret_source_of("DB_PASSWORD"), SC_SECRET_NOWHERE);

    sc_test_putenv("DB_PASSWORD", "x");
    EXPECT_EQ(sc_secret_source_of("DB_PASSWORD"), SC_SECRET_ENVIRONMENT);

    /* A named file that is not there is still the file source: it is a configured source that
     * is broken, and sc_secret_read is the one to complain about it. */
    sc_test_putenv("DB_PASSWORD_FILE", (dir_ + "/not-there").c_str());
    EXPECT_EQ(sc_secret_source_of("DB_PASSWORD"), SC_SECRET_FILE);

    write("DB_PASSWORD", "from-credential");
    sc_test_putenv("CREDENTIALS_DIRECTORY", dir_.c_str());
    EXPECT_EQ(sc_secret_source_of("DB_PASSWORD"), SC_SECRET_CREDENTIAL);

    /* A credentials directory without this credential is not a credential source. */
    EXPECT_EQ(sc_secret_source_of("EMAIL_PASSWORD"), SC_SECRET_NOWHERE);
}

TEST_F(SecretTest, TheSourceItReportsIsTheSourceItReadsFrom)
{
    /* The two walks must not drift: whichever one says wins has to be the one that answered. */
    write("DB_PASSWORD", "from-credential");
    sc_test_putenv("DB_PASSWORD", "from-env");
    sc_test_putenv("DB_PASSWORD_FILE", write("pw", "from-file").c_str());
    sc_test_putenv("CREDENTIALS_DIRECTORY", dir_.c_str());

    EXPECT_EQ(sc_secret_source_of("DB_PASSWORD"), SC_SECRET_CREDENTIAL);
    EXPECT_EQ(secret(), "from-credential");
}

TEST_F(SecretTest, ACredentialThatIsThereButUnreadableIsFatal)
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

TEST_F(SecretTest, OneTrailingLineEndingGoesAndNothingElse)
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

TEST_F(SecretTest, AnEmptyFileIsAnAnswerNotAnAbsence)
{
    sc_test_putenv("DB_PASSWORD", "from-env");
    sc_test_putenv("DB_PASSWORD_FILE", write("e", "").c_str());
    EXPECT_EQ(secret(), "");
}

TEST_F(SecretTest, ANamedFileThatCannotBeReadIsFatalRatherThanAFallback)
{
    /* The rule the whole order stands on: a silent fallback turns an unreadable secret into an
     * empty one, and an empty password is how a process connects as somebody else. */
    char out[256] = {0};
    sc_test_putenv("DB_PASSWORD", "from-env");
    sc_test_putenv("DB_PASSWORD_FILE", (dir_ + "/nope").c_str());
    EXPECT_EQ(sc_secret_read("DB_PASSWORD", out, sizeof(out)), SC_ERR_UNAVAILABLE);
}

TEST_F(SecretTest, ASecretNobodyConfiguredIsEmptyAndNotAnError)
{
    char out[256] = {'x', '\0'};
    EXPECT_EQ(sc_secret_read("DB_PASSWORD", out, sizeof(out)), SC_OK);
    EXPECT_STREQ(out, "");
}

TEST_F(SecretTest, AValueThatDoesNotFitIsRefusedRatherThanCut)
{
    char out[8] = {0};
    sc_test_putenv("DB_PASSWORD", "much longer than eight");
    EXPECT_EQ(sc_secret_read("DB_PASSWORD", out, sizeof(out)), SC_ERR_TOO_LONG);
}

} // namespace

int main(int argc, char **argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
