# slick-logger

slick-logger is a **header-only C++20** logging library for Windows, Linux, and macOS. `LOG_*`
calls enqueue entries in the calling thread's own ring buffer, and a background writer formats
them and sends them to console, file, or custom sinks. It also supports log rotation, binary
payloads, and shared-memory logging across processes.

There is nothing to link, and the only dependency beyond the C++20 standard library is
[slick-queue](https://github.com/SlickQuant/slick-queue). Start with
[Getting started](getting-started.md) if the library is not in your build yet.

## Read in this order

| Page | What it covers |
| --- | --- |
| [Getting started](getting-started.md) | Requirements, the three installation routes, the first log call, log levels, source locations, the bundled examples and tests |
| [Configuration](configuration.md) | The `LogConfig` fields, per-thread rings, string-ring sizing, lifecycle and runtime controls, the build-time queue switches |
| [Formatting](formatting.md) | `std::format` message syntax, positional and dynamic fields, `std::format_args`, and the line-pattern language |
| [Sinks](sinks.md) | Several sinks at once, addressing a single sink, every built-in sink, rotation, and writing your own `ISink` |
| [Binary and encoded logs](binary.md) | `BinarySink` payloads and their size cap, `EncodedFileSink`, and the `slick_log_decode` tool |
| [Timestamps](timestamps.md) | How the timestamp text is built, a custom timestamper, and the TSC timestamper |
| [Runtime statistics](statistics.md) | Sampling and reporting, the counters that are published, and the CSV they land in |

## Going further

| Page | What it covers |
| --- | --- |
| [Shared libraries](shared-libraries.md) | Redirecting a plugin's or strategy library's `LOG_*` calls into the host application's logger |
| [Multi-process logging](multi-process.md) | Several processes logging through shared memory into one collector, the standalone collector executable, sizing, and the constraints that come with it |
| [Architecture](architecture.md) | The thread and queue model, deferred formatting, and why the rings are lossy |
| [Performance](performance.md) | Producer-side throughput and call latency, and how the numbers were measured |

## Before you rely on it

A few behaviors are deliberate but can surprise you in production. The shared ring overwrites
its oldest entries instead of applying backpressure, format strings are stored by pointer, and
every process on a shared-memory segment must be built from the same slick-logger version.
[Known limitations](limitations.md) lists them all.

## Elsewhere

- [API reference](https://slickquant.github.io/slick-logger/api/): Doxygen-generated C++ reference for `src/logger.hpp`.
- [Repository README](https://github.com/SlickQuant/slick-logger/blob/main/README.md): overview, installation, build instructions, CI and releases.
- [Benchmarks](https://github.com/SlickQuant/slick-logger/blob/main/benchmarks/README.md): full results, comparisons and methodology.
