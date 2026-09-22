#include <slick/logger.hpp>
#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace slick::logger;

// LogEntry is the layout shared-memory producers and collectors agree on, so its
// size is part of the ABI, not an implementation detail. Pinned here so a field
// added without thinking about multi-process peers fails the build rather than a
// user's attach. %t adds exactly one uint32_t to the 1.3.0 layout.
#if (defined(_WIN64) || defined(__x86_64__) || defined(__aarch64__)) \
    && SLICK_LOGGER_MAX_ARGS == 20 && SLICK_LOGGER_TAG_SIZE == 16
static_assert(sizeof(LogEntry) == (SLICK_LOGGER_ENABLE_THREAD_ID ? 327 : 323),
              "LogEntry layout changed: every process sharing a segment must be rebuilt, "
              "and SLICK_LOGGER_ENABLE_THREAD_ID=OFF must still reproduce the 1.3.0 layout");
#endif

namespace {

// A timestamp with a distinct value in every sub-second field:
//   milliseconds 123, microseconds 123456, nanoseconds 123456789.
constexpr uint64_t kTimestamp = 1693038674123456789ULL;
// Microsecond part 000123, millisecond part 000: the zero-padding guard.
constexpr uint64_t kPaddedTimestamp = 1693038674000123000ULL;

/**
 * @brief Renders through the shared ISink path and keeps the result
 *
 * Exercises exactly what ConsoleSink and FileSink use, without a file or a
 * running logger, and doubles as a check that format_log_entry() is usable from
 * a custom sink.
 */
class CapturingSink : public ISink {
public:
    explicit CapturingSink(std::string&& name = "") : ISink(std::move(name)) {}

    void write(const LogEntry& entry) override {
        // Both or neither, as ConsoleSink does: an uncolored sink supplies no
        // reset escape either.
        last_ = colored_ ? format_log_entry(entry, &level_color, "<R>")
                         : format_log_entry(entry);
    }
    void flush() override {}

    const std::string& last() const noexcept { return last_; }

    /// Stand-in escapes: "<C>" for most levels, "<E>" for ERROR, so a test can
    /// tell which level the color was actually chosen from.
    static std::string_view level_color(LogLevel level) noexcept {
        return level == LogLevel::L_ERROR ? "<E>" : "<C>";
    }

    void enable_colors() { colored_ = true; }

    /// How many distinct compiled patterns this sink is holding on to.
    size_t owned_pattern_count() const noexcept { return owned_patterns_.size(); }

    /// Render one entry and return the line, for one-liner assertions.
    std::string render(const LogEntry& entry) {
        write(entry);
        return last_;
    }

private:
    std::string last_;
    bool colored_ = false;
};

LogEntry make_entry(uint64_t timestamp = kTimestamp, LogLevel level = LogLevel::L_INFO) {
    LogEntry entry;
    entry.level = level;
    entry.timestamp = timestamp;
    entry.format = {"hello world"};
    entry.arg_count = 0;
    return entry;
}

/**
 * @brief An entry whose message cannot be formatted
 *
 * A "d" presentation type against a string argument makes std::vformat throw,
 * which is what drives format_log_message's error path. Note the argument is
 * required: an entry with arg_count 0 is copied through verbatim and never fails.
 */
LogEntry make_bad_format_entry() {
    LogEntry entry;
    entry.level = LogLevel::L_INFO;
    entry.timestamp = kTimestamp;
    entry.format = {"value: {:d}"};
    entry.arg_count = 1;
    entry.args[0].type = ArgType::STRING_LITERAL;
    entry.args[0].value.literal_ptr = "not a number";
    return entry;
}

void add_source_location(LogEntry& entry, const char* file = "main.cpp", uint32_t line = 81) {
    entry.file = {file};
    entry.line = line;
    entry.flags |= kEntryHasSourceLocation;
}

/// Local broken-down time for a timestamp, so assertions stay timezone-agnostic.
std::tm local_tm(uint64_t timestamp_ns) {
    const time_t seconds = static_cast<time_t>(timestamp_ns / 1000000000ULL);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &seconds);
#else
    localtime_r(&seconds, &tm);
#endif
    return tm;
}

std::string render_with(std::string_view pattern, const LogEntry& entry) {
    CapturingSink sink;
    sink.set_pattern(pattern);
    return sink.render(entry);
}

std::string read_first_line(const std::string& path) {
    std::ifstream file(path);
    std::string line;
    std::getline(file, line);
    return line;
}

std::vector<std::string> read_lines(const std::string& path) {
    std::ifstream file(path);
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty()) {
            lines.push_back(line);
        }
    }
    return lines;
}

/**
 * @brief Block until @p counter moves, or give up
 *
 * What the concurrency tests below use to know the render thread is running, and
 * still running between one set_pattern() and the next. Scheduling cannot be
 * relied on for that: an optimized build reconfigures faster than a thread
 * starts, and the loop then finishes before a single render has happened, which
 * passes the test without having overlapped anything at all.
 *
 * Bounded, so a render thread that stops making progress fails the test instead
 * of hanging it.
 */
[[nodiscard]] bool wait_for_progress(const std::atomic<size_t>& counter) {
    const size_t seen = counter.load(std::memory_order_acquire);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (counter.load(std::memory_order_acquire) == seen) {
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        std::this_thread::yield();
    }
    return true;
}

/// Logger::start() logs a "SlickLogger v<x.y.z>" banner, so a test that goes
/// through the real logger has to skip it to reach its own entries.
std::vector<std::string> read_logged_lines(const std::string& path) {
    std::vector<std::string> lines = read_lines(path);
    std::erase_if(lines, [](const std::string& line) {
        return line.find("SlickLogger v") != std::string::npos;
    });
    return lines;
}

} // namespace

class PatternTest : public ::testing::Test {
protected:
    void TearDown() override {
        Logger::instance().reset();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        for (const char* file : {"pattern_default.log", "pattern_thread.log", "pattern_micro.log",
                                 "pattern_rotating.log", "pattern_daily.log", "pattern_global.log",
                                 "pattern_config.log", "pattern_reset.log"}) {
            std::error_code ec;
            std::filesystem::remove(file, ec);
        }
    }
};

// ---------------------------------------------------------------------------
// 1. Every flag renders
// ---------------------------------------------------------------------------

TEST_F(PatternTest, MessageAndLevelFlags) {
    LogEntry entry = make_entry();
    EXPECT_EQ(render_with("%v", entry), "hello world");
    EXPECT_EQ(render_with("%l", entry), "INFO");
    EXPECT_EQ(render_with("%L", entry), "I");

    entry.level = LogLevel::L_WARN;
    EXPECT_EQ(render_with("%l|%L", entry), "WARN|W");
    entry.level = LogLevel::L_FATAL;
    EXPECT_EQ(render_with("%l|%L", entry), "FATAL|F");
    entry.level = LogLevel::L_TRACE;
    EXPECT_EQ(render_with("%l|%L", entry), "TRACE|T");
}

