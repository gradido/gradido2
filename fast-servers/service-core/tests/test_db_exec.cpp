/*
 * The executor: where a unit runs, how its transaction ends, and what happens when there is more
 * work than connections.
 *
 * SQLite for everything that is about the executor rather than the database: its one writer is
 * the smallest group there is, which makes "busy" one blocked unit away, and its reads are the
 * one case that runs on the submitting thread. PostgreSQL where the point is several workers,
 * behind the SC_DB_TEST_PG_* variables test_db.cpp documents.
 *
 * No HTTP here: the executor hands units back through a hook, and these tests hand them to a
 * condition variable instead of a parked request.
 *
 * C++ because googletest is; see the note at the top of test_cache.cpp.
 */
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

extern "C" {
#include "service_core/db.h"
#include "service_core/db_exec.h"
#include "service_core/log/log.h"
#include "service_core/log/logger.h"
#include "service_core/sql.h"
#include "service_core/topology.h"
}

namespace
{

sc_sql_statement kInsert = SC_SQL_STATEMENT("exec_test.insert", "INSERT INTO t (v) VALUES ($1)",
                                            "INSERT INTO t (v) VALUES (?1)");
sc_sql_statement kCount =
    SC_SQL_STATEMENT("exec_test.count", "SELECT count(*) FROM t", "SELECT count(*) FROM t");
sc_sql_statement kSleep = SC_SQL_STATEMENT("exec_test.sleep", "SELECT pg_sleep(0.2)", "SELECT 1");

/* What a test hands the executor: the unit, and what the work did with it. */
struct TestUnit {
    sc_db_unit unit{};
    /* What the work does: insert this value, then end as `end_on` says for each attempt. */
    int64_t value = 0;
    sc_db_end first = SC_DB_COMMIT;
    sc_db_end later = SC_DB_COMMIT;
    /* Filled in by the work. */
    std::thread::id ran_on;
    int64_t counted = -1;
    sc_status work_status = SC_OK;
    /* A gate the work waits at, for filling a queue behind it. */
    std::mutex *gate = nullptr;
    /* How often the work was run, whatever the executor called it. */
    int calls = 0;
};

TestUnit *of(sc_db_unit *unit)
{
    return reinterpret_cast<TestUnit *>(unit);
}

sc_db_end insert_work(sc_db *db, sc_db_unit *unit)
{
    TestUnit *self = of(unit);
    sc_sql_param params[1] = {sc_sql_int(self->value)};
    sc_sql_error error{};

    self->ran_on = std::this_thread::get_id();
    if (self->gate != nullptr) {
        self->gate->lock();
        self->gate->unlock();
    }
    self->work_status = sc_sql_exec(db, &kInsert, params, 1, nullptr, &error);
    if (self->work_status != SC_OK)
        return SC_DB_ROLLBACK;
    return unit->attempt == 1 ? self->first : self->later;
}

sc_db_end count_work(sc_db *db, sc_db_unit *unit)
{
    TestUnit *self = of(unit);
    sc_sql_rows rows{};
    sc_sql_error error{};

    self->ran_on = std::this_thread::get_id();
    self->work_status = sc_sql_query(db, &kCount, nullptr, 0, &rows, &error);
    if (self->work_status == SC_OK && sc_sql_next(&rows))
        self->counted = sc_sql_col_int(&rows, 0);
    const sc_status closed = sc_sql_close(&rows);
    if (self->work_status == SC_OK)
        self->work_status = closed;
    return SC_DB_COMMIT;
}

sc_db_end sleep_work(sc_db *db, sc_db_unit *unit)
{
    sc_sql_rows rows{};
    sc_sql_error error{};

    of(unit)->work_status = sc_sql_query(db, &kSleep, nullptr, 0, &rows, &error);
    const sc_status closed = sc_sql_close(&rows);
    if (of(unit)->work_status == SC_OK)
        of(unit)->work_status = closed;
    return SC_DB_COMMIT;
}

/* Where finished units go: the test thread waits here for them. */
struct Returns {
    std::mutex lock;
    std::condition_variable arrived;
    std::vector<sc_db_unit *> units;
    std::atomic<int> parked{0};
    sc_status park_answer = SC_OK;

