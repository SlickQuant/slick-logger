#include <slick/logger.hpp>
#include <gtest/gtest.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#  include <windows.h>
   using LibHandle = HMODULE;
   inline LibHandle open_lib(const char* path)  { return LoadLibraryA(path); }
   inline void*     get_sym(LibHandle h, const char* sym) { return reinterpret_cast<void*>(GetProcAddress(h, sym)); }
   inline void      close_lib(LibHandle h)      { FreeLibrary(h); }
   inline std::string lib_error()               { return std::to_string(GetLastError()); }
#else
#  include <dlfcn.h>
   using LibHandle = void*;
   inline LibHandle open_lib(const char* path)  { return dlopen(path, RTLD_NOW | RTLD_LOCAL); }
   inline void*     get_sym(LibHandle h, const char* sym) { return dlsym(h, sym); }
   inline void      close_lib(LibHandle h)      { dlclose(h); }
   inline std::string lib_error()               { return dlerror() ? dlerror() : "unknown"; }
#endif

// Resolved at cmake configure time; the build system defines this macro.
#ifndef PLUGIN_LIB_PATH
#  error "PLUGIN_LIB_PATH must be defined by CMake"
#endif

using PluginInitFn     = void(*)(slick::logger::Logger*);
using PluginLogFn      = void(*)();
using PluginLocationProbeFn = int(*)();
using PluginShutdownFn = void(*)();

static std::string read_log(const std::filesystem::path& path) {
    std::ifstream f(path);
    std::string line, contents;
    std::getline(f, line); // skip version line written by Logger on init
    while (std::getline(f, line))
        contents += line + "\n";
    return contents;
}

class SharedLibraryTest : public ::testing::Test {
protected:
    void TearDown() override {
        slick::logger::Logger::instance().shutdown();
        std::filesystem::remove("test_shared_lib_host.log");
    }
};

// The plugin is a real shared library (.dll/.so) compiled separately.
// Verify that after plugin_init() all LOG_* calls inside the plugin appear
// in the host's log file.
TEST_F(SharedLibraryTest, PluginLogsRouteToHostLogger) {
    std::filesystem::remove("test_shared_lib_host.log");

    // --- Host setup ---
    slick::logger::Logger::instance().add_file_sink("test_shared_lib_host.log");
    slick::logger::Logger::instance().init(1024);

    // --- Load plugin ---
    LibHandle handle = open_lib(PLUGIN_LIB_PATH);
    ASSERT_NE(handle, LibHandle{}) << "Failed to load plugin: " << lib_error();

    auto plugin_init     = reinterpret_cast<PluginInitFn>    (get_sym(handle, "plugin_init"));
    auto plugin_log      = reinterpret_cast<PluginLogFn>     (get_sym(handle, "plugin_log_messages"));
    auto plugin_probe    = reinterpret_cast<PluginLocationProbeFn>(get_sym(handle, "plugin_log_location_probe"));
    auto plugin_shutdown = reinterpret_cast<PluginShutdownFn>(get_sym(handle, "plugin_shutdown"));

    ASSERT_NE(plugin_init,     nullptr) << "plugin_init not found";
    ASSERT_NE(plugin_log,      nullptr) << "plugin_log_messages not found";
    ASSERT_NE(plugin_probe,    nullptr) << "plugin_log_location_probe not found";
    ASSERT_NE(plugin_shutdown, nullptr) << "plugin_shutdown not found";

    // --- Simulate plugin lifecycle ---
    plugin_init(&slick::logger::Logger::instance());
    plugin_log();
    const int plugin_probe_line = plugin_probe();
    plugin_shutdown();

    // Drain the host logger's queue before unloading the plugin.
    // LOG_* with string literals stores a raw pointer to the format string,
    // which lives in the plugin's code segment. flush() blocks until the writer
    // thread has consumed all queued entries, so close_lib() can safely unmap
    // the plugin's memory without leaving dangling format-string pointers.
    // The host logger keeps running, so logging can continue after this call.
    slick::logger::Logger::instance().flush();

    close_lib(handle);

    ASSERT_TRUE(std::filesystem::exists("test_shared_lib_host.log"));
    std::string contents = read_log("test_shared_lib_host.log");

    // All messages logged from inside the plugin must appear in the host log.
    EXPECT_NE(contents.find("plugin_init: logger connected to host"), std::string::npos);
    EXPECT_NE(contents.find("plugin debug message"),  std::string::npos);
    EXPECT_NE(contents.find("plugin info message"),   std::string::npos);
    EXPECT_NE(contents.find("plugin warn message"),   std::string::npos);
    EXPECT_NE(contents.find("plugin error message"),  std::string::npos);
    EXPECT_NE(contents.find("plugin source location probe"), std::string::npos);
    EXPECT_NE(contents.find("plugin.cpp:" + std::to_string(plugin_probe_line)), std::string::npos);
}

