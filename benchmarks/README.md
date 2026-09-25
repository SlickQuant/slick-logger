# SlickLogger Benchmark Suite

Performance benchmarks comparing SlickLogger against other C++ logging libraries.

## Compared Libraries

| Name in results | What it is |
|-----------------|------------|
| `slick-logger` | This library: lock-free queue, deferred formatting, async writer thread |
| `spdlog_sync` | spdlog 1.12.0 writing through `basic_file_sink_mt` on the calling thread |
| `spdlog_async` | spdlog 1.12.0 in async mode (8192-slot queue, one worker thread) |
| `std_ofstream` | Baseline: `fmt::format` into an `std::ofstream` with `std::endl` per line |

spdlog and fmt are fetched automatically by `benchmarks/CMakeLists.txt` (spdlog 1.12.0, fmt 10.1.0).

## Results

Everything below is a real run of the suite at its default settings. Re-run it
yourself before quoting any of it — these numbers describe one machine.

### Measured on

| Property | Value |
|----------|-------|
| CPU | AMD Ryzen 9 5900HX (8 cores / 16 threads, 3.3 GHz base) |
| RAM | 32 GB |
| OS | Windows 11 Pro 26200 |
| Compiler | MSVC 19.44 (toolset 14.44.35207), `/O2 /DNDEBUG`, C++20 |
| Log output | Local NVMe SSD |
| Settings | 50,000 iterations × 3 runs (the defaults) |
| Date | 2026-09-22 |

### Read this before the tables

**Put the log output on a local disk.** Every library here ultimately writes to
a file, so the storage device sets the ceiling for the synchronous ones. The
same suite run with its working directory on an SMB share measured
`spdlog_sync` at 9,021 ops/sec and `std_ofstream` at 202 ops/sec — 144× and
310× below the local-disk numbers in the table below. `slick-logger` barely
moved (8.8M vs 7.7M), because its producer threads never touch the file. That
asymmetry makes a network-mounted run useless for comparison and flattering to
this library. The runner scripts cd into the build tree, so check where your
build tree actually lives.

### Throughput (ops/sec, higher is better)

**Small messages** — one literal, no arguments:

| Threads | slick-logger | spdlog_async | spdlog_sync | std_ofstream |
|--------:|-------------:|-------------:|------------:|-------------:|
| 1 | 7,669,915 | 3,765,386 | 1,298,350 | 62,700 |
| 2 | 6,018,601 | 2,852,539 | 822,326 | — |
| 4 | 8,723,449 | 2,242,966 | 725,643 | — |
| 8 | 10,960,620 | 1,925,261 | 490,994 | — |

**Medium messages** — three arguments (int, double, string):

| Threads | slick-logger | spdlog_async | spdlog_sync | std_ofstream |
|--------:|-------------:|-------------:|------------:|-------------:|
| 1 | 4,750,287 | 899,182 | 575,968 | 59,732 |
| 2 | 4,285,896 | 1,343,762 | 408,128 | — |
| 4 | 4,487,423 | 1,444,199 | 296,298 | — |
| 8 | 5,097,901 | 1,395,898 | 211,542 | — |

**Large messages** — eleven arguments:

| Threads | slick-logger | spdlog_async | spdlog_sync | std_ofstream |
|--------:|-------------:|-------------:|------------:|-------------:|
| 1 | 3,152,905 | 445,112 | 173,023 | 47,921 |
| 2 | 2,738,959 | 735,008 | 147,620 | — |
| 4 | 3,051,647 | 1,121,690 | 107,400 | — |
| 8 | 3,192,537 | 1,219,525 | 82,540 | — |

The baseline only runs single-threaded; a shared `std::ofstream` across threads
is not a meaningful comparison.

The 2-thread dip for `slick-logger` on small messages is reproducible: at one
thread the producer has the queue's cacheline to itself, and at two it starts
paying for the contention without yet having enough parallelism to cover it.

### Latency (ns per call, lower is better)

Producer-side cost of a `LOG_*` call — the time to stamp and enqueue, not the
time until the line reaches disk.

| Message size | Mean | Median |
|--------------|-----:|-------:|
| Small | 169 | 177 |
| Medium | 288 | 302 |
| Large | 337 | 340 |

From `latency_benchmark`, which times each call individually (10,000 samples):

| Metric | slick-logger | spdlog_sync |
|--------|-------------:|------------:|
| Mean | 173 ns | 1,420 ns |
| Median | 200 ns | 500 ns |
| P95 | 200 ns | 900 ns |
| P99 | 300 ns | 32,800 ns |
| P99.9 | 2,600 ns | 92,100 ns |
| Max | 42,800 ns | 156,700 ns |

