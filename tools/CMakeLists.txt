# Tools CMakeLists.txt
#
# Only reached when BUILD_SLICK_LOGGER_COLLECTOR is ON, so this file owns both
# the target and its install rule. Consumers that just want the header-only
# library get neither.

add_executable(slick_log_collector slick_log_collector.cpp)
target_link_libraries(slick_log_collector PRIVATE slick::logger)

install(TARGETS slick_log_collector RUNTIME DESTINATION bin)

message(STATUS "Building slick-logger collector tool")
