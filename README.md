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
- **Customizable Log Pattern**: spdlog-style `set_pattern()` controls the whole line — field order, widths, column alignment, and which span gets colored — globally or per sink
- **Source Locations by Default**: `LOG_*` macros include the call-site file and line, with runtime controls for basename vs full path
- **Runtime Configuration**: Configure sinks, queue sizes, log level, source-location output, line patterns, and timestamp formats
- **Macro Fast Path**: Disabled log levels skip argument evaluation before queueing, globally and per sink
- **Direct Sink Logging**: Route messages to a named sink or a sink reference when a message should not be broadcast
- **Binary Payloads**: Log raw bytes and write them to a file verbatim with `BinarySink`, encoding entirely under your control
- **Shared-Library Redirection**: Route plugin or strategy-library logs into a host application's logger
- **Multi-Process Logging**: Several processes can log into one shared-memory queue drained by a single collector process
- **Runtime Statistics**: An optional background thread samples throughput, queue fullness and drop counts, and writes them to a CSV
- **Header-Only**: No linking required - just include and use
- **Cross-Platform**: Supports Windows, Linux, and macOS
- **Multi-Threaded**: Safe for concurrent logging from multiple threads
- **C++20**: Utilizes modern C++ features
- **Easy to Use**: Simple macros for logging at different levels

## Requirements

