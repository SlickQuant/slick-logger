# Sinks

## Multi-sink usage

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

## Logging to specific sinks

You can log to a sink by name or through a reference to it. Each sink also filters by its own minimum log level:

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

Each sink has its own `min_level`, independent of the global logger level, so messages are filtered twice: first by the global level, then by the sink's. Use `sink->set_min_level(LogLevel::L_WARN)` to control what each sink accepts, which lets different sinks run at different verbosity. Raising a sink's `min_level` also drops entries that are already queued but not yet written, because the writer thread re-checks the level when it dispatches. Call `Logger::instance().flush()` first if those entries must survive.

Prefer the `LOG_SINK_*` macros over `sink->log_*(...)`. The direct helpers must evaluate their arguments before the call can decide to drop the message, while the macros check the sink's level first, so a filtered-out call computes nothing. The macros also attach the call site, which the direct helpers cannot:

```cpp
auto audit = Logger::instance().get_sink("audit");
audit->set_min_level(LogLevel::L_WARN);

LOG_SINK_INFO(audit, "{}", build_expensive_report());  // never evaluated
LOG_SINK_ERROR(audit, "failed with {}", code);         // written, with file:line

audit->log_info("{}", build_expensive_report());       // evaluated, then dropped
```

## Dedicated Sinks

Dedicated sinks only receive messages logged directly to them, not broadcast messages from `LOG_*` macros:

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

    // Any sink can be made dedicated, but only while the logger is stopped:
    // the writer thread reads the flag without synchronization
    Logger::instance().shutdown(false);  // stop, keeping the sinks
    auto regular_sink = Logger::instance().get_sink("regular");
    regular_sink->set_dedicated(true);   // Now it's dedicated too
    Logger::instance().init(8192);
    regular_sink->log_warn("This goes to regular.log only");

    Logger::instance().shutdown();
    return 0;
}
```

Typical uses are audit logs for security events that belong only in specific files, metrics that shouldn't clutter the main application log, separate error streams per component, and log types with regulatory requirements.

## Sink types

Every sink is added to the logger with one of the `add_*_sink()` methods. The binary and
encoded ones have [a page of their own](binary.md); the rest are listed here.

### ConsoleSink

Writes to stdout/stderr, with optional ANSI colors per log level. WARN/ERROR/FATAL can go to stderr. Works on Windows, Linux, and macOS.

### FileSink

Basic file logging. It appends to the given file and creates missing directories. All file operations happen on the single writer thread, so it is thread-safe.

Lines are formatted straight into a write cache (256 KiB by default) that goes to the file in one write when it fills, and whenever the writer thread catches up with the queue or `flush()` is called, so an idle logger has everything on disk, as before. On Windows it also writes `\r\n` itself instead of leaving the translation to a text-mode stream, which on MSVC cost more than formatting the line. The file comes out byte for byte as before. Measured on Windows/MSVC `/O2`, 1,000,000 entries drained by the writer thread:

| Line | Before | With the write cache |
| --- | ---: | ---: |
| No arguments | 1,046 ns | **298 ns** |
| Four arguments | 1,731 ns | **518 ns** |

`set_write_cache_size(bytes)` trades that against how much a long burst holds unwritten: `0` hands each line to the file as it comes. Call it before adding the sink. `RotatingFileSink` and `DailyFileSink` share the cache and write it out before each rotation, so a line always lands in the file it was written for.

### RotatingFileSink

Size-based log rotation. The size limit is configurable (default 10MB), only the last N files are kept and the oldest is deleted, and files are named `log.txt`, then `log_1.txt`, `log_2.txt`, and so on. Rotation is thread-safe.

### DailyFileSink

Date-based log rotation. The day's lines go to `filename.log`, which is renamed `filename_YYYY-MM-DD.log` at midnight. With `max_file_size` set, it also rotates within a day. The newest of the day's earlier files is `filename_YYYY-MM-DD.log` and older ones get `_001`, `_002` and so on, keeping `max_files` per date. At midnight the day's last file joins them and the rest shift along. When the sink opens, it renames a file left over from an earlier date the same way.

### BinarySink

Raw byte capture; see [Binary Logging](binary.md#binary-logging). It writes binary payloads byte for byte and adds nothing of its own (no header, framing or separators), so the file's format is whatever you wrote into it. The file is opened with `std::ios::binary`, so `0x0A` bytes survive on Windows. Override `write_payload()` to add framing. The sink is dedicated by default, so it only receives entries addressed to it.

### EncodedFileSink

`EncodedFileSink` stores log entries for later rendering with `slick_log_decode`.
`RotatingEncodedFileSink` and `DailyEncodedFileSink` use the same `RotationConfig` and file
names as their text counterparts. See [Encoded log files](binary.md#encoded-log-files) for
setup, decoding, rotation and measured writer costs.

## Rotation Configuration

`RotatingFileSink`, `DailyFileSink` and their encoded variants (see
[EncodedFileSink](#encodedfilesink)) all take a `RotationConfig`:

```cpp
slick::logger::RotationConfig config;
config.max_file_size = 50 * 1024 * 1024;  // 50MB
config.max_files = 10;                     // keep last 10 files
config.compress_old = false;               // future feature
config.rotation_hour = std::chrono::hours(0); // midnight for daily rotation
```

## Custom Sinks

A custom sink is a class derived from `ISink`:

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
        auto [message, _] = formatted_message(entry);  // std::string_view

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

A sink that writes to a file can inherit `FileSinkBase` instead, which supplies the stream, its buffer, directory creation, and `flush()`. `FileSink` and `BinarySink` are built on it. A text sink can also batch its lines through the base's write cache: `write_line(entry)` renders the entry into the cache, ends it with the platform line ending, and returns the bytes it added. For that the stream must be opened in binary mode, as `FileSink` does. A subclass that also writes `file_stream_` directly should call `write_cached()` first, so its bytes do not land ahead of lines still in the cache.

The `LogEntry` that `write()` receives is the writer thread's own copy of the entry, never the ring slot, so nothing changes it during the call, however far producers have run ahead. The writer reuses that copy for the next entry, so keep a copy rather than the reference.

`formatted_message()` returns the message body and whether it formatted cleanly (`false` means it is the `[FORMAT_ERROR: ...]` text). While the logger dispatches an entry, the message is formatted once and shared by every sink that asks for it. The first sink to ask pays for `std::format`, the rest reuse the result, and a sink that never asks costs nothing. The returned `std::string_view` is valid until the next call on that sink or the next entry; `format_log_message()` returns the same thing as an owned `std::string` if you need to keep it.

### Rendering a whole line from a custom sink

`formatted_message()` gives you just the message body. For the full line (timestamp, level, source location, and whatever pattern the user configured), call the protected `format_log_entry()` instead. `ConsoleSink` and `FileSink` use the same code, so a custom sink honors `set_pattern()` for free:

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

- The returned `std::string_view` points into a buffer the sink reuses, and is only valid until the next `format_log_entry()` call on that sink. Reusing the buffer keeps line assembly allocation-free, so copy the view if you need to keep it.
- Pass the optional colour arguments only if your sink writes to a terminal. Without them `%^`/`%$` render as nothing, so a pattern shared with a console sink will not leak escape codes into your output. The first argument is a *function* from level to escape sequence rather than a ready-made string, because an entry whose message cannot be formatted is reported at `ERROR`, and that is only known once rendering has begun:

```cpp
void write(const slick::logger::LogEntry& entry) override {
    std::string_view line = format_log_entry(entry, &my_color_for, "\033[0m");
    ...
}
static std::string_view my_color_for(slick::logger::LogLevel level) noexcept { ... }
```
