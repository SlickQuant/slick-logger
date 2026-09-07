// MIT License
//
// Copyright (c) 2025-2026 Slick Quant LLC
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.


@WARNING_MESSAGE@


#pragma once

#include <string>
#include <cstring>
#include <cstddef>
#include <cstdint>
#include <algorithm>
#include <thread>
#include <atomic>
#include <filesystem>
#include <memory>
#include <functional>
#include <iostream>
#include <fstream>
#include <sstream>
#include <chrono>
#include <iomanip>
#include <ctime>
#include <format>
#include <utility>
#include <vector>
#include <span>
#include <ranges>
#include <string_view>
#include <type_traits>
#include <system_error>
#include <slick/queue.hpp>

// For time functions on some platforms
#ifdef _WIN32
#include <time.h>
#endif

// Process id lookup, used to stamp entries in multi-process (shared memory) mode
#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

#define SLICK_LOGGER_VERSION_MAJOR @slick-logger_VERSION_MAJOR@
#define SLICK_LOGGER_VERSION_MINOR @slick-logger_VERSION_MINOR@
#define SLICK_LOGGER_VERSION_PATCH @slick-logger_VERSION_PATCH@
#define SLICK_LOGGER_VERSION "@slick-logger_VERSION_MAJOR@.@slick-logger_VERSION_MINOR@.@slick-logger_VERSION_PATCH@"

#ifndef SLICK_LOGGER_MAX_ARGS
#define SLICK_LOGGER_MAX_ARGS 20
#endif

// Size of the inline per-entry process tag, including the terminating NUL.
// Stored inline rather than in the string ring because the ring is lossy: a tag
// written once at init would be silently overwritten when the ring wraps.
#ifndef SLICK_LOGGER_TAG_SIZE
#define SLICK_LOGGER_TAG_SIZE 16
#endif

#ifndef SLICK_LOGGER_ENABLE_SOURCE_LOCATION
#define SLICK_LOGGER_ENABLE_SOURCE_LOCATION 1
#endif

#ifndef SLICK_LOGGER_FILE_NAME
#if defined(__FILE_NAME__)
#define SLICK_LOGGER_FILE_NAME __FILE_NAME__
#else
#define SLICK_LOGGER_FILE_NAME slick::logger::detail::file_name_from_path(__FILE__)
#endif
#endif

namespace slick::logger {

namespace detail {

inline constexpr const char* file_name_from_path(const char* path) noexcept {
    if (!path) {
        return nullptr;
    }
    const char* file_name = path;
    for (const char* current = path; *current != '\0'; ++current) {
        if (*current == '/' || *current == '\\') {
            file_name = current + 1;
        }
    }
    return file_name;
}

/**
 * @brief Write a value in [0, 99] as two ASCII digits, without division by 10.
 */
inline void write_2_digits(char* out, uint32_t value) noexcept {
    static constexpr char kDigits[201] =
        "00010203040506070809101112131415161718192021222324"
        "25262728293031323334353637383940414243444546474849"
        "50515253545556575859606162636465666768697071727374"
        "75767778798081828384858687888990919293949596979899";
    out[0] = kDigits[value * 2];
    out[1] = kDigits[value * 2 + 1];
}

/**
 * @brief Write microseconds in [0, 999999] as six ASCII digits.
 */
inline void write_6_digits(char* out, uint32_t value) noexcept {
    write_2_digits(out, value / 10000);
    write_2_digits(out + 2, (value / 100) % 100);
    write_2_digits(out + 4, value % 100);
}

/**
 * @brief Length of the "YYYY-MM-DD HH:MM:SS" prefix cached per whole second.
 */
inline constexpr size_t kDateTimeLen = 19;
inline constexpr size_t kTimeOffset = 11;   // index of "HH" within the prefix
inline constexpr size_t kTimeLen = 8;       // "HH:MM:SS"

/**
 * @brief Broken-down time for one whole second, rendered once and reused.
 *
 * Cached per thread rather than per formatter: the contents do not depend on
 * the output format, so every sink writing on the logger's writer thread shares
 * a single localtime() call per second. thread_local keeps format_timestamp()
 * safe to call concurrently on a shared TimestampFormatter.
 */
struct second_cache {
    int64_t seconds = INT64_MIN;              // whole seconds since epoch, or INT64_MIN if empty
    char date_time[kDateTimeLen] = {};        // "YYYY-MM-DD HH:MM:SS"
    std::tm tm = {};                          // for CUSTOM formats, which still need put_time
};

/**
 * @brief Return the cache filled for @p seconds, or nullptr if it cannot be converted.
 */
inline const second_cache* cached_second(int64_t seconds) noexcept {
    thread_local second_cache cache;
    if (cache.seconds != seconds) [[unlikely]] {
        const time_t time_val = static_cast<time_t>(seconds);
        std::tm tm;
    #if defined(_WIN32)
        if (localtime_s(&tm, &time_val) != 0) {
            return nullptr;
        }
    #else
        if (!localtime_r(&time_val, &tm)) {
            return nullptr;
        }
    #endif
        char* out = cache.date_time;
        const uint32_t year = static_cast<uint32_t>(tm.tm_year + 1900);
        write_2_digits(out, year / 100);
        write_2_digits(out + 2, year % 100);
        out[4] = '-';
        write_2_digits(out + 5, static_cast<uint32_t>(tm.tm_mon + 1));
        out[7] = '-';
        write_2_digits(out + 8, static_cast<uint32_t>(tm.tm_mday));
        out[10] = ' ';
        write_2_digits(out + 11, static_cast<uint32_t>(tm.tm_hour));
        out[13] = ':';
        write_2_digits(out + 14, static_cast<uint32_t>(tm.tm_min));
        out[16] = ':';
        write_2_digits(out + 17, static_cast<uint32_t>(tm.tm_sec));
        cache.tm = tm;
        cache.seconds = seconds;
    }
    return &cache;
}

/// Traits of every ring the logger owns. read_last is dead weight for a log
/// queue, so it stays off. Declared here rather than inside Logger so a test
/// can name the exact traits when it attaches to a segment, without the type
/// becoming supported public API.
struct logger_queue_traits : public slick::queue_traits {
    static constexpr bool enable_read_last = false;
};

} // namespace detail

inline constexpr bool has_source_location(const char* file_name, uint32_t line) noexcept {
    return file_name && *file_name != '\0' && line != 0;
}

enum class LogLevel : uint8_t {
    L_TRACE = 0,
    L_DEBUG = 1,
    L_INFO = 2,
    L_WARN = 3,
    L_ERROR = 4,
    L_FATAL = 5,
    L_OFF = 6,
};

inline constexpr const char* to_string(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::L_TRACE: return "TRACE";
        case LogLevel::L_DEBUG: return "DEBUG";
        case LogLevel::L_INFO:  return "INFO";
        case LogLevel::L_WARN:  return "WARN";
        case LogLevel::L_ERROR: return "ERROR";
        case LogLevel::L_FATAL: return "FATAL";
        case LogLevel::L_OFF:   return "OFF";
        default:              return "UNKNOWN";
    }
}

inline LogLevel to_log_level(std::string_view level_str) {
    if (level_str == "TRACE" || level_str == "trace") return LogLevel::L_TRACE;
    if (level_str == "DEBUG" || level_str == "debug") return LogLevel::L_DEBUG;
    if (level_str == "INFO" || level_str == "info")  return LogLevel::L_INFO;
    if (level_str == "WARN" || level_str == "warn")  return LogLevel::L_WARN;
    if (level_str == "ERROR" || level_str == "error") return LogLevel::L_ERROR;
    if (level_str == "FATAL" || level_str == "fatal") return LogLevel::L_FATAL;
    if (level_str == "OFF" || level_str == "off")   return LogLevel::L_OFF;
    throw std::invalid_argument("Invalid log level string: " + std::string(level_str));
}

/**
 * @brief Class to format timestamps in various formats
 */
class TimestampFormatter {
public:
    enum class Format {
        DEFAULT,          // 2024-08-26 15:30:45
        WITH_MICROSECONDS, // 2024-08-26 15:30:45.123456
        WITH_MILLISECONDS, // 2024-08-26 15:30:45.123
        ISO8601,          // 2024-08-26T15:30:45.123456Z
        TIME_ONLY,        // 15:30:45.123456
        CUSTOM            // User-defined format
    };

    TimestampFormatter(Format fmt = Format::WITH_MICROSECONDS) : format_(fmt) {}
    
    TimestampFormatter(const std::string& custom_format) 
        : format_(Format::CUSTOM), custom_format_(custom_format) {}

    std::string format_timestamp(uint64_t timestamp_ns) const {
        using namespace detail;

        // Whole seconds select the cached "YYYY-MM-DD HH:MM:SS" prefix; only the
        // sub-second digits have to be written per call.
        const int64_t seconds = static_cast<int64_t>(timestamp_ns / 1000000000ULL);
        const uint32_t us = static_cast<uint32_t>((timestamp_ns / 1000ULL) % 1000000ULL);

        const second_cache* cache = cached_second(seconds);
        if (!cache) [[unlikely]] {
            return "1970-01-01 00:00:00.000000"; // fallback timestamp
        }

        // Longest output is ISO8601: "YYYY-MM-DDTHH:MM:SS.ffffffZ" (27 chars).
        char buf[32];
        switch (format_) {
        case Format::DEFAULT:
            return std::string(cache->date_time, kDateTimeLen);

        case Format::WITH_MICROSECONDS:
            std::memcpy(buf, cache->date_time, kDateTimeLen);
            buf[19] = '.';
            write_6_digits(buf + 20, us);
            return std::string(buf, 26);

        case Format::WITH_MILLISECONDS:
            std::memcpy(buf, cache->date_time, kDateTimeLen);
            buf[19] = '.';
            write_2_digits(buf + 20, us / 10000);
            buf[22] = static_cast<char>('0' + (us / 1000) % 10);
            return std::string(buf, 23);

        case Format::ISO8601:
            std::memcpy(buf, cache->date_time, kDateTimeLen);
            buf[10] = 'T';
            buf[19] = '.';
            write_6_digits(buf + 20, us);
            buf[26] = 'Z';
            return std::string(buf, 27);

        case Format::TIME_ONLY:
            std::memcpy(buf, cache->date_time + kTimeOffset, kTimeLen);
            buf[8] = '.';
            write_6_digits(buf + 9, us);
            return std::string(buf, 15);

        case Format::CUSTOM:
            if (!custom_format_.empty()) {
                // %f placeholder uses unpadded microseconds, as today
                std::ostringstream oss;
                std::string format = custom_format_;
                size_t pos = format.find("%f");
                if (pos != std::string::npos)
                    format.replace(pos, 2, std::to_string(us));
                oss << std::put_time(&cache->tm, format.c_str());
                return oss.str();
            }
            return std::string(cache->date_time, kDateTimeLen);
        }
        return {};
    }

private:
    Format format_;
    std::string custom_format_;
};

enum class ArgType : uint8_t {
    BOOL,
    CHAR,
    U_CHAR,
    WCHAR,
    INT8_T,
    INT16_T,
    INT32_T,
    INT64_T,
    UINT8_T,
    UINT16_T,
    UINT32_T,
    UINT64_T,
    FLOAT,
    DOUBLE,
    PTR,             // pointer types
    STRING_LITERAL,  // const char* - safe to store pointer
    STRING_DYNAMIC,  // std::string - stored in separate queue
    // New values must be appended here. Shared-memory producers and collectors
    // can be built against different revisions of this header, and they agree on
    // argument types by numeric value.
    BLOB             // raw bytes - copied into the string queue, length is authoritative
};

/**
 * @brief Largest payload the string ring accepts in a single reservation
 *
 * slick-queue packs the reservation size into 16 bits, so a longer reservation
 * would corrupt the write cursor. Strings and binary payloads are truncated to
 * this length instead.
 *
 * To log more than this, pass the payload as several arguments of ONE log call:
 *
 * @code
 * LOG_SINK_INFO(sink, "{}{}", as_binary(p, kMaxPayloadBytes),
 *                             as_binary(p + kMaxPayloadBytes, rest));
 * @endcode
 *
 * A BinarySink writes an entry's payloads back to back with nothing between
 * them, so the file is byte-identical to the unsplit buffer. Spreading the
 * chunks over separate log calls does NOT work: the queue is multi-producer, so
 * another thread's entry can land between them and interleave the two payloads.
 * One entry is the unit of atomicity, which caps a payload at
 * SLICK_LOGGER_MAX_ARGS * kMaxPayloadBytes.
 */
inline constexpr uint32_t kMaxPayloadBytes = 65534;

/**
 * @brief Non-owning view over raw bytes to be logged verbatim
 *
 * Build one with as_binary(). The bytes are copied into the logger's string ring
 * while the LOG_* call runs, so the source buffer only has to stay alive for the
 * duration of that call. BinarySink writes them out untouched; text sinks render
 * them as hex.
 */
struct BinaryView {
    const std::byte* data = nullptr;
    uint32_t size = 0;
};

/**
 * @brief View raw bytes as a loggable binary payload
 * @param data Start of the payload
 * @param size Payload length in bytes; truncated to kMaxPayloadBytes
 */
inline BinaryView as_binary(const void* data, size_t size) noexcept {
    if (!data || size == 0) {
        return {};
    }
    return BinaryView{static_cast<const std::byte*>(data),
                      static_cast<uint32_t>(std::min<size_t>(size, kMaxPayloadBytes))};
}

/**
 * @brief View a contiguous range of trivially copyable elements as a binary payload
 *
 * Covers std::vector, std::array, std::span and C arrays. The element bytes are
 * taken as-is, so padding inside the element type is logged too.
 */
template<typename R>
    requires std::ranges::contiguous_range<R> &&
             std::is_trivially_copyable_v<std::ranges::range_value_t<R>>
inline BinaryView as_binary(const R& range) noexcept {
    return as_binary(std::ranges::data(range),
                     std::ranges::size(range) * sizeof(std::ranges::range_value_t<R>));
}

// Whether `_Float128` is usable as a type name in C++.
//
// It is a distinct type from `__float128` on GCC, and libstdc++ can hand it to a
// format-arg visitor, so it cannot simply be dropped. It needs its own feature
// test though: `__SIZEOF_FLOAT128__` says the target has a 128-bit float, not
// that this spelling exists. Clang defines that macro on x86-64 yet has no
// `_Float128` in C++ mode, and GCC only accepts it in C++ from version 13.
#ifndef SLICK_LOGGER_HAS_FLOAT128_T
#  if defined(__STDCPP_FLOAT128_T__) || \
      (defined(__GNUC__) && !defined(__clang__) && !defined(__INTEL_COMPILER) && __GNUC__ >= 13)
#    define SLICK_LOGGER_HAS_FLOAT128_T 1
#  else
#    define SLICK_LOGGER_HAS_FLOAT128_T 0
#  endif
#endif

// True for the target's 128-bit binary floating point types, and false (rather
// than ill-formed) wherever they do not exist. Keeping the preprocessor here
// instead of inside the if-constexpr chain matters: an ill-formed condition
// there stops the chain from discarding later branches, which is what made the
// `_Float128` spelling break every translation unit under clang.
template<typename T>
inline constexpr bool is_float128_v =
#if defined(__SIZEOF_FLOAT128__)
    std::is_same_v<T, __float128>
#  if SLICK_LOGGER_HAS_FLOAT128_T
    || std::is_same_v<T, _Float128>
#  endif
    ;
#else
    false;
#endif

// True when Args... is exactly one type convertible to std::format_args —
// used to detect the pre-built format_args overload in log_to_sink.
// std::make_format_args() returns an implementation-defined store type
// (e.g. std::_Format_arg_store on MSVC) that converts to std::format_args,
// so we check convertibility rather than exact type identity.
template<typename... Args>
inline constexpr bool is_single_format_args_v =
    sizeof...(Args) == 1 &&
    (std::is_convertible_v<std::decay_t<Args>, std::format_args> || ...);

/**
 * @brief Where a Logger's ring buffers live and what role it plays
 *
 * Local            - process-private heap queues; the logger owns a writer thread and sinks.
 * SharedProducer   - queues live in named shared memory; the logger only enqueues entries.
 *                    It has no writer thread and no sinks; a collector process owns those.
 * SharedCollector  - queues live in named shared memory; this logger owns the writer thread
 *                    and the sinks, and drains entries produced by every attached process
 *                    (including its own).
 */
