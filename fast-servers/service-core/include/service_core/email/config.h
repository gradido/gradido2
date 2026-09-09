/*
 * Where this instance sends its mail, read from the environment at startup.
 *
 * The fourth file of email/ and the only one that reads a variable: message.h holds the bytes
 * of a mail, transport.h one session, mailer.h the queue, and none of them knows where the
 * relay is -- they are handed it. This is what turns eight environment variables into the
 * `sc_mail_relay` those two take, once, while the process is starting.
 *
 * The names are legacy's -- EMAIL, EMAIL_USERNAME, EMAIL_SENDER, EMAIL_PASSWORD,
 * EMAIL_SMTP_HOST and EMAIL_SMTP_PORT are what a stage5.env holds -- so that an operator
 * moving a community to gradido2 does not have to relearn them. EMAIL_SMTP_TLS and
 * EMAIL_SENDER_NAME are new: legacy let nodemailer guess the encryption from the port number,
 * and guessing is how a password ends up on the wire in the clear.
 *
 * `packages/service-core/src/email/schema.ts` is the original of every default and every
 * rule below, and `contracts/settings.json` says why these are environment variables rather
 * than rows in the settings table: the password is a credential, and the backend opens a
 * session to the relay before there is a database to read a setting out of.
 */
#ifndef SERVICE_CORE_EMAIL_CONFIG_H
#define SERVICE_CORE_EMAIL_CONFIG_H

#include <stdint.h>

/* SC_MAIL_ADDR_MAX, the bound on an address. */
#include "service_core/email/message.h"
/* sc_mail_relay, and the URL, user and password bounds it carries. */
#include "service_core/email/transport.h"
#include "service_core/status.h"

/** Long enough that `smtps://host:port` always fits SC_MAIL_URL_MAX. */
#define SC_MAIL_HOST_MAX 100
/** The display name in front of the sender address. */
#define SC_MAIL_SENDER_NAME_MAX 128

/**
 * How the session to the relay is encrypted -- EMAIL_SMTP_TLS.
 *
 * One knob rather than a scheme and a flag, because the two are not independent: implicit is
 * TLS from the first byte and puts `smtps://` in the URL, where the other three are a plain
 * connection that may or may not be upgraded afterwards. The first three values are what
 * `sc_mail_relay.starttls` carries, which is why they are numbered as they are.
 */
typedef enum sc_mail_tls {
    SC_MAIL_TLS_NONE = 0,
    SC_MAIL_TLS_STARTTLS = 1,
    SC_MAIL_TLS_REQUIRE = 2,
    SC_MAIL_TLS_IMPLICIT = 3
} sc_mail_tls;

typedef struct sc_mail_env {
    /** EMAIL. Zero is a working configuration and the default: an instance that has not been
     *  told where to send mail sends none rather than failing to start. */
    int enabled;

    char host[SC_MAIL_HOST_MAX];   /* EMAIL_SMTP_HOST, default "localhost" */
    uint16_t port;                 /* EMAIL_SMTP_PORT, default 587 */
    sc_mail_tls tls;               /* EMAIL_SMTP_TLS, default starttls */
    char user[SC_MAIL_USER_MAX];   /* EMAIL_USERNAME, empty for a relay that wants no login */
    char pass[SC_MAIL_PASS_MAX];   /* EMAIL_PASSWORD, and only together with the user */
    char sender[SC_MAIL_ADDR_MAX]; /* EMAIL_SENDER */
    char sender_name[SC_MAIL_SENDER_NAME_MAX]; /* EMAIL_SENDER_NAME, may be empty */

    /** smtp://host:port, or smtps://host:port for implicit. Built here so that everything
     *  below this file is handed a URL rather than assembling one. */
    char url[SC_MAIL_URL_MAX];
} sc_mail_env;

/**
 * Fills @p out from the environment, applying the defaults above where a variable is unset.
 *
 * SC_ERR_TOO_LONG for a value that would not fit and SC_ERR_MALFORMED for a port that is not
 * a number, an EMAIL that is neither true nor false, an EMAIL_SMTP_TLS that is not one of the
 * four, or an EMAIL=true without a host or a sender -- in every case having already logged
 * which variable it was. The last of those is the rule
 * `packages/backend/src/config/schema.ts` forwards onto EMAIL_SMTP_HOST and EMAIL_SENDER.
 */
sc_status sc_mail_env_load(sc_mail_env *out);

/** Logs the effective mail configuration at info, once, under `cat: "startup"`. The password
 *  is reported as present or absent and never printed. */
void sc_mail_env_log(const sc_mail_env *env);

/** Fills @p out with what a session needs. It borrows @p env, which therefore has to outlive
 *  every use of the relay. */
void sc_mail_env_relay(const sc_mail_env *env, sc_mail_relay *out);

/** "none", "starttls", "require" or "implicit" -- the spelling EMAIL_SMTP_TLS and the `tls`
 *  field of contracts/logging.json's mail events use. Never NULL. */
const char *sc_mail_tls_name(sc_mail_tls tls);

#endif /* SERVICE_CORE_EMAIL_CONFIG_H */
