# SlickLogger Benchmark Suite

Performance benchmarks comparing SlickLogger against other C++ logging libraries.

## Compared Libraries

| Name in results | What it is |
|-----------------|------------|
| `slick-logger` | This library: a lock-free ring per logging thread, deferred formatting, async writer thread |
| `slick (shared)` | The same with `thread_rings = false`: every thread on one shared lock-free queue |
| `spdlog_sync` | spdlog 1.12.0 writing through `basic_file_sink_mt` on the calling thread |
| `spdlog_async` | spdlog 1.12.0 in async mode (8192-slot queue, one worker thread) |
| `std_ofstream` | Baseline: `fmt::format` into an `std::ofstream` with `std::endl` per line |

spdlog and fmt are fetched automatically by `benchmarks/CMakeLists.txt` (spdlog 1.12.0, fmt 10.1.0).

## Results

Everything below is a real run of the suite at its default settings. Re-run it
yourself before quoting any of it: these numbers describe one machine.

### Measured on

| Property | Value |
|----------|-------|
| CPU | AMD Ryzen 9 5900HX (8 cores / 16 threads, 3.3 GHz base) |
| RAM | 32 GB |
| OS | Windows 11 Pro 26200 |
| Compiler | MSVC 19.44 (toolset 14.44.35207), `/O2 /DNDEBUG`, C++20 |
| Log output | Local NVMe SSD |
| Settings | 50,000 iterations × 3 runs (the defaults) |
| Date | 2026-10-22 |

`slick-logger` is the default configuration: a ring per logging thread
(`LogConfig::thread_rings`). `slick (shared)` is the same build with
`-DSLICK_LOGGER_DEFAULT_THREAD_RINGS=OFF`, every thread on the shared rings as in
earlier releases. Runs on this machine drift by up to 2× between back-to-back
runs of one binary, so the main-suite figures are the best of three runs,
interleaved between the two builds.

### Read this before the tables

**Put the log output on a local disk.** Every library here ultimately writes to
a file, so the storage device sets the ceiling for the synchronous ones. On an
SMB share `spdlog_sync` measured 9,021 ops/sec and `std_ofstream` 202 ops/sec,
144× and 310× below the local-disk numbers, while `slick-logger` barely moved
because its producers never touch the file. A network-mounted run is useless for
comparison. The runner scripts cd into the build tree, so check where it lives.

**The writer sets the ceiling here.** Each run logs a 50,000-entry burst while a
FileSink formats it, which is slower than the producers. Whatever the writer
has not drained must wait in memory: a 4 MB thread ring holds about 32,000
medium entries or 18,000 large ones, and the rest spill to the shared rings.
Large messages fill a ring fastest, which is why they gain the least.

### Throughput (ops/sec, higher is better)

**Small messages**: one literal, no arguments:

| Threads | slick-logger | slick (shared) | spdlog_async | spdlog_sync | std_ofstream |
|--------:|-------------:|---------------:|-------------:|------------:|-------------:|
| 1 | 14,352,792 | 12,114,714 | 4,972,223 | 1,462,648 | 68,728 |
| 2 | 12,798,460 | 8,261,324 | 2,977,428 | 946,902 | n/a |
| 4 | 17,748,816 | 11,268,640 | 2,369,683 | 696,618 | n/a |
| 8 | 18,913,439 | 13,036,420 | 2,024,661 | 519,768 | n/a |

**Medium messages**: three arguments (int, double, string):

| Threads | slick-logger | slick (shared) | spdlog_async | spdlog_sync | std_ofstream |
|--------:|-------------:|---------------:|-------------:|------------:|-------------:|
| 1 | 3,766,374 | 3,373,209 | 910,457 | 523,017 | 56,860 |
| 2 | 5,328,348 | 4,354,450 | 1,511,183 | 351,020 | n/a |
| 4 | 6,706,990 | 5,176,815 | 1,523,838 | 282,248 | n/a |
| 8 | 7,484,766 | 5,954,902 | 1,542,377 | 230,245 | n/a |

**Large messages**: eleven arguments:

| Threads | slick-logger | slick (shared) | spdlog_async | spdlog_sync | std_ofstream |
|--------:|-------------:|---------------:|-------------:|------------:|-------------:|
| 1 | 2,184,524 | 2,191,357 | 319,465 | 175,137 | 48,659 |
| 2 | 3,196,001 | 3,101,373 | 591,170 | 130,251 | n/a |
| 4 | 3,506,901 | 3,178,382 | 999,338 | 104,579 | n/a |
| 8 | 3,500,203 | 3,318,504 | 1,333,143 | 90,518 | n/a |

