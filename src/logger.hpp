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
#include <cstdio>
#include <algorithm>
#include <thread>
#include <atomic>
#include <filesystem>
#include <cassert>
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
#include <deque>
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

// Stamp the producing thread id into every entry, for the %t pattern flag.
// Turning this off drops the per-call capture - one thread-local read - and
// leaves every entry carrying thread id 0. It does NOT change the LogEntry
// layout: the field is always reserved, so processes sharing a segment stay
// attach-compatible whatever each of them chose here.
#ifndef SLICK_LOGGER_ENABLE_THREAD_ID
#define SLICK_LOGGER_ENABLE_THREAD_ID 1
#endif

#if SLICK_LOGGER_ENABLE_THREAD_ID
// The real OS thread id is what a debugger, `top -H` and ETW show, so it is worth
// a little platform code rather than hashing std::thread::id. GetCurrentThreadId
// is declared here rather than by including <windows.h>, which this header
// deliberately keeps out of every translation unit that logs. The declaration is
// identical to the SDK's, so including <windows.h> either side of this is fine.
#  if defined(_WIN32)
extern "C" __declspec(dllimport) unsigned long __stdcall GetCurrentThreadId(void);
#  elif defined(__linux__)
#    include <sys/syscall.h>
#  elif defined(__APPLE__)
#    include <pthread.h>
#  endif
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
 * @brief Write a value in [0, 999] as three ASCII digits.
 */
inline void write_3_digits(char* out, uint32_t value) noexcept {
    out[0] = static_cast<char>('0' + value / 100);
    write_2_digits(out + 1, value % 100);
}

/**
 * @brief Write nanoseconds in [0, 999999999] as nine ASCII digits.
 */
inline void write_9_digits(char* out, uint32_t value) noexcept {
    out[0] = static_cast<char>('0' + value / 100000000);
    write_2_digits(out + 1, (value / 1000000) % 100);
    write_2_digits(out + 3, (value / 10000) % 100);
    write_2_digits(out + 5, (value / 100) % 100);
    write_2_digits(out + 7, value % 100);
}

/**
 * @brief Append a value in decimal, without an intermediate std::string
 *
 * std::to_string() would allocate for anything past the small-string buffer and
 * then copy; this writes the digits straight into the caller's buffer.
 */
inline void append_uint(std::string& out, uint64_t value) {
    char buf[20];
    char* const end = buf + sizeof(buf);
    char* p = end;
    do {
        *--p = static_cast<char>('0' + (value % 10));
        value /= 10;
    } while (value != 0);
    out.append(p, static_cast<size_t>(end - p));
}

