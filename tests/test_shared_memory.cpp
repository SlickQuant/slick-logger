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
#include <string>
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
        slick::SlickQueue<slick::logger::LogEntry> raw(segment.c_str());
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
    config.stalled_entry_timeout_ms = 200;
    config.sinks.push_back(std::make_shared<slick::logger::FileSink>(log_path));
    Logger::instance().init(config);

    ASSERT_TRUE(wait_for([&] { return read_all(log_path).find("SlickLogger") != std::string::npos; }));

    {
        // Punch a hole, then publish behind it and shut down immediately, well
        // inside the 200 ms timeout so the running loop cannot skip it first.
        slick::SlickQueue<slick::logger::LogEntry> raw(segment.c_str());
        (void)raw.reserve();

        LOG_INFO("queued behind the hole");
        Logger::instance().shutdown();
    }

    EXPECT_NE(read_all(log_path).find("queued behind the hole"), std::string::npos)
        << "shutdown dropped entries published behind an unpublished slot:\n"
        << read_all(log_path);
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