enum class QueueMode : uint8_t {
    Local,
    SharedProducer,
    SharedCollector
};

/**
 * @brief Per-entry flags stored in LogEntry::flags
 */
// String references in this entry are ring-buffer indices, not addresses.
// Set by producers whose queues live in shared memory, because the segment maps
// at a different base address in every process.
inline constexpr uint8_t kEntryOffsets = 0x01;
// The entry carries a valid source file/line. Needed as an explicit flag because
// ring index 0 is a legitimate string reference and cannot double as a null sentinel.
inline constexpr uint8_t kEntryHasSourceLocation = 0x02;

#pragma pack(push, 1)
/**
 * @brief Reference to a string, either by address or by ring-buffer index
 *
 * In Local mode `ptr` addresses either a string literal in the caller's read-only
 * data or a copy inside the process-private string ring. In shared-memory mode
 * `offset` holds the slick-queue reserve index of a copy inside the shared string
 * ring, which the collector resolves against its own mapping of that segment.
 */
struct StringRef {
    union {
        const char* ptr;    // Pointer to string data (entry without kEntryOffsets)
        uint64_t offset;    // String ring reserve index (entry with kEntryOffsets)
    };
    uint32_t length;    // String length; 0 means "NUL-terminated, length unknown"

    // Deliberately left as an aggregate with no user-provided constructors:
    // LogArgument stores a StringRef inside a union, and any non-trivial default
    // constructor here would delete that union's (and therefore LogEntry's)
    // default constructor. Build one with braces, e.g. StringRef{"text"}.
};

struct LogArgument {
    ArgType type;
    union {
        bool b;
        char c;
        unsigned char uc;
        wchar_t wc;
        int8_t i8;
        int16_t i16;
        int32_t i32;
        int64_t i64;
        uint8_t u8;
        uint16_t u16;
        uint32_t u32;
        uint64_t u64;
        float f;
        double d;
        void* ptr;             // For pointer types
        const char* literal_ptr;  // For string literals
        StringRef dynamic_str;    // For dynamic strings
    } value;
};

struct LogEntry {
    StringRef format; // Format string
    uint64_t timestamp; // nanoseconds since epoch
    StringRef file{}; // Source file name captured by LOG_* macros
    uint32_t pid = 0; // Producing process id; 0 in single-process (Local) mode
    uint32_t line = 0; // Source line captured by LOG_* macros
    int sink_index = -1; // Optional sink index, logged by that sink only
    LogLevel level;
    uint8_t flags = 0; // kEntryOffsets / kEntryHasSourceLocation
    uint8_t arg_count = 0; // Number of arguments
    char tag[SLICK_LOGGER_TAG_SIZE] = {}; // NUL-padded producer tag; empty in Local mode
    LogArgument args[SLICK_LOGGER_MAX_ARGS];

    // Sinks always receive entries whose string references have been resolved to
    // addresses, so these accessors are safe from any sink implementation.
    const char* format_ptr() const noexcept { return format.ptr; }
    const char* file_name() const noexcept { return file.ptr; }
};
#pragma pack(pop)

inline constexpr bool has_source_location(const LogEntry& entry) noexcept {
    return (entry.flags & kEntryHasSourceLocation) != 0;
}

class Logger;

class ISink {
public:
    ISink(std::string&& name = "") : name_(std::move(name)) {}
    virtual ~ISink() = default;
    virtual void write(const LogEntry& entry) = 0;
    virtual void flush() = 0;

    void set_min_level(LogLevel level) noexcept { min_level_.store(level, std::memory_order_release); }
    LogLevel min_level() const noexcept { return min_level_.load(std::memory_order_relaxed); }

    /**
     * @brief Fast-path check for the LOG_SINK_* macros
     * @param logger The logger the entry will be handed to
     *
     * True when an entry at this level, targeted at this sink, would actually be
     * written. Checks the sink's own minimum level before consulting the logger,
     * so a filtered-out call never touches its arguments.
     *
     * The index_ test is not an optimization: an unregistered sink has index -1,
     * and -1 means "broadcast to every sink" further down the pipeline.
     *
     * Callers that go on to log should pass the same Logger reference they will
     * log through, as the macros do. Resolving Logger::instance() separately for
     * the check and the call costs a second atomic load, and lets a set_instance()
     * landing in between test one logger and write to another.
     */
    bool should_log(const Logger& logger, LogLevel level) const noexcept;

    /// Convenience overload resolving Logger::instance() itself.
    bool should_log(LogLevel level) const noexcept;

    /**
     * @brief Set whether this sink is dedicated (logs only its own entries)
     * @param dedicated True to make the sink dedicated, false otherwise
     */
    void set_dedicated(bool dedicated) noexcept { dedicated_ = dedicated; }

    /**
     * @brief Check if this sink is dedicated (logs only its own entries)
     * @return True if the sink is dedicated, false otherwise
     */
    bool is_dedicated() const noexcept { return dedicated_; }

    /**
     * @brief Log a message with a specific log level and format to this sink only
     * @param level LogLevel of the message
     * @param format Format string (printf-style)
     * @param args Arguments for the format string
     */
    template<typename FormatT, typename... Args>
    void log(LogLevel level, FormatT&& format, Args&&... args);

    template<typename FormatT, typename... Args>
    void log_trace(FormatT&& format, Args&&... args);

    template<typename FormatT, typename... Args>
    void log_debug(FormatT&& format, Args&&... args);

    template<typename FormatT, typename... Args>
    void log_info(FormatT&& format, Args&&... args);

    template<typename FormatT, typename... Args>
    void log_warn(FormatT&& format, Args&&... args);

    template<typename FormatT, typename... Args>
    void log_error(FormatT&& format, Args&&... args);

    template<typename FormatT, typename... Args>
    void log_fatal(FormatT&& format, Args&&... args);

    const std::string_view name() const noexcept { return name_; }

    int index() const noexcept { return index_; }
    void set_index(int idx) noexcept { index_ = idx; }
protected:
    std::pair<std::string, bool> format_log_message(const LogEntry& entry);

protected:
    std::string name_;
    int index_ = -1; // Index assigned by Logger when added
    // Atomic because the LOG_SINK_* macros read it from producer threads while
    // set_min_level() may run concurrently on another. LogLevel is uint8_t-backed,
    // so this stays lock-free.
    std::atomic<LogLevel> min_level_{LogLevel::L_TRACE}; // Minimum level for this sink
    bool dedicated_ = false; // Whether this sink is dedicated (logs only its own entries)
};

namespace detail {

// Normalize the many ways a sink is held into a plain pointer, so one macro
// accepts a reference, a raw pointer, or a smart pointer. A pointer (rather
// than a reference) lets the caller null-check an empty smart pointer.
inline ISink* sink_ptr(ISink& sink) noexcept { return &sink; }
inline ISink* sink_ptr(ISink* sink) noexcept { return sink; }
template<typename T>
inline ISink* sink_ptr(const std::shared_ptr<T>& sink) noexcept { return sink.get(); }
template<typename T, typename D>
inline ISink* sink_ptr(const std::unique_ptr<T, D>& sink) noexcept { return sink.get(); }

} // namespace detail

struct RotationConfig {
    size_t max_file_size = 10 * 1024 * 1024; // 10MB
    size_t max_files = 5;
    bool compress_old = false;
    std::chrono::hours rotation_hour = std::chrono::hours(0); // Daily at midnight
};

class ConsoleSink : public ISink {
public:
    explicit ConsoleSink(bool use_colors = true, bool use_stderr_for_errors = true,
                         TimestampFormatter::Format timestamp_format = TimestampFormatter::Format::WITH_MICROSECONDS,
                         std::string&& name = "");
    
    explicit ConsoleSink(const std::string& custom_timestamp_format, bool use_colors = true, 
                        bool use_stderr_for_errors = true, std::string&& name = "");

    void write(const LogEntry& entry) override;
    void flush() override;

private:
    std::string format_log_entry(const LogEntry& entry);
    std::string get_color_code(LogLevel level);
    std::string get_reset_code();
    
    bool use_colors_;
    bool use_stderr_for_errors_;
    TimestampFormatter timestamp_formatter_;
};

/**
 * @brief Shared file-stream plumbing for sinks that write to a file
 *
 * Owns the stream, its explicit buffer, and the path. Says nothing about what
 * goes into the file, which is what lets FileSink (text) and BinarySink (raw
 * bytes) share it.
 */
class FileSinkBase : public ISink {
public:
    std::string file_path() const { return file_path_.string(); }

    void flush() override;

protected:
    /**
     * @brief Open path, throwing if it cannot be opened
     * @param mode Open mode; binary sinks must include std::ios::binary
     */
    FileSinkBase(const std::filesystem::path& file_path, std::ios::openmode mode, std::string&& name);

    /**
     * @brief Open file_stream_ on path, creating any missing parent directories first.
     *
     * Callers check file_stream_ afterwards to report failures in their own terms.
     * Directory creation is best-effort: if it fails the open fails too, which the
     * caller already handles.
     */
    void open_stream(const std::filesystem::path& path, std::ios::openmode mode);

    /// Size of the explicit stream buffer installed by open_stream().
    static constexpr size_t kStreamBufferSize = 64 * 1024;

    std::filesystem::path file_path_;
    std::ofstream file_stream_;
    /// Backing store for file_stream_'s buffer. Held by the sink so it outlives
    /// every open/close cycle; pubsetbuf() does not take ownership.
    std::vector<char> stream_buffer_;
};

class FileSink : public FileSinkBase {
public:
    explicit FileSink(const std::filesystem::path& file_path,
                      TimestampFormatter::Format timestamp_format = TimestampFormatter::Format::WITH_MICROSECONDS,
                      std::string&& name = "");

    explicit FileSink(const std::filesystem::path& file_path, const std::string& custom_timestamp_format, std::string&& name = "");

    void write(const LogEntry& entry) override;

protected:
    std::string format_log_entry(const LogEntry& entry);

    TimestampFormatter timestamp_formatter_;
};

class RotatingFileSink : public FileSink {
public:
    RotatingFileSink(const std::filesystem::path& base_path, const RotationConfig& config,
                    TimestampFormatter::Format timestamp_format = TimestampFormatter::Format::WITH_MICROSECONDS,
                    std::string&& name = "");
    
    RotatingFileSink(const std::filesystem::path& base_path, const RotationConfig& config,
                    const std::string& custom_timestamp_format, std::string&& name = "");
    
    void write(const LogEntry& entry) override;

private:
    void check_rotation();
    void rotate_files();
    std::filesystem::path get_rotated_filename(size_t index);
    
    RotationConfig config_;
    std::filesystem::path base_path_;
    size_t current_file_size_;
};

class DailyFileSink : public FileSink {
public:
    DailyFileSink(const std::filesystem::path& base_path, const RotationConfig& config,
                 TimestampFormatter::Format timestamp_format = TimestampFormatter::Format::WITH_MICROSECONDS,
                 std::string&& name = "");

    DailyFileSink(const std::filesystem::path& base_path, const RotationConfig& config,
                 const std::string& custom_timestamp_format, std::string&& name = "");

    void write(const LogEntry& entry) override;

protected:
    void check_rotation();
    virtual std::string get_date_string() const;
    void rotate_daily_files();
    void rotate_files_for_date(const std::string& date);

    std::filesystem::path get_daily_filename() const;
    std::filesystem::path get_dated_filename(const std::string& date) const;
    std::filesystem::path get_dated_indexed_filename(const std::string& date, size_t index) const;

    RotationConfig config_;
    std::filesystem::path base_path_;
    std::string current_date_;
    size_t current_file_size_;
};

/**
 * @brief Writes raw binary payloads to a file, byte for byte
 *
 * Every BinaryView argument of an entry is written out verbatim, in the order it
 * was logged. Nothing else about the entry reaches the file - no timestamp, no
 * level, no format string, no separators, no framing. The file is exactly the
 * concatenation of what the caller encoded, so the encoding (and any framing
 * needed to read it back) is entirely the caller's to define.
 *
 * @code
 * auto capture = Logger::instance().add_binary_sink("md.bin", "capture");
 * LOG_SINK_INFO(capture, "{}", slick::logger::as_binary(&msg, sizeof(msg)));
 * @endcode
 *
 * Entries carrying no binary argument produce no output. The sink is dedicated
 * by default so broadcast text entries, which it could not write anyway, never
 * reach it; call set_dedicated(false) to also receive them.
 *
 * Payloads are capped at kMaxPayloadBytes (see as_binary). Split larger records
 * across several calls.
 */
class BinarySink : public FileSinkBase {
public:
    explicit BinarySink(const std::filesystem::path& file_path, std::string&& name = "");

    void write(const LogEntry& entry) override;

    /// Bytes this sink has written since it was constructed.
    /// Safe to poll from any thread while logging is in flight.
    size_t bytes_written() const noexcept { return bytes_written_.load(std::memory_order_relaxed); }

protected:
    /**
     * @brief Write one payload to the stream
     *
     * The extension point for framing: override to prefix a length, a header, or
     * a record index. Called once per binary argument, on the writer thread only.
     */
    virtual void write_payload(const std::byte* data, size_t size);

    // Written on the writer thread, but bytes_written() is public and callers
    // poll it while logging is in flight. Relaxed throughout: it is a progress
    // counter that orders nothing else.
    std::atomic<size_t> bytes_written_{0};
};

/**
 * @brief Configuration struct for initializing the logger
 */
struct LogConfig {
    std::vector<std::shared_ptr<ISink>> sinks;
    LogLevel min_level = LogLevel::L_TRACE;
    size_t log_queue_size = 65536;
    size_t string_buffer_size = 1 << 24; // 16MB
    bool include_source_location = true;

    // ---- Multi-process (shared memory) settings; ignored when mode is Local ----

    /// Where the ring buffers live and what role this logger plays.
    QueueMode mode = QueueMode::Local;
    /// Name of the shared memory segment. Required when mode is not Local.
    /// Must be 1-24 characters of [A-Za-z0-9_]; a "_str" suffixed companion
    /// segment holds the string ring, and the combined name has to stay within
    /// the shortest platform limit (31 characters on macOS).
    std::string shared_memory_name;
    /// Optional short label identifying this process in the output, e.g. "feed".
    /// Truncated to SLICK_LOGGER_TAG_SIZE - 1 characters.
    std::string process_tag;
    /// Collector only. When true, a collector attaching to a segment that already
    /// holds entries picks up everything still resident in the ring instead of
    /// starting at the current write cursor. This is what makes it safe to start
    /// producers before the collector. Set false when a restarting collector must
    /// not re-emit entries a previous instance already wrote.
    bool collect_backlog = true;
    /// Collector only. A producer that is killed between reserve() and publish()
    /// leaves a hole that would stall the collector forever. After this long with
    /// no progress while entries are known to be reserved ahead, the collector
    /// skips the stalled slot. Zero disables the recovery.
    uint32_t stalled_entry_timeout_ms = 5000;
};

/**
 * @brief Singleton Logger class
 */
class Logger {
public:
    static Logger& instance();

    /**
     * @brief Initialize the logger with a log file path
     * @param log_file Path to the log file
     * @param log_queue_size Size of the internal log queue (must be power of 2, default 65536)
     * @param string_buffer_size Size of the internal string buffer (must be power of 2, default 16777216)
     */
    void init(const std::filesystem::path& log_file, size_t log_queue_size = 65536, size_t string_buffer_size = 1 << 24);

    /**
     * @brief Initialize the logger with a configuration struct
     * @param config LogConfig struct with sinks and settings
     *
     * Also the entry point for multi-process logging: set config.mode to
     * QueueMode::SharedCollector in the process that owns the sinks and
     * QueueMode::SharedProducer in every process that only emits log entries,
     * with the same config.shared_memory_name in all of them. Startup order does
     * not matter; whichever process gets there first creates the segments.
     */
    void init(const LogConfig& config);

    /**
     * @brief Get the queue mode this logger was initialized with
     */
    QueueMode mode() const noexcept { return mode_; }

    /**
     * @brief Get the shared memory segment name, empty unless mode() is a shared role
     */
    const std::string& shared_memory_name() const noexcept { return shm_name_; }

    /**
     * @brief Initialize logger with pre-added sinks - sinks should be added before calling this
     * @param queue_size Size of the internal log queue (must be power of 2, default 65536)
     * @param string_buffer_size Size of the internal string buffer (must be power of 2, default 16777216)
     */
    void init(size_t queue_size = 65536, size_t string_buffer_size = 1 << 24);
    
