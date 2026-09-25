# Tests CMakeLists.txt
option(BUILD_SLICK_LOGGER_TESTING "Build tests" ON)

if(BUILD_SLICK_LOGGER_TESTING)
    find_package(GTest QUIET)

    if(NOT GTest_FOUND)
        include(FetchContent)
        FetchContent_Declare(
            googletest
            GIT_REPOSITORY https://github.com/google/googletest.git
            GIT_TAG v1.17.0
        )
        # For Windows: Prevent overriding the parent project's compiler/linker settings
        set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
        FetchContent_MakeAvailable(googletest)
    endif()

    add_executable(slick_logger_tests test_logger.cpp)
    target_include_directories(slick_logger_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(slick_logger_tests slick::logger GTest::gtest_main)
    add_test(NAME slick_logger_tests COMMAND slick_logger_tests)

    add_executable(slick_logger_sink_tests test_sinks.cpp)
    target_include_directories(slick_logger_sink_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(slick_logger_sink_tests slick::logger GTest::gtest_main)
    add_test(NAME slick_logger_sink_tests COMMAND slick_logger_sink_tests)

    add_executable(slick_logger_timestamp_tests test_timestamp.cpp)
    target_include_directories(slick_logger_timestamp_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(slick_logger_timestamp_tests slick::logger GTest::gtest_main)
    add_test(NAME slick_logger_timestamp_tests COMMAND slick_logger_timestamp_tests)

    add_executable(slick_logger_pattern_tests test_pattern.cpp)
    target_include_directories(slick_logger_pattern_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(slick_logger_pattern_tests slick::logger GTest::gtest_main)
    add_test(NAME slick_logger_pattern_tests COMMAND slick_logger_pattern_tests)

    add_executable(slick_logger_statistics_tests test_statistics.cpp)
    target_include_directories(slick_logger_statistics_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(slick_logger_statistics_tests slick::logger GTest::gtest_main)
    add_test(NAME slick_logger_statistics_tests COMMAND slick_logger_statistics_tests)

    # Compiles the statistics publish against the no-P0718 branch of
    # detail::atomic_shared_ptr on every platform, not just the ones (libc++)
    # that select it. Its own executable on purpose: the macro changes the type
    # of a Logger member, so mixing it with a normally-built TU would be an ODR
    # violation rather than a test.
    add_executable(slick_logger_stats_fallback_tests test_stats_atomic_fallback.cpp)
    target_include_directories(slick_logger_stats_fallback_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(slick_logger_stats_fallback_tests slick::logger GTest::gtest_main)
    add_test(NAME slick_logger_stats_fallback_tests COMMAND slick_logger_stats_fallback_tests)

    # Builds the logger with SLICK_LOGGER_ENABLE_LOSS_DETECTION on and
    # SLICK_LOGGER_ENABLE_CPU_RELAX off. Its own executable for the same reason:
    # the switches change the type of the queues Logger holds.
    add_executable(slick_logger_queue_traits_tests test_queue_traits.cpp)
    target_include_directories(slick_logger_queue_traits_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(slick_logger_queue_traits_tests slick::logger GTest::gtest_main)
    add_test(NAME slick_logger_queue_traits_tests COMMAND slick_logger_queue_traits_tests)

    # --- Shared-library integration test ---
    # Build a minimal plugin as a real shared library (.dll/.so), then load it
    # at runtime to verify LOG_* calls inside the plugin are routed to the host
    # logger via Logger::set_instance().
    add_library(slick_logger_test_plugin SHARED plugin/plugin.cpp)
    target_link_libraries(slick_logger_test_plugin PRIVATE slick::logger)
    # Hide all symbols by default on non-Windows so only PLUGIN_EXPORT ones are visible.
    set_target_properties(slick_logger_test_plugin PROPERTIES
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON
    )

    # Resolve the plugin path at configure time so the test executable knows
    # where to find the .dll/.so without relying on PATH/LD_LIBRARY_PATH.
    add_executable(slick_logger_shared_lib_tests test_shared_library.cpp)
    target_link_libraries(slick_logger_shared_lib_tests PRIVATE slick::logger GTest::gtest_main)
    add_dependencies(slick_logger_shared_lib_tests slick_logger_test_plugin)
    target_compile_definitions(slick_logger_shared_lib_tests PRIVATE
        PLUGIN_LIB_PATH="$<TARGET_FILE:slick_logger_test_plugin>"
    )
    add_test(NAME slick_logger_shared_lib_tests COMMAND slick_logger_shared_lib_tests)

    # --- Multi-process (shared memory) integration test ---
    # Build a producer as a separate executable, then launch it from the test
    # process (which acts as the collector) to verify that log entries cross a
    # real process boundary. Same path-injection trick as the plugin test above.
    add_executable(slick_logger_test_producer shm_producer/producer_main.cpp)
    target_link_libraries(slick_logger_test_producer PRIVATE slick::logger)

    add_executable(slick_logger_shm_tests test_shared_memory.cpp)
    target_link_libraries(slick_logger_shm_tests PRIVATE slick::logger GTest::gtest_main)
    add_dependencies(slick_logger_shm_tests slick_logger_test_producer)
    target_compile_definitions(slick_logger_shm_tests PRIVATE
        SHM_PRODUCER_EXE_PATH="$<TARGET_FILE:slick_logger_test_producer>"
    )
    add_test(NAME slick_logger_shm_tests COMMAND slick_logger_shm_tests)

else()
    message(STATUS "Testing disabled - skipping slick-logger tests")
endif()
