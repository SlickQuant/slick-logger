// Multi-process logging tests.
//
// Covers the shared-memory queue modes: entries produced in one process (or one
// logger) carry ring-buffer offsets instead of addresses, and the collector
// resolves them against its own mapping before handing them to sinks. A
// producer's thread rings live in its own segment; see the last section.

#include <slick/logger.hpp>
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <new>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <vector>

#ifdef _WIN32
#  include <process.h>
#  define SLICK_TEST_GETPID _getpid
#else
#  include <unistd.h>
#  define SLICK_TEST_GETPID getpid
#endif

#include "run_process.hpp"

// Resolved at cmake configure time; the build system defines this macro.
#ifndef SHM_PRODUCER_EXE_PATH
#  error "SHM_PRODUCER_EXE_PATH must be defined by CMake"
#endif

using slick::logger::LogConfig;
using slick::logger::Logger;
using slick::logger::LogLevel;
using slick::logger::QueueMode;
using slick::logger::Logger;

// Allocation failure injection, inert until a test arms it. After the first
// array allocation of exactly array_bytes, that thread's next plain allocation
// throws bad_alloc if it is at most max_bytes. A larger one counts a miss instead.
namespace alloc_fault {
std::atomic<size_t> array_bytes{0}; // armed while non-zero
std::atomic<size_t> max_bytes{0};
std::atomic<int> failed{0};
std::atomic<int> missed{0};
thread_local bool fail_next = false;

void* allocate(size_t size) {
    if (void* p = std::malloc(size ? size : 1)) {
        return p;
    }
    throw std::bad_alloc();
}

/// Make attach_producer() fail when it lists a producer's @p rings for the
/// merge. That is the first small allocation after its ring_view array.
void fail_attach(size_t rings) {
    using slick::logger::detail::ring_view;
    static_assert(std::is_trivially_destructible_v<ring_view>, "new[] adds no cookie to the array size");
    failed = 0;
    missed = 0;
    max_bytes = rings * sizeof(ring_view*);
    array_bytes = rings * sizeof(ring_view);
}
} // namespace alloc_fault

void* operator new(size_t size) {
    if (alloc_fault::fail_next) [[unlikely]] {
        alloc_fault::fail_next = false;
        if (size <= alloc_fault::max_bytes.load(std::memory_order_relaxed)) {
            alloc_fault::failed.fetch_add(1, std::memory_order_relaxed);
            throw std::bad_alloc();
        }
        alloc_fault::missed.fetch_add(1, std::memory_order_relaxed);
    }
    return alloc_fault::allocate(size);
}

void* operator new[](size_t size) {
    size_t armed = alloc_fault::array_bytes.load(std::memory_order_relaxed);
    if (armed == size && armed != 0 && alloc_fault::array_bytes.compare_exchange_strong(armed, 0)) [[unlikely]] {
        alloc_fault::fail_next = true;
    }
    return alloc_fault::allocate(size);
}

