#include <iostream>
#include <vector>
#include <chrono>
#include <algorithm>
#include <iomanip>

#include <slick/logger.hpp>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>

#include "benchmark_utils.hpp"
#include "system_monitor.hpp"

using namespace benchmark_utils;

// Detailed latency analysis for logging calls
class LatencyAnalyzer {
public:
    struct LatencyMeasurement {
        uint64_t call_time_ns;
        uint64_t return_time_ns;
        std::string message_type;
        
        uint64_t latency_ns() const { return return_time_ns - call_time_ns; }
    };

    void add_measurement(uint64_t call_time, uint64_t return_time, const std::string& type) {
        measurements_.push_back({call_time, return_time, type});
    }

    void analyze_and_report() {
        if (measurements_.empty()) {
            std::cout << "No measurements recorded!" << std::endl;
            return;
        }

        // Sort by latency
        std::sort(measurements_.begin(), measurements_.end(),
                 [](const LatencyMeasurement& a, const LatencyMeasurement& b) {
                     return a.latency_ns() < b.latency_ns();
                 });

        std::vector<double> latencies;
        for (const auto& m : measurements_) {
            latencies.push_back(static_cast<double>(m.latency_ns()));
        }

        Statistics stats(latencies);
        
        std::cout << "=== DETAILED LATENCY ANALYSIS ===" << std::endl;
        stats.print_summary("Call Latency (ns)");

        // Show distribution
        print_latency_distribution();
        
        // Show timeline analysis
        print_timeline_analysis();
    }

private:
    void print_latency_distribution() {
        std::cout << "Latency Distribution:" << std::endl;
        
        // Create histogram buckets (nanoseconds)
        std::vector<uint64_t> buckets = {0, 100, 500, 1000, 5000, 10000, 50000, 100000, UINT64_MAX};
        std::vector<std::string> labels = {"0-100ns", "100-500ns", "500ns-1μs", "1-5μs", 
                                         "5-10μs", "10-50μs", "50-100μs", ">100μs"};
        std::vector<size_t> counts(buckets.size() - 1, 0);

        for (const auto& m : measurements_) {
            uint64_t latency = m.latency_ns();
            for (size_t i = 0; i < buckets.size() - 1; ++i) {
                if (latency >= buckets[i] && latency < buckets[i + 1]) {
                    counts[i]++;
                    break;
                }
            }
        }

        for (size_t i = 0; i < labels.size(); ++i) {
            double percentage = (counts[i] * 100.0) / measurements_.size();
            std::cout << std::left << std::setw(12) << labels[i] << ": " 
                      << std::right << std::setw(6) << counts[i] 
                      << " (" << std::fixed << std::setprecision(1) 
                      << percentage << "%)" << std::endl;
        }
        std::cout << std::endl;
    }

    void print_timeline_analysis() {
        if (measurements_.size() < 100) return;

        std::cout << "Timeline Analysis (first 100 calls vs last 100 calls):" << std::endl;
        
        // First 100
        std::vector<double> first_100;
        for (size_t i = 0; i < 100; ++i) {
            first_100.push_back(static_cast<double>(measurements_[i].latency_ns()));
        }
        
        // Last 100
        std::vector<double> last_100;
        for (size_t i = measurements_.size() - 100; i < measurements_.size(); ++i) {
            last_100.push_back(static_cast<double>(measurements_[i].latency_ns()));
        }

        Statistics first_stats(first_100);
        Statistics last_stats(last_100);

        std::cout << "First 100 calls - Mean: " << first_stats.mean() 
                  << "ns, P99: " << first_stats.percentile(99) << "ns" << std::endl;
        std::cout << "Last 100 calls  - Mean: " << last_stats.mean() 
                  << "ns, P99: " << last_stats.percentile(99) << "ns" << std::endl;
        
        double improvement = ((first_stats.mean() - last_stats.mean()) / first_stats.mean()) * 100;
        if (improvement > 0) {
            std::cout << "Performance improved by " << improvement << "% (warmup effect)" << std::endl;
        } else {
            std::cout << "Performance degraded by " << -improvement << "% (possible queue pressure)" << std::endl;
        }
        std::cout << std::endl;
    }

    std::vector<LatencyMeasurement> measurements_;
};

