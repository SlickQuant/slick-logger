#include <gtest/gtest.h>
#include <slick/logger.hpp>
#include <atomic>
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
