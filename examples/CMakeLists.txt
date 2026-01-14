# Examples CMakeLists.txt
option(BUILD_SLICK_LOGGER_EXAMPLES "Build example executables" ON)

if(BUILD_SLICK_LOGGER_EXAMPLES)
    add_executable(logger_example example.cpp)
    target_link_libraries(logger_example slick::logger)

    add_executable(multi_sink_example multi_sink_example.cpp)
    target_link_libraries(multi_sink_example slick::logger)

    add_executable(timestamp_example timestamp_example.cpp)
    target_link_libraries(timestamp_example slick::logger)
    
    message(STATUS "Building slick-logger examples")
else()
    message(STATUS "Skipping slick-logger examples")
endif()
