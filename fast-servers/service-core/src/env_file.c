/*
 * The `.env` file. service_core/env_file.h is the specification, dotenv is the reference for
 * what the form means, and packages/backend/src/setup/envFile.ts is the writer this one has
 * to agree with.
 */
#include "service_core/env_file.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/stat.h>
#endif

/** Longest variable name that can appear on the left of an `=`. */
#define NAME_MAX_LEN 128

static void say(char *error, size_t error_cap, const char *fmt, ...)
{
    va_list args;

    if (error == NULL || error_cap == 0)
        return;
    va_start(args, fmt);
    (void)vsnprintf(error, error_cap, fmt, args);
    va_end(args);
}

sc_status sc_env_set(const char *name, const char *value)
{
    if (name == NULL || value == NULL)
        return SC_ERR_INVALID_ARGUMENT;
#if defined(_WIN32)
    return _putenv_s(name, value) == 0 ? SC_OK : SC_ERR_NO_MEMORY;
#else
    return setenv(name, value, 1) == 0 ? SC_OK : SC_ERR_NO_MEMORY;
#endif
}

/**
 * The whole file, on the host's heap, or NULL when there is none.
 *
 * Read at once rather than line by line because a quoted value may hold newlines, so a line
 * is not the unit the form is written in. This is a startup path and the file is a handful of
 * kilobytes; the no-malloc rule of AGENTS.md section 1 is about the request path.
 */
static char *read_all(const char *path, size_t *length, sc_status *status, char *error,
                      size_t error_cap)
{
    FILE *file = fopen(path, "rb");
    char *text;
    size_t read;

    *length = 0;
    *status = SC_OK;
    if (file == NULL)
        return NULL; /* Not configured by a file, which is an ordinary way to be configured. */

    text = (char *)malloc(SC_ENV_FILE_MAX + 1);
    if (text == NULL) {
        (void)fclose(file);
        *status = SC_ERR_NO_MEMORY;
        return NULL;
    }
    read = fread(text, 1, SC_ENV_FILE_MAX + 1, file);
    (void)fclose(file);
    if (read > SC_ENV_FILE_MAX) {
        free(text);
        say(error, error_cap, "%s is larger than %d bytes", path, SC_ENV_FILE_MAX);
        *status = SC_ERR_TOO_LONG;
        return NULL;
    }
    text[read] = '\0';
    *length = read;
    return text;
}

