# Formatting

Message formatting with `std::format`, and the line pattern the writer thread renders.

## String Formatting with std::format

slick-logger formats messages with C++20's `std::format`:

```cpp
#include <slick/logger.hpp>

int main() {
    slick::logger::Logger::instance().init("app.log");

    // Basic placeholders
    LOG_INFO("Simple message: {}", "hello");
    LOG_INFO("Number: {}", 42);

    // Multiple arguments
    LOG_INFO("User {} has {} items", "Alice", 15);

    // Numbered (positional) placeholders, with or without a format spec
    LOG_INFO("{0} beat {1}, so {0} advances", "Alice", "Bob");
    LOG_INFO("{1:>8} | {0:.2f}", 3.14159, "label");

    // Dynamic width and precision, taken from an argument
    LOG_INFO("[{:{}}]", 42, 8);                  // [      42]
    LOG_INFO("[{:{}.{}f}]", 3.14159, 9, 2);      // [     3.14]
    LOG_INFO("[{0:{1}}]", 42, 8);                // [      42]

    // Format specifiers (same as std::format)
    LOG_INFO("Price: ${:.2f}", 29.99);           // Currency with 2 decimals
    LOG_INFO("Progress: {:.1f}%", 85.7);         // Percentage with 1 decimal
    LOG_INFO("Hex value: 0x{:x}", 255);          // Hexadecimal
    LOG_INFO("Binary: 0b{:b}", 42);              // Binary
    LOG_INFO("Scientific: {:.2e}", 12345.67);    // Scientific notation

    // Width and alignment
    LOG_INFO("Right aligned: {:>10}", "text");   // Right align in 10 chars
    LOG_INFO("Left aligned: {:<10}", "text");    // Left align in 10 chars
    LOG_INFO("Centered: {:^10}", "text");        // Center in 10 chars

    // Zero padding
    LOG_INFO("Zero padded: {:04d}", 42);         // 0042

    // wchar_t renders as text by default and as a number under b/B/d/o/x/X
    LOG_INFO("Char: {}", L'\u00e9');              // e-acute, UTF-8 encoded
    LOG_INFO("Code point: U+{:04X}", L'\u00e9');  // U+00E9

    // Custom types (as long as they support std::formatter)
    std::vector<int> numbers = {1, 2, 3, 4, 5};
    LOG_INFO("Vector size: {}", numbers.size());

    slick::logger::Logger::instance().shutdown();
    return 0;
}
```

### Numbered placeholders

`{0}`, `{1}`, ... select an argument by position and may carry a format spec
(`{1:>8}`). An index past the end of the argument list renders as
`<MISSING_ARG>` instead of discarding the line. `std::format` rejects a format
string that mixes explicit indices with bare `{}`, but this parser accepts the
mix: the automatic counter advances only through bare `{}` placeholders, so
`"{} {0} {}"` with `(111, 222)` yields `111 111 222`.

### Dynamic width and precision

A nested field supplies a width or precision from an argument: `{:{}}`,
`{:.{}f}`, `{:{}.{}f}`, and the positional `{0:{1}}`. A nested field takes the
argument *after* the one being formatted, so `"{:{}}"` formats the first
argument to the width given by the second. The argument must be a non-negative
integer; anything else is a format error. If it was not passed at all, that
field renders `<MISSING_ARG>` and the rest of the line is unaffected.

### wchar_t arguments

A `wchar_t` follows the rule `std::format` applies to `char`. It renders as
UTF-8 text by default (`{}`, `{:c}`, `{:>4}`) and as a number under a
`b`/`B`/`d`/`o`/`x`/`X` presentation type (`{:d}` on `L'A'` gives `65`). The
spec decides, not the value, so a given spec means the same thing for every
code point. A lone `wchar_t` that is not a Unicode scalar value, such as an
unpaired UTF-16 surrogate on Windows, has no character to print and falls back
to its numeric code point. Wide *strings* are not supported; convert them
yourself before logging.

`std::format` handles strings, numbers, pointers, chrono values, and any type
with a `std::formatter` specialization, including width, precision, and
alignment, and you can add formatters for your own types. It is part of the
C++20 standard library, so it adds no dependency.

!!! danger "Caveat"

    The format string must be a string literal, or otherwise outlive processing. slick-logger decides that a format string is safe to store as a pointer by checking whether the argument's type is `const char(&)[N]`, the type string literals have. That is a heuristic. Any other `const char[N]` lvalue (e.g. a `const char` array class member, or a local `const char buf[N]`) has exactly the same type and is treated as a literal too. Then only the pointer is queued, not a copy of the contents, so if the array is changed or destroyed before the writer thread consumes the entry, the output is corrupted or reads freed memory. (Changing it *after* it has been logged is safe for the message parse cache, which checks a cached parse against the text on every line; see [Deferred Formatting](architecture.md#deferred-formatting).) Pass only string literals this way. For a `const char` array member, pass `std::string_view{member}` (or another type convertible to `std::string_view`) so its contents are copied into the queue.

