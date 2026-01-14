# SlickLogger Benchmark Suite

A comprehensive performance benchmarking suite for comparing SlickLogger against other popular C++ logging libraries.

## Overview

This benchmark suite provides detailed performance analysis across multiple dimensions:

- **Throughput**: Messages per second under various thread loads
- **Latency**: Time per individual log call (nanosecond precision)  
- **Memory Usage**: Peak memory consumption and efficiency
- **Scalability**: Performance scaling with thread count
- **Burst Performance**: Handling of sudden logging spikes
- **Resource Monitoring**: CPU usage and memory tracking

## Compared Libraries

- **SlickLogger**: Our lock-free async logger with deferred formatting
- **spdlog (sync)**: Popular synchronous logging library  
- **spdlog (async)**: spdlog's asynchronous mode
- **std::ofstream**: Baseline direct file output with fmt formatting
- **fmt library**: Direct fmt::print to file (when available)

## Quick Start

### Prerequisites

- CMake 3.20 or later
- C++20 compatible compiler
- Internet connection (to download dependencies)

### Building

```bash
# Clone and build with benchmarks enabled
git clone <your-repo-url>
cd slick-logger
mkdir build && cd build

# Enable benchmarks during cmake configuration
cmake -DBUILD_BENCHMARKS=ON ..

# Build all benchmarks
cmake --build . --config Release

# Or build specific benchmarks
cmake --build . --target slick_logger_benchmark
cmake --build . --target latency_benchmark  
cmake --build . --target throughput_benchmark
cmake --build . --target memory_benchmark
```

### Running Benchmarks

```bash
# Run comprehensive benchmark suite
./slick_logger_benchmark

# Run with custom parameters (iterations, runs)  
./slick_logger_benchmark 100000 5

# Run individual benchmark suites
./latency_benchmark
./throughput_benchmark
./memory_benchmark
```

## Benchmark Programs

### 1. slick_logger_benchmark (Main Suite)

Comprehensive comparison across all libraries and scenarios:

```bash
./slick_logger_benchmark [iterations] [runs]
```

**Parameters:**
- `iterations`: Number of log calls per test (default: 50,000)
- `runs`: Number of test runs for statistical averaging (default: 3)

**Test Scenarios:**
- Single-threaded throughput (small, medium, large messages)
- Multi-threaded throughput (1, 2, 4, 8 threads)
- Latency measurements (nanosecond precision)
- Memory usage analysis
- Mixed message size performance

**Sample Output:**
```
=== THROUGHPUT BENCHMARKS ===

--- Small Messages ---

Testing with 1 thread(s):
Library               Mean      Median     P95       P99         StdDev
--------------------------------------------------------------------------------
slick_logger_small   2847291   2856543   2801234   2745123        45.2
spdlog_async_small   1934521   1945123   1876543   1823451        52.1
spdlog_sync_small     876543    881234    845123    823456        28.9
std_ofstream_small    654321    661234    634567    612345        31.4

Unit: ops/sec
```

### 2. latency_benchmark (Detailed Latency Analysis)

In-depth latency measurement with distribution analysis:

```bash
./latency_benchmark
```

**Features:**
- Individual call latency measurement
- Latency distribution histograms  
- Timeline analysis (warmup effects)
- Queue pressure impact testing
- P99.9 latency percentiles

**Sample Output:**
```
=== DETAILED LATENCY ANALYSIS ===

Samples: 10000
Mean:    58.00
Median:  51.00
Min:     40.00
Max:     6415.00
StdDev:  72.97
P95:     80.00
P99:     141.00
P99.9:   312.00

Latency Distribution:
0-100ns     :   9745 (97.5%)
100-500ns   :    253 (2.5%)
500ns-1μs  :      0 (0.0%)
1-5μs      :      1 (0.0%)
5-10μs     :      1 (0.0%)
10-50μs    :      0 (0.0%)
50-100μs   :      0 (0.0%)
>100μs     :      0 (0.0%)
```

### 3. throughput_benchmark (Scaling Analysis)

Detailed throughput testing with thread scaling analysis:

```bash
./throughput_benchmark
```

**Features:**
- Thread scaling from 1 to 16 threads
- Efficiency analysis (scaling effectiveness)
- Burst performance testing
- CPU and memory monitoring during tests
- Contention analysis

**Sample Output:**
```
=== SCALING ANALYSIS ===
Logger          Threads  Throughput     CPU % Memory MB  Efficiency
---------------------------------------------------------------------------
SlickLogger           1     6913828       0.0         2      100.0%
SlickLogger           2     7800319       0.0         2       56.4%
SlickLogger           4    12884857       0.0         0       46.6%
SlickLogger           8    16699893       0.0         0       30.2%
SlickLogger          16    21878777       0.0         0       19.8%

spdlog_async          1     4979162       0.0         0      100.0%
spdlog_async          2      446211       0.0         0        4.5%
spdlog_async          4       84410       0.0         0        0.4%
spdlog_async          8       91394       0.0         0        0.2%
spdlog_async         16       99235       0.0         0        0.1%

spdlog_sync           1     4187016       0.0         0      100.0%
spdlog_sync           2     1871659       0.0         0       22.4%
spdlog_sync           4     1666028       0.0         0        9.9%
```