The baseline only runs single-threaded; a shared `std::ofstream` across threads
is not a meaningful comparison.

With thread rings, producers share no cache line, so adding threads no longer
costs each call a contended reservation. The shared rings dip at two threads,
where each call starts paying for that contention without enough parallelism
to cover it.

### Latency (ns per call, lower is better)

Producer-side cost of a `LOG_*` call: the time to stamp and enqueue, not the
time until the line reaches disk. Total elapsed time over 50,000 calls:

| Message size | slick-logger | slick (shared) |
|--------------|-------------:|---------------:|
| Small | 153 | 162 |
| Medium | 290 | 339 |
| Large | 527 | 501 |

From `latency_benchmark`, which times each call individually (10,000 samples):

| Metric | slick-logger | slick (shared) | spdlog_sync |
|--------|-------------:|---------------:|------------:|
| Mean | 148 ns | 135 ns | 1,186 ns |
| Median | 100 ns | 100 ns | 400 ns |
| P95 | 200 ns | 200 ns | 600 ns |
| P99 | 300 ns | 200 ns | 19,900 ns |
| P99.9 | 1,500 ns | 1,400 ns | 84,400 ns |
| Max | 102,300 ns | 47,600 ns | 650,200 ns |

`spdlog_sync`'s P99 is 50× its median, because a synchronous call eventually
waits on the file. SlickLogger's P99 is 3× its median: the writer thread
absorbs the I/O. The slick means differ by one outlier: 102 µs over 10,000
samples is 10 ns of the mean.

**Timer granularity.** `latency_benchmark` reports a 100 ns minimum and median
because Windows' `high_resolution_clock` ticks at 100 ns. Per-call figures below
roughly 100 ns cannot be resolved by this harness, which is why 99.5% of its
samples land in the 100–500 ns bucket. The aggregate figures above, total
elapsed time divided by iterations, are the more precise measurement.

**Latency under background load** (`latency_benchmark`):

| Background load | slick-logger mean | P99 | slick (shared) mean | P99 |
|-----------------|------------------:|----:|--------------------:|----:|
| idle | 120 ns | 200 ns | 137 ns | 200 ns |
| 1,000 msg/sec | 179 ns | 300 ns | 157 ns | 400 ns |
| 5,000 msg/sec | 132 ns | 200 ns | 195 ns | 1,100 ns |
| 10,000 msg/sec | 148 ns | 200 ns | 195 ns | 300 ns |

### Thread scaling (`throughput_benchmark`)

Medium messages, a separate harness from the tables above, so absolute numbers
differ:

| Threads | slick-logger | Memory MB | slick (shared) | Memory MB | spdlog_async |
|--------:|-------------:|----------:|---------------:|----------:|-------------:|
| 1 | 4,999,575 | 8 | 5,271,398 | 5 | 2,162,433 |
| 2 | 6,017,662 | 11 | 4,744,565 | 10 | 802,627 |
| 4 | 8,597,898 | 28 | 8,020,758 | 16 | 368,899 |
| 8 | 13,269,243 | 48 | 10,764,654 | 16 | 123,210 |
| 16 | 17,789,638 | 80 | 14,870,110 | 16 | 117,026 |

SlickLogger's total keeps climbing through 16 threads, while `spdlog_async`'s
drops 18× from one thread to sixteen. With thread rings, memory follows the
number of logging threads: each ring is faulted in as far as it fills, up to
`thread_ring_size` (4 MB).

**Burst handling**: five bursts of 50,000 messages, one second apart:

| Logger | Mean | StdDev |
|--------|-----:|-------:|
| slick-logger | 4,169,366 ops/sec | 582,238 |
| slick (shared) | 3,962,112 ops/sec | 124,491 |
| spdlog_async | 1,429,273 ops/sec | 46,067 |

### Memory (`memory_benchmark`)

| Queue size | slick-logger peak MB | slick (shared) peak MB | spdlog_async peak MB |
|-----------:|---------------------:|-----------------------:|---------------------:|
| 1,024 | 9 | 5 | 0 |
| 8,192 | 11 | 8 | 3 |
| 65,536 | 29 | 36 | 25 |
| 262,144 | 94 | 106 | 102 |

