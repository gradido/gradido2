/*
 * Sending mail: SMTP sessions held open, mails taken off a bounded queue by a small pool of
 * worker threads that grows under load and shrinks again.
 *
 * This is the top of three files, and the only one that has threads in it. Under it sit
 * email/message.h -- the bytes of one mail -- and email/transport.h -- one session, one send --
 * and neither knows this file exists. That is what lets the Node addon in
 * packages/email-native send the same message with a libuv thread pool thread and no pool of its
 * own: a process that sends a mail per request wants the simple half, a server that sends
 * thousands wants this one.
 *
 * The shape comes from what the measurements said, not from what SMTP allows. Three numbers
 * decided the session handling, all of them from the h2o prototype's smtp_client:
 *
 *   a mail on a kept connection      85 us
 *   a mail on a new connection      225 us   -- 2,7x, on loopback
 *   a mail to MailDev, new         102 800 us -- its greeting timer, 100 ms per connection
 *
 * So a connection is held: one CURL handle per worker for as long as that worker lives, and the
 * socket underneath is curl's business. Two minutes idle and curl replaces it
 * (CURLOPT_MAXAGE_CONN, `conn_max_idle_ms = 118 * 1000` in curl's lib/url.c); a socket the relay
 * closed is detected before reuse and replaced too. From the outside that is invisible, which is
 * the point -- neither "hold it open forever" nor "one connection per mail", and nothing for a
 * caller to catch.
 *
 * ### The workers
 *
 * One worker is permanent and sleeps on a condition variable when there is nothing to do. The
 * others are started only when the first cannot keep up -- when it has been busy without a pause
 * for `spawn_after_ms` and `spawn_backlog` mails are still waiting -- and retire again after
 * `linger_ms` of idleness. A worker is a connection: an easy handle belongs to one thread, so
 * "another worker" and "another session to the relay" are the same act.
 *
 * The ceiling is `worker_max`, which defaults to half the machine's cores minus one and never
 * less than one. Against a remote relay that is the number that decides throughput -- at 30 ms
 * RTT a connection is idle 99.9 % of the time and mails per second scale linearly with how many
 * are open. Against a local relay one worker is already worth 11 000 mails/s and the second one
 * buys nothing, which is why they are not started up front.
 *
 * This does not use libuv's thread pool, and not for lack of trying: `uv_queue_work` needs a
 * `uv_loop_t`, and on the h2o path this process has none -- h2o runs its own event loop and the
 * only `uv_loop_t` in the tree belongs to the fallback HTTP backend, which a given build may not
 * even compile. The loop-free half of libuv is what AGENTS.md section 3a permits next to h2o's
 * evloop, and that is what this uses. A pool would also have no way to express the one thing
 * asked of the arrangement: a worker that stays.
 *
 * ### Retry: once, after a pause, then it is written down and dropped
 *
 * A mail that fails is tried once more after `retry_delay_ms`. If the second attempt fails it is
 * logged at error with its recipient and its Message-ID, and that is the end of it. There is no
 * third attempt and no growing backoff, because an unbounded retry is how a dead relay turns
 * into a hot loop -- and because a mail that matters more than that must not be in this queue in
 * the first place. See *What this is not*.
 *
 * ### One recipient
 *
 * gradido's mails are personalised: one address, one body, no newsletter. So there is no fan-out
 * here and no list to build -- several RCPT TO before one DATA saves nothing when the body
 * differs per recipient. If that ever changes it is a new call, not a parameter added to
 * this one.
 *
 * ### What this is not
 *
 * Not a queue that survives the process. A mail that is waiting when the process dies is gone,
 * and so is one that exhausted its retry. Persistence belongs in the database and is not built
 * yet; until it is, anything that must not be lost has to be written down before it is handed
 * here. See Architecture.md, *Mail*, for where that line is drawn and what the database version
 * has to do.
 */
#ifndef SERVICE_CORE_EMAIL_MAILER_H
#define SERVICE_CORE_EMAIL_MAILER_H

#include <stddef.h>
#include <stdint.h>

#include "service_core/status.h"

/* sc_mail and the address, subject and Message-ID bounds. */
#include "service_core/email/message.h"
/* sc_mail_relay and the URL, credential and timeout bounds. */
#include "service_core/email/transport.h"

