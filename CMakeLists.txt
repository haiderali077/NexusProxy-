cmake_minimum_required(VERSION 3.20)

project(NexusProxy VERSION 0.1.0 LANGUAGES CXX)

include(CTest)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(NEXUS_ENABLE_SANITIZERS
       "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

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

add_library(nexus_sanitizers INTERFACE)
if(NEXUS_ENABLE_SANITIZERS)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(nexus_sanitizers INTERFACE
            -fsanitize=address,undefined
            -fno-omit-frame-pointer
        )
        target_link_options(nexus_sanitizers INTERFACE
            -fsanitize=address,undefined
        )
    else()
        message(FATAL_ERROR
            "NEXUS_ENABLE_SANITIZERS requires Clang, AppleClang, or GCC")
    endif()
endif()

add_library(nexus_protocol STATIC
    protocol.cpp
)

target_include_directories(nexus_protocol PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(nexus_protocol PRIVATE
    nexus_warnings
    nexus_sanitizers
)

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
target_link_libraries(nexus_runtime PRIVATE
    nexus_warnings
    nexus_sanitizers
)

add_executable(nexus-edge
    server.cpp
)
target_link_libraries(nexus-edge PRIVATE
    nexus_runtime
    nexus_warnings
    nexus_sanitizers
)

add_executable(nexus-client
    client.cpp
)
target_link_libraries(nexus-client PRIVATE
    nexus_runtime
    nexus_warnings
    nexus_sanitizers
)

if(BUILD_TESTING)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)

    add_executable(protocol_tests
        protocol_tests.cpp
    )
    target_link_libraries(protocol_tests PRIVATE
        nexus_protocol
        nexus_warnings
        nexus_sanitizers
    )
    add_test(NAME protocol_tests COMMAND protocol_tests)

    add_executable(socket_tests
        socket_tests.cpp
    )
    target_link_libraries(socket_tests PRIVATE
        nexus_runtime
        nexus_warnings
        nexus_sanitizers
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

    add_test(
        NAME integration_concurrent_streams
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/concurrent_test.py
                $<TARGET_FILE:nexus-edge>
                $<TARGET_FILE:nexus-client>
    )
    set_tests_properties(integration_concurrent_streams PROPERTIES TIMEOUT 30)
endif()
