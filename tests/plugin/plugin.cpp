#include <slick/logger.hpp>

// Exported symbols use C linkage to avoid name-mangling differences across
// compilers and platforms (required for GetProcAddress / dlsym to work).

extern "C" {

#if defined(_WIN32)
#  define PLUGIN_EXPORT __declspec(dllexport)
#else
#  define PLUGIN_EXPORT __attribute__((visibility("default")))
#endif

/**
 * Called by the host after loading this shared library.
 * Redirects this library's Logger::instance() to the host's logger so that
 * all LOG_* calls from this library appear in the host's log file.
 *
 * IMPORTANT — unload ordering:
 * LOG_* with string literals stores a raw pointer to the format string, which
 * lives in this library's code segment. The host must call
 * Logger::instance().flush() BEFORE calling FreeLibrary/dlclose to ensure the
 * writer thread has consumed all queued entries. Unlike shutdown(), flush()
 * leaves the logger running so the host can continue logging after the call.
 */
PLUGIN_EXPORT void plugin_init(slick::logger::Logger* host_logger) {
    slick::logger::Logger::set_instance(host_logger);
    LOG_INFO("plugin_init: logger connected to host");
}

/**
 * Exercise several log levels so the test can verify each one is routed
 * to the host logger.
 */
PLUGIN_EXPORT void plugin_log_messages() {
    LOG_DEBUG("plugin debug message");
    LOG_INFO("plugin info message");
    LOG_WARN("plugin warn message");
    LOG_ERROR("plugin error message");
}

/**
 * Called by the host before unloading this shared library.
 * Restores this library's own local logger so that any logging after
 * unload does not dereference the (potentially destroyed) host logger.
 */
PLUGIN_EXPORT void plugin_shutdown() {
    slick::logger::Logger::clear_instance_override();
}

} // extern "C"