    /**
     * @brief Add a log sink
     * @param sink Shared pointer to a sink implementing ISink interface
     */
    void add_sink(std::shared_ptr<ISink> sink);

    /** 
     * @brief Clear all existing sinks
     */
    void clear_sinks();
    
    /**
     * @brief Add a console sink with optional color and error stream settings
     * @param use_colors Whether to use colors in console output (default true)
     * @param use_stderr_for_errors Whether to send warnings and errors to stderr (default true)
     * @param name Optional name for the sink
     */
    void add_console_sink(bool use_colors = true, bool use_stderr_for_errors = true, std::string&& name = "");

    /**
     * @brief Add a console sink with custom timestamp format and optional color and error stream settings
     * @param timestamp_format Predefined timestamp format enum
     * @param use_colors Whether to use colors in console output (default true)
     * @param use_stderr_for_errors Whether to send warnings and errors to stderr (default true)
     * @param name Optional name for the sink
     */
    void add_console_sink(TimestampFormatter::Format timestamp_format, bool use_colors = true, bool use_stderr_for_errors = true, std::string&& name = "");

    /**
     * @brief Add a console sink with custom timestamp format string and optional color and error stream settings
     * @param custom_timestamp_format Custom timestamp format string
     * @param use_colors Whether to use colors in console output (default true)
     * @param use_stderr_for_errors Whether to send warnings and errors to stderr (default true)
     * @param name Optional name for the sink
     */
    void add_console_sink(const std::string& custom_timestamp_format, bool use_colors = true, bool use_stderr_for_errors = true, std::string&& name = "");
    
    /**
     * @brief Add a file sink
     * @param path Path to the log file
     * @param name Optional name for the sink
     */
    void add_file_sink(const std::filesystem::path& path, std::string&& name = "");

    /**
     * @brief Add a file sink with custom timestamp format
     * @param path Path to the log file
     * @param timestamp_format Predefined timestamp format enum
     * @param name Optional name for the sink
     */
    void add_file_sink(const std::filesystem::path& path, TimestampFormatter::Format timestamp_format, std::string&& name = "");

    /**
     * @brief Add a file sink with custom timestamp format string
     * @param path Path to the log file
     * @param custom_timestamp_format Custom timestamp format string
     * @param name Optional name for the sink
     */
    void add_file_sink(const std::filesystem::path& path, const std::string& custom_timestamp_format, std::string&& name = "");
    
    /**
     * @brief Add a rotating file sink
     * @param path Base path to the log file
     * @param config Rotation configuration
     * @param name Optional name for the sink
     */
    void add_rotating_file_sink(const std::filesystem::path& path, const RotationConfig& config, std::string&& name = "");

    /**
     * @brief Add a rotating file sink with custom timestamp format
     * @param path Base path to the log file
     * @param config Rotation configuration
     * @param timestamp_format Predefined timestamp format enum
     * @param name Optional name for the sink
     */
    void add_rotating_file_sink(const std::filesystem::path& path, const RotationConfig& config, TimestampFormatter::Format timestamp_format, std::string&& name = "");

    /**
     * @brief Add a rotating file sink with custom timestamp format string
     * @param path Base path to the log file
     * @param config Rotation configuration
     * @param custom_timestamp_format Custom timestamp format string
     * @param name Optional name for the sink
     */
    void add_rotating_file_sink(const std::filesystem::path& path, const RotationConfig& config, const std::string& custom_timestamp_format, std::string&& name = "");
    
    /**
     * @brief Add a daily file sink
     * @param path Base path to the log file
     * @param name Optional name for the sink
     * @param config Optional Rotation configuration (uses default if not provided)
     */
    void add_daily_file_sink(const std::filesystem::path& path, const RotationConfig& config = {}, std::string&& name="");

    /**
     * @brief Add a daily file sink with custom timestamp format
     * @param path Base path to the log file
     * @param config Rotation configuration
     * @param timestamp_format Predefined timestamp format enum
     * @param name Optional name for the sink
     */
    void add_daily_file_sink(const std::filesystem::path& path, const RotationConfig& config, TimestampFormatter::Format timestamp_format, std::string&& name = "");

    /**
     * @brief Add a daily file sink with custom timestamp format string
     * @param path Base path to the log file
     * @param config Rotation configuration
     * @param custom_timestamp_format Custom timestamp format string
     * @param name Optional name for the sink
     */
    void add_daily_file_sink(const std::filesystem::path& path, const RotationConfig& config, const std::string& custom_timestamp_format, std::string&& name = "");

    /**
     * @brief Add a binary sink that writes raw payloads to a file
     * @param path Path to the binary file
     * @param name Optional name for the sink
     * @return The sink, ready to pass to the LOG_SINK_* macros
     *
     * Returns the sink rather than void - unlike the text sinks, a binary sink is
     * addressed directly at every call site, so handing it back saves a
     * get_sink() round trip.
     */
    std::shared_ptr<BinarySink> add_binary_sink(const std::filesystem::path& path, std::string&& name = "");

    /**
     * @brief Get the sink of givent type
     * @return The shared_ptr of the given sink type. It could be null if the sink of give type doesn't exist
     */
    template<typename SinkT>
    std::shared_ptr<ISink> get_sink() const noexcept;

    /**
     * @brief Get the sink by name
     * @return The shared_ptr of the given sink name. It could be null if the sink of give name doesn't exist
     */
    std::shared_ptr<ISink> get_sink(std::string_view name) const noexcept;

    /**
     * @brief Get the current log level
     * @return Current LogLevel
     */
    LogLevel get_level() const noexcept {
        return log_level_.load(std::memory_order_relaxed);
    }

    /**
     * @brief Set the minimum log level
     * @param level Minimum LogLevel to set
     */
    void set_level(LogLevel level) {
        log_level_.store(level, std::memory_order_release);
    }

    /**
     * @brief Set whether LOG_* macro output includes the macro call-site file and line
     * @param enabled True to include source location, false to omit it
     */
    void set_source_location_enabled(bool enabled) noexcept {
        set_source_location_options(enabled);
    }

    /**
     * @brief Check whether LOG_* macro output includes the macro call-site file and line
     * @return true if source location output is enabled
     */
    bool source_location_enabled() const noexcept {
        return (source_location_options_.load(std::memory_order_relaxed) & kSourceLocationEnabled) != 0;
    }

    /**
     * @brief Fast-path level check for LOG_* macros
     * @param level LogLevel being tested
     * @return true if the logger is running and accepts the level
     */
    bool should_log(LogLevel level) const noexcept {
        return running_.load(std::memory_order_relaxed)
            && log_queue_
            && level >= log_level_.load(std::memory_order_relaxed);
    }

    /**
     * @brief Log a message with a specific log level and format
     * @param level LogLevel of the message
     * @param format Format string (printf-style)
     * @param args Arguments for the format string
     */
    template<typename FormatT, typename... Args>
    void log(LogLevel level, FormatT&& format, Args&&... args);

    /**
     * @brief Compile-time source location data used by the LOG_* macro fast path
     */
    struct StaticSourceLocation {
        const char* file_name;

        consteval StaticSourceLocation(const char* static_file_name) noexcept
            : file_name(static_file_name) {
        }
    };

    static consteval StaticSourceLocation static_source_location(const char* file_name) noexcept {
        return StaticSourceLocation{file_name};
    }

    /**
     * @brief Log a message with a source location
     * @param level LogLevel of the message
     * @param file_path Source file path; basename extracted and copied before the entry is queued
     * @param line Source line
     * @param format Format string (printf-style)
     * @param args Arguments for the format string
     */
    template<typename FormatT, typename... Args>
    void log_with_location(LogLevel level, const char* file_path, uint32_t line, FormatT&& format, Args&&... args);

    /**
     * @brief Log a message with static-lifetime source path data from a LOG_* macro call site
     * @param level LogLevel of the message
     * @param location Static-lifetime source file path and file name from the macro call site
     * @param line Source line from the macro call site
     * @param format Format string (printf-style)
     * @param args Arguments for the format string
     */
    template<typename FormatT, typename... Args>
    void log_with_static_location(LogLevel level, StaticSourceLocation location, uint32_t line,
                                  FormatT&& format, Args&&... args);

    /**
     * @brief Log to one sink with static-lifetime source path data from a LOG_SINK_* call site
     * @param sink_index Index of the sink to log to; -1 broadcasts to every sink
     * @param level LogLevel of the message
     * @param location Static-lifetime source file path and file name from the macro call site
     * @param line Source line from the macro call site
     * @param format Format string (printf-style)
     * @param args Arguments for the format string
     */
    template<typename FormatT, typename... Args>
    void log_to_sink_with_static_location(int sink_index, LogLevel level, StaticSourceLocation location,
                                          uint32_t line, FormatT&& format, Args&&... args);

    /**
     * @brief Log a message to a specific sink by index
     * @param index Index of the sink to log to
     * @param level LogLevel of the message
     * @param format Format string (printf-style)
     * @param args Arguments for the format string
     */
    template<typename FormatT, typename... Args>
    void log_to_sink(int sink_index, LogLevel level, FormatT&& format, Args&&... args);

    /**
     * @brief Log a message to a specific sink by index with a source path and precomputed file name
     * @param index Index of the sink to log to
     * @param level LogLevel of the message
     * @param file_name Source file path; copied before the entry is queued when full path output is enabled
     * @param line Source line; copied before the entry is queued
     * @param copy_file_name Whether to copy the file name into the log entry (true) or store a pointer to it (false)
     * @param format Format string (printf-style)
     * @param args Arguments for the format string
     * @note Set copy_file_name to false only if the file name is guaranteed to remain valid until the log entry is written to the sink. Otherwise, set it to true to copy the file name into the log entry.
     */
    template<typename FormatT, typename... Args>
    void log_to_sink_with_location(int sink_index, LogLevel level, const char* file_name, uint32_t line,
                                   bool copy_file_name, FormatT&& format, Args&&... args);

    /**
     * @brief Shutdown the logger and flush all pending log entries
     * @param clear_sinks Clear sink list
     */
    void shutdown(bool clear_sinks = true);

    /**
     * @brief Block until all log entries queued before this call have been
     * written to all sinks. The logger keeps running — unlike shutdown(),
     * logging can continue normally after flush() returns.
     *
     * Primary use case: call flush() before unloading a shared library whose
     * string literals are referenced by queued log entries. Because LOG_* with
     * string literals stores a raw pointer into the caller's code segment,
     * unloading the library while entries are still queued would leave dangling
     * pointers that the writer thread would later dereference.
     *
     * Does nothing if the logger has not been initialized.
     */
    void flush();

    /**
     * @brief Reset the logger to uninitialized state
     * Note: This is primarily for testing purposes. Use with caution.
     */
    void reset();

    /**
     * @brief Redirect Logger::instance() to an external Logger.
     *
     * Because slick-logger is header-only, every shared library (.dll/.so) and
     * the host executable each compile in their own copy of Logger::instance().
     * Call this from a shared library's init function to make all LOG_* macros
     * in that library route to the host's logger instead of the library-local one.
     * Pass nullptr to restore the library-local singleton.
     *
     * Thread-safety: uses atomic store/load with acquire-release ordering, so
     * all initialization of the target logger is visible before any LOG_* call
     * after set_instance() returns.
     */
    static void set_instance(Logger* logger) noexcept;
    static void clear_instance_override() noexcept;

private:
    Logger() = default;
    ~Logger();

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;
    Logger(Logger&&) = delete;
    Logger& operator=(Logger&&) = delete;

    /// Outcome of a single writer-thread step, see drain_pending().
    enum class DrainResult : uint8_t {
        Wrote,           // entries were handed to the sinks
        Idle,            // nothing available and the read cursor has caught up
        WaitingOnStall,  // a slot ahead of us is reserved but still unpublished
        SkippedStalled   // that slot was abandoned after stalled_entry_timeout_ms
    };

    void start();
    void writer_thread_func();
    /// Perform one read/dispatch step, including stalled-slot recovery.
    /// Shared by the running loop and the shutdown drain so the two cannot diverge.
    DrainResult drain_pending();
    void write_log_entry(const LogEntry* entry_ptr, uint32_t count);
    void dispatch_entry(const LogEntry& entry);

    /// Drop every sink, resetting each one's index so handles the caller kept
    /// cannot address a slot a later add_sink() reuses. Shared by clear_sinks()
    /// and shutdown(), which must not diverge on this.
    void release_sinks();

    /// Flush every sink. Only ever called on the writer thread, which owns them.
    void flush_sinks();

    /// Release the writer thread if it is parked. Called from the enqueue path,
    /// so it costs a relaxed-order load and a predictable branch when the writer
    /// is already awake, which is the case whenever logging is ongoing.
    void wake_writer() noexcept;

    /// Release the writer thread whether or not it looks parked. Used by the rare
    /// paths - flush() and shutdown() - where missing a wake-up would stall the
    /// caller rather than merely delay an entry.
    void force_wake_writer() noexcept;

    /// Block until a producer publishes, a flush is requested, or the logger
    /// stops. Only called on the writer thread.
    void park_writer();

    /// Wait on a caller's thread until @p ready holds or the logger stops.
    /// Stays hot briefly - a drain usually completes well inside that - before
    /// falling back to sleeping so a long wait does not pin a core. The
    /// predicate is re-checked every pass, so no wake-up can be missed and
    /// shutdown always ends the wait.
    template<typename Predicate>
    void wait_until_ready(Predicate ready);
    void set_source_location_options(bool enabled) noexcept;

    // Helper function to round up to next power of 2
    static size_t round_up_to_power_of_2(size_t value) noexcept;

    template<typename T>
    void enqueue_argument(LogArgument& arg, T&& value);

    void enqueue_format_args(LogEntry& entry, std::format_args fa);

    /// Copy bytes into the string ring, optionally NUL-terminating them.
    /// Truncates at kMaxPayloadBytes; shared by the string and binary paths.
    StringRef store_bytes_in_queue(const char* data, size_t size, bool terminate);
    StringRef store_string_in_queue(std::string_view str);
    StringRef store_binary_in_queue(BinaryView payload);

    // ---- Multi-process helpers ----

    /// Create the shared-memory backed queues and cache the per-entry stamp
    /// (pid, tag, kEntryOffsets) for the requested role.
    void setup_shared_queues(const LogConfig& config, uint32_t queue_size, uint32_t string_buffer_size);

    /// Validate a segment name and throw a descriptive error when it is unusable.
    static void validate_shared_memory_name(const std::string& name);

    /// Bytes of `tag` that fit the inline per-entry tag, without splitting a
    /// multi-byte UTF-8 sequence.
    static size_t truncated_tag_length(std::string_view tag) noexcept;

    using logger_queue_traits = detail::logger_queue_traits;

    /// Attach to an existing segment when one is there, otherwise create it.
    /// This is what makes producer and collector startup order irrelevant, and it
    /// lets a producer inherit the collector's sizing when the collector went first.
    template<typename T>
    static std::unique_ptr<slick::queue<T, logger_queue_traits>> open_shared_queue(const std::string& name, uint32_t size);

    /// Turn an entry's ring indices back into addresses valid in this process.
    void rebase_entry(LogEntry& entry) const noexcept;

    /// Record a shared-memory queue this process created and must never destroy,
    /// see shutdown(). The registry is itself never freed, which keeps the
    /// retained queues reachable: leak detectors report unreachable allocations,
    /// so an intentional retention must stay visible to avoid a false positive.
    ///
    /// Each init()/shutdown() cycle that CREATED its segments retains one more
    /// mapping, so repeatedly re-initializing in a shared role grows memory
    /// monotonically. Fine for the usual one-shot process lifecycle; a process
    /// that cycles the logger many times should attach to a segment created
    /// elsewhere, which is never retained.
    static void retain_shared_queue(void* queue) noexcept;

