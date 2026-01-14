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
    
else()
    message(STATUS "Testing disabled - skipping slick-logger tests")
endif()
