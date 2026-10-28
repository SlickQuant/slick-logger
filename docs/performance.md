# Performance

These are producer-side numbers: what a `LOG_*` call costs the thread that makes it.

## Benchmark Highlights

Recorded on an AMD Ryzen 9 5900HX, Windows 11, MSVC 19.44 `/O2`, with log output on a local NVMe SSD (2026-10-22):

| Producer-side measurement | Result |
| --- | ---: |
| Literal message, 1 producer | **14.35 million calls/sec** |
| Literal message, 8 producers | **18.91 million calls/sec** |
| Three-argument message, 8 producers | **7.48 million calls/sec** |
| Mean call latency, literal message | **153 ns** |

With 4 producer processes of 2 threads each, a call took about 185-200 ns with thread rings in shared memory, against 800-950 ns through the shared queue ([multi-process results](https://github.com/SlickQuant/slick-logger/blob/main/benchmarks/README.md#multi-process)).

Call timings exclude the writer's drain and flush time. A burst a thread ring cannot hold spills to the shared rings, which can overwrite their oldest entries when overloaded. The benchmark README has the [full results, comparisons, and methodology](https://github.com/SlickQuant/slick-logger/blob/main/benchmarks/README.md#results), plus [build/run instructions](https://github.com/SlickQuant/slick-logger/blob/main/benchmarks/README.md#building) for reproducing the numbers on your own hardware.