    std::unique_ptr<slick::queue<LogEntry, logger_queue_traits>> log_queue_;
    std::unique_ptr<slick::queue<char, logger_queue_traits>> string_queue_;
    std::vector<std::shared_ptr<ISink>> sinks_;
    std::filesystem::path log_file_;
    std::thread writer_thread_;
    std::atomic<bool> running_{false};
    /// How far the writer thread has claimed into the queue. Written only by the
    /// writer thread but read by callers of flush(), so it has to be atomic even
    /// though nothing synchronises through it: relaxed is enough because it
    /// carries no data, only progress. The ordering that makes flush() a real
    /// guarantee is the flush_request_/flush_done_ handshake below.
    std::atomic<uint64_t> read_index_{0};
    /// Sink flushing is owned by the writer thread, so flush() cannot touch the
    /// sinks directly without racing it. A caller bumps flush_request_ and waits
    /// for the writer thread to publish the same generation in flush_done_.
    std::atomic<uint64_t> flush_request_{0};
    std::atomic<uint64_t> flush_done_{0};
    /// Set while the writer thread is blocked in wake_token_.wait(). Producers
    /// read it after publishing and only pay for a wake-up when one is needed.
    /// Both the store here and the load in wake_writer() are seq_cst: the writer
    /// re-checks for work *after* announcing itself and the producer checks this
    /// flag *after* publishing, so the two orderings must not be reordered past
    /// each other or a wake-up could be lost.
    std::atomic<bool> writer_parked_{false};
    /// Bumped to release the parked writer thread. The value must change, not
    /// merely be notified: atomic::wait re-blocks if the value still compares
    /// equal to the one it was given.
    std::atomic<uint32_t> wake_token_{0};
    std::atomic<LogLevel> log_level_{LogLevel::L_TRACE};
    static constexpr uint8_t kSourceLocationEnabled = 0x01;
    std::atomic<uint8_t> source_location_options_{kSourceLocationEnabled};
    std::unordered_map<std::string_view, int> sinkname_index_map_;

    // ---- Multi-process state; all inert while mode_ is Local ----
    QueueMode mode_ = QueueMode::Local;
    // Cached per-entry stamp, applied on the caller thread by log_to_sink_with_location.
    // Holding these as members keeps the hot path to a load plus a fixed-size copy.
    uint8_t entry_flags_ = 0;
    uint32_t pid_ = 0;
    char tag_[SLICK_LOGGER_TAG_SIZE] = {};
    std::string shm_name_;
    bool collect_backlog_ = true;
    uint32_t stalled_entry_timeout_ms_ = 0;
    // Scratch used by the writer thread to rebase shared-memory entries. Owned by
    // that thread alone, so no synchronization is needed.
    LogEntry rebase_scratch_{};
    // When the writer thread first noticed an unpublished slot blocking progress.
    // Writer-thread-only, like rebase_scratch_.
    std::chrono::steady_clock::time_point stalled_since_{};

    // The logger instance local to this shared library / executable.
    // override_instance_ points here by default.
    static Logger instance_;
    // Points to the active Logger for this shared library / executable.
    // Defaults to &instance_. Call set_instance() to redirect to an external
    // logger (e.g. the host application's logger when loaded as a plugin).
    static std::atomic<Logger*> override_instance_;
};



// ------------------------------ Implementation (header-only library) ------------------------------


inline bool ISink::should_log(const Logger& logger, LogLevel level) const noexcept {
    // Cheapest checks first; the logger's own check ends in an atomic load.
    return index_ >= 0
        && level >= min_level_.load(std::memory_order_relaxed)
        && logger.should_log(level);
}

inline bool ISink::should_log(LogLevel level) const noexcept {
    return should_log(Logger::instance(), level);
}

template<typename FormatT, typename... Args>
inline void ISink::log(LogLevel level, FormatT&& format, Args&&... args) {
    // Resolve the logger once and use it for both the check and the call: two
    // Logger::instance() calls would be two atomic loads, and a set_instance()
    // landing between them would test one logger and log to another.
    auto& logger = Logger::instance();
    // Same predicate the LOG_SINK_* macros use. Two reasons to check it here:
    // below-threshold entries then cost neither a queue slot nor a string-ring
    // copy, and a sink that is not attached to a logger has index -1, which
    // log_to_sink() would read as "broadcast to every sink". The arguments were
    // already evaluated by the caller - use the macros to avoid that too.
    if (!should_log(logger, level)) {
        return;
    }
    logger.log_to_sink(index_, level, std::forward<FormatT>(format), std::forward<Args>(args)...);
}

template<typename FormatT, typename... Args>
inline void ISink::log_trace(FormatT&& format, Args&&... args) {
    log(LogLevel::L_TRACE, std::forward<FormatT>(format), std::forward<Args>(args)...);
}

template<typename FormatT, typename... Args>
inline void ISink::log_debug(FormatT&& format, Args&&... args) {
    log(LogLevel::L_DEBUG, std::forward<FormatT>(format), std::forward<Args>(args)...);
}

template<typename FormatT, typename... Args>
inline void ISink::log_info(FormatT&& format, Args&&... args) {
    log(LogLevel::L_INFO, std::forward<FormatT>(format), std::forward<Args>(args)...);
}

template<typename FormatT, typename... Args>
inline void ISink::log_warn(FormatT&& format, Args&&... args) {
    log(LogLevel::L_WARN, std::forward<FormatT>(format), std::forward<Args>(args)...);
}

template<typename FormatT, typename... Args>
inline void ISink::log_error(FormatT&& format, Args&&... args) {
    log(LogLevel::L_ERROR, std::forward<FormatT>(format), std::forward<Args>(args)...);
}

template<typename FormatT, typename... Args>
inline void ISink::log_fatal(FormatT&& format, Args&&... args) {
    log(LogLevel::L_FATAL, std::forward<FormatT>(format), std::forward<Args>(args)...);
}

/**
 * @brief View a resolved StringRef, preferring the stored length over a strlen scan
 * @note Only valid on entries whose references have been resolved to addresses,
 *       which is always the case by the time a sink sees them.
 */
inline std::string_view view_string_ref(StringRef ref) noexcept {
    if (!ref.ptr) {
        return {};
    }
    return ref.length ? std::string_view{ref.ptr, ref.length} : std::string_view{ref.ptr};
}

/**
 * @brief View the payload of an ArgType::BLOB argument
 * @note Unlike view_string_ref, the stored length is authoritative and a length
 *       of 0 means an empty payload. Binary payloads can contain NUL bytes, so
 *       the strlen fallback that view_string_ref uses would read past the end.
 */
inline std::span<const std::byte> view_binary(StringRef ref) noexcept {
    if (!ref.ptr || !ref.length) {
        return {};
    }
    return {reinterpret_cast<const std::byte*>(ref.ptr), ref.length};
}

/**
 * @brief Render a binary payload as hex text for the text sinks
 * @param format_spec The full "{...}" spec from the format string
 * @param ref The payload
 *
 * Supported spec flags: 'X' for uppercase hex (lowercase by default) and a
 * ".N" precision capping how many bytes are rendered. The default cap keeps a
 * 64KB payload from turning into a 128KB console line; when it bites, the byte
 * count of the whole payload is appended.
 */
inline void append_binary_arg(std::string& out, std::string_view format_spec, StringRef ref) {
    constexpr size_t kDefaultPreviewBytes = 64;
    static constexpr char kLower[] = "0123456789abcdef";
    static constexpr char kUpper[] = "0123456789ABCDEF";

    const auto bytes = view_binary(ref);
    const char* digits = format_spec.find('X') != std::string_view::npos ? kUpper : kLower;

    size_t preview = kDefaultPreviewBytes;
    if (const auto dot = format_spec.find('.'); dot != std::string_view::npos) {
        preview = 0;
        for (size_t i = dot + 1; i < format_spec.size() && format_spec[i] >= '0' && format_spec[i] <= '9'; ++i) {
            preview = preview * 10 + static_cast<size_t>(format_spec[i] - '0');
        }
    }

    const size_t shown = std::min(preview, bytes.size());
    // Written straight into the caller's buffer. Every text sink formats the
    // entry independently, so a temporary string here would be an allocation and
    // a copy of a few hundred bytes per sink per binary argument.
    out.reserve(out.size() + shown * 2 + 24);
    for (size_t i = 0; i < shown; ++i) {
        const auto byte = static_cast<unsigned char>(bytes[i]);
        out += digits[byte >> 4];
        out += digits[byte & 0x0F];
    }
    if (shown < bytes.size()) {
        out += "...(";
        out += std::to_string(bytes.size());
        out += " bytes)";
    }
}

/// Convenience wrapper around append_binary_arg for callers that want a string.
inline std::string format_binary_arg(std::string_view format_spec, StringRef ref) {
    std::string result;
    append_binary_arg(result, format_spec, ref);
    return result;
}

/**
 * @brief Format a single log argument against one "{...}" spec
 * @note The value is taken BY VALUE on purpose. LogEntry and LogArgument are
 *       "#pragma pack(1)", so their members can sit at misaligned addresses,
 *       and std::make_format_args binds a reference to whatever it is handed.
 *       Binding that reference directly to a packed member is undefined
 *       behavior - UBSan reports "reference binding to misaligned address".
 *       Reading a packed member into a by-value parameter is well defined, and
 *       the parameter itself is always suitably aligned.
 */
template<typename T>
inline std::string format_one_arg(std::string_view format_spec, T value) {
    return std::vformat(format_spec, std::make_format_args(value));
}

inline std::pair<std::string, bool> ISink::format_log_message(const LogEntry& entry) {
    if (entry.arg_count == 0) {
        return std::make_pair(std::string{view_string_ref(entry.format)}, true);
    }

    // Since std::make_format_args doesn't work with custom types in MSVC,
    // we'll use a manual implementation that preserves std::format functionality
    // by manually parsing format specifiers and applying them to each argument
    try {
        std::string format_str{view_string_ref(entry.format)};
        std::string result;
        result.reserve(format_str.length() + 256); // Reserve some space for formatting

        size_t pos = 0;
        uint8_t arg_index = 0;

        while (pos < format_str.length()) {
            size_t brace_start = format_str.find_first_of("{}", pos);
            if (brace_start == std::string::npos) {
                // No more format specifiers, copy rest of string
                result += format_str.substr(pos);
                break;
            }

            // Copy everything before the brace
            result += format_str.substr(pos, brace_start - pos);

            if (format_str[brace_start] == '{' &&
                brace_start + 1 < format_str.length() &&
                format_str[brace_start + 1] == '{') {
                result += '{';
                pos = brace_start + 2;
                continue;
            }

            if (format_str[brace_start] == '}') {
                if (brace_start + 1 < format_str.length() &&
                    format_str[brace_start + 1] == '}') {
                    result += '}';
                    pos = brace_start + 2;
                } else {
                    throw std::format_error("unmatched '}' in format string");
                }
                continue;
            }

            // Find the closing brace
            size_t brace_end = format_str.find('}', brace_start);
            if (brace_end == std::string::npos) {
                // Malformed format string
                result += format_str.substr(brace_start);
                break;
            }

            if (arg_index >= entry.arg_count) {
                // Not enough arguments
                result += "<MISSING_ARG>";
                pos = brace_end + 1;
                continue;
            }

            // Extract format spec (everything between { and })
            std::string format_spec = format_str.substr(brace_start, brace_end - brace_start + 1);

            // Format the argument using std::format with the specific format spec
            const auto& arg = entry.args[arg_index];
            std::string formatted_arg;

            // Every case passes the union member by value through format_one_arg:
            // these members are packed and may be misaligned, so a reference must
            // never be bound directly to one. See format_one_arg.
            switch (arg.type) {
                case ArgType::BOOL:
                    formatted_arg = format_one_arg(format_spec, arg.value.b);
                    break;
                case ArgType::CHAR:
                    formatted_arg = format_one_arg(format_spec, arg.value.c);
                    break;
                case ArgType::U_CHAR:
                    formatted_arg = format_one_arg(format_spec, arg.value.uc);
                    break;
                // case ArgType::WCHAR:
                //     formatted_arg = format_one_arg(format_spec, arg.value.wc);
                //     break;
                case ArgType::INT8_T:
                    formatted_arg = format_one_arg(format_spec, arg.value.i8);
                    break;
                case ArgType::UINT8_T:
                    formatted_arg = format_one_arg(format_spec, arg.value.u8);
                    break;
                case ArgType::INT16_T:
                    formatted_arg = format_one_arg(format_spec, arg.value.i16);
                    break;
                case ArgType::UINT16_T:
                    formatted_arg = format_one_arg(format_spec, arg.value.u16);
                    break;
                case ArgType::INT32_T:
                    formatted_arg = format_one_arg(format_spec, arg.value.i32);
                    break;
                case ArgType::UINT32_T:
                    formatted_arg = format_one_arg(format_spec, arg.value.u32);
                    break;
                case ArgType::INT64_T:
                    formatted_arg = format_one_arg(format_spec, arg.value.i64);
                    break;
                case ArgType::UINT64_T:
                    formatted_arg = format_one_arg(format_spec, arg.value.u64);
                    break;
                case ArgType::FLOAT:
                    formatted_arg = format_one_arg(format_spec, arg.value.f);
                    break;
                case ArgType::DOUBLE:
                    formatted_arg = format_one_arg(format_spec, arg.value.d);
                    break;
                case ArgType::PTR:
                    formatted_arg = format_one_arg(format_spec, arg.value.ptr);
                    break;
                case ArgType::STRING_LITERAL:
                    formatted_arg = format_one_arg(format_spec, arg.value.literal_ptr);
                    break;
                case ArgType::STRING_DYNAMIC:
                    formatted_arg = format_one_arg(format_spec, view_string_ref(arg.value.dynamic_str));
                    break;
                case ArgType::BLOB:
                    // Rendered here rather than through format_one_arg: std::format
                    // has no formatter for raw bytes, and the spec flags a payload
                    // accepts are its own. Appended directly to the result, leaving
                    // formatted_arg empty, so the hex text is built once instead of
                    // being materialized and then copied.
                    append_binary_arg(result, format_spec, arg.value.dynamic_str);
                    break;
                default:
                    formatted_arg = "<UNKNOWN>";
                    break;
            }

            result += formatted_arg;
            pos = brace_end + 1;
            arg_index++;
        }

        return std::make_pair(result, true);
    } catch (const std::format_error& e) {
        // Return error message if formatting fails
        return std::make_pair(std::string("[FORMAT_ERROR: ") + e.what() +"]", false);
    } catch (...) {
        return std::make_pair("[FORMAT_ERROR: Unknown format error]", false);
    }
}

inline void append_source_location(std::string& result, const LogEntry& entry) {
    if (!has_source_location(entry)) {
        return;
    }
    result += " [";
    result += view_string_ref(entry.file);
    result += ':';
    result += std::to_string(entry.line);
    result += ']';
}

inline size_t decimal_digits(uint32_t value) noexcept {
    size_t digits = 1;
    for (uint32_t v = value; v >= 10; v /= 10) {
        ++digits;
    }
    return digits;
}

inline size_t source_location_size(const LogEntry& entry) {
    if (!has_source_location(entry)) {
        return 0;
    }

    return view_string_ref(entry.file).size() + decimal_digits(entry.line) + 4; // " [", ':', ']'
}

/**
 * @brief Append the producing process id (and tag, when set) to a formatted line
 *
 * Only multi-process entries carry a pid, so single-process output is unaffected.
 */
inline void append_process_id(std::string& result, const LogEntry& entry) {
    if (entry.pid == 0) {
        return;
    }
    result += " [";
    result += std::to_string(entry.pid);
    if (entry.tag[0] != '\0') {
        result += ':';
        result.append(entry.tag, strnlen(entry.tag, SLICK_LOGGER_TAG_SIZE));
    }
    result += ']';
}

inline size_t process_id_size(const LogEntry& entry) {
    if (entry.pid == 0) {
        return 0;
    }
    size_t size = decimal_digits(entry.pid) + 3; // " [", ']'
    const size_t tag_length = strnlen(entry.tag, SLICK_LOGGER_TAG_SIZE);
    if (tag_length) {
        size += tag_length + 1; // ':'
    }
    return size;
}

inline ConsoleSink::ConsoleSink(bool use_colors, bool use_stderr_for_errors,
                                TimestampFormatter::Format timestamp_format, std::string&& name)
    : ISink(std::move(name)), use_colors_(use_colors), use_stderr_for_errors_(use_stderr_for_errors)
    , timestamp_formatter_(timestamp_format) {
}

inline ConsoleSink::ConsoleSink(const std::string& custom_timestamp_format, bool use_colors, 
                                bool use_stderr_for_errors, std::string&& name)
    : ISink(std::move(name)), use_colors_(use_colors), use_stderr_for_errors_(use_stderr_for_errors)
    , timestamp_formatter_(custom_timestamp_format) {
}

