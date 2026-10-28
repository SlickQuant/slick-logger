# slick-logger

[![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)](https://en.cppreference.com/w/cpp/20)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![Header-only](https://img.shields.io/badge/header--only-yes-brightgreen.svg)](#installation)
[![Lock-free](https://img.shields.io/badge/concurrency-lock--free-orange.svg)](https://slickquant.github.io/slick-logger/architecture/)
[![CI](https://github.com/SlickQuant/slick-logger/actions/workflows/ci.yml/badge.svg)](https://github.com/SlickQuant/slick-logger/actions/workflows/ci.yml)
[![Documentation](https://github.com/SlickQuant/slick-logger/actions/workflows/documentation.yml/badge.svg)](https://slickquant.github.io/slick-logger/)
[![GitHub release](https://img.shields.io/github/v/release/SlickQuant/slick-logger)](https://github.com/SlickQuant/slick-logger/releases)

<a id="overview" name="overview"></a>

slick-logger is a **header-only C++20** logging library for Windows, Linux, and macOS. `LOG_*`
calls enqueue entries in the calling thread's own ring buffer, and a background writer formats them
and sends them to console, file, or custom sinks. It also supports log rotation, binary payloads,
and shared-memory logging across processes.

[Installation](#installation) | [Quick start](#quick-start) | [Guide](#documentation) | [Build from source](#building)

## Features

- **High Performance**: Asynchronous logging through a lock-free ring per logging thread, so the calling thread does as little as possible and threads never contend
- **Modern Formatting**: Message formatting with C++20 `std::format`, including width, precision and alignment
- **Multi-Sink Architecture**: Log to multiple destinations simultaneously, each with its own level, or address a single sink directly
- **Log Rotation**: Size-based and time-based rotation with configurable retention, for text and encoded files alike
- **Customizable Pattern**: An spdlog-style `set_pattern()` that controls the whole line (field order, widths, alignment and which span is colored), globally or per sink
- **Source Locations**: `LOG_*` macros record the call-site file and line by default, and a runtime switch turns that off
- **Binary Payloads**: Raw byte payloads through the same queue, written verbatim by `BinarySink`
- **Encoded Log Files**: `EncodedFileSink` writes entries in binary and skips formatting, so the writer thread drains them several times faster; `slick_log_decode` renders the file later
- **Shared-Library Redirection**: Plugin or strategy libraries can route their logs into the host application's logger
- **Multi-Process Logging**: Several processes log through shared memory, each thread into a ring of its own, and one collector drains them all
- **Runtime Statistics**: An optional background thread samples throughput, queue fullness and drop counts, and writes them to a CSV
- **Header-Only, Cross-Platform**: Nothing to link, no dependency beyond the standard library and [slick-queue](https://github.com/SlickQuant/slick-queue)

## Performance

Recorded on an AMD Ryzen 9 5900HX, Windows 11, MSVC 19.44 `/O2`, with log output on a local NVMe SSD (2026-10-22):

| Producer-side measurement | Result |
| --- | ---: |
| Literal message, 1 producer | **14.35 million calls/sec** |
| Literal message, 8 producers | **18.91 million calls/sec** |
| Three-argument message, 8 producers | **7.48 million calls/sec** |
| Mean call latency, literal message | **153 ns** |

Call timings exclude the writer's drain and flush time. With 4 producer processes of 2 threads
each, a call took about 185-200 ns with thread rings in shared memory, against 800-950 ns through
the shared queue. [Methodology and the full results](https://github.com/SlickQuant/slick-logger/blob/main/benchmarks/README.md#results)
are in the benchmark README, and [Performance](https://slickquant.github.io/slick-logger/performance/)
explains what is being measured.

By default, a full thread ring spills into the shared rings, which can overwrite their oldest
entries under overload. See [Known limitations](https://slickquant.github.io/slick-logger/limitations/).

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

<a id="usage" name="usage"></a><a id="basic-usage" name="basic-usage"></a>

## Quick start

Use this as the `main.cpp` referenced by your CMake target:

```cpp
#include <slick/logger.hpp>

int main() {
    // Initialize the logger
    slick::logger::Logger::instance().init("app.log", 1024); // queue size

    // Log messages - formatting happens in the background thread
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

By default each entry carries the call-site file and line:

```text
2026-07-01 12:34:56.123456 [INFO] [main.cpp:8] Application started
```

Log levels, source-location switches and the bundled examples are covered in [Getting started](https://slickquant.github.io/slick-logger/getting-started/).

## Documentation

<a id="navigation" name="navigation"></a>

The full guide is published at <https://slickquant.github.io/slick-logger/>. Its sources live in
[`docs/`](docs/) and render on GitHub as well.

<!-- Keep legacy README anchors beside the guide links they belong to. -->

| Guide | Use it for |
| --- | --- |
| <a id="log-macros-and-levels" name="log-macros-and-levels"></a><a id="source-location-logging" name="source-location-logging"></a><a id="log-levels" name="log-levels"></a><a id="examples" name="examples"></a><a id="building-examplestests" name="building-examplestests"></a>[Getting started](https://slickquant.github.io/slick-logger/getting-started/) | Installation, log levels, source locations, examples and tests |
| <a id="advanced-configuration" name="advanced-configuration"></a><a id="per-thread-rings" name="per-thread-rings"></a><a id="sizing-the-string-ring" name="sizing-the-string-ring"></a><a id="lifecycle-and-runtime-controls" name="lifecycle-and-runtime-controls"></a><a id="when-entries-reach-disk" name="when-entries-reach-disk"></a><a id="thread-id-capture" name="thread-id-capture"></a><a id="queue-tuning" name="queue-tuning"></a>[Configuration](https://slickquant.github.io/slick-logger/configuration/) | Sinks, queue sizing, per-thread rings and runtime controls |
| <a id="string-formatting-with-stdformat" name="string-formatting-with-stdformat"></a><a id="numbered-placeholders" name="numbered-placeholders"></a><a id="dynamic-width-and-precision" name="dynamic-width-and-precision"></a><a id="wchar_t-arguments" name="wchar_t-arguments"></a><a id="passing-stdformat_args" name="passing-stdformat_args"></a><a id="log-pattern-formatting" name="log-pattern-formatting"></a><a id="flags" name="flags"></a><a id="width-and-alignment" name="width-and-alignment"></a><a id="coloring-part-of-a-line" name="coloring-part-of-a-line"></a><a id="notes-1" name="notes-1"></a>[Formatting](https://slickquant.github.io/slick-logger/formatting/) | Message fields, line patterns, alignment and color |
| <a id="multi-sink-usage" name="multi-sink-usage"></a><a id="logging-to-specific-sinks" name="logging-to-specific-sinks"></a><a id="dedicated-sinks" name="dedicated-sinks"></a><a id="sink-types" name="sink-types"></a><a id="consolesink" name="consolesink"></a><a id="filesink" name="filesink"></a><a id="rotatingfilesink" name="rotatingfilesink"></a><a id="dailyfilesink" name="dailyfilesink"></a><a id="binarysink" name="binarysink"></a><a id="encodedfilesink" name="encodedfilesink"></a><a id="rotation-configuration" name="rotation-configuration"></a><a id="custom-sinks" name="custom-sinks"></a><a id="rendering-a-whole-line-from-a-custom-sink" name="rendering-a-whole-line-from-a-custom-sink"></a>[Sinks and rotation](https://slickquant.github.io/slick-logger/sinks/) | Console and file output, rotation and custom sinks |
| <a id="binary-logging" name="binary-logging"></a><a id="what-lands-in-the-file" name="what-lands-in-the-file"></a><a id="text-sinks" name="text-sinks"></a><a id="payloads-larger-than-64kb" name="payloads-larger-than-64kb"></a><a id="notes-2" name="notes-2"></a>[Binary and encoded logs](https://slickquant.github.io/slick-logger/binary/) | Raw payload capture, encoded entries and offline decoding |
| <a id="timestamp-formatting" name="timestamp-formatting"></a><a id="custom-timestamper" name="custom-timestamper"></a><a id="built-in-tsc-timestamper" name="built-in-tsc-timestamper"></a>[Timestamps](https://slickquant.github.io/slick-logger/timestamps/) | Timestamp text, custom clocks and the TSC timestamper |
| <a id="runtime-statistics" name="runtime-statistics"></a><a id="sampling-and-reporting-intervals" name="sampling-and-reporting-intervals"></a><a id="what-is-measured" name="what-is-measured"></a><a id="the-csv" name="the-csv"></a><a id="notes" name="notes"></a>[Runtime statistics](https://slickquant.github.io/slick-logger/statistics/) | Throughput, queue pressure, loss counters and CSV output |
| <a id="sharing-the-logger-across-shared-libraries-plugin-strategy-pattern" name="sharing-the-logger-across-shared-libraries-plugin-strategy-pattern"></a><a id="sharing-the-logger-across-shared-libraries-plugin--strategy-pattern" name="sharing-the-logger-across-shared-libraries-plugin--strategy-pattern"></a>[Shared libraries](https://slickquant.github.io/slick-logger/shared-libraries/) | Route plugin logs into the host logger and unload safely |
| <a id="multi-process-logging-shared-memory" name="multi-process-logging-shared-memory"></a><a id="collector" name="collector"></a><a id="producer" name="producer"></a><a id="output" name="output"></a><a id="standalone-collector-executable" name="standalone-collector-executable"></a><a id="configuration" name="configuration"></a><a id="thread-rings-across-processes" name="thread-rings-across-processes"></a><a id="startup-order-and-sizing" name="startup-order-and-sizing"></a><a id="constraints" name="constraints"></a>[Multi-process logging](https://slickquant.github.io/slick-logger/multi-process/) | Shared-memory producers, collectors, sizing and cleanup |
| <a id="architecture" name="architecture"></a><a id="design-principles" name="design-principles"></a><a id="deferred-formatting" name="deferred-formatting"></a><a id="multi-sink-benefits" name="multi-sink-benefits"></a><a id="integration" name="integration"></a><a id="header-only-benefits" name="header-only-benefits"></a>[Architecture](https://slickquant.github.io/slick-logger/architecture/) | Thread rings, shared rings, ordering and deferred formatting |
| <a id="benchmark-highlights" name="benchmark-highlights"></a>[Performance](https://slickquant.github.io/slick-logger/performance/) | Recorded producer timings, methodology and reproduction |
| [Known limitations](https://slickquant.github.io/slick-logger/limitations/) | Loss behavior, payload limits, lifetimes and build compatibility |
| [API reference](https://slickquant.github.io/slick-logger/api/) | Doxygen-generated C++ reference for `src/logger.hpp` |

## Building

```bash
cmake -S . -B build
cmake --build build --config Debug

# Optional extras
cmake -S . -B build -DBUILD_SLICK_LOGGER_COLLECTOR=ON -DBUILD_SLICK_LOGGER_DECODER=ON
```

That builds the examples and the test suite. To run them, see
[Building examples/tests](https://slickquant.github.io/slick-logger/getting-started/#building-examplestests);
for benchmarks, see [BENCHMARK_SETUP.md](BENCHMARK_SETUP.md).

## Building the documentation locally

The published site is MkDocs Material for the narrative docs plus Doxygen for the API reference.

```bash
# narrative docs, with live preview on http://127.0.0.1:8000
pip install -r requirements-docs.txt
mkdocs serve

# API reference (needs doxygen + graphviz on PATH)
cmake -S . -B build
cmake --build build --target slick-logger-docs   # output in doxygen-out/html
```

The `slick-logger-docs` target only appears in a top-level configure, and only when
`find_package(Doxygen)` succeeds; a normal build is unaffected if Doxygen is absent. Pulling
slick-logger in with FetchContent or `add_subdirectory()` leaves the docs targets out, so they
cannot collide with a `docs` target of your own and Doxygen is not searched for during your
configure. A top-level configure also answers to the shorter `docs` alias.

## Continuous integration

- [`ci.yml`](.github/workflows/ci.yml) builds and tests Debug and Release on Windows, Ubuntu and macOS,
  adds a Clang job and an AddressSanitizer/UBSan job, and builds the vcpkg package consumer in both
  classic and manifest mode.
- [`documentation.yml`](.github/workflows/documentation.yml) builds this guide with `mkdocs build
  --strict` and the API reference with Doxygen, then publishes both to GitHub Pages; it also runs a
  Markdown link check and markdownlint.
- [`release.yml`](.github/workflows/release.yml) packages the headers together with slick-queue and
  slick-shm, and builds the standalone collector for each platform.

## Project structure

```text
src/logger.hpp      the library: single header, installed as include/slick/logger.hpp
include/            generated headers (configured from src/ by CMake)
examples/           runnable examples, including binary, statistics and multi-process
tests/              test suite, plus the shared-library and package-consumer fixtures
tools/              slick_log_collector and slick_log_decode
benchmarks/         benchmark sources and the recorded results
docs/               this documentation's sources (MkDocs)
Doxyfile.in         API reference configuration (configured by CMake)
mkdocs.yml          documentation site configuration
```

## License

MIT. See [LICENSE](LICENSE).