/** Mails that may wait at once when the caller names no limit. */
#define SC_MAIL_QUEUE_DEFAULT 256
/*
 * Bytes one formatted message may take when the caller names no size -- 96 KiB.
 *
 * It was 32 KiB while a message was the bare HTML. A conformant one is the quoted-printable
 * HTML *and* the six inline images base64 encoded, which is ~65 KiB for the largest template,
 * so 32 KiB would refuse every mail this project sends. The cost is the queue: it reserves
 * queue_max of these up front, so the default pair is now ~24 MB rather than ~8 MB. A
 * deployment that sends fewer mails at once should say so with queue_max.
 */
#define SC_MAIL_MESSAGE_DEFAULT ((uint32_t)96 * 1024)
/** Pause before the one retry. */
#define SC_MAIL_RETRY_DELAY_DEFAULT_MS 30000
/** How long a worker must be busy without a pause before another one is started. */
#define SC_MAIL_SPAWN_AFTER_DEFAULT_MS 2000
/** And how many mails must be waiting at that moment. */
#define SC_MAIL_SPAWN_BACKLOG_DEFAULT 8
/** How long a worker that was started on demand waits for work before retiring. */
#define SC_MAIL_LINGER_DEFAULT_MS 30000
/** Hard ceiling on threads, whatever the machine says. */
#define SC_MAIL_WORKERS_MAX 32

typedef struct sc_mailer sc_mailer;

/*
 * Startup configuration. Every string is copied into the mailer, so none of it has to outlive
 * this call, and one that does not fit answers SC_ERR_TOO_LONG rather than being truncated --
 * a truncated host name connects somewhere else, and a truncated address delivers to someone
 * else.
 */
typedef struct sc_mail_config {
    /* Required. smtp://host:port for plain or STARTTLS, smtps://host:port for implicit TLS. */
    const char *url;
    /* Required. The envelope sender, and the From: header unless from_name says otherwise. */
    const char *from;
    /* Optional display name. NULL sends the bare address. */
    const char *from_name;

    /* Optional AUTH credentials. Both or neither. */
    const char *user;
    const char *pass;

    /*
     * STARTTLS on an smtp:// URL: 0 none, 1 try, 2 require. Ignored for smtps://, which is TLS
     * from the first byte either way. Require is what a relay on the far side of the internet
     * deserves; try is what a MailDev in a compose stack can be given.
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
     * NULL keeps the host's bundle, and then scan_ca_path below decides whether the directory
     * is walked as well.
     *
     * Two caveats, and neither has been re-measured:
     *
     *   The TLS underneath is mbedtls -- Architecture.md, *TLS*, says why. Both options work
     *   there (CAINFO through mbedtls_x509_crt_parse_file, CAPATH through
     *   mbedtls_x509_crt_parse_path); whether the directory costs the same 8,5 ms is unknown,
     *   so the default stays off.
     *
     *   A *cross compiled* binary has no bundle to fall back on: curl auto-detects the host's
     *   only for a native build. NULL then means "verify against nothing", which fails the
     *   handshake rather than skipping it -- name cainfo on that deployment.
     */
    const char *cainfo;
    /*
     * Non-zero lets curl scan its CA *directory* in addition to the bundle. Zero -- the default
     * -- switches that off, which is the 8,5 ms above. Turn it on only for a host that keeps
     * its trust in the directory and not in a bundle file.
     */
    int scan_ca_path;

    /* Non-zero skips certificate verification. Development only; it is logged at warn on every
     * startup that sets it, so it cannot be left on by accident. */
    int insecure;

    /* 0 selects SC_MAIL_TIMEOUT_DEFAULT_MS. Per attempt, so a mail that retries may take twice
     * this plus the retry delay before it is finally given up on. */
    long timeout_ms;

    /*
     * Mails that may wait at once. 0 selects SC_MAIL_QUEUE_DEFAULT.
     *
     * This and message_max are the whole memory ceiling and it is exact: the queue, the retry
     * ring and one buffer per queued message are all reserved at create and nothing is asked of
     * the host afterwards. Roughly `queue_max * (message_max + 600)` bytes.
     */
    uint32_t queue_max;
    /* Bytes one rendered message may take, headers included. 0 selects SC_MAIL_MESSAGE_DEFAULT.
     * A mail that would need more is refused at enqueue rather than truncated. */
    uint32_t message_max;

    /*
     * Threads. 0 means none: no worker is started and the caller sends on its own thread with
     * sc_mail_flush(), which is the mode the tests use and the one a caller with its own
     * scheduling wants. 1 or more starts that many immediately, of which the first is permanent
     * and the rest are ordinary on-demand workers that may retire.
     */
    uint32_t workers;
    /* Ceiling on workers, including the permanent one. 0 selects half the machine's cores minus
     * one, never below 1 and never above SC_MAIL_WORKERS_MAX. Against a remote relay this is the
     * number that decides throughput; against a local one it changes nothing. */
    uint32_t worker_max;

    /* 0 selects the SC_MAIL_*_DEFAULT_MS above. */
    int64_t retry_delay_ms;
    int64_t spawn_after_ms;
    int64_t linger_ms;
    /* 0 selects SC_MAIL_SPAWN_BACKLOG_DEFAULT. */
    uint32_t spawn_backlog;
} sc_mail_config;

