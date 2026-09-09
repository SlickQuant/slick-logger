#include <slick/logger.hpp>
#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

class SlickLoggerTest : public ::testing::Test {
protected:
    void TearDown() override {
        // Ensure the logger is fully stopped and reset between tests so that
        // sinks (and the string_view keys in sinkname_index_map_ that point
        // into their name strings) are destroyed before the next test starts.
        slick::logger::Logger::instance().shutdown();
        slick::logger::Logger::instance().reset();

        // Clean up all test log files
        std::filesystem::remove("test.log");
        std::filesystem::remove("test_mt.log");
        std::filesystem::remove("test_json.log");
        std::filesystem::remove("test_format_error.log");
        std::filesystem::remove("test_no_args.log");
        std::filesystem::remove("test_mixed.log");
        std::filesystem::remove("test_char_array.log");
        std::filesystem::remove("test_char_array_struct.log");
        std::filesystem::remove("test_volatile_char_array.log");
        std::filesystem::remove("test_cv_pointer.log");
        std::filesystem::remove("test_single_string.log");
        std::filesystem::remove("test_empty_string.log");
        std::filesystem::remove("test_format_args.log");
        std::filesystem::remove("test_format_args_const_char.log");
        std::filesystem::remove("test_source_location.log");
        std::filesystem::remove("test_source_location_disabled.log");
        std::filesystem::remove("test_source_location_config.log");
        std::filesystem::remove("test_set_instance_host.log");
        std::filesystem::remove("test_set_instance_plugin.log");
        std::filesystem::remove("test_sink_macro.log");
        std::filesystem::remove("test_sink_orphan.log");
        std::filesystem::remove("test_numbered.log");
        std::filesystem::remove("test_wchar.log");
        std::filesystem::remove("test_dynamic_spec.log");
        std::filesystem::remove("test_dynamic_spec_errors.log");
    }
};

TEST_F(SlickLoggerTest, BasicLogging) {
    // Clean up any existing log file
    std::filesystem::remove("test.log");

    // Initialize logger
    slick::logger::Logger::instance().init("test.log", 1024);

    // Log a message
    LOG_INFO("Test message");

    // Shutdown
    slick::logger::Logger::instance().shutdown();

    // Check if file was created and contains the message
    ASSERT_TRUE(std::filesystem::exists("test.log"));

    std::ifstream log_file("test.log");
    std::string line;
    std::getline(log_file, line);   // first line is the logger's version
    std::getline(log_file, line);
    EXPECT_TRUE(line.find("Test message") != std::string::npos);
}

TEST_F(SlickLoggerTest, LogFilter) {
    // Clean up any existing log file
    std::filesystem::remove("test.log");

    // Initialize logger
    slick::logger::Logger::instance().init("test.log", 1024);
    slick::logger::Logger::instance().set_level(slick::logger::LogLevel::L_INFO);

    LOG_INFO("Test message");
    LOG_DEBUG("This debug message should not appear");
    LOG_WARN("This is a warning");
    LOG_TRACE("This trace message should not appear");
    LOG_ERROR("This is an error");
    LOG_FATAL("This is fatal");

    // Shutdown
    slick::logger::Logger::instance().shutdown();

    // Check if file was created and contains the message
    ASSERT_TRUE(std::filesystem::exists("test.log"));

    std::ifstream log_file("test.log");
    std::string line;
    std::getline(log_file, line);   // first line is the logger's version
    std::getline(log_file, line);
    EXPECT_TRUE(line.find("Test message") != std::string::npos);
    std::getline(log_file, line);
    EXPECT_TRUE(line.find("This is a warning") != std::string::npos);
    std::getline(log_file, line);
    EXPECT_TRUE(line.find("This is an error") != std::string::npos);
    std::getline(log_file, line);
    EXPECT_TRUE(line.find("This is fatal") != std::string::npos);
}

TEST_F(SlickLoggerTest, DisabledMacrosDoNotEvaluateArguments) {
    std::filesystem::remove("test.log");

    slick::logger::Logger::instance().init("test.log", 1024);
    slick::logger::Logger::instance().set_level(slick::logger::LogLevel::L_INFO);

    int evaluation_count = 0;
    auto expensive_message = [&]() -> std::string {
        ++evaluation_count;
        return std::format("other format can't avoid {}", "YEAH");
    };

    LOG_DEBUG("Some format {}", expensive_message());
    LOG_TRACE("Some format {}", expensive_message());
    LOG_INFO("Some format {}", expensive_message());

    slick::logger::Logger::instance().shutdown();

    EXPECT_EQ(evaluation_count, 1);

    std::ifstream log_file("test.log");
    std::string line;
    std::getline(log_file, line);   // first line is the logger's version
    std::getline(log_file, line);
    EXPECT_TRUE(line.find("Some format other format can't avoid YEAH") != std::string::npos);
    EXPECT_FALSE(std::getline(log_file, line));
}

TEST_F(SlickLoggerTest, MacrosIncludeCallSiteSourceLocationByDefault) {
    std::filesystem::remove("test_source_location.log");

    slick::logger::Logger::instance().init("test_source_location.log", 1024);

    const int expected_line = __LINE__ + 1;
    LOG_INFO("Source location message");

    slick::logger::Logger::instance().shutdown();

    ASSERT_TRUE(std::filesystem::exists("test_source_location.log"));

    std::ifstream log_file("test_source_location.log");
    std::string line;
    std::getline(log_file, line);   // first line is the logger's version
    ASSERT_TRUE(std::getline(log_file, line));
    EXPECT_NE(line.find("test_logger.cpp:" + std::to_string(expected_line)), std::string::npos);
    EXPECT_NE(line.find("Source location message"), std::string::npos);
}

TEST_F(SlickLoggerTest, SourceLocationCanBeDisabledAtRuntime) {
    std::filesystem::remove("test_source_location_disabled.log");

    slick::logger::Logger::instance().init("test_source_location_disabled.log", 1024);
    slick::logger::Logger::instance().set_source_location_enabled(false);

    LOG_INFO("No source location message");

    slick::logger::Logger::instance().shutdown();

    ASSERT_TRUE(std::filesystem::exists("test_source_location_disabled.log"));

    std::ifstream log_file("test_source_location_disabled.log");
    std::string line;
    std::getline(log_file, line);   // first line is the logger's version
    ASSERT_TRUE(std::getline(log_file, line));
    EXPECT_EQ(line.find("test_logger.cpp:"), std::string::npos);
    EXPECT_NE(line.find("No source location message"), std::string::npos);
}

TEST_F(SlickLoggerTest, DynamicSourceLocationIsCopiedForDirectCalls) {
    std::filesystem::remove("test_source_location.log");

    slick::logger::Logger::instance().init("test_source_location.log", 1024);

    std::string source_path = "C:\\repo\\slick-logger\\tests\\dynamic_source.cpp";
    slick::logger::Logger::instance().log_with_location(
        slick::logger::LogLevel::L_INFO,
        source_path.c_str(),
        777,
        "Dynamic source location message");
    source_path.assign("overwritten.cpp");

    std::string split_source_path = "C:\\repo\\slick-logger\\tests\\split_source.cpp";
    slick::logger::Logger::instance().log_with_location(
        slick::logger::LogLevel::L_WARN,
        split_source_path.c_str(),
        778,
        "Dynamic split source location message");
    split_source_path.assign("overwritten_path.cpp");

    std::string file_name = "file_name_source.cpp";
    slick::logger::Logger::instance().log_with_location(
        slick::logger::LogLevel::L_WARN,
        file_name.c_str(),
        779,
        "Dynamic file name source location message");
    file_name.assign("overwritten_file_name.cpp");

    slick::logger::Logger::instance().shutdown();

    ASSERT_TRUE(std::filesystem::exists("test_source_location.log"));

    std::ifstream log_file("test_source_location.log");
    std::string file_contents;
    std::string line;
    std::getline(log_file, line);   // first line is the logger's version
    while (std::getline(log_file, line)) {
        file_contents += line + "\n";
    }

    EXPECT_NE(file_contents.find("dynamic_source.cpp:777"), std::string::npos);
    EXPECT_NE(file_contents.find("Dynamic source location message"), std::string::npos);
    EXPECT_NE(file_contents.find("split_source.cpp:778"), std::string::npos);
    EXPECT_NE(file_contents.find("Dynamic split source location message"), std::string::npos);
    EXPECT_NE(file_contents.find("file_name_source.cpp:779"), std::string::npos);
    EXPECT_NE(file_contents.find("Dynamic file name source location message"), std::string::npos);
    EXPECT_EQ(file_contents.find("overwritten.cpp"), std::string::npos);
    EXPECT_EQ(file_contents.find("overwritten_path.cpp"), std::string::npos);
    EXPECT_EQ(file_contents.find("overwritten_name.cpp"), std::string::npos);
    EXPECT_EQ(file_contents.find("overwritten_file_name.cpp"), std::string::npos);
}

