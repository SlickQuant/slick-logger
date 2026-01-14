# SlickLogger Benchmark Setup Guide

This guide helps you build and run the comprehensive benchmark suite to compare SlickLogger against other popular C++ logging libraries.

## Quick Start

### 1. Build with Benchmarks

```bash
# Clone the repository  
git clone https://github.com/SlickQuant/slick-logger.git
cd slick-logger

# Create build directory
mkdir build && cd build

# Configure with benchmarks enabled
cmake -DBUILD_BENCHMARKS=ON -DCMAKE_BUILD_TYPE=Release ..

# Build all targets
cmake --build . --config Release

# Verify benchmark executables were created
ls benchmarks/  # Linux/Mac
dir benchmarks\ # Windows
```

### 2. Run Quick Benchmark

```bash
# Linux/Mac - use shell script
cd benchmarks
./run_benchmarks.sh quick

# Windows - use batch script  
cd benchmarks
run_benchmarks.bat quick

# Or run manually
cd build/benchmark_results
../benchmarks/slick_logger_benchmark 10000 2  # 10k iterations, 2 runs
```

### 3. Run Full Benchmark Suite

```bash
# Linux/Mac
./benchmarks/run_benchmarks.sh full

# Windows  
benchmarks\run_benchmarks.bat full

# This will run:
# - Main comparison benchmark (slick_logger_benchmark)
# - Detailed latency analysis (latency_benchmark)
# - Throughput scaling tests (throughput_benchmark)  
# - Memory usage analysis (memory_benchmark)
```

## Expected Output

### Throughput Comparison
```
=== THROUGHPUT BENCHMARKS ===

--- Small Messages ---

Testing with 1 thread(s):
Library               Mean      Median         P95         P99      StdDev
--------------------------------------------------------------------------------
slick_logger_small   2847291   2856543   2801234   2745123        45.2
spdlog_async_small   1934521   1945123   1876543   1823451        52.1
spdlog_sync_small     876543    881234    845123    823456        28.9
std_ofstream_small    654321    661234    634567    612345        31.4

Unit: ops/sec

Testing with 4 thread(s):
Library               Mean      Median         P95         P99      StdDev
--------------------------------------------------------------------------------
slick_logger_small   8234567   8345123   8123456   7987654        123.4
spdlog_async_small   6543210   6612345   6234567   5987654        167.8
spdlog_sync_small    2345678   2398765   2234567   2123456         89.2
```

### Latency Analysis
```
=== DETAILED LATENCY ANALYSIS ===

Samples: 10000
Mean:    342.5 ns
Median:  298.0 ns  
P95:     567.0 ns
P99:     845.0 ns
P99.9:   1234.0 ns

Latency Distribution:
0-100ns     :    156 (1.6%)
100-500ns   :   8234 (82.3%)  
500ns-1μs   :   1456 (14.6%)
1-5μs       :    145 (1.5%)
>5μs        :      9 (0.1%)
```

### Memory Usage
```
=== MEMORY USAGE COMPARISON ===
Logger         Queue Size   Peak MB   Bytes/Msg   Efficiency
--------------------------------------------------------------
SlickLogger          8192        45        18.2         5495
spdlog_async         8192        67        27.3         3663

Efficiency = Messages per MB of memory used
```

## Performance Expectations

Based on typical hardware (modern multi-core CPU, SSD storage):

### SlickLogger Performance Targets
- **Throughput**: 2-5M messages/sec (single thread), 8-20M messages/sec (multi-thread)
- **Latency**: <500ns mean, <1μs P99 for small messages
- **Memory**: <50 bytes per queued message
- **Scaling**: >80% efficiency with 4 threads

### Comparison with spdlog
- SlickLogger should show **2-3x higher throughput** due to lock-free queue
- SlickLogger should show **2-5x lower latency** due to deferred formatting  
- Memory usage should be **comparable or better**
- Multi-threading scaling should be **significantly better**

## Troubleshooting

### Build Issues

**CMake can't find dependencies:**
```bash
# Make sure you have internet connection for FetchContent
# Or install dependencies manually:

# Ubuntu/Debian
sudo apt-get install libspdlog-dev libfmt-dev

# Use system packages
cmake -DBUILD_BENCHMARKS=ON -DUSE_SYSTEM_SPDLOG=ON ..
```

**C++20 support issues:**
```bash
# Use newer compiler
export CXX=g++-10  # or clang++-12
```

