#include <slick/logger.hpp>
#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <string>

#if defined(_WIN32)
#  include <windows.h>
   using LibHandle = HMODULE;
   inline LibHandle open_lib(const char* path)  { return LoadLibraryA(path); }
   inline void*     get_sym(LibHandle h, const char* sym) { return reinterpret_cast<void*>(GetProcAddress(h, sym)); }
   inline void      close_lib(LibHandle h)      { FreeLibrary(h); }
   inline std::string lib_error()               { return std::to_string(GetLastError()); }
#else
#  include <dlfcn.h>
   using LibHandle = void*;
   inline LibHandle open_lib(const char* path)  { return dlopen(path, RTLD_NOW | RTLD_LOCAL); }
   inline void*     get_sym(LibHandle h, const char* sym) { return dlsym(h, sym); }
   inline void      close_lib(LibHandle h)      { dlclose(h); }
   inline std::string lib_error()               { return dlerror() ? dlerror() : "unknown"; }
#endif

// Resolved at cmake configure time; the build system defines this macro.
#ifndef PLUGIN_LIB_PATH
#  error "PLUGIN_LIB_PATH must be defined by CMake"
#endif

using PluginInitFn     = void(*)(slick::logger::Logger*);
using PluginLogFn      = void(*)();
using PluginShutdownFn = void(*)();

static std::string read_log(const std::filesystem::path& path) {
    std::ifstream f(path);
    std::string line, contents;
    std::getline(f, line); // skip version line written by Logger on init
    while (std::getline(f, line))
        contents += line + "\n";
    return contents;
}

class SharedLibraryTest : public ::testing::Test {
protected:
    void TearDown() override {
        slick::logger::Logger::instance().shutdown();
        std::filesystem::remove("test_shared_lib_host.log");
    }
};

// The plugin is a real shared library (.dll/.so) compiled separately.
// Verify that after plugin_init() all LOG_* calls inside the plugin appear
// in the host's log file.
TEST_F(SharedLibraryTest, PluginLogsRouteToHostLogger) {
    std::filesystem::remove("test_shared_lib_host.log");

    // --- Host setup ---
    slick::logger::Logger::instance().add_file_sink("test_shared_lib_host.log");
    slick::logger::Logger::instance().init(1024);

    // --- Load plugin ---
    LibHandle handle = open_lib(PLUGIN_LIB_PATH);
    ASSERT_NE(handle, LibHandle{}) << "Failed to load plugin: " << lib_error();

    auto plugin_init     = reinterpret_cast<PluginInitFn>    (get_sym(handle, "plugin_init"));
    auto plugin_log      = reinterpret_cast<PluginLogFn>     (get_sym(handle, "plugin_log_messages"));
    auto plugin_shutdown = reinterpret_cast<PluginShutdownFn>(get_sym(handle, "plugin_shutdown"));

    ASSERT_NE(plugin_init,     nullptr) << "plugin_init not found";
    ASSERT_NE(plugin_log,      nullptr) << "plugin_log_messages not found";
    ASSERT_NE(plugin_shutdown, nullptr) << "plugin_shutdown not found";

    // --- Simulate plugin lifecycle ---
    plugin_init(&slick::logger::Logger::instance());
    plugin_log();
    plugin_shutdown();

    // Drain the host logger's queue BEFORE unloading the plugin.
    // LOG_* with string literals stores a raw pointer to the format string,
    // which lives in the plugin's code segment. flush() blocks until the writer
    // thread has consumed all queued entries, so close_lib() can safely unmap
    // the plugin's memory without leaving dangling format-string pointers.
    // The host logger keeps running — logging can continue after this call.
    slick::logger::Logger::instance().flush();

    close_lib(handle);

    ASSERT_TRUE(std::filesystem::exists("test_shared_lib_host.log"));
    std::string contents = read_log("test_shared_lib_host.log");

    // All messages logged from inside the plugin must appear in the host log.
    EXPECT_NE(contents.find("plugin_init: logger connected to host"), std::string::npos);
    EXPECT_NE(contents.find("plugin debug message"),  std::string::npos);
    EXPECT_NE(contents.find("plugin info message"),   std::string::npos);
    EXPECT_NE(contents.find("plugin warn message"),   std::string::npos);
    EXPECT_NE(contents.find("plugin error message"),  std::string::npos);
}

// After plugin_shutdown() clears the override, further LOG_* calls from the
// host still work normally (the host logger is unaffected by the plugin).
TEST_F(SharedLibraryTest, HostLoggerUnaffectedAfterPluginUnload) {
    std::filesystem::remove("test_shared_lib_host.log");

    slick::logger::Logger::instance().add_file_sink("test_shared_lib_host.log");
    slick::logger::Logger::instance().init(1024);

    LibHandle handle = open_lib(PLUGIN_LIB_PATH);
    ASSERT_NE(handle, LibHandle{}) << "Failed to load plugin: " << lib_error();

    auto plugin_init     = reinterpret_cast<PluginInitFn>    (get_sym(handle, "plugin_init"));
    auto plugin_shutdown = reinterpret_cast<PluginShutdownFn>(get_sym(handle, "plugin_shutdown"));

    ASSERT_NE(plugin_init,     nullptr);
    ASSERT_NE(plugin_shutdown, nullptr);

    plugin_init(&slick::logger::Logger::instance());
    plugin_shutdown();

    // Drain before unloading, then close the library.
    // The host logger remains running so we can log after unload.
    slick::logger::Logger::instance().flush();
    close_lib(handle);

    // Host logs after unload must still work.
    LOG_INFO("host message after plugin unload");
    slick::logger::Logger::instance().shutdown();

    std::string contents = read_log("test_shared_lib_host.log");
    EXPECT_NE(contents.find("host message after plugin unload"), std::string::npos);
}

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