inline void ConsoleSink::write(const LogEntry& entry) {
    std::string formatted = format_log_entry(entry);
    
    if (use_stderr_for_errors_ && (entry.level >= LogLevel::L_WARN)) {
        std::cerr << formatted << std::endl;
    } else {
        std::cout << formatted << std::endl;
    }
}

inline void ConsoleSink::flush() {
    std::cout.flush();
    std::cerr.flush();
}

inline std::string ConsoleSink::format_log_entry(const LogEntry& entry) {
    std::string level_str = to_string(entry.level);
    std::string timestamp = timestamp_formatter_.format_timestamp(entry.timestamp);
    auto [message, good] = format_log_message(entry);
    if (!good) [[unlikely]] {
        level_str = "ERROR";
    }
    std::string result;
    result.reserve(timestamp.size() + level_str.size() + process_id_size(entry)
                   + source_location_size(entry) + message.size() + 4);
    result += timestamp;
    result += " [";
    result += level_str;
    result += ']';
    append_process_id(result, entry);
    append_source_location(result, entry);
    result += ' ';
    result += message;

    if (use_colors_) {
        return get_color_code(entry.level) + result + get_reset_code();
    }
    
    return result;
}

inline std::string ConsoleSink::get_color_code(LogLevel level) {
    switch (level) {
        case LogLevel::L_TRACE: return "\033[90m";   // Dark gray
        case LogLevel::L_DEBUG: return "\033[36m";   // Cyan
        case LogLevel::L_INFO: return "\033[32m";    // Green
        case LogLevel::L_WARN: return "\033[33m";    // Yellow
        case LogLevel::L_ERROR: return "\033[31m";     // Red
        case LogLevel::L_FATAL: return "\033[91m";   // Bright red
        default: return "";
    }
}

inline std::string ConsoleSink::get_reset_code() {
    return "\033[0m";
}

inline void FileSinkBase::open_stream(const std::filesystem::path& path, std::ios::openmode mode) {
    const auto parent = path.parent_path();
    if (!parent.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(parent, ec); // no-op when it already exists
    }
    // Must be set while the stream is closed to take effect. Every caller either
    // opens for the first time or has just closed the previous file.
    if (stream_buffer_.empty()) {
        stream_buffer_.resize(kStreamBufferSize);
    }
    file_stream_.rdbuf()->pubsetbuf(stream_buffer_.data(),
                                    static_cast<std::streamsize>(stream_buffer_.size()));
    file_stream_.open(path, mode);
}

inline FileSinkBase::FileSinkBase(const std::filesystem::path& file_path, std::ios::openmode mode,
                                  std::string&& name)
    : ISink(std::move(name)), file_path_(file_path) {
    open_stream(file_path_, mode);
    if (!file_stream_) {
        throw std::runtime_error("Failed to open log file: " + file_path_.string());
    }
}

inline void FileSinkBase::flush() {
    if (file_stream_) {
        file_stream_.flush();
    }
}

inline FileSink::FileSink(const std::filesystem::path& file_path,
                          TimestampFormatter::Format timestamp_format, std::string&& name)
    : FileSinkBase(file_path, std::ios::app, std::move(name)), timestamp_formatter_(timestamp_format) {
}

inline FileSink::FileSink(const std::filesystem::path& file_path,
                          const std::string& custom_timestamp_format, std::string&& name)
    : FileSinkBase(file_path, std::ios::app, std::move(name)), timestamp_formatter_(custom_timestamp_format) {
}

inline void FileSink::write(const LogEntry& entry) {
    if (file_stream_) {
        file_stream_ << format_log_entry(entry) << "\n";
    }
}

inline std::string FileSink::format_log_entry(const LogEntry& entry) {
    std::string level_str = to_string(entry.level);
    std::string timestamp = timestamp_formatter_.format_timestamp(entry.timestamp);
    auto [message, good] = format_log_message(entry);
    if (!good) [[unlikely]] {
        level_str = "ERROR";
    }
    std::string result;
    result.reserve(timestamp.size() + level_str.size() + process_id_size(entry)
                   + source_location_size(entry) + message.size() + 4);
    result += timestamp;
    result += " [";
    result += level_str;
    result += ']';
    append_process_id(result, entry);
    append_source_location(result, entry);
    result += ' ';
    result += message;
    return result;
}

inline RotatingFileSink::RotatingFileSink(const std::filesystem::path& base_path, const RotationConfig& config,
                                          TimestampFormatter::Format timestamp_format, std::string&& name)
    : FileSink(base_path, timestamp_format, std::move(name)), config_(config), base_path_(base_path), current_file_size_(0) {
    if (std::filesystem::exists(base_path_)) {
        current_file_size_ = std::filesystem::file_size(base_path_);
    }
}

inline RotatingFileSink::RotatingFileSink(const std::filesystem::path& base_path, const RotationConfig& config,
                                        const std::string& custom_timestamp_format, std::string&& name)
    : FileSink(base_path, custom_timestamp_format, std::move(name)), config_(config), base_path_(base_path), current_file_size_(0) {
    if (std::filesystem::exists(base_path_)) {
        current_file_size_ = std::filesystem::file_size(base_path_);
    }
}

inline void RotatingFileSink::write(const LogEntry& entry) {
    check_rotation();
    
    if (file_stream_) {
        std::string formatted = format_log_entry(entry);
        file_stream_ << formatted << "\n";
        current_file_size_ += formatted.length() + 1; // +1 for newline
    }
}

inline void RotatingFileSink::check_rotation() {
    if (current_file_size_ >= config_.max_file_size) {
        rotate_files();
    }
}

inline void RotatingFileSink::rotate_files() {
    file_stream_.close();
    
    // Remove the oldest file if it exists
    auto oldest_file = get_rotated_filename(config_.max_files - 1);
    if (std::filesystem::exists(oldest_file)) {
        std::filesystem::remove(oldest_file);
    }
    
    // Rotate existing files
    for (size_t i = config_.max_files - 1; i > 0; --i) {
        auto src = (i == 1) ? base_path_ : get_rotated_filename(i - 1);
        auto dst = get_rotated_filename(i);
        
        if (std::filesystem::exists(src)) {
            std::filesystem::rename(src, dst);
        }
    }
    
    // Create new current file
    open_stream(base_path_, std::ios::out | std::ios::trunc);
    current_file_size_ = 0;
}

inline std::filesystem::path RotatingFileSink::get_rotated_filename(size_t index) {
    std::string filename = base_path_.stem().string() + "_" + std::to_string(index) + base_path_.extension().string();
    return base_path_.parent_path() / filename;
}

inline DailyFileSink::DailyFileSink(const std::filesystem::path& base_path, const RotationConfig& config,
                                    TimestampFormatter::Format timestamp_format, std::string&& name)
    : FileSink(base_path, timestamp_format, std::move(name)), config_(config), base_path_(base_path),
      current_file_size_(0) {
    current_date_ = get_date_string();

    // Check if file already exists and is from a previous day
    if (std::filesystem::exists(base_path_)) {
        auto last_write_time = std::filesystem::last_write_time(base_path_);
        auto last_write_time_t = std::chrono::system_clock::to_time_t(
            std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                last_write_time - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now()
            )
        );
        std::tm* tm_ptr = std::localtime(&last_write_time_t);
        if (tm_ptr) {
            char date_str[11];
            std::strftime(date_str, sizeof(date_str), "%Y-%m-%d", tm_ptr);
            std::string file_date = std::string(date_str);

            // If the existing file is from a previous day, rotate it
            if (file_date != current_date_) {
                file_stream_.close();

                // Check if dated file already exists, if so rotate all files for that date
                std::filesystem::path dated_file = get_dated_filename(file_date);
                if (std::filesystem::exists(dated_file)) {
                    rotate_files_for_date(file_date);
                }

                // Now rename current base file to dated filename
                std::error_code ec;
                std::filesystem::rename(base_path_, dated_file, ec);
                if (ec) {
                    // If rename fails, try copy and remove
                    std::filesystem::copy_file(base_path_, dated_file, ec);
                    if (!ec) {
                        std::filesystem::remove(base_path_, ec);
                    }
                }

                // Reopen file for today
                open_stream(base_path_, std::ios::out | std::ios::trunc);
                if (!file_stream_) {
                    throw std::runtime_error("Failed to reopen daily log file: " + base_path_.string());
                }
                current_file_size_ = 0;
            } else {
                // File is from today, continue appending
                current_file_size_ = std::filesystem::file_size(base_path_);
            }
        } else {
            // Could not get file date, just continue with current file
            current_file_size_ = std::filesystem::file_size(base_path_);
        }
    }
    // Keep logging to base_path (e.g., daily.log) - FileSink constructor already opened it
}

inline DailyFileSink::DailyFileSink(const std::filesystem::path& base_path, const RotationConfig& config,
                                  const std::string& custom_timestamp_format, std::string&& name)
    : FileSink(base_path, custom_timestamp_format, std::move(name))
    , config_(config)
    , base_path_(base_path)
    , current_file_size_(0) {
    current_date_ = get_date_string();

    // Check if file already exists and is from a previous day
    if (std::filesystem::exists(base_path_)) {
        auto last_write_time = std::filesystem::last_write_time(base_path_);
        auto last_write_time_t = std::chrono::system_clock::to_time_t(
            std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                last_write_time - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now()
            )
        );
        std::tm* tm_ptr = std::localtime(&last_write_time_t);
        if (tm_ptr) {
            char date_str[11];
            std::strftime(date_str, sizeof(date_str), "%Y-%m-%d", tm_ptr);
            std::string file_date = std::string(date_str);

            // If the existing file is from a previous day, rotate it
            if (file_date != current_date_) {
                file_stream_.close();

                // Check if dated file already exists, if so rotate all files for that date
                std::filesystem::path dated_file = get_dated_filename(file_date);
                if (std::filesystem::exists(dated_file)) {
                    rotate_files_for_date(file_date);
                }

                // Now rename current base file to dated filename
                std::error_code ec;
                std::filesystem::rename(base_path_, dated_file, ec);
                if (ec) {
                    // If rename fails, try copy and remove
                    std::filesystem::copy_file(base_path_, dated_file, ec);
                    if (!ec) {
                        std::filesystem::remove(base_path_, ec);
                    }
                }

                // Reopen file for today
                open_stream(base_path_, std::ios::out | std::ios::trunc);
                if (!file_stream_) {
                    throw std::runtime_error("Failed to reopen daily log file: " + base_path_.string());
                }
                current_file_size_ = 0;
            } else {
                // File is from today, continue appending
                current_file_size_ = std::filesystem::file_size(base_path_);
            }
        } else {
            // Could not get file date, just continue with current file
            current_file_size_ = std::filesystem::file_size(base_path_);
        }
    }
    // Keep logging to base_path (e.g., daily.log) - FileSink constructor already opened it
}

inline void DailyFileSink::write(const LogEntry& entry) {
    check_rotation();

    if (file_stream_) {
        std::string formatted = format_log_entry(entry);
        file_stream_ << formatted << "\n";
        current_file_size_ += formatted.length() + 1; // +1 for newline
    }
}

inline void DailyFileSink::check_rotation() {
    std::string today = get_date_string();

    // Check for date-based rotation
    if (today != current_date_) {
        // Close current file
        file_stream_.close();

        // Rename current base file to dated filename (e.g., daily.log -> daily_2025-08-24.log)
        std::filesystem::path old_dated_file = get_dated_filename(current_date_);
        std::error_code ec;
        if (std::filesystem::exists(base_path_)) {
            std::filesystem::rename(base_path_, old_dated_file, ec);
            if (ec) {
                // If rename fails, try copy and remove
                std::filesystem::copy_file(base_path_, old_dated_file, ec);
                if (!ec) {
                    std::filesystem::remove(base_path_, ec);
                }
            }
        }

        // Reopen base file for new day's logs
        open_stream(base_path_, std::ios::out | std::ios::trunc); // Start fresh for new day
        if (!file_stream_) {
            throw std::runtime_error("Failed to reopen daily log file: " + base_path_.string());
        }

        current_file_size_ = 0;
        current_date_ = today;
    }

    // Check for size-based rotation
    if (config_.max_file_size && current_file_size_ >= config_.max_file_size) {
        rotate_daily_files();
    }
}

inline void DailyFileSink::rotate_files_for_date(const std::string& date) {
    // Remove the oldest file if it exists and max_files is configured
    // Files are: daily_DATE.log (index 0), daily_DATE_001.log (index 1), ..., daily_DATE_<max_files-1>.log (index max_files-1)

    auto dated_file = get_dated_filename(date);

    if (config_.max_files == 1) {
        // If max_files is 1, we only keep the dated file (no indexed files)
        // Just remove the old dated file - it will be replaced by the new one
        if (std::filesystem::exists(dated_file)) {
            std::filesystem::remove(dated_file);
        }
    } else if (config_.max_files > 1) {
        // Remove the oldest indexed file
        auto oldest_file = get_dated_indexed_filename(date, config_.max_files - 1);
        if (std::filesystem::exists(oldest_file)) {
            std::filesystem::remove(oldest_file);
        }

        // Rotate existing indexed files for the given date
        // Start from the highest index and work down to 2
        for (size_t i = config_.max_files - 1; i >= 2; --i) {
            auto src = get_dated_indexed_filename(date, i - 1);
            auto dst = get_dated_indexed_filename(date, i);

            if (std::filesystem::exists(src)) {
                std::filesystem::rename(src, dst);
            }
        }

        // Rotate the dated file (without index) to _001.log
        if (std::filesystem::exists(dated_file)) {
            auto indexed_file_1 = get_dated_indexed_filename(date, 1);
            std::filesystem::rename(dated_file, indexed_file_1);
        }
    }
}

inline void DailyFileSink::rotate_daily_files() {
    file_stream_.close();

    // Rotate all existing files for the current date (if any)
    std::filesystem::path dated_file = get_dated_filename(current_date_);

    // Always rotate if dated file exists, which will shift:
    // daily_2025-10-02.log -> _001.log
    // _001.log -> _002.log, etc.
    if (std::filesystem::exists(dated_file)) {
        rotate_files_for_date(current_date_);
    }

    // Rename current base file to dated filename (e.g., daily.log -> daily_2025-10-02.log)
    std::error_code ec;
    if (std::filesystem::exists(base_path_)) {
        std::filesystem::rename(base_path_, dated_file, ec);
        if (ec) {
            // If rename fails, try copy and remove
            std::filesystem::copy_file(base_path_, dated_file, ec);
            if (!ec) {
                std::filesystem::remove(base_path_, ec);
            }
        }
    }

    // Create new current file
    open_stream(base_path_, std::ios::out | std::ios::trunc);
    if (!file_stream_) {
        throw std::runtime_error("Failed to reopen daily log file: " + base_path_.string());
    }
    current_file_size_ = 0;
}

inline std::filesystem::path DailyFileSink::get_daily_filename() const {
    std::string date_str = get_date_string();
    return get_dated_filename(date_str);
}

inline std::filesystem::path DailyFileSink::get_dated_filename(const std::string& date) const {
    std::string filename = base_path_.stem().string() + "_" + date + base_path_.extension().string();
    return base_path_.parent_path() / filename;
}

inline std::filesystem::path DailyFileSink::get_dated_indexed_filename(const std::string& date, size_t index) const {
    std::ostringstream oss;
    oss << base_path_.stem().string() << "_" << date << "_" << std::setfill('0') << std::setw(3) << index << base_path_.extension().string();
    return base_path_.parent_path() / oss.str();
}

inline std::string DailyFileSink::get_date_string() const {
    auto now = std::chrono::system_clock::now();
    time_t time_val = std::chrono::system_clock::to_time_t(now);
    std::tm* tm_ptr = std::localtime(&time_val);
    if (!tm_ptr) {
        return "1970-01-01"; // fallback date
    }
    std::tm tm = *tm_ptr;

    char date_str[11];
    std::strftime(date_str, sizeof(date_str), "%Y-%m-%d", &tm);
    return std::string(date_str);
}

inline BinarySink::BinarySink(const std::filesystem::path& file_path, std::string&& name)
    // std::ios::binary is what keeps Windows from expanding a 0x0A byte in the
    // payload into CR LF, which would corrupt every record after it.
    : FileSinkBase(file_path, std::ios::binary | std::ios::app, std::move(name)) {
    // A binary sink can only write binary payloads, so broadcast text entries
    // would be dropped anyway. Opt out with set_dedicated(false).
    set_dedicated(true);
}