    static void finished(void *context, sc_db_unit *unit)
    {
        Returns *self = static_cast<Returns *>(context);
        std::lock_guard<std::mutex> hold(self->lock);

        self->units.push_back(unit);
        self->arrived.notify_all();
    }

    static sc_status park(void *context, sc_db_unit *)
    {
        Returns *self = static_cast<Returns *>(context);

        ++self->parked;
        return self->park_answer;
    }

    bool wait_for(size_t count, int ms = 10000)
    {
        std::unique_lock<std::mutex> hold(lock);

        return arrived.wait_for(hold, std::chrono::milliseconds(ms),
                                [&] { return units.size() >= count; });
    }
};

TestUnit write_unit(int64_t value)
{
    TestUnit u;

    u.unit.access = SC_DB_WRITE;
    u.unit.work = insert_work;
    u.value = value;
    return u;
}

class SqliteExec : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        static int counter = 0;
        char name[256];
        sc_db *setup = nullptr;
        sc_sql_error error{};

        (void)std::snprintf(name, sizeof(name), "gradido_exec_test_%d_%d.sqlite", (int)getpid(),
                            counter++);
        path_ = name;
        config_.kind = SC_DB_SQLITE;
        (void)std::snprintf(config_.file, sizeof(config_.file), "%s", path_.c_str());
        ASSERT_EQ(sc_db_open(&config_, &setup), SC_OK);
        ASSERT_EQ(sc_sql_simple(setup, "CREATE TABLE t (v INTEGER UNIQUE)", &error), SC_OK)
            << error.message;
        sc_db_close(setup);

        sc_db_exec_config cfg{};
        cfg.db = &config_;
        cfg.loops = 2;
        cfg.park = Returns::park;
        cfg.finished = Returns::finished;
        cfg.context = &returns_;
        ASSERT_EQ(sc_db_exec_open(&cfg, &exec_, nullptr), SC_OK);
    }

    void TearDown() override
    {
        sc_db_exec_close(exec_);
        for (const char *suffix : {"", "-wal", "-shm"})
            (void)std::remove((path_ + suffix).c_str());
    }

    int64_t count_rows()
    {
        TestUnit u;
        int ran = 0;

        u.unit.access = SC_DB_READ;
        u.unit.work = count_work;
        EXPECT_EQ(sc_db_exec_submit(exec_, 0, &u.unit, &ran), SC_OK);
        EXPECT_EQ(ran, 1);
        return u.counted;
    }

    sc_db_config config_{};
    sc_db_exec *exec_ = nullptr;
    Returns returns_;
    std::string path_;
};

/* A write leaves the submitting thread: it is queued, parked first, run by the writer and
 * handed back through the hook -- and what it committed is there to be read afterwards. */
TEST_F(SqliteExec, AWriteRunsOnTheWriterAndCommits)
{
    TestUnit u = write_unit(1);
    int ran = -1;

    ASSERT_EQ(sc_db_exec_submit(exec_, 0, &u.unit, &ran), SC_OK);
    EXPECT_EQ(ran, 0);
    ASSERT_TRUE(returns_.wait_for(1));
    EXPECT_EQ(returns_.parked.load(), 1);
    EXPECT_EQ(u.unit.status, SC_OK);
    EXPECT_EQ(u.work_status, SC_OK);
    EXPECT_NE(u.ran_on, std::this_thread::get_id());
    EXPECT_EQ(count_rows(), 1);
}

/* A read on SQLite runs where it was submitted, on that loop's own connection, before submit
 * returns: nothing is parked and nothing is handed back. */
