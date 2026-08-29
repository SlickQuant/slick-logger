#include <gtest/gtest.h>
#include <slick/logger.hpp>
#include <fstream>
#include <regex>

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