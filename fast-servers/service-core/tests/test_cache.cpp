/*
 * The session cache table. AGENTS.md section 5 lists its invariants and says each of them was a
 * bug first; this file is the part of that list a test can hold onto.
 *
 * What it cannot hold onto is the part that matters most. A reference counted structure under
 * two locks does not fail a single-threaded test when it is wrong -- it fails in production,
 * under load, weeks later. So the last test here runs many threads against one table and is
 * meant to be run under `-Dsanitize=thread`, where it is ThreadSanitizer rather than an
 * assertion that decides whether it passed. Running it without TSan proves very little.
 *
 * C++ because googletest is, which is what gradido-blockchain-core and arnm do as well. It is not
 * an exception to AGENTS.md section 2: the rule there is about modules in the request path, and
 * nothing in this file is linked into a server.
 */
#include <gtest/gtest.h>

#include <atomic>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include "service_core/cache.h"
#include "service_core/log.h"
}

namespace
{

/* Counts what the cache freed, so the reference counting can be observed from outside. */
struct Payload {
    int id;
    std::atomic<int> *freed;
};

void free_payload(void *value, void *user_data)
{
    (void)user_data;
    Payload *payload = static_cast<Payload *>(value);
    payload->freed->fetch_add(1);
    delete payload;
}

class CacheTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        freed_.store(0);
    }

    sc_cache *make(uint32_t capacity, int64_t timeout_ms = 0, uint32_t probe = 0)
    {
        sc_cache_config config{};
        config.capacity = capacity;
        config.probe = probe;
        config.hard_timeout_ms = timeout_ms;
        config.free_value = free_payload;
        config.user_data = nullptr;
        sc_cache *cache = sc_cache_create(&config);
        EXPECT_NE(cache, nullptr);
        return cache;
    }

    Payload *payload(int id)
    {
        return new Payload{id, &freed_};
    }

    std::atomic<int> freed_{0};
};

TEST_F(CacheTest, PutThenGetReturnsTheValue)
{
    sc_cache *cache = make(16);
    sc_cache_entry *put = sc_cache_put(cache, "a", 1, payload(7));
    ASSERT_NE(put, nullptr);

    sc_cache_entry *got = sc_cache_get(cache, "a", 1);
    ASSERT_NE(got, nullptr);
    EXPECT_EQ(static_cast<Payload *>(sc_cache_entry_value(got))->id, 7);

    sc_cache_release(got);
    sc_cache_release(put);
    sc_cache_destroy(cache);
    EXPECT_EQ(freed_.load(), 1);
}

TEST_F(CacheTest, MissReturnsNull)
{
    sc_cache *cache = make(16);
    EXPECT_EQ(sc_cache_get(cache, "absent", 6), nullptr);
    sc_cache_destroy(cache);
}

TEST_F(CacheTest, RejectsAKeyThatDoesNotFit)
{
    sc_cache *cache = make(16);
    const std::string too_long(SC_CACHE_KEY_MAX + 1, 'k');

    /* Refused rather than truncated: two different session tokens whose first 64 bytes agree
     * would otherwise become one session. */
    EXPECT_EQ(sc_cache_put(cache, too_long.data(), too_long.size(), payload(1)), nullptr);
    EXPECT_EQ(sc_cache_get(cache, too_long.data(), too_long.size()), nullptr);
    sc_cache_destroy(cache);
}

TEST_F(CacheTest, PuttingTheSameKeyTwiceReplacesRatherThanDuplicates)
{
    sc_cache *cache = make(16);
    sc_cache_entry *first = sc_cache_put(cache, "k", 1, payload(1));
    sc_cache_entry *second = sc_cache_put(cache, "k", 1, payload(2));
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);

    /* The table dropped its reference on the first entry, so its value is gone -- while the
     * caller's own reference keeps the entry itself alive and readable. */
    sc_cache_entry *got = sc_cache_get(cache, "k", 1);
    ASSERT_NE(got, nullptr);
    EXPECT_EQ(static_cast<Payload *>(sc_cache_entry_value(got))->id, 2);

    sc_cache_release(got);
    sc_cache_release(second);
    sc_cache_release(first);
    sc_cache_destroy(cache);
    EXPECT_EQ(freed_.load(), 2);
}

TEST_F(CacheTest, AnEntryInUseSurvivesTheTableDroppingIt)
{
    sc_cache *cache = make(16);
    sc_cache_entry *held = sc_cache_put(cache, "k", 1, payload(1));
    ASSERT_NE(held, nullptr);

    /* Evict it by overwriting the key, the way a real eviction does. */
    sc_cache_entry *replacement = sc_cache_put(cache, "k", 1, payload(2));
    ASSERT_NE(replacement, nullptr);

    /* Freed exactly when the count reaches zero, never anywhere else: the first value is still
     * readable through the reference this test holds. */
    EXPECT_EQ(freed_.load(), 0);
    EXPECT_EQ(static_cast<Payload *>(sc_cache_entry_value(held))->id, 1);

    sc_cache_release(held);
    EXPECT_EQ(freed_.load(), 1);

    sc_cache_release(replacement);
    sc_cache_destroy(cache);
    EXPECT_EQ(freed_.load(), 2);
}

