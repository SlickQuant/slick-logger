# Runtime statistics

Both internal rings are lossy: if producers outrun the writer thread, older entries are overwritten. Statistics show that happening before it costs you a log line. A background thread samples the queues and publishes throughput, fullness and drop counts, both as a live snapshot and as rows in a CSV.

It is opt-in, and your logging threads pay nothing either way. Most figures come from cursors the queues already maintain, so calling `LOG_*` touches no counter, enabled or not. (Measured: the difference in enqueue throughput between statistics on and off was smaller than the run-to-run spread of the same configuration, so it could not be resolved either way.)

The one cost lands on the *writer* thread. String-ring occupancy needs a position that only the writer can read safely, so with statistics enabled it reads the two ring reservation cursors once per drained batch and compares one of them against its own read cursor. That is two cursor reads per *batch*, not per entry, and it never inspects entry contents. The cost is small, but it is not zero.

```cpp
using namespace slick::logger;

LogConfig config;
config.sinks.push_back(std::make_shared<FileSink>("app.log"));
config.enable_stats = true;
config.stats_file = "app_stats.csv";
config.stats_interval_ms = 1000;        // one CSV row per second
config.stats_sample_interval_ms = 10;   // sample the gauges 100x per row
config.stats_max_file_size = 16 * 1024 * 1024;

Logger::instance().init(config);

// ... later, from any thread ...
const LogStats stats = Logger::instance().stats_snapshot();
if (stats.entry_queue_pct_max > 80.0) {
    // the queue peaked above 80% this interval - the writer is falling behind
}
```

## Sampling and reporting intervals

Throughput and drop counts are cumulative counters, so they are exact whenever they are read. Queue fullness is an instantaneous gauge, so a burst that drives the queue to 90% and drains again within 200 ms is invisible to a once-per-second read.

The thread therefore samples the gauges every `stats_sample_interval_ms` and reports the peak across each `stats_interval_ms` window:

| Field | Meaning |
| --- | --- |
| `entry_queue_pct` / `string_pct` | a single point sample, taken when the row was written |
| `entry_queue_pct_max` / `string_pct_max` | the peak across the whole interval; alert on this field |
| `sample_count` | how many ticks fed those peaks, so you can see the fidelity behind them |

The sample interval is also how quickly the thread notices shutdown, so a long reporting interval never delays `shutdown()` by more than one tick. It is clamped into `[1, stats_interval_ms]`.

### What is measured

- Throughput: `produced_per_sec` is messages entering the queue, and `consumed_per_sec` is messages leaving it. In steady state the two match; `consumed` lagging `produced` is what makes the depth grow. Both are computed over the *measured* elapsed time, so a late wake-up on a loaded machine never distorts them.
- Entry queue fullness: `entry_queue_depth` is how far the writer thread is behind the producers, against `entry_queue_capacity`.
- String ring occupancy: `string_inflight_bytes` is the span between the frontier (the position below which the ring is provably free) and the producers' write cursor. The logger never calls `read()` on the string ring, so there is no read cursor to subtract, and the frontier is *not* read out of the entries the writer drains. It cannot be. A producer reserves its string bytes while it builds the entry, and its entry slot only at the end, so slot order and string order are two independent races and neither implies the other. A producer holding earlier string bytes can take a later slot. Freeing the ring up to a drained entry's own string would release bytes that an entry still queued behind it owns: the gauge would read idle over a busy ring, and the frontier would jump backwards when that entry finally drained.

  Instead the writer snapshots the entry reservation cursor and *then* the string reservation cursor, and promotes the string half only once its own read cursor has passed the entry half. The order is the proof. Every slot below the snapshot had already been taken, and a string is always reserved before its slot, so every one of those strings was reserved before the string cursor was read. Once the writer has drained past the entry half, the whole ring below the string half is free, whatever order the producers published in. Between promotions the frontier stays put, which errs on the safe side: the span grows and the gauge reads fuller, which is what a writer falling behind looks like.

  The frontier is an absolute reserve index, not a position within the ring, so a ring filled to exactly its capacity reads as `capacity` / `100%` instead of aliasing back to zero, and an overrun shows as a distance past capacity that is then clamped. It is seeded at `start()` with the ring cursor as it stands, so there is always a floor to measure from: before the first promotion nothing has been confirmed drained, and the whole span since start is in flight.

  A collector replaying a backlog is the exception. `collect_backlog` rewinds its entry reader below the point where it attached, so the entries it is about to drain own strings reserved before it existed, and the attach cursor would call every one of them free for the whole replay. Its floor is rewound the same way as the reader, to the oldest position the ring can still hold, so the replay is reported as the in-flight backlog it is.

  `string_pct_valid` covers the whole row, point value and peak alike. With a string ring present a measurement is always available, and a fully drained queue reports a valid zero. The cost is two cursor reads per drained batch on the writer thread (it never inspects entry contents for this) and nothing on the producing thread.

  One window stays invisible: a producer that has reserved string bytes but not yet taken an entry slot appears in neither ordering, so if it is preempted there, its bytes are unaccounted for until it publishes. Closing that gap would mean having producers announce reservations before publishing, which is not worth what it would cost the logging path.