## Passing std::format_args

You can pass a pre-built `std::format_args` object as the only argument to any log call. That lets you capture format arguments once and reuse them, or forward an argument pack built by another function:

```cpp
int count = 42;
double price = 9.99;
std::string_view name = "widget";

// Capture args once, pass to logger
auto args = std::make_format_args(count, price, name);
LOG_INFO("count={} price={:.2f} name={}", args);

// Also works inline
LOG_DEBUG("x={} y={}", std::make_format_args(x, y));
```

!!! note

    `std::make_format_args` requires all arguments to be lvalues. Pass temporary values through a named variable.

!!! warning "Limitation"

    Types that need their own `std::formatter` specialization (stored as `handle` inside `std::format_args`) are not supported this way and are logged as `<handle>`. Use the normal variadic log call for them.

## Log Pattern Formatting

By default a line looks like this, and every field is fixed:

```text
2026-09-19 14:02:11.481320 [INFO] [103836:app1] [main.cpp:81] work item 1 of 4
 └ timestamp                └ level  └ pid:tag    └ source      └ message
```

`set_pattern()` replaces that layout with one of your own, using the same flags as spdlog:

```cpp
using namespace slick::logger;

// Every sink, including sinks added later
Logger::instance().set_pattern("%T.%e %^%-5l%$ %-20@ %v");

// 14:02:11.481 INFO  main.cpp:81          work item 1 of 4
// 14:02:11.482 WARN  net/client.cpp:204   retrying
// 14:02:11.483 ERROR db.cpp:7             connection lost
```

Or per sink, which overrides the logger-wide default for that sink only:

```cpp
Logger::instance().add_file_sink("audit.log", "audit");
Logger::instance().get_sink("audit")->set_pattern("%Y-%m-%dT%H:%M:%SZ|%l|%v");
```

It can also be set from `LogConfig`:

```cpp
LogConfig config;
config.sinks.push_back(std::make_shared<FileSink>("app.log"));
config.pattern = "%T.%e [%L] %v";
Logger::instance().init(config);
```

### Flags

| Flag | Renders | Example |
| --- | --- | --- |
| `%v` | The formatted message | `work item 1 of 4` |
| `%l` / `%L` | Level name / single letter | `INFO` / `I` |
| `%t` | Producing thread id | `31488` |
| `%P` | Producing process id | `103836` |
| `%k` | Producer tag (slick extension) | `app1` |
| `%n` | Sink name | `audit` |
| `%s` / `%#` / `%@` | Source basename / line / `basename:line` | `main.cpp` / `81` / `main.cpp:81` |
| `%Y` `%y` `%m` `%d` | Year (4 / 2 digit), month, day | `2026` `26` `09` `19` |
| `%H` `%I` `%M` `%S` `%p` | Hour (24 / 12), minute, second, AM-PM | `14` `02` `02` `11` `PM` |
| `%e` `%f` `%F` | Milli-, micro-, nanoseconds, zero padded | `481` `481320` `481320700` |
| `%T` `%D` `%c` | `HH:MM:SS`, `MM/DD/YY`, `Www Mmm DD HH:MM:SS YYYY` | `14:02:11` `09/19/26` |
| `%a` `%A` `%b` `%B` | Weekday and month names | `Sat` `Saturday` `Sep` `September` |
| `%E` | Seconds since the epoch | `1789221731` |
| `%^` `%$` | Begin / end the colored span (console only) | |
| `%%` | A literal `%` | |
| `%+` | The built-in layout above, byte for byte | |
| `%q` | The sink's configured `TimestampFormatter` (slick extension) | |

### Width and alignment

A flag takes an optional minimum width, and a leading `-` left-aligns it. That is how columns line up:

```cpp
Logger::instance().set_pattern("%-5l %-24@ %v");
```

A field wider than its width is never truncated, and widths are capped at 64. A width on `%%` is rejected, because `%-5%` is far more likely a mistyped flag letter than a percent sign someone meant to pad.

### Coloring part of a line

Without `%^`, a color-enabled `ConsoleSink` colors the whole line, as it always has. With `%^`/`%$` it colors only the span between them:

```cpp
Logger::instance().set_pattern("%T.%e %^%-5l%$ %v");   // only the level is colored
```

The markers do nothing on non-console sinks, so the same pattern can be shared between a console sink and a file sink without leaking escape codes into the file.

A `%^` with no matching `%$` colors through to the end of the line and is closed there. spdlog leaves it open, which tints everything the terminal prints afterwards. slick-logger closes it, since "color from here on" can only sensibly mean "for the rest of this line".

