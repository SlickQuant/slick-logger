# slick-logger

[![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)](https://en.cppreference.com/w/cpp/20)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![Header-only](https://img.shields.io/badge/header--only-yes-brightgreen.svg)](#installation)
[![Lock-free](https://img.shields.io/badge/concurrency-lock--free-orange.svg)](#architecture)
[![CI](https://github.com/SlickQuant/slick-logger/actions/workflows/ci.yml/badge.svg)](https://github.com/SlickQuant/slick-logger/actions/workflows/ci.yml)
[![GitHub release](https://img.shields.io/github/v/release/SlickQuant/slick-logger)](https://github.com/SlickQuant/slick-logger/releases)

A high-performance, cross-platform **header-only** logging library for C++20 using a multi-producer, multi-consumer ring buffer with **multi-sink support**, **source-location logging**, **multi-process logging**, and **log rotation** capabilities.

## Features

- **High Performance**: Asynchronous logging using the slick-queue ring buffer for minimal latency
- **Modern Formatting**: Uses C++20 `std::format` for type-aware, efficient string formatting
- **Multi-Sink Architecture**: Log to multiple destinations simultaneously (console, files, custom sinks)
- **Log Rotation**: Size-based and time-based rotation with configurable retention
- **Colored Console Output**: ANSI color support with configurable error routing
- **Source Locations by Default**: `LOG_*` macros include the call-site file and line, with runtime controls for basename vs full path
- **Runtime Configuration**: Configure sinks, queue sizes, log level, source-location output, and timestamp formats
- **Macro Fast Path**: Disabled log levels skip argument evaluation before queueing, globally and per sink
- **Direct Sink Logging**: Route messages to a named sink or a sink reference when a message should not be broadcast
- **Binary Payloads**: Log raw bytes and write them to a file verbatim with `BinarySink`, encoding entirely under your control
- **Shared-Library Redirection**: Route plugin or strategy-library logs into a host application's logger
- **Multi-Process Logging**: Several processes can log into one shared-memory queue drained by a single collector process
- **Header-Only**: No linking required - just include and use
- **Cross-Platform**: Supports Windows, Linux, and macOS
- **Multi-Threaded**: Safe for concurrent logging from multiple threads
- **C++20**: Utilizes modern C++ features
- **Easy to Use**: Simple macros for logging at different levels

## Requirements

- **C++20 compatible compiler** with `std::format` support (GCC 11+, Clang 14+, MSVC 19.29+)
- CMake 3.20 or higher (for building examples/tests)
- slick-queue 2.0.0 or newer (multi-process logging relies on its shared-memory support). The installed CMake package requires this version through `find_dependency`, so an older slick-queue fails at configure time
- Internet connection for downloading the slick-queue header when it is not already installed

## Installation

### Option 1: Direct Copy

The simplest route is the archive attached to a [release](https://github.com/SlickQuant/slick-logger/releases): it already bundles `slick/queue.h` and the `slick/shm/` headers alongside `slick/logger.hpp`, so unpacking it and adding `include/` to your include path is all that is needed.

To assemble the headers by hand instead, you need both slick-logger and its dependency:

1. Copy the `include/slick/` directory to your project
2. Download `queue.h` from https://raw.githubusercontent.com/SlickQuant/slick-queue/main/include/slick/queue.h
3. Place `queue.h` in your include path or alongside the slick-logger headers
4. Copy the `slick/shm/` headers from [slick-shm](https://github.com/SlickQuant/slick-shm) as well — slick-queue includes them for its shared-memory support

Your project structure should look like:
```
your_project/
├── include/
│   ├── slick/
│       └── logger.hpp
│       └── queue.h
│       └── shm/
└── src/
    └── main.cpp
```

The CMake options below handle all of this automatically, and are the recommended path.

### Option 2: CMake Integration (Recommended)

CMake automatically handles the slick-queue dependency for you.

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

# Optional: disable LOG_* macro source-location capture at compile time
set(SLICK_LOGGER_ENABLE_SOURCE_LOCATION OFF CACHE BOOL "" FORCE)

# Optional: build the standalone multi-process collector executable (OFF by default).
# The shared-memory feature itself is header-only and always available.
set(BUILD_SLICK_LOGGER_COLLECTOR ON CACHE BOOL "" FORCE)

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

# Optional: also accepts -DSLICK_LOGGER_ENABLE_SOURCE_LOCATION=OFF on the CMake command line
set(SLICK_LOGGER_ENABLE_SOURCE_LOCATION OFF CACHE BOOL "" FORCE)

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

By default, each `LOG_*` macro entry includes the macro call-site file name and line number:

```text
2026-07-01 12:34:56.123456 [INFO] [main.cpp:8] Application started
```

### Log Macros and Levels

Use the level-specific macros for normal application logging:

```cpp
LOG_TRACE("order book depth={}", depth);
LOG_DEBUG("request id={}", request_id);
LOG_INFO("connected to {}", endpoint);
LOG_WARN("retrying after {} ms", delay_ms);
LOG_ERROR("request failed: {}", reason);
LOG_FATAL("unrecoverable error: {}", reason);
```

**Prefer the macros over calling `Logger::instance().log(...)` directly.** They test the level *before* the arguments appear in the expansion, so a filtered-out call computes nothing, while a direct call has to evaluate its arguments and only then discovers that the level rejects the message. They also attach the call site: `Logger::log()` passes no file or line, so entries logged that way carry no `file:line`. The direct call remains available for the cases that need it — forwarding a level chosen at runtime, or logging from code that has no call site worth recording.

```cpp
using namespace slick::logger;

Logger::instance().set_level(LogLevel::L_WARN);

LOG_DEBUG("expensive value: {}", build_expensive_debug_value()); // not evaluated
LOG_WARN("visible warning");                                     // written, with file:line

// The same message through the direct call: the argument is built, the
// entry is then dropped inside log(), and nothing records where it came from.
Logger::instance().log(LogLevel::L_DEBUG, "expensive value: {}",
                       build_expensive_debug_value());           // evaluated, then dropped

auto current_level = Logger::instance().get_level();
```

The available levels are `L_TRACE`, `L_DEBUG`, `L_INFO`, `L_WARN`, `L_ERROR`, `L_FATAL`, and `L_OFF`. A log call supports up to `SLICK_LOGGER_MAX_ARGS` format arguments; define that macro before including `slick/logger.hpp` if you need a different limit.

To target a single sink, use the `LOG_SINK_*` macros, which apply the same fast path against **both** the global level and that sink's own minimum level:

```cpp
LOG_SINK_TRACE(sink, "order book depth={}", depth);
LOG_SINK_DEBUG(sink, "request id={}", request_id);
LOG_SINK_INFO(sink, "connected to {}", endpoint);
LOG_SINK_WARN(sink, "retrying after {} ms", delay_ms);
LOG_SINK_ERROR(sink, "request failed: {}", reason);
LOG_SINK_FATAL(sink, "unrecoverable error: {}", reason);
```

`sink` may be an `ISink` reference, a raw pointer, or a smart pointer, and is evaluated exactly once. An empty smart pointer, or a sink that has not been added to a logger, makes the call a no-op. See [Logging to Specific Sinks](#logging-to-specific-sinks).

### Source Location Logging

Source-location logging is enabled by default for `LOG_*` macros. The output always uses only the basename (e.g. `main.cpp:8`):

```cpp
#include <slick/logger.hpp>

int main() {
    using namespace slick::logger;

    Logger::instance().init("app.log");

    LOG_INFO("uses basename by default"); // [main.cpp:8]

    Logger::instance().set_source_location_enabled(false);
    LOG_INFO("source location omitted");

    Logger::instance().shutdown();
}
```

You can configure the same behavior during initialization:

```cpp
using namespace slick::logger;

LogConfig config;
config.sinks.push_back(std::make_shared<FileSink>("app.log"));
config.include_source_location = true;  // default

Logger::instance().init(config);
```

Compile-time controls must be defined before including `slick/logger.hpp`:

When you use CMake with `FetchContent`, `add_subdirectory`, or `find_package`, disable `LOG_*` macro source-location capture through the `slick::logger` target with:

```bash
cmake -S . -B build -DSLICK_LOGGER_ENABLE_SOURCE_LOCATION=OFF
```

For a single target, you can also set the compile definition directly:

```cmake
target_compile_definitions(your_app PRIVATE SLICK_LOGGER_ENABLE_SOURCE_LOCATION=0)
```

```cpp
// Disable source-location capture in LOG_* macros entirely.
#define SLICK_LOGGER_ENABLE_SOURCE_LOCATION 0
#include <slick/logger.hpp>
```

For bridge code that receives source information from another logging layer, a direct source-location overload is also available. The `const char*` path passed is copied before the entry is queued, so dynamic strings are safe, and the basename is extracted automatically:

```cpp
std::string path = "C:\\repo\\app\\bridge.cpp";
slick::logger::Logger::instance().log_with_location(
    slick::logger::LogLevel::L_INFO,
    path.c_str(),
    42,
    "bridged message"); // logged as [bridge.cpp:42]
```

Direct sink helpers such as `sink->log_info(...)` route to that sink only and do not automatically attach the caller's source location. Use `LOG_*` macros when you want automatic call-site capture, or `LOG_SINK_*` when you want it *and* a specific sink.

### String Formatting with std::format

slick-logger uses C++20's `std::format` for type-aware and efficient string formatting:

```cpp
#include <slick/logger.hpp>

int main() {
    slick::logger::Logger::instance().init("app.log");

    // Basic placeholders
    LOG_INFO("Simple message: {}", "hello");
    LOG_INFO("Number: {}", 42);

    // Multiple arguments
    LOG_INFO("User {} has {} items", "Alice", 15);

    // Numbered (positional) placeholders, with or without a format spec
    LOG_INFO("{0} beat {1}, so {0} advances", "Alice", "Bob");
    LOG_INFO("{1:>8} | {0:.2f}", 3.14159, "label");

    // Dynamic width and precision, taken from an argument
    LOG_INFO("[{:{}}]", 42, 8);                  // [      42]
    LOG_INFO("[{:{}.{}f}]", 3.14159, 9, 2);      // [     3.14]
    LOG_INFO("[{0:{1}}]", 42, 8);                // [      42]

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

    // wchar_t renders as text by default and as a number under b/B/d/o/x/X
    LOG_INFO("Char: {}", L'\u00e9');              // e-acute, UTF-8 encoded
    LOG_INFO("Code point: U+{:04X}", L'\u00e9');  // U+00E9

    // Custom types (as long as they support std::formatter)
    std::vector<int> numbers = {1, 2, 3, 4, 5};
    LOG_INFO("Vector size: {}", numbers.size());

    slick::logger::Logger::instance().shutdown();
    return 0;
}
```

**Numbered placeholders:** `{0}`, `{1}`, ... select an argument by position and
may carry a format spec (`{1:>8}`). An index past the end of the argument list
renders as `<MISSING_ARG>` rather than discarding the line. Unlike `std::format`,
which rejects a format string that mixes explicit indices with bare `{}`, this
parser tolerates the mix: the automatic counter advances only through bare `{}`
placeholders, so `"{} {0} {}"` with `(111, 222)` yields `111 111 222`.

**Dynamic width and precision:** a nested field supplies a width or precision
from an argument — `{:{}}`, `{:.{}f}`, `{:{}.{}f}`, and the positional
`{0:{1}}`. A nested field takes the argument *after* the one being formatted, so
`"{:{}}"` formats the first argument to the width given by the second. The
argument must be a non-negative integer; anything else is a format error. If it
was not passed at all, that field renders `<MISSING_ARG>` and the rest of the
line is unaffected.

**wchar_t arguments:** a `wchar_t` follows the same rule `std::format` applies to
`char` — it renders as text by default (`{}`, `{:c}`, `{:>4}`), UTF-8 encoded, and
as a number under a `b`/`B`/`d`/`o`/`x`/`X` presentation type (`{:d}` on `L'A'`
gives `65`). Because the choice comes from the spec and not the value, a given
spec means the same thing for every code point. A lone `wchar_t` that is not a
Unicode scalar value — an unpaired UTF-16 surrogate on Windows — has no character
to print and falls back to its numeric code point. Wide *strings* are not
supported; convert them yourself before logging.

**Benefits of std::format:**
- **Type-Aware Formatting**: Standard C++ formatting for strings, numbers, pointers, chrono values, and custom formatter-enabled types
- **Performance**: Highly optimized formatting implementation
- **Rich Formatting**: Support for width, precision, alignment, and custom formatters
- **Extensible**: Easy to add custom formatters for user-defined types
- **Standard**: Part of C++20 standard library, no external dependencies

> **Caveat — format string must be a true string literal (or otherwise outlive processing):** slick-logger detects "safe to store pointer" format strings by checking whether the argument's type is `const char(&)[N]` — the type category string literals have. This is only a heuristic: any other `const char[N]` lvalue (e.g. a `const char` array **class member**, or a local `const char buf[N]`) has the exact same type and will be misidentified as a literal too. In that case only the pointer is queued, not a copy of the contents, so if the array is mutated or destroyed before the writer thread consumes the log entry, the logged output is corrupted or reads freed memory. Only pass genuine string literals this way; for a `const char` array member variable, pass it as `std::string_view{member}` (or another type convertible to `std::string_view`) instead, so its contents are copied into the queue.

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

    // You can also look up the first sink of a concrete type
    auto first_file_sink = Logger::instance().get_sink<FileSink>();
    if (first_file_sink) {
        first_file_sink->log_info("Message routed to the first FileSink");
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
- Raising a sink's `min_level` also drops entries that are already queued but not yet written, because the level is re-checked when the writer thread dispatches. Call `Logger::instance().flush()` first if those entries must survive.

**Prefer the `LOG_SINK_*` macros over `sink->log_*(...)`.** The direct helpers must evaluate their arguments before the call can decide to drop the message; the macros check the sink's level first, so nothing is computed for a filtered-out call. They also attach the call site, which the direct helpers cannot:

```cpp
auto audit = Logger::instance().get_sink("audit");
audit->set_min_level(LogLevel::L_WARN);

LOG_SINK_INFO(audit, "{}", build_expensive_report());  // never evaluated
LOG_SINK_ERROR(audit, "failed with {}", code);         // written, with file:line

audit->log_info("{}", build_expensive_report());       // evaluated, then dropped
```

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
    auto dedicated_sink = std::make_shared<FileSink>(
        "dedicated.log",
        TimestampFormatter::Format::WITH_MICROSECONDS,
        "dedicated");
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
    config.sinks.push_back(std::make_shared<DailyFileSink>("daily.log", RotationConfig{}));
    
    config.min_level = LogLevel::L_INFO;
    config.log_queue_size = 16384;
    config.string_buffer_size = 4 * 1024 * 1024;
    config.include_source_location = true;
    
    Logger::instance().init(config);
    
    // Logs go to: console + application.log + errors.log + daily_YYYY-MM-DD.log
    LOG_INFO("Advanced multi-sink setup complete!");
    
    Logger::instance().shutdown();
    return 0;
}
```

`LogConfig` fields:

- `sinks`: console, file, rotating file, daily file, or custom sinks
- `min_level`: global minimum level, default `LogLevel::L_TRACE`
- `log_queue_size`: internal log-entry queue size, rounded up to a power of two
- `string_buffer_size`: internal string-storage queue size, rounded up to a power of two
- `include_source_location`: include file and line for `LOG_*` macro calls, default `true`

### Lifecycle and Runtime Controls

```cpp
using namespace slick::logger;

Logger::instance().init("app.log", 65536, 4 * 1024 * 1024);

Logger::instance().set_level(LogLevel::L_DEBUG);
Logger::instance().set_source_location_enabled(true);

LOG_INFO("queued asynchronously");

Logger::instance().flush();          // wait until entries queued so far are written
Logger::instance().shutdown();       // flush and stop the writer thread
```

Useful controls:

- `init(path, log_queue_size, string_buffer_size)`: create a default file sink and start logging
- `init(config)`: initialize from `LogConfig`
- `init(queue_size, string_buffer_size)`: start with sinks that were already added
- `flush()`: wait for queued entries to be written **and flushed to their sinks**, while keeping the logger running
- `shutdown(clear_sinks = true)`: flush, stop the writer thread, and optionally clear sinks
- `reset()`: return the singleton to an uninitialized state; mainly intended for tests
- `set_level()` / `get_level()`: update or read the global level filter
- `clear_sinks()`: remove all currently registered sinks before reconfiguration

#### When entries reach disk

The writer thread flushes the file sinks once it has drained the queue, not after every entry, so a burst of logging shares a single flush instead of paying a write syscall per line. In practice this means:

- Once the logger is caught up, everything logged so far is on disk.
- While a backlog is still draining, recent entries may still be sitting in a sink's buffer.
- `flush()` returns only after the writer thread has written **and** flushed every entry queued before the call, so use it whenever you need a guarantee at a specific point (before unloading a plugin, before inspecting the log file, at a checkpoint).
- `shutdown()` drains and flushes before the writer thread exits.

`ConsoleSink` is the exception: it flushes after every line. That is deliberate - an interactive terminal is line-buffered anyway, and when stdout is redirected to a file or a pipe (CI logs, `docker logs`, process supervisors) a per-line flush is what keeps each line immediately visible to the capturing process. Console output is rarely the hot path, so the per-line cost does not matter there.

A custom sink's `flush()` is only ever called on the writer thread, so it does not need its own locking against `write()`.

An idle writer thread parks rather than polling on a timer, so an entry logged into an otherwise idle logger reaches its sink in tens of microseconds instead of waiting out a scheduler tick. Producers only signal a wake-up when the writer is actually parked, so this costs an atomic load and a predictable branch on the logging path. In `QueueMode::SharedCollector` the collector polls instead, because a producer in another process cannot signal it.

### Timestamp Formatting

Every built-in sink supports the default microsecond timestamp format, a predefined timestamp format enum, or a custom `strftime`-style format string:

```cpp
using namespace slick::logger;

Logger::instance().clear_sinks();
RotationConfig rotation;
Logger::instance().add_console_sink(TimestampFormatter::Format::ISO8601);
Logger::instance().add_file_sink("milliseconds.log", TimestampFormatter::Format::WITH_MILLISECONDS);
Logger::instance().add_rotating_file_sink("custom.log", rotation, "%Y-%m-%d %H:%M:%S");
Logger::instance().init();
```

Available predefined formats:

- `TimestampFormatter::Format::WITH_MICROSECONDS` (default)
- `TimestampFormatter::Format::WITH_MILLISECONDS`
- `TimestampFormatter::Format::DEFAULT`
- `TimestampFormatter::Format::ISO8601`
- `TimestampFormatter::Format::TIME_ONLY`
- `TimestampFormatter::Format::CUSTOM`

The predefined formats are rendered by writing digits straight into a stack buffer, on top of a broken-down time that is computed once per whole second and cached. A burst of entries within the same second therefore costs no `localtime` call at all. The cache is `thread_local`, so a `TimestampFormatter` may be shared between threads, and every sink on the writer thread shares one `localtime` call per second.

`Format::CUSTOM` is the exception: an arbitrary `strftime` pattern still goes through `std::put_time`, which measures more than an order of magnitude slower per entry than the predefined formats. Prefer a predefined format on a hot logging path.

### Sharing the Logger Across Shared Libraries (Plugin / Strategy Pattern)

Because slick-logger is header-only, each binary (EXE or `.dll`/`.so`) that includes `logger.hpp` gets its own `Logger` instance. This means `LOG_*` calls inside a dynamically-loaded plugin will not appear in the host application's log file by default.

Use `Logger::set_instance()` to redirect a plugin's `LOG_*` calls to the host's logger:

**Host application** — pass its logger to the plugin after loading:

```cpp
// host/main.cpp
#include <slick/logger.hpp>

// Platform-specific shared library loading omitted for brevity (LoadLibrary on Windows, dlopen on Linux)
using StrategyInitFn = void(*)(slick::logger::Logger&);

void load_strategy(void* handle) {
    auto* init_fn = reinterpret_cast<StrategyInitFn>(get_symbol(handle, "strategy_init"));
    if (init_fn) {
        // Redirect the plugin's LOG_* macros to this process's logger
        init_fn(slick::logger::Logger::instance());
    }
}
```

**Plugin / strategy shared library** — expose a C-linkage init function:

```cpp
// strategy/strategy.cpp
#include <slick/logger.hpp>

extern "C" void strategy_init(slick::logger::Logger& framework_logger) {
    // All LOG_* calls in this shared library now route to the host's logger
    slick::logger::Logger::set_instance(&framework_logger);
    LOG_INFO("Strategy loaded — logger connected to framework");
}

extern "C" void strategy_shutdown() {
    // Optional: restore this library's own local logger on unload
    slick::logger::Logger::clear_instance_override();
}
```

> **Why `extern "C"`?** C linkage prevents name-mangling differences between compilers, ensuring `get_symbol`/`GetProcAddress`/`dlsym` can reliably locate the function.

> **Thread-safety:** `set_instance()` uses `std::atomic` with acquire-release ordering. Call it once during plugin initialization, before any logging threads in the plugin start.

> **Unload ordering — important:** `LOG_*` with string literals stores a raw pointer to the format string, which lives in the plugin's code segment. The host must call `Logger::instance().flush()` **before** calling `FreeLibrary`/`dlclose`. `flush()` blocks until the writer thread has consumed all queued entries, then returns with the logger still running so host logging can continue normally. Using `shutdown()` instead would stop the logger entirely.

> **Multiple plugins:** Each shared library (`.dll`/`.so`) holds its own copy of `override_instance_`. All plugins can independently call `set_instance()` with the same host logger — the host logger's lock-free queue is designed for concurrent producers.

### Multi-Process Logging (Shared Memory)

Several processes can log into a single shared-memory ring buffer that one **collector**
process drains. The collector owns the sinks and the writer thread; **producer** processes
own neither and only enqueue entries.

```
Process A ──┐
Process B ──┼──► [Shared Memory Ring] ──► [Collector Process] ──┬──► ConsoleSink
Process C ──┘                                                   └──► FileSink
```

#### Collector

The collector needs at least one sink, exactly like a single-process logger:

```cpp
#include <slick/logger.hpp>
using namespace slick::logger;

LogConfig config;
config.mode = QueueMode::SharedCollector;
config.shared_memory_name = "myapp_log";
config.sinks.push_back(std::make_shared<ConsoleSink>());
config.sinks.push_back(std::make_shared<FileSink>("collected.log"));

Logger::instance().init(config);
// ... run until stopped ...
Logger::instance().shutdown();   // drains queued entries, then closes the sinks
```

#### Producer

A producer must **not** define sinks — `init()` throws if it does, since the collector owns them:

```cpp
LogConfig config;
config.mode = QueueMode::SharedProducer;
config.shared_memory_name = "myapp_log";   // same name as the collector
config.process_tag = "feed";               // optional, shows up as [pid:feed]

Logger::instance().init(config);

LOG_INFO("order {} filled at {}", order_id, price);   // goes to the collector

Logger::instance().shutdown();
```

All the usual `LOG_*` macros, sinks-by-index, log levels, and source locations work unchanged.

#### Output

Multi-process entries carry the producing process id and optional tag, right after the level:

```
2026-08-28 16:37:53.673071 [INFO] [103836:app1] [main.cpp:81] work item 1 of 4
2026-08-28 16:37:53.683942 [INFO] [72176:app2] [main.cpp:81] work item 1 of 4
```

Single-process (`QueueMode::Local`) output is unchanged — local entries carry no process id,
so nothing extra is printed.

#### Standalone collector executable

Rather than embedding a collector, you can run the bundled one. Prebuilt binaries for Linux,
macOS, and Windows are attached to each [release](https://github.com/SlickQuant/slick-logger/releases)
as `slick-log-collector-<version>-<platform>`.

To build it from source instead — it is **not built by default**:

```bash
cmake -S . -B build -DBUILD_SLICK_LOGGER_COLLECTOR=ON
cmake --build build

./build/tools/slick_log_collector --name myapp_log --console --file collected.log
```

```
slick_log_collector --name <shm-name>
                    [--console] [--file PATH] [--rotating PATH] [--daily PATH]
                    [--max-size BYTES] [--max-files N]
                    [--level trace|debug|info|warn|error|fatal]
                    [--queue-size N] [--string-buffer-size N]
                    [--stall-timeout-ms N]
```

It stops on Ctrl-C (`SIGINT`/`SIGTERM`), draining whatever is still queued before exiting.

#### Configuration

| Field | Default | Meaning |
| --- | --- | --- |
| `mode` | `QueueMode::Local` | `Local`, `SharedProducer`, or `SharedCollector` |
| `shared_memory_name` | `""` | Segment name, required for the shared modes. 1-24 characters of `[A-Za-z0-9_]` |
| `process_tag` | `""` | Optional short process label, truncated to 15 characters |
| `collect_backlog` | `true` | Collector replays entries already resident in the ring when it attaches |
| `stalled_entry_timeout_ms` | `5000` | Collector abandons an unpublished slot after this long; `0` disables |

#### Startup order and sizing

Startup order does not matter. Whichever process starts first creates the segments and fixes
their capacity from its `log_queue_size` / `string_buffer_size`; every later process attaches
and **inherits** those sizes, so mismatched settings between processes are harmless.

A collector that attaches to a segment already holding entries replays whatever is still
resident in the ring, so producers can run before any collector exists. Set
`collect_backlog = false` if a restarting collector must not re-emit entries a previous
instance already wrote.

#### Constraints

- **All processes must use the same slick-logger version, the same `SLICK_LOGGER_MAX_ARGS`,
  and the same architecture.** `sizeof(LogEntry)` is recorded in the shared-memory header, so a
  mismatch throws at attach time rather than corrupting data.
- **The ring is lossy with no backpressure.** A collector that cannot keep up loses entries, and
  string data can be overwritten before the entry referencing it is read. Size
  `string_buffer_size` generously for high-volume logging.
- **Pointer arguments** (`ArgType::PTR`) print addresses that are only meaningful inside the
  producing process.
- **Sink indices** used by `log_to_sink()` and `ISink::log()` are resolved against the
  *collector's* sink list.
- **A producer killed between reserve and publish** leaves a hole. The collector abandons it
  after `stalled_entry_timeout_ms` and logs a warning, rather than stalling forever.
- **Every attached collector sees every entry** — the ring is a broadcast. Running two collectors
  on one segment writes each entry twice.
- **`process_tag` is truncated to 15 bytes**, on a UTF-8 character boundary so a multi-byte tag is
  never cut mid-sequence.
- **Re-initializing in a shared role is not free on POSIX.** Because a created segment is never
  unlinked (see below), each `init()`/`shutdown()` cycle that *created* its segments retains one
  more mapping for the life of the process. That suits the normal one-shot lifecycle; a process
  that cycles the logger many times should attach to a segment created elsewhere, which is never
  retained. Windows is unaffected.
- **POSIX cleanup is manual, by design.** slick-logger never `shm_unlink`s a segment. Unlinking
  frees the *name* while existing mappings stay valid, so whichever process did it would strand
  everyone still attached: newcomers would create a fresh segment under the same name and their
  entries would silently vanish. Since any process may create the segment, none of them can know
  it is the last user. Segments therefore persist in `/dev/shm/` until removed:

  ```bash
  rm -f /dev/shm/myapp_log /dev/shm/myapp_log_str
  ```

  Remove them only when no participant is running. A stale segment from a previous run is
  attached to rather than recreated, and `collect_backlog` will replay whatever entries it still
  holds — clear it between unrelated runs, or set `collect_backlog = false`. On Windows the
  mapping is refcounted by the kernel and disappears once the last handle closes, so nothing
  needs cleaning up.

See `examples/multi_process_example.cpp` for a runnable version of both roles.

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

### BinarySink
Raw byte capture — see [Binary Logging](#binary-logging):
- **Verbatim**: Writes binary payloads byte for byte, adding nothing of its own
- **Your Encoding**: No header, no framing, no separators; the file's format is whatever you wrote into it
- **Binary Mode**: Opened with `std::ios::binary`, so `0x0A` bytes survive on Windows
- **Extensible**: Override `write_payload()` to add framing
- **Dedicated by Default**: Only receives entries addressed to it

## Binary Logging

`BinarySink` writes raw bytes to a file exactly as given, so an application can capture packets, wire messages, or serialized records through the same asynchronous queue as its text logs — without a second I/O path and without formatting on the writer thread.

Wrap bytes in `as_binary()` and log them like any other argument:

```cpp
#include <slick/logger.hpp>

using namespace slick::logger;

#pragma pack(push, 1)
struct Trade {
    uint64_t timestamp_ns;
    uint32_t quantity;
    double   price;
    char     symbol[8];
};
#pragma pack(pop)

int main() {
    auto& logger = Logger::instance();
    auto capture = logger.add_binary_sink("trades.bin", "capture");
    logger.init(1 << 16);

    Trade trade{1'725'000'000'000'000'000ULL, 100, 431.25, {'A','A','P','L'}};

    // A POD, by address and size
    LOG_SINK_INFO(capture, "{}", as_binary(&trade, sizeof(trade)));

    // Or any contiguous range: vector, array, span
    std::vector<uint8_t> frame{0xFE, 0xED, 0x00, 0x0A};
    LOG_SINK_INFO(capture, "{}", as_binary(frame));

    logger.shutdown();   // trades.bin is exactly 28 + 4 bytes
}
```

`add_binary_sink()` returns the sink so it can go straight into `LOG_SINK_*`.

**What lands in the file.** Only the binary payloads. Timestamp, level, format string and non-binary arguments are all ignored — the file is exactly the concatenation of what you encoded. Entries with no binary argument write nothing. This means the file has **no framing of its own**: if you need to walk it back record by record, encode that yourself, or override `write_payload()`:

```cpp
class LengthPrefixedBinarySink : public BinarySink {
public:
    using BinarySink::BinarySink;
protected:
    void write_payload(const std::byte* data, size_t size) override {
        const auto length = static_cast<uint16_t>(size);
        BinarySink::write_payload(reinterpret_cast<const std::byte*>(&length), sizeof(length));
        BinarySink::write_payload(data, size);
    }
};
```

**Text sinks render payloads as hex**, so the same call can feed a binary capture and a readable log. `{}` gives lowercase hex, `{:X}` uppercase, and `{:.N}` renders at most N bytes; the default cap is 64 bytes, after which the total byte count is appended (`0a1bff...(100 bytes)`).

**Payloads larger than 64KB.** A single payload is capped at `slick::logger::kMaxPayloadBytes` (65534) and truncated beyond it, because slick-queue packs its reservation size into 16 bits. To log more, pass the buffer as several arguments of **one** log call:

```cpp
constexpr auto kCap = kMaxPayloadBytes;
LOG_SINK_INFO(capture, "{}{}{}",
              as_binary(p,            kCap),
              as_binary(p + kCap,     kCap),
              as_binary(p + 2 * kCap, size - 2 * kCap));
```

`BinarySink` writes an entry's payloads back to back with nothing between them, so the file is byte-identical to the unsplit buffer.

Do **not** spread the chunks over separate log calls. The queue is multi-producer, so another thread's entry can land between them and interleave the two payloads in the file. One entry is the unit of atomicity, which caps a single payload at `SLICK_LOGGER_MAX_ARGS * kMaxPayloadBytes` (~1.28 MB by default; raise `SLICK_LOGGER_MAX_ARGS` for more).

**Notes:**
- The bytes are copied into the string ring while the log call runs, so the source buffer only has to outlive that call.
- `as_binary()` over a range copies the element bytes as-is, padding included.
- Binary payloads cross process boundaries like any other argument, so a shared-memory collector can own the `BinarySink`.
- A `BinarySink` is dedicated by default, so broadcast `LOG_*` entries do not reach it. Call `set_dedicated(false)` to change that.

Runnable example: [`examples/binary_sink_example.cpp`](examples/binary_sink_example.cpp).

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
- **ERROR**: Error messages
- **FATAL**: Fatal error messages

## Architecture

The logger uses a **multi-producer, single-consumer** ring buffer (slick-queue) with a **multi-sink architecture**:

```
[Thread 1] ──┐
[Thread 2] ──┼──► [Lock-Free Queue] ──► [Writer Thread] ──┬──► ConsoleSink
[Thread N] ──┘                                            ├──► FileSink  
                                                          ├──► RotatingFileSink
                                                          ├──► DailyFileSink
                                                          └──► BinarySink
```

The same structure spans processes when the queue is placed in shared memory — the producers
become other processes and the writer thread lives in the collector. See
[Multi-Process Logging](#multi-process-logging-shared-memory).

```
[Process A] ──┐
[Process B] ──┼──► [Shared Memory Queue] ──► [Collector: Writer Thread] ──► Sinks
[Process C] ──┘
```

### Key Design Principles

1. **Single Writer Thread**: One dedicated thread handles all sink operations
2. **Lock-Free Logging**: Caller threads never block on I/O operations  
3. **Flexible Sinks**: Easy to add custom sink implementations
4. **Atomic Operations**: Thread-safe queue and sink management
5. **Position-Independent Entries**: In shared-memory mode, string references are ring indices rather than addresses, so an entry stays meaningful in any attached process

### Deferred Formatting

For optimal performance, the logger defers string formatting to the background thread:

1. **Caller Thread**: Captures the format pointer, source location, and owned copies of any dynamic string data
2. **Lock-Free Queue**: Stores a compact `LogEntry` in the ring buffer with minimal caller-side work
3. **Writer Thread**: Formats the message and writes it to all matching sinks

This approach moves potentially expensive formatting and I/O operations off the critical path, making logging calls extremely fast and suitable for high-frequency logging scenarios.

### Multi-Sink Benefits

- **Simultaneous Output**: Log to console + multiple files at once
- **Different Retention**: Each sink can have its own rotation policy
- **Performance**: Single writer thread efficiently handles all sinks
- **Flexibility**: Mix and match sink types as needed

## Integration

slick-queue is downloaded automatically during the build process from https://github.com/SlickQuant/slick-queue.

## Header-Only Benefits

Being a header-only library provides several advantages:

- **Zero Linking**: No need to link against library files
- **Easy Integration**: Just include the headers in your project
- **Template Optimization**: Compiler can better optimize template instantiations
- **No Binary Dependencies**: No need to distribute or manage .lib/.a files
- **Immediate Usage**: Start logging with a single `#include <slick/logger.hpp>`

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

A sink that writes to a file can inherit `FileSinkBase` instead, which supplies the stream, its buffer, directory creation, and `flush()`. That is what `FileSink` and `BinarySink` are built on.

## Examples

The repository includes comprehensive examples:

- **`logger_example.exe`**: Basic usage with console + file output
- **`multi_sink_example.exe`**: Demonstrates all sink types, rotation, and custom sinks
- **`timestamp_example.exe`**: Demonstrates predefined and custom timestamp formats
- **`multi_process_example.exe`**: Collector and producer roles logging across process boundaries
- **`binary_sink_example.exe`**: Raw binary capture, custom framing, and the `LOG_SINK_*` fast path

## Building Examples/Tests  

If you want to build the provided examples and tests:

```bash
mkdir build
cd build
# Add -DBUILD_SLICK_LOGGER_COLLECTOR=ON to also build the standalone collector
cmake ..
cmake --build . --config Debug

# Run examples
./examples/Debug/logger_example.exe
./examples/Debug/multi_sink_example.exe
./examples/Debug/timestamp_example.exe
./examples/Debug/binary_sink_example.exe

# Multi-process example: run the collector in one terminal and producers in others
./examples/Debug/multi_process_example.exe --collector --name demo_log
./examples/Debug/multi_process_example.exe --producer --name demo_log --tag app1

# Run tests  
./tests/Debug/slick_logger_tests.exe
./tests/Debug/slick_logger_sink_tests.exe
./tests/Debug/slick_logger_timestamp_tests.exe
./tests/Debug/slick_logger_shared_lib_tests.exe
./tests/Debug/slick_logger_shm_tests.exe
```
