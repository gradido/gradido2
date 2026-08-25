/*
 * The mail client. service_core/mail.h is the specification; this file is its transcription, and
 * the reasoning for the shape -- held sessions, a growing worker pool, one retry, one recipient
 * -- lives there rather than being repeated here.
 *
 * Four things are worth knowing before changing anything below.
 *
 * **A worker is a connection.** A CURL easy handle belongs to one thread; libcurl says so and
 * means it. So a worker owns its handle for as long as it lives, and starting a second worker is
 * how a second session to the relay comes about. Nothing here reconnects -- curl replaces a
 * connection idle for two minutes and rejects one it finds dead before reusing it, so a mail
 * after an hour of silence arrives over a connection nobody asked for.
 *
 * **Nothing allocates after startup.** sc_mailer_create() takes four blocks from the host -- the
 * queue ring, the retry ring, the message pool and the mailer itself -- and that is the last
 * time the host is asked. A full queue answers SC_ERR_QUEUE_FULL, never a larger block.
 *
 * **The queue is a ring, and it used to be an arnm_bvec.** The bucket vector was right while a
 * flush emptied everything at once: clear() kept the buckets warm and the arena behind it could
 * be reset wholesale. Retry ended that. A mail that comes back has to outlive the round it
 * failed in, so there is no longer a moment when everything is dead together -- and an
 * append-only container consumed continuously grows without bound, which is what the house's
 * fixed-size rule exists to prevent. A bounded FIFO is a ring; the arena that could only free at
 * its tail became an arnm_fixed_arena_pool, which hands one arena per queued message out and
 * takes it back in any order.
 *
 * **curl does the dot stuffing.** See lib/smtp.c, cr_eob_read(). Rendering must not, and must
 * not append the terminating dot line either.
 *
 * Locking, in one line so that it stays describable in one: `lock` covers both rings, the pool,
 * the counters and every worker's bookkeeping; it is never held across curl_easy_perform(); and
 * there is no second lock.
 */
#include "service_core/mail.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <curl/curl.h>
#include <uv.h>

#include "arnm/arena.h"
#include "arnm/fixed_arena_pool.h"
#include "arnm/memory.h"

#include "service_core/atomic.h"
#include "service_core/log.h"

/* A Date: header, "Mon, 25 Aug 2026 10:20:30 +0000", plus room. */
#define SC_MAIL_DATE_MAX 48
/* "seq.unixms@domain" */
#define SC_MAIL_MSGID_MAX (64 + SC_MAIL_ADDR_MAX)
/*
 * How long an idle worker sleeps before looking at the quit flag and its own retirement clock
 * again. It is the shutdown latency of a mailer, so it is short; it is also a wakeup per idle
 * worker, so it is not shorter. The same number and the same reasoning as SC_RUNTIME_TICK_MS.
 */
#define SC_MAIL_TICK_MS 100

/*
 * One queued mail.
 *
 * The recipient is a fixed buffer rather than a pointer because the curl_slist below points at
 * it and because an address is bounded by the protocol anyway. `message` points into the arena
 * this entry borrowed from the pool, and `arena` is how that borrow is given back.
 */
typedef struct sc_mail_entry {
    char to[SC_MAIL_ADDR_MAX];
    char msgid[SC_MAIL_MSGID_MAX];
    const char *message;
    size_t message_len;
    /* The pool arena holding `message`. Returned when the mail is finally done, whichever way. */
    arnm *arena;
    /* 0 for a mail that has not been tried, 1 for one that is waiting out its retry. */
    uint32_t attempts;
    /* Unix ms before which this must not be tried again. 0 for a fresh mail. */
    int64_t not_before_ms;
    /*
     * The recipient list, built here instead of with curl_slist_append().
     *
     * CURLOPT_MAIL_RCPT is documented as taking "a fully valid list of struct curl_slist
     * structs" which curl neither copies nor frees -- "the list is not copied, so it must be
     * kept around until the transfer is done". With exactly one recipient that list is one node,
     * and a node built here costs nothing where curl_slist_append() would cost two mallocs per
     * mail. `data` points at `to` above, in the same entry, so the two have the same lifetime by
     * construction -- which is also why every copy of an entry has to repoint it.
     */
    struct curl_slist rcpt;
} sc_mail_entry;

/*
 * A bounded FIFO over a fixed array. Both rings are this.
 *
 * The retry ring comes out ordered by not_before_ms for free, because every retry waits the same
 * delay -- which is what lets a worker decide whether anything is due by looking at the head
 * alone.
 */
typedef struct sc_mail_ring {
    sc_mail_entry *slot;
    uint32_t capacity;
    uint32_t head;
    uint32_t count;
} sc_mail_ring;

typedef enum sc_mail_worker_state {
    SC_MAIL_WORKER_FREE = 0,
    SC_MAIL_WORKER_RUNNING,
    /* The thread has left its loop but has not been joined. Its slot is reusable only after it
     * is -- an unjoined thread that exited is a handle the process goes on holding. */
    SC_MAIL_WORKER_RETIRED
} sc_mail_worker_state;

typedef struct sc_mail_worker {
    sc_mailer *mailer;
    uv_thread_t thread;
    uint32_t index;
    sc_mail_worker_state state;
    /* Unix ms since which this worker has been busy without a pause, or 0 while it is idle. What
     * the decision to start another worker is made on. */
    int64_t busy_since_ms;
    /* Unix ms since which it has had nothing to do, or 0 while it is busy. */
    int64_t idle_since_ms;
} sc_mail_worker;

struct sc_mailer {
    char url[SC_MAIL_URL_MAX];
    char from[SC_MAIL_ADDR_MAX];
    char from_name[SC_MAIL_SUBJECT_MAX];
    char user[SC_MAIL_USER_MAX];
    char pass[SC_MAIL_PASS_MAX];
    char cainfo[SC_MAIL_PATH_MAX];
    /* The domain out of `from`, for the Message-ID. Without an '@' it is "localhost". */
    char msgid_domain[SC_MAIL_ADDR_MAX];

