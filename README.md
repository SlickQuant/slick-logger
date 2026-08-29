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
- **Macro Fast Path**: Disabled log levels skip argument evaluation before queueing
- **Direct Sink Logging**: Route messages to a named sink or a sink reference when a message should not be broadcast
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
- slick-queue 1.5.0 or newer (multi-process logging relies on its shared-memory support)
- Internet connection for downloading the slick-queue header when it is not already installed

## Installation

### Option 1: Direct Copy
For manual installation, you need both slick-logger and its dependency:

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

The macros use the singleton logger and are filtered before arguments are evaluated. If the current global level rejects a message, expensive arguments in that log call are not computed.

```cpp
using namespace slick::logger;

Logger::instance().set_level(LogLevel::L_WARN);

LOG_DEBUG("expensive value: {}", build_expensive_debug_value()); // not evaluated
LOG_WARN("visible warning");

auto current_level = Logger::instance().get_level();
```

The available levels are `L_TRACE`, `L_DEBUG`, `L_INFO`, `L_WARN`, `L_ERROR`, `L_FATAL`, and `L_OFF`. A log call supports up to `SLICK_LOGGER_MAX_ARGS` format arguments; define that macro before including `slick/logger.hpp` if you need a different limit.

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

Direct sink helpers such as `sink->log_info(...)` route to that sink only and do not automatically attach the caller's source location. Use `LOG_*` macros when you want automatic call-site capture.

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
- `flush()`: wait for queued entries to be written while keeping the logger running
- `shutdown(clear_sinks = true)`: flush, stop the writer thread, and optionally clear sinks
- `reset()`: return the singleton to an uninitialized state; mainly intended for tests
- `set_level()` / `get_level()`: update or read the global level filter
- `clear_sinks()`: remove all currently registered sinks before reconfiguration

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
                                                          └──► DailyFileSink
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

## Examples

The repository includes comprehensive examples:

- **`logger_example.exe`**: Basic usage with console + file output
- **`multi_sink_example.exe`**: Demonstrates all sink types, rotation, and custom sinks
- **`timestamp_example.exe`**: Demonstrates predefined and custom timestamp formats
- **`multi_process_example.exe`**: Collector and producer roles logging across process boundaries

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
