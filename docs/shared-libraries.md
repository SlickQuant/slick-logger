# Shared libraries

## Sharing the logger across shared libraries (plugin / strategy pattern)

Because slick-logger is header-only, each binary (EXE or `.dll`/`.so`) that includes `logger.hpp` gets its own `Logger` instance. By default, `LOG_*` calls inside a dynamically loaded plugin therefore do not reach the host application's log file.

Use `Logger::set_instance()` to redirect a plugin's `LOG_*` calls to the host's logger.

The host application passes its logger to the plugin after loading it:

```cpp
// host/main.cpp
#include <slick/logger.hpp>

// Platform-specific shared library loading omitted for brevity (LoadLibrary on Windows, dlopen on Linux)
using StrategyInitFn = void(*)(slick::logger::Logger&);

void load_strategy(void* handle) {
    auto* init_fn = reinterpret_cast<StrategyInitFn>(get_symbol(handle, "strategy_init"));
    if (init_fn) {
        // Redirect the plugin's LOG_* macros to this process's logger
        init_fn(slick::logger::Logger::instance());
    }
}
```

The plugin or strategy library exposes a C-linkage init function:

```cpp
// strategy/strategy.cpp
#include <slick/logger.hpp>

extern "C" void strategy_init(slick::logger::Logger& framework_logger) {
    // All LOG_* calls in this shared library now route to the host's logger
    slick::logger::Logger::set_instance(&framework_logger);
    LOG_INFO("Strategy loaded — logger connected to framework");
}

extern "C" void strategy_shutdown() {
    // Optional: restore this library's own local logger on unload
    slick::logger::Logger::clear_instance_override();
}
```

!!! question

    **Why `extern "C"`?** C linkage avoids name-mangling differences between compilers, so `get_symbol`/`GetProcAddress`/`dlsym` can reliably find the function.

!!! warning "Thread safety"

    `set_instance()` uses `std::atomic` with acquire-release ordering. Call it once during plugin initialization, before any logging threads in the plugin start.

!!! danger "Unload ordering"

    `LOG_*` with string literals stores a raw pointer to the format string, which lives in the plugin's code segment. The host must call `Logger::instance().flush()` before calling `FreeLibrary`/`dlclose`. `flush()` blocks until the writer thread has consumed all queued entries, then returns with the logger still running, so host logging continues normally. `shutdown()` would stop the logger entirely.

!!! warning "Multiple plugins"

    Each shared library (`.dll`/`.so`) holds its own copy of `override_instance_`. Every plugin can call `set_instance()` with the same host logger on its own, since the host logger's lock-free queue is designed for concurrent producers.

!!! danger "Same version on both sides"

    A plugin's `LOG_*` calls are compiled into the plugin but operate on the host's `Logger` object, so the plugin and the host must be built from the same slick-logger version, with the same `SLICK_LOGGER_MAX_ARGS`, `SLICK_LOGGER_TAG_SIZE`, `SLICK_LOGGER_ENABLE_THREAD_ID`, `SLICK_LOGGER_ENABLE_LOSS_DETECTION` and `SLICK_LOGGER_ENABLE_CPU_RELAX`. A mismatch in the `Logger` layout is not detected; it is undefined behavior. The host's settings, such as its level, pattern and [timestamper](timestamps.md#custom-timestamper), apply to the plugin's entries too.
