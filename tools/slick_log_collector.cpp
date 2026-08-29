// slick_log_collector - standalone multi-process log collector.
//
// Attaches to a named shared-memory log queue, owns the sinks, and drains
// entries produced by every process that attached to the same name. Run this
// alongside applications configured with QueueMode::SharedProducer.
//
// Built only when BUILD_SLICK_LOGGER_COLLECTOR=ON.

#include <slick/logger.hpp>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>
#include <thread>

namespace {

std::atomic<bool> g_stop{false};

extern "C" void handle_signal(int) {
    g_stop.store(true, std::memory_order_release);
}

int usage() {
    std::fprintf(stderr,
        "slick_log_collector - collect log entries from other processes\n"
        "\n"
        "usage: slick_log_collector --name <shm-name> [sinks] [options]\n"
        "\n"
        "required:\n"
        "  --name <shm-name>            shared memory segment name, 1-24 chars of [A-Za-z0-9_].\n"
        "                               Must match shared_memory_name in the producers.\n"
        "\n"
        "sinks (at least one required):\n"
        "  --console                    write to stdout/stderr with colors\n"
        "  --file <path>                write to a single file\n"
        "  --rotating <path>            write to a size-rotated file set\n"
        "  --daily <path>               write to a date-rotated file set\n"
        "\n"
        "options:\n"
        "  --max-size <bytes>           rotation size threshold (default 10485760)\n"
        "  --max-files <n>              rotated files to keep (default 5)\n"
        "  --level <name>               trace|debug|info|warn|error|fatal (default trace)\n"
        "  --queue-size <n>             entry ring capacity, must match producers (default 65536)\n"
        "  --string-buffer-size <n>     string ring capacity, must match producers (default 16777216)\n"
        "  --stall-timeout-ms <n>       abandon an unpublished slot after this long, 0 disables\n"
        "                               (default 5000)\n");
    return 2;
}

} // namespace

int main(int argc, char** argv) {
    std::string name;
    std::string level = "trace";
    std::string file_path;
    std::string rotating_path;
    std::string daily_path;
    bool console = false;
    size_t max_size = 10 * 1024 * 1024;
    size_t max_files = 5;
    size_t queue_size = 65536;
    size_t string_buffer_size = 1 << 24;
    uint32_t stall_timeout_ms = 5000;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const bool has_value = (i + 1) < argc;

        if (arg == "--console") {
            console = true;
        } else if (arg == "--help" || arg == "-h") {
            return usage();
        } else if (!has_value) {
            std::fprintf(stderr, "missing value for %s\n\n", arg.c_str());
            return usage();
        } else if (arg == "--name") {
            name = argv[++i];
        } else if (arg == "--file") {
            file_path = argv[++i];
        } else if (arg == "--rotating") {
            rotating_path = argv[++i];
        } else if (arg == "--daily") {
            daily_path = argv[++i];
        } else if (arg == "--level") {
            level = argv[++i];
        } else if (arg == "--max-size") {
            max_size = static_cast<size_t>(std::atoll(argv[++i]));
        } else if (arg == "--max-files") {
            max_files = static_cast<size_t>(std::atoll(argv[++i]));
        } else if (arg == "--queue-size") {
            queue_size = static_cast<size_t>(std::atoll(argv[++i]));
        } else if (arg == "--string-buffer-size") {
            string_buffer_size = static_cast<size_t>(std::atoll(argv[++i]));
        } else if (arg == "--stall-timeout-ms") {
            stall_timeout_ms = static_cast<uint32_t>(std::atoll(argv[++i]));
        } else {
            std::fprintf(stderr, "unknown option %s\n\n", arg.c_str());
            return usage();
        }
    }

    if (name.empty()) {
        std::fprintf(stderr, "--name is required\n\n");
        return usage();
    }
    if (!console && file_path.empty() && rotating_path.empty() && daily_path.empty()) {
        std::fprintf(stderr, "at least one sink is required\n\n");
        return usage();
    }

    slick::logger::RotationConfig rotation;
    rotation.max_file_size = max_size;
    rotation.max_files = max_files;

    slick::logger::LogConfig config;
    config.mode = slick::logger::QueueMode::SharedCollector;
    config.shared_memory_name = name;
    config.log_queue_size = queue_size;
    config.string_buffer_size = string_buffer_size;
    config.stalled_entry_timeout_ms = stall_timeout_ms;

    try {
        config.min_level = slick::logger::to_log_level(level);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "invalid --level '%s': %s\n", level.c_str(), e.what());
        return 2;
    }

    try {
        if (console) {
            config.sinks.push_back(std::make_shared<slick::logger::ConsoleSink>());
        }
        if (!file_path.empty()) {
            config.sinks.push_back(std::make_shared<slick::logger::FileSink>(file_path));
        }
        if (!rotating_path.empty()) {
            config.sinks.push_back(std::make_shared<slick::logger::RotatingFileSink>(rotating_path, rotation));
        }
        if (!daily_path.empty()) {
            config.sinks.push_back(std::make_shared<slick::logger::DailyFileSink>(daily_path, rotation));
        }

        slick::logger::Logger::instance().init(config);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "failed to start collector: %s\n", e.what());
        return 1;
    }

    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    std::fprintf(stderr, "slick_log_collector attached to '%s'. Press Ctrl-C to stop.\n", name.c_str());

    while (!g_stop.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // shutdown() drains whatever is still queued before closing the sinks.
    std::fprintf(stderr, "\nslick_log_collector draining and exiting.\n");
    slick::logger::Logger::instance().shutdown();
    return 0;
}
