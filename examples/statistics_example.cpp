// Runtime statistics: sampling the logging pipeline while it is under load.
//
// Shows how to turn on the statistics thread, read the live snapshot, and what
// the CSV it writes contains. Deliberately alternates between entries that store
// a string in the ring and entries that do not, so the string_pct_valid flag can
// be seen going both ways.

#include <slick/logger.hpp>

#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>

using namespace slick::logger;

int main() {
    LogConfig config;
    config.sinks.push_back(std::make_shared<FileSink>("statistics_example.log"));
    config.log_queue_size = 4096;
    config.string_buffer_size = 64 * 1024;

    // Statistics are opt-in and cost the logging path nothing: every figure comes
    // from cursors the queues already maintain.
    config.enable_stats = true;
    config.stats_file = "statistics_example.csv";
    config.stats_interval_ms = 200;    // one CSV row every 200 ms
    config.stats_sample_interval_ms = 5; // sample the depth gauges 40x per row
    config.stats_max_file_size = 1 * 1024 * 1024;

    Logger::instance().init(config);

    std::printf("Logging under load; statistics go to statistics_example.csv\n\n");
    std::printf("%8s %10s %10s %11s %8s %7s\n",
                "msgs/s", "queue max%", "queue now%", "str peak B", "str max%", "valid");

    const std::string payload(96, 'x');
    for (int round = 0; round < 10; ++round) {
        // A burst of entries that copy a string into the ring, so the occupancy
        // anchor has something to find...
        for (int i = 0; i < 2000; ++i) {
            LOG_INFO("burst {} payload {}", i, payload);
        }
        // ...then a quieter stretch of entries that store nothing in the ring.
        for (int i = 0; i < 500; ++i) {
            LOG_INFO("tick {}", i);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        const LogStats stats = Logger::instance().stats_snapshot();
        // The _max columns come from the oversampling tick. The "now" column is a
        // single point sample, and a burst that drains between two reports is
        // invisible to it - which is exactly why the peaks are tracked.
        std::printf("%8.0f %10.1f %10.1f %11llu %8.1f %7s\n",
                    stats.produced_per_sec,
                    stats.entry_queue_pct_max,   // the figure to alert on
                    stats.entry_queue_pct,       // a single point sample
                    static_cast<unsigned long long>(stats.string_inflight_bytes_max),
                    stats.string_pct_max,
                    stats.string_pct_valid ? "yes" : "no");
    }

    Logger::instance().flush();
    const LogStats final_stats = Logger::instance().stats_snapshot();
    std::printf("\nProduced %llu entries, consumed %llu, dropped %llu\n",
                static_cast<unsigned long long>(final_stats.entries_produced),
                static_cast<unsigned long long>(final_stats.entries_consumed),
                static_cast<unsigned long long>(final_stats.entry_loss_count));

    Logger::instance().shutdown();
    return 0;
}