TEST_F(SlickLoggerTest, EmptySourceLocationIsSuppressed) {
    std::filesystem::remove("test_source_location.log");

    slick::logger::Logger::instance().init("test_source_location.log", 1024);

    slick::logger::Logger::instance().log_with_location(
        slick::logger::LogLevel::L_INFO,
        "",
        42,
        "Empty source location message");
    slick::logger::Logger::instance().log_with_location(
        slick::logger::LogLevel::L_WARN,
        "/",
        43,
        "Root source location message");

    slick::logger::Logger::instance().shutdown();

    ASSERT_TRUE(std::filesystem::exists("test_source_location.log"));

    std::ifstream log_file("test_source_location.log");
    std::string file_contents;
    std::string line;
    std::getline(log_file, line);   // first line is the logger's version
    while (std::getline(log_file, line)) {
        file_contents += line + "\n";
    }

    EXPECT_EQ(file_contents.find(" [:42]"), std::string::npos);
    EXPECT_EQ(file_contents.find(" [:43]"), std::string::npos);
    EXPECT_NE(file_contents.find("Empty source location message"), std::string::npos);
    EXPECT_NE(file_contents.find("Root source location message"), std::string::npos);
}


TEST_F(SlickLoggerTest, LogConfigCanDisableSourceLocation) {
    std::filesystem::remove("test_source_location_config.log");

    slick::logger::LogConfig config;
    config.sinks.push_back(std::make_shared<slick::logger::FileSink>("test_source_location_config.log"));
    config.include_source_location = false;

    slick::logger::Logger::instance().init(config);

    LOG_WARN("Config disabled source location message");

    slick::logger::Logger::instance().shutdown();

    ASSERT_TRUE(std::filesystem::exists("test_source_location_config.log"));

    std::ifstream log_file("test_source_location_config.log");
    std::string line;
    std::getline(log_file, line);   // first line is the logger's version
    ASSERT_TRUE(std::getline(log_file, line));
    EXPECT_EQ(line.find("test_logger.cpp:"), std::string::npos);
    EXPECT_NE(line.find("Config disabled source location message"), std::string::npos);
}


TEST_F(SlickLoggerTest, MultiThreadedLogging) {
    std::filesystem::remove("test_mt.log");

    slick::logger::Logger::instance().init("test_mt.log", 1024);

    // Log from multiple threads
    std::thread t1([]() {
        for (int i = 0; i < 5; ++i) {
            LOG_INFO("Thread 1: {}", i);
        }
    });

    std::thread t2([]() {
        for (int i = 0; i < 5; ++i) {
            LOG_INFO("Thread 2: {}", i);
        }
    });

    t1.join();
    t2.join();

    slick::logger::Logger::instance().shutdown();

    // Count lines in log file
    std::ifstream log_file("test_mt.log");
    std::string line;
    int count = 0;
    while (std::getline(log_file, line)) {
        count++;
    }
    EXPECT_EQ(count, 11); // 5 from each thread  + 1 version line
}

TEST_F(SlickLoggerTest, JSONStringLogging) {
    std::filesystem::remove("test_json.log");
    
    slick::logger::Logger::instance().init("test_json.log", 1024);
    
    // Test logging JSON strings with curly braces (no arguments)
    LOG_INFO("[{\"T\":\"success\",\"msg\":\"connected\"}]");
    LOG_INFO("{\"user\":\"alice\",\"status\":\"active\",\"count\":42}");
    LOG_INFO("Complex JSON: {\"data\":{\"nested\":{\"value\":\"test\"}}}");
    
    slick::logger::Logger::instance().shutdown();
    
    // Verify the JSON strings were logged correctly
    ASSERT_TRUE(std::filesystem::exists("test_json.log"));
    
    std::ifstream log_file("test_json.log");
    std::string line;
    std::getline(log_file, line);   // first line is the logger's version
    
    std::getline(log_file, line);
    EXPECT_TRUE(line.find("[{\"T\":\"success\",\"msg\":\"connected\"}]") != std::string::npos);
    
    std::getline(log_file, line);
    EXPECT_TRUE(line.find("{\"user\":\"alice\",\"status\":\"active\",\"count\":42}") != std::string::npos);
    
    std::getline(log_file, line);
    EXPECT_TRUE(line.find("Complex JSON: {\"data\":{\"nested\":{\"value\":\"test\"}}}") != std::string::npos);
}

TEST_F(SlickLoggerTest, FormatErrorHandling) {
    std::filesystem::remove("test_format_error.log");
    
    slick::logger::Logger::instance().init("test_format_error.log", 1024);
    
    // Test various malformed format strings that should trigger exception handling
    LOG_INFO("Unmatched opening brace: {incomplete");
    LOG_INFO("Wrong argument count: {} {} {}", 42);  // 3 placeholders, 1 argument
    LOG_INFO("Invalid format spec: {invalid_spec}");
    LOG_INFO("Mixed issues: {unclosed and {} with missing args", "partial");
    
    // Test valid formats to ensure they still work
    LOG_INFO("Valid format: {}", "works");
    LOG_INFO("Multiple valid: {} and {}", "first", "second");
    
    slick::logger::Logger::instance().shutdown();
    
    // Verify error handling worked and log file exists
    ASSERT_TRUE(std::filesystem::exists("test_format_error.log"));
    
    std::ifstream log_file("test_format_error.log");
    std::string line;
    std::getline(log_file, line);   // first line is the logger's version
    std::string file_contents;
    while (std::getline(log_file, line)) {
        file_contents += line + "\n";
    }
    
    // Check that malformed strings are logged with error info
    EXPECT_TRUE(file_contents.find("Unmatched opening brace: {incomplete") != std::string::npos);
    EXPECT_TRUE(file_contents.find("[FORMAT_ERROR:") != std::string::npos);
    EXPECT_TRUE(file_contents.find("Wrong argument count: 42 <MISSING_ARG> <MISSING_ARG>") != std::string::npos);
    
    // Check that valid formats still work correctly
    EXPECT_TRUE(file_contents.find("Valid format: works") != std::string::npos);
    EXPECT_TRUE(file_contents.find("Multiple valid: first and second") != std::string::npos);
}

TEST_F(SlickLoggerTest, NoArgumentsFormatting) {
    std::filesystem::remove("test_no_args.log");
    
    slick::logger::Logger::instance().init("test_no_args.log", 1024);
    
    // Test strings with curly braces but no arguments - should be logged as-is
    LOG_INFO("No args: This {has} {curly} {braces}");
    LOG_INFO("WebSocket message: {\"type\":\"message\",\"data\":{\"id\":123}}");
    LOG_INFO("C++ code snippet: if (condition) { return {}; }");
    
    // Test empty format string
    LOG_INFO("");
    
    slick::logger::Logger::instance().shutdown();
    
    ASSERT_TRUE(std::filesystem::exists("test_no_args.log"));
    
    std::ifstream log_file("test_no_args.log");
    std::string line;
    std::getline(log_file, line);   // first line is the logger's version
    std::string file_contents;
    while (std::getline(log_file, line)) {
        file_contents += line + "\n";
    }
    
    // Verify strings are logged exactly as provided (no formatting attempted)
    EXPECT_TRUE(file_contents.find("No args: This {has} {curly} {braces}") != std::string::npos);
    EXPECT_TRUE(file_contents.find("WebSocket message: {\"type\":\"message\",\"data\":{\"id\":123}}") != std::string::npos);
    EXPECT_TRUE(file_contents.find("C++ code snippet: if (condition) { return {}; }") != std::string::npos);
}