- **C++20 compatible compiler** with `std::format` support (GCC 11+, Clang 14+, MSVC 19.29+)
- CMake 3.20 or higher (for building examples/tests)
- slick-queue 2.1.0 or newer (the string ring uses its `items_per_slot` constructor, and multi-process logging relies on its shared-memory support). The installed CMake package requires this version through `find_dependency`, so an older slick-queue fails at configure time
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
- `string_items_per_slot`: minimum bytes one stored string consumes in the string ring, default `64` (see [Sizing the string ring](#sizing-the-string-ring))
- `include_source_location`: include file and line for `LOG_*` macro calls, default `true`
- `enable_stats`: run the statistics thread, default `false` (see [Runtime Statistics](#runtime-statistics))
- `stats_file`: where the statistics CSV is written; empty runs the thread for `stats_snapshot()` alone
- `stats_interval_ms`: how often a CSV row is appended and a snapshot published, default `1000`
- `stats_sample_interval_ms`: how often the queue-fullness gauges are sampled between reports, default `10`
- `stats_max_file_size`: roll the CSV to `<stem>.1.csv` at this size, default 16 MB; `0` disables rolling

#### Sizing the string ring

Dynamic strings, non-literal format strings and binary payloads are copied into the string ring (in shared-memory mode, so are file names and format strings). slick-queue keeps a 16-byte control slot for each *unit* of that ring, and `string_items_per_slot` sets the unit size: every stored string is rounded up to a whole number of units.

| `string_items_per_slot` | Control array for a 16 MB ring | Ring space used by an 11-byte string | Strings a 16 MB ring can hold |
|---:|---:|---:|---:|
| 1 | 256 MB | 11 B | 16.7 M |
| 16 | 16 MB | 16 B | 1 M |
| 32 | 8 MB | 32 B | 512 K |
| **64** (default) | **4 MB** | 64 B | 262 K |

The default of 64 is one cache line, so every string starts on its own line and producer threads never false-share the lines they copy into. With 8 producer threads, that made a log call about 1.8x cheaper than at 16 or 32 (see [benchmarks/README.md](benchmarks/README.md#string-ring-unit-size-string_items_per_slot)). The cost is that a string shorter than 64 bytes still uses 64 bytes, so the ring holds at most `string_buffer_size / 64` strings. If you log many short dynamic strings into a small `string_buffer_size`, lower the value or enlarge the ring. Otherwise a string can be overwritten before the writer thread reads it.

The value is rounded up to a power of two, `0` is treated as `1`, and it is clamped to `string_buffer_size`. `1` gives the byte-granular layout of earlier releases. The `init(path, ...)` and `init(queue_size, ...)` overloads always use the default.

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

### Runtime Statistics

Both internal rings are lossy: if producers outrun the writer thread, older entries are overwritten. Statistics make that visible before it costs you a log line. A background thread samples the queues and publishes throughput, fullness and drop counts, both as a live snapshot and as rows in a CSV.

It is opt-in, and **your logging threads pay nothing either way**: most figures are derived from cursors the queues already maintain, so no counter is touched when you call `LOG_*`, enabled or not. (Measured: the difference in enqueue throughput between statistics on and off was smaller than the run-to-run spread of repeating the same configuration, so it could not be resolved either way.)

There is one cost, and it is on the *writer* thread rather than yours. String-ring occupancy needs a position that only the writer can read safely, so with statistics enabled it reads the two ring reservation cursors once per drained batch and compares one of them against its own read cursor. Two cursor reads per *batch*, not per entry, and it never inspects entry contents to do it - but not zero, and not something this README will put a number on.

```cpp
using namespace slick::logger;

LogConfig config;
config.sinks.push_back(std::make_shared<FileSink>("app.log"));
config.enable_stats = true;
config.stats_file = "app_stats.csv";
config.stats_interval_ms = 1000;        // one CSV row per second
config.stats_sample_interval_ms = 10;   // sample the gauges 100x per row
config.stats_max_file_size = 16 * 1024 * 1024;

Logger::instance().init(config);

// ... later, from any thread ...
const LogStats stats = Logger::instance().stats_snapshot();
if (stats.entry_queue_pct_max > 80.0) {
    // the queue peaked above 80% this interval - the writer is falling behind
}
```

#### Two cadences, and why peaks matter

Throughput and drop counts are cumulative counters, so they are exact whenever they are read. Queue fullness is not: it is an instantaneous gauge, and a burst that drives the queue to 90% and drains again in 200 ms is simply invisible to a once-per-second read.

So the thread samples the gauges every `stats_sample_interval_ms` and reports the **peak** across each `stats_interval_ms` window:

| Field | Meaning |
| --- | --- |
| `entry_queue_pct` / `string_pct` | a single point sample, taken when the row was written |
| `entry_queue_pct_max` / `string_pct_max` | the peak across the whole interval - **this is the field to alert on** |
| `sample_count` | how many ticks fed those peaks, so you can see the fidelity behind them |

The sample interval is also how quickly the thread notices shutdown, so a long reporting interval never delays `shutdown()` by more than one tick. It is clamped into `[1, stats_interval_ms]`.

#### What is measured

- **Throughput** - `produced_per_sec` is messages entering the queue, `consumed_per_sec` is messages leaving it. In steady state the two match; `consumed` lagging `produced` is what makes the depth grow. Both are computed over the *measured* elapsed time, so a late wake-up on a loaded machine never distorts them.
- **Entry queue fullness** - `entry_queue_depth` is how far the writer thread is behind the producers, against `entry_queue_capacity`.
- **String ring occupancy** - `string_inflight_bytes` is the span between the frontier - the position below which the ring is provably free - and the producers' write cursor. The logger never calls `read()` on the string ring, so there is no read cursor to subtract, and the frontier is deliberately *not* read out of the entries the writer drains. It cannot be: a producer reserves its string bytes while it builds the entry and its entry slot only at the end, so slot order and string order are two independent races and neither implies the other. A producer holding earlier string bytes can take a later slot, so freeing the ring up to a drained entry's own string releases bytes an entry still queued behind it owns - the gauge reads idle over a ring that is not, and the frontier then lurches backwards when that entry finally drains.

  Instead the writer snapshots the entry reservation cursor and *then* the string reservation cursor, and promotes the string half only once its own read cursor has passed the entry half. The order is the proof: every slot below the snapshot had already been taken, and a string is always reserved before its slot, so every one of those strings was reserved before the string cursor was read. When the writer has drained past the entry half, the whole ring below the string half is free - whatever order the producers published in. Between promotions the frontier simply stays put, which is the conservative direction: the span grows and the gauge reads fuller, which is exactly what a writer falling behind means. The frontier is an absolute reserve index, not a position within the ring, so a ring filled to exactly its capacity reads as `capacity` / `100%` instead of aliasing back to zero, and a genuine overrun shows as a distance past capacity that is then clamped. It is seeded at `start()` with the ring cursor as it stands, so there is always a floor to measure from: before the first promotion nothing has been confirmed drained, and the whole span since start really is in flight. A collector replaying a backlog is the exception: `collect_backlog` rewinds its entry reader below the point it attached, so the entries it is about to drain own strings reserved before it existed, and the attach cursor would call every one of them free for the whole replay. Its floor is rewound the same way the reader is - to the oldest position the ring can still hold - so the replay is reported as the in-flight backlog it is. `string_pct_valid` covers the whole row, point value and peak alike; with a string ring present a measurement is always available, and a fully drained queue is reported as a valid zero. The cost is two cursor reads per drained batch on the writer thread - it never inspects entry contents for this - and nothing at all on the producing thread.

  One window stays invisible: a producer that has reserved string bytes but has not yet taken an entry slot appears in neither ordering, so if it is preempted there its bytes are unaccounted for until it publishes. Closing that would mean having producers announce reservations before publishing, which is not worth what it would cost the logging path.
- **String ring pressure** - `string_bytes_per_sec`, `string_turnover_pct` and `string_wraps_per_sec` describe how fast the ring is being recycled. Like `string_inflight_bytes` and `string_bytes_written`, they count ring footprint: each string is included rounded up to `string_items_per_slot`, because that is the space it actually occupies. `string_turnover_pct` is a *rate*, not an occupancy: 100 means the ring turned over exactly once during the interval.
- **Loss** - `entry_loss_count` is entries overwritten before the writer could read them. It reads `0` unless the queues were built with loss-detecting traits, since `slick::queue_traits::enable_loss_detection` defaults to off. `string_loss_count` is structurally always `0`: slick-queue counts losses inside `read()`, and the logger never calls it on the string ring.

#### The CSV

Opened fresh (truncated) on `init()` with a header row, one row appended per interval, and rolled to `<stem>.1.csv` when `stats_max_file_size` is reached. Columns, in order:

```
timestamp,interval_sec,sample_count,
entries_produced,entries_consumed,produced_per_sec,consumed_per_sec,
entry_queue_depth,entry_queue_depth_max,entry_queue_depth_mean,
entry_queue_capacity,entry_queue_pct,entry_queue_pct_max,
string_inflight_bytes,string_inflight_bytes_max,string_buffer_capacity,
string_pct,string_pct_max,string_pct_valid,
string_bytes_written,string_bytes_per_sec,string_turnover_pct,
string_wraps_per_sec,entry_loss_count,string_loss_count
```

Leave `stats_file` empty to run the thread for `stats_snapshot()` alone and write no file. A CSV that cannot be created makes `init()` throw, exactly as an unopenable sink does; a write failure afterwards is reported once to `stderr` and never takes the process down.

#### Notes

- The statistics thread never touches a sink and never calls `log()`, so it cannot perturb what it measures. Sinks remain owned exclusively by the writer thread. It never reads the entry ring either - only cursors and the frontier the writer publishes.
- `stats_snapshot()` returns a copy of an immutable published sample, so it is safe to call concurrently from any number of threads and no reader can observe a half-written one. It is not guaranteed wait-free: the sample lives in a `std::atomic<std::shared_ptr>`, which is lock-free only where the implementation says so (MSVC uses an internal lock), so a caller may briefly contend with the once-per-interval publish. It never touches the logging path.
- `shutdown()` drops the last published sample along with the statistics thread, so `stats_snapshot()` reads as a zeroed `LogStats` once the logger is stopped - and stays zeroed through a re-`init()` with `enable_stats` off, or one that throws because the CSV cannot be created. Read the closing numbers *before* shutting down if you need them.
- If the CSV cannot be created, `init()` throws before anything starts, leaving the logger stopped rather than half-initialized.
- In `QueueMode::SharedProducer` there is no local writer thread, so `entries_consumed`, the depth fields and `consumed_per_sec` are reported as `0` - only the collector process can observe consumption. Produced counts and string-ring rates are still real.

### Log Pattern Formatting

By default a line looks like this, and every field is fixed:

```
2026-09-19 14:02:11.481320 [INFO] [103836:app1] [main.cpp:81] work item 1 of 4
 └ timestamp                └ level  └ pid:tag    └ source      └ message
```

`set_pattern()` replaces that layout with one of your own, using the same flags as spdlog:

```cpp
using namespace slick::logger;

// Every sink, including sinks added later
Logger::instance().set_pattern("%T.%e %^%-5l%$ %-20@ %v");

// 14:02:11.481 INFO  main.cpp:81          work item 1 of 4
// 14:02:11.482 WARN  net/client.cpp:204   retrying
// 14:02:11.483 ERROR db.cpp:7             connection lost
```

Or per sink, which overrides the logger-wide default for that sink only:

```cpp
Logger::instance().add_file_sink("audit.log", "audit");
Logger::instance().get_sink("audit")->set_pattern("%Y-%m-%dT%H:%M:%SZ|%l|%v");
```

It can also be set from `LogConfig`:

```cpp
LogConfig config;
config.sinks.push_back(std::make_shared<FileSink>("app.log"));
config.pattern = "%T.%e [%L] %v";
Logger::instance().init(config);
```

#### Flags

| Flag | Renders | Example |
| --- | --- | --- |
| `%v` | The formatted message | `work item 1 of 4` |
| `%l` / `%L` | Level name / single letter | `INFO` / `I` |
| `%t` | Producing thread id | `31488` |
| `%P` | Producing process id | `103836` |
| `%k` | Producer tag (slick extension) | `app1` |
| `%n` | Sink name | `audit` |
| `%s` / `%#` / `%@` | Source basename / line / `basename:line` | `main.cpp` / `81` / `main.cpp:81` |
| `%Y` `%y` `%m` `%d` | Year (4 / 2 digit), month, day | `2026` `26` `09` `19` |
| `%H` `%I` `%M` `%S` `%p` | Hour (24 / 12), minute, second, AM-PM | `14` `02` `02` `11` `PM` |
| `%e` `%f` `%F` | Milli-, micro-, nanoseconds, zero padded | `481` `481320` `481320700` |
| `%T` `%D` `%c` | `HH:MM:SS`, `MM/DD/YY`, `Www Mmm DD HH:MM:SS YYYY` | `14:02:11` `09/19/26` |
| `%a` `%A` `%b` `%B` | Weekday and month names | `Sat` `Saturday` `Sep` `September` |
| `%E` | Seconds since the epoch | `1789221731` |
| `%^` `%$` | Begin / end the colored span (console only) | |
| `%%` | A literal `%` | |
| `%+` | The built-in layout above, byte for byte | |
| `%q` | The sink's configured `TimestampFormatter` (slick extension) | |

#### Width and alignment

A flag takes an optional minimum width, and a leading `-` left-aligns it. This is what makes columns line up:

```cpp
Logger::instance().set_pattern("%-5l %-24@ %v");
```

A field wider than its width is never truncated, and widths are capped at 64. A width on `%%` is rejected — `%-5%` is far more likely a mistyped flag letter than a deliberately padded percent sign.

#### Coloring part of a line

Without `%^`, a color-enabled `ConsoleSink` colors the whole line, as it always has. With `%^`/`%$` it colors only the span between them:

```cpp
Logger::instance().set_pattern("%T.%e %^%-5l%$ %v");   // only the level is colored
```

The markers are inert on non-console sinks, so the same pattern can be shared between a console sink and a file sink without leaking escape codes into the file.

A `%^` with no matching `%$` colors through to the end of the line and is closed there. spdlog leaves it open, which tints everything the terminal prints afterwards; slick-logger closes it, since "color from here on" can only sensibly mean "for the rest of this line".

#### Notes

- An unknown or unsupported flag throws `std::invalid_argument` from `set_pattern()`, naming the flag, and the sink keeps the layout it already had. Nothing renders as silently empty.
- `%!` (function name), `%g` (full source path), and the elapsed-time flags `%o %i %u %O` are not supported — slick-logger does not capture that data.
- `%n` maps to the **sink** name: slick-logger has one logger, not a registry of named loggers.
- `%t` needs `SLICK_LOGGER_ENABLE_THREAD_ID`, which is on by default. See [Thread Id Capture](#thread-id-capture).
- An empty pattern means "use the built-in layout", not "emit an empty line".
- `set_pattern()` is safe to call while logging is in flight, but not concurrently with itself — it is a configuration call.
- `ISink::pattern()` reports the layout actually in force on that sink, which is not always the string last passed to it: once `set_pattern("")` has cleared a per-sink override, it reads back the logger-wide pattern the sink has fallen back to. `Logger::pattern()` reports the logger-wide default itself, and is empty when there is none.

#### Performance

A pattern is parsed exactly once, inside `set_pattern()`, into a flat list of ops. Rendering walks that list on the writer thread and appends into a buffer the sink reuses, so a line costs no parsing and no allocation beyond the message itself. Date and time flags slice the broken-down time that is computed once per whole second and cached, and the weekday/month names come from static tables — there is no `strftime`, no `put_time` and no locale anywhere on this path. Runs of adjacent date/time flags are fused at compile time, so `%Y-%m-%d %H:%M:%S` becomes a single `memcpy` rather than twelve appends.

Measured on Windows/MSVC `/O2`, minimum of many runs, rendering one line (no I/O):

| Line layout | ns/line | vs 1.3.0 |
| --- | --- | --- |
| Built-in layout, 1.3.0 | 291 | — |
| Built-in layout, now | 155 | **-47%** |
| `%+` | 161 | -45% |
| `%q\|%v` | 101 | -65% |
| `%Y-%m-%d %H:%M:%S.%f [%l] [%s:%#] %v` | 243 | -16% |
| `%T.%e %^%-5l%$ %-20@ %v` | 262 | -10% |
| `[%l] %v` | 106 | -63% |

The built-in layout got faster because the shared renderer builds the line in place: the timestamp and the level no longer become strings of their own, and the line is assembled straight into the sink's reusable buffer instead of a fresh `std::string` returned by value. A spelled-out pattern costs more than the built-in path — the per-op dispatch and, for aligned fields, the padding — but every layout above renders well ahead of what 1.3.0 shipped. On the producing thread the `LOG_*` call itself measured 96.6 → 97.8 ns, the cost of stamping the thread id.

A pattern also pays only for the fields it actually renders:

- **The message.** A pattern that renders neither the message nor a level skips the `std::format` pass altogether, which is the dominant cost of a line with arguments. On an entry with two arguments, `%T|%@` renders in **94 ns** against **705 ns** for `%T|%@|%l` — adding a level flag brings the message pass back, because a message that fails to format is reported at `ERROR` and that is only known by attempting it.
- **The broken-down time.** Only a calendar field (`%Y`, `%H`, `%T`, `%c`, `%a` …) needs one. `%e`, `%f`, `%F` and `%E` are arithmetic on the entry's own timestamp, so a pattern built from those alone never consults the per-second cache at all.

Width costs something either way, but right alignment costs more: `%-8l` appends its padding after a field that is already at the end of the line, while `%8l` has to put the padding in front of it. Measured at roughly 20ns per right-aligned field. Prefer left alignment where the column order allows it.

`%q` delegates to the sink's `TimestampFormatter`, which appends its predefined formats straight into the same buffer — the row above shows what that costs. The exception is `Format::CUSTOM`, which carries the `strftime` cost described below; spell the date out with pattern flags instead of using a custom format string on a hot path.

### Thread Id Capture

`%t` renders the id of the thread that made the `LOG_*` call, not the writer thread that formatted it. The id is the real OS thread id — what a debugger, `top -H` or ETW shows — resolved once per thread and stamped into each entry with a single thread-local read.

This adds a `uint32_t` to `LogEntry`, and that field is reserved in **every** build — the option costs the capture, never the layout. So processes sharing a segment need not agree on the setting: a producer built with it off simply publishes entries carrying thread id 0, and a collector reads them exactly as it reads anyone else's.

To drop the per-call capture — one thread-local read on the producing thread — at the cost of `%t`:

```bash
cmake -S . -B build -DSLICK_LOGGER_ENABLE_THREAD_ID=OFF
```

With the capture off, `set_pattern("%t")` throws and names the option, rather than rendering a flat `0` on every line.

### Timestamp Formatting

The pattern flags above cover date and time directly, so most users will not need this. `TimestampFormatter` remains the way to change the timestamp used by the **built-in layout** (and by `%+` and `%q`).

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

`format_timestamp()` returns a `std::string`; `append_timestamp(out, timestamp_ns)` appends to a string you already own, which is what the log line itself uses so that no timestamp allocates. `max_length()` gives the bound to `reserve()` for.

`Format::CUSTOM` takes any `strftime` format string, plus one extension: `%f` expands to the microseconds within the second, with no leading zeros. A timestamp at 45.001200 seconds renders `%S.%f` as `45.1200`, not `45.001200` — reach for the zero-padded `%f` **pattern** flag above when you need six digits. Every `%f` in the format is expanded, and `%%f` is a literal `%f`. The format is split at those flags once, when it is set, so rendering parses nothing and allocates nothing.

`strftime` itself is the floor, and it is why `Format::CUSTOM` still measures more than an order of magnitude slower per entry than the predefined formats. Measured on Windows/MSVC `/O2`, minimum of many runs, appending one timestamp to a buffer already sized for it: **25 ns** for `WITH_MICROSECONDS` against **590 ns** for `"%Y-%m-%d %H:%M:%S.%f"`. Prefer a predefined format, or the pattern flags above, on a hot logging path.

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
their capacity from its `log_queue_size` / `string_buffer_size` / `string_items_per_slot`; every
later process attaches and **inherits** those settings, so mismatched settings between processes
are harmless.

A collector that attaches to a segment already holding entries replays whatever is still
resident in the ring, so producers can run before any collector exists. Set
`collect_backlog = false` if a restarting collector must not re-emit entries a previous
instance already wrote.

#### Constraints

- **All processes must use the same slick-logger version, the same `SLICK_LOGGER_MAX_ARGS`,
  and the same architecture.** `sizeof(LogEntry)` is recorded in the shared-memory header, so a
  mismatch throws at attach time rather than corrupting data. A string segment created with
  `string_items_per_slot` other than `1` is also refused by slick-logger builds on slick-queue 2.0
  or older, which cannot read that layout.
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

1. **Caller Thread**: Captures the format pointer, source location, thread id, and owned copies of any dynamic string data
2. **Lock-Free Queue**: Stores a compact `LogEntry` in the ring buffer with minimal caller-side work
3. **Writer Thread**: Formats the message *and the line layout*, then writes to all matching sinks

This approach moves potentially expensive formatting and I/O operations off the critical path, making logging calls extremely fast and suitable for high-frequency logging scenarios.

Line patterns follow the same principle: `set_pattern()` parses the pattern once, at configuration time, into a flat list of ops. The caller thread never sees it, and the writer thread renders it by walking that list into a buffer each sink reuses — no parsing and, in steady state, no allocation per line. See [Log Pattern Formatting](#log-pattern-formatting).

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

### Rendering a whole line from a custom sink

`format_log_message()` gives you just the message body. If you want the full line — timestamp, level, source location, and whatever pattern the user configured — call the protected `format_log_entry()` instead. It is the same code `ConsoleSink` and `FileSink` use, so a custom sink honors `set_pattern()` for free:

```cpp
class SyslogSink : public slick::logger::ISink {
public:
    void write(const slick::logger::LogEntry& entry) override {
        // Renders through this sink's pattern, or the built-in layout if none is set
        std::string_view line = format_log_entry(entry);
        ::syslog(LOG_INFO, "%.*s", static_cast<int>(line.size()), line.data());
    }
    void flush() override {}
};
```

Two things to know:

- The returned `std::string_view` points into a buffer the sink reuses, and is only valid until the next `format_log_entry()` call on that sink. Reusing the buffer is what keeps the line assembly allocation-free — copy the view if you need to keep it.
- Pass the optional colour arguments only if your sink writes to a terminal. Omitting them means `%^`/`%$` render as nothing, so a pattern shared with a console sink will not leak escape codes into your output. The first argument is a *function* from level to escape sequence rather than a ready-made string, because an entry whose message cannot be formatted is reported at `ERROR`, and that is only known once rendering has begun:

```cpp
void write(const slick::logger::LogEntry& entry) override {
    std::string_view line = format_log_entry(entry, &my_color_for, "\033[0m");
    ...
}
static std::string_view my_color_for(slick::logger::LogLevel level) noexcept { ... }
```

## Examples

The repository includes comprehensive examples:

- **`logger_example.exe`**: Basic usage with console + file output
- **`multi_sink_example.exe`**: Demonstrates all sink types, rotation, and custom sinks
- **`timestamp_example.exe`**: Demonstrates predefined and custom timestamp formats
- **`multi_process_example.exe`**: Collector and producer roles logging across process boundaries
- **`binary_sink_example.exe`**: Raw binary capture, custom framing, and the `LOG_SINK_*` fast path
- **`statistics_example.exe`**: Live throughput, queue-fullness peaks, and the statistics CSV under load

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
./examples/Debug/statistics_example.exe

# Multi-process example: run the collector in one terminal and producers in others
./examples/Debug/multi_process_example.exe --collector --name demo_log
./examples/Debug/multi_process_example.exe --producer --name demo_log --tag app1

# Run tests  
./tests/Debug/slick_logger_tests.exe
./tests/Debug/slick_logger_sink_tests.exe
./tests/Debug/slick_logger_timestamp_tests.exe
./tests/Debug/slick_logger_statistics_tests.exe
./tests/Debug/slick_logger_shared_lib_tests.exe
./tests/Debug/slick_logger_shm_tests.exe
```