**Read the peak columns, and ignore the `Bytes/Msg` and `Efficiency` columns the
tool prints.** Those divide peak memory by the message count, which says little
about a logger whose rings are allocated once up front.

The queue size is the shared entry ring's. With thread rings it takes only
spills and threads past `max_thread_rings`, and each logging thread adds a ring
that is faulted in as it fills. The sustained-load test logged 299,956 messages
at 10,000 msg/sec with a 4 MB peak and 4 MB final (16 MB on the shared rings),
and ten fragmentation cycles produced 0 MB of growth.

The string ring is a `slick::queue<char>`, and slick-queue keeps a 16-byte
control slot per reservation unit. Before `LogConfig::string_items_per_slot`
existed the unit was one byte, so the default 16 MB `string_buffer_size`
carried a 256 MB control array. The default unit is now 64 bytes, which shrinks
that array to 4 MB.

### Thread ring size (`thread_ring_size`)

The main suite with the ring size set through `LogConfig`, before the default
was settled (single runs, ops/sec):

| Case | shared rings | 1 MB | **4 MB (default)** | 16 MB |
|------|-------------:|-----:|-------------------:|------:|
| Small, 1 thread | 11,931,287 | 11,648,209 | 11,800,272 | 11,442,979 |
| Small, 4 threads | 10,875,066 | 17,508,624 | 17,773,322 | 18,071,581 |
| Medium, 1 thread | 3,461,599 | 2,985,625 | 3,290,925 | 3,224,863 |
| Medium, 4 threads | 5,165,592 | 6,260,068 | 6,623,756 | 6,680,845 |
| Large, 1 thread | 2,119,881 | 1,926,121 | 2,077,526 | 1,998,514 |
| Large, 4 threads | 3,038,153 | 3,205,435 | 3,623,437 | 3,640,084 |

A bigger ring spills less, but the first lap of a fresh ring takes its page
faults on log calls, and this suite starts every run with fresh rings. 4 MB was
within a few percent of the best at every thread count. A long-running program
pays those faults once per ring; `Logger::prepare_thread()` pays them up front.

### String ring unit size (`string_items_per_slot`)

A separate scratch harness, run before thread rings, so every string went
through the string ring: 16 MB string ring, 65,536-entry queue, a null sink,
and each producer thread logging `"msg {} {} {}"` with an int, a 12-byte string
and a 40-byte string. Figures are the median producer-side cost of one log call
over five rounds of 400,000 calls per thread. Peak is the process working set.

| string_items_per_slot | 1 thread | 4 threads | 8 threads | Peak MB |
|---------------------:|---------:|----------:|----------:|--------:|
| 1 | 273 ns | 811 ns | 2,132 ns | 300 |
| 16 | 164 ns | 666 ns | 1,771 ns | 60 |
| 32 | 163 ns | 655 ns | 1,769 ns | 52 |
| **64 (default)** | 166 ns | 575 ns | 989 ns | 48 |
| 128 | n/a | n/a | 1,015 ns | 46 |

Moving from 1 byte to 16 fixes most of the memory cost. The gain from 32 to 64
is under contention: at 64 every string starts on its own cache line, so
producers no longer false-share the lines they copy into. Going past 64 gains
nothing more and only makes short strings more wasteful.

## Building

The benchmarks are gated on **two** conditions — the option, and a Release
build type. Miss either and `benchmarks/` is silently skipped, with no error:

```bash
cmake -S . -B build-bench -DBUILD_SLICK_LOGGER_BENCHMARKS=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build-bench --config Release
```

`-DCMAKE_BUILD_TYPE=Release` is required even for multi-config generators
(Visual Studio, Ninja Multi-Config), because the top-level `CMakeLists.txt`
tests that variable directly:

```cmake
if(BUILD_SLICK_LOGGER_BENCHMARKS AND CMAKE_BUILD_TYPE MATCHES Release)
    add_subdirectory(benchmarks)
endif()
```

The first configure clones and builds spdlog and fmt, which takes a few
minutes; later configures reuse them.

## Running

Executables land in `build-bench/benchmarks/Release/` (multi-config) or
`build-bench/benchmarks/` (single-config).

Run them from a directory on **local storage** — each writes its log files to
`benchmark_logs/` relative to the working directory:

```bash
cd /path/to/local/scratch
/path/to/build-bench/benchmarks/Release/slick_logger_benchmark.exe
```

