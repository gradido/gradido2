/*
 * Where a secret comes from.
 *
 * `contracts/secrets.json` is normative. The short version is that the environment may carry a
 * *path* to a secret rather than the secret, and that the best source carries no environment
 * entry at all -- a value in the environment is readable in /proc/<pid>/environ, is inherited by
 * every child process, and turns up in `docker inspect`, in crash dumps and in CI logs.
 *
 * This is not the `.env` reader. That is `service_core/env_file.h`, and it answers a different
 * question: which variables exist at all. By the time anything here is called, that file has
 * been read and what it provided is the last of the three sources below.
 */
#ifndef SERVICE_CORE_SECRET_H
#define SERVICE_CORE_SECRET_H

#include <stddef.h>

#include "service_core/status.h"

/** Longest path a credential or a `<NAME>_FILE` may name. */
#define SC_SECRET_PATH_MAX 4096

/**
 * Which of the three sources a secret comes from.
 *
 * SC_SECRET_NOWHERE when none of them has it. The order is the one sc_secret_read() walks, and
 * this is that walk asked to report the source instead of the value -- the two are the same
 * three checks in the same order, so they cannot disagree about which one wins.
 */
typedef enum sc_secret_source {
    SC_SECRET_NOWHERE = 0,
    SC_SECRET_ENVIRONMENT,
    SC_SECRET_FILE,
    SC_SECRET_CREDENTIAL
} sc_secret_source;

/**
 * Where @p name would come from, without reading it.
 *
 * `setup` is the caller this exists for. It offers a value as the default for its question and
 * writes the answer into the `.env` -- which is right for a secret that was in the environment
 * and wrong for one that was not. Asking for a password that systemd already keeps on a tmpfs
 * puts an empty line into the `.env`, and that line becomes the password on the day the
 * credential is taken away. So the question is not asked and the line is not written.
 *
 * Answers without opening a credential or a named file: what a caller wants to know here is
 * which source is in force, and reading the secret to find out would put it in a buffer for no
 * reason. A named file that does not exist is still SC_SECRET_FILE -- it is a configured source
 * that is broken, which sc_secret_read() is the one to complain about.
 */
sc_secret_source sc_secret_source_of(const char *name);

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

#endif /* SERVICE_CORE_SECRET_H */