TEST_F(SlickLoggerTest, MixedValidAndInvalidFormats) {
    std::filesystem::remove("test_mixed.log");
    
    slick::logger::Logger::instance().init("test_mixed.log", 1024);
    
    // Mix of valid formatting, invalid formatting, and no-argument logging
    LOG_INFO("Valid: User {} has {} points", "Alice", 100);
    LOG_INFO("Invalid: Too many placeholders {} {} {}", "only_one");  // 3 placeholders, 1 argument
    LOG_INFO("JSON: {\"status\":\"ok\",\"code\":200}");
    LOG_INFO("Valid again: Temperature is {:.1f}°C", 23.5);
    LOG_INFO("Escaped braces: value={{{}}} tail={{done}}", 42);
    LOG_INFO("Invalid stray close brace: } {}", 42);
    LOG_INFO("Broken: {invalid} format {"); // just a string literal
    
    slick::logger::Logger::instance().shutdown();
    
    ASSERT_TRUE(std::filesystem::exists("test_mixed.log"));
    
    std::ifstream log_file("test_mixed.log");
    std::string file_contents;
    std::string line;
    std::getline(log_file, line);   // first line is the logger's version
    while (std::getline(log_file, line)) {
        file_contents += line + "\n";
    }
    
    // Check valid formats work
    EXPECT_TRUE(file_contents.find("Valid: User Alice has 100 points") != std::string::npos);
    EXPECT_TRUE(file_contents.find("Temperature is 23.5°C") != std::string::npos);
    EXPECT_TRUE(file_contents.find("Escaped braces: value={42} tail={done}") != std::string::npos);
    
    // Check JSON is preserved
    EXPECT_TRUE(file_contents.find("JSON: {\"status\":\"ok\",\"code\":200}") != std::string::npos);
    
    // Check error handling for invalid formats
    EXPECT_TRUE(file_contents.find("<MISSING_ARG> <MISSING_ARG>") != std::string::npos);
    EXPECT_TRUE(file_contents.find("[FORMAT_ERROR: unmatched '}' in format string]") != std::string::npos);
    EXPECT_TRUE(file_contents.find("Invalid stray close brace: } 42") == std::string::npos);
}

// std::format supports positional / numbered placeholders ({0}, {1}, ...).
// Regression test: they must resolve to the right argument instead of being
// passed wholesale to std::vformat (which threw "Argument not found" and
// replaced the whole line with a FORMAT_ERROR).
TEST_F(SlickLoggerTest, NumberedPlaceholders) {
    std::filesystem::remove("test_numbered.log");

    slick::logger::Logger::instance().init("test_numbered.log", 1024);

    LOG_INFO("automatic: {} {}", 111, 222);
    LOG_INFO("numbered: {1} then {0}", 111, 222);
    LOG_INFO("indexed spec: {0:>5} {1:.1f}", 42, 3.5);
    LOG_INFO("out of range: {0} {9}", "only");
    LOG_INFO("repeated: {0} {0} {0}", 7);
    LOG_INFO("leading zeros: {01} {00}", 111, 222);

    // std::format rejects mixing automatic and explicit indices; this parser
    // tolerates it, with the two counters advancing independently, rather than
    // dropping the line. {} takes 111, {0} takes 111, {} takes 222.
    LOG_INFO("mixed: {} {0} {}", 111, 222);

    // An index long enough to overflow the uint32_t accumulator must still read
    // as out of range instead of wrapping back onto a valid argument.
    LOG_INFO("overflow: {4294967296} {99999999999999999999}", 111, 222);

    slick::logger::Logger::instance().shutdown();

    ASSERT_TRUE(std::filesystem::exists("test_numbered.log"));

    std::ifstream log_file("test_numbered.log");
    std::string line;
    std::getline(log_file, line);   // first line is the logger's version
    std::string file_contents;
    while (std::getline(log_file, line)) {
        file_contents += line + "\n";
    }

    EXPECT_TRUE(file_contents.find("automatic: 111 222") != std::string::npos);
    EXPECT_TRUE(file_contents.find("numbered: 222 then 111") != std::string::npos);
    EXPECT_TRUE(file_contents.find("indexed spec:    42 3.5") != std::string::npos);
    EXPECT_TRUE(file_contents.find("out of range: only <MISSING_ARG>") != std::string::npos);
    EXPECT_TRUE(file_contents.find("repeated: 7 7 7") != std::string::npos);
    EXPECT_TRUE(file_contents.find("leading zeros: 222 111") != std::string::npos);
    EXPECT_TRUE(file_contents.find("mixed: 111 111 222") != std::string::npos);
    EXPECT_TRUE(file_contents.find("overflow: <MISSING_ARG> <MISSING_ARG>") != std::string::npos);
}

// A format spec may itself contain replacement fields, for a dynamic width or
// precision ({0:{1}}, {:{}}, {:.{}f}). Regression test: the parser used to stop
// at the first '}', so "{0:{1}}" became the invalid single-argument spec
// "{:{1}" and every one of these forms replaced the whole message with a
// FORMAT_ERROR. Each expectation below was checked against std::format itself.
TEST_F(SlickLoggerTest, DynamicWidthAndPrecision) {
    std::filesystem::remove("test_dynamic_spec.log");

    slick::logger::Logger::instance().init("test_dynamic_spec.log", 1024);

    LOG_INFO("manual: [{0:{1}}]", 42, 8);
    LOG_INFO("automatic: [{:{}}]", 42, 8);
    LOG_INFO("precision: [{:.{}f}]", 3.14159, 3);
    LOG_INFO("both: [{:{}.{}f}]", 3.14159, 9, 2);
    LOG_INFO("fill: [{:*^{}}]", "ab", 7);
    LOG_INFO("flags: [{:#0{}x}]", 255, 8);
    LOG_INFO("reversed: [{1:{0}}]", 6, 42);

    // A nested field takes the argument *after* the one being formatted, so the
    // automatic counter has to stay in step across the rest of the line.
    LOG_INFO("interleaved: [{:{}}] {} [{:{}}]", 1, 4, "mid", 2, 5);

    // A zero width is valid and means "no minimum width". Writing the 0 into
    // the spec would make it the zero-padding flag instead, which pads a number
    // and is rejected outright for a text argument. A zero precision, by
    // contrast, is meaningful and has to survive.
    LOG_INFO("zero width text: [{:{}}]", "x", 0);
    LOG_INFO("zero width aligned: [{:*^{}}]", "x", 0);
    LOG_INFO("zero width flags: [{:#0{}x}]", 255, 0);
    LOG_INFO("zero width manual: [{0:{1}}]", "x", 0);
    LOG_INFO("zero width number: [{:{}}]", 42, 0);
    LOG_INFO("zero precision: [{:.{}}]", "abc", 0);
    LOG_INFO("zero both: [{:{}.{}f}]", 3.14159, 0, 0);

    // A nested field naming an argument that was never passed costs that field,
    // not the line - the same treatment a bare {} past the end already gets.
    LOG_INFO("missing width: [{:{}}] then {}", 42);
    LOG_INFO("missing manual: [{0:{9}}]", 42, 8);

    // A nested field that is never closed leaves the placeholder malformed, so
    // it is emitted verbatim rather than swallowing the line.
    LOG_INFO("unclosed: [{:{}]", 42, 8);

    slick::logger::Logger::instance().shutdown();

    ASSERT_TRUE(std::filesystem::exists("test_dynamic_spec.log"));

    std::ifstream log_file("test_dynamic_spec.log");
    std::string line;
    std::getline(log_file, line);   // first line is the logger's version
    std::string file_contents;
    while (std::getline(log_file, line)) {
        file_contents += line + "\n";
    }

    EXPECT_TRUE(file_contents.find("manual: [      42]") != std::string::npos);
    EXPECT_TRUE(file_contents.find("automatic: [      42]") != std::string::npos);
    EXPECT_TRUE(file_contents.find("precision: [3.142]") != std::string::npos);
    EXPECT_TRUE(file_contents.find("both: [     3.14]") != std::string::npos);
    EXPECT_TRUE(file_contents.find("fill: [**ab***]") != std::string::npos);
    EXPECT_TRUE(file_contents.find("flags: [0x0000ff]") != std::string::npos);
    EXPECT_TRUE(file_contents.find("reversed: [    42]") != std::string::npos);
    EXPECT_TRUE(file_contents.find("interleaved: [   1] mid [    2]") != std::string::npos);
    EXPECT_TRUE(file_contents.find("zero width text: [x]") != std::string::npos);
    EXPECT_TRUE(file_contents.find("zero width aligned: [x]") != std::string::npos);
    EXPECT_TRUE(file_contents.find("zero width flags: [0xff]") != std::string::npos);
    EXPECT_TRUE(file_contents.find("zero width manual: [x]") != std::string::npos);
    EXPECT_TRUE(file_contents.find("zero width number: [42]") != std::string::npos);
    EXPECT_TRUE(file_contents.find("zero precision: []") != std::string::npos);
    EXPECT_TRUE(file_contents.find("zero both: [3]") != std::string::npos);
    EXPECT_TRUE(file_contents.find("missing width: [<MISSING_ARG>] then <MISSING_ARG>") != std::string::npos);
    EXPECT_TRUE(file_contents.find("missing manual: [<MISSING_ARG>]") != std::string::npos);
    EXPECT_TRUE(file_contents.find("unclosed: [{:{}]") != std::string::npos);

    // Nothing here should have cost a line.
    EXPECT_TRUE(file_contents.find("FORMAT_ERROR") == std::string::npos);
}

