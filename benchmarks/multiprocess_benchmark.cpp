// Multi-process A/B: N producer processes log into one collector, with thread
// rings in shared memory (the default) and without (every entry through the
// shared queue). Rounds alternate between the two, since runs on one machine
// drift; min and median are reported.
//
//   multiprocess_benchmark [--producers N] [--threads T] [--burst B] [--bursts K]
//                          [--pause-ms P] [--rounds R]
//
// Each thread logs K bursts of B entries with P ms between them, so a collector
// that keeps up on average sees bursts, as a real program logs. One burst of
// many entries (--bursts 1 --burst 50000) is the sustained overload case.
// The collector's sink only counts, so the numbers measure the transport, not
// formatting. Producers time their own calls and report them through a file.

#include <slick/logger.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#  include <process.h>
#  define BENCH_GETPID _getpid
#else
#  include <unistd.h>
#  define BENCH_GETPID getpid
#endif

using namespace slick::logger;

namespace {

int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

class CountingSink : public ISink {
public:
    CountingSink() : ISink("counting") {}
    void write(const LogEntry& entry) override {
        if (entry.arg_count == 3) { // a bench entry, not a banner: time the burst alone
            const int64_t now = now_ns();
            if (first_ns_.load(std::memory_order_relaxed) == 0) {
                first_ns_.store(now, std::memory_order_relaxed);
            }
            last_ns_.store(now, std::memory_order_relaxed);
        }
        count_.fetch_add(1, std::memory_order_release);
    }
    void flush() override {}
    uint64_t count() const { return count_.load(std::memory_order_acquire); }
    int64_t span_ns() const { return last_ns_.load() - first_ns_.load(); }

private:
    std::atomic<uint64_t> count_{0};
    std::atomic<int64_t> first_ns_{0};
    std::atomic<int64_t> last_ns_{0};
};

struct load {
    int threads = 2;
    int burst = 5000;
    int bursts = 10;
    int pause_ms = 20;
    int entries() const { return burst * bursts; }
};

int run_producer(const std::string& name, const load& work, bool rings, const std::string& out) {
    const int threads = work.threads;
    LogConfig config;
    config.mode = QueueMode::SharedProducer;
    config.shared_memory_name = name;
    config.process_tag = "bench";
    config.thread_rings = rings;
    config.include_source_location = false;
    Logger::instance().init(config);

    std::atomic<int> ready{0};
    std::vector<int64_t> elapsed(threads, 0);
    std::vector<std::thread> workers;
    const std::string text = "twelve-chars";
    for (int t = 0; t < threads; ++t) {
        workers.emplace_back([&, t] {
            Logger::instance().prepare_thread();
            ready.fetch_add(1);
            while (ready.load() < threads) {
            }
            for (int b = 0; b < work.bursts; ++b) {
                const int64_t start = now_ns();
                for (int i = 0; i < work.burst; ++i) {
                    LOG_INFO("bench {} {} {}", i, 3.25, text);
                }
                elapsed[t] += now_ns() - start;
                std::this_thread::sleep_for(std::chrono::milliseconds(work.pause_ms));
            }
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }
    Logger::instance().flush(); // returns once the collector has it all
    Logger::instance().shutdown();
    int64_t sum = 0;
    for (int64_t e : elapsed) {
        sum += e;
    }
    std::ofstream(out) << static_cast<double>(sum) / (static_cast<double>(threads) * work.entries()) << "\n";
    return 0;
}

struct round_result {
    double producer_ns = 0;   // mean ns per call over every producer thread
    double drain_meps = 0;    // entries per second the collector took, first to last
    double delivered_pct = 0; // entries that reached the sink
};

round_result run_round(const std::string& exe, int round, int producers, const load& work, bool rings) {
    const std::string name = "mpb" + std::to_string(BENCH_GETPID()) + "_" + std::to_string(round);
    auto sink = std::make_shared<CountingSink>();
    LogConfig config;
    config.mode = QueueMode::SharedCollector;
    config.shared_memory_name = name;
    config.sinks.push_back(sink);
    config.include_source_location = false;
    Logger::instance().init(config);
    const uint64_t banner = 1;

    std::vector<std::thread> launches;
    std::vector<std::string> outs;
    for (int p = 0; p < producers; ++p) {
        outs.push_back(name + "_" + std::to_string(p) + ".out");
        std::filesystem::remove(outs.back());
        const std::string command = "\"" + exe + "\" --producer " + name + " --threads " +
                                    std::to_string(work.threads) + " --burst " + std::to_string(work.burst) +
                                    " --bursts " + std::to_string(work.bursts) + " --pause-ms " +
                                    std::to_string(work.pause_ms) + " --rings " + (rings ? "1" : "0") + " --out " +
                                    outs.back();
#ifdef _WIN32
        launches.emplace_back([command] { std::system(("\"" + command + "\"").c_str()); });
#else
        launches.emplace_back([command] { std::system(command.c_str()); });
#endif
    }
    for (auto& launch : launches) {
        launch.join();
    }
    const uint64_t expected = uint64_t(producers) * work.threads * work.entries() + producers /* banners */ + banner;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (sink->count() < expected && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    round_result result;
    result.delivered_pct = 100.0 * static_cast<double>(sink->count()) / static_cast<double>(expected);
    result.drain_meps = static_cast<double>(sink->count()) * 1e3 / static_cast<double>(std::max<int64_t>(1, sink->span_ns()));
    Logger::instance().shutdown();
    Logger::instance().reset();
    double sum = 0;
    for (const auto& out : outs) {
        double ns = 0;
        std::ifstream(out) >> ns;
        sum += ns;
        std::filesystem::remove(out);
    }
    result.producer_ns = sum / producers;
    for (const char* suffix : {"", "_str", ".reg"}) {
        slick::shm::shared_memory::remove((name + suffix).c_str());
    }
    return result;
}

double median(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

} // namespace

int main(int argc, char** argv) {
    int producers = 4;
    load work;
    int rounds = 5;
    std::string producer_name;
    std::string out;
    bool rings = true;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const bool has_value = i + 1 < argc;
        if (arg == "--producers" && has_value) producers = std::atoi(argv[++i]);
        else if (arg == "--threads" && has_value) work.threads = std::atoi(argv[++i]);
        else if (arg == "--burst" && has_value) work.burst = std::atoi(argv[++i]);
        else if (arg == "--bursts" && has_value) work.bursts = std::atoi(argv[++i]);
        else if (arg == "--pause-ms" && has_value) work.pause_ms = std::atoi(argv[++i]);
        else if (arg == "--rounds" && has_value) rounds = std::atoi(argv[++i]);
        else if (arg == "--producer" && has_value) producer_name = argv[++i];
        else if (arg == "--rings" && has_value) rings = std::atoi(argv[++i]) != 0;
        else if (arg == "--out" && has_value) out = argv[++i];
        else {
            std::fprintf(stderr, "unknown argument %s\n", arg.c_str());
            return 2;
        }
    }
    if (!producer_name.empty()) {
        return run_producer(producer_name, work, rings, out);
    }

    const std::string exe = std::filesystem::absolute(argv[0]).string();
    std::printf("%d producer processes x %d threads x %d bursts of %d entries, %d ms apart; %d rounds each\n",
                producers, work.threads, work.bursts, work.burst, work.pause_ms, rounds);
    std::vector<double> ns[2], meps[2], delivered[2];
    for (int r = 0; r < rounds; ++r) {
        for (int mode = 0; mode < 2; ++mode) {
            const bool with_rings = mode == 0;
            const round_result result = run_round(exe, r * 2 + mode, producers, work, with_rings);
            ns[mode].push_back(result.producer_ns);
            meps[mode].push_back(result.drain_meps);
            delivered[mode].push_back(result.delivered_pct);
            std::printf("  round %d %-13s %7.1f ns/call  %6.2f M entries/s drained  %6.2f%% delivered\n", r,
                        with_rings ? "thread rings" : "shared queue", result.producer_ns, result.drain_meps,
                        result.delivered_pct);
        }
    }
    std::printf("\n%-13s %16s %16s %18s %14s\n", "", "ns/call (min)", "ns/call (median)", "M/s drained (med)",
                "delivered (min)");
    for (int mode = 0; mode < 2; ++mode) {
        std::printf("%-13s %16.1f %16.1f %18.2f %13.2f%%\n", mode == 0 ? "thread rings" : "shared queue",
                    *std::min_element(ns[mode].begin(), ns[mode].end()), median(ns[mode]), median(meps[mode]),
                    *std::min_element(delivered[mode].begin(), delivered[mode].end()));
    }
    return 0;
}
