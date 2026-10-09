cmake_minimum_required(VERSION 3.20)
project(your_project LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(slick-logger CONFIG REQUIRED)

add_executable(your_app main.cpp)
target_link_libraries(your_app PRIVATE slick::logger)

enable_testing()
add_test(NAME package_consumer COMMAND your_app)
