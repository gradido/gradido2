/*
 * The `.env` file: read into the environment at startup, written by the setup command.
 *
 * `../Architecture.md`, *Config*, puts what is needed before the database into the
 * environment, and a file in the working directory is how a developer and a small
 * installation put it there. The reference path reads it with dotenv, out of
 * `packages/backend/src/config/index.ts`; this reads the same file, so a `.env` written by
 * either implementation's `setup` serves the other. That is not a nicety: `Architecture.md`,
 * *One implementation per deployment*, has an operator switching between the two, and a
 * configuration that only one of them can read would make the switch a reconfiguration.
 *
 * ### The form, which is dotenv's
 *
 *   NAME=value            up to a `#` or the end of the line, then trimmed
 *   NAME='value'          taken exactly as it stands, newlines included
 *   NAME="value"          the same, except that \n and \r become those characters
 *   export NAME=value     the prefix is ignored
 *   # anything            a comment, and so is a blank line
 *
 * Nothing else is unescaped inside quotes, because dotenv unescapes nothing else -- so a
 * value carrying a `'` is written in the double-quoted form and one carrying a `"` in the
 * single-quoted form. A value that would need both is refused by the writer rather than
 * written as something that reads back differently.
 *
 * ### The environment wins
 *
 * A variable that is already set is left alone, which is dotenv's rule and the one a
 * deployment depends on: systemd, docker and a shell all set variables that must not be
 * overridden by a file somebody left in the working directory.
 */
#ifndef SERVICE_CORE_ENV_FILE_H
#define SERVICE_CORE_ENV_FILE_H

#include <stddef.h>

#include "service_core/status.h"

/** The file, in the working directory -- which is where dotenv looks on the reference path,
 *  and therefore where a `.env` written by either implementation's setup is found. */
#define SC_ENV_FILE_NAME ".env"

/** One variable to write. Both strings are borrowed for the call. */
typedef struct sc_env_entry {
    const char *name;
    const char *value;
} sc_env_entry;

/** What a `.env` may weigh. Past this the file is refused rather than half read: a
 *  configuration read to the middle is worse than one not read at all. */
#define SC_ENV_FILE_MAX (64 * 1024)

/**
 * Reads @p path and sets every variable in it that the environment does not have already.
 *
 * A file that is not there is SC_OK and nothing else: not being configured by a file is the
 * ordinary case for a deployment that sets its environment some other way. SC_ERR_MALFORMED
 * for a line that is not an assignment, SC_ERR_TOO_LONG for a file past SC_ENV_FILE_MAX, and
 * in both cases @p error holds a sentence naming the line.
 *
 * Call it before anything reads the environment, from the one thread the process has at that
 * point: `setenv` is not thread safe against a concurrent `getenv`.
 */
sc_status sc_env_file_load(const char *path, char *error, size_t error_cap);

/**
 * Writes @p entries into @p path, keeping everything already in it.
 *
 * A variable that is already there is replaced where it stands, so the comments an operator
 * wrote around it stay attached to it; one that is not is appended under a heading. That is
 * what makes `setup` safe to run a second time: it is not a file generator, it is an editor
 * that only touches the lines it has an answer for.
 *
 * The file is written with mode 0600 where the host has such a thing, because it holds the
 * database password and the SMTP password.
 */
sc_status sc_env_file_write(const char *path, const sc_env_entry *entries, size_t count,
                            char *error, size_t error_cap);

/** Puts @p name into the environment, replacing what is there. What `setup` does with its
 *  own answers, so that the ordinary loaders read them without a second path. */
sc_status sc_env_set(const char *name, const char *value);

#endif /* SERVICE_CORE_ENV_FILE_H */
