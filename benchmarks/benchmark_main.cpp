#include <iostream>
#include <memory>
#include <vector>
#include <chrono>
#include <fstream>
#include <filesystem>
#include <thread>
#include <numeric>

// Include logging libraries
#include <slick/logger.hpp>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/null_sink.h>
#include <spdlog/async.h>
#include <fmt/format.h>

// Include benchmark infrastructure
#include "benchmark_utils.hpp"
#include "system_monitor.hpp"

using namespace benchmark_utils;

// Message size enum
enum class MessageSize {
    SMALL,   // ~20 bytes
    MEDIUM,  // ~100-200 bytes  
    LARGE    // ~500-1000 bytes
};

// Simple benchmark scenario base class
class BenchmarkScenario {
public:
    BenchmarkScenario(const std::string& name) : name_(name), msg_gen_() {}
    virtual ~BenchmarkScenario() = default;
    
    virtual void setup() = 0;
    virtual void cleanup() = 0;
    virtual void log_single_message() = 0;
    
    std::string get_name() const { return name_; }
    
    void run_throughput_test(size_t iterations, size_t num_threads) {
        if (num_threads == 1) {
            for (size_t i = 0; i < iterations; ++i) {
                log_single_message();
            }
        } else {
            run_multi_threaded(iterations, num_threads);
        }
    }
    
    std::vector<double> run_latency_test(size_t iterations) {
        std::vector<double> latencies;
        latencies.reserve(iterations);
        
        for (size_t i = 0; i < iterations; ++i) {
            Timer timer;
            log_single_message();
            latencies.push_back(timer.elapsed_ns());
        }
        
        return latencies;
    }
    
protected:
    std::string name_;
    MessageGenerator msg_gen_;
    
private:
    void run_multi_threaded(size_t total_iterations, size_t num_threads) {
        std::vector<std::thread> threads;
        ThreadBarrier barrier(num_threads);
        size_t iterations_per_thread = total_iterations / num_threads;
        
        for (size_t t = 0; t < num_threads; ++t) {
            threads.emplace_back([this, &barrier, iterations_per_thread]() {
                barrier.wait(); // Synchronize thread starts
                
                for (size_t i = 0; i < iterations_per_thread; ++i) {
                    log_single_message();
                }
            });
        }
        
        for (auto& thread : threads) {
            thread.join();
        }
    }
};

// SlickLogger implementations
class SlickLoggerScenario : public BenchmarkScenario {
public:
    SlickLoggerScenario(MessageSize msg_size = MessageSize::SMALL)
        : BenchmarkScenario("slick-logger"), msg_size_(msg_size) {}

