cmake_minimum_required(VERSION 3.15)
project(webxx)

set(CMAKE_CXX_STANDARD 14)

find_package(Boost REQUIRED system fiber)
find_package(Threads)

add_subdirectory(external)

add_executable(webxx main.cpp)
target_link_libraries(webxx PUBLIC asio_fiber)