/*
 * A `.env` file into the environment, and a secret out of the best source that has it.
 *
 * contracts/secrets.json is normative for the second half; this file is its C reading.
 */
#include "service_core/env.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "service_core/log/log.h"

#if defined(_WIN32)
#include <windows.h>
#define sc_setenv(name, value) (_putenv_s((name), (value)) == 0 ? 0 : -1)
#else
#include <stdlib.h>
#define sc_setenv(name, value) setenv((name), (value), 0)
#endif

/*
 * Reads a whole file into @p out.
 *
 * One trailing "\n" or "\r\n" goes, and nothing else -- contracts/secrets.json,
 * resolution.rules: `echo secret > file` writes a newline that is not part of the secret, while
 * trimming further would make a password that ends in a space impossible to configure.
 *
 * A file that does not fit is SC_ERR_TOO_LONG rather than a truncated secret.
 *
 * @p absent separates the two ways a read fails, and the separation is the contract's: a file
 * that is not there is an ordinary answer -- a unit loads the credentials it needs and no
 * others -- while a file that *is* there and cannot be read is a configured source that did not
 * deliver, and falling back from it would turn an unreadable secret into an empty one. fopen
 * reports both as NULL, so errno is what tells them apart, read immediately and once.
 */
static sc_status read_whole_file(const char *path, char *out, size_t out_size, int *absent)
{
    FILE *file;
    size_t read;

    *absent = 0;
    errno = 0;
    file = fopen(path, "rb");
    if (file == NULL) {
        *absent = (errno == ENOENT);
        return SC_ERR_UNAVAILABLE;
    }
    read = fread(out, 1, out_size, file);
    if (read == out_size && fgetc(file) != EOF) {
        fclose(file);
        return SC_ERR_TOO_LONG;
    }
    fclose(file);

    if (read != 0 && out[read - 1] == '\n')
        --read;
    if (read != 0 && out[read - 1] == '\r')
        --read;
    out[read] = '\0';
    return SC_OK;
}

/* Everything after the first '=' , with one matching pair of quotes removed. */
static void unquote(char *value)
{
    size_t len = strlen(value);
    char quote;

    if (len < 2)
        return;
    quote = value[0];
    if ((quote != '"' && quote != '\'') || value[len - 1] != quote)
        return;
    memmove(value, value + 1, len - 2);
    value[len - 2] = '\0';
}

static char *skip_blanks(char *at)
{
    while (*at == ' ' || *at == '\t')
        ++at;
    return at;
}

/* Trailing blanks off a key, so `KEY = value` names KEY and not "KEY ". */
static void trim_end(char *text)
{
    size_t len = strlen(text);
    while (len != 0 && (text[len - 1] == ' ' || text[len - 1] == '\t'))
        text[--len] = '\0';
}

sc_status sc_env_load_file(const char *path)
{
    char line[SC_ENV_LINE_MAX];
    FILE *file;
    unsigned number = 0;

    if (path == NULL)
        return SC_ERR_INVALID_ARGUMENT;
    file = fopen(path, "r");
    if (file == NULL)
        return SC_ERR_UNAVAILABLE;

    while (fgets(line, (int)sizeof(line), file) != NULL) {
        char *key;
        char *equals;
        char *value;

        ++number;
        line[strcspn(line, "\r\n")] = '\0';
        key = skip_blanks(line);
        if (*key == '\0' || *key == '#')
            continue;
        /* `export KEY=value` is what a file people also `source` looks like. */
        if (strncmp(key, "export ", 7) == 0)
            key = skip_blanks(key + 7);

        equals = strchr(key, '=');
        if (equals == NULL) {
            fclose(file);
            /* Before the logger is started, so this goes out synchronously -- which is what a
             * line about an unreadable configuration wants anyway. */
            sc_log_fatal(SC_CAT_STARTUP, "config.env_malformed",
                         "%s line %u is neither blank, a comment, nor KEY=VALUE", path, number);
            return SC_ERR_MALFORMED;
        }
        *equals = '\0';
        trim_end(key);
        value = skip_blanks(equals + 1);
        unquote(value);
        if (*key == '\0') {
            fclose(file);
            sc_log_fatal(SC_CAT_STARTUP, "config.env_malformed", "%s line %u has no name",
                         path, number);
            return SC_ERR_MALFORMED;
        }
        /* The 0 in sc_setenv is the whole point: a real environment entry wins. */
        (void)sc_setenv(key, value);
    }
    fclose(file);
    return SC_OK;
}

sc_status sc_secret_read(const char *name, char *out, size_t out_size)
{
    const char *credentials;
    const char *from_env;
    char path[SC_ENV_LINE_MAX];
    char file_variable[128];
    sc_status status;

    if (name == NULL || out == NULL || out_size == 0)
        return SC_ERR_INVALID_ARGUMENT;
    out[0] = '\0';

    /* 1. systemd. Unset everywhere that is not a systemd service, which is the platform check. */
    credentials = getenv("CREDENTIALS_DIRECTORY");
    if (credentials != NULL && credentials[0] != '\0') {
        int written = snprintf(path, sizeof(path), "%s/%s", credentials, name);
        if (written > 0 && (size_t)written < sizeof(path)) {
            int absent = 0;
            status = read_whole_file(path, out, out_size, &absent);
            if (status == SC_OK)
                return SC_OK;
            /* Absent is the ordinary case -- a unit loads the credentials it needs and no
             * others. Present and unreadable is not, and is the caller's problem to hear
             * about rather than to be quietly given something else. */
            if (!absent) {
                sc_log_fatal(SC_CAT_STARTUP, "config.secret_unreadable",
                             "the systemd credential %s could not be read", path);
                return status == SC_ERR_TOO_LONG ? SC_ERR_TOO_LONG : SC_ERR_UNAVAILABLE;
            }
        }
    }

    /* 2. A file the environment names. */
    if (snprintf(file_variable, sizeof(file_variable), "%s_FILE", name) > 0) {
        const char *named = getenv(file_variable);
        if (named != NULL && named[0] != '\0') {
            int absent = 0;
            /* No exemption for an absent one here: this path was named by a variable somebody
             * set, so "there is no such file" is a broken configuration and not a default. */
            status = read_whole_file(named, out, out_size, &absent);
            if (status == SC_OK)
                return SC_OK;
            sc_log_fatal(SC_CAT_STARTUP, "config.secret_unreadable",
                         "%s names %s, which could not be read -- refusing to fall back to %s",
                         file_variable, named, name);
            return status == SC_ERR_TOO_LONG ? SC_ERR_TOO_LONG : SC_ERR_UNAVAILABLE;
        }
    }

    /* 3. The variable itself. */
    from_env = getenv(name);
    if (from_env == NULL)
        return SC_OK;
    if (strlen(from_env) >= out_size)
        return SC_ERR_TOO_LONG;
    memcpy(out, from_env, strlen(from_env) + 1);
    return SC_OK;
}