### 4. memory_benchmark (Memory Analysis)

Comprehensive memory usage analysis:

```bash
./memory_benchmark
```

**Features:**
- Peak memory consumption measurement
- Memory-per-message efficiency
- Queue size impact analysis
- Sustained load memory stability
- Memory fragmentation detection
- Memory leak detection

**Sample Output:**
```
=== MEMORY USAGE COMPARISON ===
Logger         Queue Size   Peak MB   Bytes/Msg  Efficiency
----------------------------------------------------------------------
SlickLogger          1024        68     35036.0          29
spdlog_async         1024         0       186.0        5376
SlickLogger          8192        68      4414.0         227
spdlog_async         8192         0         0.0         inf
SlickLogger         65536        81       655.4        1526
spdlog_async        65536        25       204.0        4901
SlickLogger        262144       137       274.7        3641
spdlog_async       262144       102       204.0        4902

Efficiency = Messages per MB of memory used
```

## Benchmark Configuration

### Environment Variables

```bash
# Set CPU affinity for consistent results
export SLICK_BENCHMARK_CPU_AFFINITY=0,1,2,3

# Set output directory  
export SLICK_BENCHMARK_OUTPUT_DIR=./benchmark_results

# Enable detailed output
export SLICK_BENCHMARK_VERBOSE=1
```

### Compile-Time Options

```cmake
# Enable additional debug information in benchmarks
-DBENCHMARK_DEBUG_OUTPUT=ON

# Use system spdlog instead of fetching
-DUSE_SYSTEM_SPDLOG=ON

# Disable specific benchmarks
-DENABLE_MEMORY_BENCHMARKS=OFF
```

## Understanding Results

### Throughput Metrics

- **ops/sec**: Operations (log calls) per second
- Higher values indicate better performance
- Linear scaling with threads indicates good parallelization
- Compare against baseline (std::ofstream) for context

### Latency Metrics  

- **Mean/Median**: Average response time per call
- **P95/P99**: 95th and 99th percentile latencies
- **P99.9**: Critical for understanding tail latencies
- Lower values indicate better responsiveness

### Memory Metrics

- **Peak Memory**: Maximum memory used during test
- **Bytes/Message**: Memory efficiency indicator  
- **Efficiency Score**: Messages processed per MB
- Lower peak memory and bytes/message is better

### Scaling Efficiency

```
Efficiency = (Throughput_N_threads / Throughput_1_thread) / N_threads * 100%
```

- 100% = perfect linear scaling
- >80% = good scaling
- <60% = poor scaling, likely contention issues

## Optimization Tips

Based on benchmark results:

### For High Throughput
- Use async logging libraries (SlickLogger, spdlog async)
- Increase queue size for better batching
- Consider multiple writer threads for very high loads

### For Low Latency  
- Prefer SlickLogger's deferred formatting
- Use smaller queue sizes to reduce memory pressure
- Consider sync logging for ultra-low latency requirements

### For Memory Efficiency
- Tune queue sizes based on expected load
- Monitor for memory fragmentation in long-running applications
- Consider message size impact on memory usage

## Troubleshooting

### Common Issues

**Build Errors:**
```bash
# Missing dependencies
sudo apt-get install build-essential cmake

# C++20 support issues  
export CXX=g++-10  # or clang++-12
```

**Runtime Issues:**
```bash
# Permission errors with log files
mkdir -p benchmark_logs
chmod 755 benchmark_logs

# Memory allocation failures
ulimit -v unlimited
```

**Inconsistent Results:**
```bash
# Disable CPU frequency scaling
echo performance | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor

# Disable hyperthreading if needed
echo off | sudo tee /sys/devices/system/cpu/smt/control
```

### Performance Tuning

**For Accurate Benchmarks:**
- Run in Release mode only (`-DCMAKE_BUILD_TYPE=Release`)
- Close unnecessary applications
- Use CPU affinity to pin threads
- Run multiple iterations and average results
- Consider system warm-up time

**System Configuration:**
```bash
# Increase file descriptor limits
ulimit -n 65536

# Increase memory limits
ulimit -m unlimited

# Set process priority
nice -n -10 ./slick_logger_benchmark
```

## Extending Benchmarks

### Adding New Libraries

1. Create benchmark scenario class inheriting from `ThroughputScenario` or `LatencyScenario`
2. Implement logger-specific setup/cleanup and log_message methods
3. Add to benchmark runner in appropriate test functions
4. Update CMakeLists.txt with new dependencies

### Custom Test Scenarios

```cpp
// Example custom scenario
class CustomThroughputScenario : public ThroughputScenario<YourLogger> {
public:
    CustomThroughputScenario(size_t threads) 
        : ThroughputScenario(nullptr, "your_logger", threads) {}
    
    void setup() override {
        // Initialize your logger
    }
    
    void log_message() override {
        // Call your logger's log function
    }
};
```

## Contributing

When adding new benchmarks or improvements:

1. Follow existing code structure and naming conventions
2. Add appropriate statistical analysis
3. Include documentation for new test scenarios  
4. Ensure cross-platform compatibility
5. Add example output to documentation

## License

This benchmark suite is part of SlickLogger and follows the same MIT license.