// A nested width/precision field has to name an argument std::format could use
// as one. Anything else is a format error rather than a silently dropped spec.
TEST_F(SlickLoggerTest, DynamicWidthRejectsUnusableArgument) {
    std::filesystem::remove("test_dynamic_spec_errors.log");

    slick::logger::Logger::instance().init("test_dynamic_spec_errors.log", 1024);

    LOG_INFO("non-integer: [{:{}}]", 42, "wide");
    LOG_INFO("negative: [{:{}}]", 42, -3);

    slick::logger::Logger::instance().shutdown();

    ASSERT_TRUE(std::filesystem::exists("test_dynamic_spec_errors.log"));

    std::ifstream log_file("test_dynamic_spec_errors.log");
    std::string line;
    std::getline(log_file, line);   // first line is the logger's version
    std::string file_contents;
    while (std::getline(log_file, line)) {
        file_contents += line + "\n";
    }

    EXPECT_TRUE(file_contents.find(
        "[FORMAT_ERROR: dynamic width or precision must be an integer argument]") != std::string::npos);
    EXPECT_TRUE(file_contents.find(
        "[FORMAT_ERROR: dynamic width or precision must not be negative]") != std::string::npos);
}

// Regression test: wchar_t arguments render their value instead of <UNKNOWN>.
// Which *kind* of value is chosen by the format spec, exactly as std::format
// does for char - text by default, a number under b/B/d/o/x/X - so that a given
// spec does not change meaning with the code point.
TEST_F(SlickLoggerTest, WCharArgument) {
    std::filesystem::remove("test_wchar.log");

    slick::logger::Logger::instance().init("test_wchar.log", 1024);

    wchar_t wc = L'A';
    LOG_INFO("wchar: {}", wc);
    LOG_INFO("wchar code: {:d}", wc);
    LOG_INFO("wchar hex: {:x}", wc);
    LOG_INFO("wchar as char: {:c}", wc);

    // Above U+007F the default spec still yields a character (UTF-8 encoded),
    // not a number, and keeps the left alignment a character argument has.
    wchar_t accent = L'\u00e9';   // U+00E9, 2 UTF-8 bytes
    wchar_t cjk = L'\u4e2d';      // U+4E2D, 3 UTF-8 bytes
    LOG_INFO("wide chars: [{}] [{}]", accent, cjk);
    LOG_INFO("ascii padded: [{:4}]", wc);
    LOG_INFO("wide padded: [{:4}] [{:d}]", accent, accent);

    // An unpaired UTF-16 surrogate is not a character, so it falls back to the
    // numeric code point instead of emitting invalid UTF-8.
    wchar_t surrogate = static_cast<wchar_t>(0xD800);
    LOG_INFO("surrogate: {}", surrogate);

    slick::logger::Logger::instance().shutdown();

    ASSERT_TRUE(std::filesystem::exists("test_wchar.log"));

    std::ifstream log_file("test_wchar.log");
    std::string line;
    std::getline(log_file, line);   // first line is the logger's version
    std::string file_contents;
    while (std::getline(log_file, line)) {
        file_contents += line + "\n";
    }

    EXPECT_TRUE(file_contents.find("wchar: A") != std::string::npos);
    EXPECT_TRUE(file_contents.find("wchar code: 65") != std::string::npos);
    EXPECT_TRUE(file_contents.find("wchar hex: 41") != std::string::npos);
    EXPECT_TRUE(file_contents.find("wchar as char: A") != std::string::npos);
    EXPECT_TRUE(file_contents.find("wide chars: [\xc3\xa9] [\xe4\xb8\xad]") != std::string::npos);
    // Padding an ASCII code point is unambiguous - one column, so three spaces -
    // and it lands on the right, the way a character argument aligns rather than
    // the way a number does.
    EXPECT_TRUE(file_contents.find("ascii padded: [A   ]") != std::string::npos);

    // For a non-ASCII code point the *number* of pad columns is std::format's
    // estimated field width, which the standard leaves implementations room to
    // compute differently: libstdc++ 13 charges U+00E9 its two UTF-8 code units
    // where MSVC and libstdc++ 14 charge it one display column, so "{:4}" pads
    // it with two spaces on one and three on the other. That is not this
    // library's behavior to pin down - slick-logger
    // hands the spec and a UTF-8 string_view to std::vformat and does no padding
    // of its own. What is asserted here is what slick-logger does decide: the
    // argument arrives as UTF-8 text, padded on the right like a character, and
    // becomes a number under an integer presentation type.
    EXPECT_TRUE(file_contents.find("wide padded: [\xc3\xa9 ") != std::string::npos);
    EXPECT_TRUE(file_contents.find("] [233]") != std::string::npos);
    EXPECT_TRUE(file_contents.find("surrogate: 55296") != std::string::npos);
}

TEST_F(SlickLoggerTest, ConstCharArrayLogging) {
    std::filesystem::remove("test_char_array.log");
    
    slick::logger::Logger::instance().init("test_char_array.log", 1024);
    
    {
        const char* msg = "Const char array message";
        LOG_INFO("Message: {}", msg);
    }
    {
        std::string msg = "string message";
        LOG_INFO("Message: {}", msg);
    }
    {
        std::string msg = "Const char array string message";
        LOG_INFO("Message: {}", msg.c_str());
    }
    
    slick::logger::Logger::instance().shutdown();
    
    ASSERT_TRUE(std::filesystem::exists("test_char_array.log"));
    
    std::ifstream log_file("test_char_array.log");
    std::string file_contents;
    std::string line;
    std::getline(log_file, line);   // first line is the logger's version
    while (std::getline(log_file, line)) {
        file_contents += line + "\n";
    }
    
    // Check valid formats work
    EXPECT_TRUE(file_contents.find("Const char array message") != std::string::npos);
    EXPECT_TRUE(file_contents.find("string message") != std::string::npos);
    EXPECT_TRUE(file_contents.find("Const char array string message") != std::string::npos);
}

TEST_F(SlickLoggerTest, SingleStringLogging) {
    std::filesystem::remove("test_single_string.log");
    
    slick::logger::Logger::instance().init("test_single_string.log", 1024);
    
    LOG_INFO("string literal");
    {
        const char* msg = "Const char array message";
        LOG_INFO(msg);
    }
    {
        std::string msg = "string message";
        LOG_INFO(msg);
    }
    {
        std::string_view msg{"string_view message"};
        LOG_INFO(msg);
    }
    
    slick::logger::Logger::instance().shutdown();
    
    ASSERT_TRUE(std::filesystem::exists("test_single_string.log"));
    
    std::ifstream log_file("test_single_string.log");
    std::string file_contents;
    std::string line;
    std::getline(log_file, line);   // first line is the logger's version
    while (std::getline(log_file, line)) {
        file_contents += line + "\n";
    }
    
    // Check valid formats work
    EXPECT_TRUE(file_contents.find("string literal") != std::string::npos);
    EXPECT_TRUE(file_contents.find("Const char array message") != std::string::npos);
    EXPECT_TRUE(file_contents.find("string message") != std::string::npos);
    EXPECT_TRUE(file_contents.find("string_view message") != std::string::npos);
}

