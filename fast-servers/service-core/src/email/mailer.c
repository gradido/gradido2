/*
 * The queue and the worker pool. service_core/email/mailer.h is the specification; this file is
 * its transcription, and the reasoning for the shape -- held sessions, a growing worker pool, one
 * retry, one recipient -- lives there rather than being repeated here.
 *
 * What this file is *not* is the mail itself. The bytes are email/message.c and the SMTP session
 * is email/transport.c, neither of which knows about threads -- which is what lets the Node addon
 * in packages/email-native send the identical message without any of the machinery below.
 *
 * Four things are worth knowing before changing anything here.
 *
 * **A worker is a connection.** A session belongs to one thread; libcurl says so and means it. So
 * a worker owns its session for as long as it lives, and starting a second worker is how a second
 * session to the relay comes about. Nothing here reconnects -- curl replaces a
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
 * the counters and every worker's bookkeeping; it is never held across a send; and
 * there is no second lock.
 */
#include "service_core/email/mailer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <uv.h>

#include "arnm/arena.h"
#include "arnm/fixed_arena_pool.h"
#include "arnm/memory.h"

#include "service_core/atomic.h"
#include "service_core/log.h"

/*
 * How long an idle worker sleeps before looking at the quit flag and its own retirement clock
 * again. It is the shutdown latency of a mailer, so it is short; it is also a wakeup per idle
 * worker, so it is not shorter. The same number and the same reasoning as SC_RUNTIME_TICK_MS.
 */
#define SC_MAIL_TICK_MS 100

/*
 * One queued mail.
 *
 * The recipient is a fixed buffer rather than a pointer because an address is bounded by the
 * protocol anyway and the entry outlives whatever the caller passed. `message` points into the
 * arena this entry borrowed from the pool, and `arena` is how that borrow is given back.
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

    /* What email/transport.c is handed on every send. Its strings point at the fields above, in
     * this same struct, and the mailer is allocated once and never moved. */
    sc_mail_relay relay;

    /* The session for `workers = 0`, used only on the caller's thread by sc_mail_flush. NULL
     * whenever workers are running, so the two modes cannot quietly share one. */
    sc_mail_session *flush_handle;

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
    ring->count++;
    return 1;
}

/** Takes the oldest entry out. False when the ring is empty. */
static int ring_pop(sc_mail_ring *ring, sc_mail_entry *out)
{
    if (ring->count == 0)
        return 0;
    *out = ring->slot[ring->head];
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
 * Formats @p mail into an arena block and fills in @p entry.
 *
 * The bytes themselves are email/message.c's business; what is decided here is where they go.
 * The arena was handed over by the pool and belongs to this thread until the entry is queued, so
 * several callers can format at once and only the push itself is serialised -- this runs
 * *outside* the queue lock.
 *
 * The block is message_max and not the exact length: the pool sized every arena that way anyway,
 * so asking for less would save nothing and would cost the two passes a second measurement. A
 * message that does not fit answers SC_ERR_TOO_LONG, which is the same refusal as before.
 *
 * The Message-ID assigned here survives the retry. That is the point of it: the same mail
 * delivered twice under two identities is a duplicate in someone's inbox, and the retry is
 * exactly the path that could produce one.
 */
static sc_status render(sc_mailer *mailer, const sc_mail *mail, arnm *arena, uint64_t sequence,
                        sc_mail_entry *entry)
{
    const sc_mail_origin origin = {mailer->from, mailer->from_name, mailer->msgid_domain};
    sc_mail_message message;
    sc_status status;
    uint8_t *buffer = NULL;

    /* One clock for the Date: header, the Message-ID and the log. time() beside sc_now_ms() had
     * the header and the Message-ID a second apart whenever the two calls straddled a tick --
     * harmless, and exactly the kind of thing that wastes an afternoon when someone correlates a
     * log line with a delivered mail. */
    const int64_t now_ms = sc_now_ms();

    if (!arnm_ok(arnm_alloc(&buffer, mailer->message_max, arena)))
        return SC_ERR_TOO_LONG;

    status = sc_mail_format(&origin, mail, sequence, now_ms, (char *)buffer, mailer->message_max,
                            &message);
    if (status != SC_OK)
        return status;

    memcpy(entry->msgid, message.msgid, sizeof entry->msgid);
    entry->message = message.data;
    entry->message_len = message.len;
    entry->arena = arena;
    entry->attempts = 0;
    entry->not_before_ms = 0;
    return SC_OK;
}

/* ------------------------------------------------------------------ *
 * the transfer
 * ------------------------------------------------------------------ */

/**
 * One attempt at @p entry over @p session, which belongs to the calling thread and to no other.
 *
 * The send itself is email/transport.c; what belongs here is what a failed attempt means. Warn
 * and not error: this may still be the first of two attempts, and an error line for something
 * that is about to succeed is an alert nobody can act on. finish_entry() logs the error when
 * there is no attempt left.
 *
 * Called with no lock held. It blocks for as long as the relay takes.
 */
static sc_status attempt(sc_mail_session *session, const sc_mailer *mailer,
                         const sc_mail_entry *entry)
{
    char error[SC_MAIL_ERROR_MAX];
    sc_status status = sc_mail_session_send(session, &mailer->relay, entry->to, entry->message,
                                            entry->message_len, error, sizeof error);
    if (status != SC_OK) {
        sc_log_warn(SC_CAT_MAIL, "mail.attempt.failed", "%s <%s>: %s", entry->to, entry->msgid,
                    error);
        return status;
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
static void worker_retire(sc_mail_worker *worker, sc_mail_session *session)
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

    if (session != NULL)
        sc_mail_session_close(session);
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
    sc_mail_session *handle = sc_mail_session_open();

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

        outcome = attempt(handle, mailer, &entry);

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
        sc_mail_session_close(mailer->flush_handle);
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

    sc_mail_global_init();

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

    /* What every send is handed. The strings point into the fields copied above rather than at
     * the caller's, which is what lets the config be a stack value that goes away. */
    mailer->relay.url = mailer->url;
    mailer->relay.from = mailer->from;
    mailer->relay.user = mailer->user;
    mailer->relay.pass = mailer->pass;
    mailer->relay.cainfo = mailer->cainfo;
    mailer->relay.starttls = config->starttls;
    mailer->relay.scan_ca_path = config->scan_ca_path;
    mailer->relay.insecure = config->insecure;
    mailer->relay.timeout_ms =
        config->timeout_ms > 0 ? config->timeout_ms : SC_MAIL_TIMEOUT_DEFAULT_MS;
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
        mailer->flush_handle = sc_mail_session_open();
        if (mailer->flush_handle == NULL) {
            mailer_free(mailer);
            return SC_ERR_NO_MEMORY;
        }
    }

    if (mailer->relay.insecure) {
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
 * a session pulls libcurl out from under it. A second mailer created afterwards would also find
 * libcurl uninitialised while sc_mail_global_init() refuses to run the initialiser again. The
 * memory it would reclaim is freed by process exit anyway, which is the only moment at which the
 * call would be correct.
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
        outcome = attempt(mailer->flush_handle, mailer, &entry);

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
                         mailer->retry_delay_ms + 2 * mailer->relay.timeout_ms + SC_MAIL_TICK_MS);
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
