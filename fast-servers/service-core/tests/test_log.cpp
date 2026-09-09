/*
 * The logger, from the outside: what a caller sees on the two streams.
 *
 * The weight is on what can go wrong in an asynchronous logger and would not show up in an
 * ordinary run -- lines lost or torn under load, arenas that never come back, backpressure that
 * in truth drops -- and on the one thing that is a contract rather than an implementation
 * detail: the shape of the line, compared byte for byte against contracts/logging.json.
 *
 * Every test runs the whole logger: a config, a thread, a ring, a real file underneath. There
 * is no seam to inject here and there should not be one -- the ring and the arena handover are
 * the parts worth testing, and a fake would be the part that is not.
 *
 * The logger is process wide, so main() below starts none: each test starts its own against a
 * fresh file and stops it again. That is also what lets a test choose a ring of eight slots and
 * be sure the backpressure engages.
 *
 * C++ because googletest is; see the note at the top of test_cache.cpp.
 */
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include "service_core/log/log.h"
#include "service_core/log/logger.h"
}

#if defined(_WIN32)
#include <io.h>
#define sc_test_open _open
#define sc_test_close _close
#define sc_test_unlink _unlink
#define SC_TEST_OPEN_FLAGS (_O_RDWR | _O_CREAT | _O_TRUNC | _O_BINARY)
#define SC_TEST_OPEN_MODE (_S_IREAD | _S_IWRITE)
#include <fcntl.h>
#include <process.h>
#define sc_test_pid _getpid
#else
#include <fcntl.h>
#include <unistd.h>
#define sc_test_open open
#define sc_test_close close
#define sc_test_unlink unlink
#define SC_TEST_OPEN_FLAGS (O_RDWR | O_CREAT | O_TRUNC)
#define SC_TEST_OPEN_MODE 0644
#define sc_test_pid getpid
#endif

namespace
{

/*
 * One logger against two files of its own, torn down whatever the test does.
 *
 * The descriptors are handed to the logger and closed by this, not by it: a config names a
 * descriptor and says nothing about who opened it, which is what lets the same logger write
 * into a pipe, a file or stderr.
 */
class Logger
{
  public:
    Logger(sc_log_level min_level, uint32_t ring, bool with_pretty, bool synchronous = false)
    {
        sc_log_config cfg;
        sc_log_default_config(&cfg);
        cfg.synchronous = synchronous ? 1 : 0;

        json_path_ = path("json");
        json_fd_ = sc_test_open(json_path_.c_str(), SC_TEST_OPEN_FLAGS, SC_TEST_OPEN_MODE);
        cfg.min_level = min_level;
        cfg.json_fd = json_fd_;
        cfg.ring_capacity = ring;
        /* Eight per grade rather than the default sixty-four: these tests want the pool to run
         * dry now and then, which is the path a long burst takes on a real server. */
        cfg.spare_per_grade = 8;
        if (with_pretty) {
            pretty_path_ = path("pretty");
            pretty_fd_ = sc_test_open(pretty_path_.c_str(), SC_TEST_OPEN_FLAGS, SC_TEST_OPEN_MODE);
            cfg.pretty_fd = pretty_fd_;
            cfg.pretty_color = 0;
        }
        started_ = sc_log_init(&cfg) == 0;
        if (started_)
            sc_log_thread_join();
    }

    ~Logger()
    {
        stop();
        remove(json_path_);
        remove(pretty_path_);
    }

    Logger(const Logger &) = delete;
    Logger &operator=(const Logger &) = delete;

    bool started() const { return started_; }

    /**
     * Stops the logger, so that everything submitted is on disk before it is read back.
     *
     * The files stay where they are -- reading them is the point, and they go with the object.
     */
    void stop()
    {
        if (!started_)
            return;
        sc_log_thread_leave();
        sc_log_shutdown();
        started_ = false;
        close_fd(json_fd_);
        close_fd(pretty_fd_);
    }

    /** The JSON stream as it stands. Only meaningful after stop(). */
    std::string json() const { return slurp(json_path_); }
    std::string pretty() const { return slurp(pretty_path_); }

