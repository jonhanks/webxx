add_library(asio_fiber INTERFACE)
target_include_directories(asio_fiber INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(asio_fiber INTERFACE
        Boost::headers
        Boost::fiber
        Boost::system
        Threads::Threads
        )

add_executable(autoecho example/autoecho.cc)
target_link_libraries(autoecho PUBLIC asio_fiber)