The main suite takes roughly 10 minutes at default settings. Pass a smaller
iteration count and run count for a quick check:

```bash
# iterations, runs
slick_logger_benchmark 5000 1
```

`run_benchmarks.sh` / `run_benchmarks.bat` wrap all four programs and accept
`quick` or `full`. They locate the build tree next to the repository, so they
inherit whatever storage that tree sits on.

## Benchmark Programs

### slick_logger_benchmark

The main suite: throughput across three message sizes and 1/2/4/8 threads,
aggregate latency, and a coarse memory comparison. Takes `[iterations] [runs]`
(defaults 50,000 and 3).

```
Testing with 1 thread(s):
Library                     Mean      Median         P95         P99      StdDev
--------------------------------------------------------------------------------
slick-logger             7669915     7078844     7078844     7078844    844687.7
spdlog_sync              1298350     1332406     1332406     1332406    118814.6
spdlog_async             3765386     3825643     3825643     3825643    518847.3
std_ofstream               62700       62549       62549       62549      2653.1

Unit: ops/sec
```

With the default 3 runs, P95 and P99 are taken from three samples, so they
equal the maximum. They only become meaningful with a larger `runs` argument.

### latency_benchmark

Per-call latency with a distribution histogram, a warmup-vs-steady-state
comparison, and latency under background load.

```
=== Call Latency (ns) ===
Samples: 10000
Mean:    173.22
Median:  200.00
Min:     100.00
Max:     42800.00
StdDev:  571.48
P95:     200.00
P99:     300.00
P99.9:   2600.00

Latency Distribution:
0-100ns     :      0 (0.0%)
100-500ns   :   9946 (99.5%)
500ns-1us   :     21 (0.2%)
1-5us       :     29 (0.3%)
5-10us      :      2 (0.0%)
10-50us     :      2 (0.0%)
```

Its "Timeline Analysis" compares the first 100 calls against the last 100 and
prints a "degraded by N%" line. Treat that as noise: 100 samples at 100 ns
timer resolution is far too small a window to conclude anything from, and it
fires for spdlog too.

### throughput_benchmark

Thread scaling from 1 to 16, with CPU and memory sampling, plus the burst test.

```
=== SCALING ANALYSIS ===
Logger          Threads  Throughput     CPU % Memory MB  Efficiency
---------------------------------------------------------------------------
SlickLogger           1     3461010     169.8         1      100.0%
SlickLogger           2     4015298     249.9         2       58.0%
SlickLogger           4     6052820     455.3         4       43.7%
SlickLogger           8     8607713     792.5        11       31.1%
SlickLogger          16     9359610     791.6        16       16.9%
```

### memory_benchmark

Peak memory across four queue sizes, a 30-second sustained-load test, and a
fragmentation check. Runs for about a minute.

```
=== SUSTAINED LOAD MEMORY TEST ===
Running sustained load test for 30 seconds at 10000 msgs/sec
SlickLogger sustained load results:
  Messages logged: 300006
  Peak memory: 5 MB
  Final memory: 5 MB
```

`simple_benchmark` and `quick_benchmark` also build. They are scratch programs
for checking that a build works, not measurement tools — they report a single
timing with no statistics.

## Getting Trustworthy Numbers

- Build Release. A Debug build measures nothing useful.
- Write logs to local storage (see above).
- Close other applications; a background compile moves these numbers by more
  than most of the differences being measured.
- Raise `runs` above the default 3 before reading P95/P99.
- Check for thermal throttling on laptops — the 5900HX here sustains its clocks
  for a single suite run, but back-to-back runs drift.

## Extending

To add a library, derive from `BenchmarkScenario` in `benchmark_main.cpp`:

```cpp
class MyLoggerScenario : public BenchmarkScenario {
public:
    MyLoggerScenario(MessageSize s) : BenchmarkScenario("my_logger"), msg_size_(s) {}
    void setup() override      { /* construct, warm up */ }
    void cleanup() override    { /* flush and destroy */ }
    void log_single_message() override { /* one call, no timing code */ }
};
```

Then add it to `run_throughput_benchmarks()` and `run_latency_benchmarks()`,
and link the dependency in `benchmarks/CMakeLists.txt`.

`setup()` and `cleanup()` are outside the timed region. Put warmup in `setup()`
and any flush or shutdown in `cleanup()` — a flush left in `log_single_message()`
measures the disk instead of the library.

## License

Part of SlickLogger; same MIT license.