/* What the mailer has done since it was created. Counters are cumulative; the last two are the
 * state right now. */
typedef struct sc_mail_stats {
    uint64_t queued;  /* accepted by sc_mail_enqueue */
    uint64_t sent;    /* the relay said 250 */
    uint64_t retried; /* second attempts started */
    uint64_t failed;  /* given up on after the retry, and logged */
    uint32_t pending; /* waiting for a worker, retries included */
    uint32_t workers; /* threads alive */
} sc_mail_stats;

/**
 * Builds a mailer, its queue and its workers. Allocates: this is startup, and it is the only
 * time anything here asks the host for memory.
 *
 * Answers SC_ERR_INVALID_ARGUMENT for a config without url or from, SC_ERR_TOO_LONG for a field
 * that does not fit, and SC_ERR_NO_MEMORY for what it could not reserve. Nothing is dialled
 * here -- the first connection is opened by the first mail, so a relay that is down at startup
 * does not stop the process from starting.
 */
sc_status sc_mailer_create(const sc_mail_config *config, sc_mailer **out);

/**
 * Stops the workers, closes their sessions and gives the memory back.
 *
 * Waits for a worker that is mid-send to finish that mail; it does not cancel one, because a
 * transfer abandoned inside DATA is a mail the relay may or may not have taken. Mails still
 * waiting are dropped and counted in a warning line.
 */
void sc_mailer_destroy(sc_mailer *mailer);

/**
 * Renders @p mail and puts it in the queue. Does not dial and does not block on a send.
 *
 * Safe from any thread. The message is built once, here, so that a worker only writes bytes that
 * already exist -- and it is built outside the queue lock, so several threads can enqueue at
 * once.
 *
 * Answers SC_ERR_QUEUE_FULL when queue_max mails are already waiting, SC_ERR_TOO_LONG for a
 * message that does not fit message_max or a recipient or subject past its own bound, and
 * SC_ERR_MALFORMED for a subject that could not go into a header at all. The caller decides what
 * a full queue means, because only it knows whether this mail may be dropped or the work behind
 * it must stop.
 */
sc_status sc_mail_enqueue(sc_mailer *mailer, const sc_mail *mail);

/**
 * Sends everything that is waiting, on the calling thread. Only for a mailer built with
 * `workers = 0`; with workers running the queue belongs to them and this answers
 * SC_ERR_UNAVAILABLE rather than racing them for it.
 *
 * @p sent and @p failed are optional and count what happened in this round. A mail that fails
 * does not stop the round: a 450 or 550 on one recipient is per-transaction and the session
 * survives it, so the walk continues. A failure is put back for its one retry and is therefore
 * neither sent nor failed yet -- it is picked up by the next flush that runs after the delay.
 *
 * Answers SC_OK when the round completed, whatever the individual outcomes, and SC_ERR_NETWORK
 * when nothing could be sent at all.
 */
sc_status sc_mail_flush(sc_mailer *mailer, uint32_t *sent, uint32_t *failed);

/**
 * Waits until nothing is queued and no worker is sending, or until @p timeout_ms has passed.
 *
 * What a shutdown and a test want. Answers SC_OK when the queue really did empty and
 * SC_ERR_NETWORK on the timeout. A mail waiting out its retry delay keeps this waiting too, so
 * a timeout shorter than retry_delay_ms will report one whenever a retry is pending.
 *
 * With `workers = 0` this only reports whether the queue is empty; it cannot empty it, because
 * nothing would be running.
 */
sc_status sc_mail_drain(sc_mailer *mailer, int64_t timeout_ms);

/** Enqueue, then wait for the queue to empty. Blocks, and waits for the other mails in the
 *  queue as well -- for one mail and nothing else, that is the same thing. */
sc_status sc_mail_send(sc_mailer *mailer, const sc_mail *mail);

/** Mails waiting for a worker, retries included. */
uint32_t sc_mail_pending(const sc_mailer *mailer);

/** Everything the mailer knows about itself. @p out is written in full. */
void sc_mail_get_stats(const sc_mailer *mailer, sc_mail_stats *out);

#endif /* SERVICE_CORE_EMAIL_MAILER_H */