**Windows MSVC issues:**
```cmd
# Use Visual Studio 2019 or later
# Open "Developer Command Prompt" or "x64 Native Tools Command Prompt"

cmake -DBUILD_BENCHMARKS=ON -A x64 ..
cmake --build . --config Release
```

### Runtime Issues

**Low performance numbers:**
```bash
# Make sure running in Release mode
cmake -DCMAKE_BUILD_TYPE=Release ..

# Close other applications
# Disable CPU frequency scaling (Linux):
echo performance | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor

# Run with higher priority:
sudo nice -n -10 ./slick_logger_benchmark
```

**File permission errors:**
```bash
# Create log directories manually
mkdir -p benchmark_logs
chmod 755 benchmark_logs

# Or run from writable directory
cd /tmp
/path/to/slick_logger_benchmark
```

**Memory allocation failures:**
```bash
# Increase limits
ulimit -v unlimited
ulimit -m unlimited

# Reduce queue sizes in benchmark_config.hpp
```

### Inconsistent Results

**High variance in measurements:**
- Close unnecessary applications
- Run multiple times and average results
- Use CPU affinity: `taskset -c 0-3 ./benchmark`
- Check for thermal throttling

**Unexpectedly low SlickLogger performance:**
- Verify queue size is appropriate (64K default)
- Check that async writer thread is running
- Ensure file sink is properly configured
- Monitor for queue overflow conditions

## Customizing Benchmarks

### Modify Parameters
Edit `benchmark_config.hpp`:
```cpp
constexpr size_t DEFAULT_MEASUREMENT_ITERATIONS = 100000;  // More iterations
constexpr size_t DEFAULT_NUM_RUNS = 5;                     // More runs
```

### Add New Libraries
1. Add dependency to `benchmarks/CMakeLists.txt`
2. Create new scenario class in benchmark source
3. Add to benchmark runner

### Custom Test Scenarios
```cpp
class MyCustomScenario : public ThroughputScenario<MyLogger> {
    void setup() override { /* initialize */ }
    void log_message() override { /* log call */ }
    void cleanup() override { /* cleanup */ }
};
```

## Interpreting Results

### Throughput Analysis
- **Higher ops/sec = better performance**
- Compare single-thread baseline performance
- Analyze scaling efficiency with multiple threads
- Look for performance cliffs at high thread counts

### Latency Analysis  
- **Lower latency = better responsiveness**
- P99 latency is critical for user-facing applications
- Check latency distribution for outliers
- Monitor for latency increases under load

### Memory Analysis
- **Lower memory usage = better efficiency**
- Watch for memory leaks (increasing memory over time)
- Compare peak vs steady-state usage
- Analyze memory-per-message ratios

### Scaling Analysis
- **Efficiency = (Throughput_N / Throughput_1) / N * 100%**
- Perfect scaling = 100% efficiency
- Good scaling = >80% efficiency
- Poor scaling = <60% efficiency (contention issues)

## Performance Tips

Based on benchmark results:

### For Maximum Throughput
- Use async logging (SlickLogger, spdlog async)
- Tune queue sizes for your workload
- Consider dedicated logging threads
- Use appropriate message batching

### For Low Latency
- SlickLogger's deferred formatting helps significantly
- Smaller queue sizes reduce memory pressure  
- Consider sync logging for ultra-low latency needs
- Monitor queue depth and overflow conditions

### For Memory Efficiency  
- Right-size queue based on peak message rates
- Monitor for memory fragmentation in long-running apps
- Consider message size impact on memory usage
- Use memory-mapped files for very high throughput

## Contributing Results

If you run benchmarks on interesting hardware configurations:

1. Include system specifications (CPU, RAM, storage)
2. Note any special configuration (CPU affinity, OS tuning)
3. Share both raw numbers and analysis
4. Consider submitting performance improvements

## Next Steps

After running benchmarks:

1. **Analyze Results**: Look for performance bottlenecks
2. **Compare Libraries**: Choose best fit for your use case  
3. **Tune Configuration**: Optimize queue sizes and parameters
4. **Profile Your Application**: Test with realistic workloads
5. **Monitor Production**: Track performance metrics in production

The benchmark suite provides a solid foundation for understanding SlickLogger's performance characteristics and comparing it with alternatives. Use these insights to make informed decisions about logging infrastructure in your applications.