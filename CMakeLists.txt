cmake_minimum_required(VERSION 3.20)

project(NexusProxy VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(nexus_protocol STATIC
    protocol.cpp
)

target_include_directories(nexus_protocol PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

add_library(nexus_runtime STATIC
    relay.cpp
    socket_utils.cpp
    unique_socket.cpp
)
target_include_directories(nexus_runtime PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(nexus_runtime PUBLIC nexus_protocol)

add_executable(nexus-edge
    server.cpp
)
target_link_libraries(nexus-edge PRIVATE nexus_runtime)

add_executable(nexus-client
    client.cpp
)
target_link_libraries(nexus-client PRIVATE nexus_runtime)
