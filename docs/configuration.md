# Configuration

## Advanced Configuration

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
- `enable_stats`: run the statistics thread, default `false` (see [Runtime Statistics](statistics.md#runtime-statistics))
- `stats_file`: where the statistics CSV is written; empty runs the thread for `stats_snapshot()` alone
- `stats_interval_ms`: how often a CSV row is appended and a snapshot published, default `1000`
- `stats_sample_interval_ms`: how often the queue-fullness gauges are sampled between reports, default `10`
- `stats_max_file_size`: roll the CSV to `<stem>.1.csv` at this size, default 16 MB; `0` disables rolling
- `timestamper`: where entry timestamps come from; null keeps the current one, which is `std::chrono::system_clock` unless changed (see [Custom Timestamper](timestamps.md#custom-timestamper))
- `thread_rings`: give every logging thread its own ring, default `true` (see [Per-thread rings](#per-thread-rings))
- `thread_ring_size`: bytes per thread ring, default 4 MB, rounded up to a power of two, at least 4 KB
- `max_thread_rings`: rings before further threads use the shared rings, default `64`
- `writer_poll_interval_us`: how long the writer leaves a busy ring alone after draining it, default `5`
- `thread_ring_full`: what a call does when its thread's ring is full, `RingFullPolicy::Spill` (default) or `RingFullPolicy::Drop`
- `shared_thread_rings`, `shared_thread_ring_size`: a `SharedProducer`'s rings, default 16 of 1 MB (see [Multi-Process Logging](multi-process.md#thread-rings-across-processes))

### Per-thread rings

By default each logging thread writes to a ring of its own, in Local mode and in a `SharedProducer` (see [Thread rings across processes](multi-process.md#thread-rings-across-processes)). A call reserves one record there, with its strings inline, and publishes it with one store. No two producers touch the same cache line. In an A/B test on an 8-core laptop, a call was about 20% cheaper than with the shared rings from one thread, and 3 to 4.5x cheaper from 4 to 8 threads.

What changes compared to the shared rings (`thread_rings = false`, the default in earlier releases):

- By default a full ring spills to the shared rings until the writer catches up, so a burst slows calls down but loses nothing, unless the shared rings fill too and overwrite their oldest entries. With `thread_ring_full = RingFullPolicy::Drop`, a full ring drops the newest entry instead and counts it in `LogStats::entry_loss_count`, which keeps calls cheap under overload.
- One thread's entries stay in order, spilled ones included. Across threads the writer merges them by timestamp, so an entry stamped just before its thread was preempted can land slightly late.
- Each logging thread gets a ring of `thread_ring_size` on its first log call. Its pages are faulted in as it fills, so a thread that logs little uses little memory, but those first-lap faults land on log calls. Call `Logger::instance().prepare_thread()` at thread start to claim the ring and fault it all in up front; it works at any log level, and `init()` does it for its own thread. A thread that exits hands its ring to the next new thread.
- A ring belongs to an OS thread. A thread that logs from the host and from a plugin sharing its logger (`Logger::set_instance()`) uses one ring from both, or the shared rings from both, so its entries stay in order.
- Threads past `max_thread_rings` use the shared rings for the whole run. So do the strings of a record bigger than half a ring, though the record itself stays in the thread's ring.
- String-ring statistics describe the shared string ring only.

To make the shared rings the default again, including for the `init()` overloads that take no `LogConfig`, configure with `-DSLICK_LOGGER_DEFAULT_THREAD_RINGS=OFF` or define `SLICK_LOGGER_DEFAULT_THREAD_RINGS=0` before including the header.

### Sizing the string ring

Dynamic strings, non-literal format strings and binary payloads are copied into the string ring (in shared-memory mode, so are file names and format strings). slick-queue keeps a 16-byte control slot for each *unit* of that ring, and `string_items_per_slot` sets the unit size: every stored string is rounded up to a whole number of units.

| `string_items_per_slot` | Control array for a 16 MB ring | Ring space used by an 11-byte string | Strings a 16 MB ring can hold |
|---:|---:|---:|---:|
| 1 | 256 MB | 11 B | 16.7 M |
| 16 | 16 MB | 16 B | 1 M |
| 32 | 8 MB | 32 B | 512 K |
| **64** (default) | **4 MB** | 64 B | 262 K |

The default of 64 is one cache line, so every string starts on its own line and producer threads never false-share the lines they copy into. With 8 producer threads, that made a log call about 1.8x cheaper than at 16 or 32 (see [benchmarks/README.md](https://github.com/SlickQuant/slick-logger/blob/main/benchmarks/README.md#string-ring-unit-size-string_items_per_slot)). The cost is that a string shorter than 64 bytes still uses 64 bytes, so the ring holds at most `string_buffer_size / 64` strings. If you log many short dynamic strings into a small `string_buffer_size`, lower the value or enlarge the ring. Otherwise a string can be overwritten before the writer thread reads it.

The value is rounded up to a power of two, `0` is treated as `1`, and it is clamped to `string_buffer_size`. `1` gives the byte-granular layout of earlier releases. The `init(path, ...)` and `init(queue_size, ...)` overloads always use the default.

## Lifecycle and Runtime Controls

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
- `flush()`: wait for queued entries to be written and flushed to their sinks, while keeping the logger running
- `shutdown(clear_sinks = true)`: flush, stop the writer thread, and optionally clear sinks
- `reset()`: return the singleton to an uninitialized state; mainly intended for tests
- `prepare_thread()`: claim the calling thread's ring up front (see [Per-thread rings](#per-thread-rings)); returns whether it has one
- `set_level()` / `get_level()`: update or read the global level filter
- `set_timestamper()`: change the clock entries are stamped with; safe while the logger is running (see [Custom Timestamper](timestamps.md#custom-timestamper))
- `clear_sinks()`: remove all currently registered sinks before reconfiguration

Register sinks before `init()`, or between `shutdown()` and the next `init()`. The writer thread reads the sink list without locking, so `add_sink()`, the `add_*_sink()` helpers and `clear_sinks()` must not be called while the logger is running.

Only two sink settings may change while the logger is running: `set_min_level()` and `set_pattern()`, which the writer thread reads atomically. `set_pattern()` must still be called from one thread at a time. Every other sink setting, such as `set_dedicated()` and `set_timestamp_format()`, is read by the writer thread without synchronization, so configure it while the logger is stopped.

### When entries reach disk

The writer thread flushes its sinks once it has drained the queue, not after every entry, so a burst of logging shares a single flush instead of paying a write syscall per line. In practice:

- Once the logger is caught up, everything logged so far is on disk.
- While a backlog is still draining, recent entries may still be sitting in a sink's buffer.
- `flush()` returns only after the writer thread has written and flushed every entry queued before the call. Use it whenever you need that guarantee at a specific point: before unloading a plugin, before inspecting the log file, or at a checkpoint.
- `shutdown()` drains and flushes before the writer thread exits.

`ConsoleSink` follows the same rule. It ends each line with `'\n'` rather than `std::endl`, and `std::cout`/`std::cerr` are flushed with the other sinks once the queue drains. When stdout is redirected to a file or a pipe (CI logs, `docker logs`, process supervisors), a burst reaches the capturing process once the writer catches up, not line by line. `std::cerr` is unbuffered by default, so warnings and errors routed there by `use_stderr_for_errors` still appear immediately.

A custom sink's `flush()` is only ever called on the writer thread, so it does not need its own locking against `write()`.

An idle writer thread parks instead of polling on a timer, so an entry logged into an otherwise idle logger reaches its sink in tens of microseconds rather than waiting out a scheduler tick. Producers only signal a wake-up when the writer is actually parked, which costs the logging path an atomic load and a predictable branch. In `QueueMode::SharedCollector` the collector polls instead, because a producer in another process cannot signal it.

## Thread id capture

`%t` renders the id of the thread that made the `LOG_*` call, not the writer thread that formatted it. The id is the real OS thread id, the one a debugger, `top -H` or ETW shows. It is resolved once per thread and stamped into each entry with a single thread-local read.

This adds a `uint32_t` to `LogEntry`, and that field is reserved in every build: the option costs the capture, never the layout. Processes sharing a segment therefore need not agree on the setting. A producer built with it off publishes entries carrying thread id 0, and a collector reads them like anyone else's.

To drop the per-call capture (one thread-local read on the producing thread) at the cost of `%t`:

```bash
cmake -S . -B build -DSLICK_LOGGER_ENABLE_THREAD_ID=OFF
```

With the capture off, `set_pattern("%t")` throws and names the option, rather than rendering a flat `0` on every line.

## Queue Tuning

Two switches configure the lock-free queues behind the logger. Each is a CMake option, or a macro defined before including `slick/logger.hpp`:

| Option / macro | Default | Effect | Cost when on |
| --- | --- | --- | --- |
| `SLICK_LOGGER_ENABLE_LOSS_DETECTION` | `OFF` / `0` | Counts entries a producer overwrote before the writer read them, reported as `LogStats::entry_loss_count` | Writer thread only: a compare per read and an atomic add per lossy read. Producers pay nothing. |
| `SLICK_LOGGER_ENABLE_CPU_RELAX` | `ON` / `1` | Producers claim slots with a CAS on one shared counter; when threads log at the same instant, the losers retry. This executes a `pause`/`yield` instruction before each retry | One `pause` per lost race; a single producer thread never pays it. Off retries at once, which can shave latency with few producers per core but hammers the contended cache line with many. |

```bash
cmake -S . -B build -DSLICK_LOGGER_ENABLE_LOSS_DETECTION=ON -DSLICK_LOGGER_ENABLE_CPU_RELAX=OFF
```

A project consuming the installed package can set the same variables before `find_package(slick-logger)`.

Both switches change the type of the queues `Logger` holds, so every translation unit, and every shared library sharing a logger through `Logger::set_instance()`, must be built with the same values. `SLICK_LOGGER_MAX_ARGS` has the same rule. Separate processes sharing a segment need not agree, since neither switch changes the shared-memory layout.
