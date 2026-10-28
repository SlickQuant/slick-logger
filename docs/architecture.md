# Architecture

In Local mode each logging thread writes to a single-producer ring of its own (see [Per-thread rings](configuration.md#per-thread-rings)). One writer thread merges the rings by timestamp and feeds any number of sinks. A shared multi-producer ring (slick-queue) takes the threads past `max_thread_rings` and whatever a full ring spills:

```text
[Thread 1] ──► [ring 1] ──┐
[Thread 2] ──► [ring 2] ──┤
[Thread N] ──► [ring N] ──┼──► [Writer Thread: merge by timestamp] ──┬──► ConsoleSink
 spills, overflow threads ──► [shared ring] ──┘                      ├──► FileSink
                                                                     ├──► RotatingFileSink
                                                                     ├──► DailyFileSink
                                                                     └──► BinarySink
```

With `thread_rings = false` every thread logs through the shared ring, as in earlier releases.

The same structure spans processes in shared memory: each producer process keeps its threads'
rings in a segment of its own, all of them use the one shared ring, and the writer thread
lives in the collector. See [Multi-Process Logging](multi-process.md#multi-process-logging-shared-memory).

```text
[Process A rings] ──┐
[Process B rings] ──┼──► [Collector: Writer Thread, merge by timestamp] ──► Sinks
[Shared Ring]     ──┘
```

## Design Principles

1. One dedicated writer thread handles all sink operations.
2. Caller threads never block on I/O, and with per-thread rings they share no cache line with each other.
3. Custom sinks are easy to add.
4. Queue and sink management are thread-safe, built on atomic operations.
5. In shared-memory mode, string references in the shared queue are ring indices instead of addresses, so an entry stays valid in any attached process. A thread ring record addresses its inline strings in its producer's mapping; the collector shifts those addresses into its own and checks each against the record's bounds.

### Deferred Formatting

The logger defers string formatting to the background thread:

1. The caller thread captures the format pointer, source location, thread id, and owned copies of any dynamic string data, then writes the `LogEntry` straight into its ring. It writes the header and only the arguments the call has, not all `SLICK_LOGGER_MAX_ARGS` of them. In a thread's own ring the string bytes go into the same record, so a call is one reservation and one publishing store.
2. The rings hold the entries. A producer never waits for space: a full thread ring spills to the shared ring (or drops, see `RingFullPolicy`), and the shared ring overwrites its oldest entries.
3. A thread ring hands a record back only after the writer has dispatched it. From the shared ring the writer copies each entry out of its slot first, so a producer that has lapped the ring can never change an entry while a sink is formatting it; a copy the writer cannot prove predates such a rewrite is discarded and counted as lost. The writer then formats the message *and the line layout*, and writes to all matching sinks. The message is formatted at most once per entry, however many sinks render it, and only if one of them needs it.

Formatting and I/O are the expensive parts, and this keeps both off the calling thread, so log calls stay cheap enough for high-frequency logging.

Message format strings get the same treatment. A `LOG_*` format is a string literal, so the writer thread parses its `{}` fields the first time it sees one and replays that parse for every later line. Nothing scans for braces or rebuilds a per-field spec again, which leaves only the arguments themselves to format. The parse is keyed by the literal's address, length and argument count, and checked against a copy of its text, so a plugin reloaded with different text at the same address is parsed again instead of misread. Runtime (non-literal) format strings live in a reused ring slot and are parsed per line.

The parse also records which fields are a bare `{}` (or `{1}` once its index is dropped), and those skip `std::format` altogether. Integers and floating point go through `std::to_chars` in the shortest round-trip form, which is exactly how `std::format` defines `{}`. Pointers become `0x`-prefixed hex, and text is appended as it is. Values whose `{}` rendering is not a plain conversion (infinity, NaN, a null string, binary payloads) still go through `std::format`, and the output is identical either way; it is tested value for value against `std::format` on MSVC, GCC and Clang. Measured on Windows/MSVC `/O2`, minimum of 21 in-process runs, formatting the message alone:

| Message | Parsed every line | Cached parse + bare `{}` fast path |
|---|---|---|
| `"user {} logged in from {}"` | 378 ns | 106 ns |
| `"order {} filled {} @ {:.2f} side={} venue={}"` | 1061 ns | 474 ns |
| `"[{:>8}] {:08x} {} {} {} {}"` | 1430 ns | 893 ns |

Fields with a spec (`{:.2f}`, `{:>8}`) still cost a `std::format` call each, so prefer `{}` on a hot path when the default rendering will do.

Line patterns follow the same principle: `set_pattern()` parses the pattern once, at configuration time, into a flat list of ops. The caller thread never sees it, and the writer thread renders it by walking that list into a buffer each sink reuses, with no parsing and, in steady state, no allocation per line. See [Log Pattern Formatting](formatting.md#log-pattern-formatting).

### Multi-Sink Benefits

One log call can reach the console and several files at once. Each sink can have its own rotation policy, sink types can be mixed freely, and a single writer thread serves them all.

## Integration

When slick-queue is not already installed, CMake downloads it from <https://github.com/SlickQuant/slick-queue>.

## Header-Only Benefits

There is nothing to link and no `.lib`/`.a` files to distribute or manage. Add the headers to your project and start logging with a single `#include <slick/logger.hpp>`. The compiler also sees every template instantiation, which gives it more room to optimize.