TEST_F(SlickLoggerTest, CharArrayLogging) {
    struct Msg {
        char msg_[32];
    };

    Msg msg;
    sprintf(msg.msg_, "test char array");

    std::filesystem::remove("test_char_array.log");
    
    slick::logger::Logger::instance().init("test_char_array.log", 1024);
    
    LOG_INFO("Log char array: {}", msg.msg_);
    
    slick::logger::Logger::instance().shutdown();
    
    ASSERT_TRUE(std::filesystem::exists("test_char_array.log"));
    
    std::ifstream log_file("test_char_array.log");
    std::string file_contents;
    std::string line;
    std::getline(log_file, line);   // first line is the logger's version
    while (std::getline(log_file, line)) {
        file_contents += line + "\n";
    }
    
    // Check valid formats work
    EXPECT_TRUE(file_contents.find("Log char array: test char array") != std::string::npos);
}

TEST_F(SlickLoggerTest, CopiesConstViewOfStructCharArrayArgument) {
    struct Msg {
        char msg_[32];
    };

    std::filesystem::remove("test_char_array_struct.log");

    slick::logger::Logger::instance().init("test_char_array_struct.log", 1024);

    Msg msg;
    std::strcpy(msg.msg_, "struct member text");
    const Msg& const_msg = msg;

    LOG_INFO("Log char array: {}", const_msg.msg_);

    // enqueue_argument copies the array into the string queue synchronously,
    // before LOG_INFO returns, so mutating the source afterwards must not
    // change what was logged.
    std::strcpy(msg.msg_, "mutated member text");

    slick::logger::Logger::instance().shutdown();

    ASSERT_TRUE(std::filesystem::exists("test_char_array_struct.log"));

    std::ifstream log_file("test_char_array_struct.log");
    std::string file_contents;
    std::string line;
    std::getline(log_file, line);   // first line is the logger's version
    while (std::getline(log_file, line)) {
        file_contents += line + "\n";
    }

    EXPECT_TRUE(file_contents.find("Log char array: struct member text") != std::string::npos);
    EXPECT_TRUE(file_contents.find("mutated member text") == std::string::npos);
}

TEST_F(SlickLoggerTest, LogsVolatileCharArrayArgument) {
    struct Msg {
        volatile char msg_[32];
    };

    std::filesystem::remove("test_volatile_char_array.log");

    slick::logger::Logger::instance().init("test_volatile_char_array.log", 1024);

    Msg msg;
    std::strcpy(const_cast<char*>(msg.msg_), "volatile member text");

    LOG_INFO("Log volatile char array: {}", msg.msg_);

    slick::logger::Logger::instance().shutdown();

    ASSERT_TRUE(std::filesystem::exists("test_volatile_char_array.log"));

    std::ifstream log_file("test_volatile_char_array.log");
    std::string file_contents;
    std::string line;
    std::getline(log_file, line);   // first line is the logger's version
    while (std::getline(log_file, line)) {
        file_contents += line + "\n";
    }

    EXPECT_TRUE(file_contents.find("Log volatile char array: volatile member text") != std::string::npos);
}

TEST_F(SlickLoggerTest, LogsConstAndVolatileQualifiedPointerArguments) {
    std::filesystem::remove("test_cv_pointer.log");

    slick::logger::Logger::instance().init("test_cv_pointer.log", 1024);

    int value = 42;
    const int* const_ptr = &value;
    volatile int* volatile_ptr = &value;
    const volatile int* const_volatile_ptr = &value;
    void* addr = const_cast<void*>(static_cast<const volatile void*>(&value));

    LOG_INFO("const={:p} volatile={:p} const_volatile={:p}", const_ptr, volatile_ptr, const_volatile_ptr);

    slick::logger::Logger::instance().shutdown();

    ASSERT_TRUE(std::filesystem::exists("test_cv_pointer.log"));

    std::ifstream log_file("test_cv_pointer.log");
    std::string file_contents;
    std::string line;
    std::getline(log_file, line);   // first line is the logger's version
    while (std::getline(log_file, line)) {
        file_contents += line + "\n";
    }

    // All three pointers alias the same address, so the logger's internal
    // normalization to a plain void* must format all three identically.
    const std::string expected = std::format(
        "const={:p} volatile={:p} const_volatile={:p}", addr, addr, addr);
    EXPECT_TRUE(file_contents.find(expected) != std::string::npos);
}

TEST_F(SlickLoggerTest, EmptyStringView) {
    std::filesystem::remove("test_empty_string.log");
    
    slick::logger::Logger::instance().init("test_empty_string.log", 8);

    std::string_view s;
    
    LOG_INFO("Log empty string: {}", s);
    
    slick::logger::Logger::instance().shutdown();
    
    ASSERT_TRUE(std::filesystem::exists("test_empty_string.log"));
    
    std::ifstream log_file("test_empty_string.log");
    std::string line;
    std::getline(log_file, line);   // first line is the logger's version
    EXPECT_TRUE(std::getline(log_file, line));
    EXPECT_NE(line.find(" [INFO] "), std::string::npos);
    EXPECT_NE(line.find("Log empty string: "), std::string::npos);
}

TEST_F(SlickLoggerTest, FormatArgsLogging) {
    std::filesystem::remove("test_format_args.log");

    slick::logger::Logger::instance().init("test_format_args.log", 1024);

    int i = 42;
    double d = 3.14;
    std::string_view sv = "hello";
    bool b = true;
    void* p = &d;

    // Pre-build format_args and pass to logger
    LOG_INFO("int={} double={:.2f} str={} bool={} pointer={:p}", std::make_format_args(i, d, sv, b, p));

    slick::logger::Logger::instance().shutdown();

    ASSERT_TRUE(std::filesystem::exists("test_format_args.log"));

    std::ifstream log_file("test_format_args.log");
    std::string file_contents;
    std::string line;
    std::getline(log_file, line); // first line is the logger's version
    while (std::getline(log_file, line)) {
        file_contents += line + "\n";
    }

    EXPECT_NE(file_contents.find("int=42 double=3.14 str=hello bool=true"), std::string::npos);
}

TEST_F(SlickLoggerTest, FormatArgsConstCharBridgeLogging) {
    std::filesystem::remove("test_format_args_const_char.log");

    slick::logger::Logger::instance().init("test_format_args_const_char.log", 1024);

    auto log_bridge = [](slick::logger::LogLevel level, const char* format_text, std::format_args args) {
        slick::logger::Logger::instance().log(level, format_text, args);
    };

    int number = 7;
    std::string_view text = "bridge";
    log_bridge(slick::logger::LogLevel::L_INFO, "bridge value={} text={}", std::make_format_args(number, text));

    slick::logger::Logger::instance().shutdown();

    ASSERT_TRUE(std::filesystem::exists("test_format_args_const_char.log"));

    std::ifstream log_file("test_format_args_const_char.log");
    std::string file_contents;
    std::string line;
    std::getline(log_file, line); // first line is the logger's version
    while (std::getline(log_file, line)) {
        file_contents += line + "\n";
    }

    EXPECT_NE(file_contents.find("bridge value=7 text=bridge"), std::string::npos);
}

// set_instance() makes Logger::instance() return the supplied logger.
// clear_instance_override() restores the library-local logger.
TEST_F(SlickLoggerTest, SetInstanceChangesActiveLogger) {
    // Capture the default (library-local) logger pointer before any redirect.
    slick::logger::Logger* local = &slick::logger::Logger::instance();

    // Redirect to itself — pointer must still equal local.
    slick::logger::Logger::set_instance(local);
    EXPECT_EQ(&slick::logger::Logger::instance(), local);

    // Redirect to nullptr — clear_instance_override restores local.
    slick::logger::Logger::clear_instance_override();
    EXPECT_EQ(&slick::logger::Logger::instance(), local);
}

