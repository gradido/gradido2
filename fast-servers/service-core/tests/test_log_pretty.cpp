/*
 * sc_log_pretty_measure() against sc_log_encode_pretty(), over every shape a record has.
 *
 * The two walk the same fields in the same order and nothing in the language keeps them in step.
 * The drain loop decides on the measurement whether a line still fits, and every append inside
 * the encoder is unchecked -- so a measurement that ran short would be a write past the end.
 * This is the guard for that, and it is the reason a pretty line is measured at all: `event`,
 * `err_name` and every `data[i].key` are pointers into the caller's rodata that the submitting
 * side borrows rather than copies, so no constant derived from the arena can bound the line.
 *
 * This test reaches into service-core/src/log, which is the one thing the unit tests otherwise
 * do not do: the two calls are not on the component's surface and should not be, because
 * nothing outside the logger has any business encoding a record. build.zig puts that directory
 * on this test's include path and nothing else's.
 *
 * It is also the one target built with assertions left on -- arnm's unchecked appends assert
 * their preconditions only where the calling translation unit keeps NDEBUG off.
 *
 * C++ because googletest is; see the note at the top of test_cache.cpp.
 */
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string>

extern "C" {
#include "format.h"
#include "record.h"
}

namespace
{

/*
 * One buffer for every case, big enough for the widest line any of them makes. The encoder is
 * unchecked, so the room is checked here instead -- which is exactly the contract the drain
 * loop keeps for it.
 */
class Buffer
{
  public:
    Buffer() { init_ = arnm_byte_buffer_init(&buffer_, 256u * 1024u, nullptr) == ARNM_SUCCESS; }
    ~Buffer()
    {
        if (init_)
            arnm_byte_buffer_free(&buffer_, nullptr);
    }

    Buffer(const Buffer &) = delete;
    Buffer &operator=(const Buffer &) = delete;

    bool ok() const { return init_; }
    arnm_byte_buffer *get() { return &buffer_; }

