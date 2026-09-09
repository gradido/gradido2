/*
 * The mail configuration, read from the environment.
 * service_core/email/config.h is the specification, and
 * packages/service-core/src/email/schema.ts is the original of every default in it.
 */
#include "service_core/email/config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "service_core/log/log.h"
#include "service_core/secret.h"

#define DEFAULT_HOST "localhost"
#define DEFAULT_PORT 587

/**
 * Copies the environment variable @p name into @p dst, or @p fallback when it is unset.
 *
 * A value that does not fit answers SC_ERR_TOO_LONG. It never truncates: half a host name is
 * a host name, and it is the wrong one -- and half a password is a failed login nobody can
 * see the reason for.
 */
static sc_status copy_env(char *dst, size_t dst_size, const char *name, const char *fallback)
{
    const char *value = getenv(name);
    size_t len;

    if (value == NULL)
        value = fallback;
    len = strlen(value);
    if (len >= dst_size) {
        sc_log_fatal(SC_CAT_STARTUP, "config.value_too_long", "%s is %zu bytes, the limit is %zu",
                     name, len, dst_size - 1);
        return SC_ERR_TOO_LONG;
    }
    memcpy(dst, value, len + 1);
    return SC_OK;
}

static sc_status read_flag(int *out, const char *name, int fallback)
{
    const char *value = getenv(name);

    if (value == NULL || value[0] == '\0') {
        *out = fallback;
        return SC_OK;
    }
    if (strcmp(value, "true") == 0) {
        *out = 1;
        return SC_OK;
    }
    if (strcmp(value, "false") == 0) {
        *out = 0;
        return SC_OK;
    }
    /* Only the two spellings, because a variable that is read leniently is one that can be
     * switched off by a typo: EMAIL=1 would silently become false, and the mail that never
     * arrives is discovered days later. */
    sc_log_fatal(SC_CAT_STARTUP, "config.flag_invalid",
                 "%s is '%s', which is neither true nor false", name, value);
    return SC_ERR_MALFORMED;
}

static sc_status read_port(uint16_t *out, const char *name, uint16_t fallback)
{
    const char *value = getenv(name);
    char *end;
    unsigned long parsed;

    if (value == NULL || value[0] == '\0') {
        *out = fallback;
        return SC_OK;
    }
    parsed = strtoul(value, &end, 10);
    if (*end != '\0' || parsed == 0 || parsed > 65535) {
        sc_log_fatal(SC_CAT_STARTUP, "config.port_invalid",
                     "%s is '%s', which is not a port between 1 and 65535", name, value);
        return SC_ERR_MALFORMED;
    }
    *out = (uint16_t)parsed;
    return SC_OK;
}

const char *sc_mail_tls_name(sc_mail_tls tls)
{
    switch (tls) {
    case SC_MAIL_TLS_NONE:
        return "none";
    case SC_MAIL_TLS_REQUIRE:
        return "require";
    case SC_MAIL_TLS_IMPLICIT:
        return "implicit";
    case SC_MAIL_TLS_STARTTLS:
    default:
        return "starttls";
    }
}

static sc_status read_tls(sc_mail_tls *out)
{
    const char *value = getenv("EMAIL_SMTP_TLS");
    sc_mail_tls tls;

    if (value == NULL || value[0] == '\0') {
        *out = SC_MAIL_TLS_STARTTLS;
        return SC_OK;
    }
    for (tls = SC_MAIL_TLS_NONE; tls <= SC_MAIL_TLS_IMPLICIT; ++tls) {
        if (strcmp(value, sc_mail_tls_name(tls)) == 0) {
            *out = tls;
            return SC_OK;
        }
    }
    sc_log_fatal(SC_CAT_STARTUP, "config.mail_tls_invalid",
                 "EMAIL_SMTP_TLS is '%s', which is none, starttls, require or implicit", value);
    return SC_ERR_MALFORMED;
}

