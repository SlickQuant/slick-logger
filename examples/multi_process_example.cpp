// Multi-process logging example.
//
// One process acts as the collector: it owns the sinks and the writer thread and
// drains a shared-memory queue. Any number of producer processes attach to the
// same segment by name and only enqueue entries.
//
//   Terminal 1:  multi_process_example --collector --name demo_log
//   Terminal 2:  multi_process_example --producer  --name demo_log --tag app1
//   Terminal 3:  multi_process_example --producer  --name demo_log --tag app2
//
// Startup order does not matter: whichever process gets there first creates the
// segment, and a collector attaching later picks up whatever is still buffered.

#include <slick/logger.hpp>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <string>
#include <thread>

using namespace slick::logger;

namespace {

std::atomic<bool> g_stop{false};

extern "C" void handle_signal(int) {
    g_stop.store(true, std::memory_order_release);
}

int usage() {
    std::fprintf(stderr,
        "usage: multi_process_example --collector --name <shm-name>\n"
        "       multi_process_example --producer  --name <shm-name> [--tag <tag>] [--count <n>]\n");
    return 2;
}

// Both roles must agree on these, since they size the shared segments.
constexpr size_t kQueueSize = 4096;
constexpr size_t kStringBufferSize = 1 << 20;

int run_collector(const std::string& name) {
    LogConfig config;
    config.mode = QueueMode::SharedCollector;
    config.shared_memory_name = name;
    config.process_tag = "collector";
    config.log_queue_size = kQueueSize;
    config.string_buffer_size = kStringBufferSize;
    config.sinks.push_back(std::make_shared<ConsoleSink>());
    config.sinks.push_back(std::make_shared<FileSink>("collected.log"));

    Logger::instance().init(config);

    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);
    std::printf("Collecting into collected.log from segment '%s'. Press Ctrl-C to stop.\n", name.c_str());

    while (!g_stop.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // Drains everything still queued before closing the sinks.
    Logger::instance().shutdown();
    return 0;
}

int run_producer(const std::string& name, const std::string& tag, int count) {
    LogConfig config;
    config.mode = QueueMode::SharedProducer;
    config.shared_memory_name = name;
    config.process_tag = tag;   // shows up as [pid:tag] in the collector's output
    config.log_queue_size = kQueueSize;
    config.string_buffer_size = kStringBufferSize;
    // No sinks: the collector owns those.

    Logger::instance().init(config);

    for (int i = 0; i < count; ++i) {
        LOG_INFO("work item {} of {}", i + 1, count);
        if (i % 5 == 4) {
            LOG_WARN("checkpoint after {} items from {}", i + 1, tag);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    LOG_INFO("producer {} finished", tag);

    Logger::instance().shutdown();
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    bool collector = false;
    bool producer = false;
    std::string name;
    std::string tag = "producer";
    int count = 20;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const bool has_value = (i + 1) < argc;
        if (arg == "--collector") {
            collector = true;
        } else if (arg == "--producer") {
            producer = true;
        } else if (arg == "--name" && has_value) {
            name = argv[++i];
        } else if (arg == "--tag" && has_value) {
            tag = argv[++i];
        } else if (arg == "--count" && has_value) {
            count = std::atoi(argv[++i]);
        } else {
            return usage();
        }
    }

    if (name.empty() || collector == producer) {
        return usage();
    }

    try {
        return collector ? run_collector(name) : run_producer(name, tag, count);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
