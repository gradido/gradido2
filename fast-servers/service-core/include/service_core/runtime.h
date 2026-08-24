/*
 * Process lifetime: the quit flag, the signal handlers that raise it, and a sleep.
 *
 * One flag for the whole process. Every role's run loop polls it and returns; nothing is
 * cancelled from the outside, because a thread stopped mid-request is a thread that leaked
 * whatever it was holding.
 */
#ifndef SERVICE_CORE_RUNTIME_H
#define SERVICE_CORE_RUNTIME_H

#include <stdint.h>

/*
 * A lock-free atomic and not a `volatile sig_atomic_t`.
 *
 * sig_atomic_t is what a handler may write when the only reader is the interrupted thread. Here
 * the readers are the role threads, and a plain object written by one thread and read by another
 * is a data race whatever it is qualified with -- ThreadSanitizer says so in as many words, and
 * it is right. C11 7.14.1.1 permits a handler to touch a lock-free atomic object, which is what
 * this is on every target this builds for.
 */
typedef struct sc_quit_flag {
    volatile int32_t raised;
} sc_quit_flag;

/** Non-zero once shutdown has been requested. This is what a run loop polls. */
int sc_quit_requested(const sc_quit_flag *flag);

/**
 * Points SIGINT and SIGTERM at @p flag, and ignores SIGPIPE where it exists -- a client that
 * disconnects mid-response must fail one write, not the process.
 *
 * Call once, from main, before any thread starts.
 */
void sc_runtime_install_signal_handlers(sc_quit_flag *flag);

/** Raises the flag from ordinary code, e.g. when one role fails to start and the rest must
 *  come down with it. */
void sc_runtime_request_quit(void);

void sc_runtime_sleep_ms(unsigned int milliseconds);

/* How long a run loop may block before it looks at the quit flag again. It is the shutdown
 * latency, so it is short; it is also a wakeup per role per tick, so it is not shorter. */
#define SC_RUNTIME_TICK_MS 100

#endif /* SERVICE_CORE_RUNTIME_H */