// Simulates what a plugin/shared library's init function does:
// redirect LOG_* calls to the host logger so messages appear in the host's
// log file, not in a separate plugin-local log.
//
// In production the plugin and host are separate binaries, each with their
// own Logger::instance_. Here we model it in one process: the "host" logger
// is Logger::instance() (writing to host.log); the "plugin" temporarily adds
// its own sink (plugin.log), then calls set_instance() to point away from it.
TEST_F(SlickLoggerTest, SetInstanceRedirectsLogsToHostLogger) {
    std::filesystem::remove("test_set_instance_host.log");
    std::filesystem::remove("test_set_instance_plugin.log");

    // --- Host setup: Logger::instance() writes to host.log ---
    slick::logger::Logger::instance().add_file_sink("test_set_instance_host.log", "host_sink");
    slick::logger::Logger::instance().init(1024);

    // --- Plugin setup: same instance gets a second sink (plugin.log) ---
    // In a real scenario this would be the plugin's own Logger::instance_,
    // but since we can't construct a Logger directly, we reuse the same object
    // and differentiate by adding a dedicated plugin sink.
    auto plugin_sink = std::make_shared<slick::logger::FileSink>("test_set_instance_plugin.log", "plugin_sink");
    plugin_sink->set_dedicated(true); // dedicated: only receives direct writes, not LOG_* broadcasts
    slick::logger::Logger::instance().add_sink(plugin_sink);

    // --- Simulate plugin redirect: set_instance points to the host logger ---
    // After this, LOG_* goes to host.log. plugin.log only receives direct writes.
    slick::logger::Logger::set_instance(&slick::logger::Logger::instance());

    LOG_INFO("plugin message one");
    LOG_WARN("plugin message two");

    // Write directly to plugin sink to confirm it is still alive (dedicated path).
    plugin_sink->log_info("direct to plugin sink");

    slick::logger::Logger::clear_instance_override();
    slick::logger::Logger::instance().shutdown();

    // host.log must contain the two LOG_* messages.
    ASSERT_TRUE(std::filesystem::exists("test_set_instance_host.log"));
    std::ifstream host_file("test_set_instance_host.log");
    std::string host_contents, line;
    std::getline(host_file, line); // version line
    while (std::getline(host_file, line))
        host_contents += line + "\n";

    EXPECT_NE(host_contents.find("plugin message one"), std::string::npos);
    EXPECT_NE(host_contents.find("plugin message two"), std::string::npos);

    // plugin.log must contain only the directly-written message.
    ASSERT_TRUE(std::filesystem::exists("test_set_instance_plugin.log"));
    std::ifstream plugin_file("test_set_instance_plugin.log");
    std::string plugin_contents;
    while (std::getline(plugin_file, line))
        plugin_contents += line + "\n";

    EXPECT_NE(plugin_contents.find("direct to plugin sink"), std::string::npos);
    // Broadcast LOG_* messages must NOT appear in plugin.log (dedicated sink ignores them).
    EXPECT_EQ(plugin_contents.find("plugin message one"), std::string::npos);
    EXPECT_EQ(plugin_contents.find("plugin message two"), std::string::npos);
}

// After clear_instance_override(), LOG_* calls return to the library-local
// logger. Messages logged before and after the redirect go to different sinks.
TEST_F(SlickLoggerTest, ClearInstanceOverrideRestoresLocalLogger) {
    std::filesystem::remove("test_set_instance_host.log");
    std::filesystem::remove("test_set_instance_plugin.log");

    // "Host" file sink added to the local instance.
    slick::logger::Logger::instance().add_file_sink("test_set_instance_host.log", "host_sink");

    // "Plugin-local" dedicated sink — receives only direct writes, not LOG_*.
    auto plugin_sink = std::make_shared<slick::logger::FileSink>("test_set_instance_plugin.log", "plugin_sink");
    plugin_sink->set_dedicated(true);
    slick::logger::Logger::instance().add_sink(plugin_sink);

    slick::logger::Logger::instance().init(1024);

    // Redirect to the same logger (host), log one message, then restore.
    slick::logger::Logger::set_instance(&slick::logger::Logger::instance());
    LOG_INFO("while redirected");
    slick::logger::Logger::clear_instance_override();
    LOG_INFO("after restore");

    slick::logger::Logger::instance().shutdown();

    // host.log must contain both messages (both went through the same instance).
    std::ifstream host_file("test_set_instance_host.log");
    std::string host_contents, line;
    std::getline(host_file, line); // version line
    while (std::getline(host_file, line))
        host_contents += line + "\n";

    EXPECT_NE(host_contents.find("while redirected"), std::string::npos);
    EXPECT_NE(host_contents.find("after restore"),    std::string::npos);

    // plugin.log must be empty (dedicated sink never received LOG_* broadcasts).
    std::ifstream plugin_file("test_set_instance_plugin.log");
    std::string plugin_contents;
    while (std::getline(plugin_file, line))
        plugin_contents += line + "\n";

    EXPECT_EQ(plugin_contents.find("while redirected"), std::string::npos);
    EXPECT_EQ(plugin_contents.find("after restore"),    std::string::npos);
}

// Simulates two plugin threads both routing LOG_* through the same host logger
// concurrently after set_instance().
TEST_F(SlickLoggerTest, MultipleProducersAfterSetInstance) {
    std::filesystem::remove("test_set_instance_host.log");

    slick::logger::Logger::instance().add_file_sink("test_set_instance_host.log");
    slick::logger::Logger::instance().init(1024);

    slick::logger::Logger::set_instance(&slick::logger::Logger::instance());

    std::thread t1([]() {
        for (int i = 0; i < 5; ++i)
            LOG_INFO("plugin-thread-1: {}", i);
    });
    std::thread t2([]() {
        for (int i = 0; i < 5; ++i)
            LOG_INFO("plugin-thread-2: {}", i);
    });

    t1.join();
    t2.join();

    slick::logger::Logger::clear_instance_override();
    slick::logger::Logger::instance().shutdown();

    // All 10 messages plus the version line must be present.
    ASSERT_TRUE(std::filesystem::exists("test_set_instance_host.log"));
    std::ifstream host_file("test_set_instance_host.log");
    std::string line;
    int count = 0;
    while (std::getline(host_file, line))
        ++count;

    EXPECT_EQ(count, 11); // 10 messages + 1 version line
}


// Sinks are no longer flushed after every batch; the writer thread flushes when
// it has drained the queue, and on request. These tests pin the guarantees that
// callers can actually observe.

TEST_F(SlickLoggerTest, FlushMakesEntriesVisibleOnDisk) {
    std::filesystem::remove("test_flush_visible.log");

    auto& logger = slick::logger::Logger::instance();
    logger.reset();
    logger.add_file_sink("test_flush_visible.log");
    logger.init(1024);

    const int kMessages = 500;
    for (int i = 0; i < kMessages; ++i) {
        LOG_INFO("flush visibility message {}", i);
    }

    logger.flush();

    // Without shutting the logger down, every entry must already be readable.
    int count = 0;
    {
        std::ifstream file("test_flush_visible.log");
        ASSERT_TRUE(file.is_open());
        std::string line;
        while (std::getline(file, line)) {
            if (line.find("flush visibility message") != std::string::npos) {
                ++count;
            }
        }
    }
    EXPECT_EQ(count, kMessages);

    logger.shutdown();
    std::filesystem::remove("test_flush_visible.log");
}

TEST_F(SlickLoggerTest, IdleWriterFlushesWithoutExplicitFlush) {
    std::filesystem::remove("test_idle_flush.log");

    auto& logger = slick::logger::Logger::instance();
    logger.reset();
    logger.add_file_sink("test_idle_flush.log");
    logger.init(1024);

    const int kMessages = 200;
    for (int i = 0; i < kMessages; ++i) {
        LOG_INFO("idle flush message {}", i);
    }

    // No flush() and no shutdown(): once the writer thread drains the queue it
    // flushes on its own, so the entries appear without the caller asking.
    int count = 0;
    for (int attempt = 0; attempt < 200 && count < kMessages; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        count = 0;
        std::ifstream file("test_idle_flush.log");
        std::string line;
        while (std::getline(file, line)) {
            if (line.find("idle flush message") != std::string::npos) {
                ++count;
            }
        }
    }
    EXPECT_EQ(count, kMessages);

    logger.shutdown();
    std::filesystem::remove("test_idle_flush.log");
}

// Counts entries as the writer thread hands them over, with no I/O of its own.
class CountingSink : public slick::logger::ISink {
public:
    CountingSink() : ISink("counting") {}
    void write(const slick::logger::LogEntry&) override {
        writes_.fetch_add(1, std::memory_order_release);
    }
    void flush() override {}
    std::atomic<int> writes_{0};
};

