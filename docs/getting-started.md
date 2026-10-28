# Getting started

Once slick-logger is in your build, [Configuration](configuration.md) lists every option and
[Formatting](formatting.md) covers the format-string syntax.

## Requirements

- A C++20 compiler with `std::format` support (GCC 11+, Clang 14+, MSVC 19.29+)
- CMake 3.20 or higher (for building examples/tests)
- slick-queue 2.3.0 or newer. The string ring uses its `items_per_slot` constructor, and multi-process logging relies on its shared-memory support, including the 2.3.0 guarantee that a queue never unlinks its segment when destroyed. The installed CMake package requires this version through `find_dependency`, so an older slick-queue fails at configure time
- An internet connection to download the slick-queue header when it is not already installed

## Installation

### Option 1: vcpkg (recommended)

vcpkg installs slick-logger and its dependencies for you. In classic mode:

```sh
vcpkg install slick-logger
```

For manifest-mode projects, add a `vcpkg.json` beside your `CMakeLists.txt`:

```json
{
  "dependencies": [
    "slick-logger"
  ]
}
```

Use the installed package in your C++20 CMake project:

```cmake
cmake_minimum_required(VERSION 3.20)
project(your_project LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(slick-logger CONFIG REQUIRED)

add_executable(your_app main.cpp)
target_link_libraries(your_app PRIVATE slick::logger)
```

Configure with the [vcpkg CMake toolchain](https://learn.microsoft.com/en-us/vcpkg/users/buildsystems/cmake-integration), replacing `<path-to-vcpkg>` with your vcpkg installation directory. In manifest mode this also installs the declared dependencies:

```sh
cmake -S . -B build "-DCMAKE_TOOLCHAIN_FILE=<path-to-vcpkg>/scripts/buildsystems/vcpkg.cmake"
cmake --build build --config Release
```

CI builds and runs this package-consumer example in both classic and manifest modes; see [tests/package_consumer](https://github.com/SlickQuant/slick-logger/tree/main/tests/package_consumer).

### Option 2: CMake Integration

CMake handles the slick-queue dependency for you.

#### Using FetchContent (recommended)

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

# Optional: build slick_log_decode, which renders EncodedFileSink files as text (OFF by default).
set(BUILD_SLICK_LOGGER_DECODER ON CACHE BOOL "" FORCE)

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

find_package(slick-logger CONFIG REQUIRED)

add_executable(your_app main.cpp)
target_link_libraries(your_app PRIVATE slick::logger)
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

### Option 3: Direct Copy

The simplest route is the archive attached to a [release](https://github.com/SlickQuant/slick-logger/releases). It already bundles `slick/queue.h` and the `slick/shm/` headers alongside `slick/logger.hpp`, so you only have to unpack it and add `include/` to your include path.

To assemble the headers by hand instead, you need both slick-logger and its dependency:

1. Copy the `include/slick/` directory to your project
2. Download `queue.h` from <https://raw.githubusercontent.com/SlickQuant/slick-queue/main/include/slick/queue.h>
3. Place `queue.h` in your include path or alongside the slick-logger headers
4. Copy the `slick/shm/` headers from [slick-shm](https://github.com/SlickQuant/slick-shm) as well, since slick-queue includes them for its shared-memory support

Your project structure should look like:

```text
your_project/
├── include/
│   ├── slick/
│       └── logger.hpp
│       └── queue.h
│       └── shm/
└── src/
    └── main.cpp
```

The CMake options above also handle dependencies automatically.

## Basic Usage

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

## Log Macros and Levels

Use the level-specific macros for normal application logging:

```cpp
LOG_TRACE("order book depth={}", depth);
LOG_DEBUG("request id={}", request_id);
LOG_INFO("connected to {}", endpoint);
LOG_WARN("retrying after {} ms", delay_ms);
LOG_ERROR("request failed: {}", reason);
LOG_FATAL("unrecoverable error: {}", reason);
```

**Prefer the macros over calling `Logger::instance().log(...)` directly.** A macro tests the level *before* its arguments appear in the expansion, so a filtered-out call computes nothing. A direct call has to evaluate its arguments first and only then finds out that the level rejects the message. The macros also attach the call site; `Logger::log()` passes no file or line, so entries logged that way carry no `file:line`. The direct call is still there for code that needs it, such as forwarding a level chosen at runtime or logging from code with no call site worth recording.

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

To target a single sink, use the `LOG_SINK_*` macros. They apply the same fast path against both the global level and that sink's own minimum level:

```cpp
LOG_SINK_TRACE(sink, "order book depth={}", depth);
LOG_SINK_DEBUG(sink, "request id={}", request_id);
LOG_SINK_INFO(sink, "connected to {}", endpoint);
LOG_SINK_WARN(sink, "retrying after {} ms", delay_ms);
LOG_SINK_ERROR(sink, "request failed: {}", reason);
LOG_SINK_FATAL(sink, "unrecoverable error: {}", reason);
```

`sink` may be an `ISink` reference, a raw pointer, or a smart pointer, and is evaluated exactly once. An empty smart pointer, or a sink that has not been added to a logger, makes the call a no-op. See [Logging to Specific Sinks](sinks.md#logging-to-specific-sinks).

## Source location logging

Source-location logging is on by default for `LOG_*` macros. The output only ever uses the basename (e.g. `main.cpp:8`):

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

For bridge code that receives source information from another logging layer, there is also a direct source-location overload. It copies the `const char*` path before the entry is queued, so dynamic strings are safe, and it extracts the basename itself:

```cpp
std::string path = "C:\\repo\\app\\bridge.cpp";
slick::logger::Logger::instance().log_with_location(
    slick::logger::LogLevel::L_INFO,
    path.c_str(),
    42,
    "bridged message"); // logged as [bridge.cpp:42]
```

Direct sink helpers such as `sink->log_info(...)` route to that sink only and do not attach the caller's source location. Use `LOG_*` macros for automatic call-site capture, or `LOG_SINK_*` when you want that *and* a specific sink.

## Log Levels

- `TRACE`: detailed debug information
- `DEBUG`: general debug information
- `INFO`: informational messages
- `WARN`: warnings
- `ERROR`: errors
- `FATAL`: fatal errors

## Examples

The repository includes these examples:

- `logger_example.exe`: basic usage with console and file output
- `multi_sink_example.exe`: all sink types, rotation, and custom sinks
- `timestamp_example.exe`: predefined and custom timestamp formats, and a custom timestamper
- `multi_process_example.exe`: collector and producer roles logging across process boundaries
- `binary_sink_example.exe`: raw binary capture, custom framing, and the `LOG_SINK_*` fast path
- `statistics_example.exe`: live throughput, queue-fullness peaks, and the statistics CSV under load

## Building examples/tests

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
