cmake_minimum_required(VERSION 3.20)

project(NexusProxy VERSION 0.1.0 LANGUAGES CXX)

include(CTest)

find_package(OpenSSL 3.0 REQUIRED COMPONENTS SSL Crypto)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Keep executables in build/ even though their CMake definitions live in
# subdirectories. This preserves the familiar ./build/nexus-edge commands.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

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

add_subdirectory(src)
add_subdirectory(apps)

if(BUILD_TESTING)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_subdirectory(tests)
endif()