// After plugin_shutdown() clears the override, further LOG_* calls from the
// host still work normally (the host logger is unaffected by the plugin).
TEST_F(SharedLibraryTest, HostLoggerUnaffectedAfterPluginUnload) {
    std::filesystem::remove("test_shared_lib_host.log");

    slick::logger::Logger::instance().add_file_sink("test_shared_lib_host.log");
    slick::logger::Logger::instance().init(1024);

    LibHandle handle = open_lib(PLUGIN_LIB_PATH);
    ASSERT_NE(handle, LibHandle{}) << "Failed to load plugin: " << lib_error();

    auto plugin_init     = reinterpret_cast<PluginInitFn>    (get_sym(handle, "plugin_init"));
    auto plugin_shutdown = reinterpret_cast<PluginShutdownFn>(get_sym(handle, "plugin_shutdown"));

    ASSERT_NE(plugin_init,     nullptr);
    ASSERT_NE(plugin_shutdown, nullptr);

    plugin_init(&slick::logger::Logger::instance());
    plugin_shutdown();

    // Drain before unloading, then close the library.
    // The host logger remains running so we can log after unload.
    slick::logger::Logger::instance().flush();
    close_lib(handle);

    // Host logs after unload must still work.
    LOG_INFO("host message after plugin unload");
    slick::logger::Logger::instance().shutdown();

    std::string contents = read_log("test_shared_lib_host.log");
    EXPECT_NE(contents.find("host message after plugin unload"), std::string::npos);
}

namespace {

// Keeps every message. lines_ belongs to the writer thread until flush() returns.
class CaptureSink : public slick::logger::ISink {
public:
    CaptureSink() : ISink("capture") {}
    void write(const slick::logger::LogEntry& entry) override {
        lines_.emplace_back(formatted_message(entry).first);
    }
    void flush() override {}
    std::vector<std::string> lines_;
};

slick::logger::LogConfig ring_config(std::shared_ptr<CaptureSink> sink) {
    slick::logger::LogConfig config;
    config.sinks = {std::move(sink)};
    config.thread_rings = true;
    config.enable_stats = true;
    config.stats_interval_ms = 10;
    config.stats_sample_interval_ms = 1;
    return config;
}

// "t<thread> <seq> ..." messages by thread, in arrival order.
std::map<int, std::vector<int>> sequences(const std::vector<std::string>& lines) {
    std::map<int, std::vector<int>> by_thread;
    for (const auto& line : lines) {
        int thread = 0, seq = 0;
        if (std::sscanf(line.c_str(), "t%d %d", &thread, &seq) == 2) {
            by_thread[thread].push_back(seq);
        }
    }
    return by_thread;
}

} // namespace