    void setup() override {
        slick::logger::Logger::instance().reset();
        slick::logger::Logger::instance().add_file_sink(FileUtils::get_unique_filename("slick"));
        slick::logger::Logger::instance().init(65536);
        
        // Warm up
        for (int i = 0; i < 100; ++i) {
            LOG_INFO("Warmup message {}", i);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    void cleanup() override {
        slick::logger::Logger::instance().shutdown();
    }

    void log_single_message() override {
        switch (msg_size_) {
            case MessageSize::SMALL:
                LOG_INFO("Info message");
                break;
            case MessageSize::MEDIUM:
                LOG_INFO("Processing user request with ID {} at timestamp {} with status {}", 
                        msg_gen_.random_int(), msg_gen_.random_double(), msg_gen_.random_string());
                break;
            case MessageSize::LARGE:
                LOG_INFO("Detailed system report: CPU usage is {}%, memory usage is {} MB, "
                        "disk usage is {} GB, network throughput is {} Mbps, "
                        "active connections: {}, pending requests: {}, cache hit ratio: {}%, "
                        "database connections: {}, queue depth: {}, last error: {} at timestamp {}", 
                        msg_gen_.random_int(), msg_gen_.random_int(), msg_gen_.random_int(),
                        msg_gen_.random_double(), msg_gen_.random_int(), msg_gen_.random_int(),
                        msg_gen_.random_double(), msg_gen_.random_int(), msg_gen_.random_int(),
                        msg_gen_.random_string(), msg_gen_.random_double());
                break;
        }
    }
    
    std::string get_name() const {
        std::string size_str = (msg_size_ == MessageSize::SMALL ? "small" : 
                               msg_size_ == MessageSize::MEDIUM ? "medium" : "large");
        return name_ + "_" + size_str;
    }

private:
    MessageSize msg_size_;
};

// spdlog implementations
class SpdlogSyncScenario : public BenchmarkScenario {
public:
    SpdlogSyncScenario(MessageSize msg_size = MessageSize::SMALL)
        : BenchmarkScenario("spdlog_sync"), msg_size_(msg_size) {}

    void setup() override {
        auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(
            FileUtils::get_unique_filename("spdlog_sync"));
        logger_ = std::make_shared<spdlog::logger>("spdlog_sync", file_sink);
        logger_->set_level(spdlog::level::info);
        
        // Warm up
        for (int i = 0; i < 100; ++i) {
            logger_->info("Warmup message {}", i);
        }
    }

    void cleanup() override {
        if (logger_) {
            logger_->flush();
            logger_.reset();
        }
    }

    void log_single_message() override {
        if (!logger_) return;
        
        switch (msg_size_) {
            case MessageSize::SMALL:
                logger_->info("Info message");
                break;
            case MessageSize::MEDIUM:
                logger_->info("Processing user request with ID {} at timestamp {} with status {}", 
                             msg_gen_.random_int(), msg_gen_.random_double(), msg_gen_.random_string());
                break;
            case MessageSize::LARGE:
                logger_->info("Detailed system report: CPU usage is {}%, memory usage is {} MB, "
                            "disk usage is {} GB, network throughput is {} Mbps, "
                            "active connections: {}, pending requests: {}, cache hit ratio: {}%, "
                            "database connections: {}, queue depth: {}, last error: {} at timestamp {}", 
                            msg_gen_.random_int(), msg_gen_.random_int(), msg_gen_.random_int(),
                            msg_gen_.random_double(), msg_gen_.random_int(), msg_gen_.random_int(),
                            msg_gen_.random_double(), msg_gen_.random_int(), msg_gen_.random_int(),
                            msg_gen_.random_string(), msg_gen_.random_double());
                break;
        }
    }
    
    std::string get_name() const {
        std::string size_str = (msg_size_ == MessageSize::SMALL ? "small" : 
                               msg_size_ == MessageSize::MEDIUM ? "medium" : "large");
        return name_ + "_" + size_str;
    }

private:
    MessageSize msg_size_;
    std::shared_ptr<spdlog::logger> logger_;
};

class SpdlogAsyncScenario : public BenchmarkScenario {
public:
    SpdlogAsyncScenario(MessageSize msg_size = MessageSize::SMALL)
        : BenchmarkScenario("spdlog_async"), msg_size_(msg_size) {}

    void setup() override {
        spdlog::init_thread_pool(65536, 1);
        auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(
            FileUtils::get_unique_filename("spdlog_async"));
        logger_ = std::make_shared<spdlog::async_logger>("spdlog_async", file_sink, 
                                                        spdlog::thread_pool());
        logger_->set_level(spdlog::level::info);
        
        // Warm up
        for (int i = 0; i < 100; ++i) {
            logger_->info("Warmup message {}", i);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    void cleanup() override {
        if (logger_) {
            logger_->flush();
            logger_.reset();
        }
        spdlog::shutdown();
    }

    void log_single_message() override {
        if (!logger_) return;
        
        switch (msg_size_) {
            case MessageSize::SMALL:
                logger_->info("Info message");
                break;
            case MessageSize::MEDIUM:
                logger_->info("Processing user request with ID {} at timestamp {} with status {}", 
                             msg_gen_.random_int(), msg_gen_.random_double(), msg_gen_.random_string());
                break;
            case MessageSize::LARGE:
                logger_->info("Detailed system report: CPU usage is {}%, memory usage is {} MB, "
                            "disk usage is {} GB, network throughput is {} Mbps, "
                            "active connections: {}, pending requests: {}, cache hit ratio: {}%, "
                            "database connections: {}, queue depth: {}, last error: {} at timestamp {}", 
                            msg_gen_.random_int(), msg_gen_.random_int(), msg_gen_.random_int(),
                            msg_gen_.random_double(), msg_gen_.random_int(), msg_gen_.random_int(),
                            msg_gen_.random_double(), msg_gen_.random_int(), msg_gen_.random_int(),
                            msg_gen_.random_string(), msg_gen_.random_double());
                break;
        }
    }
    
    std::string get_name() const {
        std::string size_str = (msg_size_ == MessageSize::SMALL ? "small" : 
                               msg_size_ == MessageSize::MEDIUM ? "medium" : "large");
        return name_ + "_" + size_str;
    }

private:
    MessageSize msg_size_;
    std::shared_ptr<spdlog::logger> logger_;
};

// Baseline implementations
class StdOfstreamScenario : public BenchmarkScenario {
public:
    StdOfstreamScenario(MessageSize msg_size = MessageSize::SMALL)
        : BenchmarkScenario("std_ofstream"), msg_size_(msg_size) {}

    void setup() override {
        file_stream_.open(FileUtils::get_unique_filename("baseline"));
        if (!file_stream_) {
            throw std::runtime_error("Failed to open baseline log file");
        }
    }

    void cleanup() override {
        if (file_stream_.is_open()) {
            file_stream_.close();
        }
    }

    void log_single_message() override {
        if (!file_stream_) return;
        
        switch (msg_size_) {
            case MessageSize::SMALL:
                file_stream_ << "Info message" << std::endl;
                break;
            case MessageSize::MEDIUM:
                file_stream_ << fmt::format("Processing user request with ID {} at timestamp {} with status {}", 
                                          msg_gen_.random_int(), 
                                          msg_gen_.random_double(), 
                                          msg_gen_.random_string()) << std::endl;
                break;
            case MessageSize::LARGE:
                file_stream_ << fmt::format("Detailed system report: CPU usage is {}%, memory usage is {} MB, "
                                          "disk usage is {} GB, network throughput is {} Mbps, "
                                          "active connections: {}, pending requests: {}, cache hit ratio: {}%, "
                                          "database connections: {}, queue depth: {}, last error: {} at timestamp {}", 
                                          msg_gen_.random_int(), msg_gen_.random_int(), msg_gen_.random_int(),
                                          msg_gen_.random_double(), msg_gen_.random_int(), msg_gen_.random_int(),
                                          msg_gen_.random_double(), msg_gen_.random_int(), msg_gen_.random_int(),
                                          msg_gen_.random_string(), msg_gen_.random_double()) << std::endl;
                break;
        }
    }
    
    std::string get_name() const {
        std::string size_str = (msg_size_ == MessageSize::SMALL ? "small" : 
                               msg_size_ == MessageSize::MEDIUM ? "medium" : "large");
        return name_ + "_" + size_str;
    }

private:
    MessageSize msg_size_;
    std::ofstream file_stream_;
};

// Benchmark runner
class BenchmarkRunner {
public:
    explicit BenchmarkRunner(const BenchmarkConfig& config) : config_(config) {
        FileUtils::cleanup_test_files();
        FileUtils::create_test_directory();
    }

    void run_throughput_benchmarks() {
        ResultFormatter::print_header("THROUGHPUT BENCHMARKS");
        
        std::vector<MessageSize> message_sizes = {MessageSize::SMALL, MessageSize::MEDIUM, MessageSize::LARGE};
        std::vector<size_t> thread_counts = {1, 2, 4, 8};
        
        for (auto msg_size : message_sizes) {
            std::string size_name = (msg_size == MessageSize::SMALL ? "Small" : 
                                   msg_size == MessageSize::MEDIUM ? "Medium" : "Large");
            
            std::cout << "\n--- " << size_name << " Messages ---\n" << std::endl;
            
            for (auto num_threads : thread_counts) {
                std::cout << "Testing with " << num_threads << " thread(s):" << std::endl;
                
                std::vector<std::pair<std::string, Statistics>> results;
                
                // SlickLogger
                results.push_back(run_throughput_test(std::make_unique<SlickLoggerScenario>(msg_size), num_threads));
                
                // spdlog sync
                results.push_back(run_throughput_test(std::make_unique<SpdlogSyncScenario>(msg_size), num_threads));
                
                // spdlog async
                results.push_back(run_throughput_test(std::make_unique<SpdlogAsyncScenario>(msg_size), num_threads));
                
                // Baseline (only single-threaded)
                if (num_threads == 1) {
                    results.push_back(run_throughput_test(std::make_unique<StdOfstreamScenario>(msg_size), 1));
                }
                
                ResultFormatter::print_comparison_table(results);
            }
        }
    }

    void run_latency_benchmarks() {
        ResultFormatter::print_header("LATENCY BENCHMARKS");
        
        std::vector<MessageSize> message_sizes = {MessageSize::SMALL, MessageSize::MEDIUM, MessageSize::LARGE};
        
        for (auto msg_size : message_sizes) {
            std::string size_name = (msg_size == MessageSize::SMALL ? "Small" : 
                                   msg_size == MessageSize::MEDIUM ? "Medium" : "Large");
            
            std::cout << "\n--- " << size_name << " Messages ---\n" << std::endl;
            
            std::vector<std::pair<std::string, Statistics>> results;
            
            // Run latency tests (single-threaded only for accurate measurements)
            results.push_back(run_latency_test(std::make_unique<SlickLoggerScenario>(msg_size)));
            
            ResultFormatter::print_comparison_table(results, "ns/op");
        }
    }

    void run_memory_benchmarks() {
        ResultFormatter::print_header("MEMORY USAGE BENCHMARKS");
        
        const size_t test_iterations = 100000;
        
        // SlickLogger memory test
        {
            SystemMonitor monitor;
            ScopedMonitor scoped(monitor);
            
            auto scenario = std::make_unique<SlickLoggerScenario>(MessageSize::MEDIUM);
            scenario->setup();
            scenario->run_throughput_test(test_iterations, 1);
            scenario->cleanup();
            
            auto usage = monitor.get_current_usage();
            std::cout << "SlickLogger Memory Usage:" << std::endl;
            usage.print();
        }
        
        // spdlog async memory test
        {
            SystemMonitor monitor;
            ScopedMonitor scoped(monitor);
            
            auto scenario = std::make_unique<SpdlogAsyncScenario>(MessageSize::MEDIUM);
            scenario->setup();
            scenario->run_throughput_test(test_iterations, 1);
            scenario->cleanup();
            
            auto usage = monitor.get_current_usage();
            std::cout << "spdlog Async Memory Usage:" << std::endl;
            usage.print();
        }
    }

private:
    std::pair<std::string, Statistics> run_throughput_test(std::unique_ptr<BenchmarkScenario> scenario, size_t num_threads) {
        std::vector<double> throughputs;
        
        for (size_t run = 0; run < config_.num_runs; ++run) {
            // Setup
            scenario->setup();
            CPUWarmer::warm_up();
            
            // Run benchmark
            Timer timer;
            scenario->run_throughput_test(config_.measurement_iterations, num_threads);
            double elapsed_ms = timer.elapsed_ms();
            
            // Calculate throughput (ops/sec)
            double throughput = (config_.measurement_iterations / elapsed_ms) * 1000.0;
            throughputs.push_back(throughput);
            
            // Cleanup
            scenario->cleanup();
            
            // Small delay between runs
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        
        return {scenario->get_name(), Statistics(throughputs)};
    }
    
    std::pair<std::string, Statistics> run_latency_test(std::unique_ptr<BenchmarkScenario> scenario) {
        std::vector<double> avg_latencies;
        
        for (size_t run = 0; run < config_.num_runs; ++run) {
            scenario->setup();
            CPUWarmer::warm_up();
            
            auto latencies = scenario->run_latency_test(config_.measurement_iterations);
            
            // Calculate average for this run
            double avg_latency = std::accumulate(latencies.begin(), latencies.end(), 0.0) / latencies.size();
            avg_latencies.push_back(avg_latency);
            
            scenario->cleanup();
            
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        
        return {scenario->get_name(), Statistics(avg_latencies)};
    }

    BenchmarkConfig config_;
};

int main(int argc, char* argv[]) {
    try {
        std::cout << "SlickLogger Benchmark Suite" << std::endl;
        std::cout << "===========================" << std::endl;
        
        BenchmarkConfig config;
        config.warmup_iterations = 1000;
        config.measurement_iterations = 50000;
        config.num_runs = 3;
        
        // Parse command line arguments for quick testing
        if (argc > 1) {
            config.measurement_iterations = std::stoul(argv[1]);
        }
        if (argc > 2) {
            config.num_runs = std::stoul(argv[2]);
        }
        
        std::cout << "Configuration:" << std::endl;
        std::cout << "  Measurement iterations: " << config.measurement_iterations << std::endl;
        std::cout << "  Runs per test: " << config.num_runs << std::endl;
        std::cout << std::endl;

        BenchmarkRunner runner(config);
        
        // Run all benchmark suites
        runner.run_throughput_benchmarks();
        runner.run_latency_benchmarks();
        runner.run_memory_benchmarks();
        
        std::cout << "Benchmark complete. Log files are in the benchmark_logs/ directory." << std::endl;
        
    } catch (const std::exception& e) {
        std::cerr << "Benchmark failed: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
}