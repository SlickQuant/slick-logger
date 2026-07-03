#include <slick/logger.hpp>
#include <thread>
#include <chrono>

int main() {
    // Initialize the logger (traditional way - backwards compatible)
    slick::logger::Logger::instance().init("example.log", 1024);
    
    // Alternative: Add console sink for dual output (must be after init)
    slick::logger::Logger::instance().add_console_sink(true, true);

    // Log some messages with formatting
    LOG_INFO("Logger initialized");
    LOG_DEBUG("Debug message: value = {}", 42);
    LOG_WARN("Warning: {} items processed", 150);
    LOG_ERROR("Error occurred in {} at line {}", "function_name", 123);

    // Log a JSON-like structured message
    LOG_INFO("[{\"T\":\"success\",\"msg\":\"connected\"}]");

    // Demonstrate variadic arguments
    std::string user = "Alice";
    int age = 30;
    double balance = 1234.56;
    LOG_INFO("User {} is {} years old with balance ${:.2f}", user, age, balance);
    LOG_DEBUG("User {} has {} new messages", user, "5");

    // Simulate multi-threaded logging
    std::thread t1([]() {
        for (int i = 0; i < 10; ++i) {
            LOG_INFO("Thread 1: iteration {} of {}", i + 1, 10);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    });

    std::thread t2([]() {
        for (int i = 0; i < 10; ++i) {
            LOG_INFO("Thread 2: processing item {} at {}", i, std::chrono::system_clock::now());
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    });

    t1.join();
    t2.join();

    LOG_INFO("Logging complete - all messages were formatted in background thread");
    LOG_INFO("Messages appear both in example.log and on console with colors!");

    // Shutdown the logger
    slick::logger::Logger::instance().shutdown();

    return 0;
}