TEST_F(PatternTest, LiteralsAndEscapedPercent) {
    const LogEntry entry = make_entry();
    EXPECT_EQ(render_with("literal text", entry), "literal text");
    EXPECT_EQ(render_with("100%% done: %v", entry), "100% done: hello world");
    EXPECT_EQ(render_with("[%l] %v <<", entry), "[INFO] hello world <<");
    // An empty pattern is not an empty line: it means "use the built-in layout".
    // EmptyPatternRestoresTheBuiltInLayout covers that.
}

TEST_F(PatternTest, SourceLocationFlags) {
    LogEntry entry = make_entry();
    add_source_location(entry);

    EXPECT_EQ(render_with("%s", entry), "main.cpp");
    EXPECT_EQ(render_with("%#", entry), "81");
    EXPECT_EQ(render_with("%@", entry), "main.cpp:81");
    EXPECT_EQ(render_with("%s:%#", entry), "main.cpp:81");
}

TEST_F(PatternTest, SourceLocationFlagsRenderEmptyWhenAbsent) {
    const LogEntry entry = make_entry();  // no kEntryHasSourceLocation
    EXPECT_EQ(render_with("%s", entry), "");
    EXPECT_EQ(render_with("%#", entry), "");
    EXPECT_EQ(render_with("%@", entry), "");
    EXPECT_EQ(render_with("[%@]%v", entry), "[]hello world");
}

TEST_F(PatternTest, ProcessIdTagAndSinkName) {
    LogEntry entry = make_entry();

    // A Local-mode entry carries pid 0 but was produced by this process, so %P
    // reports this process rather than a bare zero.
    EXPECT_EQ(render_with("%P", entry), std::to_string(detail::current_process_id()));
    EXPECT_EQ(render_with("%k", entry), "");

    entry.pid = 4242;
    std::snprintf(entry.tag, sizeof(entry.tag), "%s", "collector");
    EXPECT_EQ(render_with("%P", entry), "4242");
    EXPECT_EQ(render_with("%k", entry), "collector");
    EXPECT_EQ(render_with("%P:%k", entry), "4242:collector");

    CapturingSink named("audit");
    named.set_pattern("%n");
    EXPECT_EQ(named.render(entry), "audit");

    CapturingSink unnamed;
    unnamed.set_pattern("%n");
    EXPECT_EQ(unnamed.render(entry), "");
}

TEST_F(PatternTest, DateAndTimeFlagsMatchTheTimestampFormatter) {
    const LogEntry entry = make_entry();

    // Timezone-agnostic: compare against the formatter that already ships, rather
    // than against a hard-coded local time.
    const std::string as_default =
        TimestampFormatter(TimestampFormatter::Format::DEFAULT).format_timestamp(kTimestamp);
    const std::string as_micros =
        TimestampFormatter(TimestampFormatter::Format::WITH_MICROSECONDS).format_timestamp(kTimestamp);
    const std::string as_millis =
        TimestampFormatter(TimestampFormatter::Format::WITH_MILLISECONDS).format_timestamp(kTimestamp);
    const std::string as_iso =
        TimestampFormatter(TimestampFormatter::Format::ISO8601).format_timestamp(kTimestamp);

    EXPECT_EQ(render_with("%Y-%m-%d %H:%M:%S", entry), as_default);
    EXPECT_EQ(render_with("%Y-%m-%d %H:%M:%S.%f", entry), as_micros);
    EXPECT_EQ(render_with("%Y-%m-%d %H:%M:%S.%e", entry), as_millis);
    EXPECT_EQ(render_with("%Y-%m-%dT%H:%M:%S.%fZ", entry), as_iso);
    EXPECT_EQ(render_with("%T", entry), as_default.substr(11, 8));
    EXPECT_EQ(render_with("%Y", entry), as_default.substr(0, 4));
    EXPECT_EQ(render_with("%y", entry), as_default.substr(2, 2));
    EXPECT_EQ(render_with("%m", entry), as_default.substr(5, 2));
    EXPECT_EQ(render_with("%d", entry), as_default.substr(8, 2));
    EXPECT_EQ(render_with("%D", entry),
              as_default.substr(5, 2) + "/" + as_default.substr(8, 2) + "/" + as_default.substr(2, 2));
}

TEST_F(PatternTest, SubSecondFlagsAreZeroPadded) {
    const LogEntry entry = make_entry();
    EXPECT_EQ(render_with("%e", entry), "123");
    EXPECT_EQ(render_with("%f", entry), "123456");
    EXPECT_EQ(render_with("%F", entry), "123456789");

    // The padding guard: a microsecond part of 123 must not collapse to "123".
    const LogEntry padded = make_entry(kPaddedTimestamp);
    EXPECT_EQ(render_with("%e", padded), "000");
    EXPECT_EQ(render_with("%f", padded), "000123");
    EXPECT_EQ(render_with("%F", padded), "000123000");
}