TEST_F(SqliteExec, AReadOnSqliteRunsRightHere)
{
    TestUnit u;
    int ran = 0;

    u.unit.access = SC_DB_READ;
    u.unit.work = count_work;
    ASSERT_EQ(sc_db_exec_submit(exec_, 1, &u.unit, &ran), SC_OK);
    EXPECT_EQ(ran, 1);
    EXPECT_EQ(u.ran_on, std::this_thread::get_id());
    EXPECT_EQ(u.counted, 0);
    EXPECT_EQ(returns_.parked.load(), 0);
}

TEST_F(SqliteExec, ARollbackKeepsNothing)
{
    TestUnit u = write_unit(1);
    int ran = 0;

    u.first = SC_DB_ROLLBACK;
    ASSERT_EQ(sc_db_exec_submit(exec_, 0, &u.unit, &ran), SC_OK);
    ASSERT_TRUE(returns_.wait_for(1));
    EXPECT_EQ(u.unit.status, SC_OK);
    EXPECT_EQ(count_rows(), 0);
}

/* AGAIN runs the work once more in a fresh transaction: what the first attempt wrote is gone,
 * which is why the second can write the same unique value without colliding with itself. */
TEST_F(SqliteExec, AgainRunsTheWorkAgainInAFreshTransaction)
{
    TestUnit u = write_unit(7);
    int ran = 0;

    u.first = SC_DB_AGAIN;
    u.later = SC_DB_COMMIT;
    ASSERT_EQ(sc_db_exec_submit(exec_, 0, &u.unit, &ran), SC_OK);
    ASSERT_TRUE(returns_.wait_for(1));
    EXPECT_EQ(u.unit.status, SC_OK);
    EXPECT_EQ(u.unit.attempt, 2u);
    EXPECT_EQ(u.work_status, SC_OK);
    EXPECT_EQ(count_rows(), 1);
}

/* A unit that never stops asking is a bug, and the executor says so rather than spinning. */
TEST_F(SqliteExec, AgainForeverIsStopped)
{
    TestUnit u = write_unit(1);
    int ran = 0;

    u.first = SC_DB_AGAIN;
    u.later = SC_DB_AGAIN;
    ASSERT_EQ(sc_db_exec_submit(exec_, 0, &u.unit, &ran), SC_OK);
    ASSERT_TRUE(returns_.wait_for(1));
    EXPECT_NE(u.unit.status, SC_OK);
    EXPECT_EQ(u.unit.attempt, (uint32_t)SC_DB_AGAIN_MAX);
    EXPECT_EQ(count_rows(), 0);
}

/* The one writer busy, its queue full behind it: the next unit is refused at once, without
 * being parked, and everything that was taken still runs. */
