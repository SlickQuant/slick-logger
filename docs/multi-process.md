# Multi-process logging

## Multi-Process Logging (Shared Memory)

Several processes can log into shared memory that one collector process drains. The
collector owns the sinks and the writer thread; producer processes own neither and only
enqueue entries. Each producer thread writes to a ring in its process's own segment, and
a shared ring takes their overflow (see [below](#thread-rings-across-processes)).

```text
Process A: [ring] [ring] ──┐
Process B: [ring] [ring] ──┼──► [Collector: merge by timestamp] ──┬──► ConsoleSink
overflow ──► [Shared Ring] ┘                                      └──► FileSink
```

### Collector

The collector needs at least one sink, like a single-process logger:

```cpp
#include <slick/logger.hpp>
using namespace slick::logger;

LogConfig config;
config.mode = QueueMode::SharedCollector;
config.shared_memory_name = "myapp_log";
config.sinks.push_back(std::make_shared<ConsoleSink>());
config.sinks.push_back(std::make_shared<FileSink>("collected.log"));

Logger::instance().init(config);
// ... run until stopped ...
Logger::instance().shutdown();   // drains queued entries, then closes the sinks
```

### Producer

A producer must not define sinks. `init()` throws if it does, since the collector owns them:

```cpp
LogConfig config;
config.mode = QueueMode::SharedProducer;
config.shared_memory_name = "myapp_log";   // same name as the collector
config.process_tag = "feed";               // optional, shows up as [pid:feed]

Logger::instance().init(config);

LOG_INFO("order {} filled at {}", order_id, price);   // goes to the collector

Logger::instance().shutdown();
```

All the usual `LOG_*` macros, sinks-by-index, log levels, and source locations work unchanged.
In a producer with thread rings, `flush()` returns once the collector has taken everything
logged before it, or at once when no collector is attached.

### Output

Multi-process entries carry the producing process id and optional tag, right after the level:

```text
2026-08-28 16:37:53.673071 [INFO] [103836:app1] [main.cpp:81] work item 1 of 4
2026-08-28 16:37:53.683942 [INFO] [72176:app2] [main.cpp:81] work item 1 of 4
```

Single-process (`QueueMode::Local`) output is unchanged. Local entries carry no process id,
so nothing extra is printed.

### Standalone collector executable

Instead of embedding a collector, you can run the bundled one. Prebuilt binaries for Linux,
macOS, and Windows are attached to each [release](https://github.com/SlickQuant/slick-logger/releases)
as `slick-log-collector-<version>-<platform>`.

To build it from source instead (it is not built by default):

```bash
cmake -S . -B build -DBUILD_SLICK_LOGGER_COLLECTOR=ON
cmake --build build

./build/tools/slick_log_collector --name myapp_log --console --file collected.log
```

```text
slick_log_collector --name <shm-name>
                    [--console] [--file PATH] [--rotating PATH] [--daily PATH]
                    [--encoded PATH] [--rotating-encoded PATH] [--daily-encoded PATH]
                    [--max-size BYTES] [--max-files N]
                    [--level trace|debug|info|warn|error|fatal]
                    [--queue-size N] [--string-buffer-size N]
                    [--stall-timeout-ms N]
```

It stops on Ctrl-C (`SIGINT`/`SIGTERM`), draining whatever is still queued before exiting.

### Configuration

| Field | Default | Meaning |
| --- | --- | --- |
| `mode` | `QueueMode::Local` | `Local`, `SharedProducer`, or `SharedCollector` |
| `shared_memory_name` | `""` | Segment name, required for the shared modes. 1-24 characters of `[A-Za-z0-9_]` |
| `process_tag` | `""` | Optional short process label, truncated to 15 characters |
| `collect_backlog` | `true` | Collector replays entries already resident in the ring when it attaches |
| `stalled_entry_timeout_ms` | `5000` | Collector abandons an unpublished slot after this long; `0` disables |
| `thread_rings` | `true` | Producer threads log into rings in shared memory |
| `shared_thread_rings` | `16` | Rings in a producer's segment, at most 64; further threads use the shared ring |
| `shared_thread_ring_size` | 1 MB | Bytes per ring, rounded up to a power of two |

### Thread rings across processes

A producer claims a slot in the registry, `<name>.reg`, and creates its own segment,
`<name>.r<slot>`, holding `shared_thread_rings` rings. They work as in
[Local mode](configuration.md#per-thread-rings): no two threads share a cache line, a record carries its
strings, and a full ring spills to the shared ring without breaking the thread's order. The
collector finds producers through the registry and merges every ring and the shared ring by
timestamp.

- Each producer reserves `shared_thread_rings × shared_thread_ring_size` (16 MB by default).
  Pages are faulted in as the rings fill; `prepare_thread()` faults in the calling thread's
  ring up front.
- A record becomes visible with one store, so a producer that dies never leaves a torn one.
  The collector checks producers by pid and process start time, so a pid the OS reuses is
  never mistaken for a live producer. Once a dead or closed producer's rings are drained, the
  collector unlinks its segment and frees its slot. Every entry the producer spilled carries
  its pid, so a replayed spill keeps that pid even after the slot has gone to another producer.
- A restarted collector reads only what the previous one had not taken, or nothing with
  `collect_backlog = false`.
- The registry has 64 slots. A producer that finds it full logs through the shared ring and
  warns once.
- One collector per segment: a second one fails in `init()` while the first is running.
- On Windows a segment disappears with its last handle. A producer's `shutdown()` waits up
  to a second for the collector to take what is left, and the records of a producer killed
  before any collector attached are lost.
- A burst bigger than a ring spills to the shared ring, which is contended and lossy. Size
  `shared_thread_ring_size` for your bursts.

### Startup order and sizing

Startup order does not matter. Whichever process starts first creates the segments and fixes
their capacity from its `log_queue_size` / `string_buffer_size` / `string_items_per_slot`. Every
later process attaches and inherits those settings, so mismatched settings between processes
are harmless.

A collector that attaches to a segment already holding entries replays whatever is still
resident in the ring, so producers can run before any collector exists. Set
`collect_backlog = false` if a restarting collector must not re-emit entries a previous
instance already wrote.

### Constraints

- All processes must use the same slick-logger version, the same `SLICK_LOGGER_MAX_ARGS`,
  and the same architecture. `sizeof(LogEntry)` is recorded in the shared-memory header, so a
  mismatch throws at attach time instead of corrupting data. A string segment created with
  `string_items_per_slot` other than `1` is also refused by slick-logger builds on slick-queue 2.0
  or older, which cannot read that layout. `SLICK_LOGGER_ENABLE_THREAD_ID`,
  `SLICK_LOGGER_ENABLE_LOSS_DETECTION` and `SLICK_LOGGER_ENABLE_CPU_RELAX` do not need to
  match, because none of them changes the segment layout.
- The shared ring is lossy, with no backpressure. A collector that cannot keep up loses
  entries, and string data can be overwritten before the entry referencing it is read. Thread
  rings never overwrite; a full one spills into the shared ring. Size `string_buffer_size`
  generously for high-volume logging, and build the collector with
  `SLICK_LOGGER_ENABLE_LOSS_DETECTION` to count lost entries.
- Pointer arguments (`ArgType::PTR`) print addresses that only mean something inside the
  producing process.
- Each process stamps its own entries. A producer's entries carry the time from *its*
  [timestamper](timestamps.md#custom-timestamper), not the collector's, so give every process a clock on the
  same epoch.
- Sink indices used by `log_to_sink()` and `ISink::log()` are resolved against the
  *collector's* sink list.
- A producer killed between reserve and publish leaves a hole in the shared ring. The
  collector abandons it after `stalled_entry_timeout_ms` and logs a warning instead of
  stalling forever.
- `process_tag` is truncated to 15 bytes, on a UTF-8 character boundary so a multi-byte tag is
  never cut mid-sequence.
- POSIX cleanup is manual, by design. slick-logger never `shm_unlink`s the queue, string or
  registry segment. Unlinking
  frees the *name* while existing mappings stay valid, so whichever process did it would strand
  everyone still attached: newcomers would create a fresh segment under the same name and their
  entries would silently vanish. Any process may create the segment, so none of them can know
  it is the last user. Segments therefore persist in `/dev/shm/` until removed:

  ```bash
  rm -f /dev/shm/myapp_log /dev/shm/myapp_log_str /dev/shm/myapp_log.reg /dev/shm/myapp_log.r*
  ```

  A producer's ring segment is the exception. Once that producer has closed or died and the
  collector has drained its rings, the collector unlinks the segment.

  Remove the segments only when no participant is running. A stale segment from a previous run is
  attached to rather than recreated, and `collect_backlog` will replay whatever entries it still
  holds, so clear it between unrelated runs or set `collect_backlog = false`. On Windows the
  kernel refcounts the mapping and removes it once the last handle closes, so there is nothing
  to clean up.

See `examples/multi_process_example.cpp` for a runnable version of both roles.
