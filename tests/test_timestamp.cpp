#include <gtest/gtest.h>
#include <slick/logger.hpp>
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <fstream>
#include <regex>
#include <thread>
#include <vector>

using namespace slick::logger;

class TimestampTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Clean up any previous test files
        std::filesystem::remove("test_timestamps.log");
    }
    
    void TearDown() override {
        // Clean up test files
        std::filesystem::remove("test_timestamps.log");
    }
    
    std::string read_first_line(const std::string& filename) {
        std::ifstream file(filename);
        std::string line;
        if (file.is_open()) {
            std::getline(file, line);
        }
        return line;
    }
};

TEST_F(TimestampTest, TimestampFormatterBasicFunctionality) {
    // Test different timestamp formats
    uint64_t test_timestamp = 1693038674123456789ULL; // 2023-08-26 10:37:54.123456789
    
    TimestampFormatter default_fmt(TimestampFormatter::Format::DEFAULT);
    TimestampFormatter micro_fmt(TimestampFormatter::Format::WITH_MICROSECONDS);
    TimestampFormatter milli_fmt(TimestampFormatter::Format::WITH_MILLISECONDS);
    TimestampFormatter iso_fmt(TimestampFormatter::Format::ISO8601);
    TimestampFormatter time_fmt(TimestampFormatter::Format::TIME_ONLY);
    
    std::string default_result = default_fmt.format_timestamp(test_timestamp);
    std::string micro_result = micro_fmt.format_timestamp(test_timestamp);
    std::string milli_result = milli_fmt.format_timestamp(test_timestamp);
    std::string iso_result = iso_fmt.format_timestamp(test_timestamp);
    std::string time_result = time_fmt.format_timestamp(test_timestamp);
    
    // Verify basic structure (exact time may vary due to timezone)
    EXPECT_TRUE(std::regex_match(default_result, std::regex(R"(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2})")));
    EXPECT_TRUE(std::regex_match(micro_result, std::regex(R"(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{6})")));
    EXPECT_TRUE(std::regex_match(milli_result, std::regex(R"(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{3})")));
    EXPECT_TRUE(std::regex_match(iso_result, std::regex(R"(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{6}Z)")));
    EXPECT_TRUE(std::regex_match(time_result, std::regex(R"(\d{2}:\d{2}:\d{2}\.\d{6})")));
    
    // Verify microsecond result contains more precision than default
    EXPECT_GT(micro_result.length(), default_result.length());
    EXPECT_GT(milli_result.length(), default_result.length());
}

TEST_F(TimestampTest, CustomTimestampFormat) {
    uint64_t test_timestamp = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    
    TimestampFormatter custom_fmt("%Y%m%d_%H%M%S");
    std::string result = custom_fmt.format_timestamp(test_timestamp);
    
    // Should match YYYYMMDD_HHMMSS format
    EXPECT_TRUE(std::regex_match(result, std::regex(R"(\d{8}_\d{6})")));
}

TEST_F(TimestampTest, FileSinkWithDifferentTimestampFormats) {
    // Test file sink with different timestamp formats
    {
        FileSink default_sink("test_default.log");
        FileSink micro_sink("test_micro.log", TimestampFormatter::Format::WITH_MICROSECONDS);
        FileSink milli_sink("test_milli.log", TimestampFormatter::Format::WITH_MILLISECONDS);
        FileSink custom_sink("test_custom.log", "%H:%M:%S");
        
        LogEntry entry;
        entry.level = LogLevel::L_INFO;
        entry.timestamp = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        entry.format = {"Test message"};
        entry.arg_count = 0;
        
        default_sink.write(entry);
        micro_sink.write(entry);
        milli_sink.write(entry);
        custom_sink.write(entry);
        
        default_sink.flush();
        micro_sink.flush();
        milli_sink.flush();
        custom_sink.flush();
    }
    
    // Verify different timestamp formats were written
    std::string default_line = read_first_line("test_default.log");
    std::string micro_line = read_first_line("test_micro.log");
    std::string milli_line = read_first_line("test_milli.log");
    std::string custom_line = read_first_line("test_custom.log");
    
    EXPECT_FALSE(default_line.empty());
    EXPECT_FALSE(micro_line.empty());
    EXPECT_FALSE(milli_line.empty());
    EXPECT_FALSE(custom_line.empty());
    
    // Verify the lines contain expected patterns
    EXPECT_TRUE(std::regex_search(micro_line, std::regex(R"(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{6})")));
    EXPECT_TRUE(std::regex_search(milli_line, std::regex(R"(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{3})")));
    EXPECT_TRUE(std::regex_search(custom_line, std::regex(R"(\d{2}:\d{2}:\d{2})")));
    
    // Clean up test files
    std::filesystem::remove("test_default.log");
    std::filesystem::remove("test_micro.log");
    std::filesystem::remove("test_milli.log");
    std::filesystem::remove("test_custom.log");
}