TEST_F(SqliteExec, AFullQueueIsRefusedAndWhatWasTakenStillRuns)
{
    std::mutex gate;
    std::vector<TestUnit> units(SC_DB_QUEUE_PER_WORKER + 2);
    int ran = 0;

    gate.lock();
    for (size_t i = 0; i != units.size(); ++i) {
        units[i] = write_unit((int64_t)i);
        units[i].gate = &gate;
    }
    /* The first is taken by the writer and waits at the gate; the queue behind it fills. */
    ASSERT_EQ(sc_db_exec_submit(exec_, 0, &units[0].unit, &ran), SC_OK);
    for (int spins = 0; spins != 1000; ++spins) {
        sc_db_exec_stats stats{};

        sc_db_exec_stats_of(exec_, &stats);
        if (stats.queued == 0)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    for (size_t i = 1; i != units.size() - 1; ++i)
        ASSERT_EQ(sc_db_exec_submit(exec_, 0, &units[i].unit, &ran), SC_OK) << i;
    const int parked_before = returns_.parked.load();
    EXPECT_EQ(sc_db_exec_submit(exec_, 0, &units.back().unit, &ran), SC_ERR_QUEUE_FULL);
    EXPECT_EQ(returns_.parked.load(), parked_before);

    gate.unlock();
    ASSERT_TRUE(returns_.wait_for(units.size() - 1));
    EXPECT_EQ(count_rows(), (int64_t)units.size() - 1);
}

/* Whoever parks a unit may refuse to -- the HTTP binding does when its loop has too many
 * requests outstanding -- and then the unit is not queued and never runs. */
TEST_F(SqliteExec, AParkThatRefusesKeepsTheUnitOut)
{
    TestUnit u = write_unit(1);
    int ran = 0;

    returns_.park_answer = SC_ERR_QUEUE_FULL;
    EXPECT_EQ(sc_db_exec_submit(exec_, 0, &u.unit, &ran), SC_ERR_QUEUE_FULL);
    returns_.park_answer = SC_OK;
    EXPECT_EQ(count_rows(), 0);
}

/* A unit that waited past SC_DB_QUEUE_WAIT_MS is answered as busy instead of run -- and on time,
 * even while the only worker is stuck: the client has been kept long enough, and the database is
 * not helped by the work arriving late. */
TEST_F(SqliteExec, AUnitThatWaitedTooLongIsNotRun)
{
    std::mutex gate;
    TestUnit blocker = write_unit(1);
    TestUnit late = write_unit(2);
    int ran = 0;

    gate.lock();
    blocker.gate = &gate;
    ASSERT_EQ(sc_db_exec_submit(exec_, 0, &blocker.unit, &ran), SC_OK);
    ASSERT_EQ(sc_db_exec_submit(exec_, 0, &late.unit, &ran), SC_OK);

    /* The writer is still stuck at the gate -- and the late unit comes back anyway, a little
     * after SC_DB_QUEUE_WAIT_MS, rather than whenever the writer gets to it. */
    const auto started = std::chrono::steady_clock::now();
    ASSERT_TRUE(returns_.wait_for(1, SC_DB_QUEUE_WAIT_MS + 2000));
    const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - started)
                            .count();
    {
        std::lock_guard<std::mutex> hold(returns_.lock);
        EXPECT_EQ(returns_.units.front(), &late.unit);
    }
    EXPECT_EQ(late.unit.status, SC_ERR_QUEUE_FULL);
    EXPECT_GE(waited, SC_DB_QUEUE_WAIT_MS - 100);
    EXPECT_LT(waited, SC_DB_QUEUE_WAIT_MS + 1000);

    gate.unlock();
    ASSERT_TRUE(returns_.wait_for(2));
    EXPECT_EQ(blocker.unit.status, SC_OK);
    EXPECT_EQ(count_rows(), 1);
}

/* The same rules without an executor: startup and tools run a unit on a connection they hold. */
TEST(DbRun, RunsAUnitRightHereUnderTheSameRules)
{
    sc_db_config config{};
    sc_db *db = nullptr;
    sc_sql_error error{};
    std::string path = "gradido_run_test_" + std::to_string(getpid()) + ".sqlite";

    config.kind = SC_DB_SQLITE;
    (void)std::snprintf(config.file, sizeof(config.file), "%s", path.c_str());
    ASSERT_EQ(sc_db_open(&config, &db), SC_OK);
    ASSERT_EQ(sc_sql_simple(db, "CREATE TABLE t (v INTEGER UNIQUE)", &error), SC_OK);

    TestUnit u = write_unit(3);
    u.first = SC_DB_AGAIN;
    EXPECT_EQ(sc_db_run(db, &u.unit), SC_OK);
    EXPECT_EQ(u.unit.attempt, 2u);

    TestUnit c;
    c.unit.access = SC_DB_READ;
    c.unit.work = count_work;
    EXPECT_EQ(sc_db_run(db, &c.unit), SC_OK);
    EXPECT_EQ(c.counted, 1);

    sc_db_close(db);
    for (const char *suffix : {"", "-wal", "-shm"})
        (void)std::remove((path + suffix).c_str());
}

