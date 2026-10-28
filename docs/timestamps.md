# Timestamps

## Timestamp formatting

The [pattern flags](formatting.md#flags) cover date and time directly, so most users will not need this. `TimestampFormatter` is still how you change the timestamp used by the built-in layout (and by `%+` and `%q`).

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

The predefined formats are rendered by writing digits straight into a stack buffer, on top of a broken-down time that is computed once per whole second and cached, so a burst of entries within the same second costs no `localtime` call at all. The cache is `thread_local`, so a `TimestampFormatter` may be shared between threads, and every sink on the writer thread shares one `localtime` call per second.

`format_timestamp()` returns a `std::string`. `append_timestamp(out, timestamp_ns)` appends to a string you already own; the log line itself uses it, so no timestamp allocates. `max_length()` gives the bound to `reserve()` for.

`Format::CUSTOM` takes any `strftime` format string, plus one extension: `%f` expands to the microseconds within the second, with no leading zeros. A timestamp at 45.001200 seconds renders `%S.%f` as `45.1200`, not `45.001200`; use the zero-padded `%f` pattern flag when you need six digits. Every `%f` in the format is expanded, and `%%f` is a literal `%f`. The format is split at those flags once, when it is set, so rendering parses nothing and allocates nothing.

`strftime` itself sets the floor, which is why `Format::CUSTOM` still measures more than an order of magnitude slower per entry than the predefined formats. Measured on Windows/MSVC `/O2`, minimum of many runs, appending one timestamp to a buffer already sized for it: 25 ns for `WITH_MICROSECONDS` against 590 ns for `"%Y-%m-%d %H:%M:%S.%f"`. On a hot logging path, prefer a predefined format or the pattern flags.

## Custom timestamper

`TimestampFormatter` and the pattern flags decide how a time is *printed*. The timestamper decides which time is *recorded*: it is read once per entry, on the logging thread, and stored in `LogEntry::timestamp`. It defaults to `std::chrono::system_clock`. Replace it to stamp entries with a cheaper clock (the built-in [`TscTimestamper`](#built-in-tsc-timestamper)), a simulated one (a backtest replaying historical events), or a fixed one in tests.

```cpp
using namespace slick::logger;

// 1. Derive from ITimestamper for a stateful clock
class ReplayClock : public ITimestamper {
public:
    uint64_t now() noexcept override { return time_ns_.load(std::memory_order_relaxed); }
    void advance_to(uint64_t ns) noexcept { time_ns_.store(ns, std::memory_order_relaxed); }
private:
    std::atomic<uint64_t> time_ns_{0};
};
auto replay = std::make_shared<ReplayClock>();
Logger::instance().set_timestamper(replay);

// 2. Or pass any callable - a lambda with captures, a function, a function pointer
Logger::instance().set_timestamper([&feed]() noexcept { return feed.event_time_ns(); });

// 3. Or name a std::chrono-style clock type (any type with a static now())
Logger::instance().set_timestamper<MyTscClock>();

// Or through LogConfig, applied by init() before its first entry
LogConfig config;
config.timestamper = replay;

// nullptr goes back to std::chrono::system_clock
Logger::instance().set_timestamper(nullptr);
```

The contract:

- `now()` returns nanoseconds since the Unix epoch, which is what `LogEntry::timestamp` holds and what the formatters render. A clock on another epoch still works but prints the wrong date: `steady_clock` counts from boot, so its lines show 1970, and `utc_clock` counts leap seconds, so its lines run ahead of the wall clock.
- It must be thread-safe. Every logging thread calls it concurrently, and so does the statistics thread: `LogStats::timestamp_ns` comes from the same timestamper, so statistics rows line up with the log.
- It must not throw. `now()` is `noexcept`, so a wrapped callable that throws terminates the program.
- It is on the hot path, so its cost is added to every log call. On Windows/MSVC `/O2` (minimum of 40 rounds), reading `system_clock` costs about 52 ns, a timestamper returning a stored value about 3.5 ns, and the default path's dispatch adds under 1 ns to `system_clock` itself.

`set_timestamper()` is safe while the logger is running. Logging threads read the timestamper through one atomic pointer, so a swap takes no lock and blocks no one. A thread caught mid-call finishes that one entry on the previous timestamper, which is why a replaced timestamper is kept alive until `reset()` instead of being destroyed on the spot. As with `set_pattern()`, call it from one thread at a time.

The timestamper survives `shutdown()`, and `init(config)` with a null `config.timestamper` keeps the current one, the same rule as an empty `config.pattern`. `reset()` restores `system_clock`. In shared-memory mode each producer process stamps its entries with its own timestamper. `DailyFileSink` still picks the date in its file name from the wall clock.

### Built-in TSC timestamper

`TscTimestamper` reads the CPU's cycle counter (the time stamp counter on x86-64, the generic timer on ARM64) instead of asking the OS for the time, and turns ticks into nanoseconds since the Unix epoch with one multiply. Entries keep full nanosecond resolution.

```cpp
// Construct it before the hot path starts: the constructor calibrates for ~10 ms.
Logger::instance().set_timestamper(std::make_shared<slick::logger::TscTimestamper>());

// Or choose the calibration time and how often it re-anchors to the wall clock
auto tsc = std::make_shared<slick::logger::TscTimestamper>(std::chrono::milliseconds(20),  // calibration
                                                           std::chrono::seconds(1));       // re-anchor interval
```

Measured on the Ryzen 9 5900HX / Windows 11 / MSVC `/O2` machine from the [benchmark highlights](performance.md#benchmark-highlights):

| | `system_clock` | `TscTimestamper` |
| --- | ---: | ---: |
| `now()` through the timestamper interface | 58 ns | **25 ns** |
| `LOG_INFO` with 4 arguments, null sink (min of 15 rounds) | 136 ns | **107 ns** |
| Worst distance from `system_clock` over 5 s | - | 581 ns in the first second, 66 ns after the first re-anchor |

How it stays on the wall clock:

- Calibration: the constructor sleeps for `calibration` and measures the counter's rate against `steady_clock`.
- Re-anchoring: the first `now()` past each `resync_interval` re-anchors to `system_clock` and re-measures the rate over everything since construction, so it gets more precise the longer it runs. Readers never wait for it, because a re-anchor writes a second anchor and switches to it. Both `calibration` and `resync_interval` are at least 1 ms, and a shorter value (0 or negative included) is raised to 1 ms. Without that floor on `resync_interval`, every `now()` would pay for a re-anchor, which takes two clock reads.
- Never backwards: drift found at a re-anchor is slewed away over the next interval rather than stepped, so stamps from one thread never decrease. Only a wall-clock jump larger than a slew can absorb (more than 0.1% of the interval, such as someone setting the clock) is followed by a step, as `system_clock` itself would show it.
- Fallback: where the counter cannot stand in for a clock (x86 without the invariant-TSC CPUID bit, which some virtual machines clear, or a platform with no counter), `uses_cycle_counter()` returns false and `now()` is plain `system_clock`. The counter must also agree across cores, which it does on current x86 and ARM systems. On ARM64 its rate is set by the platform and can be much coarser: 24 MHz on Apple silicon, so ~42 ns steps.