inline void BinarySink::write(const LogEntry& entry) {
    if (!file_stream_) {
        return;
    }
    for (uint8_t i = 0; i < entry.arg_count; ++i) {
        const LogArgument& arg = entry.args[i];
        if (arg.type != ArgType::BLOB) {
            continue;
        }
        const auto payload = view_binary(arg.value.dynamic_str);
        if (!payload.empty()) {
            write_payload(payload.data(), payload.size());
        }
    }
}

inline void BinarySink::write_payload(const std::byte* data, size_t size) {
    file_stream_.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
    bytes_written_.fetch_add(size, std::memory_order_relaxed);
}

inline Logger& Logger::instance() {
    return *override_instance_.load(std::memory_order_acquire);
}

inline Logger Logger::instance_;
inline std::atomic<Logger*> Logger::override_instance_{&Logger::instance_};

inline void Logger::set_instance(Logger* logger) noexcept {
    override_instance_.store(logger, std::memory_order_release);
}

inline void Logger::clear_instance_override() noexcept {
    override_instance_.store(&instance_, std::memory_order_release);
}

inline void Logger::set_source_location_options(bool enabled) noexcept {
    source_location_options_.store(enabled ? kSourceLocationEnabled : 0, std::memory_order_release);
}

inline void Logger::init(const std::filesystem::path& log_file, size_t log_queue_size, size_t string_buffer_size) {
    shutdown(); // make sure the logger is stopped
    add_sink(std::make_shared<FileSink>(log_file));
    
    // Ensure queue_size is power of 2
    log_queue_size = round_up_to_power_of_2(log_queue_size);
    string_buffer_size = round_up_to_power_of_2(string_buffer_size);

    log_queue_ = std::make_unique<slick::queue<LogEntry, logger_queue_traits>>(static_cast<uint32_t>(log_queue_size));
    string_queue_ = std::make_unique<slick::queue<char, logger_queue_traits>>(static_cast<uint32_t>(string_buffer_size));
    log_file_ = log_file;
    start();
}

inline void Logger::start() {
    running_ = true;

    // A shared-memory producer has no sinks and no writer thread: the collector
    // process owns both and drains the shared queue on everyone's behalf.
    if (mode_ != QueueMode::SharedProducer) {
        // Initialize read_index_ before starting the thread
        read_index_.store(log_queue_->initial_reading_index(), std::memory_order_relaxed);
        if (mode_ == QueueMode::SharedCollector && collect_backlog_) {
            // Producers may already have published into this segment before the
            // collector attached. Rewind to the oldest slot the ring can still
            // hold so nothing buffered is dropped; anything older than that has
            // been overwritten already.
            const uint64_t capacity = log_queue_->size();
            const uint64_t attached_at = read_index_.load(std::memory_order_relaxed);
            read_index_.store(attached_at > capacity ? attached_at - capacity : 0,
                              std::memory_order_relaxed);
        }

        // No wait for the thread to come up: running_ is already true, and the
        // queue is lock-free, so entries logged before the thread is scheduled
        // simply wait in the ring for it to drain them. The delay that used to
        // stand here could not have ensured anything anyway - a sleep is not a
        // handshake - and on Windows it cost ~15.6ms of every init().
        writer_thread_ = std::thread([this]() { writer_thread_func(); });
    }
    log(LogLevel::L_INFO, "SlickLogger v{}", SLICK_LOGGER_VERSION);
}

inline void Logger::init(const LogConfig& config) {
    shutdown(); // make sure the logger is stopped

    if (config.mode == QueueMode::SharedProducer) {
        // The collector process owns the sinks; a producer that also held sinks
        // would write the same entry twice and defeat the point of centralizing.
        if (!config.sinks.empty()) {
            throw std::runtime_error(
                "SharedProducer mode must not define sinks. The collector process owns the sinks.");
        }
    }
    else if (config.sinks.empty()) {
        throw std::runtime_error("No sink. Sinks should be added in the config.");
    }

    for (auto& sink : config.sinks) {
        add_sink(sink);
    }

    set_level(config.min_level);
    set_source_location_options(config.include_source_location);

    // Ensure queue_size is power of 2
    size_t log_queue_size = round_up_to_power_of_2(config.log_queue_size);
    size_t string_buffer_size = round_up_to_power_of_2(config.string_buffer_size);

    if (config.mode == QueueMode::Local) {
        log_queue_ = std::make_unique<slick::queue<LogEntry, logger_queue_traits>>(static_cast<uint32_t>(log_queue_size));
        string_queue_ = std::make_unique<slick::queue<char, logger_queue_traits>>(static_cast<uint32_t>(string_buffer_size));
    }
    else {
        setup_shared_queues(config, static_cast<uint32_t>(log_queue_size),
                            static_cast<uint32_t>(string_buffer_size));
    }
    start();
}

inline void Logger::validate_shared_memory_name(const std::string& name) {
    // The string ring lives in a companion segment named "<name>_str", and macOS
    // caps shm_open names at 31 characters, so the base name has to leave room.
    constexpr size_t kMaxNameLength = 24;

    if (name.empty()) {
        throw std::runtime_error("shared_memory_name is required when mode is not QueueMode::Local.");
    }
    if (name.size() > kMaxNameLength) {
        throw std::runtime_error("shared_memory_name '" + name + "' is longer than "
                                 + std::to_string(kMaxNameLength) + " characters.");
    }
    for (char c : name) {
        const bool allowed = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                          || (c >= '0' && c <= '9') || c == '_';
        if (!allowed) {
            throw std::runtime_error("shared_memory_name '" + name +
                                     "' may only contain characters in [A-Za-z0-9_].");
        }
    }
}

inline size_t Logger::truncated_tag_length(std::string_view tag) noexcept {
    constexpr size_t kMaxTagBytes = SLICK_LOGGER_TAG_SIZE - 1; // leave room for the NUL
    if (tag.size() <= kMaxTagBytes) {
        return tag.size(); // fits whole, nothing to cut
    }
    // Never cut in the middle of a UTF-8 sequence: walk back from the cut point
    // over continuation bytes (0b10xxxxxx) so the stored tag stays printable.
    // tag[length] is in range here because tag.size() > kMaxTagBytes.
    size_t length = kMaxTagBytes;
    while (length > 0 && (static_cast<unsigned char>(tag[length]) & 0xC0) == 0x80) {
        --length;
    }
    return length;
}

template<typename T>
inline std::unique_ptr<slick::queue<T, Logger::logger_queue_traits>> Logger::open_shared_queue(const std::string& name, uint32_t size) {
    std::string attach_error;
    try {
        // Attach to a segment somebody else already created. Sizing then comes
        // from its header, so a producer automatically matches the collector.
        return std::make_unique<slick::queue<T, logger_queue_traits>>(name.c_str());
    }
    catch (const std::exception& e) {
        // Usually just "nothing there yet". Keep the reason: if creating fails too,
        // reporting only the second error hides why the attach did not work, which
        // matters when the real cause was a creator still initializing rather than
        // a missing segment.
        attach_error = e.what();
    }

    try {
        // Create it. Still create-or-attach, so a process that loses the race to
        // another creator simply attaches instead.
        return std::make_unique<slick::queue<T, logger_queue_traits>>(size, name.c_str());
    }
    catch (const std::exception& e) {
        throw std::runtime_error(std::string(e.what())
                                 + " (attaching to the existing segment first failed with: "
                                 + attach_error + ")");
    }
}

inline void Logger::setup_shared_queues(const LogConfig& config, uint32_t queue_size, uint32_t string_buffer_size) {
    validate_shared_memory_name(config.shared_memory_name);

    shm_name_ = config.shared_memory_name;
    mode_ = config.mode;
    collect_backlog_ = config.collect_backlog;
    stalled_entry_timeout_ms_ = config.stalled_entry_timeout_ms;

    try {
        log_queue_ = open_shared_queue<LogEntry>(shm_name_, queue_size);
        string_queue_ = open_shared_queue<char>(shm_name_ + "_str", string_buffer_size);
    }
    catch (const std::exception& e) {
        log_queue_.reset();
        string_queue_.reset();
        mode_ = QueueMode::Local;
        shm_name_.clear();
        // A size or element-size mismatch means another process attached to the
        // same name with different settings or a different build of the header.
        throw std::runtime_error("Failed to attach shared log queue '" + config.shared_memory_name
                                 + "': " + e.what()
                                 + ". All processes must agree on slick-logger version, "
                                   "SLICK_LOGGER_MAX_ARGS, log_queue_size and string_buffer_size.");
    }

    // String references become ring indices so the collector can resolve them
    // against its own mapping of the segment.
    entry_flags_ = kEntryOffsets;
#ifdef _WIN32
    pid_ = static_cast<uint32_t>(::_getpid());
#else
    pid_ = static_cast<uint32_t>(::getpid());
#endif
    std::memset(tag_, 0, sizeof(tag_));
    std::memcpy(tag_, config.process_tag.data(), truncated_tag_length(config.process_tag));
}

inline void Logger::add_sink(std::shared_ptr<ISink> sink) {
    sink->set_index(static_cast<int>(sinks_.size()));
    sinks_.push_back(sink);
    if (!sink->name().empty()) {
        sinkname_index_map_[sink->name()] = sink->index();
    }
}

inline void Logger::release_sinks() {
    // Detach before dropping. A caller can still hold a shared_ptr to a sink this
    // logger is releasing, and an index left pointing at a slot that a later
    // add_sink() reuses would silently route that stale handle's entries to an
    // unrelated sink. -1 makes ISink::should_log() reject it instead.
    for (auto& sink : sinks_) {
        if (sink) {
            sink->set_index(-1);
        }
    }
    // sinkname_index_map_ holds string_view keys that point into the name strings
    // owned by each sink object - it must be cleared before the sink shared_ptrs
    // are destroyed so no dangling views remain.
    sinkname_index_map_.clear();
    sinks_.clear();
}

inline void Logger::clear_sinks() {
    release_sinks();
}

template<typename SinkT>
inline std::shared_ptr<ISink> Logger::get_sink() const noexcept {
    for (auto &sink : sinks_) {
        if (dynamic_cast<SinkT*>(sink.get()) != nullptr) {
            return sink;
        }
    }
    return nullptr;
}

inline std::shared_ptr<ISink> Logger::get_sink(std::string_view name) const noexcept {
    auto iter = sinkname_index_map_.find(name);
    if (iter != sinkname_index_map_.end()) {
        int index = iter->second;
        if (index >= 0 && static_cast<size_t>(index) < sinks_.size()) [[likely]] {
            return sinks_[index];
        }
    }
    return nullptr;  
}

inline void Logger::init(size_t queue_size, size_t string_buffer_size) {
    shutdown(false); // make sure the logger is stopped

    if (sinks_.empty()) {
        throw std::runtime_error("No sink. Sinks should be added first before calling this.");
    }

    // Initialize logger with pre-set sinks - sinks should be added before calling this
    // Ensure queue_size is power of 2
    queue_size = round_up_to_power_of_2(queue_size);
    string_buffer_size = round_up_to_power_of_2(string_buffer_size);

    log_queue_ = std::make_unique<slick::queue<LogEntry, logger_queue_traits>>(static_cast<uint32_t>(queue_size));
    string_queue_ = std::make_unique<slick::queue<char, logger_queue_traits>>(static_cast<uint32_t>(string_buffer_size));
    start();
}

inline void Logger::add_console_sink(bool use_colors, bool use_stderr_for_errors, std::string&& name) {
    add_sink(std::make_shared<ConsoleSink>(use_colors, use_stderr_for_errors, TimestampFormatter::Format::WITH_MICROSECONDS, std::move(name)));
}

inline void Logger::add_console_sink(TimestampFormatter::Format timestamp_format, bool use_colors, bool use_stderr_for_errors, std::string&& name) {
    add_sink(std::make_shared<ConsoleSink>(use_colors, use_stderr_for_errors, timestamp_format, std::move(name)));
}

inline void Logger::add_console_sink(const std::string& custom_timestamp_format, bool use_colors, bool use_stderr_for_errors, std::string&& name) {
    add_sink(std::make_shared<ConsoleSink>(custom_timestamp_format, use_colors, use_stderr_for_errors, std::move(name)));
}

inline void Logger::add_file_sink(const std::filesystem::path& path, std::string&& name) {
    add_sink(std::make_shared<FileSink>(path, TimestampFormatter::Format::WITH_MICROSECONDS, std::move(name)));
}

inline void Logger::add_file_sink(const std::filesystem::path& path, TimestampFormatter::Format timestamp_format, std::string&& name) {
    add_sink(std::make_shared<FileSink>(path, timestamp_format, std::move(name)));
}

inline void Logger::add_file_sink(const std::filesystem::path& path, const std::string& custom_timestamp_format, std::string&& name) {
    add_sink(std::make_shared<FileSink>(path, custom_timestamp_format, std::move(name)));
}

inline void Logger::add_rotating_file_sink(const std::filesystem::path& path, const RotationConfig& config, std::string&& name) {
    add_sink(std::make_shared<RotatingFileSink>(path, config, std::move(name)));
}

inline void Logger::add_rotating_file_sink(const std::filesystem::path& path, const RotationConfig& config, TimestampFormatter::Format timestamp_format, std::string&& name) {
    add_sink(std::make_shared<RotatingFileSink>(path, config, timestamp_format, std::move(name)));
}

inline void Logger::add_rotating_file_sink(const std::filesystem::path& path, const RotationConfig& config, const std::string& custom_timestamp_format, std::string&& name) {
    add_sink(std::make_shared<RotatingFileSink>(path, config, custom_timestamp_format, std::move(name)));
}

inline void Logger::add_daily_file_sink(const std::filesystem::path& path, const RotationConfig& config, std::string&& name) {
    add_sink(std::make_shared<DailyFileSink>(path, config, std::move(name)));
}

inline void Logger::add_daily_file_sink(const std::filesystem::path& path, const RotationConfig& config, TimestampFormatter::Format timestamp_format, std::string&& name) {
    add_sink(std::make_shared<DailyFileSink>(path, config, timestamp_format, std::move(name)));
}

inline void Logger::add_daily_file_sink(const std::filesystem::path& path, const RotationConfig& config, const std::string& custom_timestamp_format, std::string&& name) {
    add_sink(std::make_shared<DailyFileSink>(path, config, custom_timestamp_format, std::move(name)));
}

inline std::shared_ptr<BinarySink> Logger::add_binary_sink(const std::filesystem::path& path, std::string&& name) {
    auto sink = std::make_shared<BinarySink>(path, std::move(name));
    add_sink(sink);
    return sink;
}

template<typename FormatT, typename... Args>
inline void Logger::log(LogLevel level, FormatT&& format, Args&&... args) {
    log_to_sink_with_location(-1, level, nullptr, 0, false,
                              std::forward<FormatT>(format), std::forward<Args>(args)...);
}

template<typename FormatT, typename... Args>
inline void Logger::log_with_location(LogLevel level, const char* file_path, uint32_t line, FormatT&& format, Args&&... args) {
    const bool include_source_location = (source_location_options_.load(std::memory_order_relaxed) & kSourceLocationEnabled) != 0;
    const char* source_file = include_source_location ? detail::file_name_from_path(file_path) : nullptr;
    log_to_sink_with_location(-1, level,
                              source_file,
                              include_source_location ? line : 0,
                              true,
                              std::forward<FormatT>(format),
                              std::forward<Args>(args)...);
}

template<typename FormatT, typename... Args>
inline void Logger::log_with_static_location(LogLevel level, StaticSourceLocation location, uint32_t line,
                                             FormatT&& format, Args&&... args) {
    log_to_sink_with_static_location(-1, level, location, line,
                                     std::forward<FormatT>(format),
                                     std::forward<Args>(args)...);
}

template<typename FormatT, typename... Args>
inline void Logger::log_to_sink_with_static_location(int sink_index, LogLevel level, StaticSourceLocation location,
                                                     uint32_t line, FormatT&& format, Args&&... args) {
    const bool include_source_location = (source_location_options_.load(std::memory_order_relaxed) & kSourceLocationEnabled) != 0;
    log_to_sink_with_location(sink_index, level,
                              include_source_location ? location.file_name : nullptr,
                              include_source_location ? line : 0,
                              false,
                              std::forward<FormatT>(format),
                              std::forward<Args>(args)...);
}

