// Builds the logger with both queue-tuning switches flipped from their defaults:
// loss detection on, CAS backoff off.
//
// This must be its own executable. The switches select the traits of the queues
// Logger holds, so a TU built with them linked against one built without them
// would be two different definitions of Logger - an ODR violation, not a test.
#define SLICK_LOGGER_ENABLE_LOSS_DETECTION 1
#define SLICK_LOGGER_ENABLE_CPU_RELAX 0

#include <slick/logger.hpp>
#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace slick::logger;

static_assert(kLossDetectionEnabled, "SLICK_LOGGER_ENABLE_LOSS_DETECTION=1 must turn loss counting on");
static_assert(detail::logger_queue_traits::enable_loss_detection);
static_assert(!detail::logger_queue_traits::enable_cpu_relax,
              "SLICK_LOGGER_ENABLE_CPU_RELAX=0 must turn the CAS backoff off");
static_assert(!detail::logger_queue_traits::enable_read_last,
              "read_last stays off whatever the switches say: it changes the shared segment");

namespace {

constexpr uint32_t kQueueSize = 1024;

/// Counts what reaches it and, once armed, parks the writer thread inside write()
/// until released - which is what lets producers lap the queue on purpose.
class GateSink : public ISink {
public:
    void write(const LogEntry&) override {
        if (armed_.exchange(false, std::memory_order_acq_rel)) {
            parked_.store(true, std::memory_order_release);
            while (!released_.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        written_.fetch_add(1, std::memory_order_relaxed);
    }
    void flush() override {}

    void arm() noexcept { armed_.store(true, std::memory_order_release); }
    void release() noexcept { released_.store(true, std::memory_order_release); }
    uint64_t written() const noexcept { return written_.load(std::memory_order_relaxed); }

    /// Bounded, so a writer that never reaches the sink fails the test instead of hanging it.
    [[nodiscard]] bool wait_until_parked() const {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!parked_.load(std::memory_order_acquire)) {
            if (std::chrono::steady_clock::now() > deadline) {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return true;
    }

private:
    std::atomic<bool> armed_{false};
    std::atomic<bool> parked_{false};
    std::atomic<bool> released_{false};
    std::atomic<uint64_t> written_{0};
};

/// Keeps every formatted message, for checking exactly-once delivery.
class CollectingSink : public ISink {
public:
    void write(const LogEntry& entry) override { messages_.push_back(format_log_message(entry).first); }
    void flush() override {}
    const std::vector<std::string>& messages() const noexcept { return messages_; }

private:
    std::vector<std::string> messages_;  // writer thread only; read after shutdown()
};

LogConfig make_config(std::shared_ptr<ISink> sink, uint32_t queue_size) {
    LogConfig config;
    config.sinks.push_back(std::move(sink));
    config.log_queue_size = queue_size;
    config.enable_stats = true;
    config.stats_interval_ms = 20;
    config.stats_sample_interval_ms = 1;
    return config;
}

uint64_t now_ns() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

/// A sample published at or after @p after_ns, so it reflects everything before it.
LogStats wait_for_sample_after(uint64_t after_ns) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
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

} // namespace

class QueueTraitsTest : public ::testing::Test {
protected:
    void TearDown() override {
        Logger::instance().shutdown();
        Logger::instance().reset();
    }
};

TEST_F(QueueTraitsTest, OverwrittenEntriesAreCounted) {
    auto sink = std::make_shared<GateSink>();
    Logger::instance().init(make_config(sink, kQueueSize));

    sink->arm();
    LOG_INFO("park the writer");
    ASSERT_TRUE(sink->wait_until_parked()) << "the writer thread never reached the sink";

    // Four laps of the ring while nothing is being read.
    constexpr int kFlood = 4 * static_cast<int>(kQueueSize);
    for (int i = 0; i < kFlood; ++i) {
        LOG_INFO("flood");
    }

    sink->release();
    Logger::instance().flush();
    const LogStats stats = wait_for_sample_after(now_ns());

    ASSERT_NE(stats.timestamp_ns, 0u) << "no statistics sample was published";
    EXPECT_GE(stats.entries_produced, static_cast<uint64_t>(kFlood) + 1);
    // Everything but the last lap was overwritten before the writer got back to it.
    EXPECT_GE(stats.entry_loss_count, static_cast<uint64_t>(kFlood) - kQueueSize);
    // Every entry produced was either written or counted as lost - none vanish.
    EXPECT_EQ(sink->written() + stats.entry_loss_count, stats.entries_produced)
        << "written " << sink->written() << ", lost " << stats.entry_loss_count;
    EXPECT_EQ(stats.string_loss_count, 0u) << "the string ring is never read(), so never counts";
}

TEST_F(QueueTraitsTest, NoOverrunMeansNoLoss) {
    auto sink = std::make_shared<GateSink>();
    Logger::instance().init(make_config(sink, kQueueSize));

    for (int i = 0; i < 100; ++i) {
        LOG_INFO("steady {}", i);
    }
    Logger::instance().flush();
    const LogStats stats = wait_for_sample_after(now_ns());

    ASSERT_NE(stats.timestamp_ns, 0u) << "no statistics sample was published";
    EXPECT_EQ(stats.entry_loss_count, 0u);
    EXPECT_EQ(sink->written(), stats.entries_produced);
}

// With the backoff off, a producer that loses the reserve() race retries at once.
// Contended producers must still each get their own slot: every entry arrives
// exactly once.
TEST_F(QueueTraitsTest, ContendedProducersWithoutBackoffDeliverEveryEntryOnce) {
    constexpr int kThreads = 8;
    constexpr int kPerThread = 4000;
    auto sink = std::make_shared<CollectingSink>();
    // Large enough that nothing is overwritten: this is about slot claiming.
    Logger::instance().init(make_config(sink, 1u << 16));

    std::atomic<bool> go{false};
    std::vector<std::thread> producers;
    for (int t = 0; t < kThreads; ++t) {
        producers.emplace_back([t, &go] {
            while (!go.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            for (int i = 0; i < kPerThread; ++i) {
                LOG_INFO("{}:{}", t, i);
            }
        });
    }
    go.store(true, std::memory_order_release);
    for (auto& producer : producers) {
        producer.join();
    }
    Logger::instance().flush();
    EXPECT_EQ(wait_for_sample_after(now_ns()).entry_loss_count, 0u);
    Logger::instance().shutdown();

    std::set<std::string> seen;
    size_t duplicates = 0;
    for (const std::string& message : sink->messages()) {
        if (message.find("SlickLogger v") != std::string::npos) {
            continue;  // the start-up banner
        }
        if (!seen.insert(message).second) {
            ++duplicates;
        }
    }
    EXPECT_EQ(duplicates, 0u);
    EXPECT_EQ(seen.size(), static_cast<size_t>(kThreads) * kPerThread);
}
