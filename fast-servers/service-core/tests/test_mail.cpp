/*
 * The mail client. service_core/mail.h holds the design; this file holds the parts of it a test
 * can decide.
 *
 * Most of what is worth checking is refusal and bookkeeping: an address that would have to be
 * truncated, a queue at its limit, a mail that fails twice and is written off. None of that
 * needs a relay -- a port nothing listens on refuses a connection immediately, which is a
 * failed attempt arriving in microseconds rather than after a timeout, and that is what makes
 * the retry cycle testable offline at all.
 *
 * What no test here decides is whether the rendered message is one a receiver accepts, or
 * whether the pool grows under real load. Set SC_MAIL_TEST_URL to a listening SMTP server and
 * the last tests send through it for real; without it they skip rather than pass, because a
 * green test that did nothing is worse than one that is absent.
 *
 * The concurrency is meant to be run under `-Dsanitize=thread`. EnqueueIsSafeFromManyThreads
 * proves very little on its own -- a mutex that is missing does not fail a single run -- and
 * everything under TSan.
 *
 * C++ because googletest is; see the note at the top of test_cache.cpp.
 */
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include "service_core/log.h"
#include "service_core/mail.h"
}

namespace
{

/* A relay address nothing listens on, so an attempt fails at connect rather than at a timeout. */
constexpr const char *kDeadRelay = "smtp://127.0.0.1:1";

sc_mail_config base_config()
{
    sc_mail_config config{};
    config.url = kDeadRelay;
    config.from = "bench@gradido.local";
    config.timeout_ms = 500;
    /* Short enough that a test can wait it out; the product default is half a minute. */
    config.retry_delay_ms = 50;
    return config;
}

sc_mail one_mail(const char *to = "inbox@gradido.local")
{
    sc_mail mail{};
    mail.to = to;
    mail.subject = "test";
    mail.body = "hello";
    return mail;
}

void sleep_ms(int ms)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

struct MailTest : ::testing::Test {};

/* ---------------------------------------------------------------- *
 * configuration and refusal
 * ---------------------------------------------------------------- */

TEST_F(MailTest, RefusesAConfigWithoutRelayOrSender)
{
    sc_mailer *mailer = nullptr;
    sc_mail_config config = base_config();

    config.url = nullptr;
    EXPECT_EQ(sc_mailer_create(&config, &mailer), SC_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(mailer, nullptr);

    config = base_config();
    config.from = nullptr;
    EXPECT_EQ(sc_mailer_create(&config, &mailer), SC_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(mailer, nullptr);
}

/* The house rule the whole module is written around: a field that does not fit is refused, not
 * shortened. A truncated relay host connects to a different machine. */
TEST_F(MailTest, RefusesAFieldThatWouldHaveToBeTruncated)
{
    sc_mailer *mailer = nullptr;
    sc_mail_config config = base_config();
    const std::string too_long(SC_MAIL_URL_MAX + 1, 'x');
    config.url = too_long.c_str();

    EXPECT_EQ(sc_mailer_create(&config, &mailer), SC_ERR_TOO_LONG);
    EXPECT_EQ(mailer, nullptr);
}

/* And the same for a recipient, which is the one that would deliver to someone else. */
TEST_F(MailTest, RefusesARecipientThatWouldHaveToBeTruncated)
{
    sc_mailer *mailer = nullptr;
    sc_mail_config config = base_config();
    ASSERT_EQ(sc_mailer_create(&config, &mailer), SC_OK);

    const std::string too_long(SC_MAIL_ADDR_MAX + 1, 'x');
    sc_mail mail = one_mail(too_long.c_str());
    EXPECT_EQ(sc_mail_enqueue(mailer, &mail), SC_ERR_TOO_LONG);
    EXPECT_EQ(sc_mail_pending(mailer), 0u);

    sc_mailer_destroy(mailer);
}

TEST_F(MailTest, EnqueueRefusesEverythingIncomplete)
{
    sc_mailer *mailer = nullptr;
    sc_mail_config config = base_config();
    ASSERT_EQ(sc_mailer_create(&config, &mailer), SC_OK);

    sc_mail mail = one_mail();
    mail.to = nullptr;
    EXPECT_EQ(sc_mail_enqueue(mailer, &mail), SC_ERR_INVALID_ARGUMENT);
    mail = one_mail();
    mail.subject = nullptr;
    EXPECT_EQ(sc_mail_enqueue(mailer, &mail), SC_ERR_INVALID_ARGUMENT);
    mail = one_mail();
    mail.body = nullptr;
    EXPECT_EQ(sc_mail_enqueue(mailer, &mail), SC_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(sc_mail_pending(mailer), 0u);

    sc_mailer_destroy(mailer);
}

/* ---------------------------------------------------------------- *
 * the queue and its ceiling
 * ---------------------------------------------------------------- */

TEST_F(MailTest, EnqueueRendersWithoutDialling)
{
    sc_mailer *mailer = nullptr;
    sc_mail_config config = base_config();
    ASSERT_EQ(sc_mailer_create(&config, &mailer), SC_OK);

    sc_mail mail = one_mail();
    EXPECT_EQ(sc_mail_enqueue(mailer, &mail), SC_OK);
    EXPECT_EQ(sc_mail_enqueue(mailer, &mail), SC_OK);
    /* Two mails are waiting and the relay above does not exist, which it would have had to if
     * enqueue dialled. */
    EXPECT_EQ(sc_mail_pending(mailer), 2u);

    sc_mail_stats stats{};
    sc_mail_get_stats(mailer, &stats);
    EXPECT_EQ(stats.queued, 2u);
    EXPECT_EQ(stats.sent, 0u);

    sc_mailer_destroy(mailer);
}

TEST_F(MailTest, AFullQueueSaysSoRatherThanGrowing)
{
    sc_mailer *mailer = nullptr;
    sc_mail_config config = base_config();
    config.queue_max = 4;
    ASSERT_EQ(sc_mailer_create(&config, &mailer), SC_OK);

    sc_mail mail = one_mail();
    for (int i = 0; i < 4; i++)
        EXPECT_EQ(sc_mail_enqueue(mailer, &mail), SC_OK) << "at " << i;
    EXPECT_EQ(sc_mail_enqueue(mailer, &mail), SC_ERR_QUEUE_FULL);
    EXPECT_EQ(sc_mail_pending(mailer), 4u);

    sc_mailer_destroy(mailer);
}

/* A message larger than one arena is refused, and must not leave a half-built entry or a
 * borrowed arena behind -- which the second half of this test is what catches. */
TEST_F(MailTest, AMessageLargerThanItsArenaIsRefusedCleanly)
{
    sc_mailer *mailer = nullptr;
    sc_mail_config config = base_config();
    config.queue_max = 2;
    config.message_max = 2048;
    ASSERT_EQ(sc_mailer_create(&config, &mailer), SC_OK);

    const std::string huge(4096, 'x');
    sc_mail big = one_mail();
    big.body = huge.c_str();
    EXPECT_EQ(sc_mail_enqueue(mailer, &big), SC_ERR_TOO_LONG);
    EXPECT_EQ(sc_mail_pending(mailer), 0u);

    /* Both arenas are still there to be borrowed, so the refusal gave its one back. */
    sc_mail small = one_mail();
    EXPECT_EQ(sc_mail_enqueue(mailer, &small), SC_OK);
    EXPECT_EQ(sc_mail_enqueue(mailer, &small), SC_OK);

    sc_mailer_destroy(mailer);
}

/* ---------------------------------------------------------------- *
 * retry
 * ---------------------------------------------------------------- */

/*
 * The whole retry cycle, without a network: an attempt that fails puts the mail back rather than
 * counting it, the mail is not tried again before the delay, and the second failure is the end
 * of it.
 */
TEST_F(MailTest, AFailedMailIsTriedOnceMoreAfterThePauseAndThenGivenUp)
{
    sc_mailer *mailer = nullptr;
    sc_mail_config config = base_config();
    config.retry_delay_ms = 200;
    ASSERT_EQ(sc_mailer_create(&config, &mailer), SC_OK);

    sc_mail mail = one_mail();
    ASSERT_EQ(sc_mail_enqueue(mailer, &mail), SC_OK);

    /* First attempt. It failed, but it is not a failure yet -- it is on the retry ring. */
    uint32_t sent = 99;
    uint32_t failed = 99;
    EXPECT_EQ(sc_mail_flush(mailer, &sent, &failed), SC_ERR_NETWORK);
    EXPECT_EQ(sent, 0u);
    EXPECT_EQ(failed, 0u);
    EXPECT_EQ(sc_mail_pending(mailer), 1u);

    sc_mail_stats stats{};
    sc_mail_get_stats(mailer, &stats);
    EXPECT_EQ(stats.retried, 1u);
    EXPECT_EQ(stats.failed, 0u);

    /* Too early: the pause has not passed, so this flush finds nothing due and touches nothing. */
    EXPECT_EQ(sc_mail_flush(mailer, &sent, &failed), SC_OK);
    EXPECT_EQ(sent, 0u);
    EXPECT_EQ(failed, 0u);
    EXPECT_EQ(sc_mail_pending(mailer), 1u);

    sleep_ms(300);

    /* Now it is due, it fails again, and that is the end of it. */
    EXPECT_EQ(sc_mail_flush(mailer, &sent, &failed), SC_ERR_NETWORK);
    EXPECT_EQ(sent, 0u);
    EXPECT_EQ(failed, 1u);
    EXPECT_EQ(sc_mail_pending(mailer), 0u);

    sc_mail_get_stats(mailer, &stats);
    EXPECT_EQ(stats.retried, 1u);
    EXPECT_EQ(stats.failed, 1u);
    EXPECT_EQ(stats.sent, 0u);

    sc_mailer_destroy(mailer);
}

/* There is no third attempt. A mail given up on is gone, and the arena it held came back --
 * which the refill proves. */
TEST_F(MailTest, ThereIsNoThirdAttempt)
{
    sc_mailer *mailer = nullptr;
    sc_mail_config config = base_config();
    config.queue_max = 1;
    config.retry_delay_ms = 50;
    ASSERT_EQ(sc_mailer_create(&config, &mailer), SC_OK);

    sc_mail mail = one_mail();
    ASSERT_EQ(sc_mail_enqueue(mailer, &mail), SC_OK);
    (void)sc_mail_flush(mailer, nullptr, nullptr);
    sleep_ms(120);
    (void)sc_mail_flush(mailer, nullptr, nullptr);
    ASSERT_EQ(sc_mail_pending(mailer), 0u);

    sleep_ms(120);
    uint32_t sent = 99;
    uint32_t failed = 99;
    EXPECT_EQ(sc_mail_flush(mailer, &sent, &failed), SC_OK);
    EXPECT_EQ(sent, 0u);
    EXPECT_EQ(failed, 0u);

    /* The one arena the pool has is free again, so a new mail fits. */
    EXPECT_EQ(sc_mail_enqueue(mailer, &mail), SC_OK);

    sc_mailer_destroy(mailer);
}

TEST_F(MailTest, FlushingAnEmptyQueueIsNotAFailure)
{
    sc_mailer *mailer = nullptr;
    sc_mail_config config = base_config();
    ASSERT_EQ(sc_mailer_create(&config, &mailer), SC_OK);

    uint32_t sent = 99;
    uint32_t failed = 99;
    EXPECT_EQ(sc_mail_flush(mailer, &sent, &failed), SC_OK);
    EXPECT_EQ(sent, 0u);
    EXPECT_EQ(failed, 0u);
    EXPECT_EQ(sc_mail_drain(mailer, 10), SC_OK);

    sc_mailer_destroy(mailer);
}

/* ---------------------------------------------------------------- *
 * workers
 * ---------------------------------------------------------------- */

TEST_F(MailTest, WorkersStartAndAreJoinedAgain)
{
    sc_mailer *mailer = nullptr;
    sc_mail_config config = base_config();
    config.workers = 2;
    config.worker_max = 4;
    ASSERT_EQ(sc_mailer_create(&config, &mailer), SC_OK);

    sc_mail_stats stats{};
    sc_mail_get_stats(mailer, &stats);
    EXPECT_EQ(stats.workers, 2u);

    /* The interesting half is that this returns at all: a worker sleeping on a condition
     * variable has to be woken and joined, not left behind. */
    sc_mailer_destroy(mailer);
}

/* workers = 0 and workers > 0 are two modes, and the one function that only belongs to the first
 * says so instead of racing the workers for the queue. */
TEST_F(MailTest, FlushBelongsToTheCallerOnlyWhenNoWorkerRuns)
{
    sc_mailer *mailer = nullptr;
    sc_mail_config config = base_config();
    config.workers = 1;
    ASSERT_EQ(sc_mailer_create(&config, &mailer), SC_OK);

    EXPECT_EQ(sc_mail_flush(mailer, nullptr, nullptr), SC_ERR_UNAVAILABLE);

    sc_mailer_destroy(mailer);
}

/* A worker takes the queue down on its own, and the mails end as failures rather than staying
 * put -- against this relay every attempt fails, so the two attempts and the pause are the whole
 * lifetime of each mail. */
TEST_F(MailTest, AWorkerEmptiesTheQueueWithoutBeingAsked)
{
    sc_mailer *mailer = nullptr;
    sc_mail_config config = base_config();
    config.workers = 1;
    config.retry_delay_ms = 50;
    ASSERT_EQ(sc_mailer_create(&config, &mailer), SC_OK);

    sc_mail mail = one_mail();
    for (int i = 0; i < 5; i++)
        ASSERT_EQ(sc_mail_enqueue(mailer, &mail), SC_OK);

    EXPECT_EQ(sc_mail_drain(mailer, 5000), SC_OK);

    sc_mail_stats stats{};
    sc_mail_get_stats(mailer, &stats);
    EXPECT_EQ(stats.queued, 5u);
    EXPECT_EQ(stats.retried, 5u);
    EXPECT_EQ(stats.failed, 5u);
    EXPECT_EQ(stats.pending, 0u);

    sc_mailer_destroy(mailer);
}

/* Enqueue is called from wherever a request happens to be running, so it has to be safe from
 * several threads at once. Under TSan this is the test that matters; without it, it mostly
 * proves that nothing crashes. */
TEST_F(MailTest, EnqueueIsSafeFromManyThreads)
{
    sc_mailer *mailer = nullptr;
    sc_mail_config config = base_config();
    config.workers = 2;
    config.queue_max = 512;
    config.retry_delay_ms = 10;
    ASSERT_EQ(sc_mailer_create(&config, &mailer), SC_OK);

    std::atomic<int> accepted{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; t++) {
        threads.emplace_back([&mailer, &accepted, t] {
            const std::string to = "user" + std::to_string(t) + "@gradido.local";
            for (int i = 0; i < 20; i++) {
                sc_mail mail = one_mail(to.c_str());
                if (sc_mail_enqueue(mailer, &mail) == SC_OK)
                    accepted.fetch_add(1);
            }
        });
    }
    for (auto &thread : threads)
        thread.join();

    EXPECT_EQ(sc_mail_drain(mailer, 10000), SC_OK);

    sc_mail_stats stats{};
    sc_mail_get_stats(mailer, &stats);
    /* Every mail that was accepted is accounted for exactly once, whichever way it ended. */
    EXPECT_EQ(stats.queued, static_cast<uint64_t>(accepted.load()));
    EXPECT_EQ(stats.sent + stats.failed, static_cast<uint64_t>(accepted.load()));
    EXPECT_EQ(stats.pending, 0u);

    sc_mailer_destroy(mailer);
}

/* Destroying a mailer with mails still queued must not hang and must not leak the arenas they
 * hold -- the second is what Valgrind and ASan see; this only makes it happen. */
TEST_F(MailTest, DestroyWithAFullQueueIsClean)
{
    sc_mailer *mailer = nullptr;
    sc_mail_config config = base_config();
    config.queue_max = 16;
    ASSERT_EQ(sc_mailer_create(&config, &mailer), SC_OK);

    sc_mail mail = one_mail();
    for (int i = 0; i < 16; i++)
        ASSERT_EQ(sc_mail_enqueue(mailer, &mail), SC_OK);

    sc_mailer_destroy(mailer);
}

TEST_F(MailTest, NullIsAccepted)
{
    sc_mail_stats stats{};
    EXPECT_EQ(sc_mail_pending(nullptr), 0u);
    sc_mail_get_stats(nullptr, &stats);
    EXPECT_EQ(stats.queued, 0u);
    sc_mailer_destroy(nullptr);
    EXPECT_EQ(sc_mail_flush(nullptr, nullptr, nullptr), SC_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(sc_mail_drain(nullptr, 1), SC_ERR_INVALID_ARGUMENT);
}

/* ---------------------------------------------------------------- *
 * with someone on the other end
 * ---------------------------------------------------------------- */

/*
 * Skipped unless SC_MAIL_TEST_URL names a listening SMTP server, because a green test that sent
 * nothing says nothing.
 *
 *   ../h20Test/smtp_client/sink.py --port 2525 &
 *   SC_MAIL_TEST_URL=smtp://127.0.0.1:2525 zig-out/bin/test_mail
 */
const char *live_url()
{
    return std::getenv("SC_MAIL_TEST_URL");
}

TEST_F(MailTest, SendsThroughARealRelay)
{
    const char *url = live_url();
    if (url == nullptr)
        GTEST_SKIP() << "set SC_MAIL_TEST_URL to a listening SMTP server";

    sc_mailer *mailer = nullptr;
    sc_mail_config config = base_config();
    config.url = url;
    config.from_name = "Gradido";
    config.timeout_ms = 5000;
    ASSERT_EQ(sc_mailer_create(&config, &mailer), SC_OK);

    /* A body with every line ending this thing is meant to normalise, and a line that starts
     * with a dot -- which curl stuffs and this code must not. */
    sc_mail mail = one_mail();
    mail.subject = "service-core mail test";
    mail.body = "first line\nsecond line\r\nthird line\r.leading dot\nlast";

    uint32_t sent = 0;
    uint32_t failed = 0;
    ASSERT_EQ(sc_mail_enqueue(mailer, &mail), SC_OK);
    EXPECT_EQ(sc_mail_flush(mailer, &sent, &failed), SC_OK);
    EXPECT_EQ(sent, 1u);
    EXPECT_EQ(failed, 0u);

    /* The second mail is the one that proves the session was kept: it rides the connection the
     * first one opened. */
    ASSERT_EQ(sc_mail_enqueue(mailer, &mail), SC_OK);
    EXPECT_EQ(sc_mail_flush(mailer, &sent, &failed), SC_OK);
    EXPECT_EQ(sent, 1u);

    sc_mailer_destroy(mailer);
}

/* The same, driven by workers instead of by the caller: what a server actually does. */
TEST_F(MailTest, WorkersSendThroughARealRelay)
{
    const char *url = live_url();
    if (url == nullptr)
        GTEST_SKIP() << "set SC_MAIL_TEST_URL to a listening SMTP server";

    sc_mailer *mailer = nullptr;
    sc_mail_config config = base_config();
    config.url = url;
    config.timeout_ms = 5000;
    config.workers = 1;
    ASSERT_EQ(sc_mailer_create(&config, &mailer), SC_OK);

    sc_mail mail = one_mail();
    mail.subject = "service-core worker test";
    for (int i = 0; i < 20; i++)
        ASSERT_EQ(sc_mail_enqueue(mailer, &mail), SC_OK) << "at " << i;

    EXPECT_EQ(sc_mail_drain(mailer, 20000), SC_OK);

    sc_mail_stats stats{};
    sc_mail_get_stats(mailer, &stats);
    EXPECT_EQ(stats.sent, 20u);
    EXPECT_EQ(stats.failed, 0u);
    EXPECT_EQ(stats.retried, 0u);

    sc_mailer_destroy(mailer);
}

/*
 * The pool growing under load, which is the one behaviour no offline test can show: it needs a
 * relay slow enough that a worker stays busy past spawn_after_ms with a backlog behind it.
 *
 *   scratch/slowsink.py 2550 0.2 &
 *   SC_MAIL_SLOW_URL=smtp://127.0.0.1:2550 zig-out/bin/test_mail --gtest_filter=*ScaleUp*
 *
 * Any SMTP server that answers slowly will do; 150 ms per mail or more is enough.
 */
TEST_F(MailTest, WorkersScaleUpUnderLoadAndRetireAfterwards)
{
    const char *url = std::getenv("SC_MAIL_SLOW_URL");
    if (url == nullptr)
        GTEST_SKIP() << "set SC_MAIL_SLOW_URL to a deliberately slow SMTP server";

    sc_mailer *mailer = nullptr;
    sc_mail_config config = base_config();
    config.url = url;
    config.timeout_ms = 30000;
    config.workers = 1;
    config.worker_max = 8;
    config.queue_max = 128;
    /* Small enough that a test does not take a minute, and the same shape as the defaults. */
    config.spawn_after_ms = 300;
    config.spawn_backlog = 4;
    config.linger_ms = 500;
    ASSERT_EQ(sc_mailer_create(&config, &mailer), SC_OK);

    sc_mail_stats stats{};
    sc_mail_get_stats(mailer, &stats);
    ASSERT_EQ(stats.workers, 1u);

    sc_mail mail = one_mail();
    mail.subject = "service-core scale test";
    for (int i = 0; i < 60; i++)
        ASSERT_EQ(sc_mail_enqueue(mailer, &mail), SC_OK) << "at " << i;

    /* The burst arrives in microseconds, so nobody has been busy long enough yet -- the growth
     * has to come from the workers noticing it themselves while they work. */
    uint32_t peak = 0;
    for (int i = 0; i < 100 && peak < 2u; i++) {
        sleep_ms(100);
        sc_mail_get_stats(mailer, &stats);
        if (stats.workers > peak)
            peak = stats.workers;
        if (stats.pending == 0)
            break;
    }
    EXPECT_GT(peak, 1u) << "the pool never grew past its permanent worker";
    EXPECT_LE(peak, 8u) << "the pool went past worker_max";

    EXPECT_EQ(sc_mail_drain(mailer, 60000), SC_OK);
    sc_mail_get_stats(mailer, &stats);
    EXPECT_EQ(stats.sent, 60u);
    EXPECT_EQ(stats.failed, 0u);

    /* And they go away again: linger_ms after the queue ran dry, only the permanent one is left.
     * A pool that grows and never shrinks holds connections a relay counts against us. */
    for (int i = 0; i < 60; i++) {
        sc_mail_get_stats(mailer, &stats);
        if (stats.workers == 1u)
            break;
        sleep_ms(100);
    }
    EXPECT_EQ(stats.workers, 1u) << "the on-demand workers never retired";

    sc_mailer_destroy(mailer);
}

} // namespace

int main(int argc, char **argv)
{
    /* Quiet: the failure paths below log a line each, and a hundred of them would bury the one
     * line that says which assertion failed. SC_MAIL_TEST_LOG turns them back on when a test is
     * being worked on. */
    sc_log_init(std::getenv("SC_MAIL_TEST_LOG") != nullptr ? SC_LOG_DEBUG : SC_LOG_FATAL);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
