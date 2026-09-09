/*
 * The environment before anything reads it: a `.env` file, and where a secret comes from.
 *
 * Two things that are not the same and are here together because both answer the question "what
 * is in this variable" before any component asks it.
 *
 * `contracts/secrets.json` is normative for the second one. The short version is that the
 * environment may carry a *path* to a secret rather than the secret, and that the best source
 * carries no environment entry at all -- a value in the environment is readable in
 * /proc/<pid>/environ, is inherited by every child process, and turns up in `docker inspect`,
 * in crash dumps and in CI logs.
 */
#ifndef SERVICE_CORE_ENV_H
#define SERVICE_CORE_ENV_H

#include <stddef.h>

#include "service_core/status.h"

/** Longest line a `.env` file may carry, terminator included. */
#define SC_ENV_LINE_MAX 4096

/**
 * Reads `KEY=VALUE` lines from @p path into the environment, without overriding what is already
 * set.
 *
 * Not overriding is the whole behaviour: a real environment entry beats the file, so a
 * deployment that exports a variable is never surprised by a stale line in a checkout. It is
 * what bun does for the reference path, and the two have to agree or the same file configures
 * one of the two binaries.
 *
 * Blank lines and lines whose first non-blank character is `#` are skipped, as is a leading
 * `export `. A value may be wrapped in matching single or double quotes, which are removed;
 * everything else is taken literally, including `#` inside a value. No escape sequences are
 * interpreted -- a `.env` is a list of values and not a shell.
 *
 * Answers SC_OK when the file was read, SC_ERR_UNAVAILABLE when there is no such file -- which
 * is the ordinary case and not a failure -- and SC_ERR_MALFORMED for a line that is neither
 * blank, a comment, nor `KEY=VALUE`, having logged which line it was.
 */
sc_status sc_env_load_file(const char *path);

/**
 * The secret named @p name, from the best source that has it.
 *
 * In order, and the first that answers wins: `$CREDENTIALS_DIRECTORY/<name>` (systemd's
 * `LoadCredential=`, on a platform that sets it), the file named by `<name>_FILE`, then the
 * variable `<name>` itself. `contracts/secrets.json` holds the order and the reasoning.
 *
 * A source that is *named* and cannot be read is fatal rather than skipped: answers
 * SC_ERR_UNAVAILABLE, having logged which path it was. Falling back would turn an unreadable
 * secret into an empty one, and an empty password is how a process connects as somebody else.
 *
 * Answers SC_OK and writes an empty string when no source has it -- absence is not an error
 * here, because whether a secret is required is the caller's question. SC_ERR_TOO_LONG when the
 * value does not fit @p out_size, which is never truncated: half a password is not a password.
 *
 * There is no `#if defined(__linux__)` around the credential source. $CREDENTIALS_DIRECTORY is
 * simply unset everywhere else, and a check that is already there beats a platform macro.
 */
sc_status sc_secret_read(const char *name, char *out, size_t out_size);

#endif /* SERVICE_CORE_ENV_H */