// Day and month names for the %a/%A/%b/%B pattern flags. Static ASCII tables
// rather than strftime: the C locale spellings are the only ones a log file
// should carry, and a table lookup keeps these flags as cheap as the rest.
inline const char* weekday_short(int wday) noexcept {
    static constexpr const char* kNames[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    return (wday >= 0 && wday < 7) ? kNames[wday] : "???";
}

inline const char* weekday_long(int wday) noexcept {
    static constexpr const char* kNames[7] = {"Sunday", "Monday", "Tuesday", "Wednesday",
                                              "Thursday", "Friday", "Saturday"};
    return (wday >= 0 && wday < 7) ? kNames[wday] : "???";
}

inline const char* month_short(int mon) noexcept {
    static constexpr const char* kNames[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                               "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    return (mon >= 0 && mon < 12) ? kNames[mon] : "???";
}

inline const char* month_long(int mon) noexcept {
    static constexpr const char* kNames[12] = {"January", "February", "March", "April",
                                               "May", "June", "July", "August",
                                               "September", "October", "November", "December"};
    return (mon >= 0 && mon < 12) ? kNames[mon] : "???";
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

#if SLICK_LOGGER_ENABLE_THREAD_ID
/**
 * @brief This thread's OS thread id, resolved once per thread
 *
 * Stamped into every entry on the producing thread, so it costs one TLS read on
 * the logging path and nothing else.
 */
inline uint32_t current_thread_id() noexcept {
    thread_local const uint32_t id = []() noexcept -> uint32_t {
    #if defined(_WIN32)
        return static_cast<uint32_t>(::GetCurrentThreadId());
    #elif defined(__linux__)
        return static_cast<uint32_t>(::syscall(SYS_gettid));
    #elif defined(__APPLE__)
        uint64_t tid = 0;
        ::pthread_threadid_np(nullptr, &tid);
        return static_cast<uint32_t>(tid);
    #else
        // No portable OS thread id: a hash is still stable and unique among live
        // threads, it just will not match what a debugger shows.
        return static_cast<uint32_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
    #endif
    }();
    return id;
}
#endif

/**
 * @brief This process's id, resolved once
 *
 * Local-mode entries carry pid 0, so the %P pattern flag falls back to this to
 * print something meaningful rather than a bare zero.
 */
inline uint32_t current_process_id() noexcept {
    static const uint32_t id = []() noexcept -> uint32_t {
    #if defined(_WIN32)
        return static_cast<uint32_t>(::_getpid());
    #else
        return static_cast<uint32_t>(::getpid());
    #endif
    }();
    return id;
}

/**
 * @brief The broken-down time to render when localtime() fails
 *
 * TimestampFormatter falls back to "1970-01-01 00:00:00.000000" rather than
 * dropping the timestamp, so pattern date flags use the same placeholder. One
 * line reporting two different stories - a 1970 date from %q next to an empty
 * %Y - would be worse than either on its own.
 */
inline const second_cache& epoch_fallback_cache() noexcept {
    static const second_cache fallback = []() noexcept {
        second_cache cache;
        std::memcpy(cache.date_time, "1970-01-01 00:00:00", kDateTimeLen);
        cache.seconds = 0;
        return cache;
    }();
    return fallback;
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

/**
 * @brief One-character level name, for the %L pattern flag
 */
inline constexpr const char* to_short_string(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::L_TRACE: return "T";
        case LogLevel::L_DEBUG: return "D";
        case LogLevel::L_INFO:  return "I";
        case LogLevel::L_WARN:  return "W";
        case LogLevel::L_ERROR: return "E";
        case LogLevel::L_FATAL: return "F";
        case LogLevel::L_OFF:   return "O";
        default:              return "?";
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
        : format_(Format::CUSTOM), custom_format_(custom_format),
          custom_segments_(split_custom_format(custom_format)) {}

    /**
     * @brief Append the timestamp to @p out
     *
     * The form every log line goes through: a sink renders into a buffer it
     * reuses across entries, so appending here is what keeps a steady-state line
     * free of allocations. format_timestamp() is this plus a string to put the
     * result in.
     */
    void append_timestamp(std::string& out, uint64_t timestamp_ns) const {
        using namespace detail;

        // Whole seconds select the cached "YYYY-MM-DD HH:MM:SS" prefix; only the
        // sub-second digits have to be written per call.
        const int64_t seconds = static_cast<int64_t>(timestamp_ns / 1000000000ULL);
        const uint32_t us = static_cast<uint32_t>((timestamp_ns / 1000ULL) % 1000000ULL);

        const second_cache* cache = cached_second(seconds);
        if (!cache) [[unlikely]] {
            out += "1970-01-01 00:00:00.000000"; // fallback timestamp
            return;
        }

        // Longest output is ISO8601: "YYYY-MM-DDTHH:MM:SS.ffffffZ" (27 chars).
        char buf[32];
        switch (format_) {
        case Format::DEFAULT:
            out.append(cache->date_time, kDateTimeLen);
            return;

        case Format::WITH_MICROSECONDS:
            std::memcpy(buf, cache->date_time, kDateTimeLen);
            buf[19] = '.';
            write_6_digits(buf + 20, us);
            out.append(buf, 26);
            return;

        case Format::WITH_MILLISECONDS:
            std::memcpy(buf, cache->date_time, kDateTimeLen);
            buf[19] = '.';
            write_2_digits(buf + 20, us / 10000);
            buf[22] = static_cast<char>('0' + (us / 1000) % 10);
            out.append(buf, 23);
            return;

        case Format::ISO8601:
            std::memcpy(buf, cache->date_time, kDateTimeLen);
            buf[10] = 'T';
            buf[19] = '.';
            write_6_digits(buf + 20, us);
            buf[26] = 'Z';
            out.append(buf, 27);
            return;

        case Format::TIME_ONLY:
            std::memcpy(buf, cache->date_time + kTimeOffset, kTimeLen);
            buf[8] = '.';
            write_6_digits(buf + 9, us);
            out.append(buf, 15);
            return;

        case Format::CUSTOM:
            if (!custom_segments_.empty()) {
                append_custom(out, cache->tm, us);
                return;
            }
            out.append(cache->date_time, kDateTimeLen);
            return;
        }
    }

    /// The allocating form, for a caller that wants a string of its own.
    std::string format_timestamp(uint64_t timestamp_ns) const {
        std::string out;
        append_timestamp(out, timestamp_ns);
        return out;
    }

    /**
     * @brief An upper bound on what append_timestamp() writes, for reserve()
     *
     * Exact for the built-in formats - ISO8601 is the longest, at 27 characters.
     * A CUSTOM format expands through strftime, so there this is an estimate
     * rather than a bound; reserve() takes a hint, not a promise.
     */
    size_t max_length() const noexcept {
        return format_ == Format::CUSTOM ? custom_format_.size() + 32 : 27;
    }

private:
    /**
     * @brief Render a CUSTOM format: one strftime per segment, %f between them
     *
     * Allocation-free in the steady state. The format was split at every %f when
     * it was set, so nothing here parses it, and the microseconds are written
     * unpadded - "123", not "000123" - which is what %f has always meant on this
     * path.
     */
    void append_custom(std::string& out, const std::tm& tm, uint32_t us) const {
        bool first = true;
        for (const std::string& segment : custom_segments_) {
            if (!first) {
                detail::append_uint(out, us);
            }
            first = false;
            append_strftime(out, segment, tm);
        }
    }

    /**
     * @brief Append one strftime() expansion, through a stack buffer
     *
     * strftime() reports "did not fit" and "expanded to nothing" the same way,
     * with a 0 return, which is the whole difficulty here: a zero cannot simply be
     * taken for empty, or a format too long for the buffer would render as nothing
     * at all. The fast path is one call into a stack buffer; anything that does not
     * fit there goes to grow_strftime(), which settles the ambiguity properly.
     */
    static void append_strftime(std::string& out, const std::string& format, const std::tm& tm) {
        if (format.empty()) {
            return;
        }
        // Wide enough that no realistic timestamp format reaches the growing path.
        char buf[256];
        if (const size_t written = std::strftime(buf, sizeof(buf), format.c_str(), &tm)) {
            out.append(buf, written);
            return;
        }
        grow_strftime(out, format, tm);
    }

    /**
     * @brief Expand a format too long for the stack buffer, doubling until it fits
     *
     * Reached only by a format whose expansion needs 256 bytes or more, so this is
     * where the allocations live rather than on the per-line path.
     *
     * A sentinel byte in front of the format is what makes growing terminable: the
     * expansion can no longer be empty, so a 0 return means "too small" and only
     * that, and the buffer can be doubled until strftime() succeeds. The sentinel
     * leads rather than trails so that it cannot be read as part of a conversion
     * the format ends on.
     *
     * The cap is the guard against the one case no buffer size fixes: a format
     * strftime() rejects outright returns 0 however much room it is given.
     */
    static void grow_strftime(std::string& out, const std::string& format, const std::tm& tm) {
        // 1 MiB of expanded timestamp is a mistake in the format, not a timestamp.
        constexpr size_t kMaxExpansion = size_t{1} << 20;
        std::string probe;
        probe.reserve(format.size() + 1);
        probe += '\x01';
        probe += format;

        std::vector<char> spacious;
        for (size_t capacity = 512; capacity <= kMaxExpansion; capacity *= 2) {
            spacious.assign(capacity, '\0');
            const size_t written = std::strftime(spacious.data(), capacity, probe.c_str(), &tm);
            if (written) {
                // written counts the sentinel, which is not part of the timestamp.
                out.append(spacious.data() + 1, written - 1);
                return;
            }
        }
    }

    /**
     * @brief Split a CUSTOM format at every %f, once, when the format is set
     *
     * The microseconds belong between consecutive segments, so a format with no
     * %f yields a single segment and never pays for one. "%%" is stepped over
     * whole, which is why the literal percent in "%%f" is not taken for the flag.
     * Empty for an empty format, which means "use the built-in layout".
     */
    static std::vector<std::string> split_custom_format(const std::string& format) {
        std::vector<std::string> segments;
        if (format.empty()) {
            return segments;
        }
        size_t start = 0;
        for (size_t i = 0; i + 1 < format.size(); ) {
            if (format[i] != '%') {
                ++i;
            } else if (format[i + 1] == 'f') {
                segments.push_back(format.substr(start, i - start));
                i += 2;
                start = i;
            } else {
                i += 2; // "%%", or a conversion strftime handles itself
            }
        }
        segments.push_back(format.substr(start));
        return segments;
    }

    Format format_;
    std::string custom_format_;
    // custom_format_ split at every %f, once, when it was set. Rendering walks the
    // segments and writes the microseconds between them, which is what keeps a
    // CUSTOM line allocation-free: the ostringstream this replaced allocated twice
    // per line, once for a copy of the format string and once for the result.
    std::vector<std::string> custom_segments_;
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
    // Always present, even where SLICK_LOGGER_ENABLE_THREAD_ID is off and it stays
    // 0: the layout is the shared-memory contract, so the option must not move it.
    uint32_t thread_id = 0; // Producing thread id; see detail::current_thread_id()
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

/**
 * @brief A log line layout, compiled once from an spdlog-style pattern string
 *
 * Controls the whole line, not just the timestamp: which fields appear, in what
 * order, how wide, and which part of a console line is colored.
 *
 * @code
 * sink->set_pattern("%T.%e %^%-5l%$ [%s:%#] %v");
 * // 14:02:11.481 INFO  [main.cpp:81] work item 1 of 4
 * @endcode
 *
 * The pattern is parsed exactly once, here, into a flat vector of ops. Rendering
 * walks that vector and appends into a buffer the caller owns, so a log line
 * costs no parsing and no allocation. Every date and time flag reads the
 * per-second cache behind detail::cached_second(), and the weekday/month names
 * come from static tables - there is no strftime, no put_time, no ostringstream
 * and no locale anywhere on this path.
 *
 * Supported flags:
 *
 * | Flag        | Renders                                                     |
 * |-------------|-------------------------------------------------------------|
 * | `%v`        | the formatted message                                        |
 * | `%l` / `%L` | level name / single-letter level                             |
 * | `%t`        | producing thread id (needs SLICK_LOGGER_ENABLE_THREAD_ID)     |
 * | `%P`        | producing process id                                         |
 * | `%k`        | producer tag (slick extension; empty outside shared memory)   |
 * | `%n`        | sink name                                                    |
 * | `%s` `%#` `%@` | source basename / line / `basename:line`                  |
 * | `%Y` `%y` `%m` `%d` | year (4/2 digit), month, day                         |
 * | `%H` `%I` `%M` `%S` `%p` | hour (24/12), minute, second, AM-PM              |
 * | `%e` `%f` `%F` | milli-, micro-, nanoseconds, zero padded to 3/6/9        |
 * | `%T` `%D` `%c` | `HH:MM:SS`, `MM/DD/YY`, `Www Mmm DD HH:MM:SS YYYY`       |
 * | `%a` `%A` `%b` `%B` | `Mon` / `Monday` / `Jan` / `January`                 |
 * | `%E`        | seconds since the epoch                                      |
 * | `%^` `%$`   | begin / end the colored range (console sinks only)            |
 * | `%%`        | a literal `%`                                                |
 * | `%+`        | the built-in layout, byte for byte                           |
 * | `%q`        | the sink's configured TimestampFormatter (slick extension)    |
 *
 * A flag may carry a minimum width and alignment, as `%-8l` (left aligned, padded
 * to 8) or `%8l` (right aligned). A field wider than its width is never truncated.
 *
 * An unknown or unsupported flag throws std::invalid_argument rather than
 * rendering as nothing, so a typo surfaces at configuration time.
 */
class PatternFormatter {
public:
    /// Largest minimum-width a flag may request.
    static constexpr uint32_t kMaxFieldWidth = 64;

    /// An empty formatter, meaning "use the built-in layout".
    PatternFormatter() = default;

    /// @throws std::invalid_argument if @p pattern contains an unknown flag
    explicit PatternFormatter(std::string_view pattern) { compile(pattern); }

    bool empty() const noexcept { return ops_.empty(); }
    const std::string& pattern() const noexcept { return pattern_; }

    /// True if the pattern delimits its own colored range with %^ / %$.
    bool has_color_range() const noexcept { return has_color_range_; }

    /**
     * @brief True if rendering needs the formatted message
     *
     * False lets a caller skip the whole std::format pass for a pattern that
     * renders neither the message nor a level - the level is only known to be
     * ERROR once formatting the message has been attempted, so a pattern that
     * shows a level still has to pay for it.
     */
    bool needs_message() const noexcept { return needs_message_; }

    /**
     * @brief Everything a pattern can render that does not live in the LogEntry
     */
    struct Context {
        std::string_view message;      ///< already-formatted message body
        /// The level the line reports, which is L_ERROR when the message could
        /// not be formatted regardless of what the entry was logged at. Every
        /// level-rendering flag must read this rather than LogEntry::level, so
        /// %l and %L can never disagree about the same line.
        LogLevel level = LogLevel::L_ERROR;
        std::string_view level_name;   ///< text form of `level`
        std::string_view sink_name;    ///< rendered by %n
        const TimestampFormatter* timestamp = nullptr;  ///< backs %+ and %q
        std::string_view color_start;  ///< empty unless the sink colors its output
        std::string_view color_end;
    };

    /**
     * @brief Append one rendered line to @p out
     *
     * Called on the writer thread only. When the sink colors its output but the
     * pattern has no %^, the whole line is wrapped, matching the built-in layout.
     */
    void format(std::string& out, const LogEntry& entry, const Context& ctx) const;

private:
    enum class Flag : uint8_t {
        // A run of date/time flags and their separators that turned out to be one
        // contiguous slice of the cached "YYYY-MM-DD HH:MM:SS" text, fused by
        // compile() into a single memcpy. literal_off/literal_len index that text
        // rather than literals_.
        kCachedSlice,
        kLiteral, kMessage, kLevel, kLevelShort, kThreadId, kProcessId, kTag, kSinkName,
        kSourceFile, kSourceLine, kSourceLoc,
        kYear4, kYear2, kMonth, kDay, kHour24, kHour12, kAmPm, kMinute, kSecond,
        kMillis, kMicros, kNanos, kTimeHMS, kDateMDY, kDateTimeFull, kEpochSeconds,
        kWeekdayShort, kWeekdayLong, kMonthShort, kMonthLong,
        kColorBegin, kColorEnd, kDefaultHeader, kConfiguredTimestamp
    };

    struct Op {
        Flag flag = Flag::kLiteral;
        bool left_align = false;
        uint8_t width = 0;         // 0 means "natural width"
        uint32_t literal_off = 0;  // offset into literals_, for kLiteral
        uint32_t literal_len = 0;
    };

    void compile(std::string_view pattern);
    /// Map a flag character to its op. @throws std::invalid_argument if unknown.
    static Flag flag_for(char c, std::string_view pattern);

    /**
     * @brief True if @p flag renders a calendar field, and so reads the cache
     *
     * The one list of the flags that need a broken-down time: compile() asks it
     * what to set needs_cache_ to, and format() hands append_op() a cache only
     * when it says so. %e, %f, %F and %E are the time flags that answer false -
     * they slice or divide the entry's own timestamp and never look at a tm.
     */
    static bool flag_needs_cache(Flag flag) noexcept;

    /**
     * @brief Fuse runs of date/time ops into single kCachedSlice copies
     *
     * "%Y-%m-%d %H:%M:%S" is twelve ops that between them reproduce, character
     * for character, the nineteen bytes cached_second() already holds. Detecting
     * that at compile time turns the commonest timestamp patterns into one
     * memcpy, which is what keeps a spelled-out pattern as cheap as the built-in
     * layout instead of paying a dispatch per field.
     */
    void fuse_cached_slices();
    /// The slice of "YYYY-MM-DD HH:MM:SS" a flag reproduces, if it is exactly one.
    static bool cached_slice_for(Flag flag, size_t& offset, size_t& length) noexcept;
    /// The separator at an index of "YYYY-MM-DD HH:MM:SS", or '\0' at a digit.
    static constexpr char cached_separator_at(size_t index) noexcept {
        switch (index) {
            case 4: case 7:   return '-';
            case 10:          return ' ';
            case 13: case 16: return ':';
            default:          return '\0';
        }
    }
    /// True if @p literal is exactly the cached text starting at @p offset.
    static bool literal_matches_cached(std::string_view literal, size_t offset) noexcept;
    /// Append one op's text. Width padding is applied by the caller.
    void append_op(std::string& out, const Op& op, const LogEntry& entry,
                   const Context& ctx, const detail::second_cache* cache,
                   uint32_t subsecond_ns) const;

    std::vector<Op> ops_;
    std::string literals_;   // every literal run, concatenated once at compile time
    std::string pattern_;    // kept verbatim for pattern() and diagnostics
    bool has_color_range_ = false;
    // Kept apart because they cost different things. A calendar field needs the
    // per-second localtime() behind cached_second(); %e/%f/%F need one division
    // of the entry's own timestamp. A pattern carrying only sub-second digits
    // pays for neither the lookup nor the cache line it would touch.
    bool needs_cache_ = false;      // %Y %y %m %d %H %I %p %M %S %T %D %c %a %A %b %B
    bool needs_subsecond_ = false;  // %e %f %F
    bool needs_message_ = false;  // skips the std::format pass entirely when false
    // A %^ with no %$ after it. The escape would otherwise stay active past the
    // end of the line and tint everything the terminal prints next.
    bool color_left_open_ = false;
};

namespace detail {

/**
 * @brief Grow-only store of compiled patterns, shared by everything that points into one
 *
 * A compiled pattern reaches the writer thread as a bare pointer, and there is
 * no safe point at which one could be reclaimed - the writer may have loaded it
 * an instant ago - so nothing here is ever freed. What stops that from being a
 * leak is the lookup: an identical pattern hands back the formatter already
 * held, so a process that toggles between two layouts at runtime (debug detail
 * on and off, say) keeps two formatters rather than one per call. The scan is
 * linear over a handful of entries, on a configuration call.
 *
 * The deque is what makes the pointers safe: it never moves an element it
 * already holds.
 */
class pattern_store {
public:
    /**
     * @brief The formatter for @p pattern, compiling it if this is the first time
     * @return Null for an empty pattern, which means the built-in layout
     * @throws std::invalid_argument on an unknown flag, before anything is stored,
     *         so a bad pattern leaves every caller on the layout it already had
     */
    const PatternFormatter* get(std::string_view pattern) {
        if (pattern.empty()) {
            return nullptr;
        }
        for (const PatternFormatter& owned : owned_) {
            if (owned.pattern() == pattern) {
                return &owned;
            }
        }
        owned_.emplace_back(pattern);
        return &owned_.back();
    }

    /// How many distinct patterns this store has compiled.
    size_t size() const noexcept { return owned_.size(); }

private:
    std::deque<PatternFormatter> owned_;
};

} // namespace detail

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

    /**
     * @brief Set this sink's line layout, overriding any logger-wide default
     * @param pattern An spdlog-style pattern, or empty to stop overriding
     * @throws std::invalid_argument if the pattern contains an unknown flag,
     *         in which case the sink keeps the layout it already had
     *
     * Passing an empty pattern removes this sink's override rather than pinning
     * it to the built-in layout: the sink goes back to following
     * Logger::set_pattern(), including any default set later. On a sink with no
     * logger-wide default that is the built-in layout, which is what an
     * unconfigured sink uses anyway.
     *
     * The pattern is compiled here and published to the writer thread with a
     * single release store, so logging never blocks on this and the writer never
     * takes a lock to read it. Marking the sink explicitly patterned also stops a
     * later Logger::set_pattern() from overwriting a deliberate choice.
     *
     * Safe to call while logging is in flight, but NOT concurrently with itself:
     * it is a configuration call. Each call retains its compiled pattern for the
     * life of the sink - a few hundred bytes - so a pointer the writer thread has
     * already loaded can never dangle.
     */
    void set_pattern(std::string_view pattern);

    /// The active pattern, or empty when the sink uses the built-in layout.
    std::string_view pattern() const noexcept;

    /// Change the timestamp rendered by the built-in layout, %+ and %q.
    void set_timestamp_format(TimestampFormatter::Format format);
    void set_timestamp_format(const std::string& custom_format);

protected:
    std::pair<std::string, bool> format_log_message(const LogEntry& entry);

    /// Maps a level to the escape sequence that opens its color. Null for a sink
    /// that does not color its output.
    using ColorFn = std::string_view (*)(LogLevel) noexcept;

    /**
     * @brief Render one entry into this sink's reusable buffer
     * @param color_for Resolves the opening escape, or null for no color
     * @param color_end Escape sequence closing it
     * @return A view valid until the next format_log_entry() call on this sink
     *
     * The single implementation of a log line, shared by every text sink. Writer
     * thread only. Reusing the buffer is what keeps a steady-state line free of
     * allocations, so do not hold the view across calls - copy it if you must.
     *
     * The color is resolved from the level the line actually reports, which is
     * why this takes a function rather than a ready-made escape: an entry whose
     * message will not format is reported at ERROR, and that is only known once
     * the message has been attempted, inside here.
     */
    std::string_view format_log_entry(const LogEntry& entry,
                                      ColorFn color_for = nullptr,
                                      std::string_view color_end = {});

private:
    friend class Logger;
    /**
     * @brief Record a new logger-wide default, already compiled
     * @param store The logger's pattern store, kept alive for as long as this
     *              sink might still render through @p compiled
     * @param compiled The formatter for that default, or null for the built-in layout
     *
     * Takes effect unless this sink has a pattern of its own, but is remembered
     * either way so set_pattern("") can fall back to whatever the default is at
     * that point. The formatter arrives compiled because every sink inheriting
     * the same default shares one, rather than each compiling the same text.
     *
     * noexcept, and deliberately so: Logger::set_pattern() walks every sink
     * calling this, and a throw partway through would leave the sinks split
     * between two layouts. Nothing here allocates - the formatter is already
     * compiled, and the store arrives as a shared_ptr the caller copied.
     */
    void apply_default_pattern(std::shared_ptr<detail::pattern_store> store,
                               const PatternFormatter* compiled) noexcept;
    /// Publish the layout that currently wins: this sink's own, else the default.
    void publish_active_pattern() noexcept;

protected:
    std::string name_;
    int index_ = -1; // Index assigned by Logger when added
    // Atomic because the LOG_SINK_* macros read it from producer threads while
    // set_min_level() may run concurrently on another. LogLevel is uint8_t-backed,
    // so this stays lock-free.
    std::atomic<LogLevel> min_level_{LogLevel::L_TRACE}; // Minimum level for this sink
    bool dedicated_ = false; // Whether this sink is dedicated (logs only its own entries)
    // Left deliberately default-constructed: TimestampFormatter's own default is
    // Format::WITH_MICROSECONDS, which is the documented default timestamp for
    // every sink. Do not "tidy" this into an explicit initializer.
    TimestampFormatter timestamp_formatter_;
    // Writer thread only; reused across entries so a steady-state line allocates
    // nothing at all.
    std::string format_buffer_;
    // This sink's own patterns, from set_pattern(). Grow-only, so a pointer the
    // writer thread already loaded stays valid even as set_pattern() publishes a
    // replacement.
    detail::pattern_store owned_patterns_;
    // The logger's store, holding whatever inherited_pattern_ points at. Held by
    // shared_ptr rather than trusted to outlive the sink: a caller can keep its
    // own shared_ptr to a sink past Logger::reset(), or past the destruction of a
    // Logger it installed with set_instance().
    std::shared_ptr<detail::pattern_store> inherited_store_;
    // The two layouts that compete, kept apart so the override is reversible:
    // this sink's own wins while it is set, and clearing it falls back to
    // whatever the logger-wide default is by then. Null means "not set".
    const PatternFormatter* explicit_pattern_ = nullptr;
    const PatternFormatter* inherited_pattern_ = nullptr;
    // The winner of those two, which is all the writer thread reads.
    std::atomic<const PatternFormatter*> active_pattern_{nullptr};
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

/**
 * @brief Open @p path into @p stream with an explicit buffer, creating parents first
 *
 * Factored out of FileSinkBase::open_stream() so the statistics CSV writer can
 * reuse it: the statistics thread is not a sink - sinks belong to the writer
 * thread alone - so it cannot reach that protected member, and duplicating the
 * body would leave two copies of the buffering and directory rules to keep in
 * step.
 *
 * Callers check @p stream afterwards and report failures in their own terms.
 * Directory creation is best-effort: if it fails the open fails too, which the
 * caller already handles.
 *
 * @p buffer backs the stream and must outlive it - pubsetbuf() does not take
 * ownership - and is only installed while the stream is closed, which is the
 * only state in which it takes effect.
 */
/// Size of the explicit stream buffer open_buffered_stream() installs.
inline constexpr size_t kStreamBufferSize = 64 * 1024;

inline void open_buffered_stream(std::ofstream& stream, std::vector<char>& buffer,
                                 const std::filesystem::path& path,
                                 std::ios::openmode mode, size_t buffer_size) {
    const auto parent = path.parent_path();
    if (!parent.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(parent, ec); // no-op when it already exists
    }
    if (buffer.empty()) {
        buffer.resize(buffer_size);
    }
    stream.rdbuf()->pubsetbuf(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    stream.open(path, mode);
}

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
    static std::string_view get_color_code(LogLevel level) noexcept;
    static std::string_view get_reset_code() noexcept;

    bool use_colors_;
    bool use_stderr_for_errors_;
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
    static constexpr size_t kStreamBufferSize = detail::kStreamBufferSize;

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
 * @brief One sample of the logging pipeline, published by the statistics thread
 *
 * Most fields are derived from counters that already exist - the two ring write
 * cursors, the writer thread's read cursor and slick-queue's loss counter - so
 * the producer path is untouched: logging costs exactly what it did before,
 * enabled or not.
 *
 * The string occupancy fields are the exception, and only just: with statistics
 * enabled the writer reads the two ring cursors once per drained BATCH (not per
 * entry) and compares one of them against its own read cursor. It never looks
 * inside an entry to do it. That is the whole cost enabling statistics adds to
 * the logging pipeline, and none of it lands on the producing thread.
 *
 * Read it with Logger::stats_snapshot(). A zeroed instance is what a logger with
 * statistics disabled returns.
 */
struct LogStats {
    // ---- Sample identity ----

    /// Wall-clock time of the sample, in nanoseconds since the epoch. Same clock
    /// and epoch as LogEntry::timestamp, so it lines up with the log itself.
    uint64_t timestamp_ns = 0;
    /// Measured seconds since the previous sample; 0.0 on the first one. Rates
    /// below divide by this rather than by the configured interval, so a late
    /// wake-up on a loaded machine does not distort them.
    double interval_sec = 0.0;
    /// How many oversampling ticks fed the _max and _mean figures below. Exposes
    /// the fidelity behind them: a low count means the peak may have been missed.
    uint32_t sample_count = 0;

    // ---- Log entry ring ----

    /// Cumulative entries reserved by producers.
    uint64_t entries_produced = 0;
    /// Cumulative entries claimed by the writer thread. Always 0 in
    /// QueueMode::SharedProducer, which has no writer thread of its own.
    uint64_t entries_consumed = 0;
    /// produced - consumed at report time: how far the reader is behind.
    uint64_t entry_queue_depth = 0;
    /// Peak depth across the reporting interval. THIS is the figure to alert on -
    /// entry_queue_depth is a single point sample and will miss a burst that
    /// filled the ring and drained again between two reports.
    uint64_t entry_queue_depth_max = 0;
    /// Mean depth across the reporting interval.
    double entry_queue_depth_mean = 0.0;
    uint32_t entry_queue_capacity = 0;
    /// entry_queue_depth as a percentage of capacity, clamped to 100.
    double entry_queue_pct = 0.0;
    /// entry_queue_depth_max as a percentage of capacity, clamped to 100.
    double entry_queue_pct_max = 0.0;
    /// Messages per second entering the queue - the log throughput figure.
    double produced_per_sec = 0.0;
    /// Messages per second leaving it. Steady state has the two roughly equal;
    /// consumed lagging produced is what makes the depth grow.
    double consumed_per_sec = 0.0;

    // ---- String ring: occupancy ----

    /// Bytes between the oldest string the writer thread still needs and the
    /// producers' write cursor - the true in-flight span of the string ring.
    ///
    /// The logger never calls read() on the string ring, so there is no read
    /// cursor to subtract. Computed instead as write_cursor - frontier, where the
    /// frontier is the position the writer publishes once it has provably drained
    /// past every string below it - see stats_string_frontier_ for the cursor-pair
    /// rule behind that. Entry contents are never read to derive it. Only
    /// meaningful when string_pct_valid.
    uint64_t string_inflight_bytes = 0;
    /// Peak in-flight span across the reporting interval. Like
    /// entry_queue_depth_max, this is the figure to alert on: a burst that backs
    /// the string ring up and drains again between two reports is invisible to
    /// string_inflight_bytes, which is a single point sample.
    uint64_t string_inflight_bytes_max = 0;
    uint32_t string_buffer_capacity = 0;
    /// string_inflight_bytes as a percentage of capacity, clamped to 100.
    double string_pct = 0.0;
    /// string_inflight_bytes_max as a percentage of capacity, clamped to 100.
    double string_pct_max = 0.0;
    /// False only when there is no string ring to measure at all - the logger is
    /// not running, or was built with a zero-capacity ring - in which case
    /// string_inflight_bytes and string_pct carry the previous sample's values
    /// rather than a misleading zero. Whenever a ring exists a measurement is
    /// always available: the frontier is seeded at start(), so there is always a
    /// floor to measure from, and a fully drained queue reports a valid zero.
    bool string_pct_valid = false;

    // ---- String ring: write-cursor rates ----

    /// Cumulative bytes reserved in the string ring.
    uint64_t string_bytes_written = 0;
    double string_bytes_per_sec = 0.0;
    /// Bytes written this interval as a percentage of capacity: how much of the
    /// ring was recycled. This is a RATE, not an occupancy - 100 means the ring
    /// turned over exactly once during the interval. string_pct is the occupancy.
    double string_turnover_pct = 0.0;
    /// The same figure expressed as whole ring wraps per second.
    double string_wraps_per_sec = 0.0;

    // ---- Loss ----

    /// Entries a producer overwrote before the writer thread could read them.
    /// Reads 0 unless the queues were built with loss-detecting traits, since
    /// slick::queue_traits::enable_loss_detection defaults to false.
    uint64_t entry_loss_count = 0;
    /// Structurally always 0: slick-queue counts losses inside read(), and the
    /// logger never calls read() on the string ring - strings are reached by
    /// pointer. Present so the CSV schema stays symmetric and self-describing.
    uint64_t string_loss_count = 0;
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

    /// Line layout applied to every sink that has no pattern of its own.
    /// See PatternFormatter for the flags.
    ///
    /// Empty means "do not change the current default", not "use the built-in
    /// layout", so `set_pattern(p)` followed by `init(config)` keeps `p`. On a
    /// logger that has never been given a pattern - including one just past
    /// reset(), which clears it - that amounts to the built-in layout. To drop a
    /// pattern without resetting, call `set_pattern("")`.
    std::string pattern;

    // ---- Statistics; all inert unless enable_stats is true ----

    /// Run the background statistics thread. It samples the queue cursors, never
    /// touches a sink and never logs, so it cannot perturb what it measures.
    ///
    /// The producer path is unaffected either way. The one cost of enabling this
    /// falls on the writer thread, which then reads the two ring reservation
    /// cursors once per drained batch to publish the string-ring frontier. It
    /// never looks inside an entry to do it - see LogStats.
    bool enable_stats = false;
    /// Where the statistics CSV is written. Leave empty to run the thread for
    /// stats_snapshot() alone and write no file.
    std::filesystem::path stats_file;
    /// How often a row is appended and a snapshot published.
    uint32_t stats_interval_ms = 1000;
    /// How often the queue-depth gauges are sampled between reports.
    ///
    /// Depth is instantaneous: read only once per report, a burst that fills the
    /// ring and drains again in between is never seen at all. Sampling faster and
    /// reporting the peak is what makes LogStats::entry_queue_pct_max meaningful.
    /// Clamped into [1, stats_interval_ms]; setting it equal to stats_interval_ms
    /// turns oversampling off.
    ///
    /// This is also how quickly the thread notices shutdown, so a long reporting
    /// interval never delays shutdown() by more than one tick.
    uint32_t stats_sample_interval_ms = 10;
    /// Roll the CSV to "<stem>.1.csv" once it reaches this size, so a long-running
    /// process cannot fill a disk. Zero lets it grow without bound.
    size_t stats_max_file_size = 16 * 1024 * 1024;
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
     * @brief Set the line layout for every sink that has no pattern of its own
     * @param pattern An spdlog-style pattern; empty restores the built-in layout
     * @throws std::invalid_argument if the pattern contains an unknown flag,
     *         in which case no sink is changed
     *
     * Applies to sinks already registered and to any added afterwards, so it can
     * be called before or after the add_*_sink() calls. A sink configured through
     * ISink::set_pattern() keeps its own layout and is never overwritten here.
     *
     * @code
     * Logger::instance().set_pattern("%T.%e %^%-5l%$ [%s:%#] %v");
     * @endcode
     *
     * See PatternFormatter for the full flag list.
     */
    void set_pattern(std::string_view pattern);

    /// The logger-wide default pattern, or empty when none is set.
    std::string_view pattern() const noexcept {
        return default_pattern_ ? std::string_view{default_pattern_->pattern()}
                                : std::string_view{};
    }

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
     * @brief The most recent statistics sample
     * @return The last published LogStats, or a zeroed one when statistics are off
     *
     * Safe to call from any thread and at any time, including before init() and
     * after shutdown(): it returns a copy of an immutable published sample, so no
     * reader can observe a half-written one.
     *
     * shutdown() clears the published sample, so this reads zeroed once the
     * logger is stopped rather than serving the finished run's numbers. Read them
     * before shutting down if you want them.
     *
     * Never touches the logging hot path. It is not, however, guaranteed
     * wait-free: the sample is held in a std::atomic<std::shared_ptr>, which is
     * lock-free only where the implementation says so (MSVC uses an internal lock),
     * so a caller can briefly contend with the once-per-interval publish. That is
     * the price of a snapshot that is race-free by the memory model rather than
     * merely in practice; see publish_stats().
     *
     * Enable the sampling with LogConfig::enable_stats.
     *
     * @code
     * const auto stats = Logger::instance().stats_snapshot();
     * if (stats.entry_queue_pct_max > 80.0) { ... }
     * @endcode
     */
    LogStats stats_snapshot() const noexcept;

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

    // ---- Statistics ----

    /// Sample tick, gauge accumulation and periodic reporting. The whole body of
    /// the statistics thread.
    void stats_thread_func();
    /// Accumulate one queue-depth sample into the interval's max/sum/count.
    void accumulate_stats_sample() noexcept;
    /// Build a full sample from the queue cursors and the accumulators.
    LogStats sample_stats() noexcept;
    /// Fold one drained batch into the string frontier. Writer thread only; see
    /// the cursor-pair rule documented at stats_string_frontier_.
    void advance_string_frontier() noexcept;
    /// Turn the writer-published frontier into an in-flight span. Returns false
    /// only when there is no string ring to measure, leaving @p stats untouched.
    bool sample_string_occupancy(LogStats& stats) noexcept;
    /// Publish @p stats for stats_snapshot() readers. Statistics thread only.
    void publish_stats(const LogStats& stats) noexcept;
    /// Open the CSV and write its header. Throws if the file cannot be created.
    void open_stats_csv();
    void write_stats_row(const LogStats& stats);
    void write_stats_header();
    /// Roll the CSV to "<stem>.1.csv" and reopen it empty.
    void roll_stats_csv();


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
    /// Layout handed to sinks that have none of their own, including sinks added
    /// after set_pattern(). Null means the built-in layout. The one compiled copy
    /// every inheriting sink renders through, and the only record of the default:
    /// pattern() reads its text back out of it.
    const PatternFormatter* default_pattern_ = nullptr;
    /// Owns the formatters default_pattern_ points at. Shared with every sink
    /// that inherits one, so the store outlives this logger if a sink does.
    std::shared_ptr<detail::pattern_store> pattern_store_ =
        std::make_shared<detail::pattern_store>();

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
    // ---- Statistics state; all inert unless stats_enabled_ ----

    std::thread stats_thread_;
    /// The published sample, immutable once stored. The statistics thread swaps in
    /// a fresh one per report and readers only ever see a completed object, so
    /// stats_snapshot() involves no data race - see publish_stats().
    std::atomic<std::shared_ptr<const LogStats>> stats_latest_;
    /// Position just past the last string the writer thread has finished with, so
    /// everything from here to the write cursor is still outstanding. Published by
    /// the writer thread because only it holds entries that read() has already
    /// confirmed published; the statistics thread must never read the entry ring
    /// itself, where a reserved-but-unfilled slot would give it torn data. Relaxed
    /// both ways: it carries a position and orders nothing else.
    std::atomic<uint64_t> stats_string_frontier_{0};
    // The cursor pair the frontier is promoted from, and the rule behind it.
    //
    // A producer reserves its string bytes BEFORE it reserves an entry slot, so
    // string order and slot order are two independent races and neither implies
    // the other. Taking the frontier from the string of whichever entry the
    // writer happened to drain last is therefore unsound: a producer holding
    // earlier string bytes can take a later slot, and the frontier sails past
    // bytes still in use, reporting an idle ring while the ring is not.
    //
    // So the frontier is never read out of an entry at all. The writer instead
    // snapshots the entry reservation cursor and then the string reservation
    // cursor, and promotes the string half only once its read cursor has passed
    // the entry half. At that moment every slot taken before the snapshot has
    // been drained, and every one of those strings was reserved before the
    // snapshot too, so the whole string ring below it is provably free - whatever
    // order the producers published in. Between promotions the frontier simply
    // stays put, which is the conservative direction: the span grows and the
    // gauge reads fuller, which is what a stalled writer means.
    //
    // The one window it cannot see is a producer that has reserved string bytes
    // and not yet taken a slot: it is invisible in both orderings, so a producer
    // preempted there still hides its bytes until it publishes. Closing that
    // needs the producer to announce the reservation, which is not worth what it
    // would cost the logging path.
    //
    // Writer thread only. Relaxed everywhere: these carry positions and order
    // nothing else.
    uint64_t stats_gen_entry_ = 0;
    uint64_t stats_gen_string_ = 0;
    bool stats_gen_open_ = false;
    // Configuration, written by init() before the thread starts and read only by
    // it afterwards, so it needs no synchronization.
    bool stats_enabled_ = false;
    uint32_t stats_interval_ms_ = 0;
    uint32_t stats_sample_interval_ms_ = 0;
    size_t stats_max_file_size_ = 0;
    std::filesystem::path stats_file_;
    // Everything below is touched only by the statistics thread, between the
    // point start() spawns it and the point shutdown() joins it.
    uint64_t stats_depth_max_ = 0;
    uint64_t stats_depth_sum_ = 0;
    uint32_t stats_sample_count_ = 0;
    uint64_t stats_string_inflight_max_ = 0;
    /// Last in-flight span any tick in this interval resolved, and whether one
    /// did. A row with neither reports unknown rather than a misleading zero.
    uint64_t stats_string_last_ = 0;
    bool stats_string_seen_ = false;
    std::ofstream stats_stream_;
    std::vector<char> stats_stream_buffer_;
    size_t stats_bytes_written_ = 0;
    /// Cumulative counters as of the previous report, for the rate deltas.
    LogStats stats_previous_{};
    bool stats_has_previous_ = false;
    std::chrono::steady_clock::time_point stats_last_report_{};
    /// Whether a CSV write has already failed, so the diagnostic is printed once
    /// rather than on every interval.
    bool stats_write_failed_ = false;
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
 * @brief Presentation type of a "{...}" spec, or '\0' if it has none
 *
 * The presentation type is the single character just before the closing brace,
 * and can only appear after a ':'. A trailing width or precision digit
 * ("{:5}") is not a type, and neither is the last character of a fill/align
 * pair ("{:x<5}"), so only the recognized type letters are reported. Only the
 * types that matter to a character argument are listed.
 */
inline char spec_presentation_type(std::string_view format_spec) noexcept {
    // The shortest spec carrying a type is "{:d}".
    if (format_spec.size() < 4 || format_spec.find(':') == std::string_view::npos) {
        return '\0';
    }
    const char type = format_spec[format_spec.size() - 2];
    switch (type) {
        case 'b': case 'B': case 'c': case 'd': case 'o': case 'x': case 'X':
            return type;
        default:
            return '\0';
    }
}

/**
 * @brief Encode one Unicode code point as UTF-8
 * @return Number of bytes written to @p out, or 0 if @p code_point is not a
 *         Unicode scalar value - an unpaired UTF-16 surrogate (what a lone
 *         wchar_t holding half a surrogate pair looks like on Windows) or a
 *         value beyond U+10FFFF. Those have no character to encode.
 */
inline size_t encode_utf8(uint32_t code_point, char (&out)[4]) noexcept {
    if (code_point < 0x80) {
        out[0] = static_cast<char>(code_point);
        return 1;
    }
    if (code_point < 0x800) {
        out[0] = static_cast<char>(0xC0 | (code_point >> 6));
        out[1] = static_cast<char>(0x80 | (code_point & 0x3F));
        return 2;
    }
    if (code_point < 0x10000) {
        if (code_point >= 0xD800 && code_point <= 0xDFFF) {
            return 0;
        }
        out[0] = static_cast<char>(0xE0 | (code_point >> 12));
        out[1] = static_cast<char>(0x80 | ((code_point >> 6) & 0x3F));
        out[2] = static_cast<char>(0x80 | (code_point & 0x3F));
        return 3;
    }
    if (code_point <= 0x10FFFF) {
        out[0] = static_cast<char>(0xF0 | (code_point >> 18));
        out[1] = static_cast<char>(0x80 | ((code_point >> 12) & 0x3F));
        out[2] = static_cast<char>(0x80 | ((code_point >> 6) & 0x3F));
        out[3] = static_cast<char>(0x80 | (code_point & 0x3F));
        return 4;
    }
    return 0;
}

/**
 * @brief Narrow a signed argument used as a dynamic width or precision
 * @throws std::format_error if it is negative
 */
template<typename T>
inline uint64_t checked_spec_value(T value) {
    if (value < 0) {
        throw std::format_error("dynamic width or precision must not be negative");
    }
    return static_cast<uint64_t>(value);
}

/**
 * @brief Value of a log argument named by a nested width/precision field
 *
 * Only the integer types can serve as one, which is the rule std::format
 * applies to a nested field. Anything else is a format error rather than a
 * silently dropped spec: a column that quietly loses its alignment gives the
 * reader nothing to go on.
 *
 * @note The union member is read by value. These members are packed and may be
 *       misaligned, so a reference must never be bound to one - see
 *       format_one_arg.
 * @throws std::format_error if the argument cannot serve as a width/precision
 */
inline uint64_t dynamic_spec_value(const LogArgument& arg) {
    switch (arg.type) {
        case ArgType::INT8_T:   return checked_spec_value(arg.value.i8);
        case ArgType::INT16_T:  return checked_spec_value(arg.value.i16);
        case ArgType::INT32_T:  return checked_spec_value(arg.value.i32);
        case ArgType::INT64_T:  return checked_spec_value(arg.value.i64);
        case ArgType::UINT8_T:  return arg.value.u8;
        case ArgType::UINT16_T: return arg.value.u16;
        case ArgType::UINT32_T: return arg.value.u32;
        case ArgType::UINT64_T: return arg.value.u64;
        default:
            throw std::format_error("dynamic width or precision must be an integer argument");
    }
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

            // Find this field's closing brace. The spec may itself contain
            // replacement fields, for a dynamic width or precision
            // ("{:{}.{}f}"), so the first '}' is not necessarily the one that
            // ends the field - step over a nested field rather than stopping
            // inside it. A nested field cannot nest any further, so one level
            // is all the grammar allows for.
            //
            // Only a '{' inside the format spec starts one: everything before
            // the ':' is the argument index, which is digits alone. Reading a
            // '{' there as nested would change what a malformed string like
            // "{unclosed and {}" means, and it is not a nested field anyway.
            size_t brace_end = brace_start + 1;
            bool in_spec = false;
            while (brace_end < format_str.length() && format_str[brace_end] != '}') {
                if (format_str[brace_end] == ':') {
                    in_spec = true;
                } else if (in_spec && format_str[brace_end] == '{') {
                    const size_t nested_end = format_str.find('}', brace_end + 1);
                    if (nested_end == std::string::npos) {
                        break;
                    }
                    brace_end = nested_end + 1;
                    continue;
                }
                ++brace_end;
            }
            if (brace_end >= format_str.length() || format_str[brace_end] != '}') {
                // Malformed format string
                result += format_str.substr(brace_start);
                break;
            }

            // A placeholder may carry an explicit argument index ({0}, {1}, ...)
            // in front of the optional ':' format spec, so the manual parser
            // here has to resolve the index itself: each argument is formatted
            // against its own single-argument spec.
            //
            // std::format rejects a format string that mixes explicit indices
            // with bare {} automatic placeholders. This parser tolerates the
            // mix - the two counters are independent - rather than losing the
            // whole log line to a format error.
            uint8_t resolved_index = 0;
            size_t spec_prefix_len = 0; // leading digits consumed from the placeholder
            if (brace_start + 1 < brace_end &&
                format_str[brace_start + 1] >= '0' && format_str[brace_start + 1] <= '9') {
                uint32_t id = 0;
                while (brace_start + 1 + spec_prefix_len < brace_end &&
                       format_str[brace_start + 1 + spec_prefix_len] >= '0' &&
                       format_str[brace_start + 1 + spec_prefix_len] <= '9') {
                    if (id < entry.arg_count) {
                        // Once id reaches arg_count it is already out of range,
                        // and further digits can only push it further out, so
                        // stop accumulating there. Multiplying through a long
                        // digit run would overflow uint32_t and could wrap a
                        // huge index back onto a valid argument - "{4294967296}"
                        // would otherwise select argument 0.
                        id = id * 10 + static_cast<uint32_t>(format_str[brace_start + 1 + spec_prefix_len] - '0');
                    }
                    ++spec_prefix_len;
                }
                if (id >= entry.arg_count) {
                    // Not enough arguments
                    result += "<MISSING_ARG>";
                    pos = brace_end + 1;
                    continue; // manual index: the automatic counter stays untouched
                }
                resolved_index = static_cast<uint8_t>(id);
            } else {
                if (arg_index >= entry.arg_count) {
                    // Not enough arguments
                    result += "<MISSING_ARG>";
                    pos = brace_end + 1;
                    continue;
                }
                // Claimed here rather than at the end of the iteration: a nested
                // width or precision field takes the *next* automatic argument,
                // so this field's own has to be spoken for before the spec below
                // is walked.
                resolved_index = arg_index++;
            }

            // Rebuild the spec as a single-argument one. The explicit index is
            // dropped ({1:>8} -> {:>8}, {1} -> {}), and a nested width or
            // precision field is replaced by the decimal value of the argument
            // it names ({0:{1}} over (x, 8) -> {:8}). Substituting the value
            // rather than forwarding the nested field is what keeps every
            // argument formatting against a one-argument spec, which is what
            // lets each ArgType hand std::format its own union member.
            std::string format_spec;
            format_spec.reserve(brace_end - brace_start + 1);
            format_spec += '{';
            bool nested_arg_missing = false;
            bool spec_started = false; // same rule the scan above applied
            for (size_t i = brace_start + 1 + spec_prefix_len; i < brace_end; ) {
                const char ch = format_str[i];
                if (ch != '{' || !spec_started) {
                    spec_started = spec_started || ch == ':';
                    format_spec += ch;
                    ++i;
                    continue;
                }

                ++i; // step past the nested field's '{'
                uint32_t nested_id = 0;
                bool nested_explicit = false;
                while (i < brace_end && format_str[i] >= '0' && format_str[i] <= '9') {
                    if (nested_id < entry.arg_count) {
                        nested_id = nested_id * 10 + static_cast<uint32_t>(format_str[i] - '0');
                    }
                    nested_explicit = true;
                    ++i;
                }
                if (i < brace_end && format_str[i] == '}') {
                    ++i; // step past the nested field's '}'
                }

                if (!nested_explicit) {
                    nested_id = arg_index;
                    if (arg_index < entry.arg_count) {
                        ++arg_index; // as above, never past the end
                    }
                }
                if (nested_id >= entry.arg_count) {
                    nested_arg_missing = true;
                    break;
                }
                // A zero width is dropped rather than written out. A literal 0
                // in the width position is the zero-padding flag and not a
                // width at all - "{:0}" pads a number, and is rejected outright
                // for a text argument - whereas a width of zero just means no
                // minimum width, which is exactly what leaving it out says.
                // Zero is a meaningful precision ("{:.0f}"), so only the width
                // case is dropped; the '.' that introduces a precision is
                // already in the spec by the time we get here.
                const uint64_t nested_value = dynamic_spec_value(entry.args[nested_id]);
                if (nested_value != 0 || format_spec.back() == '.') {
                    format_spec += std::to_string(nested_value);
                }
            }

            if (nested_arg_missing) {
                // The width or precision names an argument that was never
                // passed, so there is no spec to format against. Reported as a
                // missing argument like any other, rather than as a format
                // error, which would cost the whole line.
                result += "<MISSING_ARG>";
                pos = brace_end + 1;
                continue;
            }
            format_spec += '}';

            // Format the argument using std::format with the specific format spec
            const auto& arg = entry.args[resolved_index];
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
                case ArgType::WCHAR: {
                    // std::format has no wchar_t formatter in a narrow (char)
                    // context on any supported standard library, so a wchar_t
                    // cannot go through format_one_arg directly. Reproduce what
                    // std::format does for char instead: render it as text by
                    // default and as a number under a b/B/d/o/x/X presentation
                    // type. Dispatching on the spec rather than on the value
                    // keeps "{}" and "{:5}" producing the same kind of output -
                    // and the same default alignment - for every code point,
                    // instead of silently switching to a number above U+007F.
                    //
                    // wchar_t is signed on Linux and macOS, so go through the
                    // unsigned type first: a negative value is a code unit, not
                    // a number to sign-extend into a huge unsigned one.
                    const auto code_point = static_cast<uint32_t>(
                        static_cast<std::make_unsigned_t<wchar_t>>(arg.value.wc));

                    std::string_view spec{format_spec};
                    std::string char_spec;
                    bool as_number = false;
                    switch (spec_presentation_type(format_spec)) {
                        case 'b': case 'B': case 'd': case 'o': case 'x': case 'X':
                            as_number = true;
                            break;
                        case 'c':
                            // "render as a character" - already the default
                            // here, and std::format's string formatter would
                            // reject the 'c', so drop it.
                            char_spec.assign(format_spec, 0, format_spec.size() - 2);
                            char_spec += '}';
                            spec = char_spec;
                            break;
                        default:
                            break;
                    }

                    char utf8[4];
                    const size_t utf8_len = as_number ? 0 : encode_utf8(code_point, utf8);
                    if (utf8_len > 0) {
                        formatted_arg = format_one_arg(spec, std::string_view{utf8, utf8_len});
                    } else {
                        // Either the spec asked for a number, or the value is
                        // not a Unicode scalar value and has no character to
                        // print - fall back to the numeric code point.
                        formatted_arg = format_one_arg(spec, code_point);
                    }
                    break;
                }
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

/**
 * @brief Append the built-in log line: "<time> [LEVEL][ [pid[:tag]]][ [file:line]] <message>"
 *
 * The one definition of the default layout. Both the no-pattern path and the %+
 * flag call it, so the two cannot drift apart. The timestamp is whatever the
 * sink is configured for, microseconds unless the caller changed it.
 */
inline void append_default_layout(std::string& out, const LogEntry& entry,
                                  const TimestampFormatter& timestamp_formatter,
                                  std::string_view level_name, std::string_view message) {
    // Reserved before the timestamp is rendered rather than after, so the line
    // takes at most one growth and the timestamp needs no string of its own.
    out.reserve(out.size() + timestamp_formatter.max_length() + level_name.size()
                + process_id_size(entry) + source_location_size(entry) + message.size() + 4);
    timestamp_formatter.append_timestamp(out, entry.timestamp);
    out += " [";
    out += level_name;
    out += ']';
    append_process_id(out, entry);
    append_source_location(out, entry);
    out += ' ';
    out += message;
}

inline PatternFormatter::Flag PatternFormatter::flag_for(char c, std::string_view pattern) {
    switch (c) {
        case 'v': return Flag::kMessage;
        case 'l': return Flag::kLevel;
        case 'L': return Flag::kLevelShort;
        case 'P': return Flag::kProcessId;
        case 'k': return Flag::kTag;
        case 'n': return Flag::kSinkName;
        case 's': return Flag::kSourceFile;
        case '#': return Flag::kSourceLine;
        case '@': return Flag::kSourceLoc;
        case 'Y': return Flag::kYear4;
        case 'y': return Flag::kYear2;
        case 'm': return Flag::kMonth;
        case 'd': return Flag::kDay;
        case 'H': return Flag::kHour24;
        case 'I': return Flag::kHour12;
        case 'p': return Flag::kAmPm;
        case 'M': return Flag::kMinute;
        case 'S': return Flag::kSecond;
        case 'e': return Flag::kMillis;
        case 'f': return Flag::kMicros;
        case 'F': return Flag::kNanos;
        case 'T': return Flag::kTimeHMS;
        case 'D': return Flag::kDateMDY;
        case 'c': return Flag::kDateTimeFull;
        case 'E': return Flag::kEpochSeconds;
        case 'a': return Flag::kWeekdayShort;
        case 'A': return Flag::kWeekdayLong;
        case 'b': return Flag::kMonthShort;
        case 'B': return Flag::kMonthLong;
        case '^': return Flag::kColorBegin;
        case '$': return Flag::kColorEnd;
        case '+': return Flag::kDefaultHeader;
        case 'q': return Flag::kConfiguredTimestamp;
        case 't':
        #if SLICK_LOGGER_ENABLE_THREAD_ID
            return Flag::kThreadId;
        #else
            throw std::invalid_argument(
                "slick-logger: pattern flag '%t' needs SLICK_LOGGER_ENABLE_THREAD_ID, which "
                "this build switched off, so every entry would render a thread id of 0. "
                "Pattern: " + std::string(pattern));
        #endif

        // spdlog flags this logger has no data for. Named apart from the unknown
        // case so the message can say why rather than just "unrecognized".
        case '!': case 'g': case 'o': case 'i': case 'u': case 'O': case 'z':
            throw std::invalid_argument(
                std::string("slick-logger: pattern flag '%") + c + "' is not supported. See "
                "PatternFormatter for the flags that are. Pattern: " + std::string(pattern));

        default:
            throw std::invalid_argument(
                std::string("slick-logger: unknown pattern flag '%") + c + "' in pattern: "
                + std::string(pattern));
    }
}

inline void PatternFormatter::compile(std::string_view pattern) {
    ops_.clear();
    literals_.clear();
    pattern_.assign(pattern);
    has_color_range_ = false;
    needs_cache_ = false;
    needs_subsecond_ = false;

    // Literal runs are accumulated and flushed as one op each, so "a%lb%vc" costs
    // three literal ops rather than one per character.
    std::string literal;
    auto flush_literal = [&]() {
        if (literal.empty()) {
            return;
        }
        Op op;
        op.flag = Flag::kLiteral;
        op.literal_off = static_cast<uint32_t>(literals_.size());
        op.literal_len = static_cast<uint32_t>(literal.size());
        literals_ += literal;
        literal.clear();
        ops_.push_back(op);
    };

    for (size_t i = 0; i < pattern.size(); ++i) {
        if (pattern[i] != '%') {
            literal += pattern[i];
            continue;
        }

        // "%[-][width]<flag>"
        size_t j = i + 1;
        bool left_align = false;
        uint32_t width = 0;
        if (j < pattern.size() && pattern[j] == '-') {
            left_align = true;
            ++j;
        }
        while (j < pattern.size() && pattern[j] >= '0' && pattern[j] <= '9') {
            width = std::min(width * 10 + static_cast<uint32_t>(pattern[j] - '0'), kMaxFieldWidth);
            ++j;
        }
        if (j >= pattern.size()) {
            throw std::invalid_argument(
                "slick-logger: pattern ends with a dangling '%': " + std::string(pattern));
        }
        if (pattern[j] == '%') {
            // "%-5%" would silently drop the -5. A width on an escaped percent is
            // meaningless, so it is far more likely a mistyped flag than intent.
            if (left_align || width != 0) {
                throw std::invalid_argument(
                    "slick-logger: '%%' takes no width or alignment; did you mean a flag "
                    "letter? Pattern: " + std::string(pattern));
            }
            literal += '%';
            i = j;
            continue;
        }

        // Resolved before the literal is flushed so an invalid flag throws with
        // the formatter still untouched.
        const Flag flag = flag_for(pattern[j], pattern);
        flush_literal();

        Op op;
        op.flag = flag;
        op.left_align = left_align;
        op.width = static_cast<uint8_t>(width);
        ops_.push_back(op);

        if (flag == Flag::kColorBegin) {
            has_color_range_ = true;
        }
        if (flag_needs_cache(flag)) {
            needs_cache_ = true;
        } else if (flag == Flag::kMillis || flag == Flag::kMicros || flag == Flag::kNanos) {
            needs_subsecond_ = true;
        }
        // %+ renders the message itself, and the level flags need to know whether
        // formatting the message failed, so all four force the format pass.
        if (flag == Flag::kMessage || flag == Flag::kDefaultHeader ||
            flag == Flag::kLevel || flag == Flag::kLevelShort) {
            needs_message_ = true;
        }
        i = j;
    }
    flush_literal();

    // Whether the last %^ was ever closed. Fixed by the op list, so it is settled
    // here rather than tracked per line.
    color_left_open_ = false;
    for (const Op& op : ops_) {
        if (op.flag == Flag::kColorBegin) {
            color_left_open_ = true;
        } else if (op.flag == Flag::kColorEnd) {
            color_left_open_ = false;
        }
    }

    fuse_cached_slices();
}

inline bool PatternFormatter::flag_needs_cache(Flag flag) noexcept {
    switch (flag) {
        // A fused run is only ever built out of the calendar flags below, so it
        // belongs here with them.
        case Flag::kCachedSlice:
        case Flag::kYear4: case Flag::kYear2: case Flag::kMonth: case Flag::kDay:
        case Flag::kHour24: case Flag::kHour12: case Flag::kAmPm:
        case Flag::kMinute: case Flag::kSecond:
        case Flag::kTimeHMS: case Flag::kDateMDY: case Flag::kDateTimeFull:
        case Flag::kWeekdayShort: case Flag::kWeekdayLong:
        case Flag::kMonthShort: case Flag::kMonthLong:
            return true;
        // %+ and %q render a timestamp of their own through TimestampFormatter,
        // which keeps its own cache lookup, so they do not need one here either.
        default:
            return false;
    }
}

inline bool PatternFormatter::cached_slice_for(Flag flag, size_t& offset, size_t& length) noexcept {
    switch (flag) {
        case Flag::kYear4:   offset = 0;  length = 4; return true;
        case Flag::kYear2:   offset = 2;  length = 2; return true;
        case Flag::kMonth:   offset = 5;  length = 2; return true;
        case Flag::kDay:     offset = 8;  length = 2; return true;
        case Flag::kHour24:  offset = 11; length = 2; return true;
        case Flag::kMinute:  offset = 14; length = 2; return true;
        case Flag::kSecond:  offset = 17; length = 2; return true;
        case Flag::kTimeHMS: offset = detail::kTimeOffset; length = detail::kTimeLen; return true;
        default: return false;
    }
}

inline bool PatternFormatter::literal_matches_cached(std::string_view literal,
                                                     size_t offset) noexcept {
    if (offset + literal.size() > detail::kDateTimeLen) {
        return false;
    }
    for (size_t i = 0; i < literal.size(); ++i) {
        // Only separators can match: a digit position varies per timestamp, so a
        // literal digit there would be a coincidence this must not act on.
        if (literal[i] != cached_separator_at(offset + i)) {
            return false;
        }
    }
    return true;
}

inline void PatternFormatter::fuse_cached_slices() {
    std::vector<Op> fused;
    fused.reserve(ops_.size());

    for (const Op& op : ops_) {
        size_t offset = 0;
        size_t length = 0;

        // A padded field has to stay its own op: the padding is measured against
        // that field alone, not against whatever it would be fused with.
        if (op.width == 0 && !fused.empty() && fused.back().flag == Flag::kCachedSlice) {
            Op& run = fused.back();
            const size_t run_end = run.literal_off + run.literal_len;

            if (cached_slice_for(op.flag, offset, length) && offset == run_end) {
                run.literal_len += static_cast<uint32_t>(length);
                continue;
            }
            if (op.flag == Flag::kLiteral &&
                literal_matches_cached(std::string_view{literals_}.substr(op.literal_off, op.literal_len),
                                       run_end)) {
                run.literal_len += op.literal_len;
                continue;
            }
        }

        if (op.width == 0 && cached_slice_for(op.flag, offset, length)) {
            Op slice;
            slice.flag = Flag::kCachedSlice;
            slice.literal_off = static_cast<uint32_t>(offset);
            slice.literal_len = static_cast<uint32_t>(length);
            fused.push_back(slice);
            continue;
        }

        fused.push_back(op);
    }

    ops_ = std::move(fused);
}

inline void PatternFormatter::append_op(std::string& out, const Op& op, const LogEntry& entry,
                                        const Context& ctx, const detail::second_cache* cache,
                                        uint32_t subsecond_ns) const {
    using namespace detail;
    char buf[32];

    // Every date and time flag slices the "YYYY-MM-DD HH:MM:SS" text that
    // cached_second() renders once per whole second, so none of them convert a
    // time_t or touch the locale. The caller supplies a cache for exactly the
    // flags that read one, the epoch placeholder standing in when localtime
    // fails, so a flag missing from flag_needs_cache() is a bug in that list
    // rather than a null this should quietly render around.
    assert(!flag_needs_cache(op.flag) || cache != nullptr);

    const auto append_cached = [&](size_t offset, size_t length) {
        out.append(cache->date_time + offset, length);
    };

    switch (op.flag) {
        case Flag::kCachedSlice:
            out.append(cache->date_time + op.literal_off, op.literal_len);
            break;
        case Flag::kLiteral:
            out.append(literals_, op.literal_off, op.literal_len);
            break;
        case Flag::kMessage:
            out += ctx.message;
            break;
        case Flag::kLevel:
            out += ctx.level_name;
            break;
        case Flag::kLevelShort:
            out += to_short_string(ctx.level);
            break;
        case Flag::kThreadId:
            append_uint(out, entry.thread_id);
            break;
        case Flag::kProcessId:
            // A Local-mode entry carries no pid, but it was produced right here,
            // so print this process rather than a bare zero.
            append_uint(out, entry.pid ? entry.pid : current_process_id());
            break;
        case Flag::kTag:
            out.append(entry.tag, strnlen(entry.tag, SLICK_LOGGER_TAG_SIZE));
            break;
        case Flag::kSinkName:
            out += ctx.sink_name;
            break;
        case Flag::kSourceFile:
            if (has_source_location(entry)) {
                out += view_string_ref(entry.file);
            }
            break;
        case Flag::kSourceLine:
            if (has_source_location(entry)) {
                append_uint(out, entry.line);
            }
            break;
        case Flag::kSourceLoc:
            if (has_source_location(entry)) {
                out += view_string_ref(entry.file);
                out += ':';
                append_uint(out, entry.line);
            }
            break;
        case Flag::kYear4:   append_cached(0, 4); break;
        case Flag::kYear2:   append_cached(2, 2); break;
        case Flag::kMonth:   append_cached(5, 2); break;
        case Flag::kDay:     append_cached(8, 2); break;
        case Flag::kHour24:  append_cached(11, 2); break;
        case Flag::kMinute:  append_cached(14, 2); break;
        case Flag::kSecond:  append_cached(17, 2); break;
        case Flag::kTimeHMS: append_cached(kTimeOffset, kTimeLen); break;
        case Flag::kHour12: {
            int hour = cache->tm.tm_hour % 12;
            if (hour == 0) {
                hour = 12;
            }
            write_2_digits(buf, static_cast<uint32_t>(hour));
            out.append(buf, 2);
            break;
        }
        case Flag::kAmPm:
            out += (cache->tm.tm_hour < 12) ? "AM" : "PM";
            break;
        case Flag::kMillis:
            write_3_digits(buf, subsecond_ns / 1000000u);
            out.append(buf, 3);
            break;
        case Flag::kMicros:
            write_6_digits(buf, subsecond_ns / 1000u);
            out.append(buf, 6);
            break;
        case Flag::kNanos:
            write_9_digits(buf, subsecond_ns);
            out.append(buf, 9);
            break;
        case Flag::kDateMDY:
            std::memcpy(buf, cache->date_time + 5, 2);      // MM
            buf[2] = '/';
            std::memcpy(buf + 3, cache->date_time + 8, 2);  // DD
            buf[5] = '/';
            std::memcpy(buf + 6, cache->date_time + 2, 2);  // YY
            out.append(buf, 8);
            break;
        case Flag::kDateTimeFull:
            // "Www Mmm DD HH:MM:SS YYYY", the shape std::asctime produces.
            out += weekday_short(cache->tm.tm_wday);
            out += ' ';
            out += month_short(cache->tm.tm_mon);
            out += ' ';
            out.append(cache->date_time + 8, 2);
            out += ' ';
            out.append(cache->date_time + kTimeOffset, kTimeLen);
            out += ' ';
            out.append(cache->date_time, 4);
            break;
        case Flag::kEpochSeconds:
            append_uint(out, entry.timestamp / 1000000000ULL);
            break;
        case Flag::kWeekdayShort: out += weekday_short(cache->tm.tm_wday); break;
        case Flag::kWeekdayLong:  out += weekday_long(cache->tm.tm_wday); break;
        case Flag::kMonthShort:   out += month_short(cache->tm.tm_mon); break;
        case Flag::kMonthLong:    out += month_long(cache->tm.tm_mon); break;
        case Flag::kColorBegin:   out += ctx.color_start; break;
        case Flag::kColorEnd:     out += ctx.color_end; break;
        case Flag::kDefaultHeader:
            if (ctx.timestamp) {
                append_default_layout(out, entry, *ctx.timestamp, ctx.level_name, ctx.message);
            }
            break;
        case Flag::kConfiguredTimestamp:
            if (ctx.timestamp) {
                ctx.timestamp->append_timestamp(out, entry.timestamp);
            }
            break;
    }
}

inline void PatternFormatter::format(std::string& out, const LogEntry& entry,
                                     const Context& ctx) const {
    // Null exactly when the pattern renders no calendar field, which is the only
    // case in which nothing reads it: every op that does is a flag_needs_cache()
    // flag, and those are what set needs_cache_. When one is present the pointer
    // is never null - a localtime failure renders the same 1970 placeholder the
    // built-in layout uses, which keeps every date op free of a null check.
    const detail::second_cache* cache = nullptr;
    if (needs_cache_) {
        cache = detail::cached_second(static_cast<int64_t>(entry.timestamp / 1000000000ULL));
        if (!cache) [[unlikely]] {
            cache = &detail::epoch_fallback_cache();
        }
    }
    uint32_t subsecond_ns = 0;
    if (needs_subsecond_) {
        subsecond_ns = static_cast<uint32_t>(entry.timestamp % 1000000000ULL);
    }

    // A pattern that does not mark its own range colors the whole line, which is
    // what the built-in layout does.
    const bool wrap_color = !ctx.color_start.empty() && !has_color_range_;
    if (wrap_color) {
        out += ctx.color_start;
    }

    for (const Op& op : ops_) {
        const size_t start = out.size();
        append_op(out, op, entry, ctx, cache, subsecond_ns);
        if (op.width == 0) {
            continue;
        }
        const size_t written = out.size() - start;
        if (written >= op.width) {
            continue;  // never truncate a field to fit its width
        }
        const size_t pad = op.width - written;
        if (op.left_align) {
            out.append(pad, ' ');
        } else {
            // The field is already at the tail, so this shifts only its own bytes
            // - at most kMaxFieldWidth of them - not the line before it. Sliding
            // them by hand instead, with memmove or a byte loop, measured no
            // faster than letting the string do it.
            out.insert(start, pad, ' ');
        }
    }

    if (wrap_color) {
        out += ctx.color_end;
    } else if (color_left_open_) {
        // "%^" with no "%$" means "color from here to the end of the line", not
        // "leave the terminal tinted for everything printed afterwards".
        out += ctx.color_end;
    }
}

inline void ISink::publish_active_pattern() noexcept {
    active_pattern_.store(explicit_pattern_ ? explicit_pattern_ : inherited_pattern_,
                          std::memory_order_release);
}

inline void ISink::set_pattern(std::string_view pattern) {
    // Compiled before anything is assigned, so an invalid pattern throws with the
    // sink still on the layout it already had.
    explicit_pattern_ = owned_patterns_.get(pattern);
    publish_active_pattern();
}

inline void ISink::apply_default_pattern(std::shared_ptr<detail::pattern_store> store,
                                        const PatternFormatter* compiled) noexcept {
    // Recorded even while an explicit pattern wins, so that clearing the
    // explicit one later falls back to the default in force at that moment.
    inherited_store_ = std::move(store);
    inherited_pattern_ = compiled;
    publish_active_pattern();
}

inline std::string_view ISink::pattern() const noexcept {
    const PatternFormatter* active = active_pattern_.load(std::memory_order_acquire);
    return active ? std::string_view{active->pattern()} : std::string_view{};
}

inline void ISink::set_timestamp_format(TimestampFormatter::Format format) {
    timestamp_formatter_ = TimestampFormatter(format);
}

inline void ISink::set_timestamp_format(const std::string& custom_format) {
    timestamp_formatter_ = TimestampFormatter(custom_format);
}

inline std::string_view ISink::format_log_entry(const LogEntry& entry,
                                                ColorFn color_for,
                                                std::string_view color_end) {
    const PatternFormatter* active = active_pattern_.load(std::memory_order_acquire);

    // The message comes first because a format error overrides the level that
    // gets printed, so neither the level nor its color can be resolved until the
    // message has been built. A pattern that renders neither skips the whole
    // std::format pass; needs_message() is what says so.
    std::string message;
    bool good = true;
    if (!active || active->needs_message()) {
        auto formatted = format_log_message(entry);
        message = std::move(formatted.first);
        good = formatted.second;
    }
    // A message that could not be formatted is reported at ERROR whatever it was
    // logged at. Derived once here so every level-rendering flag, and the color,
    // agree on what the line says.
    const LogLevel level = good ? entry.level : LogLevel::L_ERROR;
    const std::string_view level_name = to_string(level);
    const std::string_view color_start = color_for ? color_for(level) : std::string_view{};

    format_buffer_.clear();
    if (active) {
        PatternFormatter::Context ctx;
        ctx.message = message;
        ctx.level = level;
        ctx.level_name = level_name;
        ctx.sink_name = name_;
        ctx.timestamp = &timestamp_formatter_;
        ctx.color_start = color_start;
        ctx.color_end = color_end;
        active->format(format_buffer_, entry, ctx);
    } else {
        format_buffer_ += color_start;
        append_default_layout(format_buffer_, entry, timestamp_formatter_, level_name, message);
        format_buffer_ += color_end;
    }
    return format_buffer_;
}

inline ConsoleSink::ConsoleSink(bool use_colors, bool use_stderr_for_errors,
                                TimestampFormatter::Format timestamp_format, std::string&& name)
    : ISink(std::move(name)), use_colors_(use_colors), use_stderr_for_errors_(use_stderr_for_errors) {
    set_timestamp_format(timestamp_format);
}

inline ConsoleSink::ConsoleSink(const std::string& custom_timestamp_format, bool use_colors,
                                bool use_stderr_for_errors, std::string&& name)
    : ISink(std::move(name)), use_colors_(use_colors), use_stderr_for_errors_(use_stderr_for_errors) {
    set_timestamp_format(custom_timestamp_format);
}

inline void ConsoleSink::write(const LogEntry& entry) {
    // The color follows the level the line reports, not entry.level: an entry
    // whose message will not format prints as ERROR and must look like it.
    const std::string_view formatted = use_colors_
        ? format_log_entry(entry, &get_color_code, get_reset_code())
        : format_log_entry(entry);

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

inline std::string_view ConsoleSink::get_color_code(LogLevel level) noexcept {
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

inline std::string_view ConsoleSink::get_reset_code() noexcept {
    return "\033[0m";
}

inline void FileSinkBase::open_stream(const std::filesystem::path& path, std::ios::openmode mode) {
    // The buffer must be installed while the stream is closed to take effect. Every
    // caller either opens for the first time or has just closed the previous file.
    detail::open_buffered_stream(file_stream_, stream_buffer_, path, mode, kStreamBufferSize);
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
    : FileSinkBase(file_path, std::ios::app, std::move(name)) {
    set_timestamp_format(timestamp_format);
}

inline FileSink::FileSink(const std::filesystem::path& file_path,
                          const std::string& custom_timestamp_format, std::string&& name)
    : FileSinkBase(file_path, std::ios::app, std::move(name)) {
    set_timestamp_format(custom_timestamp_format);
}

inline void FileSink::write(const LogEntry& entry) {
    if (file_stream_) {
        const std::string_view formatted = format_log_entry(entry);
        file_stream_ << formatted << "\n";
    }
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
        const std::string_view formatted = format_log_entry(entry);
        file_stream_ << formatted << "\n";
        current_file_size_ += formatted.size() + 1; // +1 for newline
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
        const std::string_view formatted = format_log_entry(entry);
        file_stream_ << formatted << "\n";
        current_file_size_ += formatted.size() + 1; // +1 for newline
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

inline LogStats Logger::stats_snapshot() const noexcept {
    const std::shared_ptr<const LogStats> latest =
        stats_latest_.load(std::memory_order_acquire);
    return latest ? *latest : LogStats{};
}

inline void Logger::publish_stats(const LogStats& stats) noexcept {
    // Each report publishes a fresh, immutable LogStats and swaps the pointer, so
    // a reader is never looking at storage the statistics thread is writing.
    //
    // A seqlock over a shared LogStats would be cheaper, but it is a data race by
    // the letter of the memory model - the reader copies the payload while the
    // writer assigns it and only afterwards learns the generation moved - which is
    // undefined behavior no fence can repair. One allocation per reporting
    // interval (once a second by default), entirely off the logging path, is a
    // trivial price for a snapshot that is actually race-free.
    try {
        stats_latest_.store(std::make_shared<const LogStats>(stats),
                            std::memory_order_release);
    } catch (const std::bad_alloc&) {
        // Keep the previous snapshot rather than terminating a background thread.
    }
}

inline void Logger::accumulate_stats_sample() noexcept {
    if (!log_queue_) {
        return;
    }
    // Consumed first: the two loads are not atomic together, and loading the
    // producer cursor second makes it the fresher of the pair, so the common case
    // needs no clamp. The clamp below still covers the reverse ordering.
    const uint64_t consumed = read_index_.load(std::memory_order_relaxed);
    const uint64_t produced = log_queue_->initial_reading_index();
    const uint64_t depth = produced > consumed ? produced - consumed : 0;

    if (depth > stats_depth_max_) {
        stats_depth_max_ = depth;
    }
    stats_depth_sum_ += depth;
    ++stats_sample_count_;

    // The string ring is a gauge too, and is invisible to a once-per-report read
    // for exactly the same reason the depth is: by the time a report lands, the
    // burst that backed the ring up has usually drained. Sampling it costs three
    // relaxed loads - the writer's read cursor and the two ring cursors - so it is
    // cheap enough to run on the tick.
    if (mode_ == QueueMode::SharedProducer) {
        return; // no local reader, so there is no frontier to measure against
    }
    LogStats probe;
    if (sample_string_occupancy(probe)) {
        stats_string_seen_ = true;
        stats_string_last_ = probe.string_inflight_bytes;
        if (probe.string_inflight_bytes > stats_string_inflight_max_) {
            stats_string_inflight_max_ = probe.string_inflight_bytes;
        }
    }
}

inline void Logger::advance_string_frontier() noexcept {
    // Called by the writer once per drained batch, with the batch already
    // dispatched. Implements the cursor-pair rule described at
    // stats_string_frontier_: hold one (entry cursor, string cursor) snapshot and
    // promote the string half only once the read cursor has passed the entry half.
    if (!stats_gen_open_) {
        // Order matters and is the whole proof. R is read FIRST: every entry with
        // a slot below R had already taken that slot, and a producer reserves its
        // string before it takes a slot, so every one of those strings was
        // reserved before R was read - and therefore before S, read after it. So
        // "all slots below R drained" really does mean "everything below S
        // consumed". Reading S first would let a slot below R reserve a string
        // above S in between, and the promotion would free bytes still in use.
        stats_gen_entry_ = log_queue_->initial_reading_index();
        stats_gen_string_ = string_queue_->initial_reading_index();
        stats_gen_open_ = true;
    }
    // read_index_ is this thread's own cursor, already advanced past the batch
    // just written, so a relaxed load sees the writer's own last store.
    if (read_index_.load(std::memory_order_relaxed) >= stats_gen_entry_) {
        stats_string_frontier_.store(stats_gen_string_, std::memory_order_relaxed);
        stats_gen_open_ = false;
    }
}

inline bool Logger::sample_string_occupancy(LogStats& stats) noexcept {
    if (!log_queue_ || !string_queue_) {
        return false;
    }
    const uint64_t capacity = string_queue_->size();
    if (capacity == 0) {
        return false;
    }

    // The entry ring's reservation cursor is NOT a publication boundary: a producer
    // reserves a slot, then fills it, then publishes. Reading a slot off that
    // cursor would race the fill and yield torn data, so the statistics thread
    // never touches the entry ring directly. The writer thread instead publishes
    // the frontier below, taken from an entry read() has already handed it.
    if (read_index_.load(std::memory_order_relaxed) >=
        log_queue_->initial_reading_index()) {
        // Nothing outstanding, so nothing in the string ring is still needed. A
        // real measurement of zero, not an absent one: reporting it as invalid
        // would hide the healthiest state the pipeline has.
        stats.string_inflight_bytes = 0;
        stats.string_pct = 0.0;
        stats.string_pct_valid = true;
        return true;
    }

    // Everything from the frontier to the write cursor is still outstanding. The
    // frontier starts life at the ring cursor start() saw, so there is always a
    // floor to measure from: before the writer has confirmed a single drain,
    // nothing has been consumed, and the whole span since start really is in
    // flight. That is a measurement, not an absence - reporting it as unknown
    // would blank the row exactly while a startup burst was filling the ring.
    const uint64_t watermark = stats_string_frontier_.load(std::memory_order_relaxed);
    const uint64_t write_cursor = string_queue_->initial_reading_index();
    // An absolute reserve index in both modes, so the span is a plain
    // subtraction. Keeping it absolute is what lets an exactly full ring read as
    // capacity instead of aliasing to zero, and makes a genuine overrun visible
    // as a distance past capacity rather than silently folding back into range.
    uint64_t behind = write_cursor > watermark ? write_cursor - watermark : 0;
    if (behind > capacity) {
        behind = capacity;
    }

    stats.string_inflight_bytes = behind;
    stats.string_pct = std::min(
        100.0, static_cast<double>(behind) * 100.0 / static_cast<double>(capacity));
    stats.string_pct_valid = true;
    return true;
}
inline LogStats Logger::sample_stats() noexcept {
    LogStats stats;
    const auto now = std::chrono::system_clock::now();
    stats.timestamp_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count());
    stats.sample_count = stats_sample_count_;

    if (!log_queue_ || !string_queue_) {
        return stats;
    }

    const uint64_t consumed = read_index_.load(std::memory_order_relaxed);
    const uint64_t produced = log_queue_->initial_reading_index();
    const uint64_t depth = produced > consumed ? produced - consumed : 0;
    const uint64_t entry_capacity = log_queue_->size();
    // A shared-memory producer has no writer thread, so read_index_ never advances
    // and anything derived from it would be a fiction. Report those as zero, the
    // same reasoning flush() uses to return early in that mode.
    const bool has_reader = (mode_ != QueueMode::SharedProducer);

    stats.entries_produced = produced;
    stats.entries_consumed = has_reader ? consumed : 0;
    stats.entry_queue_depth = has_reader ? depth : 0;
    // The report-time reading is a sample too, and is taken after the last tick,
    // so fold it in - otherwise the "peak" could come out below the value in the
    // very same row.
    stats.entry_queue_depth_max = has_reader ? std::max(stats_depth_max_, depth) : 0;
    stats.entry_queue_capacity = static_cast<uint32_t>(entry_capacity);
    if (has_reader && stats_sample_count_ != 0) {
        stats.entry_queue_depth_mean =
            static_cast<double>(stats_depth_sum_) / static_cast<double>(stats_sample_count_);
    }
    if (has_reader && entry_capacity != 0) {
        const double capacity_d = static_cast<double>(entry_capacity);
        stats.entry_queue_pct =
            std::min(100.0, static_cast<double>(depth) * 100.0 / capacity_d);
        stats.entry_queue_pct_max = std::min(
            100.0, static_cast<double>(stats.entry_queue_depth_max) * 100.0 / capacity_d);
    }

    const uint64_t string_capacity = string_queue_->size();
    stats.string_bytes_written = string_queue_->initial_reading_index();
    stats.string_buffer_capacity = static_cast<uint32_t>(string_capacity);
    stats.entry_loss_count = log_queue_->loss_count();
    stats.string_loss_count = string_queue_->loss_count();

    if (has_reader) {
        // string_pct_valid covers the whole row, point value and peak alike. A row
        // whose ticks never resolved a frontier reports unknown rather than a
        // confident zero: a zero peak would read as "no pressure" at exactly the
        // moment the ring might be filling unobserved.
        if (sample_string_occupancy(stats)) {
            stats_string_seen_ = true;
            stats_string_last_ = stats.string_inflight_bytes;
        } else if (stats_string_seen_) {
            // This read found nothing, but a tick in this interval did. Report
            // that rather than the previous interval's, which is staler.
            stats.string_inflight_bytes = stats_string_last_;
            stats.string_pct_valid = true;
        } else {
            // Nothing seen at all this interval; carry the last known value so a
            // consumer sees the previous reading rather than a spurious zero.
            stats.string_inflight_bytes = stats_previous_.string_inflight_bytes;
            stats.string_pct_valid = false;
        }

        if (stats.string_pct_valid) {
            // The peak comes from the oversampling ticks, which catch the bursts a
            // single point sample misses.
            stats.string_inflight_bytes_max =
                std::max(stats_string_inflight_max_, stats.string_inflight_bytes);
        }
        if (string_capacity != 0) {
            const double capacity_d = static_cast<double>(string_capacity);
            stats.string_pct = std::min(
                100.0, static_cast<double>(stats.string_inflight_bytes) * 100.0 / capacity_d);
            stats.string_pct_max = std::min(
                100.0,
                static_cast<double>(stats.string_inflight_bytes_max) * 100.0 / capacity_d);
        }
    }

    if (stats_has_previous_) {
        const double elapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - stats_last_report_).count();
        stats.interval_sec = elapsed;
        if (elapsed > 0.0) {
            // Deltas over the MEASURED elapsed time, so a late wake-up shifts when
            // a sample lands but never distorts the rate it reports.
            const auto delta = [](uint64_t now_v, uint64_t then_v) noexcept -> double {
                return now_v > then_v ? static_cast<double>(now_v - then_v) : 0.0;
            };
            const double bytes =
                delta(stats.string_bytes_written, stats_previous_.string_bytes_written);
            stats.produced_per_sec =
                delta(stats.entries_produced, stats_previous_.entries_produced) / elapsed;
            stats.consumed_per_sec =
                delta(stats.entries_consumed, stats_previous_.entries_consumed) / elapsed;
            stats.string_bytes_per_sec = bytes / elapsed;
            if (string_capacity != 0) {
                const double capacity_d = static_cast<double>(string_capacity);
                stats.string_turnover_pct = bytes * 100.0 / capacity_d;
                stats.string_wraps_per_sec = bytes / capacity_d / elapsed;
            }
        }
    }
    return stats;
}

inline void Logger::stats_thread_func() {
    using clock = std::chrono::steady_clock;
    const auto tick = std::chrono::milliseconds(stats_sample_interval_ms_);
    const auto report_every = std::chrono::milliseconds(stats_interval_ms_);

    stats_last_report_ = clock::now();
    auto next_report = stats_last_report_ + report_every;

    while (running_.load(std::memory_order_relaxed)) {
        // Sleeping one tick at a time is what keeps shutdown prompt: atomic::wait
        // has no timeout in C++20, and a condition variable would need a mutex the
        // rest of this logger does without. The tick doubles as the oversampling
        // rate, so shutdown latency and gauge fidelity come from one knob.
        std::this_thread::sleep_for(tick);
        if (!running_.load(std::memory_order_relaxed)) {
            break;
        }
        accumulate_stats_sample();

        const auto now = clock::now();
        if (now < next_report) {
            continue;
        }
        const LogStats stats = sample_stats();
        publish_stats(stats);
        write_stats_row(stats);

        stats_previous_ = stats;
        stats_has_previous_ = true;
        stats_last_report_ = now;
        stats_depth_max_ = 0;
        stats_depth_sum_ = 0;
        stats_sample_count_ = 0;
        stats_string_inflight_max_ = 0;
        stats_string_last_ = 0;
        stats_string_seen_ = false;
        // Advance from the previous boundary rather than from now, so a late tick
        // does not push every later report further and further out.
        next_report += report_every;
        if (next_report <= now) {
            next_report = now + report_every;
        }
    }

    // A last report, so both the file and the snapshot capture the end state.
    accumulate_stats_sample();
    const LogStats final_stats = sample_stats();
    publish_stats(final_stats);
    write_stats_row(final_stats);
}

inline void Logger::write_stats_header() {
    static constexpr std::string_view kHeader =
        "timestamp,interval_sec,sample_count,"
        "entries_produced,entries_consumed,produced_per_sec,consumed_per_sec,"
        "entry_queue_depth,entry_queue_depth_max,entry_queue_depth_mean,"
        "entry_queue_capacity,entry_queue_pct,entry_queue_pct_max,"
        "string_inflight_bytes,string_inflight_bytes_max,string_buffer_capacity,"
        "string_pct,string_pct_max,string_pct_valid,"
        "string_bytes_written,string_bytes_per_sec,string_turnover_pct,"
        "string_wraps_per_sec,entry_loss_count,string_loss_count\n";
    stats_stream_ << kHeader;
    stats_bytes_written_ = kHeader.size();
}

inline void Logger::open_stats_csv() {
    if (stats_file_.empty()) {
        return; // snapshots only; nothing to write
    }
    detail::open_buffered_stream(stats_stream_, stats_stream_buffer_, stats_file_,
                                 std::ios::out | std::ios::trunc,
                                 detail::kStreamBufferSize);
    if (!stats_stream_) {
        throw std::runtime_error("Failed to open statistics file: " + stats_file_.string());
    }
    write_stats_header();
}

inline void Logger::roll_stats_csv() {
    stats_stream_.flush();
    stats_stream_.close();

    std::filesystem::path backup = stats_file_;
    backup.replace_extension();
    backup += ".1.csv";

    std::error_code ec;
    std::filesystem::remove(backup, ec);
    std::filesystem::rename(stats_file_, backup, ec);
    if (ec) {
        // Same fallback the daily sink uses: a rename across devices fails, a
        // copy does not.
        std::filesystem::copy_file(stats_file_, backup,
                                   std::filesystem::copy_options::overwrite_existing, ec);
        if (!ec) {
            std::filesystem::remove(stats_file_, ec);
        }
    }

    detail::open_buffered_stream(stats_stream_, stats_stream_buffer_, stats_file_,
                                 std::ios::out | std::ios::trunc,
                                 detail::kStreamBufferSize);
    if (stats_stream_) {
        write_stats_header();
    }
}

inline void Logger::write_stats_row(const LogStats& stats) {
    if (stats_file_.empty() || !stats_stream_.is_open()) {
        return;
    }
    if (stats_max_file_size_ != 0 && stats_bytes_written_ >= stats_max_file_size_) {
        roll_stats_csv();
        if (!stats_stream_.is_open()) {
            return;
        }
    }

    std::string row;
    row.reserve(256);
    TimestampFormatter{TimestampFormatter::Format::WITH_MICROSECONDS}
        .append_timestamp(row, stats.timestamp_ns);
    // No field can contain a comma, so the rows need no quoting.
    std::format_to(std::back_inserter(row),
                   ",{:.6f},{},{},{},{:.3f},{:.3f},{},{},{:.2f},{},{:.2f},{:.2f},"
                   "{},{},{},{:.2f},{:.2f},{},{},{:.3f},{:.3f},{:.3f},{},{}\n",
                   stats.interval_sec, stats.sample_count,
                   stats.entries_produced, stats.entries_consumed,
                   stats.produced_per_sec, stats.consumed_per_sec,
                   stats.entry_queue_depth, stats.entry_queue_depth_max,
                   stats.entry_queue_depth_mean, stats.entry_queue_capacity,
                   stats.entry_queue_pct, stats.entry_queue_pct_max,
                   stats.string_inflight_bytes, stats.string_inflight_bytes_max,
                   stats.string_buffer_capacity,
                   stats.string_pct, stats.string_pct_max,
                   stats.string_pct_valid ? 1 : 0,
                   stats.string_bytes_written, stats.string_bytes_per_sec,
                   stats.string_turnover_pct, stats.string_wraps_per_sec,
                   stats.entry_loss_count, stats.string_loss_count);

    stats_stream_ << row;
    stats_stream_.flush(); // a crash is exactly when these rows matter most
    stats_bytes_written_ += row.size();

    if (!stats_stream_ && !stats_write_failed_) {
        // Never throw here: this runs on a background thread, and a failed CSV must
        // not take the process down or stop stats_snapshot() from working. Say so
        // once rather than on every interval.
        stats_write_failed_ = true;
        std::fprintf(stderr, "SlickLogger: failed writing statistics to %s\n",
                     stats_file_.string().c_str());
    }
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
    // Before running_ and before any thread: open_stats_csv() throws on a file it
    // cannot create, and a throw from here must leave the logger untouched rather
    // than half-started with a live writer thread the failed caller never expects.
    if (stats_enabled_) {
        stats_depth_max_ = 0;
        stats_depth_sum_ = 0;
        stats_sample_count_ = 0;
        stats_string_inflight_max_ = 0;
        stats_string_last_ = 0;
        stats_string_seen_ = false;
        stats_bytes_written_ = 0;
        stats_has_previous_ = false;
        stats_write_failed_ = false;
        // Seeded with the ring cursor as it stands now, not with a "nothing known
        // yet" sentinel. Nothing this run reserves can end at or below it, so it
        // is a true floor, and until the first promotion it is also the exact
        // answer: the writer has confirmed no drain, so everything reserved since
        // start is still in flight. A fresh local ring starts at zero; a shared
        // segment starts wherever it was attached, which is why this is read
        // rather than assumed.
        stats_gen_entry_ = 0;
        stats_gen_string_ = 0;
        stats_gen_open_ = false;
        uint64_t string_floor =
            string_queue_ ? string_queue_->initial_reading_index() : 0;
        if (mode_ == QueueMode::SharedCollector && collect_backlog_) {
            // A collector replaying a backlog is the one case where the attach
            // point is NOT a floor. It deliberately rewinds its entry reader
            // below that point (see below), so the entries it is about to drain
            // own strings that producers reserved before it ever attached -
            // strings the attach cursor would declare free from the first
            // sample, reporting an idle ring throughout the replay. Rewind the
            // floor exactly the way the reader is rewound, to the oldest
            // position the ring can still hold: anything older than that has
            // been overwritten anyway, and what is left really is in flight
            // until the replay has drained it.
            const uint64_t capacity = string_queue_ ? string_queue_->size() : 0;
            string_floor = string_floor > capacity ? string_floor - capacity : 0;
        }
        stats_string_frontier_.store(string_floor, std::memory_order_relaxed);
        open_stats_csv();
    }

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

    // Spawned last, so the banner above is already in the queue rather than
    // racing the first sample. The CSV is already open by this point.
    if (stats_enabled_) {
        stats_thread_ = std::thread([this]() { stats_thread_func(); });
    }
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

    // Before the sinks are registered, so add_sink() hands each one the default.
    // An empty config.pattern leaves any earlier set_pattern() alone rather than
    // silently clearing it.
    if (!config.pattern.empty()) {
        set_pattern(config.pattern);
    }

    for (auto& sink : config.sinks) {
        add_sink(sink);
    }

    set_level(config.min_level);
    set_source_location_options(config.include_source_location);

    // Written before start() spawns the statistics thread, which is the only
    // reader of these afterwards, so they need no synchronization.
    stats_enabled_ = config.enable_stats;
    stats_file_ = config.stats_file;
    stats_interval_ms_ = std::max(1u, config.stats_interval_ms);
    // Oversampling faster than the reporting interval is the point; slower than it
    // would starve the reports, so clamp rather than honour it.
    stats_sample_interval_ms_ =
        std::clamp(config.stats_sample_interval_ms, 1u, stats_interval_ms_);
    stats_max_file_size_ = config.stats_max_file_size;

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
    // The single place every sink passes through, so applying the default here is
    // what lets set_pattern() be called before or after the add_*_sink() calls.
    // Unconditional even when there is no default, so the sink's record of what
    // it would inherit always matches this logger.
    sink->apply_default_pattern(pattern_store_, default_pattern_);
    sinks_.push_back(sink);
    if (!sink->name().empty()) {
        sinkname_index_map_[sink->name()] = sink->index();
    }
}

inline void Logger::set_pattern(std::string_view pattern) {
    // Compiled exactly once, here. An invalid pattern throws before any sink has
    // been touched, so a typo cannot leave half the sinks reformatted, and the
    // sinks that do take it then share the one formatter rather than each
    // compiling the same text over again. The loop itself cannot fail at all -
    // apply_default_pattern() is noexcept - so the compile is the only step that
    // can, and it happens before the first sink is reached.
    const PatternFormatter* compiled = pattern_store_->get(pattern);
    default_pattern_ = compiled;
    for (auto& sink : sinks_) {
        if (sink) {
            sink->apply_default_pattern(pattern_store_, compiled);
        }
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
#if SLICK_LOGGER_ENABLE_THREAD_ID
    entry.thread_id = detail::current_thread_id();
#endif
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
        // Must be joined before the queues are released below: the statistics
        // thread dereferences both of them on every tick. It notices running_
        // within one sample tick, so this never blocks for long.
        if (stats_thread_.joinable()) {
            stats_thread_.join();
        }
    }
    if (stats_stream_.is_open()) {
        stats_stream_.flush();
        stats_stream_.close();
    }
    stats_enabled_ = false;
    // Dropped here rather than only in reset(), because shutdown() is what turns
    // statistics off and stats_snapshot() documents a zeroed LogStats for a logger
    // with them disabled. Left in place, the last snapshot of a finished run stays
    // readable afterwards, and - since init() shuts down first - would resurface as
    // live-looking data after a re-init with statistics off, or after an init()
    // that threw out of open_stats_csv(). The statistics thread was joined above,
    // so nothing can publish a new snapshot over this one.
    stats_latest_.store(nullptr, std::memory_order_release);
    stats_file_.clear();
    stats_interval_ms_ = 0;
    stats_sample_interval_ms_ = 0;
    stats_max_file_size_ = 0;
    
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
    // Cleared for the same reason as log_level_ above. init(config) deliberately
    // treats an empty config.pattern as "leave the current default alone", so a
    // pattern surviving reset() would silently reappear on sinks registered by
    // the next init() - exactly the cross-contamination reset() exists to avoid.
    default_pattern_ = nullptr;

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

    // Same reasoning: shutdown() above joined the statistics thread, so nothing
    // else can be touching any of this, and clearing it leaves a reused Logger
    // indistinguishable from a fresh one. The published snapshot is not among
    // them - shutdown() drops that itself, so it is gone for every caller, not
    // just the ones that go on to reset().
    stats_previous_ = LogStats{};
    stats_has_previous_ = false;
    stats_string_frontier_.store(0, std::memory_order_relaxed);
    stats_gen_entry_ = 0;
    stats_gen_string_ = 0;
    stats_gen_open_ = false;
    stats_depth_max_ = 0;
    stats_depth_sum_ = 0;
    stats_sample_count_ = 0;
    stats_string_inflight_max_ = 0;
    stats_string_last_ = 0;
    stats_string_seen_ = false;
    stats_bytes_written_ = 0;
    stats_write_failed_ = false;
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

        if (stats_enabled_ && string_queue_) {
            // Placed AFTER the dispatch: the promotion below means "the writer is
            // done with everything under this position", which is only true once
            // the batch has actually been written.
            advance_string_frontier();
        }
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