// Replace every unaligned form. One left to the runtime (or ASan) would free
// memory with a call that doesn't match how it was allocated.
void* operator new(size_t size, const std::nothrow_t&) noexcept { return std::malloc(size ? size : 1); }
void* operator new[](size_t size, const std::nothrow_t&) noexcept { return std::malloc(size ? size : 1); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }
void operator delete[](void* p, size_t) noexcept { std::free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { std::free(p); }

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

void touch_file(const std::filesystem::path& path) {
    std::ofstream(path) << "go\n";
}

// Launch the producer executable and wait for it to exit, returning its exit code.
int run_producer(const std::string& args) {
    return run_process(SHM_PRODUCER_EXE_PATH, args);
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

// A collector's formats and file names arrive copied, at a new address each
// time: an EncodedFileSink must still render them, pid and tag included, as
// FileSink did.
TEST_F(SharedMemoryTest, EncodedSinkRendersAnotherProcessAsFileSinkDoes) {
    const auto log_path = temp_log("test_shm_encoded.log");
    const auto encoded_path = temp_log("test_shm_encoded.slog");
    const auto decoded_path = temp_log("test_shm_decoded.log");
    const auto segment = unique_segment_name("slt_enc_");

    LogConfig config;
    config.mode = QueueMode::SharedCollector;
    config.shared_memory_name = segment;
    config.process_tag = "collector";
    config.log_queue_size = 1024;
    config.string_buffer_size = 1 << 16;
    config.sinks.push_back(std::make_shared<slick::logger::FileSink>(log_path));
    config.sinks.push_back(std::make_shared<slick::logger::EncodedFileSink>(encoded_path));
    Logger::instance().init(config);
    ASSERT_EQ(run_producer("--name " + segment + " --tag prod --count 50 --queue-size 1024 --string-buffer-size 65536"),
              0);
    ASSERT_TRUE(wait_for([&] { return read_all(log_path).find("producer done") != std::string::npos; }));
    Logger::instance().shutdown();

    {
        slick::logger::EncodedLogReader reader(encoded_path);
        slick::logger::FileSink decoded(decoded_path);
        while (const slick::logger::LogEntry* entry = reader.next()) {
            decoded.write(*entry);
        }
        EXPECT_EQ(reader.error(), "");
    }
    const std::string expected = read_all(log_path);
    EXPECT_NE(expected.find(":prod]"), std::string::npos);
    EXPECT_EQ(read_all(decoded_path), expected);
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
// with it. On POSIX, slick-queue before 2.3.0 shm_unlink()ed a segment its owner
// created when the queue was destroyed, which frees the name while the
// collector's mapping stays valid: later producers would then create a different
// segment under the same name and their entries would never reach the
// still-attached collector.
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

#ifdef __linux__
// shutdown() must release the mapping of a segment this process created, yet
// leave the segment itself for anyone attached or yet to attach. Before
// slick-queue 2.3.0 a creator's destructor unlinked its segment, so the logger
// kept every queue it created alive until exit instead: each init()/shutdown()
// cycle in a shared role leaked one more mapping.
TEST_F(SharedMemoryTest, ShutdownUnmapsCreatedSegmentButKeepsItsName) {
    const auto first_path = temp_log("test_shm_release_first.log");
    const auto second_path = temp_log("test_shm_release_second.log");
    const auto segment = unique_segment_name("slt_rl_");

    // Matches the entry and string segments alike, "<segment>" and "<segment>_str".
    auto mapped = [&] {
        std::ifstream maps("/proc/self/maps");
        const std::string needle = "/dev/shm/" + segment;
        std::string line;
        while (std::getline(maps, line)) {
            if (line.find(needle) != std::string::npos) {
                return true;
            }
        }
        return false;
    };
    auto make_config = [&](const std::filesystem::path& path) {
        LogConfig config;
        config.mode = QueueMode::SharedCollector;
        config.shared_memory_name = segment;
        config.log_queue_size = 1024;
        config.string_buffer_size = 1 << 16;
        config.sinks.push_back(std::make_shared<slick::logger::FileSink>(path));
        return config;
    };

    // The first run creates both segments and logs into them.
    Logger::instance().init(make_config(first_path));
    ASSERT_TRUE(mapped());
    LOG_INFO("entry from the creating run");
    ASSERT_TRUE(wait_for([&] {
        return read_all(first_path).find("entry from the creating run") != std::string::npos;
    }));
    Logger::instance().shutdown();
    EXPECT_FALSE(mapped()) << "shutdown() kept the segments it created mapped";

    // The name outlived the mapping: a second run attaches to the same segment
    // and replays its backlog rather than creating an empty one.
    Logger::instance().reset();
    Logger::instance().init(make_config(second_path));
    const bool replayed = wait_for([&] {
        return read_all(second_path).find("entry from the creating run") != std::string::npos;
    });
    Logger::instance().shutdown();
    EXPECT_TRUE(replayed) << "the segment did not survive its creator's shutdown:\n" << read_all(second_path);

    slick::shm::shared_memory::remove(segment.c_str());
    slick::shm::shared_memory::remove((segment + "_str").c_str());
}
#endif

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
// point it attached, but the string frontier was seeded at that point. Every
// string those replayed entries own was reserved before the collector attached,
// so it sat below the frontier and read as free from the very first sample: a
// collector attaching to a segment stuffed with undrained entries reported an
// idle string ring for the whole replay, just when that ring is at its fullest.
// The floor is now rewound with the reader.
TEST_F(SharedMemoryTest, BacklogReplayReportsTheStringsItHasNotDrainedYet) {
    const auto log_path = temp_log("test_shm_backlog_stats.log");
    const auto segment = unique_segment_name("slt_bs_");
    constexpr int kProduced = 40;

    // A real producer process fills the segment first and then lingers, so the
    // segment is still there for the collector to attach to. Everything it logs
    // is published, and still undrained, before the collector exists. It logs
    // through the shared rings, since with thread rings its strings would stay
    // in its own segment.
    const auto ready_marker = temp_log("test_shm_backlog_ready.marker");
    std::thread producer([&] {
        run_producer("--name " + segment + " --tag pre --count " + std::to_string(kProduced) +
                     " --queue-size 1024 --string-buffer-size 65536 --linger-ms 3000 --thread-rings 0"
                     " --ready-file " + ready_marker.string());
    });
    // A sleep would only make the backlog probable: lose the race and the
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
    // drained for the entire dispatch, which short-circuits the occupancy to a
    // valid zero whatever the frontier says. A few live entries behind the
    // replay keep the queue non-empty, so the frontier is what is
    // being measured here. They are logged before the sample, so the write
    // cursor is static by the time it is read.
    for (int i = 0; i < 3; ++i) {
        LOG_INFO("live entry {} behind the replay", i);
    }

    // Demand a sample published after those entries: the first report can land
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
    // in flight for it, where the seeded-at-attach frontier called all of it free.
    EXPECT_EQ(stats.string_inflight_bytes, stats.string_bytes_written)
        << "the replayed backlog's strings were reported free from the first sample";

    Logger::instance().shutdown();
    producer.join();
}

// ---------------------------------------------------------------------------
// Thread rings in shared memory: a producer process's rings live in a segment
// of its own, found through the registry, "<name>.reg".
// ---------------------------------------------------------------------------

namespace {

using slick::logger::detail::shm_registry;
using slick::logger::detail::shm_registry_slot;

/// The "seq <tag> <thread> <i>" lines of one tag, in file order, per thread. A
/// line that does not parse lands under thread -1.
std::map<int, std::vector<int>> sequences(const std::vector<std::string>& lines, const std::string& tag) {
    std::map<int, std::vector<int>> out;
    const std::string needle = "seq " + tag + " ";
    for (const auto& line : lines) {
        const size_t at = line.find(needle);
        if (at == std::string::npos) {
            continue;
        }
        int thread = -1;
        int index = -1;
        if (std::sscanf(line.c_str() + at + needle.size(), "%d %d", &thread, &index) == 2) {
            out[thread].push_back(index);
        } else {
            out[-1].push_back(-1);
        }
    }
    return out;
}

/// Empty when @p values is 0, 1, 2, ...; otherwise where it stops being so.
std::string first_gap(const std::vector<int>& values) {
    for (size_t i = 0; i < values.size(); ++i) {
        if (values[i] != static_cast<int>(i)) {
            return "at " + std::to_string(i) + " found " + std::to_string(values[i]);
        }
    }
    return {};
}

/// A segment's registry, mapped into this process.
struct registry_mapping {
    slick::shm::shared_memory shm;
    shm_registry* registry;

    explicit registry_mapping(const std::string& segment)
        : shm((segment + ".reg").c_str(), slick::shm::open_existing),
          registry(static_cast<shm_registry*>(shm.data())) {}

    shm_registry_slot& slot(uint32_t index) { return registry->slots[index]; }
    uint32_t state(uint32_t index) { return shm_registry_slot::state_of(slot(index).control.load()); }
};

LogConfig collector_config(const std::string& segment, const std::filesystem::path& path) {
    LogConfig config;
    config.mode = QueueMode::SharedCollector;
    config.shared_memory_name = segment;
    config.process_tag = "collector";
    config.log_queue_size = 1024;
    config.string_buffer_size = 1 << 16;
    config.sinks.push_back(std::make_shared<slick::logger::FileSink>(path));
    return config;
}

uint32_t this_pid() {
    return static_cast<uint32_t>(SLICK_TEST_GETPID());
}

} // namespace

// Each producer thread logs through a ring of its own, and its entries reach the
// collector complete and in order without touching the shared entry queue.
TEST_F(SharedMemoryTest, ProducerThreadsLogThroughTheirOwnRings) {
    const auto log_path = temp_log("test_shm_rings.log");
    const auto segment = unique_segment_name("slt_tr_");
    constexpr int kThreads = 4;
    constexpr int kCount = 2000;

    Logger::instance().init(collector_config(segment, log_path));
    ASSERT_EQ(run_producer("--name " + segment + " --tag rings --threads " + std::to_string(kThreads) +
                           " --count " + std::to_string(kCount) + " --queue-size 1024 --string-buffer-size 65536"),
              0);
    ASSERT_TRUE(wait_for([&] { return read_all(log_path).find("producer done") != std::string::npos; }));
    {
        // Only the two banners and "producer done" could have taken the shared path.
        slick::queue<slick::logger::LogEntry, slick::logger::detail::logger_queue_traits> raw(segment.c_str());
        EXPECT_LT(raw.initial_reading_index(), 16u) << "the producer's threads logged through the shared queue";
    }
    Logger::instance().shutdown();

    const auto seq = sequences(read_lines(log_path), "rings");
    ASSERT_EQ(seq.size(), static_cast<size_t>(kThreads));
    for (const auto& [thread, values] : seq) {
        EXPECT_EQ(values.size(), static_cast<size_t>(kCount)) << "thread " << thread;
        EXPECT_EQ(first_gap(values), "") << "thread " << thread;
    }
}

// Several producer processes at once, each in its own registry slot.
TEST_F(SharedMemoryTest, ManyProducerProcessesEachKeepTheirOrder) {
    const auto log_path = temp_log("test_shm_many.log");
    const auto segment = unique_segment_name("slt_mp_");
    constexpr int kProducers = 6;
    constexpr int kCount = 500;

    Logger::instance().init(collector_config(segment, log_path));
    std::vector<std::thread> producers;
    std::vector<int> codes(kProducers, -1);
    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back([&, p] {
            codes[p] = run_producer("--name " + segment + " --tag p" + std::to_string(p) + " --threads 2 --count " +
                                    std::to_string(kCount) + " --queue-size 1024 --string-buffer-size 65536");
        });
    }
    for (auto& producer : producers) {
        producer.join();
    }
    for (int p = 0; p < kProducers; ++p) {
        ASSERT_EQ(codes[p], 0) << "producer " << p;
    }
    ASSERT_TRUE(wait_for([&] {
        return count_lines_containing(read_lines(log_path), "producer done") == static_cast<size_t>(kProducers);
    }));
    Logger::instance().shutdown();

    const auto lines = read_lines(log_path);
    for (int p = 0; p < kProducers; ++p) {
        const auto seq = sequences(lines, "p" + std::to_string(p));
        ASSERT_EQ(seq.size(), 2u) << "producer " << p;
        for (const auto& [thread, values] : seq) {
            EXPECT_EQ(values.size(), static_cast<size_t>(kCount)) << "producer " << p << " thread " << thread;
            EXPECT_EQ(first_gap(values), "") << "producer " << p << " thread " << thread;
        }
    }
}

// A ring too small to keep up spills into the shared queue. The collector reads
// rings only every 20 ms here, so a spilled entry is there long before the ring
// records ahead of it are read: it must wait for them. Paced, so the collector
// keeps up and the producer keeps going back to its ring between spills.
TEST_F(SharedMemoryTest, SpillsFromAFullRingStayBehindItsRecords) {
    const auto log_path = temp_log("test_shm_spill.log");
    const auto segment = unique_segment_name("slt_sp_");
    constexpr int kCount = 3000;

    LogConfig config = collector_config(segment, log_path);
    config.log_queue_size = 16384;
    config.string_buffer_size = 1 << 22;
    config.writer_poll_interval_us = 20000;
    Logger::instance().init(config);
    ASSERT_EQ(run_producer("--name " + segment + " --tag spill --threads 2 --count " + std::to_string(kCount) +
                           " --pace-us 30 --ring-size 4096 --queue-size 16384 --string-buffer-size 4194304"),
              0);
    ASSERT_TRUE(wait_for([&] { return read_all(log_path).find("producer done") != std::string::npos; }));
    {
        slick::queue<slick::logger::LogEntry, slick::logger::detail::logger_queue_traits> raw(segment.c_str());
        ASSERT_GT(raw.initial_reading_index(), 1000u) << "nothing spilled, so nothing was tested";
    }
    Logger::instance().shutdown();

    const auto seq = sequences(read_lines(log_path), "spill");
    ASSERT_EQ(seq.size(), 2u);
    for (const auto& [thread, values] : seq) {
        EXPECT_EQ(values.size(), static_cast<size_t>(kCount)) << "thread " << thread;
        EXPECT_EQ(first_gap(values), "") << "thread " << thread;
    }
}

// A producer killed while its threads log: the collector takes every record it
// published, none of them torn, then unlinks its segment and frees its slot.
TEST_F(SharedMemoryTest, RecoversFromAProducerKilledMidBurst) {
    const auto log_path = temp_log("test_shm_killed.log");
    const auto segment = unique_segment_name("slt_kl_");
    constexpr int kThreads = 3;
    constexpr int kCrashAfter = 3000;
    // Fits a thread's 1 MB ring (about 6,900 of these records). More would spill
    // into the shared queue, which overwrites its oldest entries when the
    // collector falls behind: a loss this test is not about.
    constexpr int kCount = 4000;

    Logger::instance().init(collector_config(segment, log_path));
    // --flush-first: the collector holds the segment before the burst, so it
    // outlives the producer on Windows too.
    const int rc = run_producer("--name " + segment + " --tag crash --flush-first --threads " +
                                std::to_string(kThreads) + " --count " + std::to_string(kCount) +
                                " --crash-after " + std::to_string(kCrashAfter) +
                                " --queue-size 1024 --string-buffer-size 65536");
    ASSERT_NE(rc, 0) << "the producer was meant to be killed";

    registry_mapping registry(segment);
    const std::string ring_segment = segment + ".r0";
    EXPECT_TRUE(wait_for([&] {
        return registry.state(0) == shm_registry_slot::kFree &&
               !slick::shm::shared_memory::exists(ring_segment.c_str());
    })) << "the dead producer's slot was not freed, state " << registry.state(0);
    Logger::instance().shutdown();

    const auto seq = sequences(read_lines(log_path), "crash");
    EXPECT_EQ(seq.count(-1), 0u) << "a torn record reached the sink";
    size_t total = 0;
    for (const auto& [thread, values] : seq) {
        total += values.size();
        EXPECT_EQ(first_gap(values), "") << "thread " << thread;
    }
    EXPECT_GE(total, static_cast<size_t>(kCrashAfter)) << "published records were lost";
}

// A dead producer's pid handed to a live process must not keep its slot alive:
// the start time tells them apart.
TEST_F(SharedMemoryTest, ReusedPidIsNotTakenForTheDeadProducer) {
    const auto log_path = temp_log("test_shm_pid_reuse.log");
    const auto segment = unique_segment_name("slt_pr_");
    const auto logged = temp_log("test_shm_pid_reuse.logged");
    const auto go = temp_log("test_shm_pid_reuse.go");

    // No collector yet: the producer logs, then dies without closing its slot.
    int rc = 0;
    std::thread producer([&] {
        rc = run_producer("--name " + segment + " --tag reuse --log pre 5 --touch " + logged.string() + " --wait " +
                          go.string() + " --crash --queue-size 1024 --string-buffer-size 65536");
    });
    ASSERT_TRUE(wait_for([&] { return std::filesystem::exists(logged); }));
    registry_mapping registry(segment);
    ASSERT_EQ(registry.state(0), shm_registry_slot::kActive);
    // On Windows a segment goes with its last handle: hold this one past the producer.
    slick::shm::shared_memory keep((segment + ".r0").c_str(), slick::shm::open_existing);
    touch_file(go);
    producer.join();
    ASSERT_NE(rc, 0);

    // This process now plays the one the OS handed the dead producer's pid to.
    ASSERT_EQ(registry.state(0), shm_registry_slot::kActive);
    registry.slot(0).pid.store(this_pid());

    Logger::instance().init(collector_config(segment, log_path));
    EXPECT_TRUE(wait_for([&] { return registry.state(0) == shm_registry_slot::kFree; }))
        << "a live process with the dead producer's pid kept its slot";
    Logger::instance().shutdown();
    EXPECT_EQ(count_lines_containing(read_lines(log_path), "step pre"), 5u);
}

namespace {

/// A collector restart against a producer that keeps running: phase a goes to
/// the first collector, b is logged with none attached, c goes to the second.
void run_collector_restart(bool collect_backlog, const std::filesystem::path& first_path,
                           const std::filesystem::path& second_path, const std::filesystem::path& marker_prefix) {
    const auto segment = unique_segment_name("slt_rs_");
    auto marker = [&](const char* name) { return marker_prefix.string() + name; };
    for (const char* name : {".a", ".b", ".logged_b", ".c"}) {
        std::error_code ec;
        std::filesystem::remove(marker(name), ec);
    }

    Logger::instance().init(collector_config(segment, first_path));
    int rc = -1;
    std::thread producer([&] {
        rc = run_producer("--name " + segment + " --tag rs --log a 5 --flush --touch " + marker(".a") + " --wait " +
                          marker(".b") + " --log b 5 --touch " + marker(".logged_b") + " --wait " + marker(".c") +
                          " --log c 5 --flush --queue-size 1024 --string-buffer-size 65536");
    });
    ASSERT_TRUE(wait_for([&] { return std::filesystem::exists(marker(".a")); }));
    Logger::instance().shutdown();
    Logger::instance().reset();

    touch_file(marker(".b"));
    ASSERT_TRUE(wait_for([&] { return std::filesystem::exists(marker(".logged_b")); }));
    LogConfig config = collector_config(segment, second_path);
    config.collect_backlog = collect_backlog;
    Logger::instance().init(config);
    touch_file(marker(".c"));
    producer.join();
    ASSERT_EQ(rc, 0);
    ASSERT_TRUE(wait_for([&] { return read_all(second_path).find("producer done") != std::string::npos; }));
    Logger::instance().shutdown();

    const auto first = read_lines(first_path);
    const auto second = read_lines(second_path);
    EXPECT_EQ(count_lines_containing(first, "step a"), 5u);
    EXPECT_EQ(count_lines_containing(second, "step a"), 0u) << "the second collector replayed what the first wrote";
    EXPECT_EQ(count_lines_containing(second, "step b"), collect_backlog ? 5u : 0u);
    EXPECT_EQ(count_lines_containing(second, "step c"), 5u);
    for (const char* name : {".a", ".b", ".logged_b", ".c"}) {
        std::error_code ec;
        std::filesystem::remove(marker(name), ec);
    }
}

} // namespace

TEST_F(SharedMemoryTest, RestartedCollectorReplaysOnlyWhatWasNotRead) {
    run_collector_restart(true, temp_log("test_shm_restart_a1.log"), temp_log("test_shm_restart_a2.log"),
                          "test_shm_restart_a");
}

TEST_F(SharedMemoryTest, RestartedCollectorWithoutBacklogSkipsUnreadRecords) {
    run_collector_restart(false, temp_log("test_shm_restart_b1.log"), temp_log("test_shm_restart_b2.log"),
                          "test_shm_restart_b");
}

// With every registry slot held by a live process, a producer logs through the
// shared rings instead, and says so.
TEST_F(SharedMemoryTest, FullRegistryFallsBackToTheSharedRings) {
    const auto log_path = temp_log("test_shm_full.log");
    const auto segment = unique_segment_name("slt_fr_");

    Logger::instance().init(collector_config(segment, log_path));
    registry_mapping registry(segment);
    const uint64_t start_time = slick::logger::detail::process_start_time(this_pid());
    for (uint32_t i = 0; i < shm_registry::kSlotCount; ++i) {
        registry.slot(i).pid.store(this_pid());
        registry.slot(i).start_time.store(start_time);
        registry.slot(i).control.store(shm_registry_slot::make(shm_registry_slot::kActive, 1));
    }
    const int rc = run_producer("--name " + segment + " --tag full --count 5 --queue-size 1024 --string-buffer-size 65536");
    const bool done = wait_for([&] { return read_all(log_path).find("producer done") != std::string::npos; });
    for (uint32_t i = 0; i < shm_registry::kSlotCount; ++i) {
        registry.slot(i).control.store(shm_registry_slot::make(shm_registry_slot::kFree, 1));
    }
    Logger::instance().shutdown();

    ASSERT_EQ(rc, 0);
    ASSERT_TRUE(done);
    const auto lines = read_lines(log_path);
    EXPECT_EQ(count_lines_containing(lines, "producer message"), 5u);
    EXPECT_EQ(count_lines_containing(lines, "no thread rings in shared memory"), 1u);
}

// Two collectors would both hand ring space back to the producers.
TEST_F(SharedMemoryTest, SecondCollectorIsRefused) {
    const auto log_path = temp_log("test_shm_first_collector.log");
    const auto other_path = temp_log("test_shm_second_collector.log");
    const auto segment = unique_segment_name("slt_2c_");

    Logger::instance().init(collector_config(segment, log_path));
    EXPECT_EQ(run_producer("--name " + segment + " --collector " + other_path.string()), 3)
        << "a second collector attached to the same registry";
    Logger::instance().shutdown();

    // Released on shutdown, so the next one gets in.
    EXPECT_EQ(run_producer("--name " + segment + " --collector " + other_path.string()), 0);
}

// A producer built with another layout cannot be read: it is skipped, and said so once.
TEST_F(SharedMemoryTest, ProducerWithAnotherLayoutIsRefusedWithOneWarning) {
    using slick::logger::detail::shm_ring_segment;
    const auto log_path = temp_log("test_shm_layout.log");
    const auto segment = unique_segment_name("slt_ly_");

    Logger::instance().init(collector_config(segment, log_path));
    registry_mapping registry(segment);
    // Slot 0 as a producer of another build would leave it.
    const std::string name = segment + ".r0";
    slick::shm::shared_memory foreign(name.c_str(), shm_ring_segment::bytes(1, 4096), slick::shm::create_only);
    auto* const header = static_cast<shm_ring_segment*>(foreign.data());
    header->magic = shm_ring_segment::kMagic;
    header->layout_version = shm_ring_segment::kLayoutVersion;
    header->generation = 1;
    header->ring_count = 1;
    header->ring_capacity = 4096;
    header->record_layout = shm_ring_segment::record_layout_of() + 1;
    registry.slot(0).pid.store(this_pid());
    registry.slot(0).start_time.store(slick::logger::detail::process_start_time(this_pid()));
    registry.slot(0).control.store(shm_registry_slot::make(shm_registry_slot::kActive, 1));
    registry.registry->changes.fetch_add(1);

    EXPECT_TRUE(wait_for([&] { return read_all(log_path).find("another slick-logger layout") != std::string::npos; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(300)); // a few more scans
    registry.slot(0).control.store(shm_registry_slot::make(shm_registry_slot::kFree, 1));
    Logger::instance().shutdown();
    EXPECT_EQ(count_lines_containing(read_lines(log_path), "another slick-logger layout"), 1u);
    slick::shm::shared_memory::remove(name.c_str());
}

namespace {

/// Holds the writer inside write() on a message containing "step gate", then on
/// one containing "step hold", until each is opened. Counts every write, and the
/// "step hold" ones.
class GateSink : public slick::logger::ISink {
public:
    GateSink() : ISink("gate") {}
    void write(const slick::logger::LogEntry& entry) override {
        const std::string message(formatted_message(entry).first);
        writes_.fetch_add(1, std::memory_order_release);
        if (message.find("step hold") != std::string::npos) {
            holds_.fetch_add(1, std::memory_order_release);
        }
        for (gate& g : gates_) {
            if (message.find(g.text) != std::string::npos) {
                g.reached.store(true, std::memory_order_release);
                while (!g.open.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
            }
        }
    }
    void flush() override {}
    void open_all() {
        for (gate& g : gates_) {
            g.open.store(true, std::memory_order_release);
        }
    }
    struct gate {
        const char* text;
        std::atomic<bool> reached{false};
        std::atomic<bool> open{false};
    };
    gate gates_[2] = {{"step gate"}, {"step hold"}};
    std::atomic<int> writes_{0};
    std::atomic<int> holds_{0};
};

LogConfig gated_collector_config(const std::string& segment, std::shared_ptr<GateSink> sink) {
    LogConfig config;
    config.mode = QueueMode::SharedCollector;
    config.shared_memory_name = segment;
    config.log_queue_size = 1024;
    config.string_buffer_size = 1 << 16;
    config.sinks.push_back(std::move(sink));
    return config;
}

} // namespace

// A producer that registers while the writer is stuck in a sink is not attached
// yet when the writer gets to a flush() request: its rings must still count. Its
// entries hold the writer too, so a flush() that ignored them returns early.
TEST_F(SharedMemoryTest, FlushWaitsForAProducerThatRegisteredWhileTheWriterWasBusy) {
    const auto segment = unique_segment_name("slt_fl_");
    const auto gate_logged = temp_log("test_shm_flush_gate.marker");
    const auto late_logged = temp_log("test_shm_flush_late.marker");
    const auto release = temp_log("test_shm_flush_release.marker");
    auto sink = std::make_shared<GateSink>();
    Logger::instance().init(gated_collector_config(segment, sink));

    const std::string tail = " --queue-size 1024 --string-buffer-size 65536 --touch ";
    std::thread first([&] {
        run_producer("--name " + segment + " --tag p1 --log gate 1" + tail + gate_logged.string() + " --wait " +
                     release.string());
    });
    const bool gated = wait_for([&] { return sink->gates_[0].reached.load(); });
    std::thread second([&] {
        run_producer("--name " + segment + " --tag p2 --log hold 5" + tail + late_logged.string() + " --wait " +
                     release.string());
    });
    const bool logged = wait_for([&] { return std::filesystem::exists(late_logged); });
    std::atomic<int> holds_at_return{-1};
    std::thread flusher([&] {
        Logger::instance().flush();
        holds_at_return.store(sink->holds_.load(std::memory_order_acquire), std::memory_order_release);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(200)); // the request is in
    sink->gates_[0].open.store(true, std::memory_order_release);
    std::this_thread::sleep_for(std::chrono::milliseconds(300)); // time to return early, if it would
    sink->open_all();
    flusher.join();
    touch_file(release);
    first.join();
    second.join();
    Logger::instance().shutdown();

    ASSERT_TRUE(gated && logged);
    EXPECT_EQ(holds_at_return.load(), 5) << "flush() returned before a newly registered producer's entries";
}

namespace {

/// The "[<pid>" part of the "[<pid>:<tag>]" stamp on each line holding
/// "seq <tag> ". A line without a stamp gives "none".
std::set<std::string> seq_stamps(const std::vector<std::string>& lines, const std::string& tag) {
    std::set<std::string> stamps;
    for (const auto& line : lines) {
        if (line.find("seq " + tag + " ") == std::string::npos) {
            continue;
        }
        const size_t end = line.find(":" + tag + "]");
        const size_t begin = end == std::string::npos ? end : line.rfind('[', end);
        stamps.insert(begin == std::string::npos ? std::string("none") : line.substr(begin, end - begin));
    }
    return stamps;
}

bool slot_is(registry_mapping& registry, uint32_t index, uint32_t state, uint32_t generation) {
    const uint64_t control = registry.slot(index).control.load();
    return shm_registry_slot::state_of(control) == state && shm_registry_slot::generation_of(control) == generation;
}

} // namespace

// Spills stuck behind a hole in the shared queue outlive their producer, and the
// next owner of its registry slot. They carry that producer's pid themselves.
TEST_F(SharedMemoryTest, SpillsKeepTheirPidAfterTheSlotIsReused) {
    const auto log_path = temp_log("test_shm_spill_pid.log");
    const auto segment = unique_segment_name("slt_sr_");
    LogConfig config = collector_config(segment, log_path);
    config.stalled_entry_timeout_ms = 8000;
    Logger::instance().init(config);
    ASSERT_TRUE(wait_for([&] { return read_all(log_path).find("SlickLogger") != std::string::npos; }));
    {
        slick::queue<slick::logger::LogEntry, slick::logger::detail::logger_queue_traits> raw(segment.c_str());
        (void)raw.reserve(); // a hole: everything queued behind it waits out the timeout
    }

    // Its tiny ring fills at once, so most of its entries spill behind the hole.
    ASSERT_EQ(run_producer("--name " + segment + " --tag p1 --threads 1 --count 100 --ring-size 4096"
                           " --queue-size 1024 --string-buffer-size 65536"),
              0);
    registry_mapping registry(segment);
    ASSERT_TRUE(wait_for([&] { return slot_is(registry, 0, shm_registry_slot::kFree, 1); }))
        << "the first producer was never detached";
    // A second producer takes the same slot and goes too.
    ASSERT_EQ(run_producer("--name " + segment + " --tag p2 --count 1 --queue-size 1024 --string-buffer-size 65536"),
              0);
    ASSERT_TRUE(wait_for([&] { return slot_is(registry, 0, shm_registry_slot::kFree, 2); }))
        << "the second producer did not reuse the slot";

    const bool spilled_out = wait_for([&] { return count_lines_containing(read_lines(log_path), "seq p1 ") == 100; },
                                      std::chrono::seconds(20));
    Logger::instance().shutdown();
    ASSERT_TRUE(spilled_out);

    // Its ring records and spills should all share one stamp, and that stamp has a pid.
    const auto stamps = seq_stamps(read_lines(log_path), "p1");
    EXPECT_EQ(stamps.size(), 1u) << "some of the producer's entries lost its pid";
    EXPECT_EQ(stamps.count("none"), 0u) << "they carry no pid";
}

// A collector replaying its backlog sees spills the previous collector already
// wrote, after their producer's slot went to another producer. They must still
// carry the first producer's pid.
TEST_F(SharedMemoryTest, ReplayedSpillsKeepTheirPid) {
    const auto first_log = temp_log("test_shm_replay_first.log");
    const auto log_path = temp_log("test_shm_replay_second.log");
    const auto segment = unique_segment_name("slt_rp_");

    Logger::instance().init(collector_config(segment, first_log));
    // Held open so the queues outlive both collectors on Windows too.
    slick::queue<slick::logger::LogEntry, slick::logger::detail::logger_queue_traits> raw(segment.c_str());
    slick::shm::shared_memory strings((segment + "_str").c_str(), slick::shm::open_existing);
    registry_mapping registry(segment);
    ASSERT_EQ(run_producer("--name " + segment + " --tag p1 --threads 1 --count 100 --ring-size 4096"
                           " --queue-size 1024 --string-buffer-size 65536"),
              0);
    ASSERT_TRUE(wait_for([&] { return count_lines_containing(read_lines(first_log), "seq p1 ") == 100; }));
    ASSERT_TRUE(wait_for([&] { return slot_is(registry, 0, shm_registry_slot::kFree, 1); }));
    ASSERT_EQ(run_producer("--name " + segment + " --tag p2 --count 1 --queue-size 1024 --string-buffer-size 65536"),
              0);
    ASSERT_TRUE(wait_for([&] { return slot_is(registry, 0, shm_registry_slot::kFree, 2); }))
        << "the second producer did not reuse the slot";
    Logger::instance().shutdown();
    Logger::instance().reset();
    ASSERT_GT(raw.initial_reading_index(), 50u) << "nothing spilled, so nothing would be replayed";

    Logger::instance().init(collector_config(segment, log_path)); // collect_backlog: replays the queue
    const bool replayed = wait_for([&] { return count_lines_containing(read_lines(log_path), "seq p1 ") > 0; });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    Logger::instance().shutdown();
    ASSERT_TRUE(replayed) << "the second collector replayed none of the spills";

    const auto expected = seq_stamps(read_lines(first_log), "p1");
    ASSERT_EQ(expected.size(), 1u);
    ASSERT_EQ(expected.count("none"), 0u);
    EXPECT_EQ(seq_stamps(read_lines(log_path), "p1"), expected) << "the replayed spills lost their producer's pid";
}

// As above, but the collector is killed before it reads the spills, and the
// next collector replays them.
TEST_F(SharedMemoryTest, SpillsKeepTheirPidAcrossACollectorCrash) {
    const auto first_log = temp_log("test_shm_crash_first.log");
    const auto log_path = temp_log("test_shm_crash_second.log");
    const auto go = temp_log("test_shm_crash_go.marker");
    const auto segment = unique_segment_name("slt_cc_");

    // The first collector is another process, so it can be killed.
    int first_rc = 0;
    std::thread first([&] {
        first_rc = run_producer("--name " + segment + " --collector " + first_log.string() +
                                " --stall-ms 60000 --queue-size 1024 --string-buffer-size 65536 --wait " + go.string() +
                                " --crash");
    });
    ASSERT_TRUE(wait_for([&] { return read_all(first_log).find("SlickLogger") != std::string::npos; }));
    // Held here so the segments outlive both collectors on Windows too.
    slick::queue<slick::logger::LogEntry, slick::logger::detail::logger_queue_traits> raw(segment.c_str());
    slick::shm::shared_memory strings((segment + "_str").c_str(), slick::shm::open_existing);
    registry_mapping registry(segment);
    (void)raw.reserve(); // a hole the first collector never gets past

    ASSERT_EQ(run_producer("--name " + segment + " --tag p1 --threads 1 --count 100 --ring-size 4096"
                           " --queue-size 1024 --string-buffer-size 65536"),
              0);
    // Retired by the first collector: free, or waiting for its spills.
    const bool draining = wait_for([&] {
        const uint32_t state = registry.state(0);
        return state != shm_registry_slot::kActive && state != shm_registry_slot::kClosing;
    });
    const int second_rc =
        run_producer("--name " + segment + " --tag p2 --count 1 --queue-size 1024 --string-buffer-size 65536");
    touch_file(go); // the first collector dies with the spills unread
    first.join();
    ASSERT_TRUE(draining && second_rc == 0);
    ASSERT_NE(first_rc, 0) << "the first collector was meant to be killed";

    LogConfig config = collector_config(segment, log_path);
    config.stalled_entry_timeout_ms = 300;
    Logger::instance().init(config); // replays the backlog, hole and spills
    const bool spilled_out = wait_for([&] { return count_lines_containing(read_lines(log_path), "seq p1 ") > 0; });
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    Logger::instance().shutdown();
    ASSERT_TRUE(spilled_out) << "the second collector replayed none of the spills";

    // p1's ring records went to the first collector with its pid, so that log has
    // the stamp to expect. A line with no pid prints no stamp, which reads as "none".
    const auto expected = seq_stamps(read_lines(first_log), "p1");
    ASSERT_EQ(expected.size(), 1u);
    ASSERT_EQ(expected.count("none"), 0u);
    EXPECT_EQ(seq_stamps(read_lines(log_path), "p1"), expected) << "the replayed spills lost their producer's pid";
}

// String accounting with a producer that registered while the writer was busy:
// its record's strings were reported free while the record still waited.
TEST_F(SharedMemoryTest, StringFrontierWaitsForAProducerNotAttachedYet) {
    const auto segment = unique_segment_name("slt_sf_");
    const auto gate_logged = temp_log("test_shm_frontier_gate.marker");
    const auto go_hold = temp_log("test_shm_frontier_go.marker");
    const auto hold_logged = temp_log("test_shm_frontier_hold.marker");
    const auto big_logged = temp_log("test_shm_frontier_big.marker");
    const auto release = temp_log("test_shm_frontier_release.marker");
    auto sink = std::make_shared<GateSink>();
    LogConfig config = gated_collector_config(segment, sink);
    config.enable_stats = true;
    config.stats_interval_ms = 30;
    config.stats_sample_interval_ms = 2;
    Logger::instance().init(config);

    const std::string sizes = " --queue-size 1024 --string-buffer-size 65536";
    // "hold" is logged after "gate" was taken, so it is not in that batch, and
    // before "big", so the writer reaches it first.
    std::thread first([&] {
        run_producer("--name " + segment + " --tag p1 --log gate 1 --touch " + gate_logged.string() + " --wait " +
                     go_hold.string() + " --log hold 1 --touch " + hold_logged.string() + " --wait " +
                     release.string() + sizes);
    });
    const bool gated = wait_for([&] { return sink->gates_[0].reached.load(); });
    touch_file(go_hold);
    const bool held = wait_for([&] { return std::filesystem::exists(hold_logged); });
    // Registers while the writer is stuck in the sink, so it is not attached yet.
    std::thread second([&] {
        run_producer("--name " + segment + " --tag p2 --ring-size 4096 --big 3000 --touch " + big_logged.string() +
                     " --wait " + release.string() + sizes);
    });
    const bool big = wait_for([&] { return std::filesystem::exists(big_logged); });
    sink->gates_[0].open.store(true, std::memory_order_release);
    const bool on_hold = wait_for([&] { return sink->gates_[1].reached.load(); }); // "big" still waits
    std::this_thread::sleep_for(std::chrono::milliseconds(150)); // a few reports
    const slick::logger::LogStats stats = Logger::instance().stats_snapshot();
    sink->open_all();
    touch_file(release);
    first.join();
    second.join();
    Logger::instance().shutdown();

    ASSERT_TRUE(gated && held && big && on_hold);
    EXPECT_GE(stats.string_inflight_bytes, 3000u)
        << "the pending record's string was reported free; written " << stats.string_bytes_written;
}

// A collector's ring totals are refreshed by its registry scans. Between two of
// them the totals can read "drained" while a new record waits, so they must not
// short-circuit the string ring gauge to zero.
TEST_F(SharedMemoryTest, StringGaugeDoesNotTrustRingTotalsBetweenScans) {
    const auto segment = unique_segment_name("slt_st_");
    const auto ready = temp_log("test_shm_totals_ready.marker");
    const auto hello = temp_log("test_shm_totals_hello.marker");
    const auto go_gate = temp_log("test_shm_totals_go_gate.marker");
    const auto go_big = temp_log("test_shm_totals_go_big.marker");
    const auto big_logged = temp_log("test_shm_totals_big.marker");
    const auto release = temp_log("test_shm_totals_release.marker");
    auto sink = std::make_shared<GateSink>();
    LogConfig config = gated_collector_config(segment, sink);
    config.enable_stats = true;
    config.stats_interval_ms = 30;
    config.stats_sample_interval_ms = 2;
    Logger::instance().init(config);

    const std::string sizes = " --queue-size 1024 --string-buffer-size 65536";
    std::thread big([&] {
        run_producer("--name " + segment + " --tag p1 --ring-size 4096 --log ready 1 --touch " + ready.string() +
                     " --wait " + go_big.string() + " --big 3000 --touch " + big_logged.string() + " --wait " +
                     release.string() + sizes);
    });
    std::thread gate([&] {
        run_producer("--name " + segment + " --tag p0 --log hello 1 --touch " + hello.string() + " --wait " +
                     go_gate.string() + " --log gate 1 --wait " + release.string() + sizes);
    });
    // Both banners and both first entries written, then a scan or two: the
    // totals now say every ring is drained.
    const bool started = wait_for([&] {
        return std::filesystem::exists(ready) && std::filesystem::exists(hello) &&
               sink->writes_.load(std::memory_order_acquire) >= 5;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    touch_file(go_gate);
    const bool gated = wait_for([&] { return sink->gates_[0].reached.load(); }); // the writer is stuck: no scans
    touch_file(go_big);
    const bool logged = wait_for([&] { return std::filesystem::exists(big_logged); });
    std::this_thread::sleep_for(std::chrono::milliseconds(150)); // a few reports
    const slick::logger::LogStats stats = Logger::instance().stats_snapshot();
    sink->open_all();
    touch_file(release);
    big.join();
    gate.join();
    Logger::instance().shutdown();

    ASSERT_TRUE(started && gated && logged);
    EXPECT_GE(stats.string_inflight_bytes, 3000u)
        << "the waiting record's string was reported free; written " << stats.string_bytes_written;
}

// The first producer, attached outside a registry scan (here by the frontier),
// left the collector's cached ring count at zero, and the string gauge read
// "nothing pending" while its big record waited.
TEST_F(SharedMemoryTest, StringGaugeCountsTheFirstProducerAttachedOutsideAScan) {
    const auto segment = unique_segment_name("slt_fp_");
    const auto logged = temp_log("test_shm_first_logged.marker");
    const auto release = temp_log("test_shm_first_release.marker");
    auto sink = std::make_shared<GateSink>();
    LogConfig config = gated_collector_config(segment, sink);
    config.enable_stats = true;
    config.stats_interval_ms = 30;
    config.stats_sample_interval_ms = 2;
    Logger::instance().init(config);
    ASSERT_TRUE(wait_for([&] { return sink->writes_.load(std::memory_order_acquire) >= 1; })); // the banner

    LOG_INFO("step gate"); // the writer is stuck on this, so it scans nothing
    const bool gated = wait_for([&] { return sink->gates_[0].reached.load(); });
    std::thread producer([&] {
        run_producer("--name " + segment + " --tag p1 --ring-size 4096 --log hold 1 --big 3000 --touch " +
                     logged.string() + " --wait " + release.string() + " --queue-size 1024 --string-buffer-size 65536");
    });
    const bool big = wait_for([&] { return std::filesystem::exists(logged); });
    sink->gates_[0].open.store(true, std::memory_order_release);
    const bool on_hold = wait_for([&] { return sink->gates_[1].reached.load(); }); // "big" still waits
    std::this_thread::sleep_for(std::chrono::milliseconds(150)); // a few reports
    const slick::logger::LogStats stats = Logger::instance().stats_snapshot();
    sink->open_all();
    touch_file(release);
    producer.join();
    Logger::instance().shutdown();

    ASSERT_TRUE(gated && big && on_hold);
    EXPECT_GE(stats.string_inflight_bytes, 3000u)
        << "the waiting record's string was reported free; written " << stats.string_bytes_written;
}

// attach_producer() takes all of a producer's rings or none. Here the list of
// rings the merge reads fails to grow once. A producer committed before that
// would stay half attached, never read and never retried.
TEST_F(SharedMemoryTest, AttachIsRetriedAfterAnAllocationFailure) {
    const auto log_path = temp_log("test_shm_attach_oom.log");
    const auto segment = unique_segment_name("slt_ao_");
    const auto release = temp_log("test_shm_attach_oom_release.marker");
    constexpr size_t kRings = 3; // an array size nothing else in the collector allocates
    constexpr size_t kCount = 20;

    Logger::instance().init(collector_config(segment, log_path));
    alloc_fault::fail_attach(kRings);
    std::thread producer([&] {
        run_producer("--name " + segment + " --tag oom --rings " + std::to_string(kRings) + " --log attach " +
                     std::to_string(kCount) + " --wait " + release.string() +
                     " --queue-size 1024 --string-buffer-size 65536");
    });
    const bool drained =
        wait_for([&] { return count_lines_containing(read_lines(log_path), "step attach ") == kCount; });
    touch_file(release);
    producer.join();
    Logger::instance().shutdown();
    alloc_fault::array_bytes = 0;

    EXPECT_EQ(alloc_fault::failed.load(), 1) << "missed " << alloc_fault::missed.load();
    EXPECT_TRUE(drained) << "the producer's rings were never read";
}

// The collector frees a dead producer's slot and segment only when there is
// nothing to read. An attach that fails for now (out of memory here) keeps
// both, records and all, for the next scan.
TEST_F(SharedMemoryTest, DeadProducersRecordsOutliveAFailedAttach) {
    const auto log_path = temp_log("test_shm_attach_dead.log");
    const auto segment = unique_segment_name("slt_ad_");
    const auto logged = temp_log("test_shm_attach_dead.logged");
    const auto go = temp_log("test_shm_attach_dead.go");
    constexpr size_t kRings = 3;
    constexpr size_t kCount = 20;

    // No collector yet: the producer logs, then dies without closing its slot.
    int rc = 0;
    std::thread producer([&] {
        rc = run_producer("--name " + segment + " --tag dead --rings " + std::to_string(kRings) + " --log dead " +
                          std::to_string(kCount) + " --touch " + logged.string() + " --wait " + go.string() +
                          " --crash --queue-size 1024 --string-buffer-size 65536");
    });
    ASSERT_TRUE(wait_for([&] { return std::filesystem::exists(logged); }));
    // On Windows a segment is destroyed with its last handle, so hold these open
    // past the producer's exit.
    registry_mapping registry(segment);
    slick::shm::shared_memory keep((segment + ".r0").c_str(), slick::shm::open_existing);
    touch_file(go);
    producer.join();
    ASSERT_NE(rc, 0);
    ASSERT_EQ(registry.state(0), shm_registry_slot::kActive);

    alloc_fault::fail_attach(kRings); // fails the collector's first attach, inside init()
    Logger::instance().init(collector_config(segment, log_path));
    const bool drained =
        wait_for([&] { return count_lines_containing(read_lines(log_path), "step dead ") == kCount; });
    Logger::instance().shutdown();
    alloc_fault::array_bytes = 0;

    EXPECT_EQ(alloc_fault::failed.load(), 1) << "missed " << alloc_fault::missed.load();
    EXPECT_TRUE(drained) << "the dead producer's segment was freed with its records unread";
}
