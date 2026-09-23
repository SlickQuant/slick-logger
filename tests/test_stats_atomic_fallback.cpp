// Compiles the statistics publish against the NO-P0718 branch of
// detail::atomic_shared_ptr, whatever this toolchain actually provides.
//
// libc++ ships no std::atomic<std::shared_ptr<T>>, and naming that type there
// instantiates the primary std::atomic template, which hard-errors on its
// is_trivially_copyable mandate. That broke every macOS build of the header
// while Windows and Linux stayed green, so the fallback needs a home that
// compiles it everywhere rather than only on the platform that selects it.
//
// This must be its own executable. The macro changes the type of
// Logger::stats_latest_, so a TU built with it linked against one built without
// it would be two different definitions of Logger - an ODR violation, not a test.
#define SLICK_LOGGER_HAS_ATOMIC_SHARED_PTR 0

#include <slick/logger.hpp>
#include <gtest/gtest.h>
#include <chrono>
#include <filesystem>
#include <memory>
#include <thread>

using namespace slick::logger;

namespace {
constexpr const char* kLogFile = "test_stats_fallback.log";
} // namespace

class SlickLoggerStatsFallbackTest : public ::testing::Test {
protected:
    void TearDown() override {
        Logger::instance().shutdown();
        Logger::instance().reset();
        std::filesystem::remove(kLogFile);
    }
};

// The branch under test is selected by the macro above, not by the platform.
TEST_F(SlickLoggerStatsFallbackTest, FallbackBranchIsTheOneCompiled) {
    ASSERT_EQ(SLICK_LOGGER_HAS_ATOMIC_SHARED_PTR, 0);
}

TEST_F(SlickLoggerStatsFallbackTest, LoadStoreRoundTripsThroughFallback) {
    detail::atomic_shared_ptr<const LogStats> slot;
    EXPECT_EQ(slot.load(std::memory_order_acquire), nullptr);

    auto published = std::make_shared<const LogStats>();
    slot.store(published, std::memory_order_release);

    const auto observed = slot.load(std::memory_order_acquire);
    ASSERT_NE(observed, nullptr);
    EXPECT_EQ(observed.get(), published.get()) << "the stored sample is what a reader sees";
    EXPECT_EQ(observed.use_count(), 3) << "a reader holds its referent alive";

    slot.store(nullptr, std::memory_order_release);
    EXPECT_EQ(slot.load(std::memory_order_acquire), nullptr);
    EXPECT_NE(observed, nullptr) << "a retired sample outlives the slot that held it";
}

// The end-to-end path: the statistics thread publishes through the fallback and
// stats_snapshot() reads back through it.
TEST_F(SlickLoggerStatsFallbackTest, SnapshotSeesPublishedSample) {
    LogConfig config;
    config.sinks.push_back(std::make_shared<FileSink>(kLogFile));
    config.enable_stats = true;
    config.stats_interval_ms = 20;
    config.stats_sample_interval_ms = 1;
    Logger::instance().init(config);

    for (int i = 0; i < 64; ++i) {
        LOG_INFO("fallback publish {}", i);
    }
    Logger::instance().flush();

    LogStats stats{};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        stats = Logger::instance().stats_snapshot();
        if (stats.timestamp_ns != 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    ASSERT_NE(stats.timestamp_ns, 0u) << "no sample was ever published through the fallback";
    EXPECT_GE(stats.entries_produced, 64u);
    EXPECT_GT(stats.entry_queue_capacity, 0u);
}

// A logger with statistics off publishes nothing, so the slot stays empty and
// the documented zeroed LogStats has to come back out of the fallback.
TEST_F(SlickLoggerStatsFallbackTest, EmptySlotYieldsZeroedStats) {
    LogConfig config;
    config.sinks.push_back(std::make_shared<FileSink>(kLogFile));
    ASSERT_FALSE(config.enable_stats);
    Logger::instance().init(config);

    LOG_INFO("no statistics here");
    Logger::instance().flush();

    const auto stats = Logger::instance().stats_snapshot();
    EXPECT_EQ(stats.timestamp_ns, 0u);
    EXPECT_EQ(stats.sample_count, 0u);
    EXPECT_EQ(stats.entries_produced, 0u);
}
