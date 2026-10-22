#include <slick/logger.hpp>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using slick::logger::Logger;
using slick::logger::LogConfig;
using slick::logger::LogEntry;
using slick::logger::LogStats;

namespace {

struct Line {
    uint32_t thread_id;
    std::string message;
};

// Keeps every message. lines_ is the writer thread's until flush() returns.
class CaptureSink : public slick::logger::ISink {
public:
    CaptureSink() : ISink("capture") {}
    void write(const LogEntry& entry) override {
        lines_.push_back({entry.thread_id, std::string(formatted_message(entry).first)});
        count_.store(lines_.size(), std::memory_order_release);
    }
    void flush() override {}
    std::vector<Line> lines_;
    std::atomic<size_t> count_{0};
};

// Holds the writer inside write() on the "hold" entry until release_ is set.
class HoldingSink : public CaptureSink {
public:
    void write(const LogEntry& entry) override {
        CaptureSink::write(entry);
        if (lines_.back().message == "hold") {
            reached_.store(true, std::memory_order_release);
            while (!release_.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
        }
    }
    std::atomic<bool> reached_{false};
    std::atomic<bool> release_{false};
};

LogConfig ring_config(std::shared_ptr<slick::logger::ISink> sink) {
    LogConfig config;
    config.sinks = {std::move(sink)};
    config.thread_rings = true;
    return config;
}

// Messages "<thread> <seq> ..." grouped by thread, banner and others skipped.
std::map<int, std::vector<int>> sequences(const std::vector<Line>& lines) {
    std::map<int, std::vector<int>> by_thread;
    for (const auto& line : lines) {
        int thread = 0, seq = 0;
        if (std::sscanf(line.message.c_str(), "t%d %d", &thread, &seq) == 2) {
            by_thread[thread].push_back(seq);
        }
    }
    return by_thread;
}

template<typename Pred>
bool eventually(Pred pred) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!pred()) {
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

class ThreadRingTest : public ::testing::Test {
protected:
    void TearDown() override { Logger::instance().reset(); }
};

} // namespace

TEST_F(ThreadRingTest, OneThreadsEntriesArriveInOrderWithTheirStrings) {
    auto& logger = Logger::instance();
    auto sink = std::make_shared<CaptureSink>();
    logger.init(ring_config(sink));
    constexpr int kCount = 2000;
    for (int i = 0; i < kCount; ++i) {
        const std::string text(static_cast<size_t>(i % 50), static_cast<char>('a' + i % 26));
        char buffer[8] = "buf";
        LOG_INFO("t0 {} [{}] {} {} {}", i, text, std::string_view{"view"}, buffer, static_cast<const char*>("ptr"));
    }
    logger.flush();
    ASSERT_EQ(sink->lines_.size(), kCount + 1u); // and the banner
    for (int i = 0; i < kCount; ++i) {
        const std::string text(static_cast<size_t>(i % 50), static_cast<char>('a' + i % 26));
        ASSERT_EQ(sink->lines_[i + 1].message, "t0 " + std::to_string(i) + " [" + text + "] view buf ptr");
    }
}

TEST_F(ThreadRingTest, EveryThreadKeepsItsOrder) {
    auto& logger = Logger::instance();
    auto sink = std::make_shared<CaptureSink>();
    logger.init(ring_config(sink));
    // Fits a 1 MiB ring even if the writer never ran, so nothing may be dropped.
    constexpr int kThreads = 8;
    constexpr int kCount = 5000;
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([t] {
            const std::string payload = "payload-of-thread-" + std::to_string(t);
            for (int i = 0; i < kCount; ++i) {
                LOG_INFO("t{} {} {}", t, i, payload);
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    logger.flush();
    ASSERT_EQ(sink->lines_.size(), 1u + kThreads * kCount);
    std::map<int, uint32_t> thread_ids;
    for (const auto& line : sink->lines_) {
        int thread = 0, seq = 0;
        char payload[64] = {};
        if (std::sscanf(line.message.c_str(), "t%d %d %63s", &thread, &seq, payload) == 3) {
            ASSERT_EQ(std::string(payload), "payload-of-thread-" + std::to_string(thread));
            const auto [it, added] = thread_ids.emplace(thread, line.thread_id);
            ASSERT_EQ(it->second, line.thread_id) << "one thread, one thread id";
        }
    }
    const auto by_thread = sequences(sink->lines_);
    ASSERT_EQ(by_thread.size(), static_cast<size_t>(kThreads));
    for (const auto& [thread, seqs] : by_thread) {
        ASSERT_EQ(seqs.size(), static_cast<size_t>(kCount));
        for (int i = 0; i < kCount; ++i) {
            ASSERT_EQ(seqs[i], i) << "thread " << thread;
        }
    }
}

TEST_F(ThreadRingTest, FullRingSpillsToTheSharedRingInOrder) {
    auto& logger = Logger::instance();
    auto sink = std::make_shared<HoldingSink>();
    LogConfig config = ring_config(sink);
    config.thread_ring_size = 4096;
    config.enable_stats = true;
    config.stats_interval_ms = 10;
    config.stats_sample_interval_ms = 1;
    logger.init(config);

    LOG_INFO("hold");
    ASSERT_TRUE(eventually([&] { return sink->reached_.load(std::memory_order_acquire); }));
    // Far more than 4 KiB holds behind the held entry, so most of these spill.
    constexpr int kCount = 500;
    for (int i = 0; i < kCount; ++i) {
        LOG_INFO("t0 {} {}", i, "text-" + std::to_string(i));
    }
    sink->release_.store(true, std::memory_order_release);
    logger.flush();
    // The writer has passed every spilled entry, so these go back to the ring.
    for (int i = kCount; i < 2 * kCount; ++i) {
        LOG_INFO("t0 {} {}", i, "text-" + std::to_string(i));
        if (i % 20 == 0) {
            logger.flush();
        }
    }
    logger.flush();

    const auto seqs = sequences(sink->lines_)[0];
    ASSERT_EQ(seqs.size(), static_cast<size_t>(2 * kCount)) << "nothing dropped";
    for (int i = 0; i < 2 * kCount; ++i) {
        ASSERT_EQ(seqs[i], i);
    }
    for (const auto& line : sink->lines_) {
        int seq = 0;
        if (std::sscanf(line.message.c_str(), "t0 %d", &seq) == 1) {
            ASSERT_EQ(line.message, "t0 " + std::to_string(seq) + " text-" + std::to_string(seq));
        }
    }
    const LogStats stats = logger.stats_snapshot();
    EXPECT_EQ(stats.entry_loss_count, 0u);
    // Spilled strings go through the shared string ring; ring records keep theirs inline.
    EXPECT_TRUE(eventually([&] { return logger.stats_snapshot().string_bytes_written > 0; }))
        << "nothing spilled, so the test did not cover the spill";
}

TEST_F(ThreadRingTest, DropPolicyKeepsTheOldestAndCountsTheRest) {
    auto& logger = Logger::instance();
    auto sink = std::make_shared<HoldingSink>();
    LogConfig config = ring_config(sink);
    config.thread_ring_size = 4096;
    config.thread_ring_full = slick::logger::RingFullPolicy::Drop;
    config.enable_stats = true;
    config.stats_interval_ms = 10;
    config.stats_sample_interval_ms = 1;
    logger.init(config);

    LOG_INFO("hold");
    ASSERT_TRUE(eventually([&] { return sink->reached_.load(std::memory_order_acquire); }));
    constexpr int kCount = 500; // far more than 4 KiB holds behind the held entry
    for (int i = 0; i < kCount; ++i) {
        LOG_INFO("t0 {} {}", i, std::string("text"));
    }
    sink->release_.store(true, std::memory_order_release);
    logger.flush();

    const auto seqs = sequences(sink->lines_)[0];
    ASSERT_FALSE(seqs.empty());
    ASSERT_LT(seqs.size(), static_cast<size_t>(kCount));
    for (size_t i = 0; i < seqs.size(); ++i) {
        ASSERT_EQ(seqs[i], static_cast<int>(i)) << "the oldest entries are the ones kept";
    }
    const uint64_t dropped = kCount - seqs.size();
    EXPECT_TRUE(eventually([&] { return logger.stats_snapshot().entry_loss_count == dropped; }))
        << "loss count " << logger.stats_snapshot().entry_loss_count << ", dropped " << dropped;
    EXPECT_EQ(logger.stats_snapshot().string_bytes_written, 0u) << "a dropped entry never touches the shared rings";
}

TEST_F(ThreadRingTest, SpilledEntriesWaitForRingRecordsTheWriterHasNotSeenYet) {
    // After draining a ring the writer leaves it alone for the poll interval, but
    // reads the shared queue at once. A spill made in that window must still come
    // out after the ring records written before it.
    auto& logger = Logger::instance();
    auto sink = std::make_shared<CaptureSink>();
    LogConfig config = ring_config(sink);
    config.thread_ring_size = 4096;
    config.writer_poll_interval_us = 200000; // a window long enough to log into
    logger.init(config);
    LOG_INFO("t0 0 {}", std::string("first"));
    ASSERT_TRUE(eventually([&] { return sink->count_.load(std::memory_order_acquire) >= 2; }));
    // The ring was just drained, so the writer will not look at it again for a
    // while: these fill it, and the rest spill.
    constexpr int kCount = 300;
    for (int i = 1; i < kCount; ++i) {
        LOG_INFO("t0 {} {}", i, std::string("next"));
    }
    logger.flush();
    const auto seqs = sequences(sink->lines_)[0];
    ASSERT_EQ(seqs.size(), static_cast<size_t>(kCount));
    for (int i = 0; i < kCount; ++i) {
        ASSERT_EQ(seqs[i], i);
    }
}

TEST_F(ThreadRingTest, ThreadsSpillingUnderLoadKeepTheirOrder) {
    // A writer slower than four producers and rings of 4 KiB: every thread spills
    // and returns to its ring many times. Equal timestamps are common here too.
    auto& logger = Logger::instance();
    auto sink = std::make_shared<CaptureSink>();
    LogConfig config = ring_config(sink);
    config.thread_ring_size = 4096;
    config.log_queue_size = size_t{1} << 17; // room for every spill, so none is overwritten
    logger.init(config);
    constexpr int kThreads = 4;
    constexpr int kCount = 20000;
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([t] {
            const std::string payload = "spill-payload-" + std::to_string(t);
            for (int i = 0; i < kCount; ++i) {
                LOG_INFO("t{} {} {}", t, i, payload);
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    logger.flush();
    ASSERT_EQ(sink->lines_.size(), 1u + kThreads * kCount);
    const auto by_thread = sequences(sink->lines_);
    ASSERT_EQ(by_thread.size(), static_cast<size_t>(kThreads));
    for (const auto& [thread, seqs] : by_thread) {
        ASSERT_EQ(seqs.size(), static_cast<size_t>(kCount));
        for (int i = 0; i < kCount; ++i) {
            ASSERT_EQ(seqs[i], i) << "thread " << thread;
        }
    }
}

TEST_F(ThreadRingTest, ThreadsPastTheCapUseTheSharedRingInOrder) {
    auto& logger = Logger::instance();
    auto sink = std::make_shared<CaptureSink>();
    LogConfig config = ring_config(sink);
    config.max_thread_rings = 2; // this thread holds one since the banner
    logger.init(config);
    constexpr int kThreads = 4;
    constexpr int kCount = 1000;
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([t] {
            for (int i = 0; i < kCount; ++i) {
                LOG_INFO("t{} {} {}", t, i, std::string("text"));
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    logger.flush();
    ASSERT_EQ(sink->lines_.size(), 1u + kThreads * kCount);
    for (const auto& [thread, seqs] : sequences(sink->lines_)) {
        ASSERT_EQ(seqs.size(), static_cast<size_t>(kCount));
        for (int i = 0; i < kCount; ++i) {
            ASSERT_EQ(seqs[i], i) << "thread " << thread;
        }
    }
}

TEST_F(ThreadRingTest, StringsTooBigForTheRingKeepTheirPlace) {
    auto& logger = Logger::instance();
    auto sink = std::make_shared<CaptureSink>();
    LogConfig config = ring_config(sink);
    config.thread_ring_size = 4096; // records up to 2 KiB
    logger.init(config);
    const std::string big(3000, 'x');
    for (int i = 0; i < 30; ++i) {
        // Equal timestamps are common at 100 ns clock resolution, so order must
        // not depend on them: the big entry stays in this thread's ring.
        LOG_INFO("t0 {} small", 3 * i);
        LOG_INFO("t0 {} {}", 3 * i + 1, big);
        LOG_INFO("t0 {} small", 3 * i + 2);
        logger.flush();
    }
    const auto seqs = sequences(sink->lines_)[0];
    ASSERT_EQ(seqs.size(), 90u);
    for (int i = 0; i < 90; ++i) {
        ASSERT_EQ(seqs[i], i);
    }
    EXPECT_EQ(sink->lines_[2].message, "t0 1 " + big);
}

TEST_F(ThreadRingTest, ExitedThreadsHandTheirRingOn) {
    auto& logger = Logger::instance();
    auto sink = std::make_shared<CaptureSink>();
    LogConfig config = ring_config(sink);
    config.max_thread_rings = 2; // this thread's, and one for the workers to share in turn
    config.enable_stats = true;
    config.stats_interval_ms = 10;
    config.stats_sample_interval_ms = 1;
    logger.init(config);
    constexpr int kThreads = 50;
    for (int t = 0; t < kThreads; ++t) {
        std::thread([t] {
            for (int i = 0; i < 10; ++i) {
                LOG_INFO("t{} {} {}", t, i, std::string("inline"));
            }
        }).join();
        logger.flush(); // drained, so the next thread can take the slot over
    }
    ASSERT_EQ(sink->lines_.size(), 1u + kThreads * 10);
    // A worker without a ring would have put its strings in the shared string ring.
    ASSERT_TRUE(eventually([&] { return logger.stats_snapshot().entries_produced == 1u + kThreads * 10; }));
    EXPECT_EQ(logger.stats_snapshot().string_bytes_written, 0u);
}

TEST_F(ThreadRingTest, SinkCanLogFromTheWriterThread) {
    class EchoSink : public CaptureSink {
    public:
        void write(const LogEntry& entry) override {
            CaptureSink::write(entry);
            if (lines_.back().message == "ping") {
                LOG_INFO("pong {}", std::string("from the writer"));
            }
        }
    };
    auto& logger = Logger::instance();
    auto sink = std::make_shared<EchoSink>();
    logger.init(ring_config(sink));
    LOG_INFO("ping");
    logger.flush();
    logger.flush(); // the writer's own ring did not exist at the first flush
    ASSERT_EQ(sink->lines_.size(), 3u);
    EXPECT_EQ(sink->lines_[2].message, "pong from the writer");
}

TEST_F(ThreadRingTest, PrepareThreadClaimsARingAheadOfTheFirstCall) {
    auto& logger = Logger::instance();
    EXPECT_FALSE(logger.prepare_thread()) << "not running";

    // Rings off: nothing to claim.
    LogConfig shared = ring_config(std::make_shared<CaptureSink>());
    shared.thread_rings = false;
    logger.init(shared);
    EXPECT_FALSE(logger.prepare_thread());
    logger.shutdown();

    auto sink = std::make_shared<CaptureSink>();
    LogConfig config = ring_config(sink);
    config.max_thread_rings = 2;
    config.min_level = slick::logger::LogLevel::L_WARN; // filters the INFO banner, and any LOG_INFO warm-up
    logger.init(config);
    EXPECT_TRUE(logger.prepare_thread()) << "the banner was filtered, so this claims the first ring";
    EXPECT_TRUE(logger.prepare_thread()) << "and a second call finds it";

    std::atomic<bool> first_ready{false};
    std::atomic<bool> first_done{false};
    bool first_claimed = false;
    bool second_claimed = true;
    std::thread first([&] {
        LOG_INFO("filtered, claims nothing");
        first_claimed = logger.prepare_thread();
        first_ready.store(true, std::memory_order_release);
        while (!first_done.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        LOG_WARN("t1 0 {}", std::string("from a prepared ring"));
    });
    ASSERT_TRUE(eventually([&] { return first_ready.load(std::memory_order_acquire); }));
    std::thread second([&] { second_claimed = logger.prepare_thread(); }); // the registry is full now
    second.join();
    first_done.store(true, std::memory_order_release);
    first.join();
    logger.flush();
    EXPECT_TRUE(first_claimed);
    EXPECT_FALSE(second_claimed);
    ASSERT_EQ(sequences(sink->lines_)[1].size(), 1u);
}

TEST_F(ThreadRingTest, ReinitStartsFreshRings) {
    auto& logger = Logger::instance();
    for (int run = 0; run < 3; ++run) {
        auto sink = std::make_shared<CaptureSink>();
        LogConfig config = ring_config(sink);
        config.thread_ring_size = size_t{4096} << run; // a different size each run
        logger.init(config);
        for (int i = 0; i < 20; ++i) {
            LOG_INFO("t0 {} {}", i, std::string("run"));
        }
        logger.flush();
        ASSERT_EQ(sequences(sink->lines_)[0].size(), 20u) << "run " << run;
        logger.shutdown();
    }
}
