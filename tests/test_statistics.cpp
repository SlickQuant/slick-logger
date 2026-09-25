// Recorded before the header supplies its defaults: whether this build left the
// queue switches alone. The CMake options pass them to every target, so a build
// configured with them flipped must still compile this file, and only a default
// build can pin the defaults.
#if !defined(SLICK_LOGGER_ENABLE_LOSS_DETECTION) && !defined(SLICK_LOGGER_ENABLE_CPU_RELAX)
#define STATS_TEST_QUEUE_DEFAULTS 1
#endif

#include <slick/logger.hpp>
#include <gtest/gtest.h>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <string>
#include <atomic>
#include <thread>
#include <vector>

using namespace slick::logger;

namespace {

constexpr const char* kLogFile = "test_stats.log";
constexpr const char* kCsvFile = "test_stats.csv";
constexpr const char* kCsvBackup = "test_stats.1.csv";
constexpr const char* kCsvBlocked = "test_stats_blocked.csv";

/// The exact column list write_stats_header() emits. Duplicated here on purpose:
/// the CSV is a published format, so a change to it should break a test.
constexpr const char* kExpectedHeader =
    "timestamp,interval_sec,sample_count,"
    "entries_produced,entries_consumed,produced_per_sec,consumed_per_sec,"
    "entry_queue_depth,entry_queue_depth_max,entry_queue_depth_mean,"
    "entry_queue_capacity,entry_queue_pct,entry_queue_pct_max,"
    "string_inflight_bytes,string_inflight_bytes_max,string_buffer_capacity,"
    "string_pct,string_pct_max,string_pct_valid,"
    "string_bytes_written,string_bytes_per_sec,string_turnover_pct,"
    "string_wraps_per_sec,entry_loss_count,string_loss_count";

LogConfig make_config(uint32_t interval_ms, uint32_t sample_ms, bool with_csv = true) {
    LogConfig config;
    config.sinks.push_back(std::make_shared<FileSink>(kLogFile));
    config.log_queue_size = 1024;
    config.string_buffer_size = 4096;
    config.enable_stats = true;
    config.stats_interval_ms = interval_ms;
    config.stats_sample_interval_ms = sample_ms;
    if (with_csv) {
        config.stats_file = kCsvFile;
    }
    return config;
}

/// Wait until the statistics thread has published a real sample, or give up.
/// Polling a deadline rather than sleeping a fixed span keeps the test from
/// flaking on a loaded CI machine.
LogStats wait_for_sample(uint32_t min_samples = 1, int timeout_ms = 5000) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    LogStats stats{};
    while (std::chrono::steady_clock::now() < deadline) {
        stats = Logger::instance().stats_snapshot();
        if (stats.sample_count >= min_samples && stats.timestamp_ns != 0) {
            return stats;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return stats;
}

/// Nanoseconds on the same clock LogStats::timestamp_ns uses, so a test can
/// demand a sample published *after* a point in time rather than whatever stale
/// one happens to be current.
uint64_t now_ns() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

/// Wait for a sample published at or after @p after_ns. Waiting for "any sample"
/// is not enough after an action like flush(): the current snapshot may predate
/// it entirely, so the assertion would run against state from before the action.
LogStats wait_for_sample_after(uint64_t after_ns, int timeout_ms = 5000) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    LogStats stats{};
    while (std::chrono::steady_clock::now() < deadline) {
        stats = Logger::instance().stats_snapshot();
        if (stats.timestamp_ns >= after_ns) {
            return stats;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return stats;
}

std::vector<std::string> read_lines(const char* path) {
    std::vector<std::string> lines;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        if (line.size() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.size()) {
            lines.push_back(line);
        }
    }
    return lines;
}

size_t count_fields(const std::string& line) {
    return static_cast<size_t>(std::count(line.begin(), line.end(), ',')) + 1;
}

} // namespace

class SlickLoggerStatsTest : public ::testing::Test {
protected:
    void TearDown() override {
        Logger::instance().shutdown();
        Logger::instance().reset();
        std::filesystem::remove(kLogFile);
        std::filesystem::remove(kCsvFile);
        std::filesystem::remove(kCsvBackup);
        std::filesystem::remove_all(kCsvBlocked);
    }
};

// ---------------------------------------------------------------- snapshot API

TEST_F(SlickLoggerStatsTest, StatsDisabledByDefault) {
    LogConfig config;
    config.sinks.push_back(std::make_shared<FileSink>(kLogFile));
    ASSERT_FALSE(config.enable_stats);
    Logger::instance().init(config);

    LOG_INFO("no statistics here");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    const auto stats = Logger::instance().stats_snapshot();
    EXPECT_EQ(stats.timestamp_ns, 0u);
    EXPECT_EQ(stats.sample_count, 0u);
    EXPECT_EQ(stats.entries_produced, 0u);
    EXPECT_FALSE(std::filesystem::exists(kCsvFile));
}

TEST_F(SlickLoggerStatsTest, SnapshotReportsCapacities) {
    Logger::instance().init(make_config(50, 5));

    const auto stats = wait_for_sample();
    // Both sizes are already powers of two, so they survive round_up_to_power_of_2.
    EXPECT_EQ(stats.entry_queue_capacity, 1024u);
    EXPECT_EQ(stats.string_buffer_capacity, 4096u);
}

TEST_F(SlickLoggerStatsTest, SnapshotTracksProducedEntries) {
    Logger::instance().init(make_config(50, 5));

    constexpr int kCount = 200;
    for (int i = 0; i < kCount; ++i) {
        LOG_INFO("entry {}", i);
    }
    Logger::instance().flush();
    const uint64_t logged_at = now_ns();

    // Must be a sample published after the logging; an older one legitimately
    // shows a smaller count.
    const auto stats = wait_for_sample_after(logged_at);
    EXPECT_GE(stats.entries_produced, static_cast<uint64_t>(kCount));
}

TEST_F(SlickLoggerStatsTest, DepthReturnsToZeroAfterFlush) {
    Logger::instance().init(make_config(50, 5));

    for (int i = 0; i < 100; ++i) {
        LOG_INFO("entry {}", i);
    }
    Logger::instance().flush();

    // flush() guarantees the writer drained everything queued before it, so a
    // sample taken afterwards must see the reader caught up.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    LogStats stats{};
    while (std::chrono::steady_clock::now() < deadline) {
        stats = Logger::instance().stats_snapshot();
        if (stats.sample_count != 0 && stats.entry_queue_depth == 0) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    EXPECT_EQ(stats.entry_queue_depth, 0u);
}

TEST_F(SlickLoggerStatsTest, FirstSampleHasZeroRates) {
    Logger::instance().init(make_config(50, 5));

    const auto stats = wait_for_sample();
    // The very first report has no predecessor to take a delta against; later ones
    // do, so assert the invariant that holds for both.
    if (stats.interval_sec == 0.0) {
        EXPECT_DOUBLE_EQ(stats.produced_per_sec, 0.0);
        EXPECT_DOUBLE_EQ(stats.consumed_per_sec, 0.0);
        EXPECT_DOUBLE_EQ(stats.string_bytes_per_sec, 0.0);
    } else {
        EXPECT_GT(stats.interval_sec, 0.0);
    }
}

TEST_F(SlickLoggerStatsTest, ThroughputIsPositiveUnderLoad) {
    Logger::instance().init(make_config(50, 5));

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    LogStats stats{};
    while (std::chrono::steady_clock::now() < deadline) {
        for (int i = 0; i < 200; ++i) {
            LOG_INFO("load {}", i);
        }
        stats = Logger::instance().stats_snapshot();
        if (stats.produced_per_sec > 0.0) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    EXPECT_GT(stats.produced_per_sec, 0.0);
}

TEST_F(SlickLoggerStatsTest, StringRingMetricsAdvance) {
    Logger::instance().init(make_config(50, 5));

    const std::string payload(64, 'x');
    for (int i = 0; i < 50; ++i) {
        LOG_INFO("dynamic {}", payload);
    }
    Logger::instance().flush();
    const uint64_t logged_at = now_ns();

    const auto stats = wait_for_sample_after(logged_at);
    EXPECT_GT(stats.string_bytes_written, 0u);
}

TEST_F(SlickLoggerStatsTest, LossCountIsReadableAndZeroByDefault) {
    Logger::instance().init(make_config(50, 5));
    LOG_INFO("hello");
    Logger::instance().flush();

    const auto stats = wait_for_sample();
    // SLICK_LOGGER_ENABLE_LOSS_DETECTION defaults to off, so loss_count() is a
    // compile-time zero. The string ring reads zero regardless: the logger never
    // calls read() on it. test_queue_traits.cpp covers the counting build; a build
    // that turns counting on still reads zero here, since nothing was overrun.
#ifdef STATS_TEST_QUEUE_DEFAULTS
    static_assert(!kLossDetectionEnabled, "loss detection must stay opt-in");
    static_assert(detail::logger_queue_traits::enable_cpu_relax, "the CAS backoff must stay on by default");
#endif
    EXPECT_EQ(stats.entry_loss_count, 0u);
    EXPECT_EQ(stats.string_loss_count, 0u);
}

TEST_F(SlickLoggerStatsTest, ResetClearsStats) {
    Logger::instance().init(make_config(50, 5));
    for (int i = 0; i < 50; ++i) {
        LOG_INFO("entry {}", i);
    }
    ASSERT_GT(wait_for_sample().timestamp_ns, 0u);

    Logger::instance().shutdown();
    Logger::instance().reset();

    const auto stats = Logger::instance().stats_snapshot();
    EXPECT_EQ(stats.timestamp_ns, 0u);
    EXPECT_EQ(stats.entries_produced, 0u);
    EXPECT_EQ(stats.sample_count, 0u);
    EXPECT_FALSE(stats.string_pct_valid);
}

TEST_F(SlickLoggerStatsTest, ShutdownClearsStats) {
    // Regression: shutdown() switched statistics off but left the last published
    // snapshot in place, so stats_snapshot() went on serving a finished run's
    // numbers for a logger that was no longer collecting any. Only reset() cleared
    // it, and nothing obliges a caller to reach for reset() after shutdown().
    Logger::instance().init(make_config(50, 5));
    for (int i = 0; i < 50; ++i) {
        LOG_INFO("entry {}", i);
    }
    ASSERT_GT(wait_for_sample().timestamp_ns, 0u);

    Logger::instance().shutdown(); // deliberately no reset()

    const auto stats = Logger::instance().stats_snapshot();
    EXPECT_EQ(stats.timestamp_ns, 0u);
    EXPECT_EQ(stats.entries_produced, 0u);
    EXPECT_EQ(stats.sample_count, 0u);
    EXPECT_FALSE(stats.string_pct_valid);
}

TEST_F(SlickLoggerStatsTest, ReinitWithStatsDisabledClearsStats) {
    // Same leak reached through init(), which shuts down internally: the stale
    // snapshot used to survive into a run with statistics switched off, where
    // stats_snapshot() is documented to read as a zeroed LogStats.
    Logger::instance().init(make_config(50, 5));
    for (int i = 0; i < 50; ++i) {
        LOG_INFO("entry {}", i);
    }
    ASSERT_GT(wait_for_sample().timestamp_ns, 0u);

    LogConfig plain;
    plain.sinks.push_back(std::make_shared<FileSink>(kLogFile));
    ASSERT_FALSE(plain.enable_stats);
    Logger::instance().init(plain);

    LOG_INFO("no statistics here");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    const auto stats = Logger::instance().stats_snapshot();
    EXPECT_EQ(stats.timestamp_ns, 0u);
    EXPECT_EQ(stats.entries_produced, 0u);
    EXPECT_EQ(stats.sample_count, 0u);
}

TEST_F(SlickLoggerStatsTest, FailedStatsInitAfterRunClearsStats) {
    // And through a failed init(): shutdown() has already stopped the previous run
    // by the time open_stats_csv() throws, so the caller is left with a logger that
    // collects nothing while still reporting the numbers of the run before it.
    Logger::instance().init(make_config(50, 5));
    for (int i = 0; i < 50; ++i) {
        LOG_INFO("entry {}", i);
    }
    ASSERT_GT(wait_for_sample().timestamp_ns, 0u);

    std::filesystem::create_directory(kCsvBlocked); // a directory cannot be opened as a file
    LogConfig bad = make_config(50, 5);
    bad.stats_file = kCsvBlocked;
    EXPECT_THROW(Logger::instance().init(bad), std::runtime_error);

    const auto stats = Logger::instance().stats_snapshot();
    EXPECT_EQ(stats.timestamp_ns, 0u);
    EXPECT_EQ(stats.entries_produced, 0u);
    EXPECT_EQ(stats.sample_count, 0u);
}

TEST_F(SlickLoggerStatsTest, SnapshotsWorkWithoutCsvFile) {
    Logger::instance().init(make_config(50, 5, false));

    for (int i = 0; i < 50; ++i) {
        LOG_INFO("entry {}", i);
    }
    const auto stats = wait_for_sample();

    EXPECT_GT(stats.timestamp_ns, 0u);
    EXPECT_GT(stats.entries_produced, 0u);
    EXPECT_FALSE(std::filesystem::exists(kCsvFile));
}

TEST_F(SlickLoggerStatsTest, ShutdownJoinsStatsThreadPromptly) {
    // A 5 s reporting interval with a 10 ms tick: shutdown is bounded by the tick,
    // not by the interval.
    Logger::instance().init(make_config(5000, 10));
    LOG_INFO("hello");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    const auto start = std::chrono::steady_clock::now();
    Logger::instance().shutdown();
    const auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 2000);
}

// ------------------------------------------------------------- gauge sampling

TEST_F(SlickLoggerStatsTest, BurstPeakIsCapturedBetweenReports) {
    // The test that justifies oversampling: a burst that fills the queue and
    // drains again between two reports is invisible to a single point sample, so
    // the peak has to come from the fast tick.
    Logger::instance().init(make_config(500, 5));

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    LogStats stats{};
    while (std::chrono::steady_clock::now() < deadline) {
        for (int i = 0; i < 500; ++i) {
            LOG_INFO("burst {}", i);
        }
        Logger::instance().flush(); // drain well before the report boundary
        std::this_thread::sleep_for(std::chrono::milliseconds(20));

        stats = Logger::instance().stats_snapshot();
        if (stats.entry_queue_depth_max > 0) {
            break;
        }
    }
    EXPECT_GT(stats.entry_queue_depth_max, 0u);
    EXPECT_GT(stats.entry_queue_pct_max, 0.0);
    EXPECT_GE(stats.entry_queue_depth_max, stats.entry_queue_depth);
}

TEST_F(SlickLoggerStatsTest, SampleCountReflectsOversampling) {
    Logger::instance().init(make_config(200, 5));

    // 200 ms of 5 ms ticks is ~40 samples; assert only a generous lower bound,
    // because a loaded CI machine will not hit the nominal rate.
    const auto stats = wait_for_sample(5, 10000);
    EXPECT_GE(stats.sample_count, 5u);
}

TEST_F(SlickLoggerStatsTest, SampleIntervalClampedToReportInterval) {
    // A tick slower than the reporting interval would starve the reports; it is
    // clamped instead, so samples still arrive.
    LogConfig config = make_config(50, 5);
    config.stats_sample_interval_ms = 5000;
    Logger::instance().init(config);

    const auto stats = wait_for_sample(1, 3000);
    EXPECT_GT(stats.timestamp_ns, 0u);
    EXPECT_GE(stats.sample_count, 1u);
}

// ----------------------------------------------------------- string occupancy

TEST_F(SlickLoggerStatsTest, StringOccupancyFoundFromDynamicArgs) {
    Logger::instance().init(make_config(50, 5));

    const std::string payload(100, 'y');
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    LogStats stats{};
    while (std::chrono::steady_clock::now() < deadline) {
        // No pause between bursts: the report is taken on the statistics thread at
        // a moment this test does not control, so the ring has to be kept occupied
        // rather than refilled in bursts the writer can drain in the gaps.
        for (int i = 0; i < 200; ++i) {
            LOG_INFO("dynamic {}", payload);
        }
        stats = Logger::instance().stats_snapshot();
        // Regression (CI flake): breaking on string_pct_valid alone was not waiting
        // for the state this test is about. A drained ring reports a *valid* zero -
        // see StringOccupancyIsValidZeroWhenDrained - so the loop would exit on a
        // report taken between bursts and hand the assertion below the zero it is
        // asserting against. Wait for the occupancy itself.
        if (stats.string_pct_valid && stats.string_inflight_bytes > 0) {
            break;
        }
    }
    EXPECT_TRUE(stats.string_pct_valid);
    EXPECT_GT(stats.string_inflight_bytes, 0u);
}

TEST_F(SlickLoggerStatsTest, StringOccupancyNeverExceedsCapacity) {
    Logger::instance().init(make_config(50, 5));

    // Deliberately overrun the 4 KB string ring many times over.
    const std::string payload(200, 'z');
    for (int i = 0; i < 2000; ++i) {
        LOG_INFO("overflow {}", payload);
    }

    const auto stats = wait_for_sample();
    EXPECT_LE(stats.string_pct, 100.0);
    EXPECT_LE(stats.string_pct_max, 100.0);
    EXPECT_LE(stats.string_inflight_bytes,
              static_cast<uint64_t>(stats.string_buffer_capacity));
    EXPECT_LE(stats.entry_queue_pct, 100.0);
    EXPECT_LE(stats.entry_queue_pct_max, 100.0);
}

TEST_F(SlickLoggerStatsTest, StringOccupancyIsValidZeroWhenDrained) {
    Logger::instance().init(make_config(50, 5));

    for (int i = 0; i < 100; ++i) {
        LOG_INFO("count {}", i);
    }
    Logger::instance().flush();
    const uint64_t drained_at = now_ns();

    // A drained queue means nothing in the string ring is still needed. That is a
    // real measurement of zero, not an absent one, so it must be reported valid -
    // otherwise the healthiest state the pipeline has would read as "unknown".
    //
    // The sample has to be one taken after the drain: any older one may still
    // describe a queue with entries outstanding, which is a different state and
    // makes this assertion flaky rather than wrong. Nothing logs after the flush -
    // the statistics thread never calls log() - so the queue stays drained.
    const LogStats stats = wait_for_sample_after(drained_at);
    ASSERT_GE(stats.timestamp_ns, drained_at);
    ASSERT_EQ(stats.entry_queue_depth, 0u);
    EXPECT_TRUE(stats.string_pct_valid);
    EXPECT_EQ(stats.string_inflight_bytes, 0u);
    EXPECT_DOUBLE_EQ(stats.string_pct, 0.0);
}

TEST_F(SlickLoggerStatsTest, StringOccupancyPeakSurvivesDrain) {
    // The string ring is a gauge like the entry queue, so its peak has to come
    // from the oversampling tick: by report time the burst has usually drained.
    Logger::instance().init(make_config(200, 5));

    const std::string payload(100, 'q');
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    LogStats stats{};
    while (std::chrono::steady_clock::now() < deadline) {
        for (int i = 0; i < 1000; ++i) {
            LOG_INFO("burst {}", payload);
        }
        Logger::instance().flush();
        stats = Logger::instance().stats_snapshot();
        if (stats.string_inflight_bytes_max > 0) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    EXPECT_GT(stats.string_inflight_bytes_max, 0u);
    EXPECT_GT(stats.string_pct_max, 0.0);
    EXPECT_GE(stats.string_inflight_bytes_max, stats.string_inflight_bytes);
}

// ------------------------------------------------------------------- CSV file

TEST_F(SlickLoggerStatsTest, CsvHasHeaderAndRows) {
    Logger::instance().init(make_config(50, 5));

    for (int i = 0; i < 500; ++i) {
        LOG_INFO("entry {}", i);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    Logger::instance().shutdown();

    ASSERT_TRUE(std::filesystem::exists(kCsvFile));
    const auto lines = read_lines(kCsvFile);
    ASSERT_GE(lines.size(), 2u);
    EXPECT_EQ(lines[0], kExpectedHeader);

    const size_t expected_fields = count_fields(lines[0]);
    for (size_t i = 1; i < lines.size(); ++i) {
        EXPECT_EQ(count_fields(lines[i]), expected_fields)
            << "row " << i << " has the wrong field count: " << lines[i];
    }
}

TEST_F(SlickLoggerStatsTest, CsvRollsAtMaxSize) {
    LogConfig config = make_config(20, 5);
    config.stats_max_file_size = 512; // a handful of rows
    Logger::instance().init(config);

    for (int i = 0; i < 200; ++i) {
        LOG_INFO("entry {}", i);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    Logger::instance().shutdown();

    ASSERT_TRUE(std::filesystem::exists(kCsvBackup));
    ASSERT_TRUE(std::filesystem::exists(kCsvFile));

    // The reopened primary file starts with the header again, so each rolled file
    // stands on its own.
    const auto lines = read_lines(kCsvFile);
    ASSERT_GE(lines.size(), 1u);
    EXPECT_EQ(lines[0], kExpectedHeader);

    const auto backup_lines = read_lines(kCsvBackup);
    ASSERT_GE(backup_lines.size(), 1u);
    EXPECT_EQ(backup_lines[0], kExpectedHeader);
}

// ------------------------------------------------- regressions: race and rollback

TEST_F(SlickLoggerStatsTest, ConcurrentSnapshotsStayConsistent) {
    // Regression: stats_snapshot() used a seqlock over a shared LogStats, so a
    // reader copied the payload while the statistics thread assigned to it and
    // only afterwards noticed the generation had moved - a data race by the letter
    // of the memory model. Published snapshots are immutable now, so a reader can
    // never observe a half-written one.
    //
    // This also exercises the string frontier under real producer concurrency: the
    // sampler used to raw-copy slots off the reservation cursor, which a producer
    // may have reserved but not yet filled.
    Logger::instance().init(make_config(20, 2));

    std::atomic<bool> stop{false};
    std::atomic<int> inconsistent{0};

    std::vector<std::thread> readers;
    for (int t = 0; t < 4; ++t) {
        readers.emplace_back([&] {
            while (!stop.load(std::memory_order_relaxed)) {
                const LogStats s = Logger::instance().stats_snapshot();
                // Every one of these holds within any single sample. A torn read
                // mixing two samples, or reading a partially written one, breaks
                // at least one of them.
                if (s.entry_queue_capacity != 0 && s.entry_queue_capacity != 1024) {
                    ++inconsistent;
                }
                if (s.string_buffer_capacity != 0 && s.string_buffer_capacity != 4096) {
                    ++inconsistent;
                }
                if (s.entry_queue_depth_max < s.entry_queue_depth) {
                    ++inconsistent;
                }
                if (s.string_inflight_bytes_max < s.string_inflight_bytes) {
                    ++inconsistent;
                }
                if (s.entry_queue_pct > 100.0 || s.string_pct > 100.0) {
                    ++inconsistent;
                }
                if (s.string_inflight_bytes > s.string_buffer_capacity &&
                    s.string_buffer_capacity != 0) {
                    ++inconsistent;
                }
            }
        });
    }

    std::vector<std::thread> producers;
    for (int t = 0; t < 4; ++t) {
        producers.emplace_back([] {
            const std::string payload(80, 'c');
            for (int i = 0; i < 20000; ++i) {
                LOG_INFO("mt {} {}", i, payload);
            }
        });
    }
    for (auto& producer : producers) {
        producer.join();
    }
    stop.store(true, std::memory_order_relaxed);
    for (auto& reader : readers) {
        reader.join();
    }

    EXPECT_EQ(inconsistent.load(), 0);
}

TEST_F(SlickLoggerStatsTest, FailedCsvOpenLeavesLoggerStopped) {
    // Regression: start() set running_ and spawned the writer thread before
    // open_stats_csv() could throw, so a caller whose init() failed was left with a
    // live logger it never asked for. The CSV is opened before anything starts now.
    std::filesystem::create_directory(kCsvBlocked); // a directory cannot be opened as a file

    LogConfig config = make_config(50, 5);
    config.stats_file = kCsvBlocked;
    EXPECT_THROW(Logger::instance().init(config), std::runtime_error);

    // Nothing may be running: no writer thread, no version banner, and a LOG_* call
    // that quietly does nothing rather than queueing into a failed logger.
    LOG_INFO("must not be logged");
    Logger::instance().flush();

    EXPECT_TRUE(read_lines(kLogFile).empty());
    EXPECT_EQ(Logger::instance().stats_snapshot().timestamp_ns, 0u);
}

TEST_F(SlickLoggerStatsTest, LoggerUsableAfterFailedStatsInit) {
    // The rollback has to be complete enough that a corrected init() still works.
    std::filesystem::create_directory(kCsvBlocked);

    LogConfig bad = make_config(50, 5);
    bad.stats_file = kCsvBlocked;
    EXPECT_THROW(Logger::instance().init(bad), std::runtime_error);

    Logger::instance().init(make_config(50, 5));
    LOG_INFO("logged after recovery");
    Logger::instance().flush();

    const auto stats = wait_for_sample();
    EXPECT_GT(stats.entries_produced, 0u);
    EXPECT_TRUE(std::filesystem::exists(kCsvFile));
}

// --------------------------------------------- regression: frontier under a slow sink

namespace {

/// A sink that can be told to block for a long time inside write(), so the writer
/// thread can be pinned on one specific entry while producers pile up behind it.
class StallingSink : public ISink {
public:
    StallingSink() : ISink("stalling") {}
    void write(const LogEntry&) override {
        // Counted on ENTRY, before the stall: a test waiting for the count to reach
        // n knows the writer is inside the nth write() and has therefore finished
        // everything the previous batch did afterwards - the frontier update
        // included. That is a happens-after a sleep cannot express.
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

/// Block until @p sink has entered its @p target-th write(), or give up.
bool wait_for_writes(const std::shared_ptr<StallingSink>& sink, uint32_t target,
                     int timeout_ms = 5000) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (sink->writes() >= target) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

} // namespace

TEST_F(SlickLoggerStatsTest, StringPressureVisibleWhileWriterStallsOnLiteralEntry) {
    // Regression: the writer published the frontier from every entry it claimed,
    // including entries referencing no string at all - which reset it to "no
    // anchor". Pin the writer on one literal entry while producers fill the string
    // ring behind it and the stats thread saw outstanding depth but nothing to
    // anchor to, so string_inflight_bytes_max sat at zero while the ring filled.
    // The frontier only ever moves forward to a real position now.
    auto sink = std::make_shared<StallingSink>();

    LogConfig config;
    config.sinks.push_back(sink);
    config.log_queue_size = 1024;
    config.string_buffer_size = 4096;
    config.enable_stats = true;
    config.stats_interval_ms = 30;
    config.stats_sample_interval_ms = 2;
    Logger::instance().init(config);

    // One real string first, so a valid frontier exists to be preserved.
    const std::string payload(120, 's');
    LOG_INFO("anchor {}", payload);
    Logger::instance().flush();

    // Pin the writer on a single literal-only entry - the kind that used to wipe
    // the frontier - for far longer than a reporting interval.
    sink->set_stall_ms(400);
    LOG_INFO("literal only, no string ring usage");
    std::this_thread::sleep_for(std::chrono::milliseconds(30)); // let it claim that entry

    // Fill the string ring while the writer cannot drain.
    for (int i = 0; i < 400; ++i) {
        LOG_INFO("dynamic {}", payload);
    }

    // Sample while still stalled: a backlog is outstanding and the ring is full of
    // strings the writer has not reached.
    const uint64_t filled_at = now_ns();
    const LogStats stats = wait_for_sample_after(filled_at, 300);
    sink->set_stall_ms(0);

    ASSERT_GT(stats.entry_queue_depth, 0u) << "the writer should still be backed up";
    EXPECT_TRUE(stats.string_pct_valid)
        << "a wiped frontier made the whole row read as unknown";
    EXPECT_GT(stats.string_inflight_bytes_max, 0u)
        << "a zero peak reads as 'no pressure' while the ring is actually full";
    EXPECT_GT(stats.string_pct_max, 0.0);
}

TEST_F(SlickLoggerStatsTest, StringPressureDecaysWhenDynamicEntriesStop) {
    // Regression: the published position was the START of the oldest string the
    // writer had consumed, so once its entry was written the metric kept measuring
    // from a string nobody needed any more. Under a continuous literal-only
    // backlog the queue never empties, so the drained-queue reset never fires and
    // the stale value was reported as live pressure indefinitely.
    //
    // The watermark is the END of the last consumed string now: with no new string
    // reserved since, the write cursor sits exactly on it and the span is zero.
    auto sink = std::make_shared<StallingSink>();

    LogConfig config;
    config.sinks.push_back(sink);
    config.log_queue_size = 4096;
    config.string_buffer_size = 4096;
    config.enable_stats = true;
    config.stats_interval_ms = 30;
    config.stats_sample_interval_ms = 2;
    Logger::instance().init(config);

    // Real string pressure first.
    const std::string payload(120, 'p');
    for (int i = 0; i < 20; ++i) {
        LOG_INFO("dynamic {}", payload);
    }
    Logger::instance().flush();

    // Dynamic entries stop here. Keep a literal-only backlog running so the queue
    // never empties and the drained-queue reset cannot paper over the staleness.
    sink->set_stall_ms(1);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    LogStats stats{};
    bool saw_backlog = false;
    while (std::chrono::steady_clock::now() < deadline) {
        for (int i = 0; i < 200; ++i) {
            LOG_INFO("literal only {}", i); // reserves nothing in the string ring
        }
        stats = Logger::instance().stats_snapshot();
        if (stats.entry_queue_depth > 0) {
            saw_backlog = true;
            if (stats.string_inflight_bytes == 0) {
                break; // the span decayed to zero as it should
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    sink->set_stall_ms(0);

    ASSERT_TRUE(saw_backlog) << "the literal backlog never materialized";
    // A backlog is outstanding, but none of it needs the string ring, so the
    // reported span must be zero rather than a leftover from the dynamic phase.
    EXPECT_EQ(stats.string_inflight_bytes, 0u);
    EXPECT_DOUBLE_EQ(stats.string_pct, 0.0);
}

TEST_F(SlickLoggerStatsTest, StringPressureVisibleAtExactlyOneWrap) {
    // Regression: the frontier recorded only the string ring SLOT the writer had
    // reached, and the span was that slot subtracted from the write cursor modulo
    // capacity. A slot carries no lap count, so an empty ring and an exactly full
    // one occupy the same one: pin the writer on a valid frontier, have producers
    // reserve exactly one capacity behind it, and the span folded back to zero -
    // 0 bytes, 0%, reported as valid, with the entire ring still outstanding.
    // The frontier is an absolute reserve index now, so the span is a plain
    // subtraction that cannot alias.
    constexpr uint32_t kCapacity = 4096;
    auto sink = std::make_shared<StallingSink>();

    LogConfig config;
    config.sinks.push_back(sink);
    config.log_queue_size = 1024;
    config.string_buffer_size = kCapacity;
    config.enable_stats = true;
    config.stats_interval_ms = 30;
    config.stats_sample_interval_ms = 2;
    Logger::instance().init(config);

    // A drained anchor first: the writer consumes this string, so the frontier
    // ends up exactly on the write cursor and the span is a true zero. Everything
    // reserved from here on is measured against that point.
    const std::string anchor(120, 'a');
    LOG_INFO("anchor {}", anchor);
    Logger::instance().flush();

    const LogStats anchored = wait_for_sample_after(now_ns(), 2000);
    ASSERT_EQ(anchored.string_buffer_capacity, kCapacity);
    ASSERT_TRUE(anchored.string_pct_valid);
    ASSERT_EQ(anchored.string_inflight_bytes, 0u) << "the anchor should be fully drained";

    // string_bytes_written is the string ring's write cursor, and with the anchor
    // drained the frontier sits on it. Reserving exactly kCapacity from here lands
    // the cursor back on the frontier's slot - the aliasing case. Split into two
    // reservations that meet the ring boundary exactly, because slick-queue pads a
    // reservation that would straddle the end rather than splitting it, and that
    // padding would consume more of the index space than the bytes asked for.
    const uint64_t frontier = anchored.string_bytes_written;
    const uint32_t offset = static_cast<uint32_t>(frontier % kCapacity);
    const uint32_t to_boundary = kCapacity - offset; // fills the ring to its end
    const uint32_t after_wrap = offset;              // wraps and stops on the frontier

    // Pin the writer on a literal-only entry, which reserves nothing and so leaves
    // the frontier parked where the anchor put it.
    sink->set_stall_ms(2000);
    LOG_INFO("literal only, no string ring usage");
    std::this_thread::sleep_for(std::chrono::milliseconds(50)); // let it claim that entry

    // store_bytes_in_queue() reserves length + 1 for a terminated string, so a
    // payload of n - 1 bytes claims exactly n.
    LOG_INFO("{}", std::string(to_boundary - 1, 'x'));
    if (after_wrap != 0) {
        LOG_INFO("{}", std::string(after_wrap - 1, 'y'));
    }

    const LogStats stats = wait_for_sample_after(now_ns(), 1500);
    sink->set_stall_ms(0);

    ASSERT_EQ(stats.string_bytes_written, frontier + kCapacity)
        << "the test must reserve exactly one capacity for the aliasing to arise";
    ASSERT_GT(stats.entry_queue_depth, 0u) << "the writer should still be backed up";
    ASSERT_TRUE(stats.string_pct_valid);
    // The ring is exactly full: every byte between the frontier and the write
    // cursor is still needed, and one more reservation would overwrite a live
    // string. Reporting zero here is the worst possible answer - it reads as an
    // idle ring at the moment the ring is at its limit.
    EXPECT_EQ(stats.string_inflight_bytes, kCapacity)
        << "an exactly full ring aliased to an empty one";
    EXPECT_DOUBLE_EQ(stats.string_pct, 100.0);
    EXPECT_EQ(stats.string_inflight_bytes_max, kCapacity);
    EXPECT_DOUBLE_EQ(stats.string_pct_max, 100.0);
}

TEST_F(SlickLoggerStatsTest, StringPressureSaturatesWhenProducersLapDuringDispatch) {
    // Regression: the absolute frontier was recovered by lifting the entry's string
    // POINTER against the write cursor - but the cursor was read after the batch had
    // been dispatched. A pointer names a slot, and lifting it picks the lap that
    // slot belongs to, so a cursor taken after a slow sink has run picks a LATER
    // lap: the frontier slides forward past strings the batch still held and the
    // span collapses to a residual, or to zero on a whole number of laps. The
    // reading was worst exactly where it mattered - a sink slow enough to let
    // producers lap the ring reported an idle ring. The cursor is sampled before
    // the dispatch now, so the span instead grows past capacity and saturates.
    //
    // Distinct from StringPressureVisibleAtExactlyOneWrap, where the stalled entry
    // is literal-only and the frontier was already resolved before the stall began.
    // Here the stall is inside the dispatch of the string-bearing entry itself,
    // which is the window the lift depends on.
    constexpr uint32_t kCapacity = 4096;
    auto sink = std::make_shared<StallingSink>();

    LogConfig config;
    config.sinks.push_back(sink);
    config.log_queue_size = 1024;
    config.string_buffer_size = kCapacity;
    config.enable_stats = true;
    config.stats_interval_ms = 30;
    config.stats_sample_interval_ms = 2;
    Logger::instance().init(config);
    Logger::instance().flush(); // drain the version banner, so the counts below are ours

    const uint32_t banner_writes = sink->writes();

    // Pin the writer INSIDE the dispatch of an entry that carries a string: this
    // batch's frontier is resolved only once write() returns.
    sink->set_stall_ms(1000);
    const std::string anchor(120, 'a');
    LOG_INFO("anchor {}", anchor);
    ASSERT_TRUE(wait_for_writes(sink, banner_writes + 1))
        << "the writer never claimed the anchor";

    // Nothing else has reserved yet, so the write cursor is exactly where the
    // anchor's string ends - the position its frontier must resolve to.
    const LogStats anchored = wait_for_sample_after(now_ns(), 2000);
    ASSERT_EQ(anchored.string_buffer_capacity, kCapacity);
    const uint64_t frontier = anchored.string_bytes_written;
    const uint32_t offset = static_cast<uint32_t>(frontier % kCapacity);
    ASSERT_EQ(sink->writes(), banner_writes + 1)
        << "the writer left the anchor's write() early; the lap below would not "
           "land inside the dispatch window this test exists to cover";

    // Lap the ring exactly twice while the writer is still inside that write().
    // Two whole laps put the cursor back on the frontier's own slot, which is the
    // lift's worst case. Split so no reservation straddles the ring end, which
    // slick-queue pads rather than splits; store_bytes_in_queue() reserves
    // length + 1 for a terminated string, so a payload of n - 1 claims exactly n.
    LOG_INFO("{}", std::string(kCapacity - offset - 1, 'x')); // up to the ring end
    LOG_INFO("{}", std::string(kCapacity - 1, 'y'));          // one full lap
    if (offset != 0) {
        LOG_INFO("{}", std::string(offset - 1, 'z'));         // back onto the slot
    }

    // The writer leaves the anchor's write(), resolves the frontier from it, then
    // blocks on the first flood entry. Entering that second write() is what tells
    // us the frontier has been published - a sleep could not.
    ASSERT_TRUE(wait_for_writes(sink, banner_writes + 2))
        << "the writer never finished the anchor";

    const LogStats stats = wait_for_sample_after(now_ns(), 2000);
    sink->set_stall_ms(0);

    ASSERT_EQ(stats.string_bytes_written, frontier + uint64_t{2} * kCapacity)
        << "the flood must be exactly two laps for the lift to land back on the "
           "frontier's slot";
    ASSERT_GT(stats.entry_queue_depth, 0u) << "the writer should still be backed up";
    ASSERT_TRUE(stats.string_pct_valid);
    // Producers have overwritten the whole ring twice over behind a writer that has
    // consumed one entry of it. Saturated is the only honest reading.
    EXPECT_EQ(stats.string_inflight_bytes, kCapacity)
        << "the frontier was dragged forward by the lap and reported an idle ring";
    EXPECT_DOUBLE_EQ(stats.string_pct, 100.0);
    EXPECT_EQ(stats.string_inflight_bytes_max, kCapacity);
    EXPECT_DOUBLE_EQ(stats.string_pct_max, 100.0);
}

TEST_F(SlickLoggerStatsTest, StringPressureSaturatesForAnEntryLappedInTheBacklog) {
    // Regression: the lap a string pointer belonged to was chosen as the newest one
    // at or below the write cursor. That is right only while the string is still
    // resident, and an entry can be lapped before the writer ever reaches it -
    // published early behind a slow sink, then buried by producers while it waited.
    // Its stale slot was lifted onto a recent lap, the frontier jumped forward past
    // strings still outstanding, and the span collapsed to a residual or to zero.
    // The lap is chosen from the frontier upwards now, so a stale slot lands on an
    // earlier lap, the span comes out too large and clamps to capacity: full.
    //
    // slick-queue reserves one entry slot per log call and read() returns a single
    // reservation, so each entry is dispatched on its own - the entry that gets
    // lapped here is the whole batch, which is why no newer string rescues it.
    constexpr uint32_t kCapacity = 4096;
    auto sink = std::make_shared<StallingSink>();

    LogConfig config;
    config.sinks.push_back(sink);
    config.log_queue_size = 1024;
    config.string_buffer_size = kCapacity;
    config.enable_stats = true;
    config.stats_interval_ms = 30;
    config.stats_sample_interval_ms = 2;
    Logger::instance().init(config);

    // A drained anchor establishes the frontier the lap is chosen from.
    const std::string anchor(120, 'a');
    LOG_INFO("anchor {}", anchor);
    Logger::instance().flush();
    const LogStats anchored = wait_for_sample_after(now_ns(), 2000);
    ASSERT_EQ(anchored.string_buffer_capacity, kCapacity);
    ASSERT_EQ(anchored.string_inflight_bytes, 0u);
    const uint64_t frontier = anchored.string_bytes_written;
    const uint32_t banner_writes = sink->writes();

    // Block the writer on an entry that reserves nothing, so the frontier stays put
    // while the entry below is published and then buried.
    sink->set_stall_ms(1200);
    LOG_INFO("literal only, no string ring usage");
    ASSERT_TRUE(wait_for_writes(sink, banner_writes + 1)) << "the writer never blocked";

    // The victim: a string-bearing entry that will sit in the backlog.
    LOG_INFO("{}", std::string(120, 'v'));
    const LogStats victim_in = wait_for_sample_after(now_ns(), 2000);
    const uint64_t victim_end = victim_in.string_bytes_written;
    ASSERT_GT(victim_end, frontier);
    ASSERT_EQ(sink->writes(), banner_writes + 1)
        << "the writer left the blocker early; the victim would not be buried";

    // Bury it: exactly two laps, so the lift's old anchor lands back on the victim's
    // own slot. Split so no reservation straddles the ring end, which slick-queue
    // pads rather than splits.
    const uint32_t offset = static_cast<uint32_t>(victim_end % kCapacity);
    LOG_INFO("{}", std::string(kCapacity - offset - 1, 'x'));
    LOG_INFO("{}", std::string(kCapacity - 1, 'y'));
    if (offset != 0) {
        LOG_INFO("{}", std::string(offset - 1, 'z'));
    }

    // Writes 2 and 3 are the victim and the entry after it: reaching the third means
    // the victim was dispatched AND its watermark published.
    ASSERT_TRUE(wait_for_writes(sink, banner_writes + 3))
        << "the writer never got past the victim";

    const LogStats stats = wait_for_sample_after(now_ns(), 2000);
    sink->set_stall_ms(0);

    ASSERT_EQ(stats.string_bytes_written, victim_end + uint64_t{2} * kCapacity)
        << "the burial must be exactly two laps to land on the victim's own slot";
    ASSERT_GT(stats.entry_queue_depth, 0u) << "the writer should still be backed up";
    ASSERT_TRUE(stats.string_pct_valid);
    // Two laps were written over a ring the writer has barely started draining.
    EXPECT_EQ(stats.string_inflight_bytes, kCapacity)
        << "a slot lapped in the backlog was lifted onto a recent lap";
    EXPECT_DOUBLE_EQ(stats.string_pct, 100.0);
    EXPECT_EQ(stats.string_inflight_bytes_max, kCapacity);
    EXPECT_DOUBLE_EQ(stats.string_pct_max, 100.0);
}


TEST_F(SlickLoggerStatsTest, StringPressureSaturatesForAnEntryLappedBeforeAnyAnchor) {
    // Regression: the lapped-in-the-backlog case above is caught by choosing the
    // lap from the frontier upwards - but only once a frontier exists. The FIRST
    // entry to be drained with a ring-resident string has none, and that case used
    // to be resolved the discarded way round: the newest lap at or below the write
    // cursor. That is right only while the string is still resident, and startup
    // is exactly where it need not be. If producers flood before the writer
    // samples the cursor for that first batch, the cursor sits whole laps past the
    // entry; on a whole number of laps the lift landed back on the entry's own
    // slot, the frontier was published at the cursor and an overrun ring reported
    // 0% - with no earlier anchor to correct it, because none had ever existed.
    //
    // start()'s banner does reserve from the ring (its version argument is a char
    // array, which enqueue_argument copies), so it normally anchors the run on its
    // own first drain. A min_level that filters the banner out is what leaves the
    // ring genuinely untouched, and makes the unanchored window reachable without
    // racing the writer thread's startup.
    constexpr uint32_t kCapacity = 4096;
    auto sink = std::make_shared<StallingSink>();

    LogConfig config;
    config.sinks.push_back(sink);
    config.min_level = LogLevel::L_WARN; // drops the INFO banner, so nothing anchors
    config.log_queue_size = 1024;
    config.string_buffer_size = kCapacity;
    config.enable_stats = true;
    config.stats_interval_ms = 30;
    config.stats_sample_interval_ms = 2;
    Logger::instance().init(config);

    {
        // Nothing has reserved a byte: the banner was filtered, so the frontier is
        // unset rather than merely stale.
        const LogStats fresh = wait_for_sample_after(now_ns(), 2000);
        ASSERT_EQ(fresh.string_buffer_capacity, kCapacity);
        ASSERT_EQ(fresh.string_bytes_written, 0u)
            << "the banner reached the ring; this test needs it filtered";
    }

    // Block the writer on an entry that reserves nothing, so it is busy while the
    // victim below is published and buried, and leaves the frontier unset.
    sink->set_stall_ms(1000);
    LOG_WARN("literal only, no string ring usage");
    ASSERT_TRUE(wait_for_writes(sink, 1)) << "the writer never blocked";

    // The victim: the first entry of the run to name the string ring at all.
    LOG_WARN("{}", std::string(120, 'v'));
    const LogStats victim_in = wait_for_sample_after(now_ns(), 2000);
    const uint64_t victim_end = victim_in.string_bytes_written;
    ASSERT_GT(victim_end, 0u) << "the victim reserved nothing from the string ring";
    ASSERT_LT(victim_end, kCapacity) << "the victim must sit on lap zero";

    // Bury it under exactly two laps, so the newest lap at or below the cursor is
    // the victim's own slot - the aliasing that reported an empty ring. Split so
    // no reservation straddles the ring end, which slick-queue pads rather than
    // splits.
    const uint32_t offset = static_cast<uint32_t>(victim_end % kCapacity);
    LOG_WARN("{}", std::string(kCapacity - offset - 1, 'x'));
    LOG_WARN("{}", std::string(kCapacity - 1, 'y'));
    if (offset != 0) {
        LOG_WARN("{}", std::string(offset - 1, 'z'));
    }
    ASSERT_EQ(sink->writes(), 1u)
        << "the writer left the blocker early; the victim would not be buried";

    // Writes 2 and 3 are the victim and the entry behind it: reaching the third
    // means the victim was dispatched AND its watermark published.
    ASSERT_TRUE(wait_for_writes(sink, 3)) << "the writer never got past the victim";

    const LogStats stats = wait_for_sample_after(now_ns(), 2000);
    sink->set_stall_ms(0);

    ASSERT_EQ(stats.string_bytes_written, victim_end + uint64_t{2} * kCapacity)
        << "the burial must be exactly two laps to land on the victim's own slot";
    ASSERT_GT(stats.entry_queue_depth, 0u) << "the writer should still be backed up";
    ASSERT_TRUE(stats.string_pct_valid);
    // Two laps were written over a ring nothing had ever been consumed from.
    EXPECT_EQ(stats.string_inflight_bytes, kCapacity)
        << "the first entry to name the ring was anchored on the write cursor";
    EXPECT_DOUBLE_EQ(stats.string_pct, 100.0);
    EXPECT_EQ(stats.string_inflight_bytes_max, kCapacity);
    EXPECT_DOUBLE_EQ(stats.string_pct_max, 100.0);
}

TEST_F(SlickLoggerStatsTest, DrainedEntryFreesNothingWhileEntriesStayQueued) {
    // Regression: the frontier was read out of whichever entry the writer drained
    // last. A producer reserves its string bytes while the entry is being built
    // and its entry slot only at the end, so slot order and string order are two
    // independent races - a producer holding EARLIER string bytes can take a
    // LATER slot. Freeing the ring up to the drained entry's own string therefore
    // released bytes an entry still queued behind it owned, and the gauge read
    // idle over a ring that was not; worse, the frontier then moved backwards
    // when that entry finally drained.
    //
    // The frontier comes from a cursor pair now - entry cursor snapshotted first,
    // then string cursor - and promotes only once the writer has drained past the
    // entry half. So a drained entry frees nothing while anything published
    // before that snapshot is still queued, whatever order the producers used.
    // That is what this pins: three entries published before the writer gets past
    // the first, and draining the first must not shrink the span by its bytes.
    constexpr uint32_t kCapacity = 4096;
    constexpr size_t kPayload = 120;                  // 121 bytes asked for each
    auto sink = std::make_shared<StallingSink>();
    sink->set_stall_ms(1000); // pins the writer from its very first write

    LogConfig config;
    // The ring charges whole string_items_per_slot units, so each string's
    // footprint is its 121 bytes rounded up to one.
    const uint64_t unit = config.string_items_per_slot;
    const uint64_t kEach = (kPayload + 1 + unit - 1) / unit * unit;
    config.sinks.push_back(sink);
    config.min_level = LogLevel::L_WARN; // drops the INFO banner: the ring starts empty
    config.log_queue_size = 1024;
    config.string_buffer_size = kCapacity;
    config.enable_stats = true;
    config.stats_interval_ms = 30;
    config.stats_sample_interval_ms = 2;
    Logger::instance().init(config);

    // First entry: the writer claims it, enters write(), and blocks there. Its
    // frontier step has not run yet - that happens once write() returns.
    LOG_WARN("{}", std::string(kPayload, 'a'));
    ASSERT_TRUE(wait_for_writes(sink, 1)) << "the writer never blocked on the first entry";

    // Two more published while it is blocked, so both are in the ring before the
    // writer's snapshot is taken. The third is what keeps the queue non-empty
    // while the second is being written: read_index_ advances at read() time, so
    // without it the queue would look drained and report a valid zero.
    LOG_WARN("{}", std::string(kPayload, 'b'));
    LOG_WARN("{}", std::string(kPayload, 'c'));
    ASSERT_EQ(sink->writes(), 1u) << "the writer left the first entry early";

    // Reaching the second write means the first was dispatched AND its frontier
    // step ran. The third is still queued, so nothing has been confirmed drained.
    ASSERT_TRUE(wait_for_writes(sink, 2)) << "the writer never got past the first entry";
    const LogStats stats = wait_for_sample_after(now_ns(), 2000);
    sink->set_stall_ms(0);

    ASSERT_EQ(sink->writes(), 2u) << "the writer ran ahead; the third entry drained";
    ASSERT_EQ(stats.string_bytes_written, 3u * kEach) << "exactly three strings reserved";
    ASSERT_GT(stats.entry_queue_depth, 0u) << "the third entry must still be queued";
    ASSERT_TRUE(stats.string_pct_valid);
    // All three strings are still in flight: one is mid-write and two are queued.
    // The old rule freed the first entry's bytes the moment it was dispatched.
    EXPECT_EQ(stats.string_inflight_bytes, 3u * kEach)
        << "draining one entry freed bytes while entries behind it were queued";
}

// ------------------------------------------------------ string_items_per_slot

namespace {

/// Log one 10-byte string (11 bytes with its terminator) into an otherwise empty
/// string ring and report the footprint the ring charged for it.
uint64_t footprint_of_one_short_string(uint32_t items_per_slot) {
    LogConfig config = make_config(20, 2, false);
    config.min_level = LogLevel::L_WARN; // drops the INFO banner: the ring starts empty
    config.string_items_per_slot = items_per_slot;
    Logger::instance().init(config);

    LOG_WARN("{}", std::string(10, 's'));
    Logger::instance().flush();
    const LogStats stats = wait_for_sample_after(now_ns());
    // As TearDown does, so the next call starts from a clean logger.
    Logger::instance().shutdown();
    Logger::instance().reset();
    return stats.string_bytes_written;
}

} // namespace

TEST_F(SlickLoggerStatsTest, StringFootprintRoundsToItemsPerSlot) {
    // A string consumes whole units of string_items_per_slot bytes, so a short one
    // costs a full unit; the default puts every string on its own cache line.
    EXPECT_EQ(footprint_of_one_short_string(kDefaultStringItemsPerSlot),
              uint64_t{kDefaultStringItemsPerSlot});
    EXPECT_EQ(footprint_of_one_short_string(1), 11u) << "1 must keep the byte-exact layout";
    EXPECT_EQ(footprint_of_one_short_string(16), 16u);
}

TEST_F(SlickLoggerStatsTest, StringItemsPerSlotIsNormalized) {
    // slick-queue throws on a value that is not a power of 2 or exceeds the ring,
    // so init() normalizes instead of failing: 0 reads as 1, anything else rounds
    // up to a power of 2, and the result is clamped to the (4096-byte) ring.
    EXPECT_EQ(footprint_of_one_short_string(0), 11u);
    EXPECT_EQ(footprint_of_one_short_string(48), 64u);
    EXPECT_EQ(footprint_of_one_short_string(1u << 20), 4096u);
    // Above 2^31 the round-up would reach 2^32, which is 0 as a uint32_t (and in a
    // 32-bit size_t); it must still clamp to the ring rather than make init() throw.
    EXPECT_EQ(footprint_of_one_short_string(std::numeric_limits<uint32_t>::max()), 4096u);
    EXPECT_EQ(footprint_of_one_short_string((1u << 31) + 1), 4096u);
}

TEST_F(SlickLoggerStatsTest, StringsLongerThanOneUnitRoundTrip) {
    // A string spanning several units still occupies one reservation, and the
    // short strings around it must land on the unit boundaries after it.
    LogConfig config = make_config(20, 2, false);
    config.string_items_per_slot = 64;
    Logger::instance().init(config);

    const std::string before(5, 'b');
    const std::string big(1000, 'L');
    const std::string after(70, 'a');
    LOG_INFO("before={} big={} after={}", before, big, after);
    Logger::instance().flush();
    Logger::instance().shutdown();

    std::ifstream in(kLogFile);
    const std::string contents((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    EXPECT_NE(contents.find("before=" + before + " big=" + big + " after=" + after), std::string::npos);
}
