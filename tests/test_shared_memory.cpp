// Multi-process logging tests.
//
// Covers the shared-memory queue modes: entries produced in one process (or one
// logger) carry ring-buffer offsets instead of addresses, and the collector
// resolves them against its own mapping before handing them to sinks.

#include <slick/logger.hpp>
#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifdef _WIN32
#  include <process.h>
#  define SLICK_TEST_GETPID _getpid
#else
#  include <sys/wait.h>
#  include <unistd.h>
#  define SLICK_TEST_GETPID getpid
#endif

// Resolved at cmake configure time; the build system defines this macro.
#ifndef SHM_PRODUCER_EXE_PATH
#  error "SHM_PRODUCER_EXE_PATH must be defined by CMake"
#endif

using slick::logger::LogConfig;
using slick::logger::Logger;
using slick::logger::LogLevel;
using slick::logger::QueueMode;
using slick::logger::Logger;

namespace {

// Segment names must be unique per test so that a leftover segment from an
// earlier run (or a parallel ctest job) cannot be attached by mistake.
std::string unique_segment_name(const char* prefix) {
    static int counter = 0;
    return std::string(prefix) + std::to_string(SLICK_TEST_GETPID()) + "_" + std::to_string(++counter);
}

std::vector<std::string> read_lines(const std::filesystem::path& path) {
    std::vector<std::string> lines;
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) {
        lines.push_back(line);
    }
    return lines;
}