/** Whether @p c may start a variable name. */
static int is_name_start(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

static int is_name_char(char c)
{
    return is_name_start(c) || (c >= '0' && c <= '9');
}

/** Past spaces and tabs, but never past the end of a line. */
static size_t skip_blanks(const char *text, size_t at, size_t length)
{
    while (at < length && (text[at] == ' ' || text[at] == '\t'))
        ++at;
    return at;
}

static size_t skip_line(const char *text, size_t at, size_t length)
{
    while (at < length && text[at] != '\n')
        ++at;
    return at < length ? at + 1 : at;
}

/**
 * Copies the value that starts at @p at into @p out and answers where it ended.
 *
 * The three forms of the header, and nothing between them: a quote as the first character
 * decides, and the closing quote of the same kind ends the value however much lies between.
 * Only the double-quoted form unescapes anything, and only \n and \r, which is exactly what
 * dotenv does with it.
 */
static size_t read_value(const char *text, size_t at, size_t length, char *out, size_t out_size,
                         int *too_long)
{
    size_t written = 0;
    char quote = 0;

    *too_long = 0;
    if (at < length && (text[at] == '\'' || text[at] == '"'))
        quote = text[at++];

    while (at < length) {
        char c = text[at];

        if (quote != 0) {
            if (c == quote) {
                ++at;
                break;
            }
            if (quote == '"' && c == '\\' && at + 1 < length &&
                (text[at + 1] == 'n' || text[at + 1] == 'r')) {
                c = text[at + 1] == 'n' ? '\n' : '\r';
                ++at;
            }
        } else {
            /* Unquoted ends at the line, and at a `#`, which starts a comment. */
            if (c == '\n' || c == '\r' || c == '#')
                break;
        }
        if (written + 1 >= out_size) {
            *too_long = 1;
            return at;
        }
        out[written++] = c;
        ++at;
    }

    if (quote == 0) {
        /* Trailing blanks are not part of an unquoted value; a value that wants them is
         * quoted, which is what the writer does with one. */
        while (written != 0 && (out[written - 1] == ' ' || out[written - 1] == '\t'))
            --written;
    }
    out[written] = '\0';
    return at;
}

sc_status sc_env_file_load(const char *path, char *error, size_t error_cap)
{
    char name[NAME_MAX_LEN];
    /* One value, sized past the largest a variable of this project carries -- a database file
     * path. A value longer than this refuses the file rather than truncating: half a path and
     * half a password are the wrong value, not a shorter one. */
    char value[1024];
    size_t length = 0;
    size_t at = 0;
    sc_status status = SC_OK;
    char *text;

    if (path == NULL)
        return SC_ERR_INVALID_ARGUMENT;
    if (error != NULL && error_cap > 0)
        error[0] = '\0';

    text = read_all(path, &length, &status, error, error_cap);
    if (text == NULL)
        return status;

    while (at < length) {
        size_t name_length = 0;
        int too_long = 0;

        at = skip_blanks(text, at, length);
        if (at >= length)
            break;
        if (text[at] == '\n' || text[at] == '\r' || text[at] == '#') {
            at = skip_line(text, at, length);
            continue;
        }
        /* `export NAME=value`, which a file meant to be sourced by a shell carries. */
        if (strncmp(text + at, "export", 6) == 0 && at + 6 < length &&
            (text[at + 6] == ' ' || text[at + 6] == '\t')) {
            at = skip_blanks(text, at + 6, length);
        }

        if (!is_name_start(text[at])) {
            say(error, error_cap, "%s holds a line that is not an assignment", path);
            status = SC_ERR_MALFORMED;
            break;
        }
        while (at < length && is_name_char(text[at])) {
            if (name_length + 1 >= sizeof(name)) {
                say(error, error_cap, "%s holds a variable name longer than %d bytes", path,
                    (int)sizeof(name) - 1);
                status = SC_ERR_TOO_LONG;
                break;
            }
            name[name_length++] = text[at++];
        }
        if (status != SC_OK)
            break;
        name[name_length] = '\0';

        at = skip_blanks(text, at, length);
        if (at >= length || text[at] != '=') {
            say(error, error_cap, "%s holds %s without a value", path, name);
            status = SC_ERR_MALFORMED;
            break;
        }
        at = skip_blanks(text, at + 1, length);
        at = read_value(text, at, length, value, sizeof(value), &too_long);
        if (too_long) {
            say(error, error_cap, "%s is longer than %d bytes", name, (int)sizeof(value) - 1);
            status = SC_ERR_TOO_LONG;
            break;
        }
        at = skip_line(text, at, length);

        /* The environment wins: a variable systemd, docker or a shell already set is not
         * overridden by a file somebody left in the working directory. */
        if (getenv(name) == NULL) {
#if defined(_WIN32)
            (void)_putenv_s(name, value);
#else
            (void)setenv(name, value, 0);
#endif
        }
    }

    free(text);
    return status;
}

/** The form the value has to be written in, or 0 when no form carries it. */
static char quote_for(const char *value)
{
    size_t i;
    int bare = 1;

    for (i = 0; value[i] != '\0'; ++i) {
        char c = value[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
              strchr("_@%+:,./=-", c) != NULL)) {
            bare = 0;
            break;
        }
    }
    if (bare)
        return ' ';
    if (strchr(value, '\'') == NULL)
        return '\'';
    /* The double-quoted form unescapes \n and \r, so a value carrying those two characters
     * literally cannot be written in it. */
    if (strchr(value, '"') == NULL && strstr(value, "\\n") == NULL && strstr(value, "\\r") == NULL)
        return '"';
    return 0;
}

/** Appends `NAME=value` to @p out. 0 when it did not fit or no quoting carries the value. */
static int append_assignment(char *out, size_t out_size, size_t *at, const sc_env_entry *entry,
                             char *error, size_t error_cap)
{
    char quote = quote_for(entry->value);
    int written;

    if (quote == 0) {
        say(error, error_cap,
            "%s cannot be written to the file: no quoting dotenv reads carries both a ' and a "
            "\" in one value",
            entry->name);
        return 0;
    }
    written = quote == ' '
                  ? snprintf(out + *at, out_size - *at, "%s=%s\n", entry->name, entry->value)
                  : snprintf(out + *at, out_size - *at, "%s=%c%s%c\n", entry->name, quote,
                             entry->value, quote);
    if (written < 0 || (size_t)written >= out_size - *at) {
        say(error, error_cap, "the file would be larger than %d bytes", SC_ENV_FILE_MAX);
        return 0;
    }
    *at += (size_t)written;
    return 1;
}