  private:
    static std::string path(const char *tag)
    {
        const char *dir = std::getenv("TMPDIR");
        char name[256];
        std::snprintf(name, sizeof(name), "%s/sc_log_test_%s_%d_%u.log",
                      dir != nullptr && dir[0] != '\0' ? dir : "/tmp", tag, (int)sc_test_pid(),
                      next_id());
        return std::string(name);
    }

    static unsigned next_id()
    {
        static std::atomic<unsigned> counter{0};
        return counter.fetch_add(1);
    }

    static std::string slurp(const std::string &path)
    {
        std::string out;
        char buffer[4096];
        std::FILE *file;
        size_t read;

        if (path.empty())
            return out;
        file = std::fopen(path.c_str(), "rb");
        if (file == nullptr)
            return out;
        while ((read = std::fread(buffer, 1, sizeof(buffer), file)) > 0)
            out.append(buffer, read);
        std::fclose(file);
        return out;
    }

    static void close_fd(int &fd)
    {
        if (fd >= 0) {
            sc_test_close(fd);
            fd = -1;
        }
    }

    static void remove(const std::string &path)
    {
        if (!path.empty())
            sc_test_unlink(path.c_str());
    }

    std::string json_path_;
    std::string pretty_path_;
    int json_fd_ = -1;
    int pretty_fd_ = -1;
    bool started_ = false;
};

/** The first line without its newline. */
std::string first_line(const std::string &all)
{
    const size_t end = all.find('\n');
    return end == std::string::npos ? all : all.substr(0, end);
}

/** Everything from the first comma on -- the line minus its timestamp, which changes per run. */
std::string without_time(const std::string &line)
{
    const size_t comma = line.find(',');
    return comma == std::string::npos ? std::string() : line.substr(comma);
}

size_t count_lines(const std::string &s)
{
    return (size_t)std::count(s.begin(), s.end(), '\n');
}

/** Lines carrying one event id. The logger's own warnings share the stream and are not payload. */
size_t count_event(const std::string &s, const char *event)
{
    const std::string needle = std::string("\"event\":\"") + event + "\"";
    size_t n = 0;
    for (size_t at = s.find(needle); at != std::string::npos; at = s.find(needle, at + 1))
        ++n;
    return n;
}

/* ------------------------------------------------------------------ Shape */

TEST(LogShape, EnvelopeOnly)
{
    Logger logger(SC_LOG_INFO, 64, false);
    ASSERT_TRUE(logger.started());
    sc_log(SC_LOG_INFO, SC_CAT_STARTUP, "server.listen", "server is listening on port %d", 4000);
    logger.stop();

    const std::string line = first_line(logger.json());
    /* The time is a number and the first field; everything after it is fixed. */
    EXPECT_EQ(line.rfind("{\"time\":1", 0), 0u) << line;
    EXPECT_EQ(without_time(line),
              ",\"level\":30,\"cat\":\"startup\",\"event\":\"server.listen\""
              ",\"msg\":\"server is listening on port 4000\"}");
}

TEST(LogShape, RequestUserAndData)
{
    const sc_log_value data[] = {SC_LOG_STR("method", "POST"),
                                 SC_LOG_STR("path", "/api/user/login"),
                                 SC_LOG_INT("status", 200), SC_LOG_INT("ms", 12)};
    sc_log_context ctx = {};
    ctx.req = "0f9c1a7e-3d2b-4c55-9a10-7e2f6b8d4413";
    ctx.usr = 4711;
    ctx.data = data;
    ctx.data_count = 4;

    Logger logger(SC_LOG_INFO, 64, false);
    ASSERT_TRUE(logger.started());
    sc_log_event(SC_LOG_INFO, SC_CAT_HTTP, "request.complete", &ctx, "request completed");
    logger.stop();

    EXPECT_EQ(without_time(first_line(logger.json())),
              ",\"level\":30,\"cat\":\"http\",\"event\":\"request.complete\""
              ",\"req\":\"0f9c1a7e-3d2b-4c55-9a10-7e2f6b8d4413\",\"usr\":4711"
              ",\"data\":{\"method\":\"POST\",\"path\":\"/api/user/login\""
              ",\"status\":200,\"ms\":12},\"msg\":\"request completed\"}");
}

TEST(LogShape, ErrorObjectBoolAndNull)
{
    const sc_log_value data[] = {SC_LOG_INT("status", 401), SC_LOG_BOOL("locked", 0),
                                 SC_LOG_NULL("expected")};
    sc_log_context ctx = {};
    ctx.err_name = "AUTH_INVALID_CREDENTIALS";
    ctx.err_code = 1204;
    ctx.data = data;
    ctx.data_count = 3;

    Logger logger(SC_LOG_INFO, 64, false);
    ASSERT_TRUE(logger.started());
    sc_log_event(SC_LOG_ERROR, SC_CAT_AUTH, "login.rejected", &ctx, "login rejected");
    logger.stop();

    EXPECT_EQ(without_time(first_line(logger.json())),
              ",\"level\":50,\"cat\":\"auth\",\"event\":\"login.rejected\""
              ",\"err\":{\"code\":1204,\"name\":\"AUTH_INVALID_CREDENTIALS\"}"
              ",\"data\":{\"status\":401,\"locked\":false,\"expected\":null}"
              ",\"msg\":\"login rejected\"}");
}

TEST(LogShape, EscapesWhatJsonCannotHoldLiterally)
{
    Logger logger(SC_LOG_INFO, 64, false);
    ASSERT_TRUE(logger.started());
    sc_log(SC_LOG_INFO, SC_CAT_USER, "user.note",
           "quote=\" backslash=\\ tab=\t newline=\n bell=%c umlaut=\xc3\xa4", 7);
    logger.stop();

    const std::string all = logger.json();
    EXPECT_NE(all.find("quote=\\\" backslash=\\\\ tab=\\t newline=\\n bell=\\u0007"
                       " umlaut=\xc3\xa4"),
              std::string::npos)
        << all;
    /* One line, not two: a newline inside the sentence must not break through. */
    EXPECT_EQ(count_lines(all), 1u);
}

TEST(LogShape, KeepsALineWhoseBytesAreNotUtf8)
{
    /*
     * The line has to survive, and that is not free: asking arnm's writer to escape a string is
     * also what puts it under yyjson's UTF-8 check, and a string that fails it fails the whole
     * document -- so before this was guarded, one bad byte in a request path made the entire
     * line vanish without a trace. Which is the input somebody reads the log to find out about.
     *
     * What is kept is the valid prefix and a U+FFFD; what must never happen is a missing line.
     */
    const sc_log_value data[] = {SC_LOG_STR("path", "/api/\xffuser"), SC_LOG_INT("status", 400)};
    sc_log_context ctx = {};
    ctx.req = "c0ffee\xfe-1234";
    ctx.data = data;
    ctx.data_count = 2;

    Logger logger(SC_LOG_INFO, 64, false);
    ASSERT_TRUE(logger.started());
    sc_log(SC_LOG_INFO, SC_CAT_USER, "user.note", "before=[%s] after", "\xff\xfe");
    sc_log_event(SC_LOG_WARN, SC_CAT_HTTP, "request.failed", &ctx, "bad request");
    sc_log(SC_LOG_INFO, SC_CAT_USER, "user.after", "a clean line after the dirty ones");
    logger.stop();

    const std::string all = logger.json();
    EXPECT_EQ(count_lines(all), 3u) << all;
    EXPECT_NE(all.find("user.note"), std::string::npos) << all;
    EXPECT_NE(all.find("request.failed"), std::string::npos) << all;
    EXPECT_NE(all.find("user.after"), std::string::npos) << all;

    /* The valid prefix is kept and the tail becomes one replacement character -- the tail, not
     * the bad byte alone: everything after the first one goes, `] after` here included. */
    EXPECT_NE(all.find("\"msg\":\"before=[\xef\xbf\xbd\"}"), std::string::npos) << all;
    EXPECT_NE(all.find("\"path\":\"/api/\xef\xbf\xbd\""), std::string::npos) << all;
    EXPECT_NE(all.find("\"req\":\"c0ffee\xef\xbf\xbd\""), std::string::npos) << all;
    /* Nothing of the bad bytes themselves reaches the stream. */
    EXPECT_EQ(all.find('\xff'), std::string::npos) << all;
    /* And the fields around them are untouched. */
    EXPECT_NE(all.find("\"status\":400"), std::string::npos) << all;
}

TEST(LogShape, DropsWhatIsBelowTheLevel)
{
    Logger logger(SC_LOG_WARN, 64, false);
    ASSERT_TRUE(logger.started());
    sc_log(SC_LOG_DEBUG, SC_CAT_DB, "db.query", "should be missing");
    sc_log(SC_LOG_INFO, SC_CAT_DB, "db.query", "should be missing too");
    sc_log(SC_LOG_WARN, SC_CAT_DB, "db.slow", "should be present");
    sc_log(SC_LOG_ERROR, SC_CAT_DB, "db.fail", "should be present too");
    logger.stop();

    const std::string all = logger.json();
    EXPECT_EQ(count_lines(all), 2u);
    EXPECT_EQ(all.find("should be missing"), std::string::npos);
    EXPECT_NE(all.find("db.slow"), std::string::npos);
}

TEST(LogShape, TruncatesAnOversizedSentenceRatherThanTheStructure)
{
    const std::string big(4095, 'x');
    Logger logger(SC_LOG_INFO, 64, false);
    ASSERT_TRUE(logger.started());
    sc_log(SC_LOG_INFO, SC_CAT_HTTP, "big.line", "%s", big.c_str());
    logger.stop();

    const std::string all = logger.json();
    ASSERT_EQ(count_lines(all), 1u);
    EXPECT_LT(all.size(), 4096u);
    const std::string line = first_line(all);
    ASSERT_FALSE(line.empty());
    /* The sentence lost its tail; the object it sits in did not. */
    EXPECT_EQ(line.back(), '}');
}

/* ------------------------------------------------------------------ The two other paths */

TEST(LogDirect, WritesSynchronouslyEvenOnAFullRing)
{
    sc_log_stats stats;
    Logger logger(SC_LOG_INFO, 8, false);
    ASSERT_TRUE(logger.started());
    sc_log_direct(SC_LOG_INFO, SC_CAT_HTTP, "health.ok", nullptr, "healthy");
    sc_log_get_stats(&stats);
    logger.stop();

    EXPECT_NE(logger.json().find("health.ok"), std::string::npos);
    EXPECT_GE(stats.direct, 1u);
}

TEST(LogPretty, WritesTheHumanLineBesideTheContractedOne)
{
    const sc_log_value data[] = {SC_LOG_INT("status", 200)};
    sc_log_context ctx = {};
    ctx.usr = 4711;
    ctx.data = data;
    ctx.data_count = 1;

    Logger logger(SC_LOG_INFO, 64, true);
    ASSERT_TRUE(logger.started());
    sc_log_event(SC_LOG_INFO, SC_CAT_HTTP, "request.complete", &ctx, "request completed");
    logger.stop();

    const std::string json = logger.json();
    const std::string pretty = logger.pretty();
    EXPECT_NE(json.find("{\"time\":"), std::string::npos);
    ASSERT_FALSE(pretty.empty());
    EXPECT_NE(pretty.find("INFO (http/request.complete): request completed"), std::string::npos)
        << pretty;
    EXPECT_NE(pretty.find("usr=4711"), std::string::npos) << pretty;
    EXPECT_NE(pretty.find("status=200"), std::string::npos) << pretty;
    /* Neither stream leaks into the other. */
    EXPECT_EQ(pretty.find('{'), std::string::npos) << pretty;
}

/* ------------------------------------------------------------------ Backpressure */

TEST(LogBackpressure, BlocksRatherThanDropping)
{
    sc_log_stats stats;
    Logger logger(SC_LOG_INFO, 8, false);
    ASSERT_TRUE(logger.started());
    for (int i = 0; i != 5000; ++i)
        sc_log(SC_LOG_INFO, SC_CAT_HTTP, "bp.line", "line %d", i);
    sc_log_get_stats(&stats);
    logger.stop();

    /* Eight slots and five thousand lines: either the submitter waited, or lines were lost. */
    EXPECT_EQ(count_event(logger.json(), "bp.line"), 5000u);
    EXPECT_GT(stats.blocked, 0u);
}

TEST(LogBackpressure, WarnsAboutItsOwnRingAroundThatRing)
{
    Logger logger(SC_LOG_INFO, 16, false);
    ASSERT_TRUE(logger.started());
    for (int i = 0; i != 2000; ++i)
        sc_log(SC_LOG_INFO, SC_CAT_HTTP, "hw.line", "line %d", i);
    logger.stop();

    const std::string all = logger.json();
    EXPECT_NE(all.find("log.ring.highwater"), std::string::npos);
    EXPECT_NE(all.find("\"percent\":90"), std::string::npos);
}

/* ------------------------------------------------------------------ Concurrency */

constexpr int kMpscThreads = 8;
constexpr int kMpscLines = 10000;

void mpsc_worker(int id)
{
    sc_log_thread_join();
    for (int i = 0; i != kMpscLines; ++i) {
        const sc_log_value data[2] = {SC_LOG_INT("t", id), SC_LOG_INT("i", i)};
        sc_log_context ctx = {};
        ctx.data = data;
        ctx.data_count = 2;
        ctx.usr = (uint64_t)(id + 1);
        sc_log_event(SC_LOG_INFO, SC_CAT_HTTP, "load.line", &ctx, "thread %d line %d", id, i);
    }
    sc_log_thread_leave();
}

/** "t" and "i" out of one line. No JSON parser needed; the format is the thing under test. */
bool parse_ti(const std::string &line, long *t, long *i)
{
    const size_t at_t = line.find("\"t\":");
    const size_t at_i = line.find("\"i\":");
    if (at_t == std::string::npos || at_i == std::string::npos)
        return false;
    *t = std::strtol(line.c_str() + at_t + 4, nullptr, 10);
    *i = std::strtol(line.c_str() + at_i + 4, nullptr, 10);
    return true;
}

TEST(LogConcurrency, EightThreadsLoseTearAndReorderNothing)
{
    std::vector<std::thread> threads;
    sc_log_stats stats;

    /* A small ring on purpose: the backpressure is supposed to actually engage. */
    Logger logger(SC_LOG_INFO, 256, false);
    ASSERT_TRUE(logger.started());
    for (int k = 0; k != kMpscThreads; ++k)
        threads.emplace_back(mpsc_worker, k);
    for (auto &thread : threads)
        thread.join();
    sc_log_get_stats(&stats);
    logger.stop();

    std::vector<std::vector<char>> seen(kMpscThreads, std::vector<char>(kMpscLines, 0));
    std::vector<long> last(kMpscThreads, -1);
    size_t lines = 0, malformed = 0, duplicated = 0, out_of_order = 0;

    const std::string all = logger.json();
    for (size_t at = 0; at < all.size();) {
        const size_t end = all.find('\n', at);
        const std::string line = all.substr(at, end == std::string::npos ? end : end - at);
        at = end == std::string::npos ? all.size() : end + 1;
        if (line.empty())
            continue;
        /* The logger's own high-water warnings belong in this stream but are not payload. */
        if (line.find("log.ring.highwater") != std::string::npos)
            continue;
        ++lines;

        long t = 0, i = 0;
        if (line.size() < 10 || line.rfind("{\"time\":", 0) != 0 || line.back() != '}' ||
            !parse_ti(line, &t, &i) || t < 0 || t >= kMpscThreads || i < 0 || i >= kMpscLines) {
            ++malformed;
            continue;
        }
        if (seen[(size_t)t][(size_t)i])
            ++duplicated;
        seen[(size_t)t][(size_t)i] = 1;
        if (i <= last[(size_t)t])
            ++out_of_order;
        last[(size_t)t] = i;
    }

    EXPECT_EQ(lines, (size_t)kMpscThreads * kMpscLines);
    EXPECT_EQ(malformed, 0u);
    EXPECT_EQ(duplicated, 0u);
    EXPECT_EQ(out_of_order, 0u);

    size_t missing = 0;
    for (const auto &per_thread : seen)
        for (char one : per_thread)
            if (!one)
                ++missing;
    EXPECT_EQ(missing, 0u);
    EXPECT_EQ(stats.submitted, (uint64_t)kMpscThreads * kMpscLines);
    EXPECT_GT(stats.blocked, 0u) << "the ring never filled -- this test then measures nothing";
}

TEST(LogConcurrency, EveryArenaComesBackIntoItsOwnPool)
{
    /*
     * sc_log_thread_leave() returns only once arnm's own acquired_count reads zero for every
     * grade -- which happens only when every arena was returned exactly once and to the pool it
     * came from. mpsc_worker calls it, so a thread that finishes at all has proved it, and a
     * thread that has not would hang here rather than fail.
     */
    std::vector<std::thread> threads;
    Logger logger(SC_LOG_INFO, 128, false);
    ASSERT_TRUE(logger.started());
    for (int k = 0; k != 4; ++k)
        threads.emplace_back(mpsc_worker, k);
    for (auto &thread : threads)
        thread.join();
    SUCCEED() << "every thread unregistered, so every arena came back";
}

/* ------------------------------------------------------------------ The synchronous mode */

TEST(LogSynchronous, WritesTheSameLineWithoutStartingAThread)
{
    /*
     * What a command runs with: no ring, no drain thread, one write per line on the calling
     * thread. `setup` reads an answer off the terminal it logs to, and a line still sitting in a
     * 64 KiB buffer would surface between two questions.
     *
     * The line itself must not change with the mode -- it is the contracted one either way, and
     * this compares it against the same string LogShape.EnvelopeOnly asserts on.
     */
    Logger logger(SC_LOG_INFO, 64, false, true);
    ASSERT_TRUE(logger.started());

    /* No thread was started, so there is no pool to take and nothing to give back. */
    EXPECT_EQ(sc_log_thread_join(), -1);

    sc_log(SC_LOG_INFO, SC_CAT_STARTUP, "server.listen", "server is listening on port %d", 4000);
    /* Read before the stop, which is the whole difference: a line is on the descriptor by the
     * time the call returns rather than when the logger gets round to it. */
    const std::string line = first_line(logger.json());
    EXPECT_EQ(without_time(line),
              ",\"level\":30,\"cat\":\"startup\",\"event\":\"server.listen\""
              ",\"msg\":\"server is listening on port 4000\"}");
}

TEST(LogSynchronous, StillHonoursTheLevel)
{
    Logger logger(SC_LOG_WARN, 64, false, true);
    ASSERT_TRUE(logger.started());
    sc_log(SC_LOG_INFO, SC_CAT_DB, "db.query", "should be missing");
    sc_log(SC_LOG_WARN, SC_CAT_DB, "db.slow", "should be present");
    logger.stop();

    const std::string all = logger.json();
    EXPECT_EQ(count_lines(all), 1u);
    EXPECT_NE(all.find("db.slow"), std::string::npos);
}

/* ------------------------------------------------------------------ No logger at all */

TEST(LogWithoutInit, WritesTheLineSynchronouslyInsteadOfCrashing)
{
    /*
     * sc_config_load() logs about an environment it cannot read, and it runs before anything has
     * been started; a unit test links this component and arranges no logger at all. Both have to
     * get their lines rather than a crash, which is what the synchronous fallback is for.
     *
     * There is no file to read back: the fallback writes to stderr, which is where a line with
     * nobody to route it belongs, and which is also what sc_log_shutdown() puts the
     * configuration back to -- the descriptors a stopped logger held are the caller's again and
     * may already have been handed to something else. So what is under test is that the calls
     * return, and where they land is checked by the eye reading the test output.
     */
    sc_log(SC_LOG_FATAL, SC_CAT_STARTUP, "config.failed", "no logger is running, and that is fine");
    sc_log_direct(SC_LOG_FATAL, SC_CAT_STARTUP, "config.failed", nullptr, "nor for this one");
    EXPECT_EQ(sc_log_thread_join(), -1) << "there is no ring to take a pool from";
    SUCCEED();
}

} // namespace

int main(int argc, char **argv)
{
    /* No logger here on purpose: every test starts one of its own, and a second sc_log_init
     * would be refused. The lines this file writes without one go to stderr, synchronously. */
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