TEST_F(PatternTest, TwelveHourAndNameFlags) {
    const LogEntry entry = make_entry();
    const std::tm tm = local_tm(kTimestamp);

    static constexpr const char* kWeekShort[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static constexpr const char* kWeekLong[] = {"Sunday", "Monday", "Tuesday", "Wednesday",
                                                "Thursday", "Friday", "Saturday"};
    static constexpr const char* kMonShort[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                                "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    static constexpr const char* kMonLong[] = {"January", "February", "March", "April",
                                               "May", "June", "July", "August",
                                               "September", "October", "November", "December"};

    EXPECT_EQ(render_with("%a", entry), kWeekShort[tm.tm_wday]);
    EXPECT_EQ(render_with("%A", entry), kWeekLong[tm.tm_wday]);
    EXPECT_EQ(render_with("%b", entry), kMonShort[tm.tm_mon]);
    EXPECT_EQ(render_with("%B", entry), kMonLong[tm.tm_mon]);

    int hour12 = tm.tm_hour % 12;
    if (hour12 == 0) {
        hour12 = 12;
    }
    char expected_hour[3];
    std::snprintf(expected_hour, sizeof(expected_hour), "%02d", hour12);
    EXPECT_EQ(render_with("%I", entry), expected_hour);
    EXPECT_EQ(render_with("%p", entry), tm.tm_hour < 12 ? "AM" : "PM");

    // "Www Mmm DD HH:MM:SS YYYY", the shape std::asctime produces.
    EXPECT_TRUE(std::regex_match(render_with("%c", entry),
                                 std::regex(R"([A-Z][a-z]{2} [A-Z][a-z]{2} \d{2} \d{2}:\d{2}:\d{2} \d{4})")))
        << render_with("%c", entry);

    EXPECT_EQ(render_with("%E", entry), std::to_string(kTimestamp / 1000000000ULL));
}

// %e, %f, %F and %E read the entry's own timestamp - a division and some
// digits - while every calendar flag needs the broken-down time behind
// cached_second(). Only the second group makes format() pay for that lookup, so
// the first group has to render correctly with no calendar flag beside it to
// force one, and both groups have to agree when they do share a line.
TEST_F(PatternTest, TimeFlagsNeedingNoBrokenDownTimeRenderOnTheirOwn) {
    const LogEntry entry = make_entry();
    const std::string epoch_seconds = std::to_string(kTimestamp / 1000000000ULL);

    EXPECT_EQ(render_with("%E", entry), epoch_seconds);
    EXPECT_EQ(render_with("%E.%F", entry), epoch_seconds + ".123456789");
    EXPECT_EQ(render_with("%e|%f|%F", entry), "123|123456|123456789");

    const std::string as_default =
        TimestampFormatter(TimestampFormatter::Format::DEFAULT).format_timestamp(kTimestamp);
    EXPECT_EQ(render_with("%H:%M:%S.%e", entry), as_default.substr(11, 8) + ".123");
    EXPECT_EQ(render_with("%E %T", entry), epoch_seconds + " " + as_default.substr(11, 8));
}

// ---------------------------------------------------------------------------
// 2. %+ is byte-identical to the built-in layout
// ---------------------------------------------------------------------------

TEST_F(PatternTest, DefaultHeaderFlagMatchesNoPatternExactly) {
    // Plain entry, entry with source location, entry with pid, entry with pid and
    // tag, and all of them together: %+ must reproduce each byte for byte.
    std::vector<LogEntry> entries;

    entries.push_back(make_entry());

    LogEntry with_location = make_entry();
    add_source_location(with_location);
    entries.push_back(with_location);

    LogEntry with_pid = make_entry();
    with_pid.pid = 9182;
    entries.push_back(with_pid);

    LogEntry with_tag = make_entry();
    with_tag.pid = 9182;
    std::snprintf(with_tag.tag, sizeof(with_tag.tag), "%s", "feed");
    entries.push_back(with_tag);

    LogEntry everything = make_entry(kTimestamp, LogLevel::L_ERROR);
    add_source_location(everything, "net/client.cpp", 204);
    everything.pid = 9182;
    std::snprintf(everything.tag, sizeof(everything.tag), "%s", "feed");
    entries.push_back(everything);

    for (const LogEntry& entry : entries) {
        CapturingSink built_in;              // no pattern at all
        CapturingSink explicit_default;
        explicit_default.set_pattern("%+");
        EXPECT_EQ(built_in.render(entry), explicit_default.render(entry));
    }
}

TEST_F(PatternTest, BuiltInLayoutShapeIsUnchanged) {
    LogEntry entry = make_entry();
    add_source_location(entry);
    entry.pid = 9182;
    std::snprintf(entry.tag, sizeof(entry.tag), "%s", "feed");

    CapturingSink sink;
    const std::string line = sink.render(entry);

    // "<ts> [INFO] [9182:feed] [main.cpp:81] hello world"
    EXPECT_TRUE(std::regex_match(
        line,
        std::regex(R"(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{6} \[INFO\] \[9182:feed\] \[main\.cpp:81\] hello world)")))
        << line;
}

// ---------------------------------------------------------------------------
// 3. The default timestamp is still microseconds
// ---------------------------------------------------------------------------

TEST_F(PatternTest, DefaultTimestampIsMicrosecondsForEverySink) {
    // Exactly six fractional digits: a slip to milliseconds or whole seconds fails.
    const std::regex micros(R"(^\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{6} \[INFO\] hello world$)");

    LogEntry entry = make_entry();

    {
        CapturingSink sink;
        EXPECT_TRUE(std::regex_match(sink.render(entry), micros)) << sink.render(entry);
    }
    {
        FileSink sink("pattern_micro.log");
        sink.write(entry);
        sink.flush();
    }
    EXPECT_TRUE(std::regex_match(read_first_line("pattern_micro.log"), micros))
        << read_first_line("pattern_micro.log");
    {
        RotationConfig rotation;
        RotatingFileSink sink("pattern_rotating.log", rotation);
        sink.write(entry);
        sink.flush();
    }
    EXPECT_TRUE(std::regex_match(read_first_line("pattern_rotating.log"), micros))
        << read_first_line("pattern_rotating.log");
    {
        RotationConfig rotation;
        DailyFileSink sink("pattern_daily.log", rotation);
        sink.write(entry);
        sink.flush();
    }
    EXPECT_TRUE(std::regex_match(read_first_line("pattern_daily.log"), micros))
        << read_first_line("pattern_daily.log");

    // The same six digits through %+ and through %q.
    CapturingSink via_default;
    via_default.set_pattern("%+");
    EXPECT_TRUE(std::regex_match(via_default.render(entry), micros)) << via_default.render(entry);

    CapturingSink via_configured;
    via_configured.set_pattern("%q");
    EXPECT_TRUE(std::regex_match(via_configured.render(entry),
                                 std::regex(R"(^\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{6}$)")))
        << via_configured.render(entry);
}

TEST_F(PatternTest, ConfiguredTimestampFlagFollowsTheSinkSetting) {
    const LogEntry entry = make_entry();

    CapturingSink sink;
    sink.set_pattern("%q");
    EXPECT_EQ(sink.render(entry),
              TimestampFormatter(TimestampFormatter::Format::WITH_MICROSECONDS).format_timestamp(kTimestamp));

    sink.set_timestamp_format(TimestampFormatter::Format::ISO8601);
    EXPECT_EQ(sink.render(entry),
              TimestampFormatter(TimestampFormatter::Format::ISO8601).format_timestamp(kTimestamp));

    sink.set_timestamp_format(TimestampFormatter::Format::TIME_ONLY);
    EXPECT_EQ(sink.render(entry),
              TimestampFormatter(TimestampFormatter::Format::TIME_ONLY).format_timestamp(kTimestamp));
}

// ---------------------------------------------------------------------------
// 4. Width and alignment
// ---------------------------------------------------------------------------

TEST_F(PatternTest, WidthPadsAndAligns) {
    LogEntry entry = make_entry();

    EXPECT_EQ(render_with("[%-8l]", entry), "[INFO    ]");  // left aligned
    EXPECT_EQ(render_with("[%8l]", entry), "[    INFO]");   // right aligned
    EXPECT_EQ(render_with("[%-5l]%v", entry), "[INFO ]hello world");

    // A field at least as wide as its width is left alone, never truncated.
    EXPECT_EQ(render_with("[%4l]", entry), "[INFO]");
    EXPECT_EQ(render_with("[%2l]", entry), "[INFO]");
    entry.level = LogLevel::L_TRACE;
    EXPECT_EQ(render_with("[%3l]", entry), "[TRACE]");

    // Padding applies to any flag, not just the level.
    entry.level = LogLevel::L_INFO;
    add_source_location(entry);
    EXPECT_EQ(render_with("[%-14@]", entry), "[main.cpp:81   ]");
    EXPECT_EQ(render_with("[%6#]", entry), "[    81]");
}

TEST_F(PatternTest, WidthIsCapped) {
    const LogEntry entry = make_entry();
    // 999 clamps to kMaxFieldWidth rather than producing a 999-char field.
    const std::string line = render_with("%999l", entry);
    EXPECT_EQ(line.size(), PatternFormatter::kMaxFieldWidth);
    EXPECT_EQ(line.substr(line.size() - 4), "INFO");
}

TEST_F(PatternTest, ColumnsLineUpAcrossLevels) {
    // The usual reason to customize a header: aligned columns.
    std::vector<std::string> lines;
    for (LogLevel level : {LogLevel::L_INFO, LogLevel::L_WARN, LogLevel::L_ERROR}) {
        LogEntry entry = make_entry(kTimestamp, level);
        add_source_location(entry);
        lines.push_back(render_with("%-5l %-20@ %v", entry));
    }
    const size_t message_column = lines.front().find("hello world");
    ASSERT_NE(message_column, std::string::npos);
    for (const std::string& line : lines) {
        EXPECT_EQ(line.find("hello world"), message_column) << line;
    }
}

// ---------------------------------------------------------------------------
// 5. Color range
// ---------------------------------------------------------------------------

TEST_F(PatternTest, ColorRangeWrapsOnlyTheMarkedSpan) {
    const LogEntry entry = make_entry();

    CapturingSink sink;
    sink.enable_colors();
    sink.set_pattern("%T %^%l%$ %v");

    const std::string line = sink.render(entry);
    EXPECT_NE(line.find("<C>INFO<R>"), std::string::npos) << line;
    // The line itself is not wrapped: the pattern marked its own range.
    EXPECT_FALSE(line.starts_with("<C>")) << line;
    EXPECT_TRUE(line.ends_with("hello world")) << line;
}

TEST_F(PatternTest, PatternWithoutColorRangeWrapsTheWholeLine) {
    const LogEntry entry = make_entry();

    CapturingSink sink;
    sink.enable_colors();
    sink.set_pattern("%T [%l] %v");

    const std::string line = sink.render(entry);
    EXPECT_TRUE(line.starts_with("<C>")) << line;
    EXPECT_TRUE(line.ends_with("<R>")) << line;
    EXPECT_EQ(line.find("<C>", 3), std::string::npos) << line;  // only the one
}

TEST_F(PatternTest, UnclosedColorRangeIsClosedAtEndOfLine) {
    // "%^" with no "%$" must not leave the escape active past the newline, or
    // everything the terminal prints afterwards stays tinted.
    CapturingSink sink;
    sink.enable_colors();
    sink.set_pattern("%T %^%l %v");

    const std::string line = sink.render(make_entry());
    EXPECT_NE(line.find("<C>"), std::string::npos) << line;
    EXPECT_TRUE(line.ends_with("<R>")) << line;
}

TEST_F(PatternTest, UnclosedColorRangeAfterAClosedOneIsAlsoClosed) {
    CapturingSink sink;
    sink.enable_colors();
    sink.set_pattern("%^%l%$ %^%v");  // second range left open

    const std::string line = sink.render(make_entry());
    EXPECT_TRUE(line.ends_with("<R>")) << line;
}

TEST_F(PatternTest, ClosedColorRangeIsNotClosedTwice) {
    CapturingSink sink;
    sink.enable_colors();
    sink.set_pattern("%^%l%$ %v");

    const std::string line = sink.render(make_entry());
    EXPECT_EQ(line, "<C>INFO<R> hello world");
}

TEST_F(PatternTest, ColorFollowsTheLevelTheLineReports) {
    // An unformattable message prints as ERROR, so it must be colored as ERROR
    // rather than in the original level's color.
    CapturingSink sink;
    sink.enable_colors();
    sink.set_pattern("%^%l%$ %v");

    const LogEntry good = make_entry();
    ASSERT_EQ(good.level, LogLevel::L_INFO);
    EXPECT_TRUE(sink.render(good).starts_with("<C>INFO<R>")) << sink.render(good);

    const LogEntry bad = make_bad_format_entry();
    ASSERT_EQ(bad.level, LogLevel::L_INFO);
    EXPECT_TRUE(sink.render(bad).starts_with("<E>ERROR<R>")) << sink.render(bad);
}

TEST_F(PatternTest, BuiltInLayoutAlsoColorsByTheReportedLevel) {
    CapturingSink sink;  // no pattern: the built-in layout
    sink.enable_colors();

    EXPECT_TRUE(sink.render(make_entry()).starts_with("<C>"));
    EXPECT_TRUE(sink.render(make_bad_format_entry()).starts_with("<E>"));
}

TEST_F(PatternTest, UncoloredSinkEmitsNoEscapesForAColorPattern) {
    const LogEntry entry = make_entry();

    // A pattern shared between a console sink and a file sink must not leak
    // escape codes into the file.
    CapturingSink file_like;  // no colors configured
    file_like.set_pattern("%T %^%l%$ %v");
    const std::string line = file_like.render(entry);
    EXPECT_EQ(line.find('\033'), std::string::npos) << line;
    EXPECT_NE(line.find("INFO"), std::string::npos) << line;
}

TEST_F(PatternTest, ConsoleSinkColorsWholeLineByDefault) {
    std::stringstream buffer;
    std::streambuf* old_cout = std::cout.rdbuf(buffer.rdbuf());
    {
        ConsoleSink sink(true, false);  // colors on, everything to stdout
        sink.write(make_entry());
    }
    std::cout.rdbuf(old_cout);

    const std::string output = buffer.str();
    EXPECT_TRUE(output.starts_with("\033[32m")) << output;  // green for INFO
    EXPECT_NE(output.find("\033[0m"), std::string::npos) << output;
}

// ---------------------------------------------------------------------------
// 6. Bad patterns throw, leaving the sink untouched
// ---------------------------------------------------------------------------

TEST_F(PatternTest, UnknownFlagsThrow) {
    CapturingSink sink;
    EXPECT_THROW(sink.set_pattern("%Z"), std::invalid_argument);
    EXPECT_THROW(sink.set_pattern("%w"), std::invalid_argument);
    EXPECT_THROW(sink.set_pattern("%"), std::invalid_argument);       // dangling
    EXPECT_THROW(sink.set_pattern("%-8"), std::invalid_argument);     // dangling after width
    EXPECT_THROW(sink.set_pattern("ok %v then %J"), std::invalid_argument);
}

TEST_F(PatternTest, WidthOnAnEscapedPercentThrows) {
    // "%-5%" used to swallow the -5 and emit a bare "%". Far more likely a
    // mistyped flag letter than a deliberate escape, so it is rejected.
    CapturingSink sink;
    EXPECT_THROW(sink.set_pattern("%-5%"), std::invalid_argument);
    EXPECT_THROW(sink.set_pattern("%8%"), std::invalid_argument);

    // A plain escape is still fine, with or without neighbours.
    EXPECT_NO_THROW(sink.set_pattern("100%% done"));
    EXPECT_EQ(sink.render(make_entry()), "100% done");
}

TEST_F(PatternTest, IdenticalPatternsReuseOneCompiledFormatter) {
    // Toggling between layouts at runtime must not retain a formatter per call:
    // nothing is ever reclaimed, so reuse is what bounds the growth.
    CapturingSink sink;
    const LogEntry entry = make_entry();

    for (int i = 0; i < 500; ++i) {
        sink.set_pattern("[%l] %v");
        EXPECT_EQ(sink.render(entry), "[INFO] hello world");
        sink.set_pattern("%L|%v");
        EXPECT_EQ(sink.render(entry), "I|hello world");
    }
    // 1000 set_pattern calls over two distinct patterns still render correctly;
    // the reuse itself is asserted by owned_pattern_count() staying at two.
    EXPECT_EQ(sink.owned_pattern_count(), 2u);
}

TEST_F(PatternTest, DistinctPatternsEachGetAFormatter) {
    CapturingSink sink;
    sink.set_pattern("%v");
    sink.set_pattern("%l");
    sink.set_pattern("%v");  // back to the first: reused, not a third
    EXPECT_EQ(sink.owned_pattern_count(), 2u);
}

TEST_F(PatternTest, UnsupportedSpdlogFlagsThrow) {
    CapturingSink sink;
    for (const char* pattern : {"%!", "%g", "%o", "%i", "%u", "%O", "%z"}) {
        EXPECT_THROW(sink.set_pattern(pattern), std::invalid_argument) << pattern;
    }
}

TEST_F(PatternTest, AFailedSetPatternKeepsThePreviousLayout) {
    const LogEntry entry = make_entry();

    CapturingSink sink;
    sink.set_pattern("[%l] %v");
    const std::string before = sink.render(entry);
    ASSERT_EQ(before, "[INFO] hello world");

    EXPECT_THROW(sink.set_pattern("%Z"), std::invalid_argument);
    EXPECT_EQ(sink.render(entry), before);
    EXPECT_EQ(sink.pattern(), "[%l] %v");
}

TEST_F(PatternTest, EmptyPatternRestoresTheBuiltInLayout) {
    const LogEntry entry = make_entry();

    CapturingSink sink;
    sink.set_pattern("[%l] %v");
    ASSERT_EQ(sink.render(entry), "[INFO] hello world");

    sink.set_pattern("");
    EXPECT_EQ(sink.pattern(), "");

    CapturingSink untouched;
    EXPECT_EQ(sink.render(entry), untouched.render(entry));
}

TEST_F(PatternTest, ThreadIdFlagMatchesTheBuildSetting) {
    CapturingSink sink;
#if SLICK_LOGGER_ENABLE_THREAD_ID
    EXPECT_NO_THROW(sink.set_pattern("%t"));
#else
    // Compiled out: the flag must say so rather than render nothing.
    EXPECT_THROW(sink.set_pattern("%t"), std::invalid_argument);
#endif
}

// ---------------------------------------------------------------------------
// 7. Format errors still surface as level ERROR
// ---------------------------------------------------------------------------

TEST_F(PatternTest, FormatErrorOverridesTheRenderedLevel) {
    const LogEntry entry = make_bad_format_entry();

    CapturingSink sink;
    sink.set_pattern("%l|%v");
    const std::string line = sink.render(entry);

    // The level printed is ERROR even though the entry is INFO, matching the
    // built-in layout's behaviour.
    EXPECT_TRUE(line.starts_with("ERROR|")) << line;

    CapturingSink built_in;
    EXPECT_NE(built_in.render(entry).find("[ERROR]"), std::string::npos) << built_in.render(entry);
}

TEST_F(PatternTest, FormatErrorOverridesEveryLevelFlagConsistently) {
    // %L derived its letter from LogEntry::level while %l used the rendered
    // level, so a failed INFO line printed "I" next to a FORMAT_ERROR message.
    // Both must report the effective level.
    const LogEntry entry = make_bad_format_entry();
    ASSERT_EQ(entry.level, LogLevel::L_INFO);

    CapturingSink sink;
    sink.set_pattern("%L");
    EXPECT_EQ(sink.render(entry), "E");

    sink.set_pattern("%l");
    EXPECT_EQ(sink.render(entry), "ERROR");

    // The two flags agree on the same line, for a good entry and a bad one.
    sink.set_pattern("%l|%L");
    EXPECT_EQ(sink.render(entry), "ERROR|E");
    EXPECT_EQ(sink.render(make_entry()), "INFO|I");
}

TEST_F(PatternTest, LevelFlagsStillForceTheMessagePass) {
    // The format pass is skipped for patterns that render neither the message
    // nor a level. A level flag must keep forcing it, or %l/%L would report the
    // original level for an entry whose message cannot be formatted.
    const LogEntry bad = make_bad_format_entry();

    CapturingSink with_level;
    with_level.set_pattern("%T [%l]");
    EXPECT_TRUE(with_level.render(bad).ends_with("[ERROR]")) << with_level.render(bad);

    CapturingSink with_short_level;
    with_short_level.set_pattern("%T [%L]");
    EXPECT_TRUE(with_short_level.render(bad).ends_with("[E]")) << with_short_level.render(bad);

    // %+ renders the message itself, so it forces the pass too.
    CapturingSink with_default;
    with_default.set_pattern("%+");
    EXPECT_NE(with_default.render(bad).find("[ERROR]"), std::string::npos);
}

TEST_F(PatternTest, MessageLessPatternRendersRegardlessOfFormatErrors) {
    // Nothing on this line depends on the message, so a broken format string
    // must not disturb it.
    LogEntry bad = make_bad_format_entry();
    add_source_location(bad);

    CapturingSink sink;
    sink.set_pattern("%T|%@");
    const std::string line = sink.render(bad);
    EXPECT_TRUE(line.ends_with("|main.cpp:81")) << line;
    EXPECT_EQ(line.find("FORMAT_ERROR"), std::string::npos) << line;

    // And a good entry through the same pattern is identical in shape.
    EXPECT_TRUE(sink.render(make_entry()).find('|') != std::string::npos);
}

TEST_F(PatternTest, LevelFlagsAgreeForEveryLevel) {
    CapturingSink sink;
    sink.set_pattern("%l|%L");
    const std::pair<LogLevel, const char*> expected[] = {
        {LogLevel::L_TRACE, "TRACE|T"}, {LogLevel::L_DEBUG, "DEBUG|D"},
        {LogLevel::L_INFO, "INFO|I"},   {LogLevel::L_WARN, "WARN|W"},
        {LogLevel::L_ERROR, "ERROR|E"}, {LogLevel::L_FATAL, "FATAL|F"},
    };
    for (const auto& [level, text] : expected) {
        EXPECT_EQ(sink.render(make_entry(kTimestamp, level)), text);
    }
}

TEST_F(PatternTest, FormatErrorDoesNotAffectLaterEntries) {
    CapturingSink sink;
    sink.set_pattern("%l|%v");

    const LogEntry bad = make_bad_format_entry();
    EXPECT_TRUE(sink.render(bad).starts_with("ERROR|")) << sink.render(bad);

    // The reused buffer must not leak the previous line into the next one.
    EXPECT_EQ(sink.render(make_entry()), "INFO|hello world");
}

// ---------------------------------------------------------------------------
// 8. Global default vs per-sink override
// ---------------------------------------------------------------------------

TEST_F(PatternTest, GlobalPatternReachesSinksAddedBeforeAndAfter) {
    auto before = std::make_shared<CapturingSink>("before");
    auto after = std::make_shared<CapturingSink>("after");

    Logger::instance().clear_sinks();
    Logger::instance().add_sink(before);
    Logger::instance().set_pattern("[%l] %v");
    Logger::instance().add_sink(after);

    const LogEntry entry = make_entry();
    EXPECT_EQ(before->render(entry), "[INFO] hello world");
    EXPECT_EQ(after->render(entry), "[INFO] hello world");
    EXPECT_EQ(Logger::instance().pattern(), "[%l] %v");
}

// A logger-wide pattern is compiled once and handed to every sink that inherits
// it, rather than recompiled per sink: an inheriting sink owns no formatter of
// its own at all, whether it was added before or after the pattern was set.
TEST_F(PatternTest, LoggerWidePatternIsCompiledOnceForEverySink) {
    auto first = std::make_shared<CapturingSink>("first");
    auto second = std::make_shared<CapturingSink>("second");
    auto third = std::make_shared<CapturingSink>("third");

    Logger::instance().clear_sinks();
    Logger::instance().add_sink(first);
    Logger::instance().add_sink(second);
    Logger::instance().set_pattern("[%l] %v");
    Logger::instance().add_sink(third);

    const LogEntry entry = make_entry();
    for (const auto& sink : {first, second, third}) {
        EXPECT_EQ(sink->render(entry), "[INFO] hello world") << sink->name();
        EXPECT_EQ(sink->owned_pattern_count(), 0u) << sink->name();
    }
}

// A sink can outlive the logger-wide default that reached it: a caller holding
// its own shared_ptr past reset(), or - the reason this matters - a plugin whose
// Logger goes away while the host still holds the sink. The sink keeps the
// compiled pattern alive, so rendering afterwards is the inherited layout rather
// than a read through a dangling pointer.
TEST_F(PatternTest, InheritedPatternSurvivesTheLoggerDroppingIt) {
    auto sink = std::make_shared<CapturingSink>("kept");

    Logger::instance().clear_sinks();
    Logger::instance().add_sink(sink);
    Logger::instance().set_pattern("[%l] %v");

    Logger::instance().reset();
    EXPECT_EQ(Logger::instance().pattern(), "");

    EXPECT_EQ(sink->render(make_entry()), "[INFO] hello world");
}

TEST_F(PatternTest, ExplicitSinkPatternSurvivesAGlobalPattern) {
    auto custom = std::make_shared<CapturingSink>("custom");
    auto plain = std::make_shared<CapturingSink>("plain");
    custom->set_pattern("<%v>");

    Logger::instance().clear_sinks();
    Logger::instance().add_sink(custom);
    Logger::instance().add_sink(plain);
    Logger::instance().set_pattern("[%l] %v");

    const LogEntry entry = make_entry();
    EXPECT_EQ(custom->render(entry), "<hello world>");   // untouched
    EXPECT_EQ(plain->render(entry), "[INFO] hello world");
}

TEST_F(PatternTest, ExplicitSinkPatternSurvivesBeingAddedAfterAGlobalPattern) {
    Logger::instance().clear_sinks();
    Logger::instance().set_pattern("[%l] %v");

    auto custom = std::make_shared<CapturingSink>("custom");
    custom->set_pattern("<%v>");
    Logger::instance().add_sink(custom);

    EXPECT_EQ(custom->render(make_entry()), "<hello world>");
}

TEST_F(PatternTest, EmptySinkPatternStopsOverridingTheLoggerDefault) {
    // set_pattern("") reads as "stop overriding", so a later logger-wide pattern
    // must reach the sink again. Pinning it forever left no way to un-override.
    auto sink = std::make_shared<CapturingSink>("sink");

    Logger::instance().clear_sinks();
    Logger::instance().add_sink(sink);
    sink->set_pattern("<%v>");
    ASSERT_EQ(sink->render(make_entry()), "<hello world>");

    sink->set_pattern("");
    Logger::instance().set_pattern("[%l] %v");
    EXPECT_EQ(sink->render(make_entry()), "[INFO] hello world");
    EXPECT_EQ(sink->pattern(), "[%l] %v");
}

TEST_F(PatternTest, EmptySinkPatternFallsBackToTheDefaultAlreadyInForce) {
    // Not just future defaults: clearing the override must immediately restore
    // the default that is already set.
    auto sink = std::make_shared<CapturingSink>("sink");

    Logger::instance().clear_sinks();
    Logger::instance().set_pattern("[%l] %v");
    Logger::instance().add_sink(sink);
    sink->set_pattern("<%v>");
    ASSERT_EQ(sink->render(make_entry()), "<hello world>");

    sink->set_pattern("");
    EXPECT_EQ(sink->render(make_entry()), "[INFO] hello world");
}

TEST_F(PatternTest, OverrideCanBeSetClearedAndSetAgain) {
    auto sink = std::make_shared<CapturingSink>("sink");
    Logger::instance().clear_sinks();
    Logger::instance().add_sink(sink);
    Logger::instance().set_pattern("[%l] %v");

    const LogEntry entry = make_entry();
    EXPECT_EQ(sink->render(entry), "[INFO] hello world");   // inherited
    sink->set_pattern("<%v>");
    EXPECT_EQ(sink->render(entry), "<hello world>");        // overridden
    sink->set_pattern("");
    EXPECT_EQ(sink->render(entry), "[INFO] hello world");   // inherited again
    sink->set_pattern("{%L}");
    EXPECT_EQ(sink->render(entry), "{I}");                  // overridden again

    // And the logger default can still be changed underneath an override,
    // taking effect once the override is dropped.
    Logger::instance().set_pattern("%L|%v");
    EXPECT_EQ(sink->render(entry), "{I}");
    sink->set_pattern("");
    EXPECT_EQ(sink->render(entry), "I|hello world");
}

TEST_F(PatternTest, EmptySinkPatternWithNoLoggerDefaultIsTheBuiltInLayout) {
    // The standalone case: nothing to inherit means the built-in layout.
    auto sink = std::make_shared<CapturingSink>("sink");
    Logger::instance().clear_sinks();
    Logger::instance().add_sink(sink);

    sink->set_pattern("<%v>");
    sink->set_pattern("");

    CapturingSink untouched;
    EXPECT_EQ(sink->render(make_entry()), untouched.render(make_entry()));
}

TEST_F(PatternTest, InvalidGlobalPatternChangesNoSink) {
    auto sink = std::make_shared<CapturingSink>("sink");

    Logger::instance().clear_sinks();
    Logger::instance().add_sink(sink);
    Logger::instance().set_pattern("[%l] %v");

    EXPECT_THROW(Logger::instance().set_pattern("%Z"), std::invalid_argument);
    EXPECT_EQ(sink->render(make_entry()), "[INFO] hello world");
    EXPECT_EQ(Logger::instance().pattern(), "[%l] %v");
}

TEST_F(PatternTest, ConfigPatternAppliesToConfiguredSinks) {
    LogConfig config;
    config.sinks.push_back(std::make_shared<FileSink>("pattern_config.log"));
    config.pattern = "%L|%v";
    Logger::instance().init(config);

    LOG_INFO("config pattern");
    Logger::instance().reset();

    const std::vector<std::string> lines = read_logged_lines("pattern_config.log");
    ASSERT_EQ(lines.size(), 1u);
    EXPECT_EQ(lines.front(), "I|config pattern");
}

TEST_F(PatternTest, ResetClearsTheLoggerWidePattern) {
    // reset() promises fresh state, and init(config) treats an empty
    // config.pattern as "leave the default alone". A pattern surviving reset()
    // therefore reappeared on sinks registered by the next init().
    Logger::instance().clear_sinks();
    Logger::instance().set_pattern("%L|%v");
    ASSERT_EQ(Logger::instance().pattern(), "%L|%v");

    Logger::instance().reset();
    EXPECT_EQ(Logger::instance().pattern(), "");

    LogConfig config;  // pattern deliberately left empty
    config.sinks.push_back(std::make_shared<FileSink>("pattern_reset.log"));
    Logger::instance().init(config);
    LOG_INFO("after reset");
    Logger::instance().reset();

    const std::vector<std::string> lines = read_logged_lines("pattern_reset.log");
    ASSERT_EQ(lines.size(), 1u);
    // The built-in layout, not the "%L|%v" set before the reset.
    EXPECT_TRUE(std::regex_match(
        lines.front(),
        std::regex(R"(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{6} \[INFO\] \[test_pattern\.cpp:\d+\] after reset)")))
        << lines.front();
}

TEST_F(PatternTest, AnEmptyConfigPatternKeepsAnExistingDefault) {
    // The other half of the contract: without a reset, an empty config.pattern
    // must not silently wipe a pattern the caller just set.
    Logger::instance().clear_sinks();
    Logger::instance().set_pattern("%L|%v");

    LogConfig config;  // pattern deliberately left empty
    config.sinks.push_back(std::make_shared<FileSink>("pattern_reset.log"));
    Logger::instance().init(config);
    LOG_INFO("kept");
    Logger::instance().reset();

    const std::vector<std::string> lines = read_logged_lines("pattern_reset.log");
    ASSERT_EQ(lines.size(), 1u);
    EXPECT_EQ(lines.front(), "I|kept");
}

TEST_F(PatternTest, EndToEndThroughTheRealLogger) {
    Logger::instance().clear_sinks();
    Logger::instance().add_file_sink("pattern_global.log");
    Logger::instance().set_pattern("%L [%s:%#] %v");
    Logger::instance().init(1024);

    LOG_INFO("value is {}", 42);
    Logger::instance().reset();

    const std::vector<std::string> lines = read_logged_lines("pattern_global.log");
    ASSERT_EQ(lines.size(), 1u);
    EXPECT_TRUE(std::regex_match(lines.front(),
                                 std::regex(R"(I \[test_pattern\.cpp:\d+\] value is 42)")))
        << lines.front();
}

// ---------------------------------------------------------------------------
// 9. %t carries the producing thread
// ---------------------------------------------------------------------------

#if SLICK_LOGGER_ENABLE_THREAD_ID
TEST_F(PatternTest, ThreadIdIsTheProducingThreadNotTheWriter) {
    Logger::instance().clear_sinks();
    Logger::instance().add_file_sink("pattern_thread.log");
    Logger::instance().set_pattern("%t|%v");
    Logger::instance().init(1024);

    uint32_t first_id = 0;
    uint32_t second_id = 0;
    std::thread first([&] {
        first_id = detail::current_thread_id();
        LOG_INFO("first");
    });
    std::thread second([&] {
        second_id = detail::current_thread_id();
        LOG_INFO("second");
    });
    first.join();
    second.join();
    Logger::instance().reset();

    const std::vector<std::string> lines = read_logged_lines("pattern_thread.log");
    ASSERT_EQ(lines.size(), 2u);

    std::set<std::string> rendered;
    for (const std::string& line : lines) {
        const size_t bar = line.find('|');
        ASSERT_NE(bar, std::string::npos) << line;
        rendered.insert(line.substr(0, bar));
    }

    // Two producing threads, two distinct ids. Had the entry been stamped on the
    // writer thread instead, both lines would carry one id.
    EXPECT_EQ(rendered.size(), 2u);
    EXPECT_NE(first_id, second_id);
    EXPECT_EQ(rendered.count(std::to_string(first_id)), 1u) << "missing " << first_id;
    EXPECT_EQ(rendered.count(std::to_string(second_id)), 1u) << "missing " << second_id;
    // Nor is it this thread, which only started them.
    EXPECT_EQ(rendered.count(std::to_string(detail::current_thread_id())), 0u);
}
#endif

// ---------------------------------------------------------------------------
// 10. A pattern is compiled once, not per entry
// ---------------------------------------------------------------------------

TEST_F(PatternTest, RepeatedRenderingIsStable) {
    // Guards the reused format buffer: 1000 renders must all produce the same
    // line, with nothing carried over between them.
    CapturingSink sink;
    sink.set_pattern("%Y-%m-%d %H:%M:%S.%f [%-5l] %v");
    const LogEntry entry = make_entry();

    const std::string expected = sink.render(entry);
    for (int i = 0; i < 1000; ++i) {
        EXPECT_EQ(sink.render(entry), expected);
    }

    // And a longer line followed by a shorter one does not leave a tail behind.
    LogEntry longer = make_entry();
    longer.format = {"a much longer message than the previous one"};
    EXPECT_NE(sink.render(longer), expected);
    EXPECT_EQ(sink.render(entry), expected);
}

TEST_F(PatternTest, PatternAccessorReportsWhatWasSet) {
    CapturingSink sink;
    EXPECT_EQ(sink.pattern(), "");
    sink.set_pattern("%T %v");
    EXPECT_EQ(sink.pattern(), "%T %v");
    sink.set_pattern("%L|%v");
    EXPECT_EQ(sink.pattern(), "%L|%v");
}

// ---------------------------------------------------------------------------
// 11. Reconfiguring while a sink renders
// ---------------------------------------------------------------------------

// The only reason the writer thread may hold a bare pointer to a compiled
// pattern is that the store is grow-only and backed by a deque: compiling a new
// pattern never moves or frees one already handed out. That is the whole safety
// argument for set_pattern() being callable while logging is in flight, so it
// gets a test rather than only a comment - and under a thread sanitizer this is
// where a race on the handoff would surface.
TEST_F(PatternTest, LoggerPatternCanChangeWhileASinkRenders) {
    // All distinct, so each set_pattern() below really does compile a formatter
    // and push it into the store the render thread is reading through.
    constexpr int kPatternCount = 128;
    const LogEntry entry = make_entry();
    std::vector<std::string> patterns;
    std::set<std::string> expected;
    for (int i = 0; i < kPatternCount; ++i) {
        patterns.push_back(std::format("%T.%e [#{}] %v", i));
        expected.insert(render_with(patterns.back(), entry));
    }

    auto sink = std::make_shared<CapturingSink>("racing");
    Logger::instance().clear_sinks();
    Logger::instance().add_sink(sink);
    // The layout in force until the first set_pattern() lands.
    expected.insert(sink->render(entry));

    std::atomic<bool> stop{false};
    std::atomic<size_t> render_count{0};
    std::vector<std::string> unexpected; // render thread only, read after the join

    std::thread renderer([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            const std::string line = sink->render(entry);
            if (!expected.contains(line)) {
                unexpected.push_back(line);
            }
            render_count.fetch_add(1, std::memory_order_relaxed);
        }
    });

    // Nothing below may depend on the two threads happening to interleave. The
    // renderer is confirmed live before the first pattern is set, and confirmed to
    // have rendered again after each one, so every layout in the loop is seen by a
    // render that ran while the store was being written to.
    bool renderer_running = wait_for_progress(render_count);

    // The first pass compiles every pattern, growing the store under the reader.
    // The second re-sets the same ones, which is the pure republish path.
    for (int pass = 0; pass < 2 && renderer_running; ++pass) {
        for (const std::string& pattern : patterns) {
            Logger::instance().set_pattern(pattern);
            renderer_running = wait_for_progress(render_count);
            if (!renderer_running) {
                break;
            }
        }
    }

    stop.store(true, std::memory_order_relaxed);
    renderer.join();

    // Not just "it did not crash": the render loop has to have run alongside the
    // reconfiguration for any of this to have been tested. Guaranteed rather than
    // hoped for now - one render per pattern is waited on above.
    EXPECT_TRUE(renderer_running) << "the render thread stopped making progress";
    EXPECT_GE(render_count.load(), static_cast<size_t>(2 * kPatternCount));
    // Every line rendered was one of the layouts that were in force - never a
    // half-applied one, and never a read through a formatter that had moved.
    EXPECT_TRUE(unexpected.empty())
        << unexpected.size() << " unrecognized line(s), first: "
        << (unexpected.empty() ? std::string{} : unexpected.front());
    // The layout the last set_pattern() installed is the one left standing.
    EXPECT_EQ(sink->render(entry), render_with(patterns.back(), entry));
    // And the sink compiled none of them itself: the logger-wide pattern is shared.
    EXPECT_EQ(sink->owned_pattern_count(), 0u);
}