TEST_F(TimestampTest, ConsoleSinkWithTimestampFormats) {
    // Test that console sink constructors work with different timestamp formats
    EXPECT_NO_THROW({
        ConsoleSink default_sink;
        ConsoleSink micro_sink(true, true, TimestampFormatter::Format::WITH_MICROSECONDS);
        ConsoleSink milli_sink(true, true, TimestampFormatter::Format::WITH_MILLISECONDS);
        ConsoleSink custom_sink("%H:%M:%S", true, true);
    });
}

TEST_F(TimestampTest, RotatingFileSinkWithTimestampFormats) {
    RotationConfig config;
    config.max_file_size = 1024;
    config.max_files = 3;
    
    // Test that rotating file sink constructors work with different timestamp formats
    EXPECT_NO_THROW({
        RotatingFileSink default_sink("test_rotating.log", config);
        RotatingFileSink micro_sink("test_rotating_micro.log", config, TimestampFormatter::Format::WITH_MICROSECONDS);
        RotatingFileSink custom_sink("test_rotating_custom.log", config, "%H:%M:%S");
    });
    
    // Clean up
    std::filesystem::remove("test_rotating.log");
    std::filesystem::remove("test_rotating_micro.log");
    std::filesystem::remove("test_rotating_custom.log");
}

TEST_F(TimestampTest, DailyFileSinkWithTimestampFormats) {
    RotationConfig config;
    
    // Test that daily file sink constructors work with different timestamp formats
    EXPECT_NO_THROW({
        DailyFileSink default_sink("test_daily.log", config);
        DailyFileSink micro_sink("test_daily_micro.log", config, TimestampFormatter::Format::WITH_MICROSECONDS);
        DailyFileSink custom_sink("test_daily_custom.log", config, "%H:%M:%S");
    });
    
    // Clean up
    std::filesystem::remove("test_daily.log");
    std::filesystem::remove("test_daily_micro.log");
    std::filesystem::remove("test_daily_custom.log");
}
// The formats below are produced by hand-written digit rendering over a
// per-second cache, so these tests pin the exact digits and the relationships
// between formats rather than only their shape.

TEST_F(TimestampTest, SubSecondDigitsAreExact) {
    // The date/time part depends on the local timezone, but the fractional part
    // never does, so it can be asserted exactly.
    struct Case { uint64_t sub_ns; const char* micros; const char* millis; };
    const Case cases[] = {
        {0ULL,         "000000", "000"},
        {1000ULL,      "000001", "000"},
        {9000ULL,      "000009", "000"},
        {10000ULL,     "000010", "000"},
        {999000ULL,    "000999", "000"},
        {1000000ULL,   "001000", "001"},
        {9999000ULL,   "009999", "009"},
        {10000000ULL,  "010000", "010"},
        {123456000ULL, "123456", "123"},
        {999999000ULL, "999999", "999"},
        {999999999ULL, "999999", "999"}, // sub-microsecond digits are truncated
    };

    const uint64_t whole_second = 1693038674ULL * 1000000000ULL;
    TimestampFormatter micro_fmt(TimestampFormatter::Format::WITH_MICROSECONDS);
    TimestampFormatter milli_fmt(TimestampFormatter::Format::WITH_MILLISECONDS);

    for (const auto& c : cases) {
        const uint64_t ts = whole_second + c.sub_ns;
        const std::string micro = micro_fmt.format_timestamp(ts);
        const std::string milli = milli_fmt.format_timestamp(ts);
        ASSERT_EQ(micro.size(), 26u) << "sub_ns=" << c.sub_ns;
        ASSERT_EQ(milli.size(), 23u) << "sub_ns=" << c.sub_ns;
        EXPECT_EQ(micro.substr(20, 6), c.micros) << "sub_ns=" << c.sub_ns;
        EXPECT_EQ(milli.substr(20, 3), c.millis) << "sub_ns=" << c.sub_ns;
    }
}