sc_status sc_mail_env_load(sc_mail_env *out)
{
    sc_status status;
    int written;

    if (out == NULL)
        return SC_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));

    status = read_flag(&out->enabled, "EMAIL", 0);
    if (status != SC_OK)
        return status;
    status = copy_env(out->host, sizeof(out->host), "EMAIL_SMTP_HOST", DEFAULT_HOST);
    if (status != SC_OK)
        return status;
    status = read_port(&out->port, "EMAIL_SMTP_PORT", DEFAULT_PORT);
    if (status != SC_OK)
        return status;
    status = read_tls(&out->tls);
    if (status != SC_OK)
        return status;
    status = copy_env(out->user, sizeof(out->user), "EMAIL_USERNAME", "");
    if (status != SC_OK)
        return status;
    /* Not copy_env: a relay password may come from a systemd credential or from a file the
     * environment names, and only lastly from the variable -- contracts/secrets.json, the same
     * order DB_PASSWORD resolves by. Everything else here is a value somebody may read over a
     * shoulder; these two are not. */
    status = sc_secret_read("EMAIL_PASSWORD", out->pass, sizeof(out->pass));
    if (status != SC_OK) {
        if (status == SC_ERR_TOO_LONG)
            sc_log_fatal(SC_CAT_STARTUP, "config.value_too_long",
                         "EMAIL_PASSWORD is %zu bytes at most", sizeof(out->pass) - 1);
        return status;
    }
    status = copy_env(out->sender, sizeof(out->sender), "EMAIL_SENDER", "");
    if (status != SC_OK)
        return status;
    status = copy_env(out->sender_name, sizeof(out->sender_name), "EMAIL_SENDER_NAME", "");
    if (status != SC_OK)
        return status;

    /*
     * The two rules that need EMAIL as well as the field they are about, which is why they
     * cannot live on either field: an empty host and an empty sender are the defaults and are
     * correct -- right up to the moment somebody writes EMAIL=true and expects mail to arrive.
     * packages/backend/src/config/schema.ts forwards the same two onto the same two variables.
     */
    if (out->enabled && out->host[0] == '\0') {
        sc_log_fatal(SC_CAT_STARTUP, "config.mail_incomplete",
                     "EMAIL=true needs a relay in EMAIL_SMTP_HOST");
        return SC_ERR_MALFORMED;
    }
    if (out->enabled && out->sender[0] == '\0') {
        sc_log_fatal(SC_CAT_STARTUP, "config.mail_incomplete",
                     "EMAIL=true needs a sender address in EMAIL_SENDER");
        return SC_ERR_MALFORMED;
    }

    written = snprintf(out->url, sizeof(out->url), "%s://%s:%u",
                       out->tls == SC_MAIL_TLS_IMPLICIT ? "smtps" : "smtp", out->host,
                       (unsigned)out->port);
    if (written < 0 || (size_t)written >= sizeof(out->url)) {
        sc_log_fatal(SC_CAT_STARTUP, "config.value_too_long",
                     "EMAIL_SMTP_HOST and EMAIL_SMTP_PORT do not fit a URL of %zu bytes",
                     sizeof(out->url) - 1);
        return SC_ERR_TOO_LONG;
    }
    return SC_OK;
}

void sc_mail_env_log(const sc_mail_env *env)
{
    if (env == NULL)
        return;
    if (!env->enabled) {
        sc_log_info(SC_CAT_STARTUP, "config.mail", "no mail: EMAIL is false");
        return;
    }
    /* The sender is an email address, which contracts/logging.json never logs, and the
     * password is a credential -- reported as present or absent, the way db.c reports the
     * database's. */
    sc_log_info(SC_CAT_STARTUP, "config.mail", "%s, tls %s, user %s, password %s", env->url,
                sc_mail_tls_name(env->tls), env->user[0] != '\0' ? env->user : "(none)",
                env->pass[0] != '\0' ? "set" : "(unset)");
}

void sc_mail_env_relay(const sc_mail_env *env, sc_mail_relay *out)
{
    if (env == NULL || out == NULL)
        return;
    memset(out, 0, sizeof(*out));
    out->url = env->url;
    out->from = env->sender;
    out->user = env->user[0] != '\0' ? env->user : NULL;
    out->pass = env->user[0] != '\0' ? env->pass : NULL;
    /* Implicit TLS is the scheme, not an upgrade, so it leaves starttls at none: the session
     * is encrypted from the first byte and there is nothing to upgrade. */
    out->starttls = env->tls == SC_MAIL_TLS_IMPLICIT ? 0 : (int)env->tls;
}