    int starttls;
    int scan_ca_path;
    int insecure;
    long timeout_ms;
    uint32_t message_max;
    int64_t retry_delay_ms;
    int64_t spawn_after_ms;
    int64_t linger_ms;
    uint32_t spawn_backlog;
    uint32_t worker_max;

    /* Covers everything below it, and nothing is held across a send. */
    uv_mutex_t lock;
    int lock_ready;
    /* Raised when a mail is queued or the mailer is stopping. What an idle worker sleeps on. */
    uv_cond_t work;
    int work_ready;
    /* Raised whenever a mail leaves the system. What sc_mail_drain waits on. */
    uv_cond_t idle;
    int idle_ready;

    sc_mail_ring queue;
    sc_mail_ring retry;
    /* One arena per queued message. Not thread safe by design -- "a pool per thread is the
     * intended shape", says its header -- so every call into it happens under `lock`. */
    arnm_fixed_arena_pool pool;
    int pool_ready;

    sc_mail_worker worker[SC_MAIL_WORKERS_MAX];
    uint32_t worker_live; /* slots in RUNNING */
    uint32_t worker_busy; /* of those, currently inside a send */
    volatile int32_t stopping;

    /* The handle for `workers = 0`, used only on the caller's thread by sc_mail_flush. NULL
     * whenever workers are running, so the two modes cannot quietly share one. */
    CURL *flush_handle;

    uint64_t queued;
    uint64_t sent;
    uint64_t retried;
    uint64_t failed;
    uint64_t sequence;
};

/* ------------------------------------------------------------------ *
 * small helpers
 * ------------------------------------------------------------------ */

static int arnm_ok(arnm_result result)
{
    return result == ARNM_SUCCESS;
}

/*
 * curl_global_init(), exactly once per process.
 *
 * curl_easy_init() will call it on its own if nobody did, and the documentation is explicit that
 * doing so is not thread safe -- which matters more here than it did before, because every
 * worker creates its own handle and they start together. uv_once is what the cache's hash seed
 * uses for the same reason and is already linked here.
 *
 * There is no matching curl_global_cleanup(); see the note on sc_mailer_destroy().
 */
static uv_once_t g_curl_once = UV_ONCE_INIT;

static void curl_init_once(void)
{
    (void)curl_global_init(CURL_GLOBAL_ALL);
}

/**
 * Copies @p src into a fixed field, answering SC_ERR_TOO_LONG rather than truncating.
 *
 * A truncated host name connects to a different machine and a truncated address delivers to a
 * different person, which is why this is the one convention the whole file uses. NULL is a valid
 * input and leaves the field empty.
 */
static sc_status copy_field(char *dst, size_t cap, const char *src)
{
    size_t len;
    if (src == NULL) {
        dst[0] = '\0';
        return SC_OK;
    }
    len = strlen(src);
    if (len >= cap)
        return SC_ERR_TOO_LONG;
    memcpy(dst, src, len + 1);
    return SC_OK;
}

/*
 * The day and month names of RFC 5322, spelled out rather than taken from strftime().
 *
 * %a and %b are locale dependent, and a process started under de_DE would put "Mo, 25 Aug" in a
 * header whose grammar allows exactly seven day names, all English. It would be accepted by most
 * receivers and rejected by some, which is the worst kind of bug to find.
 */