TEST_F(TimestampTest, FormatsAgreeWithEachOther) {
    // Every format is rendered from the same cached prefix, so they must stay
    // consistent with one another.
    TimestampFormatter default_fmt(TimestampFormatter::Format::DEFAULT);
    TimestampFormatter micro_fmt(TimestampFormatter::Format::WITH_MICROSECONDS);
    TimestampFormatter milli_fmt(TimestampFormatter::Format::WITH_MILLISECONDS);
    TimestampFormatter iso_fmt(TimestampFormatter::Format::ISO8601);
    TimestampFormatter time_fmt(TimestampFormatter::Format::TIME_ONLY);

    const uint64_t timestamps[] = {
        0ULL,                                        // epoch
        1693038674123456789ULL,                      // 2023-08-26
        1709164800987654321ULL,                      // 2024-02-29, leap day
        1735689599999999000ULL,                      // 2024-12-31 23:59:59.999999
        1735689600000000000ULL,                      // 2025-01-01 00:00:00
        4102444800000000000ULL,                      // 2100-01-01
    };

    for (uint64_t ts : timestamps) {
        const std::string def = default_fmt.format_timestamp(ts);
        const std::string micro = micro_fmt.format_timestamp(ts);
        const std::string milli = milli_fmt.format_timestamp(ts);
        const std::string iso = iso_fmt.format_timestamp(ts);
        const std::string time_only = time_fmt.format_timestamp(ts);

        ASSERT_EQ(def.size(), 19u) << "ts=" << ts;
        ASSERT_EQ(iso.size(), 27u) << "ts=" << ts;
        ASSERT_EQ(time_only.size(), 15u) << "ts=" << ts;

        EXPECT_EQ(micro.substr(0, 19), def) << "ts=" << ts;
        EXPECT_EQ(milli.substr(0, 19), def) << "ts=" << ts;
        EXPECT_EQ(milli.substr(20, 3), micro.substr(20, 3)) << "ts=" << ts;

        // ISO8601 is the microsecond format with 'T' as the separator and a 'Z' suffix.
        std::string iso_from_micro = micro;
        iso_from_micro[10] = 'T';
        iso_from_micro += 'Z';
        EXPECT_EQ(iso, iso_from_micro) << "ts=" << ts;

        // TIME_ONLY is the time portion of the microsecond format.
        EXPECT_EQ(time_only, micro.substr(11)) << "ts=" << ts;
    }
}

TEST_F(TimestampTest, PerSecondCacheRefreshesAcrossSeconds) {
    // The "YYYY-MM-DD HH:MM:SS" prefix is cached per whole second; interleaving
    // seconds must never hand back a stale prefix.
    TimestampFormatter fmt(TimestampFormatter::Format::WITH_MICROSECONDS);

    const uint64_t a = 1693038674123456000ULL;
    const uint64_t b = 1693038675123456000ULL; // one second later
    const uint64_t c = 1793038674123456000ULL; // years later

    const std::string first_a = fmt.format_timestamp(a);
    const std::string first_b = fmt.format_timestamp(b);
    const std::string first_c = fmt.format_timestamp(c);

    EXPECT_NE(first_a, first_b);
    EXPECT_NE(first_b, first_c);

    // Re-reading in a different order must reproduce the same strings.
    for (int i = 0; i < 4; ++i) {
        EXPECT_EQ(fmt.format_timestamp(c), first_c);
        EXPECT_EQ(fmt.format_timestamp(a), first_a);
        EXPECT_EQ(fmt.format_timestamp(b), first_b);
    }

    // Repeating the same second must be stable (the cache-hit path).
    for (int i = 0; i < 4; ++i) {
        EXPECT_EQ(fmt.format_timestamp(a), first_a);
    }
}

