/*
 * One SMTP session over libcurl. service_core/email/transport.h is the specification.
 *
 * The whole curl surface of this project is here: curl_easy_{init,setopt,reset,perform,cleanup,
 * strerror}, curl_global_init and one stack-allocated curl_slist. Nothing above this file
 * includes <curl/curl.h>, which is what lets the mailer, the Node addon and the tests speak
 * about a session without speaking about curl.
 */
#include "service_core/email/transport.h"

#include <stdio.h>
#include <string.h>

#include <curl/curl.h>

#include "service_core/atomic.h"

/* What curl reads the body through: a cursor over the formatted message, no copy. */
typedef struct sc_mail_reader {
    const char *data;
    size_t len;
    size_t offset;
} sc_mail_reader;

static size_t read_body(char *dst, size_t size, size_t nmemb, void *userp)
{
    sc_mail_reader *reader = userp;
    const size_t room = size * nmemb;
    const size_t left = reader->len - reader->offset;
    const size_t n = left < room ? left : room;
    if (n == 0)
        return 0;
    memcpy(dst, reader->data + reader->offset, n);
    reader->offset += n;
    return n;
}

/* The relay answers a line or two per command and none of it belongs on stdout. */
static size_t discard(char *ptr, size_t size, size_t nmemb, void *userp)
{
    (void)ptr;
    (void)userp;
    return size * nmemb;
}

/*
 * curl_global_init(), once.
 *
 * A compare-and-swap and not a mutex, and deliberately not libuv's uv_once: this file is
 * compiled into a Node addon that links no libuv at all, and service_core/atomic.h is four
 * functions over the compiler's builtins. It is not a full once-barrier -- a second thread
 * arriving during the very first call would return before curl is ready -- and it does not have
 * to be: both callers make this call while their process is still single threaded, which is what
 * the header asks for.
 */
static volatile int32_t g_global_init;

sc_status sc_mail_global_init(void)
{
    if (sc_atomic_cas(&g_global_init, 0, 1))
        (void)curl_global_init(CURL_GLOBAL_ALL);
    return SC_OK;
}

sc_mail_session *sc_mail_session_open(void)
{
    return (sc_mail_session *)curl_easy_init();
}

void sc_mail_session_close(sc_mail_session *session)
{
    if (session != NULL)
        curl_easy_cleanup((CURL *)session);
}

/**
 * The options that describe the relay rather than the mail: where it is, how the session is
 * encrypted, who it is verified against and who is speaking.
 *
 * Shared by the send and the probe because they reach the same relay the same way -- the only
 * difference between the two is what happens after the handshake, and a second copy of this
 * block is how the probe would come to accept a certificate the send refuses.
 */
static void apply_relay(CURL *handle, const sc_mail_relay *relay, long timeout_ms)
{
    curl_easy_setopt(handle, CURLOPT_URL, relay->url);
    curl_easy_setopt(handle, CURLOPT_TIMEOUT_MS,
                     relay->timeout_ms > 0 ? relay->timeout_ms : timeout_ms);
    /*
     * Signals are how libcurl's own timeouts work when it has no threaded resolver, and a
     * library that installs a SIGALRM handler from a worker thread is a library fighting the
     * process for it. curl's own documentation calls NOSIGNAL mandatory for multi-threaded use.
     */
    curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);

    if (relay->starttls == 1)
        curl_easy_setopt(handle, CURLOPT_USE_SSL, (long)CURLUSESSL_TRY);
    else if (relay->starttls >= 2)
        curl_easy_setopt(handle, CURLOPT_USE_SSL, (long)CURLUSESSL_ALL);

    /*
     * The CA directory, off unless asked for. curl compiles /etc/ssl/certs in as CURLOPT_CAPATH
     * and reads every file in it for every new connection -- 150 certificates parsed again each
     * time, measured at 8,5 ms of an 11,5 ms cold TLS send. Setting it to NULL is what switches
     * that off; CURLOPT_SSL_VERIFYPEER does not, because curl decides where to look before it
     * decides whether to look.
     */
    if (!relay->scan_ca_path)
        curl_easy_setopt(handle, CURLOPT_CAPATH, (const char *)NULL);
    if (relay->cainfo != NULL && relay->cainfo[0] != '\0')
        curl_easy_setopt(handle, CURLOPT_CAINFO, relay->cainfo);
    if (relay->insecure) {
        curl_easy_setopt(handle, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(handle, CURLOPT_SSL_VERIFYHOST, 0L);
    }
    if (relay->user != NULL && relay->user[0] != '\0')
        curl_easy_setopt(handle, CURLOPT_USERNAME, relay->user);
    if (relay->pass != NULL && relay->pass[0] != '\0')
        curl_easy_setopt(handle, CURLOPT_PASSWORD, relay->pass);
}