/** The entry a line assigns to, or NULL for a comment, a blank line or a name nobody answered. */
static const sc_env_entry *assigned(const char *line, size_t line_length,
                                    const sc_env_entry *entries, size_t count, const int *written)
{
    size_t at = skip_blanks(line, 0, line_length);
    size_t begin;
    size_t i;

    if (at >= line_length || !is_name_start(line[at]))
        return NULL;
    begin = at;
    while (at < line_length && is_name_char(line[at]))
        ++at;
    {
        size_t name_length = at - begin;
        size_t after = skip_blanks(line, at, line_length);

        if (after >= line_length || line[after] != '=')
            return NULL;
        for (i = 0; i != count; ++i) {
            if (!written[i] && strlen(entries[i].name) == name_length &&
                strncmp(entries[i].name, line + begin, name_length) == 0)
                return &entries[i];
        }
    }
    return NULL;
}

sc_status sc_env_file_write(const char *path, const sc_env_entry *entries, size_t count,
                            char *error, size_t error_cap)
{
    /* One flag per entry, so that a variable named twice in the file is answered once -- the
     * first occurrence, which is the one dotenv reads. */
    int written[64];
    size_t existing_length = 0;
    size_t at = 0;
    size_t i;
    size_t out_at = 0;
    int heading = 0;
    sc_status status = SC_OK;
    char *existing;
    char *out;
    FILE *file;

    if (path == NULL || entries == NULL || count > sizeof(written) / sizeof(written[0]))
        return SC_ERR_INVALID_ARGUMENT;
    if (error != NULL && error_cap > 0)
        error[0] = '\0';
    memset(written, 0, sizeof(written));

    existing = read_all(path, &existing_length, &status, error, error_cap);
    if (status != SC_OK)
        return status;

    /* Room for the file as it stands plus every answer, which is the largest the result can
     * be: a replaced line is written in place of the one it replaces. */
    out = (char *)malloc(SC_ENV_FILE_MAX + 1);
    if (out == NULL) {
        free(existing);
        return SC_ERR_NO_MEMORY;
    }

    while (at < existing_length) {
        size_t end = at;
        const sc_env_entry *entry;

        while (end < existing_length && existing[end] != '\n')
            ++end;
        entry = assigned(existing + at, end - at, entries, count, written);
        if (entry != NULL) {
            if (!append_assignment(out, SC_ENV_FILE_MAX + 1, &out_at, entry, error, error_cap)) {
                status = SC_ERR_MALFORMED;
                break;
            }
            written[entry - entries] = 1;
        } else {
            size_t line = end - at;

            if (out_at + line + 1 >= SC_ENV_FILE_MAX) {
                say(error, error_cap, "the file would be larger than %d bytes", SC_ENV_FILE_MAX);
                status = SC_ERR_TOO_LONG;
                break;
            }
            memcpy(out + out_at, existing + at, line);
            out_at += line;
            out[out_at++] = '\n';
        }
        at = end < existing_length ? end + 1 : end;
    }
    free(existing);

    for (i = 0; status == SC_OK && i != count; ++i) {
        if (written[i])
            continue;
        if (!heading) {
            /* Once, above the block that was appended rather than replaced. */
            const char *text = out_at == 0 ? "# written by the setup command\n"
                                           : "\n# written by the setup command\n";
            size_t len = strlen(text);

            if (out_at + len >= SC_ENV_FILE_MAX) {
                say(error, error_cap, "the file would be larger than %d bytes", SC_ENV_FILE_MAX);
                status = SC_ERR_TOO_LONG;
                break;
            }
            memcpy(out + out_at, text, len);
            out_at += len;
            heading = 1;
        }
        if (!append_assignment(out, SC_ENV_FILE_MAX + 1, &out_at, &entries[i], error, error_cap))
            status = SC_ERR_MALFORMED;
    }

    if (status == SC_OK) {
        file = fopen(path, "wb");
        if (file == NULL) {
            say(error, error_cap, "%s cannot be written", path);
            status = SC_ERR_UNAVAILABLE;
        } else {
            if (fwrite(out, 1, out_at, file) != out_at) {
                say(error, error_cap, "%s could not be written in full", path);
                status = SC_ERR_UNAVAILABLE;
            }
            (void)fclose(file);
#if !defined(_WIN32)
            /* It holds the database password and the SMTP password. */
            (void)chmod(path, S_IRUSR | S_IWUSR);
#endif
        }
    }

    free(out);
    return status;
}