TEST_F(TimestampTest, SharedFormatterIsSafeAcrossThreads) {
    // A single formatter may legitimately be shared; the per-second cache is
    // thread-local so concurrent use must not tear the returned strings.
    const TimestampFormatter fmt(TimestampFormatter::Format::WITH_MICROSECONDS);

    std::vector<uint64_t> timestamps;
    for (int i = 0; i < 500; ++i) {
        timestamps.push_back(1693038674000000000ULL + static_cast<uint64_t>(i) * 500000000ULL);
    }

    // Expected values computed on this thread first.
    std::vector<std::string> expected;
    expected.reserve(timestamps.size());
    for (uint64_t ts : timestamps) {
        expected.push_back(fmt.format_timestamp(ts));
    }

    std::atomic<int> mismatches{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&, t]() {
            // Each thread walks the list from a different offset so the threads
            // request different seconds at the same time.
            const size_t offset = static_cast<size_t>(t) * 37;
            for (int pass = 0; pass < 20; ++pass) {
                for (size_t i = 0; i < timestamps.size(); ++i) {
                    const size_t idx = (i + offset) % timestamps.size();
                    if (fmt.format_timestamp(timestamps[idx]) != expected[idx]) {
                        ++mismatches;
                    }
                }
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    EXPECT_EQ(mismatches.load(), 0);
}

// append_timestamp() is the form every log line goes through, so it must append
// to whatever the sink's buffer already holds rather than assign over it, and
// produce exactly what format_timestamp() returns for the same timestamp. A line
// that rendered its timestamp differently depending on which one it called would
// be a difference nothing else in the header can see.
TEST_F(TimestampTest, AppendTimestampAppendsExactlyWhatFormatTimestampReturns) {
    const uint64_t test_timestamp = 1693038674123456789ULL;

    const TimestampFormatter formatters[] = {
        TimestampFormatter(TimestampFormatter::Format::DEFAULT),
        TimestampFormatter(TimestampFormatter::Format::WITH_MICROSECONDS),
        TimestampFormatter(TimestampFormatter::Format::WITH_MILLISECONDS),
        TimestampFormatter(TimestampFormatter::Format::ISO8601),
        TimestampFormatter(TimestampFormatter::Format::TIME_ONLY),
        TimestampFormatter("%Y%m%d_%H%M%S"),
    };

    for (const TimestampFormatter& fmt : formatters) {
        const std::string expected = fmt.format_timestamp(test_timestamp);

        std::string out = "prefix ";
        fmt.append_timestamp(out, test_timestamp);
        EXPECT_EQ(out, "prefix " + expected);

        // Appending twice must not reuse or overwrite the first result.
        fmt.append_timestamp(out, test_timestamp);
        EXPECT_EQ(out, "prefix " + expected + expected);

        // max_length() backs the one reserve() a log line makes, so it has to
        // cover what append_timestamp() actually writes.
        EXPECT_GE(fmt.max_length(), expected.size());
    }
}

// A CUSTOM format is split at every %f once, when it is set, so the render path
// never parses the format string again. These pin what that split has to preserve.
TEST_F(TimestampTest, CustomFormatMicrosecondsAreUnpadded) {
    // %f in a CUSTOM format means "microseconds, no leading zeros", unlike the
    // %f pattern flag, which is zero-padded to six digits.
    struct Case { uint64_t sub_ns; const char* micros; };
    const Case cases[] = {
        {0ULL,         "0"},
        {1000ULL,      "1"},
        {9000ULL,      "9"},
        {123000ULL,    "123"},
        {123456000ULL, "123456"},
        {999999000ULL, "999999"},
    };

    const uint64_t whole_second = 1693038674ULL * 1000000000ULL;
    const TimestampFormatter fmt("%H:%M:%S.%f");
    for (const auto& c : cases) {
        const std::string result = fmt.format_timestamp(whole_second + c.sub_ns);
        ASSERT_NE(result.find('.'), std::string::npos) << "sub_ns=" << c.sub_ns;
        EXPECT_EQ(result.substr(result.find('.') + 1), c.micros) << "sub_ns=" << c.sub_ns;
    }
}

TEST_F(TimestampTest, CustomFormatRendersEveryMicrosecondFlag) {
    const uint64_t ts = 1693038674ULL * 1000000000ULL + 123456000ULL;

    EXPECT_EQ(TimestampFormatter("%f").format_timestamp(ts), "123456");
    EXPECT_EQ(TimestampFormatter("[%f]").format_timestamp(ts), "[123456]");
    // More than one: rewriting the format string per line replaced only the
    // first, leaving the rest to reach strftime as an unknown conversion.
    EXPECT_EQ(TimestampFormatter("%f-%f").format_timestamp(ts), "123456-123456");
    EXPECT_EQ(TimestampFormatter("<%f><%f><%f>").format_timestamp(ts),
              "<123456><123456><123456>");
}

TEST_F(TimestampTest, CustomFormatEscapedPercentIsNotTheMicrosecondFlag) {
    const uint64_t ts = 1693038674ULL * 1000000000ULL + 123456000ULL;

    // "%%" is a literal percent, so the 'f' that follows one is a literal 'f'.
    EXPECT_EQ(TimestampFormatter("100%%f").format_timestamp(ts), "100%f");
    // And a real flag right after an escaped percent is still a flag.
    EXPECT_EQ(TimestampFormatter("%%%f").format_timestamp(ts), "%123456");
}

TEST_F(TimestampTest, CustomFormatIsRenderedWhateverItsLength) {
    const uint64_t ts = 1693038674ULL * 1000000000ULL + 123456000ULL;

    // Rendering expands through a stack buffer, and strftime() cannot say whether
    // a 0 return means "empty" or "did not fit", so a format that overruns the
    // buffer has to come out whole rather than empty. The sizes here straddle the
    // stack buffer and every step the growing path takes past it: 4096 and 5000
    // are the ones a single fixed-size fallback used to drop on the floor.
    for (const size_t length : {size_t{600}, size_t{4095}, size_t{4096}, size_t{5000},
                                size_t{20000}}) {
        const std::string filler(length, 'x');
        EXPECT_EQ(TimestampFormatter(filler + " %f").format_timestamp(ts), filler + " 123456")
            << "length=" << length;
    }

    // The growing path has to expand real conversions too, not only literal text.
    const std::string filler(5000, 'y');
    const std::string rendered = TimestampFormatter(filler + " %H:%M:%S").format_timestamp(ts);
    ASSERT_EQ(rendered.size(), filler.size() + 1 + 8) << rendered.size();
    EXPECT_EQ(rendered.substr(0, filler.size()), filler);
    EXPECT_TRUE(std::regex_match(rendered.substr(filler.size() + 1),
                                 std::regex(R"(\d{2}:\d{2}:\d{2})")))
        << rendered.substr(filler.size() + 1);

    // An empty CUSTOM format means the default layout, not an empty timestamp.
    EXPECT_EQ(TimestampFormatter(std::string{}).format_timestamp(ts),
              TimestampFormatter(TimestampFormatter::Format::DEFAULT).format_timestamp(ts));
}

// ------------------------------------------------------------------ timestamper

namespace {

constexpr uint64_t kFixedTimestamp = 1693038674123456789ULL;
constexpr const char* kTimestamperLog = "test_timestamper.log";

uint64_t system_now_ns() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

/// Records the timestamp of every entry it is handed. Written on the writer
/// thread only, and read after flush() or shutdown(), which order the two.
class TimestampCaptureSink : public ISink {
public:
    TimestampCaptureSink() : ISink("timestamp_capture") {}
    void write(const LogEntry& entry) override {
        // Copied out first: LogEntry is #pragma pack(1), so push_back's const
        // reference must not bind to entry.timestamp, which can sit misaligned.
        const uint64_t timestamp = entry.timestamp;
        stamps_.push_back(timestamp);
    }
    void flush() override {}
    const std::vector<uint64_t>& stamps() const noexcept { return stamps_; }

private:
    std::vector<uint64_t> stamps_;
};

class FixedTimestamper : public ITimestamper {
public:
    explicit FixedTimestamper(uint64_t value) : value_(value) {}
    uint64_t now() noexcept override { return value_; }

private:
    uint64_t value_;
};

/// Calls a LingeringTimestamper served after its destructor had already run.
std::atomic<int> g_calls_after_destruction{0};

/// A fixed timestamper that notices being called once destroyed. now() yields
/// between reading its value and checking it is still alive, which widens the
/// window in which a swap racing the call could free it from under the caller.
class LingeringTimestamper : public ITimestamper {
public:
    explicit LingeringTimestamper(uint64_t value) : value_(value) {}
    ~LingeringTimestamper() override { alive_.store(false, std::memory_order_relaxed); }

    uint64_t now() noexcept override {
        const uint64_t value = value_;
        std::this_thread::yield();
        if (!alive_.load(std::memory_order_relaxed)) {
            g_calls_after_destruction.fetch_add(1, std::memory_order_relaxed);
        }
        return value;
    }

private:
    uint64_t value_;
    std::atomic<bool> alive_{true};
};

/// A chrono-style clock frozen at kFixedTimestamp, in microseconds so the
/// adapter's conversion to nanoseconds is exercised too.
struct FrozenMicrosecondClock {
    using duration = std::chrono::microseconds;
    using rep = duration::rep;
    using period = duration::period;
    using time_point = std::chrono::time_point<FrozenMicrosecondClock>;
    static constexpr bool is_steady = true;
    static time_point now() noexcept {
        return time_point{duration{static_cast<rep>(kFixedTimestamp / 1000)}};
    }
};

} // namespace

class TimestamperTest : public ::testing::Test {
protected:
    void SetUp() override { Logger::instance().reset(); }

    void TearDown() override {
        Logger::instance().reset();
        std::filesystem::remove(kTimestamperLog);
    }

    /// Start the logger on a fresh capture sink. The banner init() logs is
    /// captured as well, so every test sees at least one stamp.
    std::shared_ptr<TimestampCaptureSink> start(LogConfig config = {}) {
        auto sink = std::make_shared<TimestampCaptureSink>();
        config.sinks.push_back(sink);
        Logger::instance().init(config);
        return sink;
    }

    static void expect_all_equal(const std::vector<uint64_t>& stamps, uint64_t expected) {
        ASSERT_FALSE(stamps.empty());
        for (const uint64_t stamp : stamps) {
            EXPECT_EQ(stamp, expected);
        }
    }
};

TEST_F(TimestamperTest, DefaultIsSystemClock) {
    EXPECT_EQ(Logger::instance().timestamper(), nullptr);

    const uint64_t before = system_now_ns();
    auto sink = start();
    LOG_INFO("default clock");
    Logger::instance().flush();
    const uint64_t after = system_now_ns();

    ASSERT_FALSE(sink->stamps().empty());
    for (const uint64_t stamp : sink->stamps()) {
        EXPECT_GE(stamp, before);
        EXPECT_LE(stamp, after);
    }
    EXPECT_GE(Logger::instance().current_timestamp(), after);
}

TEST_F(TimestamperTest, DerivedTimestamperStampsEveryEntry) {
    Logger::instance().set_timestamper(std::make_shared<FixedTimestamper>(kFixedTimestamp));
    EXPECT_EQ(Logger::instance().current_timestamp(), kFixedTimestamp);

    auto sink = std::make_shared<TimestampCaptureSink>();
    LogConfig config;
    config.sinks.push_back(sink);
    config.sinks.push_back(std::make_shared<FileSink>(kTimestamperLog));
    Logger::instance().init(config);
    LOG_INFO("fixed clock");
    Logger::instance().shutdown();

    ASSERT_EQ(sink->stamps().size(), 2u); // the init() banner and the message
    expect_all_equal(sink->stamps(), kFixedTimestamp);

    // End to end: the rendered line carries the configured time, not the wall clock.
    const std::string expected =
        TimestampFormatter(TimestampFormatter::Format::WITH_MICROSECONDS).format_timestamp(kFixedTimestamp);
    std::ifstream in(kTimestamperLog);
    std::string line;
    size_t lines = 0;
    while (std::getline(in, line)) {
        ++lines;
        EXPECT_EQ(line.rfind(expected, 0), 0u) << line;
    }
    EXPECT_EQ(lines, 2u);
}

TEST_F(TimestamperTest, CapturingLambdaStampsEntries) {
    std::atomic<uint64_t> ticks{0};
    Logger::instance().set_timestamper([&ticks]() noexcept {
        return kFixedTimestamp + ticks.fetch_add(1, std::memory_order_relaxed);
    });
    ASSERT_NE(Logger::instance().timestamper(), nullptr);

    auto sink = start();
    for (int i = 0; i < 10; ++i) {
        LOG_INFO("tick {}", i);
    }
    // Stopped before ticks goes out of scope; reset() in TearDown frees the lambda.
    Logger::instance().shutdown();

    // One producing thread, so the stamps reach the sink in the order taken.
    ASSERT_EQ(sink->stamps().size(), 11u);
    for (size_t i = 0; i < sink->stamps().size(); ++i) {
        EXPECT_EQ(sink->stamps()[i], kFixedTimestamp + i);
    }
}

TEST_F(TimestamperTest, ChronoClockType) {
    Logger::instance().set_timestamper<FrozenMicrosecondClock>();
    const uint64_t expected = kFixedTimestamp / 1000 * 1000;
    EXPECT_EQ(Logger::instance().current_timestamp(), expected);

    auto sink = start();
    LOG_INFO("frozen clock");
    Logger::instance().flush();
    expect_all_equal(sink->stamps(), expected);
}

TEST_F(TimestamperTest, NullRestoresSystemClock) {
    Logger::instance().set_timestamper(std::make_shared<FixedTimestamper>(kFixedTimestamp));
    ASSERT_EQ(Logger::instance().current_timestamp(), kFixedTimestamp);

    Logger::instance().set_timestamper(nullptr);
    EXPECT_EQ(Logger::instance().timestamper(), nullptr);

    const uint64_t before = system_now_ns();
    const uint64_t now = Logger::instance().current_timestamp();
    EXPECT_GE(now, before);
    EXPECT_LE(now, system_now_ns());
}

TEST_F(TimestamperTest, LogConfigTimestamperAppliesAndPersists) {
    auto fixed = std::make_shared<FixedTimestamper>(kFixedTimestamp);
    LogConfig config;
    config.timestamper = fixed;
    auto sink = start(config);
    EXPECT_EQ(Logger::instance().timestamper(), fixed);
    LOG_INFO("configured");
    Logger::instance().flush();
    // The banner too: init() installs the timestamper before it starts logging.
    expect_all_equal(sink->stamps(), kFixedTimestamp);

    // A config without one keeps the current timestamper, as an empty pattern does.
    auto second = start();
    EXPECT_EQ(Logger::instance().timestamper(), fixed);
    LOG_INFO("still configured");
    Logger::instance().flush();
    expect_all_equal(second->stamps(), kFixedTimestamp);

    // reset() is what goes back to system_clock.
    Logger::instance().reset();
    EXPECT_EQ(Logger::instance().timestamper(), nullptr);
    EXPECT_NE(Logger::instance().current_timestamp(), kFixedTimestamp);
}

TEST_F(TimestamperTest, LogConfigKeepsPositionalInitializerOrder) {
    // LogConfig is an aggregate, so a positional initializer written against a
    // release without timestamper must still compile and mean the same thing.
    // Inserting the new member anywhere but last shifts every value after it:
    // mode would land on a shared_ptr and this would not compile at all.
    auto fixed = std::make_shared<FixedTimestamper>(kFixedTimestamp);
    const LogConfig config{
        {},                          // sinks
        LogLevel::L_WARN,            // min_level
        1024,                        // log_queue_size
        4096,                        // string_buffer_size
        16,                          // string_items_per_slot
        false,                       // include_source_location
        QueueMode::SharedCollector,  // mode
        "segment",                   // shared_memory_name
        "tag",                       // process_tag
        false,                       // collect_backlog
        250,                         // stalled_entry_timeout_ms
        "%v",                        // pattern
        true,                        // enable_stats
        "stats.csv",                 // stats_file
        500,                         // stats_interval_ms
        5,                           // stats_sample_interval_ms
        2048,                        // stats_max_file_size
        fixed,                       // timestamper, the newest member
    };

    EXPECT_TRUE(config.sinks.empty());
    EXPECT_EQ(config.min_level, LogLevel::L_WARN);
    EXPECT_EQ(config.log_queue_size, 1024u);
    EXPECT_EQ(config.string_buffer_size, 4096u);
    EXPECT_EQ(config.string_items_per_slot, 16u);
    EXPECT_FALSE(config.include_source_location);
    EXPECT_EQ(config.mode, QueueMode::SharedCollector);
    EXPECT_EQ(config.shared_memory_name, "segment");
    EXPECT_EQ(config.process_tag, "tag");
    EXPECT_FALSE(config.collect_backlog);
    EXPECT_EQ(config.stalled_entry_timeout_ms, 250u);
    EXPECT_EQ(config.pattern, "%v");
    EXPECT_TRUE(config.enable_stats);
    EXPECT_EQ(config.stats_file, std::filesystem::path("stats.csv"));
    EXPECT_EQ(config.stats_interval_ms, 500u);
    EXPECT_EQ(config.stats_sample_interval_ms, 5u);
    EXPECT_EQ(config.stats_max_file_size, 2048u);
    EXPECT_EQ(config.timestamper, fixed);
}

TEST_F(TimestamperTest, SwapWhileLoggingIsSafe) {
    constexpr uint64_t kA = kFixedTimestamp;
    constexpr uint64_t kB = kFixedTimestamp + 1000;
    constexpr int kThreads = 4;
    constexpr int kPerThread = 2000;

    g_calls_after_destruction.store(0, std::memory_order_relaxed);
    Logger::instance().set_timestamper(std::make_shared<LingeringTimestamper>(kA));
    auto sink = start();

    std::atomic<int> producing{kThreads};
    std::vector<std::thread> producers;
    for (int t = 0; t < kThreads; ++t) {
        producers.emplace_back([&producing] {
            for (int i = 0; i < kPerThread; ++i) {
                LOG_INFO("swap {}", i);
            }
            producing.fetch_sub(1, std::memory_order_relaxed);
        });
    }
    // A fresh object every swap, and the main thread keeps no reference to it:
    // a replaced timestamper freed on the spot would be a use-after-free here.
    uint64_t swaps = 0;
    do {
        Logger::instance().set_timestamper(
            std::make_shared<LingeringTimestamper>((swaps++ & 1) ? kB : kA));
    } while (producing.load(std::memory_order_relaxed) > 0);
    for (auto& producer : producers) {
        producer.join();
    }
    Logger::instance().flush();

    EXPECT_GT(swaps, 0u);
    EXPECT_EQ(g_calls_after_destruction.load(std::memory_order_relaxed), 0);
    ASSERT_EQ(sink->stamps().size(), static_cast<size_t>(kThreads * kPerThread + 1));
    for (const uint64_t stamp : sink->stamps()) {
        EXPECT_TRUE(stamp == kA || stamp == kB) << stamp;
    }
}

// ------------------------------------------------------------- TscTimestamper

namespace {

// Far wider than the timestamper's own error, which is a few microseconds at
// most: this only has to tolerate a loaded CI machine descheduling the test
// between its two clock reads.
constexpr int64_t kWallClockTolerance = 2'000'000; // 2 ms

/// The signed gap between a TscTimestamper reading and the wall clock, taken
/// between two system_clock reads so the gap excludes the time spent reading.
int64_t gap_to_wall_clock(TscTimestamper& tsc, uint64_t& stamp) {
    const uint64_t before = system_now_ns();
    stamp = tsc.now();
    const uint64_t after = system_now_ns();
    if (stamp < before) return static_cast<int64_t>(stamp - before);
    if (stamp > after) return static_cast<int64_t>(stamp - after);
    return 0;
}

} // namespace

TEST(TscTimestamperTest, TracksTheWallClock) {
    TscTimestamper tsc;
    if (!tsc.uses_cycle_counter()) {
        GTEST_SKIP() << "no invariant cycle counter here (a VM?); now() is system_clock";
    }
    uint64_t previous = 0;
    for (int i = 0; i < 2000; ++i) {
        uint64_t stamp = 0;
        const int64_t gap = gap_to_wall_clock(tsc, stamp);
        EXPECT_LE(std::abs(gap), kWallClockTolerance) << "sample " << i;
        EXPECT_GE(stamp, previous) << "sample " << i;
        previous = stamp;
        if (i % 100 == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
}

TEST(TscTimestamperTest, ReanchorsWithoutGoingBackwards) {
    // A 20 ms interval re-anchors a dozen times in this test. Each one slews the
    // drift away rather than stepping, so one thread's stamps never decrease.
    TscTimestamper tsc(std::chrono::milliseconds(2), std::chrono::milliseconds(20));
    if (!tsc.uses_cycle_counter()) {
        GTEST_SKIP() << "no invariant cycle counter here; now() is system_clock";
    }
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
    uint64_t previous = 0;
    int samples = 0;
    while (std::chrono::steady_clock::now() < end) {
        uint64_t stamp = 0;
        const int64_t gap = gap_to_wall_clock(tsc, stamp);
        EXPECT_LE(std::abs(gap), kWallClockTolerance) << "sample " << samples;
        ASSERT_GE(stamp, previous) << "went backwards at sample " << samples;
        previous = stamp;
        ++samples;
    }
    EXPECT_GT(samples, 1000);
}

TEST(TscTimestamperTest, ConcurrentReadersDuringReanchors) {
    // A 1 ms interval puts many threads past it at once, all racing to re-anchor
    // while the rest keep reading the anchor being replaced.
    TscTimestamper tsc(std::chrono::milliseconds(2), std::chrono::milliseconds(1));
    constexpr int kThreads = 4;
    std::atomic<int64_t> worst_gap{0};
    std::vector<std::thread> readers;
    for (int t = 0; t < kThreads; ++t) {
        readers.emplace_back([&] {
            const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
            int64_t worst = 0;
            while (std::chrono::steady_clock::now() < end) {
                uint64_t stamp = 0;
                worst = std::max(worst, std::abs(gap_to_wall_clock(tsc, stamp)));
            }
            int64_t seen = worst_gap.load();
            while (seen < worst && !worst_gap.compare_exchange_weak(seen, worst)) {
            }
        });
    }
    for (auto& reader : readers) {
        reader.join();
    }
    EXPECT_LE(worst_gap.load(), kWallClockTolerance);
}

TEST_F(TimestamperTest, TscTimestamperStampsEntries) {
    auto tsc = std::make_shared<TscTimestamper>();
    Logger::instance().set_timestamper(tsc);
    const uint64_t before = system_now_ns();
    auto sink = start();
    for (int i = 0; i < 100; ++i) {
        LOG_INFO("tsc {}", i);
    }
    Logger::instance().flush();
    const uint64_t after = system_now_ns();

    // One producing thread, so the stamps arrive in the order they were taken.
    ASSERT_EQ(sink->stamps().size(), 101u); // the init() banner and the messages
    uint64_t previous = 0;
    for (const uint64_t stamp : sink->stamps()) {
        EXPECT_GE(stamp + kWallClockTolerance, before);
        EXPECT_LE(stamp, after + kWallClockTolerance);
        EXPECT_GE(stamp, previous);
        previous = stamp;
    }
}
