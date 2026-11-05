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
    heartbeat.cpp
    relay.cpp
    retry.cpp
    shutdown.cpp
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

    add_executable(relay_tests
        relay_tests.cpp
    )
    target_link_libraries(relay_tests PRIVATE
        nexus_runtime
        nexus_warnings
        nexus_sanitizers
    )
    add_test(NAME relay_tests COMMAND relay_tests)

    add_executable(retry_tests
        retry_tests.cpp
    )
    target_link_libraries(retry_tests PRIVATE
        nexus_runtime
        nexus_warnings
        nexus_sanitizers
    )
    add_test(NAME retry_tests COMMAND retry_tests)

    add_executable(heartbeat_tests
        heartbeat_tests.cpp
    )
    target_link_libraries(heartbeat_tests PRIVATE
        nexus_runtime
        nexus_warnings
        nexus_sanitizers
    )
    add_test(NAME heartbeat_tests COMMAND heartbeat_tests)

    add_executable(shutdown_tests
        shutdown_tests.cpp
    )
    target_link_libraries(shutdown_tests PRIVATE
        nexus_runtime
        nexus_warnings
        nexus_sanitizers
    )
    add_test(NAME shutdown_tests COMMAND shutdown_tests)

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

    add_test(
        NAME integration_disconnect_reconnect
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/reconnect_test.py
                $<TARGET_FILE:nexus-edge>
                $<TARGET_FILE:nexus-client>
    )
    set_tests_properties(integration_disconnect_reconnect PROPERTIES TIMEOUT 30)

    add_test(
        NAME integration_automatic_reconnect
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/automatic_reconnect_test.py
                $<TARGET_FILE:nexus-edge>
                $<TARGET_FILE:nexus-client>
    )
    set_tests_properties(integration_automatic_reconnect PROPERTIES TIMEOUT 30)

    add_test(
        NAME integration_heartbeat
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/heartbeat_test.py
                $<TARGET_FILE:nexus-edge>
                $<TARGET_FILE:nexus-client>
    )
    set_tests_properties(integration_heartbeat PROPERTIES TIMEOUT 20)

    add_test(
        NAME integration_timeouts
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/timeout_test.py
                $<TARGET_FILE:nexus-edge>
                $<TARGET_FILE:nexus-client>
    )
    set_tests_properties(integration_timeouts PROPERTIES TIMEOUT 35)

    add_test(
        NAME integration_graceful_shutdown
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/shutdown_test.py
                $<TARGET_FILE:nexus-edge>
                $<TARGET_FILE:nexus-client>
    )
    set_tests_properties(integration_graceful_shutdown PROPERTIES TIMEOUT 20)

    add_test(
        NAME integration_protocol_and_limits
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/protection_test.py
                $<TARGET_FILE:nexus-edge>
                $<TARGET_FILE:nexus-client>
    )
    set_tests_properties(integration_protocol_and_limits PROPERTIES TIMEOUT 30)
endif()