### Notes

- An unknown or unsupported flag makes `set_pattern()` throw `std::invalid_argument` naming the flag, and the sink keeps the layout it already had. No flag silently renders as empty.
- `%!` (function name), `%g` (full source path), and the elapsed-time flags `%o %i %u %O` are not supported, because slick-logger does not capture that data.
- `%n` maps to the sink name: slick-logger has one logger, not a registry of named loggers.
- `%t` needs `SLICK_LOGGER_ENABLE_THREAD_ID`, which is on by default. See [Thread Id Capture](configuration.md#thread-id-capture).
- An empty pattern means "use the built-in layout", not "emit an empty line".
- `set_pattern()` is safe to call while logging is in flight, but not concurrently with itself; it is a configuration call.
- `ISink::pattern()` reports the layout in force on that sink, which is not always the string last passed to it: once `set_pattern("")` has cleared a per-sink override, it reads back the logger-wide pattern the sink has fallen back to. `Logger::pattern()` reports the logger-wide default itself, and is empty when there is none.

### Performance

A pattern is parsed once, inside `set_pattern()`, into a flat list of ops. Rendering walks that list on the writer thread and appends into a buffer the sink reuses, so a line costs no parsing and no allocation beyond the message itself. Date and time flags slice a broken-down time that is computed once per whole second and cached, and the weekday/month names come from static tables. Nothing on this path calls `strftime`, `put_time` or the locale. Runs of adjacent date/time flags are fused at compile time, so `%Y-%m-%d %H:%M:%S` becomes a single `memcpy` instead of twelve appends, and a `.%e`, `.%f` or `.%F` right after one joins it. Every literal run is folded into the op that follows it, so `[%l] %v` renders as two ops, not four.

The line is then sized once, to an upper bound computed from the pattern and the entry, and every op copies its text straight through a cursor into that space, with no capacity check, no `std::string::append` call and no out-of-line dispatch per piece. The built-in layout is written through the same code, which is also why it no longer formats numbers through `std::to_string`.

Measured on Windows/MSVC `/O2` (Ryzen 9 5900HX), minimum of 21 interleaved rounds, rendering one line through `ISink::format_log_entry()` (an entry with a source location and a message without arguments, no I/O):

| Line layout | 2.1.0 ns/line | 2.2.0 ns/line | change |
| --- | --- | --- | --- |
| Built-in layout | 253 | 207 | **-18%** |
| `%+` | 253 | 227 | -10% |
| `%q\|%v` | 178 | 181 | ~0% |
| `%Y-%m-%d %H:%M:%S.%f [%l] [%s:%#] %v` | 332 | 240 | **-28%** |
| `%T.%e %^%-5l%$ %-20@ %v` | 346 | 264 | -24% |
| `[%l] %v` | 181 | 166 | -8% |

A spelled-out pattern now renders within about 15% of the built-in layout, where 2.1.0 was about 30% slower. The rest of each row is the message itself, which costs the same whatever the layout. Timing only the layout step, with the message already formatted, `%Y-%m-%d %H:%M:%S.%f [%l] [%@] %v` takes 91 ns against 90 ns for the built-in layout (2.1.0: 197 ns against 114 ns). On an entry with arguments, `std::format` dominates the line, and the layout accounts for only a few percent of it. On the producing thread, stamping the thread id (added in 2.0.0) took the `LOG_*` call itself from 96.6 to 97.8 ns.

A pattern also pays only for the fields it renders:

- The message. A pattern that renders neither the message nor a level skips the `std::format` pass altogether, and that pass is most of the cost of a line with arguments. On an entry with two arguments, `%T|%@` renders in 94 ns against 705 ns for `%T|%@|%l`. Adding a level flag brings the message pass back, because a message that fails to format is reported at `ERROR`, and that is only known by trying.
- The broken-down time. Only a calendar field (`%Y`, `%H`, `%T`, `%c`, `%a` …) needs one. `%e`, `%f`, `%F` and `%E` are arithmetic on the entry's own timestamp, so a pattern built from those alone never consults the per-second cache.

Width costs something either way, but right alignment costs more: `%-8l` appends its padding after a field that is already at the end of the line, while `%8l` has to put the padding in front of it. That measured at roughly 10ns per right-aligned field, so prefer left alignment where the column order allows it.

`%q` delegates to the sink's `TimestampFormatter`, which appends its predefined formats straight into the same buffer; the `%q|%v` row above shows what that costs. The exception is `Format::CUSTOM`, which carries the `strftime` cost described in [Timestamp formatting](timestamps.md#timestamp-formatting). On a hot path, spell the date out with pattern flags instead of using a custom format string.