TEST_F(PatternTest, SinkPatternCanChangeWhileTheSinkRenders) {
    // The same handoff one level down: the sink's own store, grown by its own
    // set_pattern() rather than by the logger's.
    constexpr int kPatternCount = 64;
    const LogEntry entry = make_entry();
    std::vector<std::string> patterns;
    std::set<std::string> expected;
    for (int i = 0; i < kPatternCount; ++i) {
        patterns.push_back(std::format("[#{}] %l %v", i));
        expected.insert(render_with(patterns.back(), entry));
    }

    CapturingSink sink;
    expected.insert(sink.render(entry));

    std::atomic<bool> stop{false};
    std::atomic<size_t> render_count{0};
    std::vector<std::string> unexpected;
    std::thread renderer([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            const std::string line = sink.render(entry);
            if (!expected.contains(line)) {
                unexpected.push_back(line);
            }
            render_count.fetch_add(1, std::memory_order_relaxed);
        }
    });

    bool renderer_running = wait_for_progress(render_count);
    for (const std::string& pattern : patterns) {
        sink.set_pattern(pattern);
        renderer_running = wait_for_progress(render_count);
        if (!renderer_running) {
            break;
        }
    }

    stop.store(true, std::memory_order_relaxed);
    renderer.join();

    EXPECT_TRUE(renderer_running) << "the render thread stopped making progress";
    EXPECT_GE(render_count.load(), static_cast<size_t>(kPatternCount));
    EXPECT_TRUE(unexpected.empty())
        << unexpected.size() << " unrecognized line(s), first: "
        << (unexpected.empty() ? std::string{} : unexpected.front());
    EXPECT_EQ(sink.owned_pattern_count(), static_cast<size_t>(kPatternCount));
    EXPECT_EQ(sink.render(entry), render_with(patterns.back(), entry));
}