TEST_F(SlickLoggerTest, ParkedWriterIsAlwaysWokenByANewEntry) {
    // The idle writer thread parks on an atomic, and atomic::wait has no
    // timeout: a lost wake-up strands an entry indefinitely rather than merely
    // delaying it. The riskiest moment is an entry published in the window
    // between the writer deciding to park and actually parking, so the gap
    // before each entry is swept across that boundary rather than fixed.
    //
    // Sleeping cannot produce those gaps - Windows rounds any sleep request up
    // to ~15.6ms - so the gap is busy-waited.
    auto spin_for = [](double microseconds) {
        const auto start = std::chrono::steady_clock::now();
        while (std::chrono::duration<double, std::micro>(
                   std::chrono::steady_clock::now() - start).count() < microseconds) {
            std::this_thread::yield();
        }
    };

    auto& logger = slick::logger::Logger::instance();
    logger.reset();
    auto sink = std::make_shared<CountingSink>();
    logger.add_sink(sink);
    logger.init(4096);

    logger.flush();
    const int baseline = sink->writes_.load(std::memory_order_acquire);

    const int kRounds = 1500;
    for (int i = 0; i < kRounds; ++i) {
        // Sweeps 0..299us, which brackets the writer's idle spin, so some
        // iterations land inside the park window. Every 250th round idles long
        // enough to park deeply as well.
        if (i % 250 == 249) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        } else {
            spin_for(static_cast<double>(i % 300));
        }

        LOG_INFO("park probe {}", i);

        // A delayed entry is fine; a stranded one is not. Checking before the
        // next entry is logged means nothing else can wake the writer for us.
        const int expected = baseline + i + 1;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (sink->writes_.load(std::memory_order_acquire) < expected &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }
        ASSERT_GE(sink->writes_.load(std::memory_order_acquire), expected)
            << "writer thread was not woken for entry " << i;
    }

    logger.shutdown();
}

TEST_F(SlickLoggerTest, ShutdownAndFlushCompleteWhileWriterIsParked) {
    // Both paths have to release a parked writer explicitly; without that the
    // join or the flush would block until something else happened to be logged.
    auto& logger = slick::logger::Logger::instance();
    logger.reset();
    auto sink = std::make_shared<CountingSink>();
    logger.add_sink(sink);
    logger.init(4096);

    LOG_INFO("before the writer parks");
    std::this_thread::sleep_for(std::chrono::milliseconds(50)); // let it park

    auto started = std::chrono::steady_clock::now();
    logger.flush(); // must not wait on another entry arriving
    auto flush_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started).count();
    EXPECT_LT(flush_ms, 1000) << "flush() did not wake the parked writer";

    std::this_thread::sleep_for(std::chrono::milliseconds(50)); // let it park again

    started = std::chrono::steady_clock::now();
    logger.shutdown();
    auto shutdown_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started).count();
    EXPECT_LT(shutdown_ms, 1000) << "shutdown() did not wake the parked writer";
}

// A sink that is slow to write, so the writer thread is still mid-batch when a
// caller asks to flush. The queue cursor advances when a batch is claimed rather
// than when it is written, so this is what distinguishes "the writer took the
// entries" from "the entries reached the sink and were flushed".
class SlowCountingSink : public slick::logger::ISink {
public:
    SlowCountingSink() : ISink("slow_counting") {}

    void write(const slick::logger::LogEntry&) override {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        writes_.fetch_add(1, std::memory_order_relaxed);
    }

    void flush() override {
        writes_at_last_flush_.store(writes_.load(std::memory_order_relaxed),
                                    std::memory_order_relaxed);
    }

    std::atomic<int> writes_{0};
    std::atomic<int> writes_at_last_flush_{-1};
};

TEST_F(SlickLoggerTest, FlushWaitsForSinksNotJustTheQueueCursor) {
    auto& logger = slick::logger::Logger::instance();
    logger.reset();
    auto sink = std::make_shared<SlowCountingSink>();
    logger.add_sink(sink);
    logger.init(4096);

    // Each write takes far longer than flush()'s polling interval, so "the queue
    // cursor reached the target" and "the sink finished and was flushed" are
    // separated by a wide, reliable margin.
    const int kMessages = 30;
    for (int i = 0; i < kMessages; ++i) {
        LOG_INFO("slow sink message {}", i);
    }

    logger.flush();

    // flush() must not return until the sink has actually written every entry
    // and been flushed afterwards. The sink also sees the version line logged by
    // init(), so the counts are lower bounds.
    EXPECT_GE(sink->writes_.load(), kMessages);
    EXPECT_GE(sink->writes_at_last_flush_.load(), kMessages);

    logger.shutdown();
}

TEST_F(SlickLoggerTest, FlushStillWorksAcrossResetCycles) {
    // reset() clears the flush generation counters, so exercise flush() over
    // repeated init()/reset() cycles rather than only the first one. The slow
    // sink keeps the assertions meaningful: a flush() that returned before the
    // sink was done could not report every entry as written and flushed.
    auto& logger = slick::logger::Logger::instance();

    for (int cycle = 0; cycle < 3; ++cycle) {
        logger.reset();
        auto sink = std::make_shared<SlowCountingSink>();
        logger.add_sink(sink);
        logger.init(4096);

        const int kMessages = 20;
        for (int i = 0; i < kMessages; ++i) {
            LOG_INFO("reset cycle {} message {}", cycle, i);
        }

        logger.flush();

        EXPECT_GE(sink->writes_.load(), kMessages)
            << "flush() returned before the sink had the entries, cycle " << cycle;
        EXPECT_GE(sink->writes_at_last_flush_.load(), kMessages)
            << "flush() returned without the sink being flushed, cycle " << cycle;

        logger.shutdown();
    }
}