bool real_postgres(sc_db_config *config, uint16_t pool_size)
{
    const char *host = std::getenv("SC_DB_TEST_PG_HOST");
    const char *port = std::getenv("SC_DB_TEST_PG_PORT");
    const char *user = std::getenv("SC_DB_TEST_PG_USER");
    const char *password = std::getenv("SC_DB_TEST_PG_PASSWORD");
    const char *database = std::getenv("SC_DB_TEST_PG_DATABASE");

    if (host == nullptr || !sc_db_kind_available(SC_DB_POSTGRESQL))
        return false;
    *config = sc_db_config{};
    config->kind = SC_DB_POSTGRESQL;
    config->pool_size = pool_size;
    (void)std::snprintf(config->host, sizeof(config->host), "%s", host);
    config->port = static_cast<uint16_t>(port != nullptr ? std::atoi(port) : 5432);
    (void)std::snprintf(config->user, sizeof(config->user), "%s",
                        user != nullptr ? user : "gradido");
    (void)std::snprintf(config->password, sizeof(config->password), "%s",
                        password != nullptr ? password : "");
    (void)std::snprintf(config->database, sizeof(config->database), "%s",
                        database != nullptr ? database : "postgres");
    return true;
}

/* Four workers, eight units that each hold a connection for 200 ms: two rounds, not eight. */
TEST(PostgresExec, WorkersRunUnitsSideBySide)
{
    sc_db_config config{};
    sc_db_exec *exec = nullptr;
    Returns returns;
    uint16_t opened = 0;

    if (!real_postgres(&config, 4))
        GTEST_SKIP() << "set SC_DB_TEST_PG_HOST to a reachable PostgreSQL";

    sc_db_exec_config cfg{};
    cfg.db = &config;
    cfg.loops = 1;
    cfg.finished = Returns::finished;
    cfg.context = &returns;
    ASSERT_EQ(sc_db_exec_open(&cfg, &exec, &opened), SC_OK);
    EXPECT_EQ(opened, 4);

    std::vector<TestUnit> units(8);
    const auto started = std::chrono::steady_clock::now();
    for (auto &u : units) {
        int ran = 0;

        u.unit.access = SC_DB_READ;
        u.unit.work = sleep_work;
        ASSERT_EQ(sc_db_exec_submit(exec, 0, &u.unit, &ran), SC_OK);
        EXPECT_EQ(ran, 0) << "a PostgreSQL read goes to a worker";
    }
    ASSERT_TRUE(returns.wait_for(8));
    const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - started)
                          .count();
    for (auto &u : units) {
        EXPECT_EQ(u.unit.status, SC_OK);
        EXPECT_EQ(u.work_status, SC_OK);
    }
    EXPECT_GE(took, 380);
    EXPECT_LT(took, 1200) << "eight 200 ms units on four workers took " << took << " ms";
    sc_db_exec_close(exec);
}

sc_sql_statement kRerunInsert =
    SC_SQL_STATEMENT("exec_test.rerun_insert", "INSERT INTO rerun_t (v) VALUES ($1)", nullptr);
sc_sql_statement kRerunCount =
    SC_SQL_STATEMENT("exec_test.rerun_count", "SELECT count(*) FROM rerun_t", nullptr);

/* The first attempt makes the session forget every prepared statement halfway through, the way
 * a DISCARD ALL or a pooler handing the transaction to another server connection would. */
sc_db_end forget_halfway(sc_db *db, sc_db_unit *unit)
{
    TestUnit *self = of(unit);
    sc_sql_error error{};
    sc_sql_param first[1] = {sc_sql_int(1)};
    sc_sql_param second[1] = {sc_sql_int(2)};

    ++self->calls;
    self->work_status = sc_sql_exec(db, &kRerunInsert, first, 1, nullptr, &error);
    if (self->work_status != SC_OK)
        return SC_DB_ROLLBACK;
    /* Counted by the work itself: to the work, the rerun is the same attempt as the run it
     * repeats, which is the point being tested. */
    if (self->calls == 1 && sc_sql_simple(db, "DEALLOCATE ALL", &error) != SC_OK)
        return SC_DB_ROLLBACK;
    self->work_status = sc_sql_exec(db, &kRerunInsert, second, 1, nullptr, &error);
    return self->work_status == SC_OK ? SC_DB_COMMIT : SC_DB_ROLLBACK;
}

