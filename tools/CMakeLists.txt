# Tools CMakeLists.txt
#
# Only reached when BUILD_SLICK_LOGGER_COLLECTOR or BUILD_SLICK_LOGGER_DECODER
# is ON, so this file owns the targets and their install rules. Consumers that
# just want the header-only library get neither.

if(BUILD_SLICK_LOGGER_COLLECTOR)
    add_executable(slick_log_collector slick_log_collector.cpp)
    target_link_libraries(slick_log_collector PRIVATE slick::logger)
    install(TARGETS slick_log_collector RUNTIME DESTINATION bin)
    message(STATUS "Building slick-logger collector tool")
endif()

if(BUILD_SLICK_LOGGER_DECODER)
    add_executable(slick_log_decode slick_log_decode.cpp)
    target_link_libraries(slick_log_decode PRIVATE slick::logger)
    install(TARGETS slick_log_decode RUNTIME DESTINATION bin)
    message(STATUS "Building slick-logger decode tool")
endif()