TEST_F(SlickLoggerTest, RepeatedAndConcurrentFlushesAllComplete) {
    std::filesystem::remove("test_flush_repeat.log");

    auto& logger = slick::logger::Logger::instance();
    logger.reset();
    logger.add_file_sink("test_flush_repeat.log");
    logger.init(1024);

    // Interleaved logging and flushing from several threads must not hang: each
    // flush waits for its own generation to be acknowledged.
    std::atomic<int> written{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&logger, &written]() {
            for (int i = 0; i < 50; ++i) {
                LOG_INFO("concurrent flush message {}", i);
                ++written;
                logger.flush();
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }

    logger.flush();

    int count = 0;
    {
        std::ifstream file("test_flush_repeat.log");
        ASSERT_TRUE(file.is_open());
        std::string line;
        while (std::getline(file, line)) {
            if (line.find("concurrent flush message") != std::string::npos) {
                ++count;
            }
        }
    }
    EXPECT_EQ(count, written.load());

    logger.shutdown();
    std::filesystem::remove("test_flush_repeat.log");
}

// ---------------------------------------------------------------------------
// LOG_SINK_* macros
// ---------------------------------------------------------------------------

TEST_F(SlickLoggerTest, SinkMacrosDoNotEvaluateArgumentsWhenSinkLevelDisabled) {
    slick::logger::Logger::instance().clear_sinks();
    slick::logger::Logger::instance().add_file_sink("test_sink_macro.log", "macro_sink");
    slick::logger::Logger::instance().init(1024);

    auto sink = slick::logger::Logger::instance().get_sink("macro_sink");
    ASSERT_TRUE(sink != nullptr);
    sink->set_min_level(slick::logger::LogLevel::L_WARN);

    int evaluation_count = 0;
    auto expensive = [&]() -> std::string {
        ++evaluation_count;
        return "payload";
    };

    LOG_SINK_TRACE(sink, "trace {}", expensive());
    LOG_SINK_DEBUG(sink, "debug {}", expensive());
    LOG_SINK_INFO(sink, "info {}", expensive());
    LOG_SINK_WARN(sink, "warn {}", expensive());

    slick::logger::Logger::instance().shutdown();

    EXPECT_EQ(evaluation_count, 1);

    std::ifstream log_file("test_sink_macro.log");
    std::string content((std::istreambuf_iterator<char>(log_file)),
                        std::istreambuf_iterator<char>());
    EXPECT_NE(content.find("warn payload"), std::string::npos);
    EXPECT_EQ(content.find("info payload"), std::string::npos);
}

TEST_F(SlickLoggerTest, SinkMacrosDoNotEvaluateArgumentsWhenGlobalLevelDisabled) {
    slick::logger::Logger::instance().clear_sinks();
    slick::logger::Logger::instance().add_file_sink("test_sink_macro.log", "macro_sink");
    slick::logger::Logger::instance().init(1024);
    slick::logger::Logger::instance().set_level(slick::logger::LogLevel::L_ERROR);

    auto sink = slick::logger::Logger::instance().get_sink("macro_sink");
    ASSERT_TRUE(sink != nullptr);
    // The sink itself accepts everything; only the global level filters here.
    sink->set_min_level(slick::logger::LogLevel::L_TRACE);

    int evaluation_count = 0;
    auto expensive = [&]() -> std::string {
        ++evaluation_count;
        return "payload";
    };

    LOG_SINK_INFO(sink, "info {}", expensive());
    LOG_SINK_WARN(sink, "warn {}", expensive());
    LOG_SINK_ERROR(sink, "error {}", expensive());

    slick::logger::Logger::instance().shutdown();

    EXPECT_EQ(evaluation_count, 1);
}

TEST_F(SlickLoggerTest, SinkMacrosCaptureCallSiteSourceLocation) {
    slick::logger::Logger::instance().clear_sinks();
    slick::logger::Logger::instance().add_file_sink("test_sink_macro.log", "macro_sink");
    slick::logger::Logger::instance().init(1024);

    auto sink = slick::logger::Logger::instance().get_sink("macro_sink");
    ASSERT_TRUE(sink != nullptr);

    const int expected_line = __LINE__ + 1;
    LOG_SINK_INFO(sink, "Sink macro source location");

    slick::logger::Logger::instance().shutdown();

    std::ifstream log_file("test_sink_macro.log");
    std::string content((std::istreambuf_iterator<char>(log_file)),
                        std::istreambuf_iterator<char>());
    EXPECT_NE(content.find("test_logger.cpp:" + std::to_string(expected_line)), std::string::npos);
    EXPECT_NE(content.find("Sink macro source location"), std::string::npos);
}

TEST_F(SlickLoggerTest, SinkMacrosAcceptRefPointerAndSharedPtr) {
    slick::logger::Logger::instance().clear_sinks();
    slick::logger::Logger::instance().add_file_sink("test_sink_macro.log", "macro_sink");
    slick::logger::Logger::instance().init(1024);

    auto shared = slick::logger::Logger::instance().get_sink("macro_sink");
    ASSERT_TRUE(shared != nullptr);
    slick::logger::ISink* raw = shared.get();
    slick::logger::ISink& ref = *shared;

    LOG_SINK_INFO(shared, "via shared_ptr");
    LOG_SINK_INFO(raw, "via raw pointer");
    LOG_SINK_INFO(ref, "via reference");

    // An empty smart pointer must be a no-op, not a crash.
    std::shared_ptr<slick::logger::ISink> empty;
    LOG_SINK_INFO(empty, "must not be logged");

    slick::logger::Logger::instance().shutdown();

    std::ifstream log_file("test_sink_macro.log");
    std::string content((std::istreambuf_iterator<char>(log_file)),
                        std::istreambuf_iterator<char>());
    EXPECT_NE(content.find("via shared_ptr"), std::string::npos);
    EXPECT_NE(content.find("via raw pointer"), std::string::npos);
    EXPECT_NE(content.find("via reference"), std::string::npos);
    EXPECT_EQ(content.find("must not be logged"), std::string::npos);
}

TEST_F(SlickLoggerTest, SinkMacrosEvaluateSinkExpressionOnce) {
    slick::logger::Logger::instance().clear_sinks();
    slick::logger::Logger::instance().add_file_sink("test_sink_macro.log", "macro_sink");
    slick::logger::Logger::instance().init(1024);

    auto shared = slick::logger::Logger::instance().get_sink("macro_sink");
    ASSERT_TRUE(shared != nullptr);

    int lookups = 0;
    auto fetch = [&]() -> std::shared_ptr<slick::logger::ISink> {
        ++lookups;
        return shared;
    };

    LOG_SINK_INFO(fetch(), "single evaluation");

    slick::logger::Logger::instance().shutdown();

    EXPECT_EQ(lookups, 1);
}

TEST_F(SlickLoggerTest, SinkMacroOnUnregisteredSinkDoesNotBroadcast) {
    // An unregistered sink has index -1, which downstream means "every sink".
    // should_log() must reject it rather than let it broadcast.
    slick::logger::Logger::instance().clear_sinks();
    slick::logger::Logger::instance().add_file_sink("test_sink_macro.log", "macro_sink");
    slick::logger::Logger::instance().init(1024);

    auto orphan = std::make_shared<slick::logger::FileSink>("test_sink_orphan.log");
    ASSERT_EQ(orphan->index(), -1);

    LOG_SINK_ERROR(orphan, "must not broadcast");
    orphan->log_error("must not broadcast");  // the direct helper must refuse too

    slick::logger::Logger::instance().shutdown();

    std::ifstream log_file("test_sink_macro.log");
    std::string content((std::istreambuf_iterator<char>(log_file)),
                        std::istreambuf_iterator<char>());
    EXPECT_EQ(content.find("must not broadcast"), std::string::npos);
}

TEST_F(SlickLoggerTest, ClearSinksDetachesHandlesFromReusedSlots) {
    // A handle kept across clear_sinks() must not address the slot a later
    // add_sink() reuses - both sinks would otherwise sit at index 0.
    slick::logger::Logger::instance().clear_sinks();
    slick::logger::Logger::instance().add_file_sink("test_sink_orphan.log", "old");

    auto stale = slick::logger::Logger::instance().get_sink("old");
    ASSERT_TRUE(stale != nullptr);
    ASSERT_EQ(stale->index(), 0);

    slick::logger::Logger::instance().clear_sinks();
    EXPECT_EQ(stale->index(), -1);

    slick::logger::Logger::instance().add_file_sink("test_sink_macro.log", "replacement");
    slick::logger::Logger::instance().init(1024);

    LOG_SINK_ERROR(stale, "must not reach the replacement");
    stale->log_error("must not reach the replacement either");

    slick::logger::Logger::instance().shutdown();

    std::ifstream log_file("test_sink_macro.log");
    std::string content((std::istreambuf_iterator<char>(log_file)),
                        std::istreambuf_iterator<char>());
    EXPECT_EQ(content.find("must not reach the replacement"), std::string::npos);
}

TEST_F(SlickLoggerTest, ShutdownDetachesHandlesFromReusedSlots) {
    // Same hazard through shutdown(), which clears sinks by default.
    slick::logger::Logger::instance().clear_sinks();
    slick::logger::Logger::instance().add_file_sink("test_sink_orphan.log", "old");
    slick::logger::Logger::instance().init(1024);

    auto stale = slick::logger::Logger::instance().get_sink("old");
    ASSERT_TRUE(stale != nullptr);
    ASSERT_EQ(stale->index(), 0);

    slick::logger::Logger::instance().shutdown();
    EXPECT_EQ(stale->index(), -1);

    slick::logger::Logger::instance().add_file_sink("test_sink_macro.log", "replacement");
    slick::logger::Logger::instance().init(1024);

    LOG_SINK_ERROR(stale, "must not reach the replacement");

    slick::logger::Logger::instance().shutdown();

    std::ifstream log_file("test_sink_macro.log");
    std::string content((std::istreambuf_iterator<char>(log_file)),
                        std::istreambuf_iterator<char>());
    EXPECT_EQ(content.find("must not reach the replacement"), std::string::npos);
}

TEST_F(SlickLoggerTest, DirectSinkLogRespectsSinkMinLevel) {
    // sink->log_*() cannot skip evaluating its arguments, but it must still skip
    // the enqueue when the entry is below the sink's own level.
    slick::logger::Logger::instance().clear_sinks();
    slick::logger::Logger::instance().add_file_sink("test_sink_macro.log", "macro_sink");
    slick::logger::Logger::instance().init(1024);

    auto sink = slick::logger::Logger::instance().get_sink("macro_sink");
    ASSERT_TRUE(sink != nullptr);
    sink->set_min_level(slick::logger::LogLevel::L_ERROR);

    sink->log_info("below threshold");
    sink->log_error("above threshold");

    slick::logger::Logger::instance().shutdown();

    std::ifstream log_file("test_sink_macro.log");
    std::string content((std::istreambuf_iterator<char>(log_file)),
                        std::istreambuf_iterator<char>());
    EXPECT_EQ(content.find("below threshold"), std::string::npos);
    EXPECT_NE(content.find("above threshold"), std::string::npos);
}

int main(int argc, char **argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