// Measure SlickLogger latency
void measure_slick_logger_latency() {
    std::cout << "=== SLICK LOGGER LATENCY TEST ===" << std::endl;
    
    slick::logger::Logger::instance().reset();
    slick::logger::Logger::instance().add_file_sink(FileUtils::get_unique_filename("slick_latency"));
    slick::logger::Logger::instance().init(65536);

    LatencyAnalyzer analyzer;
    MessageGenerator msg_gen;
    
    const size_t num_measurements = 10000;
    
    // Warmup
    for (size_t i = 0; i < 1000; ++i) {
        LOG_INFO("Warmup message {}", i);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Measure latencies
    for (size_t i = 0; i < num_measurements; ++i) {
        auto start = std::chrono::high_resolution_clock::now();
        LOG_INFO("Latency test message {} with value {}", i, msg_gen.random_int());
        auto end = std::chrono::high_resolution_clock::now();
        
        analyzer.add_measurement(
            std::chrono::duration_cast<std::chrono::nanoseconds>(start.time_since_epoch()).count(),
            std::chrono::duration_cast<std::chrono::nanoseconds>(end.time_since_epoch()).count(),
            "slick-logger"
        );
    }

    analyzer.analyze_and_report();
    
    slick::logger::Logger::instance().shutdown();
}

// Measure spdlog sync latency
void measure_spdlog_sync_latency() {
    std::cout << "=== SPDLOG SYNC LATENCY TEST ===" << std::endl;
    
    auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(
        FileUtils::get_unique_filename("spdlog_sync_latency"));
    auto logger = std::make_shared<spdlog::logger>("spdlog_sync", file_sink);
    logger->set_level(spdlog::level::info);

    LatencyAnalyzer analyzer;
    MessageGenerator msg_gen;
    
    const size_t num_measurements = 10000;
    
    // Warmup
    for (size_t i = 0; i < 1000; ++i) {
        logger->info("Warmup message {}", i);
    }

    // Measure latencies
    for (size_t i = 0; i < num_measurements; ++i) {
        auto start = std::chrono::high_resolution_clock::now();
        logger->info("Latency test message {} with value {}", i, msg_gen.random_int());
        auto end = std::chrono::high_resolution_clock::now();
        
        analyzer.add_measurement(
            std::chrono::duration_cast<std::chrono::nanoseconds>(start.time_since_epoch()).count(),
            std::chrono::duration_cast<std::chrono::nanoseconds>(end.time_since_epoch()).count(),
            "spdlog_sync"
        );
    }

    analyzer.analyze_and_report();
    
    logger->flush();
}

// Test latency under different queue pressure conditions
void measure_queue_pressure_effects() {
    std::cout << "=== QUEUE PRESSURE EFFECTS ===" << std::endl;
    
    slick::logger::Logger::instance().reset();
    slick::logger::Logger::instance().add_file_sink(FileUtils::get_unique_filename("slick_pressure"));
    slick::logger::Logger::instance().init(65536);

    MessageGenerator msg_gen;
    
    // Test different background load levels
    std::vector<size_t> background_loads = {0, 1000, 5000, 10000};
    
    for (size_t load : background_loads) {
        std::cout << "Testing with background load: " << load << " messages/sec" << std::endl;
        
        // Start background thread if needed
        std::atomic<bool> background_running{load > 0};
        std::thread background_thread;
        
        if (load > 0) {
            background_thread = std::thread([&background_running, &msg_gen, load]() {
                auto interval = std::chrono::nanoseconds(1000000000 / load); // ns between messages
                
                while (background_running) {
                    auto start = std::chrono::high_resolution_clock::now();
                    LOG_DEBUG("Background message {}", msg_gen.random_int());
                    
                    auto elapsed = std::chrono::high_resolution_clock::now() - start;
                    if (elapsed < interval) {
                        std::this_thread::sleep_for(interval - elapsed);
                    }
                }
            });
        }
        
        // Let background load stabilize
        if (load > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        
        // Measure foreground latency
        std::vector<double> latencies;
        const size_t num_measurements = 1000;
        
        for (size_t i = 0; i < num_measurements; ++i) {
            Timer timer;
            LOG_INFO("Foreground message {} under load", i);
            latencies.push_back(timer.elapsed_ns());
        }
        
        // Stop background load
        background_running = false;
        if (background_thread.joinable()) {
            background_thread.join();
        }
        
        Statistics stats(latencies);
        std::cout << "  Mean latency: " << std::fixed << std::setprecision(0) 
                  << stats.mean() << "ns" << std::endl;
        std::cout << "  P99 latency:  " << std::fixed << std::setprecision(0) 
                  << stats.percentile(99) << "ns" << std::endl;
        std::cout << std::endl;
    }
    
    slick::logger::Logger::instance().shutdown();
}

int main(int argc, char* argv[]) {
    try {
        std::cout << "SlickLogger Detailed Latency Benchmark" << std::endl;
        std::cout << "======================================" << std::endl << std::endl;
        
        FileUtils::cleanup_test_files();
        FileUtils::create_test_directory();
        
        measure_slick_logger_latency();
        measure_spdlog_sync_latency();
        measure_queue_pressure_effects();
        
        std::cout << "Latency benchmark completed." << std::endl;
        
    } catch (const std::exception& e) {
        std::cerr << "Latency benchmark failed: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
}