  private:
    arnm_byte_buffer buffer_ {};
    bool init_ = false;
};

Buffer &buffer()
{
    static Buffer one;
    return one;
}

/** Measures the record, writes it, and holds the two answers together. */
void expect_measured_length(const sc_log_record &record, int color, const char *what)
{
    sc_log_pretty_plan plan;
    const uint8_t *text = nullptr;
    uint32_t wrote = 0;
    const size_t want = sc_log_pretty_measure(&plan, &record, color);

    arnm_byte_buffer_clear(buffer().get());
    ASSERT_GE(arnm_byte_buffer_available(buffer().get()), want)
        << what << ": the test buffer is too small for " << want << " bytes";

    sc_log_encode_pretty(buffer().get(), &plan, &record, color);
    ASSERT_EQ(arnm_byte_buffer_access(buffer().get(), &text, &wrote), ARNM_SUCCESS);

    EXPECT_EQ((size_t)wrote, want) << what << " (color=" << color << ")";
    ASSERT_GT(wrote, 0u) << what;
    EXPECT_EQ(text[wrote - 1], '\n') << what << ": the line does not end in a newline";
}

void expect_both(const sc_log_record &record, const char *what)
{
    expect_measured_length(record, 0, what);
    expect_measured_length(record, 1, what);
}

sc_log_record base_record()
{
    sc_log_record record;
    std::memset(&record, 0, sizeof(record));
    record.time_ms = 45130092;
    record.event = "request.complete";
    record.msg = "request completed";
    record.level = SC_LOG_INFO;
    record.cat = SC_CAT_HTTP;
    return record;
}

class LogPretty : public ::testing::Test
{
  protected:
    void SetUp() override { ASSERT_TRUE(buffer().ok()); }
};

TEST_F(LogPretty, EveryLevelAndEveryOptionalPiece)
{
    for (uint8_t level = 0; level <= SC_LOG_FATAL; ++level) {
        sc_log_record record = base_record();
        record.level = level;
        expect_both(record, "envelope only");

        record.req = "c0ffee-1234";
        expect_both(record, "with req");
        record.usr = 4711;
        expect_both(record, "with usr");
        record.err_name = "TimeoutError";
        record.err_code = 504;
        expect_both(record, "with err");
    }
}

TEST_F(LogPretty, EmptyPiecesGoThroughUnguarded)
{
    /* A run of length zero reaches arnm unguarded -- an empty message and an empty data value
     * are ordinary, and guarding twenty appends against them cost more than it saved. */
    sc_log_value values[2];
    sc_log_record record = base_record();
    record.time_ms = 1;
    record.event = "";
    record.msg = "";
    record.cat = SC_CAT_DB;
    record.req = "";
    record.err_name = "";
    record.err_code = 0;
    values[0] = sc_log_value SC_LOG_STR("", "");
    values[1] = sc_log_value SC_LOG_STR("", nullptr);
    record.data = values;
    record.data_count = 2;
    expect_both(record, "empty pieces");
}

TEST_F(LogPretty, EveryCategoryIncludingOnePastTheEnd)
{
    for (size_t cat = 0; cat <= (size_t)SC_CAT__COUNT; ++cat) {
        sc_log_record record = base_record();
        record.time_ms = 1;
        record.event = "e";
        record.msg = "m";
        record.cat = (uint8_t)cat;
        expect_both(record, "category");
    }
}

TEST_F(LogPretty, EveryCountAndEveryKindOfDataValue)
{
    for (uint16_t count = 0; count <= SC_LOG_DATA_MAX; ++count) {
        sc_log_value values[SC_LOG_DATA_MAX];
        sc_log_record record = base_record();
        record.time_ms = 1;
        record.event = "e";
        record.msg = "m";
        record.cat = SC_CAT_DB;
        record.data = values;
        record.data_count = count;
        for (uint16_t i = 0; i != count; ++i) {
            switch (i % 6) {
            case 0: values[i] = sc_log_value SC_LOG_STR("key", "value"); break;
            case 1: values[i] = sc_log_value SC_LOG_STR("key", nullptr); break;
            case 2: values[i] = sc_log_value SC_LOG_INT("key", -1234); break;
            case 3: values[i] = sc_log_value SC_LOG_UINT("key", 42u); break;
            case 4: values[i] = sc_log_value SC_LOG_BOOL("key", i & 1); break;
            default: values[i] = sc_log_value SC_LOG_NULL("key"); break;
            }
        }
        expect_both(record, "data values");
    }
}

TEST_F(LogPretty, NumberBoundaries)
{
    static const int64_t kInts[] = {0,         1,         -1,       9,        -9,
                                    10,        -10,       999999999, INT64_MAX, INT64_MIN};
    static const uint64_t kUints[] = {0u, 1u, 9u, 10u, 4711u, UINT64_MAX};

    for (int64_t number : kInts) {
        sc_log_value values[1] = {SC_LOG_INT("n", 0)};
        sc_log_record record = base_record();
        record.time_ms = 1;
        record.event = "e";
        record.msg = "m";
        record.cat = SC_CAT_DB;
        values[0].number = number;
        record.data = values;
        record.data_count = 1;
        expect_both(record, "int64 boundary");
    }
    for (uint64_t number : kUints) {
        sc_log_value values[1] = {SC_LOG_UINT("n", 0)};
        sc_log_record record = base_record();
        record.time_ms = 1;
        record.event = "e";
        record.msg = "m";
        record.cat = SC_CAT_DB;
        record.usr = number;
        record.err_name = "E";
        record.err_code = (uint32_t)number;
        values[0].unumber = number;
        record.data = values;
        record.data_count = 1;
        expect_both(record, "uint64 boundary");
    }
}

TEST_F(LogPretty, EveryReadingTheClockCanShow)
{
    /* The stamp is twelve characters whatever the time is -- the padding only ever adds zeros,
     * and a measurement that assumed otherwise would be wrong for one hour in ten. */
    for (int64_t ms = 0; ms < 86400000; ms += 997) {
        sc_log_record record = base_record();
        record.time_ms = ms;
        record.event = "e";
        record.msg = "m";
        record.cat = SC_CAT_DB;
        expect_measured_length(record, 0, "clock");
    }
}

TEST_F(LogPretty, LongBorrowedLiteralsOutrunAnyConstantBound)
{
    /* The case that made the old constant bound wrong: none of these is in the arena, so the
     * cap on the arena says nothing at all about the line they spell. */
    static char long_key[600];
    static char long_event[600];
    static char long_err[600];
    static char long_text[600];
    sc_log_value values[SC_LOG_DATA_MAX];
    sc_log_pretty_plan plan;

    std::memset(long_key, 'k', sizeof(long_key) - 1);
    std::memset(long_event, 'e', sizeof(long_event) - 1);
    std::memset(long_err, 'E', sizeof(long_err) - 1);
    std::memset(long_text, 't', sizeof(long_text) - 1);

    sc_log_record record = base_record();
    record.event = long_event;
    record.err_name = long_err;
    record.err_code = 504;
    record.msg = "m";
    record.req = "r";
    record.usr = UINT64_MAX;
    for (size_t i = 0; i != SC_LOG_DATA_MAX; ++i)
        values[i] = sc_log_value SC_LOG_STR(long_key, long_text);
    record.data = values;
    record.data_count = SC_LOG_DATA_MAX;
    expect_both(record, "long borrowed literals");

    EXPECT_GT(sc_log_pretty_measure(&plan, &record, 1), (size_t)SC_LOG_GRADE_MAX)
        << "this record was supposed to spell a line wider than its arena could ever be";
}

} // namespace

int main(int argc, char **argv)
{
    /* No logger: nothing here writes a line. The two calls under test take a record and a
     * buffer and know nothing about a ring. */
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
