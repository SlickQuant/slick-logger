// Producer half of the multi-process logging tests.
//
// Attaches to a shared-memory log queue as a QueueMode::SharedProducer logger
// (no sinks, no writer thread) and emits a fixed set of messages. The collector
// lives in the test process, which launches this executable and then asserts on
// what landed in its sink.
//
// Steps (--log, --wait, --touch, --flush, --crash) run in the order given and
// replace the default messages; --threads replaces them with per-thread
// sequences. --collector runs as a second collector instead.

#include <slick/logger.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
extern "C" __declspec(dllimport) void* __stdcall GetCurrentProcess(void);
extern "C" __declspec(dllimport) int __stdcall TerminateProcess(void*, unsigned int);
#else
#  include <signal.h>
#  include <unistd.h>
#endif

namespace {

int usage() {
    std::fprintf(stderr,
                 "usage: producer --name <shm-name> [--tag <tag>] [--count <n>]\n"
                 "                [--queue-size <n>] [--string-buffer-size <n>]\n"
                 "                [--string-items-per-slot <n>] [--thread-rings 0|1]\n"
                 "                [--rings <n>] [--ring-size <bytes>] [--threads <n>]\n"
                 "                [--pace-us <n>] [--crash-after <n>] [--flush-first]\n"
                 "                [--log <label> <n>] [--big <bytes>] [--wait <file>] [--touch <file>]\n"
                 "                [--flush] [--crash] [--collector <log>] [--stall-ms <n>]\n"
                 "                [--linger-ms <n>] [--ready-file <path>]\n");
    return 2;
}

/// Killed, not exited: no shutdown(), no destructors, other threads stopped wherever they are.
[[noreturn]] void crash() {
#ifdef _WIN32
    ::TerminateProcess(::GetCurrentProcess(), 9);
#else
    ::kill(::getpid(), SIGKILL);
#endif
    std::abort();
}

void touch(const std::string& path) {
    if (std::FILE* marker = std::fopen(path.c_str(), "wb")) {
        std::fputs("ready\n", marker);
        std::fclose(marker);
    }
}

bool wait_file(const std::string& path) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!std::filesystem::exists(path)) {
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return true;
}

struct step {
    enum kind_t { Log, Big, Wait, Touch, Flush, Crash } kind;
    std::string text;
    int count = 0;
};

} // namespace