- String ring pressure: `string_bytes_per_sec`, `string_turnover_pct` and `string_wraps_per_sec` describe how fast the ring is recycled. Like `string_inflight_bytes` and `string_bytes_written`, they count ring footprint: each string counts as its size rounded up to `string_items_per_slot`, because that is the space it occupies. `string_turnover_pct` is a *rate*, not an occupancy: 100 means the ring turned over exactly once during the interval.
- Loss: `entry_loss_count` is entries a producer overwrote before the writer could read them, including any it overwrote while the writer was copying one out, which the writer then discards instead of logging it torn. Counting overwrites is opt-in: build with `SLICK_LOGGER_ENABLE_LOSS_DETECTION` (see [Queue Tuning](configuration.md#queue-tuning)). Without it an overwrite goes uncounted rather than being reported as a loss, and `kLossDetectionEnabled` tells the two cases apart. The process that reads the queue does the counting, so in shared-memory mode the collector has the figure and a producer reads `0`. `string_loss_count` is always `0` by construction: slick-queue counts losses inside `read()`, and the logger never calls it on the string ring. With [per-thread rings](configuration.md#per-thread-rings), entries dropped under `RingFullPolicy::Drop` count too, whatever the build option - so even a build without detection can report a non-zero `entry_loss_count`.
- Thread rings: the entry counts and depth include every ring. `thread_ring_count` is how many rings have been allocated this run. Exited threads hand theirs on, so it follows the peak number of logging threads.

### The CSV

The file is opened fresh (truncated) on `init()` with a header row. One row is appended per interval, and the file rolls over to `<stem>.1.csv` when it reaches `stats_max_file_size`. Columns, in order:

```text
timestamp,interval_sec,sample_count,
entries_produced,entries_consumed,produced_per_sec,consumed_per_sec,
entry_queue_depth,entry_queue_depth_max,entry_queue_depth_mean,
entry_queue_capacity,entry_queue_pct,entry_queue_pct_max,
string_inflight_bytes,string_inflight_bytes_max,string_buffer_capacity,
string_pct,string_pct_max,string_pct_valid,
string_bytes_written,string_bytes_per_sec,string_turnover_pct,
string_wraps_per_sec,entry_loss_count,string_loss_count,thread_ring_count
```

Leave `stats_file` empty to run the thread for `stats_snapshot()` alone and write no file. A CSV that cannot be created makes `init()` throw, the same as a sink that cannot be opened. A write failure afterwards is reported once to `stderr` and never takes the process down.

### Notes

- The statistics thread never touches a sink and never calls `log()`, so it cannot disturb what it measures. Sinks stay owned by the writer thread alone. The statistics thread never reads the entry ring either, only cursors and the frontier the writer publishes.
- `stats_snapshot()` returns a copy of an immutable published sample, so any number of threads can call it at once and no reader can see a half-written one. It is not guaranteed wait-free: the sample lives in a `std::atomic<std::shared_ptr>`, which is lock-free only where the implementation says so (MSVC uses an internal lock), so a caller may briefly contend with the once-per-interval publish. It never touches the logging path.
- `shutdown()` drops the last published sample along with the statistics thread, so `stats_snapshot()` reads as a zeroed `LogStats` once the logger is stopped. It stays zeroed through a re-`init()` with `enable_stats` off, or one that throws because the CSV cannot be created. Read the closing numbers *before* shutting down if you need them.
- If the CSV cannot be created, `init()` throws before anything starts, leaving the logger stopped rather than half-initialized.
- In `QueueMode::SharedProducer` there is no local writer thread, so `entries_consumed`, the depth fields and `consumed_per_sec` are reported as `0`; only the collector process can observe consumption. Produced counts and string-ring rates are still real.