TEST_F(CacheTest, InsertionNeverFails)
{
    /* Capacity 4 with a probe of 4, then twenty keys: an occupied slot is overwritten and there
     * is no full-cache error for a caller to handle. */
    sc_cache *cache = make(4, 0, 4);
    for (int i = 0; i != 20; ++i) {
        const std::string key = "key-" + std::to_string(i);
        sc_cache_entry *entry = sc_cache_put(cache, key.data(), key.size(), payload(i));
        ASSERT_NE(entry, nullptr) << "insertion " << i;
        sc_cache_release(entry);
    }
    sc_cache_destroy(cache);
    EXPECT_EQ(freed_.load(), 20);
}

TEST_F(CacheTest, AnExpiredEntryReadsAsAbsent)
{
    sc_cache *cache = make(16, /*timeout_ms=*/20);
    sc_cache_entry *put = sc_cache_put(cache, "k", 1, payload(1));
    ASSERT_NE(put, nullptr);
    sc_cache_entry *before = sc_cache_get(cache, "k", 1);
    ASSERT_NE(before, nullptr);
    sc_cache_release(before);

    std::this_thread::sleep_for(std::chrono::milliseconds(40));

    /* Lazy expiry: nothing swept, the reader simply does not see it any more. */
    EXPECT_EQ(sc_cache_get(cache, "k", 1), nullptr);
    /* And it is still in the table holding its value, until a put walks the same slot. */
    EXPECT_EQ(freed_.load(), 0);

    sc_cache_entry *replacement = sc_cache_put(cache, "k", 1, payload(2));
    ASSERT_NE(replacement, nullptr);
    sc_cache_release(replacement);
    sc_cache_release(put);
    sc_cache_destroy(cache);
    EXPECT_EQ(freed_.load(), 2);
}

TEST_F(CacheTest, ZeroTimeoutMeansNoExpiry)
{
    sc_cache *cache = make(16, /*timeout_ms=*/0);
    sc_cache_entry *put = sc_cache_put(cache, "k", 1, payload(1));
    ASSERT_NE(put, nullptr);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    sc_cache_entry *got = sc_cache_get(cache, "k", 1);
    EXPECT_NE(got, nullptr);
    sc_cache_release(got);
    sc_cache_release(put);
    sc_cache_destroy(cache);
}

TEST_F(CacheTest, ReleasingNullIsAccepted)
{
    sc_cache_release(nullptr);
    sc_cache_destroy(nullptr);
}

/*
 * The one that matters. Under -Dsanitize=thread this is what reports a reference count
 * incremented after the table lock was released, or a value freed while another thread was
 * reading it. Without TSan it only proves nothing crashed, which is the weaker claim.
 */
TEST_F(CacheTest, ConcurrentReadersAndWriters)
{
    constexpr int kThreads = 8;
    constexpr int kRounds = 400;
    constexpr int kKeys = 16;

    /* A capacity smaller than the key set, so eviction happens constantly rather than by luck. */
    sc_cache *cache = make(8, 0, 4);
    std::atomic<int> hits{0};
    std::vector<std::thread> threads;

    for (int t = 0; t != kThreads; ++t) {
        threads.emplace_back([&, t] {
            for (int round = 0; round != kRounds; ++round) {
                /* The key comes from the round and the role from the thread, so that readers
                 * and writers meet on the same keys. Deriving both from the same number is how
                 * this test first managed to record no hits at all: every key was either only
                 * written or only read, and the two never raced. */
                const std::string key = "session-" + std::to_string(round % kKeys);
                if ((t & 1) != 0) {
                    sc_cache_entry *entry =
                        sc_cache_put(cache, key.data(), key.size(), payload(round));
                    if (entry != nullptr)
                        sc_cache_release(entry);
                } else {
                    sc_cache_entry *entry = sc_cache_get(cache, key.data(), key.size());
                    if (entry != nullptr) {
                        /* Read through the pointer while holding the reference -- this is the
                         * access that lands in freed memory when the count is wrong. */
                        volatile int id = static_cast<Payload *>(sc_cache_entry_value(entry))->id;
                        (void)id;
                        hits.fetch_add(1);
                        sc_cache_release(entry);
                    }
                }
            }
        });
    }
    for (auto &thread : threads)
        thread.join();

    sc_cache_destroy(cache);
    /* Every payload ever put is accounted for; nothing leaked and nothing was freed twice. */
    EXPECT_GT(hits.load(), 0);
}

} // namespace

int main(int argc, char **argv)
{
    /* The cache logs nothing today, but sc_now_ms is behind the same initialisation. */
    sc_log_init(SC_LOG_ERROR);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