int main(int argc, char** argv) {
    std::string name;
    std::string tag;
    int count = 10;
    size_t queue_size = 1024;
    size_t string_buffer_size = 1 << 16;
    uint32_t string_items_per_slot = slick::logger::kDefaultStringItemsPerSlot;
    int linger_ms = 0;
    std::string ready_file;
    int thread_rings = 1; // on whatever the build default, so the tests do not depend on it
    uint32_t rings = 0;
    size_t ring_size = 0;
    int threads = 0;
    long long crash_after = 0;
    int pace_us = 0;
    bool flush_first = false;
    std::string collector_log;
    int stall_ms = -1;
    std::vector<step> steps;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const bool has_value = (i + 1) < argc;
        if (arg == "--name" && has_value) {
            name = argv[++i];
        } else if (arg == "--tag" && has_value) {
            tag = argv[++i];
        } else if (arg == "--count" && has_value) {
            count = std::atoi(argv[++i]);
        } else if (arg == "--queue-size" && has_value) {
            queue_size = static_cast<size_t>(std::atoll(argv[++i]));
        } else if (arg == "--string-buffer-size" && has_value) {
            string_buffer_size = static_cast<size_t>(std::atoll(argv[++i]));
        } else if (arg == "--string-items-per-slot" && has_value) {
            // Only honoured when this process creates the segment; an attacher
            // adopts whatever the creator chose.
            string_items_per_slot = static_cast<uint32_t>(std::atoll(argv[++i]));
        } else if (arg == "--ready-file" && has_value) {
            // Touched once every entry above is published, so a collector that
            // must attach to a segment that already holds a backlog can wait on a
            // fact instead of guessing at a sleep.
            ready_file = argv[++i];
        } else if (arg == "--linger-ms" && has_value) {
            // Keep the process (and therefore the shared mapping) alive a while
            // longer, so a collector that starts second still finds the segment.
            linger_ms = std::atoi(argv[++i]);
        } else if (arg == "--thread-rings" && has_value) {
            thread_rings = std::atoi(argv[++i]);
        } else if (arg == "--rings" && has_value) {
            rings = static_cast<uint32_t>(std::atoi(argv[++i]));
        } else if (arg == "--ring-size" && has_value) {
            ring_size = static_cast<size_t>(std::atoll(argv[++i]));
        } else if (arg == "--threads" && has_value) {
            threads = std::atoi(argv[++i]);
        } else if (arg == "--pace-us" && has_value) {
            pace_us = std::atoi(argv[++i]); // between a thread's entries, so the collector keeps up
        } else if (arg == "--crash-after" && has_value) {
            crash_after = std::atoll(argv[++i]);
        } else if (arg == "--flush-first") {
            flush_first = true;
        } else if (arg == "--collector" && has_value) {
            collector_log = argv[++i];
        } else if (arg == "--stall-ms" && has_value) {
            stall_ms = std::atoi(argv[++i]); // stalled_entry_timeout_ms for --collector
        } else if (arg == "--log" && i + 2 < argc) {
            steps.push_back({step::Log, argv[i + 1], std::atoi(argv[i + 2])});
            i += 2;
        } else if (arg == "--big" && has_value) {
            steps.push_back({step::Big, {}, std::atoi(argv[++i])}); // one entry with a string this long
        } else if (arg == "--wait" && has_value) {
            steps.push_back({step::Wait, argv[++i]});
        } else if (arg == "--touch" && has_value) {
            steps.push_back({step::Touch, argv[++i]});
        } else if (arg == "--flush") {
            steps.push_back({step::Flush, {}});
        } else if (arg == "--crash") {
            steps.push_back({step::Crash, {}});
        } else {
            return usage();
        }
    }

    if (name.empty()) {
        return usage();
    }

    auto& logger = slick::logger::Logger::instance();
    try {
        slick::logger::LogConfig config;
        config.mode = collector_log.empty() ? slick::logger::QueueMode::SharedProducer
                                            : slick::logger::QueueMode::SharedCollector;
        config.shared_memory_name = name;
        config.process_tag = tag;
        config.log_queue_size = queue_size;
        config.string_buffer_size = string_buffer_size;
        config.string_items_per_slot = string_items_per_slot;
        if (thread_rings >= 0) {
            config.thread_rings = thread_rings != 0;
        }
        if (rings != 0) {
            config.shared_thread_rings = rings;
        }
        if (ring_size != 0) {
            config.shared_thread_ring_size = ring_size;
        }
        if (!collector_log.empty()) {
            config.sinks.push_back(std::make_shared<slick::logger::FileSink>(collector_log));
        }
        if (stall_ms >= 0) {
            config.stalled_entry_timeout_ms = static_cast<uint32_t>(stall_ms);
        }
        logger.init(config);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "producer init failed: %s\n", e.what());
        return 3;
    }

    if (flush_first) {
        // Returns once the collector has taken it, so it is attached from here on.
        LOG_INFO("producer hello from {}", tag);
        logger.flush();
    }

    for (const step& s : steps) {
        switch (s.kind) {
        case step::Log:
            for (int i = 0; i < s.count; ++i) {
                LOG_INFO("step {} {} from {}", s.text, i, tag);
            }
            break;
        case step::Big:
            LOG_INFO("big {} from {}", std::string(static_cast<size_t>(s.count), 'x'), tag);
            break;
        case step::Wait:
            if (!wait_file(s.text)) {
                std::fprintf(stderr, "producer timed out waiting for %s\n", s.text.c_str());
                return 4;
            }
            break;
        case step::Touch:
            touch(s.text);
            break;
        case step::Flush:
            logger.flush();
            break;
        case step::Crash:
            crash();
        }
    }

    if (threads > 0) {
        // Each thread numbers its entries, so the collector can check every
        // thread's order and that nothing in the middle went missing.
        std::atomic<long long> logged{0};
        std::vector<std::thread> workers;
        for (int t = 0; t < threads; ++t) {
            workers.emplace_back([&, t] {
                for (int i = 0; i < count; ++i) {
                    LOG_INFO("seq {} {} {}", tag, t, i);
                    logged.fetch_add(1, std::memory_order_relaxed);
                    const auto until = std::chrono::steady_clock::now() + std::chrono::microseconds(pace_us);
                    while (pace_us > 0 && std::chrono::steady_clock::now() < until) {
                    }
                }
            });
        }
        if (crash_after > 0) {
            while (logged.load(std::memory_order_relaxed) < crash_after) {
                std::this_thread::yield();
            }
            crash(); // mid-burst: the workers are still logging
        }
        for (auto& worker : workers) {
            worker.join();
        }
    } else if (steps.empty() && collector_log.empty()) {
        const std::string dynamic_text = "dynamic-" + tag;
        for (int i = 0; i < count; ++i) {
            // Literal format string plus a mix of argument kinds, so the test covers
            // the whole "copy into the shared ring, resolve in the collector" path.
            LOG_INFO("producer message {} of {} from {}", i, count, dynamic_text);
        }

        // A binary payload takes the same "copy into the shared ring" path, so this
        // covers the collector rebasing an ArgType::BLOB offset in its own mapping.
        const unsigned char payload[] = {0x00, 0xDE, 0xAD, 0xBE, 0xEF, 0x0A};
        LOG_INFO("producer payload {}", slick::logger::as_binary(payload, sizeof(payload)));
    }

    if (collector_log.empty()) {
        LOG_WARN("producer done");
    }

    if (!ready_file.empty()) {
        // After the entries, before the linger: everything this producer logs is
        // published by the time the file appears, because a SharedProducer has no
        // writer thread of its own: LOG_* publishes inline.
        touch(ready_file);
    }

    if (linger_ms > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(linger_ms));
    }

    logger.shutdown();
    return 0;
}
