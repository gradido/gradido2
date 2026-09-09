/*
 * A secret out of the best source that has it. contracts/secrets.json is normative; this file is
 * its C reading.
 *
 * The `.env` half of this used to live here too and does not any more: service_core/env_file.h
 * reads that file, writes it for `setup`, and knows dotenv's quoting rules -- one reader for one
 * format, and the reference path reads the same file with dotenv itself.
 */
#include "service_core/secret.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "service_core/log/log.h"

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

sc_secret_source sc_secret_source_of(const char *name)
{
    const char *credentials;
    char file_variable[128];

    if (name == NULL)
        return SC_SECRET_NOWHERE;

    credentials = getenv("CREDENTIALS_DIRECTORY");
    if (credentials != NULL && credentials[0] != '\0') {
        char path[SC_SECRET_PATH_MAX];
        int written = snprintf(path, sizeof(path), "%s/%s", credentials, name);
        if (written > 0 && (size_t)written < sizeof(path)) {
            /* Opened and closed rather than stat'ed: whether this process can read it is the
             * question, and a mode nobody may read is not a source in force. */
            FILE *file = fopen(path, "rb");
            if (file != NULL) {
                fclose(file);
                return SC_SECRET_CREDENTIAL;
            }
        }
    }

    if (snprintf(file_variable, sizeof(file_variable), "%s_FILE", name) > 0) {
        const char *named = getenv(file_variable);
        if (named != NULL && named[0] != '\0')
            return SC_SECRET_FILE;
    }

    return getenv(name) != NULL ? SC_SECRET_ENVIRONMENT : SC_SECRET_NOWHERE;
}

sc_status sc_secret_read(const char *name, char *out, size_t out_size)
{
    const char *credentials;
    const char *from_env;
    char path[SC_SECRET_PATH_MAX];
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
