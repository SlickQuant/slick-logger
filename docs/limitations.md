# Known limitations

Most of these are deliberate design choices.

## The rings are lossy

- The shared ring has no backpressure. When producers outrun the writer thread, older entries
  are overwritten, and string data can be overwritten before the entry that references it is
  read. Thread rings preserve unread entries: a full ring spills into the shared ring by default,
  or discards the new entry with `RingFullPolicy::Drop`. See
  [Architecture](architecture.md) and [Per-thread rings](configuration.md#per-thread-rings).
- `LogStats::entry_loss_count` counts shared-ring overwrites only when
  `SLICK_LOGGER_ENABLE_LOSS_DETECTION=ON`. This includes entries the writer discards after a
  producer rewrites them during copying. Detection costs the writer a compare per read and an
  atomic add per lossy read; `kLossDetectionEnabled` reports whether it is enabled.
  Entries dropped under `RingFullPolicy::Drop` are counted with either setting, so
  `entry_loss_count` can be non-zero with detection off. See
  [Queue tuning](configuration.md#queue-tuning) and [What is measured](statistics.md#what-is-measured).
- `string_loss_count` is always `0` by construction: slick-queue counts losses inside `read()`,
  and the logger never calls it on the string ring.

## Format strings are stored by pointer

- A `LOG_*` call recognizes a string literal by its type (`const char(&)[N]`) and queues only the
  pointer. Any other `const char[N]` lvalue, such as a class member or a local `const char`
  array, has the same type and is treated the same way, so changing or destroying it before the
  writer thread consumes the entry corrupts the output. Pass `std::string_view{member}` instead.
  See [Formatting](formatting.md#string-formatting-with-stdformat).
- A pre-built `std::format_args` cannot carry types that need their own `std::formatter`
  specialization: those are logged as `<handle>`. Use the variadic call for them. See
  [Passing std::format_args](formatting.md#passing-stdformat_args).

## Fixed limits

- A log call takes at most `SLICK_LOGGER_MAX_ARGS` format arguments (20 by default). Redefine it
  before including `slick/logger.hpp`. See
  [Log macros and levels](getting-started.md#log-macros-and-levels).
- A single binary payload is capped at `kMaxPayloadBytes` (65534) and truncated beyond it,
  because slick-queue packs its reservation size into 16 bits. Split a larger buffer across
  arguments of one call: the cap per entry is
  `SLICK_LOGGER_MAX_ARGS * kMaxPayloadBytes` (~1.28 MB by default). See
  [Payloads larger than 64KB](binary.md#payloads-larger-than-64kb).
- `process_tag` is truncated to 15 bytes, on a UTF-8 character boundary. See
  [Multi-process logging](multi-process.md#multi-process-logging-shared-memory).

## Build settings that must match

- A plugin and its host share one `Logger` object, and processes logging into one shared-memory
  segment share its layout. Either way, all of them must be built from the same slick-logger
  version with the same `SLICK_LOGGER_MAX_ARGS`, and processes on one segment also need the same
  architecture. A layout mismatch throws at attach time in the multi-process case, but is
  undefined behavior across a shared library boundary, where more settings must match. See
  [Shared libraries](shared-libraries.md) and [Constraints](multi-process.md#constraints).
- Turning off source locations, at build time (`SLICK_LOGGER_ENABLE_SOURCE_LOCATION=OFF`) or at
  runtime (`set_source_location_enabled(false)`), leaves entries with no `file:line`.
- Compiling out thread-id capture (`SLICK_LOGGER_ENABLE_THREAD_ID=OFF`) makes
  `set_pattern("%t")` throw and name the option, rather than rendering a flat `0`. See
  [Thread id capture](configuration.md#thread-id-capture).

## Multi-process specifics

- Pointer arguments print addresses that only mean something inside the producing process.
- Each process stamps its own entries with *its* [timestamper](timestamps.md#custom-timestamper),
  so every process needs a clock on the same epoch.
- Sink indices used by `log_to_sink()` and `ISink::log()` resolve against the *collector's* sink
  list.
- On POSIX, segment cleanup is manual by design: slick-logger never `shm_unlink`s the queue,
  string or registry segments. Windows removes them once the last handle closes. The commands
  are in [Constraints](multi-process.md#constraints).
- A producer killed between reserve and publish leaves a hole in the shared ring, which the
  collector abandons after `stalled_entry_timeout_ms` and logs as a warning.

## Not implemented

- `RotationConfig::compress_old` is a placeholder; rotated files are not compressed. See
  [Rotation configuration](sinks.md#rotation-configuration).
