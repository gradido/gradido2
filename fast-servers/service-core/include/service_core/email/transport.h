/*
 * One SMTP session: connect, authenticate, hand over one message.
 *
 * The second half both callers share (see email/message.h for the first). It is a session and
 * not a mailer: no queue, no retry, no threads, no logging -- a failed send answers with a
 * status and the relay's own words, and what to do about it is the caller's decision.
 *
 * A session is a curl easy handle and **belongs to one thread**; libcurl says so and means it.
 * Whoever opens one owns it until it is closed. Holding it across several mails is what makes
 * the difference worth knowing about:
 *
 *   a mail on a kept session      85 us
 *   a mail on a new session      225 us   -- 2,7x, and that is loopback
 *
 * Against a remote relay the gap is a TCP handshake, a TLS handshake and the SMTP greeting
 * dialogue per mail, so it is round trips rather than microseconds. email/mailer.h keeps its
 * sessions for that reason; a caller sending one mail per request does not have to.
 *
 * The type stays opaque so that <curl/curl.h> does not travel with this header -- the same rule
 * AGENTS.md section 3a states for arnm's parser: reach for the surface, not for what is under it.
 */
#ifndef SERVICE_CORE_EMAIL_TRANSPORT_H
#define SERVICE_CORE_EMAIL_TRANSPORT_H

#include <stddef.h>

#include "service_core/status.h"

/* The one CA path a relay is verified against, at most. */
#define SC_MAIL_PATH_MAX 512
/* smtp://host:port, smtps://host:port */
#define SC_MAIL_URL_MAX 128
#define SC_MAIL_USER_MAX 128
#define SC_MAIL_PASS_MAX 128
/* What sc_mail_session_send() may write into the caller's error buffer. */
#define SC_MAIL_ERROR_MAX 128
/** Round trip a single attempt is given when the relay config names none. */
#define SC_MAIL_TIMEOUT_DEFAULT_MS 10000L

/** One session. Opaque: a curl easy handle underneath, owned by one thread. */
typedef struct sc_mail_session sc_mail_session;

/* Where and how to reach the relay. Every string is borrowed for the call. */
typedef struct sc_mail_relay {
    /* Required. smtp://host:port for plain or STARTTLS, smtps://host:port for implicit TLS. */
    const char *url;
    /* Required. The envelope sender. */
    const char *from;

    /* Optional AUTH credentials. Both or neither. */
    const char *user;
    const char *pass;

    /*
     * STARTTLS on an smtp:// URL: 0 none, 1 try, 2 require. Ignored for smtps://, which is TLS
     * from the first byte either way.
     */
    int starttls;

    /*
     * The one CA certificate the relay is verified against, instead of the host's bundle.
     *
     * Worth naming, and not for tidiness: curl sets CURLOPT_CAPATH to a compiled-in
     * /etc/ssl/certs and both OpenSSL and wolfSSL read that directory *per connection* -- about
     * 150 certificates parsed again every time. Measured, it was 8,5 ms of an 11,5 ms cold TLS
     * send. Naming one file and leaving the directory unset took the same send to 3,0 ms.
     * --insecure does not help: curl sets the verify locations before it decides not to verify.
     *
     * A *cross compiled* binary has no bundle to fall back on, so NULL there means "verify
     * against nothing", which fails the handshake rather than skipping it.
     */
    const char *cainfo;
    /* Non-zero lets curl scan its CA *directory* as well. Zero -- the default -- is the 8,5 ms
     * above. Turn it on only for a host that keeps its trust in a directory and not in a file. */
    int scan_ca_path;
    /* Non-zero skips certificate verification. Development only. */
    int insecure;

    /* Round trip one attempt is given. 0 selects SC_MAIL_TIMEOUT_DEFAULT_MS. */
    long timeout_ms;
} sc_mail_relay;

/**
 * curl_global_init(), once per process, before any session is opened.
 *
 * curl_easy_init() calls it on its own if nobody did, and curl's documentation is explicit that
 * doing so is not thread safe -- which matters as soon as two sessions are opened at once. Call
 * this while the process is still single threaded; a second call does nothing.
 */
sc_status sc_mail_global_init(void);

/** Opens a session. NULL when curl has none to give. Close it on the thread that opened it. */
sc_mail_session *sc_mail_session_open(void);
void sc_mail_session_close(sc_mail_session *session);

/**
 * Sends @p message of @p len bytes to the single recipient @p to, and blocks for as long as the
 * relay takes.
 *
 * Every option is set per mail rather than once, because the recipient and the body change per
 * mail anyway and a handle half configured at two different times is how an option survives into
 * a mail it was not meant for. curl_easy_reset keeps what matters -- live connections, the DNS
 * cache and the TLS session cache are documented as surviving it, which is the whole reason a
 * session is worth keeping.
 *
 * SC_ERR_NETWORK when the relay refused or could not be reached; @p error then holds
 * curl_easy_strerror()'s sentence, which belongs in the caller's log rather than in one written
 * here. @p error may be NULL.
 */
sc_status sc_mail_session_send(sc_mail_session *session, const sc_mail_relay *relay,
                               const char *to, const char *message, size_t len, char *error,
                               size_t error_cap);

#endif /* SERVICE_CORE_EMAIL_TRANSPORT_H */
