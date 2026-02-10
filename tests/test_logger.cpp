#include <slick/logger.hpp>
#include <gtest/gtest.h>
#include <thread>
#include <filesystem>
#include <fstream>
#include <string>

class SlickLoggerTest : public ::testing::Test {
protected:
    void TearDown() override {
        // Clean up all test log files
        std::filesystem::remove("test.log");
        std::filesystem::remove("test_mt.log");
        std::filesystem::remove("test_json.log");
        std::filesystem::remove("test_format_error.log");
        std::filesystem::remove("test_no_args.log");
        std::filesystem::remove("test_mixed.log");
        std::filesystem::remove("test_char_array.log");
        std::filesystem::remove("test_single_string.log");
        std::filesystem::remove("test_empty_string.log");
        std::filesystem::remove("test_format_args.log");
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
    
    // Check JSON is preserved
    EXPECT_TRUE(file_contents.find("JSON: {\"status\":\"ok\",\"code\":200}") != std::string::npos);
    
    // Check error handling for invalid formats
    EXPECT_TRUE(file_contents.find("<MISSING_ARG> <MISSING_ARG>") != std::string::npos);
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
    // Check valid formats work
    EXPECT_EQ(line.size(), 52);
    EXPECT_EQ(line.find(" [INFO] Log empty string: "), 26);
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

    log_file.close();
    std::filesystem::remove("test_format_args.log");
}

int main(int argc, char **argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
