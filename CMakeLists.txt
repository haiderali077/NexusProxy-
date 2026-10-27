cmake_minimum_required(VERSION 3.20)

project(NexusProxy VERSION 0.1.0 LANGUAGES CXX)

include(CTest)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(nexus_warnings INTERFACE)
if(MSVC)
    target_compile_options(nexus_warnings INTERFACE /W4 /permissive-)
else()
    target_compile_options(nexus_warnings INTERFACE
        -Wall
        -Wextra
        -Wpedantic
    )
endif()

add_library(nexus_protocol STATIC
    protocol.cpp
)

target_include_directories(nexus_protocol PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(nexus_protocol PRIVATE nexus_warnings)

add_library(nexus_runtime STATIC
    config.cpp
    relay.cpp
    socket_utils.cpp
    unique_socket.cpp
)
target_include_directories(nexus_runtime PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(nexus_runtime PUBLIC nexus_protocol)
target_link_libraries(nexus_runtime PRIVATE nexus_warnings)

add_executable(nexus-edge
    server.cpp
)
target_link_libraries(nexus-edge PRIVATE nexus_runtime nexus_warnings)

add_executable(nexus-client
    client.cpp
)
target_link_libraries(nexus-client PRIVATE nexus_runtime nexus_warnings)

if(BUILD_TESTING)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)

    add_executable(protocol_tests
        protocol_tests.cpp
    )
    target_link_libraries(protocol_tests PRIVATE
        nexus_protocol
        nexus_warnings
    )
    add_test(NAME protocol_tests COMMAND protocol_tests)

    add_executable(socket_tests
        socket_tests.cpp
    )
    target_link_libraries(socket_tests PRIVATE
        nexus_runtime
        nexus_warnings
    )
    add_test(NAME socket_tests COMMAND socket_tests)

    add_test(
        NAME integration_single_request
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration_test.py
                $<TARGET_FILE:nexus-edge>
                $<TARGET_FILE:nexus-client>
    )
    set_tests_properties(integration_single_request PROPERTIES TIMEOUT 20)
endif()