// #define IS_STRING_LITERAL(x) ([&]<class T = char>() { \
//     return std::is_same_v<decltype(x), T const (&)[sizeof(x)]> && \
//     requires { std::type_identity_t<T[sizeof(x) + 1]>{x}; }; }())

#define IS_STRING_LITERAL(x) ([&]<class U = char>() { \
    return std::is_same_v<decltype(x), U const (&)[sizeof(x)]>; }()) 

template<typename FormatT, typename... Args>
inline void Logger::log_to_sink(int sink_index, LogLevel level, FormatT&& format, Args&&... args) {
    log_to_sink_with_location(sink_index, level, nullptr, 0, false,
                              std::forward<FormatT>(format), std::forward<Args>(args)...);
}

template<typename FormatT, typename... Args>
inline void Logger::log_to_sink_with_location(int sink_index, LogLevel level, const char* file_name, uint32_t line,
                                              bool copy_file_name,
                                              FormatT&& format, Args&&... args) {
    if (!running_.load(std::memory_order_relaxed) || !log_queue_ || level < log_level_.load(std::memory_order_relaxed))
    {
        return;
    }

    auto now = std::chrono::system_clock::now();
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();

    // In shared-memory mode every string reference must be a ring index, because
    // the segment maps at a different base address in each attached process.
    const bool use_offsets = (entry_flags_ & kEntryOffsets) != 0;

    LogEntry entry;
    entry.level = level;
    entry.timestamp = ns;
    entry.flags = entry_flags_;
    entry.pid = pid_;
    std::memcpy(entry.tag, tag_, sizeof(entry.tag));
    if (has_source_location(file_name, line)) {
        // A static file name lives in the caller's read-only data, which no other
        // process can address, so shared mode always copies it into the ring.
        entry.file = (copy_file_name || use_offsets)
            ? store_string_in_queue(file_name)
            : StringRef{file_name, 0};
        entry.line = line;
        entry.flags |= kEntryHasSourceLocation;
    }
    entry.sink_index = sink_index;
    if constexpr (IS_STRING_LITERAL(format)) {
        constexpr uint32_t format_length = static_cast<uint32_t>(sizeof(format) - 1);
        entry.format = use_offsets
            ? store_string_in_queue(std::string_view{format, format_length})
            : StringRef{format, format_length};  // String literal - safe to store pointer
    }
    else {
        // Non-literal format strings are copied into the string queue so their
        // lifetime extends until the writer thread consumes the entry.
        static_assert(std::is_convertible_v<FormatT, std::string_view>,
                      "Format string type must be a string literal or convertible to std::string_view.");
        entry.format = store_string_in_queue(
            std::string_view{std::forward<FormatT>(format)});
    }

    if constexpr (is_single_format_args_v<Args...>) {
        // Pre-built std::format_args: unpack values into the entry on the
        // calling thread (format_args holds non-owning references).
        enqueue_format_args(entry,
            std::get<0>(std::forward_as_tuple(std::forward<Args>(args)...)));
    } else {
        // Normal path: push individual arguments
        entry.arg_count = sizeof...(args);
        size_t arg_idx = 0;
        static_assert(sizeof...(args) <= SLICK_LOGGER_MAX_ARGS, "Too many log arguments");
        (enqueue_argument(entry.args[arg_idx++], std::forward<Args>(args)), ...);
    }



    uint64_t index = log_queue_->reserve();
    *(*log_queue_)[index] = std::move(entry);
    log_queue_->publish(index);
    wake_writer();
}

template<typename T>
inline void Logger::enqueue_argument(LogArgument& arg, T&& value) {
    using DecayedT = std::decay_t<T>;
    using BareT = std::remove_reference_t<T>;

    if constexpr (std::is_same_v<DecayedT, bool>) {
        arg.type = ArgType::BOOL;
        arg.value.b = value;
    }
    else if constexpr (std::is_same_v<DecayedT, char>) {
        arg.type = ArgType::CHAR;
        arg.value.c = value;
    }
    else if constexpr (std::is_same_v<DecayedT, unsigned char>) {
        arg.type = ArgType::U_CHAR;
        arg.value.uc = value;
    }
    else if constexpr (std::is_same_v<DecayedT, wchar_t>) {
        arg.type = ArgType::WCHAR;
        arg.value.wc = value;
    }
    else if constexpr (std::is_integral_v<DecayedT>) {
        switch (sizeof(DecayedT)) {
        case sizeof(int8_t):
            if (std::is_signed_v<DecayedT>) {
                arg.type = ArgType::INT8_T;
                arg.value.i8 = static_cast<int8_t>(value);
            } else {
                arg.type = ArgType::UINT8_T;
                arg.value.u8 = static_cast<uint8_t>(value);
            }
            return;
        case sizeof(int16_t):
            if (std::is_signed_v<DecayedT>) {
                arg.type = ArgType::INT16_T;
                arg.value.i16 = static_cast<int16_t>(value);
            } else {
                arg.type = ArgType::UINT16_T;
                arg.value.u16 = static_cast<uint16_t>(value);
            }
            return;
        case sizeof(int32_t):
            if (std::is_signed_v<DecayedT>) {
                arg.type = ArgType::INT32_T;
                arg.value.i32 = static_cast<int32_t>(value);
            } else {
                arg.type = ArgType::UINT32_T;
                arg.value.u32 = static_cast<uint32_t>(value);
            }
            return;
        case sizeof(int64_t):
            if (std::is_signed_v<DecayedT>) {
                arg.type = ArgType::INT64_T;
                arg.value.i64 = static_cast<int64_t>(value);
            } else {
                arg.type = ArgType::UINT64_T;
                arg.value.u64 = static_cast<uint64_t>(value);
            }
            return;
        default:
            // larger integral types? - convert to string
            arg.type = ArgType::STRING_DYNAMIC;
            arg.value.dynamic_str = store_string_in_queue(std::to_string(value));
            return;
        }
    }
    else if constexpr (std::is_floating_point_v<DecayedT>) {
        switch (sizeof(DecayedT)) {
        case sizeof(float):
            arg.type = ArgType::FLOAT;
            arg.value.f = static_cast<float>(value);
            return;
        case sizeof(double):    
            arg.type = ArgType::DOUBLE;
            arg.value.d = static_cast<double>(value);
            return;
        }
    }
    else if constexpr (std::is_enum_v<DecayedT>) {
        enqueue_argument(arg, static_cast<std::underlying_type_t<DecayedT>>(value));
    }
    else if constexpr (std::is_same_v<DecayedT, std::chrono::system_clock::time_point>) {
        arg.type = ArgType::INT64_T;
        arg.value.i64 = std::chrono::duration_cast<std::chrono::nanoseconds>(value.time_since_epoch()).count();
    }
    else if constexpr (std::is_same_v<DecayedT, BinaryView>) {
        // Raw bytes: copied into the string ring like any other dynamic payload,
        // but without a terminator. The stored length is what makes them readable
        // back, since embedded NULs are legitimate payload. Stored even when empty
        // so shared-memory entries always carry a resolvable ring index rather
        // than a null pointer.
        arg.type = ArgType::BLOB;
        arg.value.dynamic_str = store_binary_in_queue(value);
    }
    else if constexpr (std::is_array_v<BareT> &&
                       std::is_same_v<std::remove_cv_t<std::remove_extent_t<BareT>>, char>) {
        // value may be cv-qualified (e.g. a volatile struct member); strip
        // qualifiers for the read since we only need a defensive snapshot.
        const char* data = const_cast<const char*>(static_cast<const volatile char*>(value));
        constexpr size_t extent = std::extent_v<BareT>;
        size_t length = strnlen(data, extent);
        arg.type = ArgType::STRING_DYNAMIC;
        arg.value.dynamic_str = store_string_in_queue(std::string_view{data, length});
    }
    else if constexpr (std::is_same_v<DecayedT, const char*>) {
        arg.type = ArgType::STRING_DYNAMIC;
        arg.value.dynamic_str = store_string_in_queue(value);
    }
    else if constexpr (std::is_same_v<DecayedT, char*>) {
        arg.type = ArgType::STRING_DYNAMIC;
        arg.value.dynamic_str = store_string_in_queue(value);
    }
    else if constexpr (std::is_same_v<DecayedT, std::string>) {
        // Dynamic string - copy to string queue
        arg.type = ArgType::STRING_DYNAMIC;
        arg.value.dynamic_str = store_string_in_queue(value);
    }
    else if constexpr (std::is_same_v<DecayedT, std::string_view>) {
        // Could be either - need to determine at runtime or copy to be safe       
        arg.type = ArgType::STRING_DYNAMIC;
        arg.value.dynamic_str = store_string_in_queue(value);
    }
    else if constexpr (std::is_same_v<DecayedT, const void*>) {
        arg.type = ArgType::PTR;
        arg.value.ptr = const_cast<void*>(value);
    }
    else if constexpr (std::is_pointer_v<DecayedT>) {
        arg.type = ArgType::PTR;
        // Strip any cv-qualification on the pointee; we only store the
        // address, never dereference it, so const/volatile don't matter.
        arg.value.ptr = const_cast<void*>(static_cast<const volatile void*>(value));
    }
    else {
        // custom type - convert to string
        arg.type = ArgType::STRING_DYNAMIC;
        arg.value.dynamic_str = store_string_in_queue(std::to_string(value));
    }
}

inline StringRef Logger::store_bytes_in_queue(const char* data, size_t size, bool terminate) {
    // slick-queue packs the reservation size into 16 bits, so an over-long
    // reservation would corrupt the write cursor. Truncate instead.
    uint32_t length = static_cast<uint32_t>(std::min<size_t>(size, kMaxPayloadBytes));
    // slick-queue rejects a zero-size reservation, so an empty unterminated
    // payload still claims one byte. That keeps the invariant every reader
    // depends on - a stored reference always resolves to a real ring slot - at
    // the cost of one byte in a case that carries no data anyway.
    uint32_t reserved = std::max(1u, length + (terminate ? 1u : 0u));

    uint64_t start_index = string_queue_->reserve(reserved);

    char* dest = (*string_queue_)[start_index];
    if (length) {
        std::memcpy(dest, data, length);
    }
    if (terminate) {
        // Copy only the payload, then terminate explicitly.
        dest[length] = '\0';
    }
    string_queue_->publish(start_index, reserved);

    StringRef ref;
    if (entry_flags_ & kEntryOffsets) {
        // Shared memory: hand out the ring index, which every attached process
        // can resolve against its own mapping.
        ref.offset = start_index;
    }
    else {
        ref.ptr = dest;
    }
    ref.length = length;
    return ref;
}

inline StringRef Logger::store_string_in_queue(std::string_view str) {
    // Strings are terminated: readers reach them through view_string_ref(), where
    // a length of 0 means "NUL-terminated, length unknown".
    return store_bytes_in_queue(str.data(), str.length(), true);
}

inline StringRef Logger::store_binary_in_queue(BinaryView payload) {
    // Binary payloads are not terminated. view_binary() treats the stored length
    // as authoritative - a payload can contain NUL bytes, so a terminator would
    // mean nothing to a reader - and skipping it saves a byte of ring space and
    // a store per binary argument.
    return store_bytes_in_queue(reinterpret_cast<const char*>(payload.data), payload.size, false);
}

inline void Logger::enqueue_format_args(LogEntry& entry, std::format_args fa) {
    size_t arg_idx = 0;
    while (arg_idx < SLICK_LOGGER_MAX_ARGS) {
        auto arg = fa.get(arg_idx);
        bool is_monostate = false;
        std::visit_format_arg([&]<typename T>(T&& v) {
            using DT = std::decay_t<T>;
            if constexpr (std::is_same_v<DT, std::monostate>) {
                is_monostate = true;
            } else if constexpr (std::is_same_v<DT, typename std::basic_format_arg<std::format_context>::handle>) {
                // Custom type via handle: std::format_context is not publicly
                // constructible, so store a placeholder instead.
                enqueue_argument(entry.args[arg_idx], std::string_view{"<handle>"});
#ifdef __SIZEOF_INT128__
            } else if constexpr (std::is_same_v<DT, __int128>) {
                enqueue_argument(entry.args[arg_idx], static_cast<long long>(v));
            } else if constexpr (std::is_same_v<DT, unsigned __int128>) {
                enqueue_argument(entry.args[arg_idx], static_cast<unsigned long long>(v));
#endif
            } else if constexpr (is_float128_v<DT>) {
                enqueue_argument(entry.args[arg_idx], static_cast<double>(v));
            } else {
                enqueue_argument(entry.args[arg_idx], std::forward<T>(v));
            }
        }, arg);
        if (is_monostate) break;
        ++arg_idx;
    }
    entry.arg_count = static_cast<uint8_t>(arg_idx);
}

inline void Logger::retain_shared_queue(void* queue) noexcept {
    if (!queue) {
        return;
    }
    struct RetainedQueue {
        void* queue;
        RetainedQueue* next;
    };
    // Never emptied, and the head is a static so leak detectors treat it as a
    // root: that is what keeps the retained queues reachable rather than looking
    // like lost allocations. A by-value container would be destroyed at exit, and
    // its ordering against the detector's final scan is not guaranteed.
    static std::atomic<RetainedQueue*> head{nullptr};

    auto* node = new (std::nothrow) RetainedQueue{queue, nullptr};
    if (!node) {
        // The queue is retained regardless; the registry only records it.
        return;
    }
    node->next = head.load(std::memory_order_relaxed);
    while (!head.compare_exchange_weak(node->next, node,
                                       std::memory_order_release,
                                       std::memory_order_relaxed)) {
    }
}

inline void Logger::shutdown(bool clear_sinks) {
    if (running_.load(std::memory_order_relaxed)) {
        running_.store(false, std::memory_order_release);
        // The writer thread may be parked. atomic::wait has no timeout, so it
        // only ever resumes because the token changed - without this the join
        // below would block until something else happened to be logged.
        force_wake_writer();
        if (writer_thread_.joinable()) {
            writer_thread_.join();
        }
    }
    
    if (clear_sinks) {
        // Release sinks to free file handles and other resources, and to detach
        // any handle the caller still holds. Safe here: the writer thread, the
        // only other user of sinks_, has been joined above.
        release_sinks();
    }
#ifndef _WIN32
    // POSIX only: slick-queue's destructor shm_unlink()s a segment this process
    // created. Unlinking frees the NAME while existing mappings stay valid, so any
    // process still attached would keep draining memory that newcomers can no
    // longer reach - they would create a fresh segment under the same name and
    // their entries would silently vanish. Producer-first startup makes this easy
    // to hit: a short-lived first producer would strand the collector.
    //
    // Nobody can safely unlink while others might still attach, so the creating
    // handle is retained for the life of the process instead of destroyed. The OS
    // reclaims the mapping at exit; the name persists until removed (see the POSIX
    // cleanup note in the README).
    //
    // use_shm() must be checked as well: a local heap queue also reports
    // own_buffer() == true, and retaining that one would hold the whole buffer.
    if (log_queue_ && log_queue_->use_shm() && log_queue_->own_buffer()) {
        retain_shared_queue(log_queue_.release());
    }
    if (string_queue_ && string_queue_->use_shm() && string_queue_->own_buffer()) {
        retain_shared_queue(string_queue_.release());
    }
#endif
    log_queue_.reset();
    string_queue_.reset();

    mode_ = QueueMode::Local;
    entry_flags_ = 0;
    pid_ = 0;
    std::memset(tag_, 0, sizeof(tag_));
    shm_name_.clear();
    collect_backlog_ = true;
    stalled_entry_timeout_ms_ = 0;
}

inline Logger::~Logger() {
    shutdown();
}

inline void Logger::flush() {
    if (!running_.load(std::memory_order_relaxed) || !log_queue_) {
        return;
    }
    if (mode_ == QueueMode::SharedProducer) {
        // No local writer thread: entries are published to the shared queue the
        // moment log() returns, and only the collector process can observe them
        // reaching a sink. Publishing is all this process can guarantee.
        return;
    }
    // Snapshot the write cursor: any entry reserved before this point has
    // already been assigned an index below drain_target.
    const uint64_t drain_target = log_queue_->initial_reading_index();
    // Wait until the writer thread's read cursor reaches drain_target.
    force_wake_writer(); // the entries may have arrived while the writer was parked
    wait_until_ready([&] {
        return read_index_.load(std::memory_order_relaxed) >= drain_target;
    });

    // Consuming an entry only hands it to a sink; the sink may still be holding it
    // in a stream buffer. Ask the writer thread - the only thread allowed to touch
    // the sinks - to flush, and wait for it to acknowledge this request.
    const uint64_t generation = flush_request_.fetch_add(1, std::memory_order_acq_rel) + 1;
    force_wake_writer(); // it may be parked, and would not see the request otherwise
    wait_until_ready([&] { return flush_done_.load(std::memory_order_acquire) >= generation; });
}