sc_status sc_mail_session_send(sc_mail_session *session, const sc_mail_relay *relay, const char *to,
                               const char *message, size_t len, char *error, size_t error_cap)
{
    CURL *handle = (CURL *)session;
    sc_mail_reader reader;
    CURLcode rc;
    /*
     * The recipient list, built here instead of with curl_slist_append().
     *
     * CURLOPT_MAIL_RCPT is documented as taking "a fully valid list of struct curl_slist structs"
     * which curl neither copies nor frees -- "the list is not copied, so it must be kept around
     * until the transfer is done". With exactly one recipient that list is one node, and a node
     * on this stack frame outlives the perform() below by construction, where curl_slist_append()
     * would cost two mallocs per mail.
     */
    struct curl_slist rcpt;

    if (error != NULL && error_cap > 0)
        error[0] = '\0';
    if (handle == NULL || relay == NULL || relay->url == NULL || relay->from == NULL ||
        to == NULL || message == NULL)
        return SC_ERR_INVALID_ARGUMENT;

    reader.data = message;
    reader.len = len;
    reader.offset = 0;
    /* curl's slist takes a char*; it reads the recipient and never writes through it. */
    rcpt.data = (char *)to;
    rcpt.next = NULL;

    /*
     * Every option is set per mail rather than once, because the recipient and the body change
     * per mail anyway and a handle half configured at two different times is how an option
     * survives into a mail it was not meant for. curl_easy_reset keeps what matters: the
     * documentation lists live connections, the DNS cache and the session ID cache among the
     * things it does *not* touch, which is the whole reason a session is worth keeping.
     */
    curl_easy_reset(handle);
    apply_relay(handle, relay, SC_MAIL_TIMEOUT_DEFAULT_MS);
    curl_easy_setopt(handle, CURLOPT_MAIL_FROM, relay->from);
    curl_easy_setopt(handle, CURLOPT_MAIL_RCPT, &rcpt);
    curl_easy_setopt(handle, CURLOPT_UPLOAD, 1L);
    curl_easy_setopt(handle, CURLOPT_READFUNCTION, read_body);
    curl_easy_setopt(handle, CURLOPT_READDATA, &reader);
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, discard);
    curl_easy_setopt(handle, CURLOPT_INFILESIZE_LARGE, (curl_off_t)len);

    rc = curl_easy_perform(handle);
    if (rc != CURLE_OK) {
        /* The relay's own words, for the caller's log. Nothing is logged here: whether a failed
         * attempt is a warning or the end of a mail is the mailer's decision, not this one's. */
        if (error != NULL && error_cap > 0)
            snprintf(error, error_cap, "%s", curl_easy_strerror(rc));
        return SC_ERR_NETWORK;
    }
    return SC_OK;
}

sc_status sc_mail_session_probe(sc_mail_session *session, const sc_mail_relay *relay, char *error,
                                size_t error_cap)
{
    CURL *handle = (CURL *)session;
    CURLcode rc;

    if (error != NULL && error_cap > 0)
        error[0] = '\0';
    if (handle == NULL || relay == NULL || relay->url == NULL)
        return SC_ERR_INVALID_ARGUMENT;

    curl_easy_reset(handle);
    apply_relay(handle, relay, SC_MAIL_PROBE_TIMEOUT_DEFAULT_MS);
    /*
     * NOOP, which is the whole probe: curl opens the session, greets, upgrades the connection if
     * the relay was configured for it, authenticates if there are credentials, and then sends
     * the one SMTP command every server answers and no server acts on. So a refusal here is a
     * refusal of the session and not of a mail, and nothing is ever queued or delivered.
     *
     * Deliberately not CURLOPT_CONNECT_ONLY, which reads like the option for this and is not:
     * it ends the transfer before the protocol's own phase and answers CURLE_OPERATION_TIMEDOUT
     * at once against a relay that is there and answering.
     */
    curl_easy_setopt(handle, CURLOPT_NOBODY, 1L);
    curl_easy_setopt(handle, CURLOPT_CUSTOMREQUEST, "NOOP");
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, discard);

    rc = curl_easy_perform(handle);
    if (rc != CURLE_OK) {
        if (error != NULL && error_cap > 0)
            snprintf(error, error_cap, "%s", curl_easy_strerror(rc));
        return SC_ERR_NETWORK;
    }
    return SC_OK;
}