std::string read_all(const std::filesystem::path& path) {
    std::ifstream f(path);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

size_t count_lines_containing(const std::vector<std::string>& lines, std::string_view needle) {
    size_t n = 0;
    for (const auto& line : lines) {
        if (line.find(needle) != std::string::npos) {
            ++n;
        }
    }
    return n;
}

// Launch the producer executable and wait for it to exit, returning its exit code.
int run_producer(const std::string& args) {
#ifdef _WIN32
    // cmd.exe strips the outer pair of quotes, so the whole command needs one
    // extra layer for paths that contain spaces.
    const std::string command = std::string("\"\"") + SHM_PRODUCER_EXE_PATH + "\" " + args + "\"";
    return std::system(command.c_str());
#else
    const std::string command = std::string("\"") + SHM_PRODUCER_EXE_PATH + "\" " + args;
    const int status = std::system(command.c_str());
    if (status == -1) {
        return -1;
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

// Wait until `predicate` holds or the timeout expires. The collector drains on a
// background thread, so tests poll rather than sleeping a fixed amount.
template<typename Predicate>
bool wait_for(Predicate predicate, std::chrono::milliseconds timeout = std::chrono::seconds(10)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return predicate();
}

class SharedMemoryTest : public ::testing::Test {
protected:
    void TearDown() override {
        Logger::instance().shutdown();
        Logger::instance().reset();
        for (const auto& path : temp_files_) {
            std::error_code ec;
            std::filesystem::remove(path, ec);
        }
        temp_files_.clear();
    }

    std::filesystem::path temp_log(const std::string& name) {
        std::filesystem::path path = name;
        std::error_code ec;
        std::filesystem::remove(path, ec);
        temp_files_.push_back(path);
        return path;
    }

    std::vector<std::filesystem::path> temp_files_;
};

} // namespace

// A collector is also a producer of its own entries, so a single process in
// SharedCollector mode exercises the full offset encode/resolve round trip.
TEST_F(SharedMemoryTest, CollectorResolvesItsOwnEntries) {
    const auto log_path = temp_log("test_shm_roundtrip.log");
    const auto segment = unique_segment_name("slt_rt_");

    LogConfig config;
    config.mode = QueueMode::SharedCollector;
    config.shared_memory_name = segment;
    config.process_tag = "collector";
    config.log_queue_size = 1024;
    config.string_buffer_size = 1 << 16;
    config.sinks.push_back(std::make_shared<slick::logger::FileSink>(log_path));
    Logger::instance().init(config);

    const std::string dynamic_string = "dynamic-value";
    const char* c_string = "c-string-value";
    char char_array[] = "char-array-value";

    LOG_INFO("literal only");
    LOG_INFO("integers {} {} {}", 42, -7, 3.5);
    LOG_INFO("strings {} {} {}", dynamic_string, c_string, char_array);
    LOG_WARN("runtime format {}", std::string("through-string-queue"));

    Logger::instance().flush();
    ASSERT_TRUE(wait_for([&] { return read_all(log_path).find("runtime format") != std::string::npos; }));
    Logger::instance().shutdown();

    const std::string contents = read_all(log_path);
    EXPECT_NE(contents.find("literal only"), std::string::npos);
    EXPECT_NE(contents.find("integers 42 -7 3.5"), std::string::npos);
    EXPECT_NE(contents.find("strings dynamic-value c-string-value char-array-value"), std::string::npos);
    EXPECT_NE(contents.find("runtime format through-string-queue"), std::string::npos);

    // Every entry carries this process's id and tag.
    const std::string stamp = "[" + std::to_string(SLICK_TEST_GETPID()) + ":collector]";
    EXPECT_NE(contents.find(stamp), std::string::npos);

    // Source location survives the offset round trip.
    EXPECT_NE(contents.find("test_shared_memory.cpp:"), std::string::npos);
}

// The real thing: a separate process enqueues, this process collects.
TEST_F(SharedMemoryTest, CollectsEntriesFromAnotherProcess) {
    const auto log_path = temp_log("test_shm_two_process.log");
    const auto segment = unique_segment_name("slt_2p_");
    constexpr int kMessageCount = 50;

    LogConfig config;
    config.mode = QueueMode::SharedCollector;
    config.shared_memory_name = segment;
    config.process_tag = "collector";
    config.log_queue_size = 1024;
    config.string_buffer_size = 1 << 16;
    config.sinks.push_back(std::make_shared<slick::logger::FileSink>(log_path));
    Logger::instance().init(config);

    const int rc = run_producer("--name " + segment + " --tag prod --count " + std::to_string(kMessageCount)
                                + " --queue-size 1024 --string-buffer-size 65536");
    ASSERT_EQ(rc, 0) << "producer process failed";

    ASSERT_TRUE(wait_for([&] {
        return read_all(log_path).find("producer done") != std::string::npos;
    })) << "collector never saw the producer's final message";

    Logger::instance().shutdown();

    const auto lines = read_lines(log_path);
    EXPECT_EQ(count_lines_containing(lines, "producer message"), static_cast<size_t>(kMessageCount));
    EXPECT_EQ(count_lines_containing(lines, "dynamic-prod"), static_cast<size_t>(kMessageCount));

    // The binary payload survived the ring-offset round trip into this process.
    EXPECT_EQ(count_lines_containing(lines, "producer payload 00deadbeef0a"), 1u);

    // The producer's pid and tag are recorded, and they are not this process's.
    const std::string own_stamp = "[" + std::to_string(SLICK_TEST_GETPID()) + ":collector]";
    size_t producer_lines = 0;
    for (const auto& line : lines) {
        if (line.find("producer message") != std::string::npos) {
            EXPECT_NE(line.find(":prod]"), std::string::npos) << line;
            EXPECT_EQ(line.find(own_stamp), std::string::npos) << line;
            ++producer_lines;
        }
    }
    EXPECT_EQ(producer_lines, static_cast<size_t>(kMessageCount));
}

// Startup order must not matter: the producer creates the segment here.
TEST_F(SharedMemoryTest, ProducerMayStartBeforeCollector) {
    const auto log_path = temp_log("test_shm_producer_first.log");
    const auto segment = unique_segment_name("slt_pf_");

    // The producer lingers so its mapping keeps the segment alive while the
    // collector attaches. Its entries are already published by then.
    std::thread producer([&] {
        run_producer("--name " + segment + " --tag early --count 5"
                     " --queue-size 1024 --string-buffer-size 65536 --linger-ms 3000");
    });

    // Give the producer a head start so it is the process that creates the segment.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    LogConfig config;
    config.mode = QueueMode::SharedCollector;
    config.shared_memory_name = segment;
    config.process_tag = "late";
    config.log_queue_size = 1024;
    config.string_buffer_size = 1 << 16;
    config.sinks.push_back(std::make_shared<slick::logger::FileSink>(log_path));
    Logger::instance().init(config);

    const bool saw_messages = wait_for([&] {
        return read_all(log_path).find("producer message") != std::string::npos;
    });

    producer.join();
    Logger::instance().shutdown();

    EXPECT_TRUE(saw_messages) << "collector attaching second saw nothing:\n" << read_all(log_path);
}

// A producer that created the segment and then exits must not take the segment
// with it. On POSIX slick-queue's destructor shm_unlink()s a segment its owner
// created, which frees the name while the collector's mapping stays valid: later
// producers would then create a different segment under the same name and their
// entries would never reach the still-attached collector.
TEST_F(SharedMemoryTest, SegmentSurvivesTheProducerThatCreatedIt) {
    const auto log_path = temp_log("test_shm_creator_exit.log");
    const auto segment = unique_segment_name("slt_ce_");

    // Producer "first" creates the segment and lingers just long enough for the
    // collector to attach while it is still alive.
    std::thread first([&] {
        run_producer("--name " + segment + " --tag first --count 3"
                     " --queue-size 1024 --string-buffer-size 65536 --linger-ms 1500");
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    LogConfig config;
    config.mode = QueueMode::SharedCollector;
    config.shared_memory_name = segment;
    config.log_queue_size = 1024;
    config.string_buffer_size = 1 << 16;
    config.sinks.push_back(std::make_shared<slick::logger::FileSink>(log_path));
    Logger::instance().init(config);

    ASSERT_TRUE(wait_for([&] {
        return read_all(log_path).find("dynamic-first") != std::string::npos;
    })) << "collector never saw the creating producer";

    // The creator now exits, releasing (and previously unlinking) the segment.
    first.join();

    // A second producer starts afterwards. It must land in the same segment the
    // collector is still draining.
    ASSERT_EQ(run_producer("--name " + segment + " --tag second --count 3"
                           " --queue-size 1024 --string-buffer-size 65536"), 0);

    const bool saw_second = wait_for([&] {
        return read_all(log_path).find("dynamic-second") != std::string::npos;
    });

    Logger::instance().shutdown();
    EXPECT_TRUE(saw_second)
        << "producer starting after the segment's creator exited was orphaned:\n"
        << read_all(log_path);
}

// string_items_per_slot is part of the string segment's layout, so it is the
// creator's call: a producer configured differently must adopt the collector's
// value rather than fail to attach or misread the ring.
TEST_F(SharedMemoryTest, AttacherAdoptsCreatorsStringItemsPerSlot) {
    const auto log_path = temp_log("test_shm_items_per_slot.log");
    const auto segment = unique_segment_name("slt_ips_");
    constexpr uint32_t kCreatorItemsPerSlot = 128;

    LogConfig config;
    config.mode = QueueMode::SharedCollector;
    config.shared_memory_name = segment;
    config.log_queue_size = 1024;
    config.string_buffer_size = 1 << 16;
    config.string_items_per_slot = kCreatorItemsPerSlot;
    config.sinks.push_back(std::make_shared<slick::logger::FileSink>(log_path));
    Logger::instance().init(config);

    {
        slick::queue<char, slick::logger::detail::logger_queue_traits> ring((segment + "_str").c_str());
        EXPECT_EQ(ring.items_per_slot(), kCreatorItemsPerSlot);
    }

    ASSERT_EQ(run_producer("--name " + segment + " --tag ips --count 5"
                           " --queue-size 1024 --string-buffer-size 65536 --string-items-per-slot 8"), 0)
        << "a producer with a different string_items_per_slot failed to attach";

    ASSERT_TRUE(wait_for([&] {
        return read_all(log_path).find("producer done") != std::string::npos;
    })) << "collector never saw the producer's final message";
    Logger::instance().shutdown();

    const auto lines = read_lines(log_path);
    EXPECT_EQ(count_lines_containing(lines, "dynamic-ips"), 5u);
}

// A producer killed between reserve() and publish() must not stall the collector
// forever; after the timeout it abandons the slot and keeps going.
TEST_F(SharedMemoryTest, SkipsEntryStalledByADeadProducer) {
    const auto log_path = temp_log("test_shm_stalled.log");
    const auto segment = unique_segment_name("slt_st_");

    LogConfig config;
    config.mode = QueueMode::SharedCollector;
    config.shared_memory_name = segment;
    config.log_queue_size = 1024;
    config.string_buffer_size = 1 << 16;
    config.stalled_entry_timeout_ms = 200;
    config.sinks.push_back(std::make_shared<slick::logger::FileSink>(log_path));
    Logger::instance().init(config);

    // Wait for the startup banner so the collector's cursor is caught up.
    ASSERT_TRUE(wait_for([&] { return read_all(log_path).find("SlickLogger") != std::string::npos; }));

    {
        // Attach to the same segment and reserve a slot that is never published,
        // exactly what a producer crashing mid-write leaves behind.
        slick::queue<slick::logger::LogEntry, slick::logger::detail::logger_queue_traits> raw(segment.c_str());
        (void)raw.reserve();

        LOG_INFO("after the hole");

        EXPECT_TRUE(wait_for([&] {
            return read_all(log_path).find("after the hole") != std::string::npos;
        })) << "collector stayed stuck behind the unpublished slot";
    }

    Logger::instance().shutdown();
    EXPECT_NE(read_all(log_path).find("skipped an unpublished log entry"), std::string::npos);
}

// shutdown() promises to drain what is queued. A hole left by a dead producer
// must therefore be abandoned during the shutdown drain as well, not merely
// waited out, or every entry published behind it is lost.
TEST_F(SharedMemoryTest, ShutdownDrainsEntriesBehindAStalledSlot) {
    const auto log_path = temp_log("test_shm_drain_stalled.log");
    const auto segment = unique_segment_name("slt_ds_");

    LogConfig config;
    config.mode = QueueMode::SharedCollector;
    config.shared_memory_name = segment;
    config.log_queue_size = 1024;
    config.string_buffer_size = 1 << 16;
    // Generous relative to the microseconds the body below needs. If the running
    // loop skipped the hole first, the drain path would go untested and the test
    // would still pass, so the margin exists to keep it honest under CI jitter.
    config.stalled_entry_timeout_ms = 1000;
    config.sinks.push_back(std::make_shared<slick::logger::FileSink>(log_path));
    Logger::instance().init(config);

    ASSERT_TRUE(wait_for([&] { return read_all(log_path).find("SlickLogger") != std::string::npos; }));

    {
        // Punch a hole, then publish behind it and shut down immediately, well
        // inside the timeout so the running loop cannot skip it first.
        slick::queue<slick::logger::LogEntry, slick::logger::detail::logger_queue_traits> raw(segment.c_str());
        (void)raw.reserve();

        LOG_INFO("queued behind the hole");
        Logger::instance().shutdown();
    }

    EXPECT_NE(read_all(log_path).find("queued behind the hole"), std::string::npos)
        << "shutdown dropped entries published behind an unpublished slot:\n"
        << read_all(log_path);
}

// collect_backlog = false is the escape hatch for a restarting collector that
// must not re-emit what a previous instance already wrote.
TEST_F(SharedMemoryTest, CollectBacklogFalseSkipsEntriesAlreadyInTheRing) {
    const auto first_path = temp_log("test_shm_backlog_first.log");
    const auto second_path = temp_log("test_shm_backlog_second.log");
    const auto segment = unique_segment_name("slt_nb_");

    auto make_config = [&](const std::filesystem::path& path, bool collect_backlog) {
        LogConfig config;
        config.mode = QueueMode::SharedCollector;
        config.shared_memory_name = segment;
        config.log_queue_size = 1024;
        config.string_buffer_size = 1 << 16;
        config.collect_backlog = collect_backlog;
        config.sinks.push_back(std::make_shared<slick::logger::FileSink>(path));
        return config;
    };

    // A first collector creates the segment and logs something into it.
    Logger::instance().init(make_config(first_path, true));
    LOG_INFO("entry from the first collector");
    ASSERT_TRUE(wait_for([&] {
        return read_all(first_path).find("entry from the first collector") != std::string::npos;
    }));
    Logger::instance().shutdown();

    // A second collector attaches to the same segment with the backlog disabled.
    Logger::instance().reset();
    Logger::instance().init(make_config(second_path, false));
    LOG_INFO("entry from the second collector");
    ASSERT_TRUE(wait_for([&] {
        return read_all(second_path).find("entry from the second collector") != std::string::npos;
    }));
    Logger::instance().shutdown();

    const std::string second = read_all(second_path);
    EXPECT_EQ(second.find("entry from the first collector"), std::string::npos)
        << "collect_backlog = false still replayed the earlier entry:\n" << second;
}

// Strings are copied into a ring whose reservation size is a 16-bit field, so an
// over-long one must be truncated rather than overflowing the write cursor.
TEST_F(SharedMemoryTest, OverlongStringsAreTruncatedNotCorrupting) {
    const auto log_path = temp_log("test_shm_overlong.log");
    const auto segment = unique_segment_name("slt_ov_");

    LogConfig config;
    config.mode = QueueMode::SharedCollector;
    config.shared_memory_name = segment;
    config.log_queue_size = 1024;
    config.string_buffer_size = 1 << 20;
    config.sinks.push_back(std::make_shared<slick::logger::FileSink>(log_path));
    Logger::instance().init(config);

    // One byte past the 65534 cap, so the tail must be dropped.
    const std::string huge(70000, 'x');
    LOG_INFO("huge[{}]", huge);
    LOG_INFO("still alive after the huge entry");

    ASSERT_TRUE(wait_for([&] {
        return read_all(log_path).find("still alive after the huge entry") != std::string::npos;
    })) << "the queue did not survive an over-long string";
    Logger::instance().shutdown();

    const auto lines = read_lines(log_path);
    size_t huge_line_length = 0;
    for (const auto& line : lines) {
        if (line.find("huge[") != std::string::npos) {
            huge_line_length = line.size();
        }
    }
    ASSERT_GT(huge_line_length, 0u) << "the over-long entry was dropped entirely";
    // Truncated to the cap, not the original 70000 bytes.
    EXPECT_LT(huge_line_length, 70000u);
    EXPECT_GT(huge_line_length, 65000u);
}

// The inline tag field is 16 bytes including the NUL, so a longer tag is cut.
// The cut must land on a character boundary or the output becomes mojibake.
TEST_F(SharedMemoryTest, LongUtf8TagIsTruncatedOnACharacterBoundary) {
    const auto log_path = temp_log("test_shm_utf8_tag.log");
    const auto segment = unique_segment_name("slt_u8_");

    LogConfig config;
    config.mode = QueueMode::SharedCollector;
    config.shared_memory_name = segment;
    // Ten two-byte characters (U+00E9) = 20 bytes. Two-byte characters matter
    // here: the 15-byte cap is odd, so a byte-based cut lands mid-character and
    // leaves a dangling lead byte. A three-byte character would divide 15 evenly
    // and the naive cut would look correct by accident.
    config.process_tag = "\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9"
                         "\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9";
    config.log_queue_size = 1024;
    config.string_buffer_size = 1 << 16;
    config.sinks.push_back(std::make_shared<slick::logger::FileSink>(log_path));
    Logger::instance().init(config);

    LOG_INFO("tagged entry");
    ASSERT_TRUE(wait_for([&] { return read_all(log_path).find("tagged entry") != std::string::npos; }));
    Logger::instance().shutdown();

    // Pull the tag back out of "[<pid>:<tag>]".
    const std::string contents = read_all(log_path);
    const std::string needle = ":";
    const size_t stamp = contents.find("[" + std::to_string(SLICK_TEST_GETPID()) + ":");
    ASSERT_NE(stamp, std::string::npos) << contents;
    const size_t tag_begin = contents.find(':', stamp) + 1;
    const size_t tag_end = contents.find(']', tag_begin);
    ASSERT_NE(tag_end, std::string::npos);
    const std::string tag = contents.substr(tag_begin, tag_end - tag_begin);

    // Whole characters only: 14 bytes, i.e. seven complete two-byte characters,
    // rather than the 15 a byte-based cut would keep.
    EXPECT_EQ(tag.size(), 14u) << "expected a cut back to a character boundary, got " << tag.size();
    ASSERT_EQ(tag.size() % 2, 0u) << "tag was cut mid-character";
    for (size_t i = 0; i < tag.size(); i += 2) {
        EXPECT_EQ(tag.compare(i, 2, "\xC3\xA9"), 0) << "damaged character at byte " << i;
    }
}

TEST_F(SharedMemoryTest, ReportsModeAndSegmentName) {
    const auto log_path = temp_log("test_shm_accessors.log");
    const auto segment = unique_segment_name("slt_ac_");

    LogConfig config;
    config.mode = QueueMode::SharedCollector;
    config.shared_memory_name = segment;
    config.log_queue_size = 1024;
    config.string_buffer_size = 1 << 16;
    config.sinks.push_back(std::make_shared<slick::logger::FileSink>(log_path));
    Logger::instance().init(config);

    EXPECT_EQ(Logger::instance().mode(), QueueMode::SharedCollector);
    EXPECT_EQ(Logger::instance().shared_memory_name(), segment);

    // shutdown() returns the logger to the Local defaults.
    Logger::instance().shutdown();
    EXPECT_EQ(Logger::instance().mode(), QueueMode::Local);
    EXPECT_TRUE(Logger::instance().shared_memory_name().empty());
}

TEST_F(SharedMemoryTest, RejectsInvalidConfiguration) {
    LogConfig missing_name;
    missing_name.mode = QueueMode::SharedProducer;
    EXPECT_THROW(Logger::instance().init(missing_name), std::runtime_error);

    LogConfig bad_characters;
    bad_characters.mode = QueueMode::SharedProducer;
    bad_characters.shared_memory_name = "has/slash";
    EXPECT_THROW(Logger::instance().init(bad_characters), std::runtime_error);

    LogConfig too_long;
    too_long.mode = QueueMode::SharedProducer;
    too_long.shared_memory_name = std::string(25, 'a');
    EXPECT_THROW(Logger::instance().init(too_long), std::runtime_error);

    LogConfig producer_with_sink;
    producer_with_sink.mode = QueueMode::SharedProducer;
    producer_with_sink.shared_memory_name = unique_segment_name("slt_bad_");
    producer_with_sink.sinks.push_back(std::make_shared<slick::logger::FileSink>("test_shm_unused.log"));
    EXPECT_THROW(Logger::instance().init(producer_with_sink), std::runtime_error);
    std::error_code ec;
    std::filesystem::remove("test_shm_unused.log", ec);

    LogConfig collector_without_sink;
    collector_without_sink.mode = QueueMode::SharedCollector;
    collector_without_sink.shared_memory_name = unique_segment_name("slt_bad_");
    EXPECT_THROW(Logger::instance().init(collector_without_sink), std::runtime_error);
}

// Local mode must be untouched by the multi-process work: no pid field, and the
// zero-copy string-literal path is still in use.
TEST_F(SharedMemoryTest, LocalModeOutputIsUnchanged) {
    const auto log_path = temp_log("test_shm_local_mode.log");

    Logger::instance().add_file_sink(log_path);
    Logger::instance().init(size_t{1024});
    EXPECT_EQ(Logger::instance().mode(), QueueMode::Local);
    EXPECT_TRUE(Logger::instance().shared_memory_name().empty());

    LOG_INFO("local message {}", 1);
    Logger::instance().shutdown();

    const std::string own_pid = "[" + std::to_string(SLICK_TEST_GETPID()) + "]";
    const std::string contents = read_all(log_path);
    EXPECT_NE(contents.find("local message 1"), std::string::npos);
    EXPECT_EQ(contents.find(own_pid), std::string::npos) << "local mode should not stamp a pid";
}

// ---------------------------------------------------------------------------
// Statistics across a collector backlog replay
// ---------------------------------------------------------------------------

namespace {

/// Holds the collector inside write() so a backlog replay can be sampled while
/// it is still running rather than after it has finished.
class BacklogStallingSink : public slick::logger::ISink {
public:
    BacklogStallingSink() : ISink("backlog-stalling") {}
    void write(const slick::logger::LogEntry&) override {
        writes_.fetch_add(1, std::memory_order_release);
        const int ms = stall_ms_.load(std::memory_order_relaxed);
        if (ms > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        }
    }
    void flush() override {}
    void set_stall_ms(int ms) noexcept { stall_ms_.store(ms, std::memory_order_relaxed); }
    uint32_t writes() const noexcept { return writes_.load(std::memory_order_acquire); }

private:
    std::atomic<int> stall_ms_{0};
    std::atomic<uint32_t> writes_{0};
};

} // namespace

// Regression: a collector replaying a backlog rewinds its entry reader below the
// point it attached, but the string frontier was seeded AT that point. Every
// string those replayed entries own was reserved before the collector attached,
// so it sat below the frontier and read as free from the very first sample: a
// collector attaching to a segment stuffed with undrained entries reported an
// idle string ring for the whole replay - precisely when that ring is at its
// fullest. The floor is rewound with the reader now.
TEST_F(SharedMemoryTest, BacklogReplayReportsTheStringsItHasNotDrainedYet) {
    const auto log_path = temp_log("test_shm_backlog_stats.log");
    const auto segment = unique_segment_name("slt_bs_");
    constexpr int kProduced = 40;

    // A real producer process fills the segment first and then lingers, so the
    // segment is still there for the collector to attach to. Everything it logs
    // is published - and undrained - before the collector exists.
    const auto ready_marker = temp_log("test_shm_backlog_ready.marker");
    std::thread producer([&] {
        run_producer("--name " + segment + " --tag pre --count " + std::to_string(kProduced) +
                     " --queue-size 1024 --string-buffer-size 65536 --linger-ms 3000"
                     " --ready-file " + ready_marker.string());
    });
    // A sleep would only make the backlog PROBABLE: lose the race and the
    // collector creates the segment itself, there is no backlog to replay, and
    // the test silently stops testing anything. Wait for the producer to say it
    // has published instead.
    ASSERT_TRUE(wait_for([&] { return std::filesystem::exists(ready_marker); }))
        << "the producer never signalled that it had filled the segment";

    auto sink = std::make_shared<BacklogStallingSink>();
    sink->set_stall_ms(80); // keeps the replay running while we sample it

    LogConfig config;
    config.mode = QueueMode::SharedCollector;
    config.shared_memory_name = segment;
    config.log_queue_size = 1024;
    config.string_buffer_size = 1 << 16;
    config.enable_stats = true;
    config.stats_interval_ms = 20;
    config.stats_sample_interval_ms = 2;
    config.sinks.push_back(sink);
    ASSERT_TRUE(config.collect_backlog) << "this test needs the default";
    Logger::instance().init(config);

    ASSERT_TRUE(wait_for([&] { return sink->writes() >= 1; }))
        << "the collector never started replaying the backlog";

    // read() hands back the whole published backlog as one run, so read_index_
    // jumps to the end of it before the first sink write and the queue reads as
    // drained for the entire dispatch - which short-circuits the occupancy to a
    // valid zero whatever the frontier says. A few live entries behind the
    // replay keep the queue genuinely non-empty, so the frontier is what is
    // being measured here. They are logged before the sample, so the write
    // cursor is static by the time it is read.
    for (int i = 0; i < 3; ++i) {
        LOG_INFO("live entry {} behind the replay", i);
    }

    // Demand a sample published AFTER those entries: the first report can land
    // within a few ms of init(), long before any of this, and asserting on it
    // would be asserting on the state before the test set it up.
    const uint64_t logged_at = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    slick::logger::LogStats stats{};
    const bool sampled = wait_for([&] {
        stats = Logger::instance().stats_snapshot();
        return stats.timestamp_ns >= logged_at && stats.sample_count > 0;
    }, std::chrono::seconds(3));
    const uint32_t writes_at_sample = sink->writes();
    sink->set_stall_ms(0);

    ASSERT_TRUE(sampled) << "no statistics sample landed during the replay";
    ASSERT_LT(writes_at_sample, static_cast<uint32_t>(kProduced))
        << "the replay finished before it could be sampled";
    ASSERT_GT(stats.entry_queue_depth, 0u) << "the backlog should still be queued";
    ASSERT_GT(stats.string_bytes_written, 1000u)
        << "the producer should have left several KB of strings behind";
    ASSERT_LT(stats.string_bytes_written, stats.string_buffer_capacity)
        << "the backlog must fit in one lap, so the rewound floor lands at zero";
    ASSERT_TRUE(stats.string_pct_valid);

    // The producer stopped before the collector attached and the collector logs
    // nothing but its own banner, so the write cursor is static here. This
    // collector has consumed almost none of it, so essentially the whole ring is
    // in flight for it - the seeded-at-attach frontier called all of it free.
    EXPECT_EQ(stats.string_inflight_bytes, stats.string_bytes_written)
        << "the replayed backlog's strings were reported free from the first sample";

    Logger::instance().shutdown();
    producer.join();
}
