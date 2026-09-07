// Producer half of the multi-process logging tests.
//
// Attaches to a shared-memory log queue as a QueueMode::SharedProducer logger
// (no sinks, no writer thread) and emits a fixed set of messages. The collector
// lives in the test process, which launches this executable and then asserts on
// what landed in its sink.

#include <slick/logger.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

namespace {

int usage() {
    std::fprintf(stderr,
                 "usage: producer --name <shm-name> [--tag <tag>] [--count <n>]\n"
                 "                [--queue-size <n>] [--string-buffer-size <n>]\n"
                 "                [--linger-ms <n>]\n");
    return 2;
}

} // namespace

int main(int argc, char** argv) {
    std::string name;
    std::string tag;
    int count = 10;
    size_t queue_size = 1024;
    size_t string_buffer_size = 1 << 16;
    int linger_ms = 0;

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
        } else if (arg == "--linger-ms" && has_value) {
            // Keep the process (and therefore the shared mapping) alive a while
            // longer, so a collector that starts second still finds the segment.
            linger_ms = std::atoi(argv[++i]);
        } else {
            return usage();
        }
    }

    if (name.empty()) {
        return usage();
    }

    try {
        slick::logger::LogConfig config;
        config.mode = slick::logger::QueueMode::SharedProducer;
        config.shared_memory_name = name;
        config.process_tag = tag;
        config.log_queue_size = queue_size;
        config.string_buffer_size = string_buffer_size;
        slick::logger::Logger::instance().init(config);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "producer init failed: %s\n", e.what());
        return 1;
    }

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

    LOG_WARN("producer done");

    if (linger_ms > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(linger_ms));
    }

    slick::logger::Logger::instance().shutdown();
    return 0;
}