inline void Logger::reset() {
    shutdown();
    // Reset all state for fresh initialization
    log_file_.clear();
    read_index_.store(0, std::memory_order_relaxed);
    log_level_.store(LogLevel::L_TRACE);
    source_location_options_.store(kSourceLocationEnabled, std::memory_order_relaxed);

    // shutdown() has joined the writer thread, so nothing else can be touching the
    // wake and flush state by now. None of this is required for correctness: the
    // counters are monotonic, the writer compares flush_request_ against
    // flush_done_ for inequality rather than order (so it resynchronises on its
    // first iteration whatever they hold), and wake_token_ is only ever compared
    // for change. Clearing them simply leaves a reused Logger indistinguishable
    // from a fresh one, which makes init()/reset() cycles easier to reason about.
    // Keep flush_request_ and flush_done_ together all the same, so the
    // flush_done_ <= flush_request_ invariant is obvious at a glance.
    flush_request_.store(0, std::memory_order_relaxed);
    flush_done_.store(0, std::memory_order_relaxed);
    wake_token_.store(0, std::memory_order_relaxed);
    writer_parked_.store(false, std::memory_order_relaxed);
}

inline Logger::DrainResult Logger::drain_pending() {
    // read() takes a plain uint64_t& and advances it in place. The atomic must
    // not be passed directly: queue also has a read(std::atomic<uint64_t>&)
    // overload for multiple consumers sharing one cursor, and binding that by
    // accident would change how slots are claimed. So the cursor is round-tripped
    // through a local and republished at exactly the point read() used to move it.
    uint64_t cursor = read_index_.load(std::memory_order_relaxed);
    auto [entry_ptr, count] = log_queue_->read(cursor);
    read_index_.store(cursor, std::memory_order_relaxed);

    if (entry_ptr && count) {
        write_log_entry(entry_ptr, count);
        stalled_since_ = {};
        return DrainResult::Wrote;
    }

    // A producer process killed between reserve() and publish() leaves a hole that
    // read() will never return, stalling every entry behind it. Only shared queues
    // can be orphaned this way, so the recovery is scoped to the collector role.
    const bool recover_stalls = (mode_ == QueueMode::SharedCollector) && stalled_entry_timeout_ms_ > 0;
    if (!recover_stalls || log_queue_->initial_reading_index() <= cursor) {
        stalled_since_ = {};
        return DrainResult::Idle;
    }

    // Entries are reserved ahead of us but this slot never got published.
    const auto now = std::chrono::steady_clock::now();
    if (stalled_since_ == std::chrono::steady_clock::time_point{}) {
        stalled_since_ = now;
        return DrainResult::WaitingOnStall;
    }
    if (now - stalled_since_ < std::chrono::milliseconds(stalled_entry_timeout_ms_)) {
        return DrainResult::WaitingOnStall;
    }

    read_index_.store(cursor + 1, std::memory_order_relaxed); // abandon the stalled slot
    stalled_since_ = {};
    return DrainResult::SkippedStalled;
}

inline void Logger::writer_thread_func() {
    stalled_since_ = {};

    // Sinks are flushed when the queue runs dry rather than after every batch.
    // Flushing per batch meant one fflush per entry whenever the writer thread
    // kept up with the producers, which cost far more than formatting the entry.
    // Draining to empty and then flushing preserves what callers can observe -
    // once the logger is caught up, everything logged is on disk - while letting
    // a burst share a single flush.
    bool sinks_dirty = false;

    // Idle backoff. std::this_thread::sleep_for is deliberately not used here:
    // Windows' default timer resolution is ~15.6ms, so a "1ms" sleep really
    // parks the thread for ~15.6ms and puts that much latency between a log call
    // and the entry reaching its sink. Instead the thread stays hot for a short
    // burst - covering the gaps in an active logging stream - and then parks on
    // wake_token_ until a producer, a flush request, or shutdown releases it.
    static constexpr uint32_t kIdleYieldLimit = 64;
    uint32_t idle_yields = 0;

    // Parking only works when the producers share this process: writer_parked_
    // lives in this address space, and neither WaitOnAddress nor a futex on a
    // private mapping can be signalled by another process. A collector therefore
    // keeps polling for entries that cross a process boundary. mode_ is fixed
    // before this thread starts and does not change while it runs.
    const bool can_park = (mode_ == QueueMode::Local);

    while (running_.load(std::memory_order_relaxed)) {
        // Read the request before flushing so a request arriving mid-flush is not
        // mistaken for one this flush already covered.
        const uint64_t flush_requested = flush_request_.load(std::memory_order_acquire);
        if (flush_requested != flush_done_.load(std::memory_order_relaxed)) {
            flush_sinks();
            sinks_dirty = false;
            flush_done_.store(flush_requested, std::memory_order_release);
        }

        switch (drain_pending()) {
        case DrainResult::Wrote:
            sinks_dirty = true;
            idle_yields = 0;
            break;
        case DrainResult::SkippedStalled:
            log(LogLevel::L_WARN, "SlickLogger: skipped an unpublished log entry, "
                                  "a producer process likely died mid-write");
            idle_yields = 0;
            break;
        case DrainResult::WaitingOnStall:
            // A dead producer's slot clears by timeout rather than by a wake-up,
            // so this must keep re-checking instead of parking. It is a rare,
            // multi-second path where the coarse sleep does no harm.
            if (sinks_dirty) {
                flush_sinks();
                sinks_dirty = false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            break;
        case DrainResult::Idle:
            if (sinks_dirty) {
                flush_sinks(); // caught up: make everything written so far visible
                sinks_dirty = false;
            }
            if (idle_yields < kIdleYieldLimit) {
                ++idle_yields;
                std::this_thread::yield();
            } else if (can_park) {
                park_writer();
                idle_yields = 0;
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            break;
        }
    }

    // Drain remaining messages after running_ becomes false. A hole must be
    // abandoned here too, not just waited out: entries published behind it are
    // still valid and would otherwise be lost on shutdown. Skipping cannot loop
    // forever because every skip advances read_index_ toward the write cursor.
    // Nothing is logged about a skip during the drain, because running_ is already
    // false and the entry could never be consumed.
    stalled_since_ = {};
    while (true) {
        const DrainResult result = drain_pending();
        if (result == DrainResult::Idle) {
            break;
        }
        if (result == DrainResult::WaitingOnStall) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    // The writer thread owns the sinks, so this final flush is the last chance to
    // get the drained entries onto disk before shutdown() joins and returns.
    flush_sinks();
    flush_done_.store(flush_request_.load(std::memory_order_acquire), std::memory_order_release);
}

inline void Logger::rebase_entry(LogEntry& entry) const noexcept {
    entry.format.ptr = (*string_queue_)[entry.format.offset];
    if (entry.flags & kEntryHasSourceLocation) {
        entry.file.ptr = (*string_queue_)[entry.file.offset];
    }
    for (uint8_t i = 0; i < entry.arg_count; ++i) {
        LogArgument& arg = entry.args[i];
        if (arg.type == ArgType::STRING_DYNAMIC || arg.type == ArgType::STRING_LITERAL ||
            arg.type == ArgType::BLOB) {
            arg.value.dynamic_str.ptr = (*string_queue_)[arg.value.dynamic_str.offset];
        }
    }
    entry.flags &= static_cast<uint8_t>(~kEntryOffsets);
}

inline void Logger::write_log_entry(const LogEntry* entry_ptr, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) {
        const LogEntry& entry = entry_ptr[i];
        if (entry.flags & kEntryOffsets) {
            // Came from shared memory: copy it out and turn the ring indices into
            // addresses valid here, so sinks always see resolved pointers.
            rebase_scratch_ = entry;
            rebase_entry(rebase_scratch_);
            dispatch_entry(rebase_scratch_);
        }
        else {
            dispatch_entry(entry);
        }
    }
}

inline void Logger::flush_sinks() {
    for (auto& sink : sinks_) {
        if (sink) {
            sink->flush();
        }
    }
}

template<typename Predicate>
inline void Logger::wait_until_ready(Predicate ready) {
    // Long enough to cover a normal drain without sleeping, short enough that a
    // genuinely slow wait stops burning a core.
    static constexpr uint32_t kCallerYieldLimit = 1024;
    for (uint32_t spins = 0; ; ++spins) {
        if (ready() || !running_.load(std::memory_order_relaxed)) {
            return;
        }
        if (spins < kCallerYieldLimit) {
            std::this_thread::yield();
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
}

inline void Logger::wake_writer() noexcept {
    if (writer_parked_.load(std::memory_order_seq_cst)) [[unlikely]] {
        force_wake_writer();
    }
}

inline void Logger::force_wake_writer() noexcept {
    wake_token_.fetch_add(1, std::memory_order_release);
    wake_token_.notify_all();
}

inline void Logger::park_writer() {
    // Read the token before announcing the park: a producer that bumps it in the
    // window between here and wait() leaves the value different from the one
    // passed in, so wait() returns immediately instead of missing the wake-up.
    const uint32_t token = wake_token_.load(std::memory_order_acquire);
    writer_parked_.store(true, std::memory_order_seq_cst);

    // Re-check everything the park waits for. Anything that became true before
    // writer_parked_ became visible would otherwise never wake us.
    const bool work_pending =
        !running_.load(std::memory_order_relaxed) ||
        flush_request_.load(std::memory_order_acquire) != flush_done_.load(std::memory_order_relaxed) ||
        (log_queue_ && log_queue_->initial_reading_index() !=
                           read_index_.load(std::memory_order_relaxed));

    if (!work_pending) {
        wake_token_.wait(token, std::memory_order_acquire);
    }
    writer_parked_.store(false, std::memory_order_release);
}

inline void Logger::dispatch_entry(const LogEntry& entry) {
    if (entry.sink_index >= 0 && static_cast<size_t>(entry.sink_index) < sinks_.size()) {
        // Write to specific sink
        auto& sink = sinks_[entry.sink_index];
        if (entry.level < sink->min_level()) {
            return; // Skip if log level is below sink's minimum level
        }
        sink->write(entry);
    }
    else {
        // Write to all non-dedicated sinks
        for (auto& sink : sinks_) {
            if (entry.level < sink->min_level() || sink->is_dedicated()) {
                continue; // Skip if log level is below sink's minimum level or sink is dedicated
            }
            sink->write(entry);
        }
    }
}

inline size_t Logger::round_up_to_power_of_2(size_t value) noexcept {
    if (value & (value - 1)) {
        // Round up to next power of 2
        size_t temp = value;
        temp--;
        temp |= temp >> 1;
        temp |= temp >> 2;
        temp |= temp >> 4;
        temp |= temp >> 8;
        temp |= temp >> 16;

#if defined(_M_X64) || defined(__x86_64__)
        if constexpr (sizeof(size_t) > 4) {
            temp |= temp >> 32;
        }
#endif
        return temp + 1;
    }
    return value; // Already a power of 2
}

} // namespace slick::logger

// Macros for easy logging
#if SLICK_LOGGER_ENABLE_SOURCE_LOCATION
#define SLICK_LOGGER_LOG_AT_CALL_SITE(logger_instance, level, ...) \
    do { \
        static constexpr const char* slick_logger_file_name__ = SLICK_LOGGER_FILE_NAME; \
        (logger_instance).log_with_static_location( \
            level, \
            slick::logger::Logger::static_source_location(slick_logger_file_name__), \
            __LINE__, __VA_ARGS__); \
    } while (false)
#else
#define SLICK_LOGGER_LOG_AT_CALL_SITE(logger_instance, level, ...) \
    (logger_instance).log(level, __VA_ARGS__)
#endif

#define SLICK_LOGGER_LOG_IF_ENABLED(level, ...)                  \
    do {                                                         \
        auto& slick_logger_instance__ = slick::logger::Logger::instance(); \
        if (slick_logger_instance__.should_log(level)) {         \
            SLICK_LOGGER_LOG_AT_CALL_SITE(slick_logger_instance__, level, __VA_ARGS__); \
        }                                                        \
    } while (false)

#define LOG_TRACE(...) SLICK_LOGGER_LOG_IF_ENABLED(slick::logger::LogLevel::L_TRACE, __VA_ARGS__)
#define LOG_DEBUG(...) SLICK_LOGGER_LOG_IF_ENABLED(slick::logger::LogLevel::L_DEBUG, __VA_ARGS__)
#define LOG_INFO(...) SLICK_LOGGER_LOG_IF_ENABLED(slick::logger::LogLevel::L_INFO, __VA_ARGS__)
#define LOG_WARN(...) SLICK_LOGGER_LOG_IF_ENABLED(slick::logger::LogLevel::L_WARN, __VA_ARGS__)
#define LOG_ERROR(...) SLICK_LOGGER_LOG_IF_ENABLED(slick::logger::LogLevel::L_ERROR, __VA_ARGS__)
#define LOG_FATAL(...) SLICK_LOGGER_LOG_IF_ENABLED(slick::logger::LogLevel::L_FATAL, __VA_ARGS__)

// ---- Sink-targeted logging ----
//
// LOG_SINK_<LEVEL>(sink, ...) routes to one sink and, unlike sink->log_<level>(...),
// checks both the sink's own minimum level and the global level BEFORE the
// arguments appear in the expansion - so a filtered-out call evaluates nothing.
// It also attaches the call site, which the direct sink helpers cannot.
//
// `sink` may be an ISink reference, a raw pointer, or a smart pointer, and is
// evaluated exactly once.

#if SLICK_LOGGER_ENABLE_SOURCE_LOCATION
#define SLICK_LOGGER_LOG_TO_SINK_AT_CALL_SITE(logger_instance, sink_index, level, ...) \
    do { \
        static constexpr const char* slick_logger_file_name__ = SLICK_LOGGER_FILE_NAME; \
        (logger_instance).log_to_sink_with_static_location( \
            sink_index, \
            level, \
            slick::logger::Logger::static_source_location(slick_logger_file_name__), \
            __LINE__, __VA_ARGS__); \
    } while (false)
#else
#define SLICK_LOGGER_LOG_TO_SINK_AT_CALL_SITE(logger_instance, sink_index, level, ...) \
    (logger_instance).log_to_sink(sink_index, level, __VA_ARGS__)
#endif

#define SLICK_LOGGER_LOG_TO_SINK_IF_ENABLED(sink, level, ...)            \
    do {                                                                 \
        auto* slick_logger_sink__ = slick::logger::detail::sink_ptr(sink); \
        auto& slick_logger_instance__ = slick::logger::Logger::instance(); \
        if (slick_logger_sink__ && slick_logger_sink__->should_log(slick_logger_instance__, level)) { \
            SLICK_LOGGER_LOG_TO_SINK_AT_CALL_SITE(slick_logger_instance__, \
                slick_logger_sink__->index(), level, __VA_ARGS__);       \
        }                                                                \
    } while (false)

#define LOG_SINK_TRACE(sink, ...) SLICK_LOGGER_LOG_TO_SINK_IF_ENABLED(sink, slick::logger::LogLevel::L_TRACE, __VA_ARGS__)
#define LOG_SINK_DEBUG(sink, ...) SLICK_LOGGER_LOG_TO_SINK_IF_ENABLED(sink, slick::logger::LogLevel::L_DEBUG, __VA_ARGS__)
#define LOG_SINK_INFO(sink, ...) SLICK_LOGGER_LOG_TO_SINK_IF_ENABLED(sink, slick::logger::LogLevel::L_INFO, __VA_ARGS__)
#define LOG_SINK_WARN(sink, ...) SLICK_LOGGER_LOG_TO_SINK_IF_ENABLED(sink, slick::logger::LogLevel::L_WARN, __VA_ARGS__)
#define LOG_SINK_ERROR(sink, ...) SLICK_LOGGER_LOG_TO_SINK_IF_ENABLED(sink, slick::logger::LogLevel::L_ERROR, __VA_ARGS__)
#define LOG_SINK_FATAL(sink, ...) SLICK_LOGGER_LOG_TO_SINK_IF_ENABLED(sink, slick::logger::LogLevel::L_FATAL, __VA_ARGS__)