/*
 * Inside a transaction a lost statement cannot be prepared again on the spot -- PostgreSQL has
 * already aborted the transaction, and a second try is refused with 25P02. So the unit runs
 * again from its start, in a fresh transaction with the lost statement prepared afresh: what the
 * first run wrote went with the rollback, and the second writes it once. It is the same attempt
 * run again, not a new one.
 */
TEST(PostgresExec, AUnitWhoseSessionLostItsStatementsRunsAgain)
{
    sc_db_config config{};
    sc_db *db = nullptr;
    sc_sql_error error{};

    if (!real_postgres(&config, 1))
        GTEST_SKIP() << "set SC_DB_TEST_PG_HOST to a reachable PostgreSQL";
    ASSERT_EQ(sc_db_open(&config, &db), SC_OK);
    ASSERT_EQ(sc_sql_simple(db, "CREATE TEMP TABLE rerun_t (v bigint UNIQUE)", &error), SC_OK)
        << error.message;

    TestUnit u;
    u.unit.access = SC_DB_WRITE;
    u.unit.work = forget_halfway;
    EXPECT_EQ(sc_db_run(db, &u.unit), SC_OK) << u.unit.error.message;
    EXPECT_EQ(u.calls, 2) << "run again after the statement was lost";
    /* ...and not charged to the unit's own budget: a registration that met a DISCARD ALL still
     * has all five of its draws. */
    EXPECT_EQ(u.unit.attempt, 1u);
    EXPECT_EQ(u.work_status, SC_OK);

    sc_sql_rows rows{};
    ASSERT_EQ(sc_sql_query(db, &kRerunCount, nullptr, 0, &rows, &error), SC_OK) << error.message;
    ASSERT_TRUE(sc_sql_next(&rows));
    EXPECT_EQ(sc_sql_col_int(&rows, 0), 2);
    EXPECT_EQ(sc_sql_close(&rows), SC_OK);
    sc_db_close(db);
}

/* Workers go to cache groups the way loops do, and a group without a worker of its own joins
 * a neighbour: two L3s and four workers are two groups, two L3s and one worker are one. */
TEST(PostgresExec, WorkersAreGroupedByCache)
{
    const char *lists[] = {"0-7", "8-15"};
    sc_topology topology;
    sc_db_config config{};

    if (!real_postgres(&config, 4))
        GTEST_SKIP() << "set SC_DB_TEST_PG_HOST to a reachable PostgreSQL";
    sc_topology_from_lists(lists, 2, nullptr, &topology);
    /* Grouped but not pinned: this machine may not have CPU 15. */
    topology.pinnable = 0;

    for (uint16_t workers : {uint16_t{4}, uint16_t{1}}) {
        sc_db_exec *exec = nullptr;
        sc_db_exec_stats stats{};
        sc_db_exec_config cfg{};

        config.pool_size = workers;
        cfg.db = &config;
        cfg.loops = 16;
        cfg.topology = &topology;
        ASSERT_EQ(sc_db_exec_open(&cfg, &exec, nullptr), SC_OK);
        sc_db_exec_stats_of(exec, &stats);
        EXPECT_EQ(stats.workers, workers);
        EXPECT_EQ(stats.groups, workers == 4 ? 2 : 1);
        sc_db_exec_close(exec);
    }
}

} // namespace

int main(int argc, char **argv)
{
    sc_log_config log_cfg;
    int result;

    sc_log_default_config(&log_cfg);
    log_cfg.min_level = std::getenv("SC_DB_TEST_LOG") != nullptr ? SC_LOG_DEBUG : SC_LOG_FATAL;
    sc_log_init(&log_cfg);
    sc_log_thread_join();

    ::testing::InitGoogleTest(&argc, argv);
    result = RUN_ALL_TESTS();

    sc_log_thread_leave();
    sc_log_shutdown();
    return result;
}
