# slick-logger

[![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)](https://en.cppreference.com/w/cpp/20)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![Header-only](https://img.shields.io/badge/header--only-yes-brightgreen.svg)](#installation)
[![Lock-free](https://img.shields.io/badge/concurrency-lock--free-orange.svg)](#architecture)
[![CI](https://github.com/SlickQuant/slick-logger/actions/workflows/ci.yml/badge.svg)](https://github.com/SlickQuant/slick-logger/actions/workflows/ci.yml)
[![GitHub release](https://img.shields.io/github/v/release/SlickQuant/slick-logger)](https://github.com/SlickQuant/slick-logger/releases)

A high-performance, cross-platform **header-only** logging library for C++20 using a multi-producer, multi-consumer ring buffer with **multi-sink support** and **log rotation** capabilities.

## Features

- **High Performance**: Asynchronous logging using slick_queue ring buffer for minimal latency
- **Modern Formatting**: Uses C++20 `std::format` for type-safe, efficient string formatting
- **Multi-Sink Architecture**: Log to multiple destinations simultaneously (console, files, custom sinks)
- **Log Rotation**: Size-based and time-based rotation with configurable retention
- **Colored Console Output**: ANSI color support with configurable error routing
- **Header-Only**: No linking required - just include and use
- **Cross-Platform**: Supports Windows, Linux, and macOS
- **Multi-Threaded**: Safe for concurrent logging from multiple threads
- **C++20**: Utilizes modern C++ features
- **Easy to Use**: Simple macros for logging at different levels

## Requirements

- **C++20 compatible compiler** with `std::format` support (GCC 11+, Clang 14+, MSVC 19.29+)
- CMake 3.20 or higher (for building examples/tests)
- Internet connection for downloading slick_queue header

## Installation

### Option 1: Direct Copy
For manual installation, you need both slick-logger and its dependency:

1. Copy the `include/slick/` directory to your project
2. Download `queue.h` from https://raw.githubusercontent.com/SlickQuant/slick-queue/main/include/slick/queue.h
3. Place `queue.h` in your include path or alongside the slick-logger headers

Your project structure should look like:
```
your_project/
├── include/
│   ├── slick/
│       └── logger.hpp
│       └── queue.h
└── src/
    └── main.cpp
```

### Option 2: CMake Integration (Recommended)

CMake automatically handles the slick_queue dependency for you.

#### Using FetchContent (Recommended)
```cmake
cmake_minimum_required(VERSION 3.20)
project(your_project)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

# Disable examples, tests, and benchmarks for slick-logger
set(BUILD_SLICK_LOGGER_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_SLICK_LOGGER_TESTING OFF CACHE BOOL "" FORCE)
set(BUILD_SLICK_LOGGER_BENCHMARKS OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    slick-logger
    GIT_REPOSITORY https://github.com/SlickQuant/slick-logger.git
    GIT_TAG main
)

FetchContent_MakeAvailable(slick-logger)

add_executable(your_app main.cpp)
target_link_libraries(your_app slick::logger)
```

#### Using find_package
If you've installed slick-logger system-wide:

```cmake
cmake_minimum_required(VERSION 3.20)
project(your_project)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(slick-logger REQUIRED)

add_executable(your_app main.cpp)
target_link_libraries(your_app slick::logger)
```

#### Manual Integration
```cmake
cmake_minimum_required(VERSION 3.20)
project(your_project)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Add slick-logger include directory
include_directories(path/to/slick-logger/include, path/to/slick-queue/include)

add_executable(your_app main.cpp)
```

## Usage

### Basic Usage

```cpp
#include <slick/logger.hpp>

int main() {
    // Initialize the logger (traditional way)
    slick::logger::Logger::instance().init("app.log", 1024); // queue size

    // Log messages - formatting happens in background thread for performance
    LOG_INFO("Application started");
    LOG_DEBUG("Debug value: {}", 42);                    // std::format style placeholders
    LOG_WARN("Processed {} items", 150);
    LOG_ERROR("Error in {} at line {}", "function_name", 123);
    LOG_INFO("User {} balance: ${:.2f}", "Alice", 1234.56); // Format specifiers supported

    // Shutdown (optional, called automatically on destruction)
    slick::logger::Logger::instance().shutdown();
    return 0;
}
```

### String Formatting with std::format

slick-logger uses C++20's `std::format` for type-safe and efficient string formatting:

```cpp
#include <slick/logger.hpp>

int main() {
    slick::logger::Logger::instance().init("app.log");

    // Basic placeholders
    LOG_INFO("Simple message: {}", "hello");
    LOG_INFO("Number: {}", 42);

    // Multiple arguments
    LOG_INFO("User {} has {} items", "Alice", 15);

    // Format specifiers (same as std::format)
    LOG_INFO("Price: ${:.2f}", 29.99);           // Currency with 2 decimals
    LOG_INFO("Progress: {:.1f}%", 85.7);         // Percentage with 1 decimal
    LOG_INFO("Hex value: 0x{:x}", 255);          // Hexadecimal
    LOG_INFO("Binary: 0b{:b}", 42);              // Binary
    LOG_INFO("Scientific: {:.2e}", 12345.67);    // Scientific notation

    // Width and alignment
    LOG_INFO("Right aligned: {:>10}", "text");   // Right align in 10 chars
    LOG_INFO("Left aligned: {:<10}", "text");    // Left align in 10 chars
    LOG_INFO("Centered: {:^10}", "text");        // Center in 10 chars

    // Zero padding
    LOG_INFO("Zero padded: {:04d}", 42);         // 0042

    // Custom types (as long as they support std::formatter)
    std::vector<int> numbers = {1, 2, 3, 4, 5};
    LOG_INFO("Vector size: {}", numbers.size());

    slick::logger::Logger::instance().shutdown();
    return 0;
}
```

**Benefits of std::format:**
- **Type Safety**: Compile-time checking of format strings and arguments
- **Performance**: Highly optimized formatting implementation
- **Rich Formatting**: Support for width, precision, alignment, and custom formatters
- **Extensible**: Easy to add custom formatters for user-defined types
- **Standard**: Part of C++20 standard library, no external dependencies

### Passing std::format_args

You can pass a pre-built `std::format_args` object as the single argument to any log call. This lets you capture format arguments once and reuse them, or forward a pre-built arg pack from another function:

```cpp
int count = 42;
double price = 9.99;
std::string_view name = "widget";

// Capture args once, pass to logger
auto args = std::make_format_args(count, price, name);
LOG_INFO("count={} price={:.2f} name={}", args);

// Also works inline
LOG_DEBUG("x={} y={}", std::make_format_args(x, y));
```

> **Note:** `std::make_format_args` requires all arguments to be lvalues. Pass temporary values via a named variable.

> **Limitation:** Custom formatter types (types requiring a `std::formatter` specialization, represented as `handle` inside `std::format_args`) are not supported. They will be logged as `<handle>`. Use the normal variadic log call for custom-formatted types.

### Multi-Sink Usage

```cpp
#include <slick/logger.hpp>

int main() {
    using namespace slick::logger;

    // Setup multiple sinks
    Logger::instance().clear_sinks();
    Logger::instance().add_console_sink(true, true);     // colors + stderr for errors
    Logger::instance().add_file_sink("app.log");         // basic file logging

    // Configure rotation
    RotationConfig rotation;
    rotation.max_file_size = 10 * 1024 * 1024;  // 10MB
    rotation.max_files = 5;                      // keep last 5 files
    Logger::instance().add_rotating_file_sink("debug.log", rotation);

    // Initialize with queue size
    Logger::instance().init(8192);

    // Logs appear in console (colored) AND both files!
    LOG_INFO("Multi-sink logging is active!");
    LOG_ERROR("Errors go to stderr and files");

    Logger::instance().shutdown();
    return 0;
}
```

### Logging to Specific Sinks

You can log messages to specific sinks by name or get a reference to a sink. Each sink also supports its own minimum log level filtering:

```cpp
#include <slick/logger.hpp>

int main() {
    using namespace slick::logger;

    Logger::instance().clear_sinks();
    Logger::instance().add_file_sink("app.log", "app_sink");
    Logger::instance().add_file_sink("debug.log", "debug_sink");
    Logger::instance().add_console_sink(true, false, "console");

    // Set per-sink log levels (second level of filtering)
    auto debug_sink = Logger::instance().get_sink("debug_sink");
    if (debug_sink) {
        debug_sink->set_min_level(LogLevel::L_DEBUG);  // Only DEBUG and above
    }

    Logger::instance().init(8192);

    // Log to all sinks (default behavior) - filtered by global level
    LOG_INFO("This goes to all sinks that accept INFO level");

    // Log to specific sink by reference
    auto app_sink = Logger::instance().get_sink("app_sink");
    if (app_sink) {
        app_sink->log_info("This goes only to app.log");
        app_sink->log_error("Error in app.log only");
    }

    // Direct logging to debug sink - also filtered by sink's min level
    if (debug_sink) {
        debug_sink->log_debug("Debug info only in debug.log");
        debug_sink->log_trace("This won't appear - below sink's min level");
        debug_sink->log_warn("Warning only in debug.log");
    }

    Logger::instance().shutdown();
    return 0;
}
```

**Sink-Level Log Filtering:**
- Each sink has its own `min_level` setting independent of the global logger level
- Messages are filtered twice: first by global logger level, then by sink-specific level
- Use `sink->set_min_level(LogLevel::L_WARN)` to control what each sink accepts
- This allows different sinks to have different verbosity levels

### Dedicated Sinks

Dedicated sinks only receive messages logged directly to them, not broadcast messages from LOG_* macros:

```cpp
#include <slick/logger.hpp>

int main() {
    using namespace slick::logger;

    Logger::instance().clear_sinks();

    // Create a regular sink (receives all messages)
    Logger::instance().add_file_sink("regular.log", "regular");

    // Create a dedicated sink (only receives direct messages)
    auto dedicated_sink = std::make_shared<FileSink>("dedicated.log", "dedicated");
    dedicated_sink->set_dedicated(true);  // Mark as dedicated
    Logger::instance().add_sink(dedicated_sink);

    Logger::instance().init(8192);

    // This goes to regular.log only (dedicated sink ignores broadcasts)
    LOG_INFO("Broadcast message - regular sink only");

    // This goes to dedicated.log only
    dedicated_sink->log_info("Direct message to dedicated sink");

    // You can also make any sink dedicated
    auto regular_sink = Logger::instance().get_sink("regular");
    if (regular_sink) {
        regular_sink->set_dedicated(true);  // Now it's dedicated too
        regular_sink->log_warn("This goes to regular.log only");
    }

    Logger::instance().shutdown();
    return 0;
}
```

**Use Cases for Dedicated Sinks:**
- **Audit Logging**: Critical security events that should only go to specific files
- **Performance Monitoring**: Metrics that shouldn't clutter main application logs
- **Error Isolation**: Separate error streams for different components
- **Compliance**: Regulatory requirements for certain log types

### Advanced Configuration

```cpp
#include <slick/logger.hpp>

int main() {
    using namespace slick::logger;
    
    // Create configuration
    LogConfig config;
    config.sinks.push_back(std::make_shared<ConsoleSink>(true, true));
    config.sinks.push_back(std::make_shared<FileSink>("application.log"));
    
    // Add rotating file sink for errors
    RotationConfig rotation;
    rotation.max_file_size = 5 * 1024 * 1024;  // 5MB
    rotation.max_files = 10;
    config.sinks.push_back(std::make_shared<RotatingFileSink>("errors.log", rotation));
    
    // Add daily log files
    config.sinks.push_back(std::make_shared<DailyFileSink>("daily.log"));
    
    config.min_level = LogLevel::INFO;
    config.queue_size = 16384;
    
    Logger::instance().init(config);
    
    // Logs go to: console + application.log + errors.log + daily_YYYY-MM-DD.log
    LOG_INFO("Advanced multi-sink setup complete!");
    
    Logger::instance().shutdown();
    return 0;
}
```

## Sink Types

### ConsoleSink
Outputs to stdout/stderr with optional ANSI color support:
- **Colors**: Configurable color coding by log level
- **Error Routing**: WARN/ERROR/FATAL can go to stderr
- **Cross-Platform**: Works on Windows, Linux, macOS

### FileSink  
Basic file logging:
- **Append Mode**: Writes to specified file
- **Thread-Safe**: Single writer thread handles all file operations
- **Auto-Creation**: Creates directories if they don't exist

### RotatingFileSink
Size-based log rotation:
- **Max File Size**: Configurable size limit (default: 10MB)
- **File Retention**: Keep last N files, auto-delete oldest
- **Naming**: `log.txt` → `log_1.txt` → `log_2.txt` etc.
- **Atomic Rotation**: Thread-safe file rotation

### DailyFileSink
Date-based log rotation:
- **Daily Files**: Creates new file each day
- **Date Format**: `filename_YYYY-MM-DD.log`
- **Automatic**: Switches files at midnight
- **Retention**: Configurable cleanup of old files

## Rotation Configuration

```cpp
slick::logger::RotationConfig config;
config.max_file_size = 50 * 1024 * 1024;  // 50MB
config.max_files = 10;                     // keep last 10 files
config.compress_old = false;               // future feature
config.rotation_hour = std::chrono::hours(0); // midnight for daily rotation
```

## Log Levels

- **TRACE**: Detailed debug information
- **DEBUG**: General debug information  
- **INFO**: Informational messages
- **WARN**: Warning messages
- **ERR**: Error messages (internally named ERR to avoid Windows macro conflicts)
- **FATAL**: Fatal error messages

## Architecture

The logger uses a **multi-producer, single-consumer** ring buffer (slick_queue) with a **multi-sink architecture**:

```
[Thread 1] ──┐
[Thread 2] ──┼──► [Lock-Free Queue] ──► [Writer Thread] ──┬──► ConsoleSink
[Thread N] ──┘                                            ├──► FileSink  
                                                          ├──► RotatingFileSink
                                                          └──► DailyFileSink
```

### Key Design Principles

1. **Single Writer Thread**: One dedicated thread handles all sink operations
2. **Lock-Free Logging**: Caller threads never block on I/O operations  
3. **Flexible Sinks**: Easy to add custom sink implementations
4. **Atomic Operations**: Thread-safe queue and sink management

### Deferred Formatting

For optimal performance, the logger defers string formatting to the background thread:

1. **Caller Thread**: Captures format string and arguments in a lambda (fast)
2. **Lock-Free Queue**: Stores the lambda in the ring buffer (minimal latency)
3. **Writer Thread**: Executes lambda to format string and writes to all sinks

This approach moves potentially expensive formatting and I/O operations off the critical path, making logging calls extremely fast and suitable for high-frequency logging scenarios.

### Multi-Sink Benefits

- **Simultaneous Output**: Log to console + multiple files at once
- **Different Retention**: Each sink can have its own rotation policy
- **Performance**: Single writer thread efficiently handles all sinks
- **Flexibility**: Mix and match sink types as needed

## Integration

slick_queue is downloaded automatically during the build process from https://github.com/SlickQuant/slick_queue.

## Header-Only Benefits

Being a header-only library provides several advantages:

- **Zero Linking**: No need to link against library files
- **Easy Integration**: Just include the headers in your project
- **Template Optimization**: Compiler can better optimize template instantiations
- **No Binary Dependencies**: No need to distribute or manage .lib/.a files
- **Immediate Usage**: Start logging with just two `#include` statements

## Custom Sinks

Creating custom sinks is straightforward - just inherit from `ISink`:

```cpp
class JsonSink : public slick::logger::ISink {
    std::ofstream file_;
    bool first_entry_ = true;
    
public:
    explicit JsonSink(const std::filesystem::path& filename) : file_(filename) {
        file_ << "[\n"; // Start JSON array
    }
    
    void write(const slick::logger::LogEntry& entry) override {
        // Format as JSON - see examples/multi_sink_example.cpp for full implementation
        const char* level_str = /* convert level to string */;
        auto [message, _] = format_log_message(entry);
        
        if (!first_entry_) file_ << ",\n";
        first_entry_ = false;
        
        file_ << "  {\n"
              << "    \"timestamp\": \"" << /* formatted timestamp */ << "\",\n"  
              << "    \"level\": \"" << level_str << "\",\n"
              << "    \"message\": \"" << message << "\"\n"
              << "  }";
    }
    
    void flush() override { file_.flush(); }
};

// Usage
Logger::instance().add_sink(std::make_shared<JsonSink>("app.json"));
```

## Examples

The repository includes comprehensive examples:

- **`logger_example.exe`**: Basic usage with console + file output
- **`multi_sink_example.exe`**: Demonstrates all sink types, rotation, and custom sinks

## Building Examples/Tests  

If you want to build the provided examples and tests:

```bash
mkdir build
cd build
cmake ..
cmake --build . --config Debug

# Run examples
./examples/Debug/logger_example.exe
./examples/Debug/multi_sink_example.exe

# Run tests  
./tests/Debug/slick_logger_tests.exe         # Original tests
./tests/Debug/slick_logger_sink_tests.exe    # Multi-sink tests
```