// Each module has its own thread_local, but a thread logging from the host and
// from a plugin sharing its Logger must still get one ring: two would order its
// entries by timestamp alone, and equal timestamps are common.
TEST_F(SharedLibraryTest, HostAndPluginShareOneRingPerThread) {
    auto& logger = slick::logger::Logger::instance();
    auto sink = std::make_shared<CaptureSink>();
    logger.init(ring_config(sink)); // the banner gives this thread its ring

    LibHandle handle = open_lib(PLUGIN_LIB_PATH);
    ASSERT_NE(handle, LibHandle{}) << "Failed to load plugin: " << lib_error();
    auto plugin_init     = reinterpret_cast<PluginInitFn>    (get_sym(handle, "plugin_init"));
    auto plugin_log      = reinterpret_cast<PluginLogFn>     (get_sym(handle, "plugin_log_messages"));
    auto plugin_shutdown = reinterpret_cast<PluginShutdownFn>(get_sym(handle, "plugin_shutdown"));
    ASSERT_TRUE(plugin_init && plugin_log && plugin_shutdown);

    LOG_INFO("host before");
    plugin_init(&logger);
    plugin_log();
    LOG_INFO("host after");
    plugin_shutdown();
    logger.flush();
    close_lib(handle);

    const std::vector<std::string> expected = {
        "host before", "plugin_init: logger connected to host", "plugin debug message",
        "plugin info message", "plugin warn message", "plugin error message", "host after"};
    ASSERT_EQ(sink->lines_.size(), expected.size() + 1); // and the banner
    for (size_t i = 0; i < expected.size(); ++i) {
        EXPECT_EQ(sink->lines_[i + 1], expected[i]);
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (logger.stats_snapshot().entries_produced < sink->lines_.size() &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_EQ(logger.stats_snapshot().thread_ring_count, 1u);
}

// A plugin that claimed a thread's ring and is then unloaded runs its exit hook
// on that thread, which lives on. The thread must stop using the ring once the
// hook has offered it to others, or two threads would write one ring.
TEST_F(SharedLibraryTest, RingClaimedByAnUnloadedPluginIsNotShared) {
    auto& logger = slick::logger::Logger::instance();
    auto sink = std::make_shared<CaptureSink>();
    auto config = ring_config(sink);
    config.max_thread_rings = 2; // the banner's, and the worker's
    logger.init(config);

    LibHandle handle = open_lib(PLUGIN_LIB_PATH);
    ASSERT_NE(handle, LibHandle{}) << "Failed to load plugin: " << lib_error();
    auto plugin_init     = reinterpret_cast<PluginInitFn>    (get_sym(handle, "plugin_init"));
    auto plugin_shutdown = reinterpret_cast<PluginShutdownFn>(get_sym(handle, "plugin_shutdown"));
    ASSERT_TRUE(plugin_init && plugin_shutdown);

    constexpr int kCount = 20000;
    std::atomic<bool> unloaded{false};
    std::atomic<bool> go{false};
    std::thread worker([&] {
        plugin_init(&logger); // its first entry: the plugin's module claims the ring
        LOG_INFO("worker before unload"); // and the host's module caches it
        plugin_shutdown();
        logger.flush();       // the plugin's format strings must be written before it goes
        close_lib(handle);    // its thread_locals die here, the exit hook included
        unloaded.store(true, std::memory_order_release);
        while (!go.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        for (int i = 0; i < kCount; ++i) {
            LOG_INFO("t1 {} {}", i, std::string("worker"));
        }
    });
    while (!unloaded.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    logger.flush(); // drained: if the hook freed the worker's ring, the next thread can take it
    std::thread other([&] {
        go.store(true, std::memory_order_release);
        for (int i = 0; i < kCount; ++i) {
            LOG_INFO("t2 {} {}", i, std::string("other"));
        }
    });
    worker.join();
    other.join();
    logger.flush();

    const auto by_thread = sequences(sink->lines_);
    ASSERT_EQ(by_thread.size(), 2u);
    for (const auto& [thread, seqs] : by_thread) {
        ASSERT_EQ(seqs.size(), static_cast<size_t>(kCount)) << "thread " << thread;
        for (int i = 0; i < kCount; ++i) {
            ASSERT_EQ(seqs[i], i) << "thread " << thread;
        }
    }
}

namespace {

// Holds the writer inside write() on the "gate" entry until release_ is set.
class GateSink : public CaptureSink {
public:
    void write(const slick::logger::LogEntry& entry) override {
        CaptureSink::write(entry);
        count_.store(lines_.size(), std::memory_order_release);
        if (lines_.back() == "gate") {
            reached_.store(true, std::memory_order_release);
            while (!release_.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
        }
    }
    std::atomic<size_t> count_{0};
    std::atomic<bool> reached_{false};
    std::atomic<bool> release_{false};
};

size_t index_of(const std::vector<std::string>& lines, const std::string& text) {
    return static_cast<size_t>(std::find(lines.begin(), lines.end(), text) - lines.begin());
}

} // namespace

// A thread that found every ring taken stays on the shared rings for the run,
// from its plugins too. A plugin that took a ring freed meanwhile split the
// thread's entries over two sources, and on equal timestamps its first entry
// overtook the host's earlier one.
TEST_F(SharedLibraryTest, PluginKeepsAThreadOnTheSharedRings) {
    auto& logger = slick::logger::Logger::instance();
    auto sink = std::make_shared<GateSink>();
    auto config = ring_config(sink);
    config.max_thread_rings = 2;
    logger.set_timestamper([]() noexcept { return uint64_t{1}; }); // every entry ties

    std::atomic<int> step{0};
    const auto wait_step = [&step](int target) {
        while (step.load(std::memory_order_acquire) < target) {
            std::this_thread::yield();
        }
    };
    std::thread holder([&] { // ring 0, from init()
        logger.init(config);
        step.store(1, std::memory_order_release);
        wait_step(2);
        LOG_INFO("gate");
        wait_step(4);
    });
    wait_step(1);
    std::thread second([&] { // ring 1, free again once it exits
        LOG_INFO("second");
        wait_step(3);
    });
    while (sink->count_.load(std::memory_order_acquire) < 2) { // the banner and "second": ring 1 is drained
        std::this_thread::yield();
    }
    step.store(2, std::memory_order_release);
    while (!sink->reached_.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    LOG_INFO("host first"); // both rings taken: the shared rings
    step.store(3, std::memory_order_release);
    second.join();

    LibHandle handle = open_lib(PLUGIN_LIB_PATH);
    ASSERT_NE(handle, LibHandle{}) << "Failed to load plugin: " << lib_error();
    auto plugin_init     = reinterpret_cast<PluginInitFn>    (get_sym(handle, "plugin_init"));
    auto plugin_shutdown = reinterpret_cast<PluginShutdownFn>(get_sym(handle, "plugin_shutdown"));
    ASSERT_TRUE(plugin_init && plugin_shutdown);
    plugin_init(&logger); // the plugin's first entry on this thread, with ring 1 free
    LOG_INFO("host after");
    plugin_shutdown();
    sink->release_.store(true, std::memory_order_release);
    logger.flush();
    close_lib(handle);
    step.store(4, std::memory_order_release);
    holder.join();
    logger.set_timestamper(nullptr);

    const auto& lines = sink->lines_;
    const size_t first = index_of(lines, "host first");
    const size_t plugin = index_of(lines, "plugin_init: logger connected to host");
    const size_t after = index_of(lines, "host after");
    ASSERT_LT(after, lines.size());
    EXPECT_LT(first, plugin) << "the plugin's entry overtook the host's earlier one";
    EXPECT_LT(plugin, after);
}

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