The tail is the point: `spdlog_sync`'s P99 is 66× its median, because a
synchronous call eventually waits on the file. SlickLogger's P99 is 1.5× its
median — the writer thread absorbs the I/O.

**Timer granularity.** `latency_benchmark` reports a 100 ns minimum and a
median landing exactly on 200 ns because Windows' `high_resolution_clock` ticks
at 100 ns. Per-call figures below roughly 100 ns cannot be resolved by this
harness, which is why its distribution collapses into the 100–500 ns bucket
(99.5% of samples). The aggregate figures in the table above — total elapsed
time divided by iterations — are the more precise measurement.

**Latency under background load** (`latency_benchmark`):

| Background load | Mean | P99 |
|-----------------|-----:|----:|
| idle | 170 ns | 300 ns |
| 1,000 msg/sec | 318 ns | 500 ns |
| 5,000 msg/sec | 209 ns | 300 ns |
| 10,000 msg/sec | 187 ns | 200 ns |

### Thread scaling (`throughput_benchmark`)

Medium messages, a separate harness from the table above, so absolute numbers
differ:

| Threads | slick-logger | Efficiency | spdlog_async | Efficiency |
|--------:|-------------:|-----------:|-------------:|-----------:|
| 1 | 3,461,010 | 100.0% | 1,654,210 | 100.0% |
| 2 | 4,015,298 | 58.0% | 658,008 | 19.9% |
| 4 | 6,052,820 | 43.7% | 255,286 | 3.9% |
| 8 | 8,607,713 | 31.1% | 140,065 | 1.1% |
| 16 | 9,359,610 | 16.9% | 113,637 | 0.4% |

Efficiency is per-thread, so it falls as threads are added even while total
throughput rises. What matters is the direction: SlickLogger's total keeps
climbing through 16 threads, while `spdlog_async`'s total *drops* — 1.65M at
one thread down to 114K at sixteen, a 14× regression under contention.

**Burst handling** — five bursts of 50,000 messages, one second apart:

| Logger | Mean | StdDev |
|--------|-----:|-------:|
| slick-logger | 3,083,616 ops/sec | 56,461 |
| spdlog_async | 1,429,902 ops/sec | 96,855 |

### Memory (`memory_benchmark`)

| Logger | Queue size | Peak MB |
|--------|-----------:|--------:|
| SlickLogger | 1,024 | 5 |
| SlickLogger | 8,192 | 8 |
| SlickLogger | 65,536 | 36 |
| SlickLogger | 262,144 | 105 |
| spdlog_async | 1,024 | 0 |
| spdlog_async | 8,192 | 3 |
| spdlog_async | 65,536 | 25 |
| spdlog_async | 262,144 | 102 |

**Read the peak column, and ignore the `Bytes/Msg` and `Efficiency` columns the
tool prints.** Those divide peak memory by the message count, which says little
about a logger whose rings are allocated once up front.

The string ring is a `slick::queue<char>`, and slick-queue keeps a 16-byte
control slot per reservation unit. Before `LogConfig::string_items_per_slot`
existed the unit was one byte, so the default 16 MB `string_buffer_size`
carried a 256 MB control array, and SlickLogger peaked at 257 MB even with a
1,024-entry queue. The default unit is now 64 bytes, which shrinks that array to
4 MB. The peaks above are mostly the entry queue and the parts of the string
ring actually written to.

Steady-state behaviour is unremarkable by comparison — the sustained-load test
logged 299,905 messages at 10,000 msg/sec with a 16 MB peak and 16 MB final, and
ten fragmentation cycles produced 0 MB of growth.

### String ring unit size (`string_items_per_slot`)

A separate scratch harness: 16 MB string ring, 65,536-entry queue, a null sink,
and each producer thread logging `"msg {} {} {}"` with an int, a 12-byte string
and a 40-byte string. Figures are the median producer-side cost of one log call
over five rounds of 400,000 calls per thread. Peak is the process working set.

| string_items_per_slot | 1 thread | 4 threads | 8 threads | Peak MB |
|---------------------:|---------:|----------:|----------:|--------:|
| 1 | 273 ns | 811 ns | 2,132 ns | 300 |
| 16 | 164 ns | 666 ns | 1,771 ns | 60 |
| 32 | 163 ns | 655 ns | 1,769 ns | 52 |
| **64 (default)** | 166 ns | 575 ns | 989 ns | 48 |
| 128 | — | — | 1,015 ns | 46 |

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
