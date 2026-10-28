# Binary and encoded logs

`BinarySink` writes raw bytes; `EncodedFileSink` writes entries in binary and leaves the
formatting to a decoder. Both share the lock-free queue and the writer thread with the
text sinks described under [Sinks](sinks.md).

| To capture | Sink | Read the file with |
| --- | --- | --- |
| Packet or application payload bytes | [BinarySink](#binary-logging) | Your application's parser |
| Log entries with timestamps and arguments | [EncodedFileSink](#encoded-log-files) | `slick_log_decode` or `EncodedLogReader` |

## Binary logging

`BinarySink` writes raw bytes to a file exactly as given, so an application can capture packets, wire messages, or serialized records through the same asynchronous queue as its text logs, with no second I/O path and no formatting on the writer thread.

Wrap bytes in `as_binary()` and log them like any other argument:

```cpp
#include <slick/logger.hpp>

using namespace slick::logger;

#pragma pack(push, 1)
struct Trade {
    uint64_t timestamp_ns;
    uint32_t quantity;
    double   price;
    char     symbol[8];
};
#pragma pack(pop)

int main() {
    auto& logger = Logger::instance();
    auto capture = logger.add_binary_sink("trades.bin", "capture");
    logger.init(1 << 16);

    Trade trade{1'725'000'000'000'000'000ULL, 100, 431.25, {'A','A','P','L'}};

    // A POD, by address and size
    LOG_SINK_INFO(capture, "{}", as_binary(&trade, sizeof(trade)));

    // Or any contiguous range: vector, array, span
    std::vector<uint8_t> frame{0xFE, 0xED, 0x00, 0x0A};
    LOG_SINK_INFO(capture, "{}", as_binary(frame));

    logger.shutdown();   // trades.bin is exactly 28 + 4 bytes
}
```

`add_binary_sink()` returns the sink so it can go straight into `LOG_SINK_*`.

### What lands in the file

Only the binary payloads. Timestamp, level, format string and non-binary arguments are all ignored, and the file is exactly the concatenation of what you encoded. Entries with no binary argument write nothing. The file has no framing of its own, so if you need to walk it back record by record, encode that yourself or override `write_payload()`:

```cpp
class LengthPrefixedBinarySink : public BinarySink {
public:
    using BinarySink::BinarySink;
protected:
    void write_payload(const std::byte* data, size_t size) override {
        const auto length = static_cast<uint16_t>(size);
        BinarySink::write_payload(reinterpret_cast<const std::byte*>(&length), sizeof(length));
        BinarySink::write_payload(data, size);
    }
};
```

#### Text sinks

Text sinks render payloads as hex, so the same call can feed a binary capture and a readable log. `{}` gives lowercase hex, `{:X}` uppercase, and `{:.N}` renders at most N bytes. The default cap is 64 bytes, after which the total byte count is appended (`0a1bff...(100 bytes)`).

#### Payloads larger than 64KB

A single payload is capped at `slick::logger::kMaxPayloadBytes` (65534) and truncated beyond it, because slick-queue packs its reservation size into 16 bits. To log more, pass the buffer as several arguments of a single log call:

```cpp
constexpr auto kCap = kMaxPayloadBytes;
LOG_SINK_INFO(capture, "{}{}{}",
              as_binary(p,            kCap),
              as_binary(p + kCap,     kCap),
              as_binary(p + 2 * kCap, size - 2 * kCap));
```

`BinarySink` writes an entry's payloads back to back with nothing between them, so the file is byte-identical to the unsplit buffer.

Do not spread the chunks over separate log calls. The queue is multi-producer, so another thread's entry can land between them and interleave the two payloads in the file. One entry is the unit of atomicity, which caps a single payload at `SLICK_LOGGER_MAX_ARGS * kMaxPayloadBytes` (~1.28 MB by default; raise `SLICK_LOGGER_MAX_ARGS` for more).

#### Notes

- The bytes are copied into the string ring while the log call runs, so the source buffer only has to outlive that call.
- `as_binary()` over a range copies the element bytes as they are, padding included.
- Binary payloads cross process boundaries like any other argument, so a shared-memory collector can own the `BinarySink`.
- A `BinarySink` is dedicated by default, so broadcast `LOG_*` entries do not reach it. Call `set_dedicated(false)` to change that.

Runnable example: [`examples/binary_sink_example.cpp`](https://github.com/SlickQuant/slick-logger/blob/main/examples/binary_sink_example.cpp).

## Encoded log files

`EncodedFileSink` writes entries in binary and leaves the formatting for later. A text sink spends most of the writer thread's time turning entries into text. This one writes what the entry holds: timestamp, level, thread, pid and tag, source location, and each argument as its bytes. A format string or file name goes in once, and entries refer to it by id after that.

```cpp
auto& logger = Logger::instance();
logger.add_encoded_file_sink("app.slog");
logger.init(1 << 16);
LOG_INFO("order {} filled {} @ {}", id, qty, price);
```

`slick_log_decode` renders the file, each line exactly as `FileSink` would have written it. Build it with `-DBUILD_SLICK_LOGGER_DECODER=ON`.

```bash
slick_log_decode app.slog -o app.log                 # stdout without -o
slick_log_decode app.slog --pattern "%T.%e %l %v"
```

It exits with 1 when a crash cut the last record short, after writing everything before it. In code, `EncodedLogReader` hands back each `LogEntry` for any sink to render:

```cpp
EncodedLogReader reader("app.slog");
FileSink text("app.log");
while (const LogEntry* entry = reader.next()) text.write(*entry);
```

Writer thread cost per entry on Windows/MSVC `/O2`, with the entries already queued when it starts. About 50-100 ns of each is the writer reading and merging the rings.

| Entry | FileSink | EncodedFileSink | Speed-up, 1 / 4 / 8 producers | Bytes per entry, text / encoded |
| --- | ---: | ---: | ---: | ---: |
| No arguments | 292 ns | 133 ns | 2.2x / 2.0x / 2.5x | 70 / 28 |
| Four scalars | 513 ns | 150 ns | 3.4x / 3.2x / 3.5x | 93 / 45 |
| Three scalars and a string | 611 ns | 141 ns | 4.3x / 3.6x / 3.4x | 100 / 64 |
| Eight scalars | 888 ns | 172 ns | 5.2x / 4.6x / 4.4x | 95 / 80 |

Each sink starts its session with a header, so appending to an existing file is fine. It keeps up to 16 MiB of format strings and file names to refer to by id (`set_text_table_limit()`), and writes newer text inline in each entry. A collector's entries arrive with their text copied, so the sink matches it by content, which costs a little more than a `LOG_*` literal matched by its address. The collector writes one with `--encoded <path>`.

`RotatingEncodedFileSink` rotates by size and `DailyEncodedFileSink` by date, with the same `RotationConfig` and file names as `RotatingFileSink` and `DailyFileSink`. Add them with `add_rotating_encoded_file_sink()` and `add_daily_encoded_file_sink()`, or with `--rotating-encoded` and `--daily-encoded` on the collector. Every file starts with a header and defines the texts it uses, so each one decodes alone. Pass several to decode them together, oldest first:

```bash
slick_log_decode app_2.slog app_1.slog app.slog -o app.log
```

The daily sink switches files when an entry's timestamp reaches local midnight, so each entry lands in its own date's file. The check is one comparison per entry.