static const char *const k_day_name[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
static const char *const k_month_name[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                             "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

/** Writes an RFC 5322 date-time in UTC, e.g. "Mon, 25 Aug 2026 10:20:30 +0000". */
static void render_date(char *dst, size_t cap, time_t when)
{
    struct tm tm_buf;
    const struct tm *tm;
#if defined(_WIN32)
    tm = (gmtime_s(&tm_buf, &when) == 0) ? &tm_buf : NULL;
#else
    tm = gmtime_r(&when, &tm_buf);
#endif
    if (tm == NULL) {
        /* A clock this broken is not worth failing a mail over; the receiver stamps its own
         * Received: header either way. */
        snprintf(dst, cap, "Thu, 01 Jan 1970 00:00:00 +0000");
        return;
    }
    snprintf(dst, cap, "%s, %02d %s %04d %02d:%02d:%02d +0000", k_day_name[tm->tm_wday % 7],
             tm->tm_mday, k_month_name[tm->tm_mon % 12], tm->tm_year + 1900, tm->tm_hour,
             tm->tm_min, tm->tm_sec);
}

/*
 * Length @p body takes once every bare LF has become CRLF.
 *
 * A CR already followed by an LF is left alone; a lone CR becomes CRLF as well, because a bare CR
 * is not a line ending anything downstream agrees on.
 */
static size_t crlf_length(const char *body)
{
    size_t n = 0;
    for (const char *p = body; *p != '\0'; p++) {
        if (*p == '\r') {
            n += 2;
            if (p[1] == '\n')
                p++;
        } else if (*p == '\n') {
            n += 2;
        } else {
            n++;
        }
    }
    return n;
}

/** Writes @p body into @p dst with CRLF line endings. Returns what it wrote. */
static size_t crlf_copy(char *dst, const char *body)
{
    size_t n = 0;
    for (const char *p = body; *p != '\0'; p++) {
        if (*p == '\r' || *p == '\n') {
            dst[n++] = '\r';
            dst[n++] = '\n';
            if (*p == '\r' && p[1] == '\n')
                p++;
        } else {
            dst[n++] = *p;
        }
    }
    return n;
}

/* ------------------------------------------------------------------ *
 * the rings
 * ------------------------------------------------------------------ */

static int ring_init(sc_mail_ring *ring, uint32_t capacity)
{
    ring->slot = calloc(capacity, sizeof *ring->slot);
    ring->capacity = capacity;
    ring->head = 0;
    ring->count = 0;
    return ring->slot != NULL;
}

static void ring_free(sc_mail_ring *ring)
{
    free(ring->slot);
    ring->slot = NULL;
    ring->capacity = 0;
    ring->count = 0;
}

/** Appends a copy of @p entry. False when the ring is full; the caller has lost nothing. */
static int ring_push(sc_mail_ring *ring, const sc_mail_entry *entry)
{
    sc_mail_entry *dst;
    if (ring->count >= ring->capacity)
        return 0;
    dst = &ring->slot[(ring->head + ring->count) % ring->capacity];
    *dst = *entry;
    /* The list node points into the entry it lives in, so a copy has to be repointed or it would
     * name the address of whatever the source entry was. */
    dst->rcpt.data = dst->to;
    dst->rcpt.next = NULL;
    ring->count++;
    return 1;
}

/** Takes the oldest entry out. False when the ring is empty. */
static int ring_pop(sc_mail_ring *ring, sc_mail_entry *out)
{
    if (ring->count == 0)
        return 0;
    *out = ring->slot[ring->head];
    out->rcpt.data = out->to;
    out->rcpt.next = NULL;
    ring->head = (ring->head + 1) % ring->capacity;
    ring->count--;
    return 1;
}

static const sc_mail_entry *ring_peek(const sc_mail_ring *ring)
{
    return ring->count > 0 ? &ring->slot[ring->head] : NULL;
}

/* ------------------------------------------------------------------ *
 * rendering
 * ------------------------------------------------------------------ */

/*
 * Builds the RFC 5322 message for @p mail into @p arena and fills in @p entry.
 *
 * Runs *outside* the queue lock: the arena was handed over by the pool and belongs to this
 * thread until the entry is queued, so several callers can render at once and only the push
 * itself is serialised.
 *
 * Two passes: snprintf against a zero-length buffer measures the header block exactly, the body
 * is measured by crlf_length(), and only then is anything taken from the arena -- which cannot
 * grow, so asking it for the right size the first time is the whole game.
 *
 * The Message-ID is assigned here and survives the retry. That is the point of it: the same mail
 * delivered twice under two identities is a duplicate in someone's inbox, and the retry is
 * exactly the path that could produce one.
 */
static sc_status render(sc_mailer *mailer, const sc_mail *mail, arnm *arena, uint64_t sequence,
                        sc_mail_entry *entry)
{
    char date[SC_MAIL_DATE_MAX];
    const char *fmt;
    int header_len;
    size_t body_len;
    size_t total;
    size_t written;
    uint8_t *buffer = NULL;

    /* One clock for both, and it is the log's. time() beside sc_now_ms() had the Date: header and
     * the Message-ID a second apart whenever the two calls straddled a tick -- harmless, and
     * exactly the kind of thing that wastes an afternoon when someone correlates a log line with
     * a delivered mail. */
    const int64_t now_ms = sc_now_ms();
    render_date(date, sizeof date, (time_t)(now_ms / 1000));
    snprintf(entry->msgid, sizeof entry->msgid, "%llu.%lld@%s", (unsigned long long)sequence,
             (long long)now_ms, mailer->msgid_domain);

    /*
     * No 8-bit transfer encoding is declared: the body is announced as UTF-8 text and handed to
     * the relay as it is. Every relay this project will speak to advertises 8BITMIME, and a
     * quoted-printable encoder is a thing to add when one of them does not, not before.
     */
    fmt = mailer->from_name[0] != '\0'
              ? "Date: %s\r\n"
                "From: %s <%s>\r\n"
                "To: %s\r\n"
                "Subject: %s\r\n"
                "Message-ID: <%s>\r\n"
                "MIME-Version: 1.0\r\n"
                "Content-Type: text/plain; charset=utf-8\r\n"
                "\r\n"
              : "Date: %s\r\n"
                "From: %s%s\r\n"
                "To: %s\r\n"
                "Subject: %s\r\n"
                "Message-ID: <%s>\r\n"
                "MIME-Version: 1.0\r\n"
                "Content-Type: text/plain; charset=utf-8\r\n"
                "\r\n";

    header_len = snprintf(NULL, 0, fmt, date, mailer->from_name, mailer->from, entry->to,
                          mail->subject, entry->msgid);
    if (header_len < 0)
        return SC_ERR_MALFORMED;

    body_len = crlf_length(mail->body);
    /* The header block, the body, and the CRLF that closes the last line if the body did not.
     * curl appends the ".\r\n" after it; see the note at the top of this file. */
    total = (size_t)header_len + body_len + 2;
    /* >= and not >, because the request below is total + 1: at exactly UINT32_MAX that wraps to
     * a zero-byte allocation an arena is happy to grant, and the write then goes past the end of
     * a block that was never there. */
    if (total >= UINT32_MAX || total + 1 > mailer->message_max)
        return SC_ERR_TOO_LONG;

    if (!arnm_ok(arnm_alloc(&buffer, (uint32_t)(total + 1), arena)))
        return SC_ERR_TOO_LONG;

    snprintf((char *)buffer, (size_t)header_len + 1, fmt, date, mailer->from_name, mailer->from,
             entry->to, mail->subject, entry->msgid);
    written = (size_t)header_len;
    written += crlf_copy((char *)buffer + written, mail->body);
    if (written < 2 || buffer[written - 1] != '\n') {
        buffer[written++] = '\r';
        buffer[written++] = '\n';
    }
    buffer[written] = '\0';

    entry->message = (const char *)buffer;
    entry->message_len = written;
    entry->arena = arena;
    entry->attempts = 0;
    entry->not_before_ms = 0;
    entry->rcpt.data = entry->to;
    entry->rcpt.next = NULL;
    return SC_OK;
}

/* ------------------------------------------------------------------ *
 * the transfer
 * ------------------------------------------------------------------ */

/* What curl reads the body through: a cursor over the rendered message, no copy. */
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

/**
 * Sends one entry through @p handle, which belongs to the calling thread and to no other.
 *
 * Every option is set per mail rather than once, because CURLOPT_MAIL_RCPT and CURLOPT_READDATA
 * change per mail anyway and a handle half configured at two different times is how an option
 * survives into a mail it was not meant for. curl_easy_reset keeps what matters: the
 * documentation lists live connections, the DNS cache and the session ID cache among the things
 * it does *not* touch, which is the whole reason the handle is worth keeping.
 *
 * Called with no lock held. It blocks for as long as the relay takes.
 */
static sc_status send_entry(CURL *handle, const sc_mailer *mailer, const sc_mail_entry *entry)
{
    sc_mail_reader reader = {entry->message, entry->message_len, 0};
    CURLcode rc;

    curl_easy_reset(handle);
    curl_easy_setopt(handle, CURLOPT_URL, mailer->url);
    curl_easy_setopt(handle, CURLOPT_MAIL_FROM, mailer->from);
    curl_easy_setopt(handle, CURLOPT_MAIL_RCPT, &entry->rcpt);
    curl_easy_setopt(handle, CURLOPT_UPLOAD, 1L);
    curl_easy_setopt(handle, CURLOPT_READFUNCTION, read_body);
    curl_easy_setopt(handle, CURLOPT_READDATA, &reader);
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, discard);
    curl_easy_setopt(handle, CURLOPT_INFILESIZE_LARGE, (curl_off_t)entry->message_len);
    curl_easy_setopt(handle, CURLOPT_TIMEOUT_MS, mailer->timeout_ms);
    /*
     * Signals are how libcurl's own timeouts work when it has no threaded resolver, and a
     * library that installs a SIGALRM handler from a worker thread is a library fighting the
     * process for it. curl's own documentation calls NOSIGNAL mandatory for multi-threaded use.
     */
    curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);

    if (mailer->starttls == 1)
        curl_easy_setopt(handle, CURLOPT_USE_SSL, (long)CURLUSESSL_TRY);
    else if (mailer->starttls >= 2)
        curl_easy_setopt(handle, CURLOPT_USE_SSL, (long)CURLUSESSL_ALL);

    /*
     * The CA directory, off unless asked for. curl compiles /etc/ssl/certs in as CURLOPT_CAPATH
     * and reads every file in it for every new connection -- 150 certificates parsed again each
     * time, measured at 8,5 ms of an 11,5 ms cold TLS send. Setting it to NULL is what switches
     * that off; CURLOPT_SSL_VERIFYPEER does not, because curl decides where to look before it
     * decides whether to look.
     */
    if (!mailer->scan_ca_path)
        curl_easy_setopt(handle, CURLOPT_CAPATH, (const char *)NULL);
    if (mailer->cainfo[0] != '\0')
        curl_easy_setopt(handle, CURLOPT_CAINFO, mailer->cainfo);
    if (mailer->insecure) {
        curl_easy_setopt(handle, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(handle, CURLOPT_SSL_VERIFYHOST, 0L);
    }
    if (mailer->user[0] != '\0')
        curl_easy_setopt(handle, CURLOPT_USERNAME, mailer->user);
    if (mailer->pass[0] != '\0')
        curl_easy_setopt(handle, CURLOPT_PASSWORD, mailer->pass);

    rc = curl_easy_perform(handle);
    if (rc != CURLE_OK) {
        /* Warn and not error: this may still be the first of two attempts, and an error line for
         * something that is about to succeed is an alert nobody can act on. finish_entry() logs
         * the error when there is no attempt left. */
        sc_log_warn(SC_CAT_MAIL, "mail.attempt.failed", "%s <%s>: %s", entry->to, entry->msgid,
                    curl_easy_strerror(rc));
        return SC_ERR_NETWORK;
    }
    sc_log_debug(SC_CAT_MAIL, "mail.send.ok", "%s <%s>, %zu bytes", entry->to, entry->msgid,
                 entry->message_len);
    return SC_OK;
}

/* ------------------------------------------------------------------ *
 * what happens to an entry when its attempt is over -- lock held
 * ------------------------------------------------------------------ */

/**
 * Books the outcome of one attempt and disposes of @p entry accordingly.
 *
 * Success and final failure both end the mail: the arena goes back to the pool and the entry is
 * gone. A first failure goes into the retry ring with a not-before stamp and keeps its arena,
 * which is the reason the pool exists rather than one arena reset per round.
 *
 * The retry ring holds queue_max entries, the same as the queue, so it cannot refuse -- every
 * mail that could possibly be in flight fits. The branch that says otherwise is there because a
 * silent drop would be indistinguishable from a delivered mail.
 */
static void finish_entry(sc_mailer *mailer, sc_mail_entry *entry, sc_status outcome)
{
    if (outcome == SC_OK) {
        mailer->sent++;
    } else if (entry->attempts == 0) {
        entry->attempts = 1;
        entry->not_before_ms = sc_now_ms() + mailer->retry_delay_ms;
        if (ring_push(&mailer->retry, entry)) {
            mailer->retried++;
            return;
        }
        sc_log_error(SC_CAT_MAIL, "mail.retry.no_room", "%s <%s> dropped: retry ring full",
                     entry->to, entry->msgid);
        mailer->failed++;
    } else {
        /* The second attempt failed. This is the end of the line, and it is the line that has to
         * be findable afterwards -- it is the only record that this mail ever existed. */
        sc_log_error(SC_CAT_MAIL, "mail.failed", "%s <%s> given up after 2 attempts", entry->to,
                     entry->msgid);
        mailer->failed++;
    }
    (void)arnm_fixed_arena_pool_free(&mailer->pool, entry->arena);
    entry->arena = NULL;
}

/**
 * Takes the next entry that may be tried now, preferring a due retry over a fresh mail.
 *
 * @p wait_ms is set to how long the caller should sleep when nothing is due: the time until the
 * oldest retry becomes due, capped at a tick so that the stop flag and the retirement clock are
 * still looked at. Lock held throughout.
 */
static int take_due(sc_mailer *mailer, sc_mail_entry *out, int64_t *wait_ms)
{
    const sc_mail_entry *head = ring_peek(&mailer->retry);
    const int64_t now = sc_now_ms();

    *wait_ms = SC_MAIL_TICK_MS;
    if (head != NULL) {
        if (head->not_before_ms <= now)
            return ring_pop(&mailer->retry, out);
        /* Every retry waits the same delay, so the ring is ordered by not_before_ms and the head
         * is the earliest of them. Nothing behind it can be due while it is not. */
        const int64_t until = head->not_before_ms - now;
        if (until < *wait_ms)
            *wait_ms = until;
    }
    return ring_pop(&mailer->queue, out);
}

static uint32_t pending_locked(const sc_mailer *mailer)
{
    return mailer->queue.count + mailer->retry.count;
}

/* ------------------------------------------------------------------ *
 * workers
 * ------------------------------------------------------------------ */

static void worker_main(void *arg);

/**
 * Starts a worker in slot @p index. Lock held.
 *
 * A slot holding a retired thread is joined before it is reused: that thread has already left
 * its loop, so this returns at once, but skipping it would leak a thread handle per retirement
 * and the process would run out of those long before it ran out of anything else.
 */
static int worker_start(sc_mailer *mailer, uint32_t index)
{
    sc_mail_worker *worker = &mailer->worker[index];

    if (worker->state == SC_MAIL_WORKER_RETIRED) {
        (void)uv_thread_join(&worker->thread);
        worker->state = SC_MAIL_WORKER_FREE;
    }
    if (worker->state != SC_MAIL_WORKER_FREE)
        return 0;

    worker->mailer = mailer;
    worker->index = index;
    worker->busy_since_ms = 0;
    worker->idle_since_ms = sc_now_ms();
    worker->state = SC_MAIL_WORKER_RUNNING;
    mailer->worker_live++;
    if (uv_thread_create(&worker->thread, worker_main, worker) != 0) {
        worker->state = SC_MAIL_WORKER_FREE;
        mailer->worker_live--;
        sc_log_error(SC_CAT_MAIL, "mail.worker.start_failed", "worker %u could not be started",
                     index);
        return 0;
    }
    sc_log_debug(SC_CAT_MAIL, "mail.worker.started", "worker %u, %u alive", index,
                 mailer->worker_live);
    return 1;
}

/**
 * Starts another worker if the ones already there cannot keep up. Lock held; called after a push.
 *
 * The condition is the one asked for: every worker busy, the longest-running of them busy without
 * a pause for spawn_after_ms, and a backlog still waiting behind them. A worker that is free
 * means the queue is not the problem, and another thread would only open a connection the relay
 * has to hold for nothing.
 */
static void maybe_spawn(sc_mailer *mailer)
{
    int64_t oldest_busy = 0;
    uint32_t free_slot = SC_MAIL_WORKERS_MAX;

    if (mailer->worker_live == 0 || mailer->worker_live >= mailer->worker_max)
        return;
    if (pending_locked(mailer) < mailer->spawn_backlog)
        return;

    for (uint32_t i = 0; i < SC_MAIL_WORKERS_MAX; i++) {
        const sc_mail_worker *worker = &mailer->worker[i];
        if (worker->state == SC_MAIL_WORKER_RUNNING) {
            if (worker->busy_since_ms == 0)
                return; /* someone is free; the queue will be looked at without a new thread */
            if (oldest_busy == 0 || worker->busy_since_ms < oldest_busy)
                oldest_busy = worker->busy_since_ms;
        } else if (free_slot == SC_MAIL_WORKERS_MAX) {
            free_slot = i;
        }
    }
    if (free_slot == SC_MAIL_WORKERS_MAX)
        return;
    if (sc_now_ms() - oldest_busy < mailer->spawn_after_ms)
        return;

    if (worker_start(mailer, free_slot)) {
        sc_log_info(SC_CAT_MAIL, "mail.worker.scaled_up", "%u waiting, %u workers",
                    pending_locked(mailer), mailer->worker_live);
    }
}

/** Leaves the worker loop: gives the slot back and wakes whoever might have been waiting on it. */
static void worker_retire(sc_mail_worker *worker, CURL *handle)
{
    sc_mailer *mailer = worker->mailer;

    worker->state = SC_MAIL_WORKER_RETIRED;
    worker->busy_since_ms = 0;
    mailer->worker_live--;
    sc_log_debug(SC_CAT_MAIL, "mail.worker.retired", "worker %u, %u alive", worker->index,
                 mailer->worker_live);
    /* A retiring worker may have been the last one a drain was waiting on. */
    uv_cond_broadcast(&mailer->idle);
    uv_mutex_unlock(&mailer->lock);

    if (handle != NULL)
        curl_easy_cleanup(handle);
}

/*
 * One worker: take a mail, send it without the lock, book the outcome, repeat.
 *
 * Worker 0 is the permanent one and never retires -- it goes back to sleeping on the condition
 * variable, which costs nothing while there is no mail. Every other worker retires after
 * linger_ms without work, giving its connection back to the relay.
 */
static void worker_main(void *arg)
{
    sc_mail_worker *worker = arg;
    sc_mailer *mailer = worker->mailer;
    CURL *handle = curl_easy_init();

    uv_mutex_lock(&mailer->lock);

    if (handle == NULL) {
        /* Without a handle this thread can do nothing but occupy a slot, and pretending
         * otherwise would have it spin on a queue it cannot drain. */
        sc_log_error(SC_CAT_MAIL, "mail.worker.no_handle", "worker %u has no curl handle",
                     worker->index);
        worker_retire(worker, NULL);
        return;
    }

    for (;;) {
        sc_mail_entry entry;
        int64_t wait_ms = SC_MAIL_TICK_MS;
        sc_status outcome;

        if (sc_atomic_load(&mailer->stopping))
            break;

        if (!take_due(mailer, &entry, &wait_ms)) {
            if (worker->busy_since_ms != 0) {
                worker->busy_since_ms = 0;
                worker->idle_since_ms = sc_now_ms();
            }
            /* A worker started on demand gives its slot and its connection back once the burst it
             * was started for is over. Worker 0 stays, whatever happens. */
            if (worker->index != 0 && mailer->worker_live > 1 &&
                sc_now_ms() - worker->idle_since_ms >= mailer->linger_ms)
                break;
            (void)uv_cond_timedwait(&mailer->work, &mailer->lock,
                                    (uint64_t)wait_ms * UINT64_C(1000000));
            continue;
        }

        if (worker->busy_since_ms == 0)
            worker->busy_since_ms = sc_now_ms();
        worker->idle_since_ms = 0;
        mailer->worker_busy++;
        /*
         * Asked here and not only in sc_mail_enqueue, because a producer that drops two hundred
         * mails at once and then goes quiet would otherwise never see a second worker: at the
         * moment of the last push nobody has been busy for spawn_after_ms yet, and after it
         * nothing asks again. This is the point where "a worker has been at it a while and there
         * is still a backlog" actually becomes true.
         */
        maybe_spawn(mailer);
        uv_mutex_unlock(&mailer->lock);

        outcome = send_entry(handle, mailer, &entry);

        uv_mutex_lock(&mailer->lock);
        mailer->worker_busy--;
        finish_entry(mailer, &entry, outcome);
        /* Both, and for different waiters: a retry that went back on the ring is work for
         * somebody, and a mail that left the system may be the last one a drain waited for. */
        uv_cond_signal(&mailer->work);
        uv_cond_broadcast(&mailer->idle);
    }

    worker_retire(worker, handle);
}

/* ------------------------------------------------------------------ *
 * lifetime
 * ------------------------------------------------------------------ */

/** Half the machine's cores minus one, and never outside 1..SC_MAIL_WORKERS_MAX. */
static uint32_t default_worker_max(void)
{
    const uint32_t half = (uint32_t)uv_available_parallelism() / 2u;
    const uint32_t max = half > 1u ? half - 1u : 1u;
    return max > SC_MAIL_WORKERS_MAX ? SC_MAIL_WORKERS_MAX : max;
}

/**
 * Gives every arena still held by a queued mail back to the pool. Lock not needed: called only
 * once the workers have been joined.
 *
 * arnm_fixed_arena_pool_release() refuses while anything is out, and it is right to -- but that
 * turns a queue that still holds mails at shutdown into a block nobody frees. So the rings are
 * emptied first.
 */
static void return_arenas(sc_mailer *mailer)
{
    sc_mail_ring *rings[2] = {&mailer->queue, &mailer->retry};
    for (size_t r = 0; r < 2; r++) {
        sc_mail_entry entry;
        while (ring_pop(rings[r], &entry)) {
            if (entry.arena != NULL)
                (void)arnm_fixed_arena_pool_free(&mailer->pool, entry.arena);
        }
    }
}

static void mailer_free(sc_mailer *mailer)
{
    if (mailer->flush_handle != NULL)
        curl_easy_cleanup(mailer->flush_handle);
    if (mailer->pool_ready) {
        return_arenas(mailer);
        (void)arnm_fixed_arena_pool_release(&mailer->pool, NULL);
    }
    ring_free(&mailer->queue);
    ring_free(&mailer->retry);
    if (mailer->idle_ready)
        uv_cond_destroy(&mailer->idle);
    if (mailer->work_ready)
        uv_cond_destroy(&mailer->work);
    if (mailer->lock_ready)
        uv_mutex_destroy(&mailer->lock);
    free(mailer);
}

sc_status sc_mailer_create(const sc_mail_config *config, sc_mailer **out)
{
    sc_mailer *mailer;
    sc_status status;
    const char *at;
    uint32_t queue_max;
    uint32_t workers;
    uint32_t started;

    if (config == NULL || out == NULL || config->url == NULL || config->from == NULL)
        return SC_ERR_INVALID_ARGUMENT;
    *out = NULL;

    uv_once(&g_curl_once, curl_init_once);

    mailer = calloc(1, sizeof *mailer);
    if (mailer == NULL)
        return SC_ERR_NO_MEMORY;

#define COPY(field, source)                                                                      \
    do {                                                                                         \
        status = copy_field(mailer->field, sizeof mailer->field, (source));                      \
        if (status != SC_OK) {                                                                   \
            sc_log_error(SC_CAT_MAIL, "mail.config.too_long", "%s does not fit", #field);         \
            mailer_free(mailer);                                                                 \
            return status;                                                                       \
        }                                                                                        \
    } while (0)
    COPY(url, config->url);
    COPY(from, config->from);
    COPY(from_name, config->from_name);
    COPY(user, config->user);
    COPY(pass, config->pass);
    COPY(cainfo, config->cainfo);
#undef COPY

    /* The Message-ID's right hand side. RFC 5322 wants a domain there, and the sender's is the
     * one this process can claim; a `from` without an '@' is not an address, but it is not this
     * function's job to reject it, so it gets a domain that resolves nowhere. */
    at = strchr(mailer->from, '@');
    (void)copy_field(mailer->msgid_domain, sizeof mailer->msgid_domain,
                     (at != NULL && at[1] != '\0') ? at + 1 : "localhost");

    mailer->starttls = config->starttls;
    mailer->scan_ca_path = config->scan_ca_path;
    mailer->insecure = config->insecure;
    mailer->timeout_ms = config->timeout_ms > 0 ? config->timeout_ms : SC_MAIL_TIMEOUT_DEFAULT_MS;
    mailer->message_max = config->message_max > 0 ? config->message_max : SC_MAIL_MESSAGE_DEFAULT;
    mailer->retry_delay_ms =
        config->retry_delay_ms > 0 ? config->retry_delay_ms : SC_MAIL_RETRY_DELAY_DEFAULT_MS;
    mailer->spawn_after_ms =
        config->spawn_after_ms > 0 ? config->spawn_after_ms : SC_MAIL_SPAWN_AFTER_DEFAULT_MS;
    mailer->linger_ms = config->linger_ms > 0 ? config->linger_ms : SC_MAIL_LINGER_DEFAULT_MS;
    mailer->spawn_backlog =
        config->spawn_backlog > 0 ? config->spawn_backlog : SC_MAIL_SPAWN_BACKLOG_DEFAULT;
    mailer->worker_max = config->worker_max > 0 ? config->worker_max : default_worker_max();
    if (mailer->worker_max > SC_MAIL_WORKERS_MAX)
        mailer->worker_max = SC_MAIL_WORKERS_MAX;

    queue_max = config->queue_max > 0 ? config->queue_max : SC_MAIL_QUEUE_DEFAULT;
    /* The pool counts its arenas in a uint16_t, and a queue that could not have one arena per
     * entry would refuse mails for a reason nobody could see in the config. */
    if (queue_max > UINT16_MAX)
        queue_max = UINT16_MAX;

    workers = config->workers;
    if (workers > mailer->worker_max)
        workers = mailer->worker_max;

    if (!ring_init(&mailer->queue, queue_max) || !ring_init(&mailer->retry, queue_max)) {
        mailer_free(mailer);
        return SC_ERR_NO_MEMORY;
    }
    /* One arena per queued message, all of them in one block, and the ceiling is known the moment
     * this returns: queue_max * message_max. NULL is the host allocator. */
    if (!arnm_ok(arnm_fixed_arena_pool_init(&mailer->pool, mailer->message_max,
                                            (uint16_t)queue_max, NULL))) {
        mailer_free(mailer);
        return SC_ERR_NO_MEMORY;
    }
    mailer->pool_ready = 1;

    /* One at a time, each recording that it succeeded, because mailer_free() destroys exactly
     * the ones that were created and a half-initialised mutex is not a thing it may touch. */
    if (uv_mutex_init(&mailer->lock) != 0) {
        mailer_free(mailer);
        return SC_ERR_NO_MEMORY;
    }
    mailer->lock_ready = 1;
    if (uv_cond_init(&mailer->work) != 0) {
        mailer_free(mailer);
        return SC_ERR_NO_MEMORY;
    }
    mailer->work_ready = 1;
    if (uv_cond_init(&mailer->idle) != 0) {
        mailer_free(mailer);
        return SC_ERR_NO_MEMORY;
    }
    mailer->idle_ready = 1;

    if (workers == 0) {
        /* No threads: the caller drives with sc_mail_flush on its own thread, and this is the
         * handle it uses. It exists only in this mode, so the two can never share one. */
        mailer->flush_handle = curl_easy_init();
        if (mailer->flush_handle == NULL) {
            mailer_free(mailer);
            return SC_ERR_NO_MEMORY;
        }
    }

    if (mailer->insecure) {
        sc_log_warn(SC_CAT_MAIL, "mail.insecure",
                    "certificate verification is off for %s; development only", mailer->url);
    }

    uv_mutex_lock(&mailer->lock);
    for (uint32_t i = 0; i < workers; i++) {
        if (!worker_start(mailer, i))
            break;
    }
    started = mailer->worker_live;
    uv_mutex_unlock(&mailer->lock);

    if (workers > 0 && started == 0) {
        sc_log_fatal(SC_CAT_MAIL, "mail.worker.none", "no mail worker could be started");
        mailer_free(mailer);
        return SC_ERR_NO_MEMORY;
    }

    sc_log_info(SC_CAT_MAIL, "mail.ready",
                "%s, from %s, queue %u x %u bytes, %u workers of at most %u", mailer->url,
                mailer->from, queue_max, mailer->message_max, started, mailer->worker_max);

    *out = mailer;
    return SC_OK;
}

/*
 * No curl_global_cleanup() here, and that is deliberate.
 *
 * It is a process-wide teardown, not a per-mailer one: calling it while another role still holds
 * a handle pulls libcurl out from under it. A second mailer created afterwards would also find
 * libcurl uninitialised while uv_once refuses to run the initialiser again. The memory it would
 * reclaim is freed by process exit anyway, which is the only moment at which the call would be
 * correct.
 */
void sc_mailer_destroy(sc_mailer *mailer)
{
    uv_thread_t joinable[SC_MAIL_WORKERS_MAX];
    uint32_t join_count = 0;
    uint32_t dropped;

    if (mailer == NULL)
        return;

    uv_mutex_lock(&mailer->lock);
    sc_atomic_store(&mailer->stopping, 1);
    dropped = pending_locked(mailer);
    /* Every worker is either waiting on this or inside a send; the waiters wake and leave, and
     * the senders find the flag when their mail is done. Nothing is cancelled mid-DATA -- a
     * transfer abandoned there is a mail the relay may or may not have taken. */
    uv_cond_broadcast(&mailer->work);
    for (uint32_t i = 0; i < SC_MAIL_WORKERS_MAX; i++) {
        if (mailer->worker[i].state != SC_MAIL_WORKER_FREE)
            joinable[join_count++] = mailer->worker[i].thread;
    }
    uv_mutex_unlock(&mailer->lock);

    for (uint32_t i = 0; i < join_count; i++)
        (void)uv_thread_join(&joinable[i]);

    if (dropped > 0) {
        sc_log_warn(SC_CAT_MAIL, "mail.dropped", "%u mails were still waiting", dropped);
    }
    mailer_free(mailer);
}

/* ------------------------------------------------------------------ *
 * the queue
 * ------------------------------------------------------------------ */

sc_status sc_mail_enqueue(sc_mailer *mailer, const sc_mail *mail)
{
    sc_mail_entry entry;
    arnm *arena = NULL;
    sc_status status;
    uint64_t sequence;

    if (mailer == NULL || mail == NULL || mail->to == NULL || mail->subject == NULL ||
        mail->body == NULL)
        return SC_ERR_INVALID_ARGUMENT;

    memset(&entry, 0, sizeof entry);
    status = copy_field(entry.to, sizeof entry.to, mail->to);
    if (status != SC_OK)
        return status;

    /* The pool is not thread safe, so borrowing and returning both happen under the lock.
     * Rendering does not: once an arena is ours it is ours alone, and holding the queue lock
     * across a copy of the body would serialise every producer behind the largest mail. */
    uv_mutex_lock(&mailer->lock);
    if (sc_atomic_load(&mailer->stopping)) {
        uv_mutex_unlock(&mailer->lock);
        return SC_ERR_UNAVAILABLE;
    }
    if (mailer->queue.count >= mailer->queue.capacity ||
        !arnm_ok(arnm_fixed_arena_pool_alloc(&mailer->pool, &arena))) {
        uv_mutex_unlock(&mailer->lock);
        return SC_ERR_QUEUE_FULL;
    }
    sequence = ++mailer->sequence;
    uv_mutex_unlock(&mailer->lock);

    status = render(mailer, mail, arena, sequence, &entry);

    uv_mutex_lock(&mailer->lock);
    /* Checked again, because the lock was let go for the rendering and a destroy may have run in
     * that window. Pushing after it would put a mail on a queue nobody will look at again, and
     * maybe_spawn() below would start a thread destroy has already finished collecting for
     * joining -- which is a thread that outlives the mailer it points at. */
    if (sc_atomic_load(&mailer->stopping))
        status = SC_ERR_UNAVAILABLE;
    if (status != SC_OK || !ring_push(&mailer->queue, &entry)) {
        /* Either the message did not fit its arena, the mailer is stopping, or the queue filled
         * up while this thread was rendering. All give the arena straight back; nothing
         * half-built is left behind. */
        (void)arnm_fixed_arena_pool_free(&mailer->pool, arena);
        uv_mutex_unlock(&mailer->lock);
        return status != SC_OK ? status : SC_ERR_QUEUE_FULL;
    }
    mailer->queued++;
    maybe_spawn(mailer);
    uv_cond_signal(&mailer->work);
    uv_mutex_unlock(&mailer->lock);
    return SC_OK;
}

sc_status sc_mail_flush(sc_mailer *mailer, uint32_t *sent, uint32_t *failed)
{
    uint32_t ok_count = 0;
    uint32_t fail_count = 0;
    uint32_t attempted = 0;

    if (sent != NULL)
        *sent = 0;
    if (failed != NULL)
        *failed = 0;
    if (mailer == NULL)
        return SC_ERR_INVALID_ARGUMENT;
    /* The workers own the queue. Two threads pulling from it would be fine; this one also owning
     * a handle the workers know nothing about is not the kind of sharing that stays correct. */
    if (mailer->flush_handle == NULL)
        return SC_ERR_UNAVAILABLE;

    for (;;) {
        sc_mail_entry entry;
        int64_t wait_ms = 0;
        uint32_t attempts_before;
        sc_status outcome;
        int have;

        uv_mutex_lock(&mailer->lock);
        have = take_due(mailer, &entry, &wait_ms);
        uv_mutex_unlock(&mailer->lock);
        if (!have)
            break;

        attempted++;
        attempts_before = entry.attempts;
        outcome = send_entry(mailer->flush_handle, mailer, &entry);

        uv_mutex_lock(&mailer->lock);
        finish_entry(mailer, &entry, outcome);
        uv_mutex_unlock(&mailer->lock);

        if (outcome == SC_OK)
            ok_count++;
        else if (attempts_before != 0)
            /* The retry failed too, so this one is over. A first failure is neither sent nor
             * failed yet -- it is on the retry ring, and the next flush after the delay decides. */
            fail_count++;
    }

    if (sent != NULL)
        *sent = ok_count;
    if (failed != NULL)
        *failed = fail_count;
    if (attempted > 0) {
        sc_log_info(SC_CAT_MAIL, "mail.flush", "%u attempted, %u sent, %u waiting", attempted,
                    ok_count, sc_mail_pending(mailer));
    }
    /* Nothing at all got through: the relay, not the mails. */
    return (attempted > 0 && ok_count == 0) ? SC_ERR_NETWORK : SC_OK;
}

sc_status sc_mail_drain(sc_mailer *mailer, int64_t timeout_ms)
{
    int64_t deadline;
    sc_status status = SC_OK;

    if (mailer == NULL)
        return SC_ERR_INVALID_ARGUMENT;
    deadline = sc_now_ms() + (timeout_ms > 0 ? timeout_ms : 0);

    uv_mutex_lock(&mailer->lock);
    while (pending_locked(mailer) > 0 || mailer->worker_busy > 0) {
        const int64_t left = deadline - sc_now_ms();
        if (left <= 0) {
            status = SC_ERR_NETWORK;
            break;
        }
        /* Nothing is running in flush mode, so waiting could only ever time out. Say so at once
         * rather than after the caller's timeout has passed. */
        if (mailer->flush_handle != NULL) {
            status = SC_ERR_NETWORK;
            break;
        }
        (void)uv_cond_timedwait(&mailer->idle, &mailer->lock, (uint64_t)left * UINT64_C(1000000));
    }
    uv_mutex_unlock(&mailer->lock);
    return status;
}

sc_status sc_mail_send(sc_mailer *mailer, const sc_mail *mail)
{
    const sc_status status = sc_mail_enqueue(mailer, mail);
    if (status != SC_OK)
        return status;
    if (mailer->flush_handle != NULL)
        return sc_mail_flush(mailer, NULL, NULL);
    /* Long enough for one attempt, the retry pause and the second attempt, which is the longest a
     * single mail can legitimately take. */
    return sc_mail_drain(mailer,
                         mailer->retry_delay_ms + 2 * mailer->timeout_ms + SC_MAIL_TICK_MS);
}

/*
 * const, and it takes the lock -- which means casting it away.
 *
 * The alternative is an unlocked read of two counters a worker is writing, which is a data race
 * whatever it is qualified with; ThreadSanitizer says so and is right. The mutex is the mailer's
 * own and locking it changes nothing a caller can observe, which is what makes the cast honest
 * rather than a way around the qualifier.
 */
uint32_t sc_mail_pending(const sc_mailer *mailer)
{
    sc_mailer *self = (sc_mailer *)mailer;
    uint32_t pending;

    if (mailer == NULL)
        return 0;
    uv_mutex_lock(&self->lock);
    pending = pending_locked(self);
    uv_mutex_unlock(&self->lock);
    return pending;
}

void sc_mail_get_stats(const sc_mailer *mailer, sc_mail_stats *out)
{
    sc_mailer *self = (sc_mailer *)mailer;

    if (out == NULL)
        return;
    memset(out, 0, sizeof *out);
    if (mailer == NULL)
        return;

    uv_mutex_lock(&self->lock);
    out->queued = self->queued;
    out->sent = self->sent;
    out->retried = self->retried;
    out->failed = self->failed;
    out->pending = pending_locked(self);
    out->workers = self->worker_live;
    uv_mutex_unlock(&self->lock);
}
