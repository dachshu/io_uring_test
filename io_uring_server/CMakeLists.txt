cmake_minimum_required(VERSION 3.17)

project("io_uring_server")
set(CMAKE_VERBOSE_MAKEFILE true)

set(OUTPUT_NAME "${CMAKE_PROJECT_NAME}")
set(SRC_FILES
    server.cpp
    )

if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

INCLUDE_DIRECTORIES(./liburing/src/include/)
LINK_DIRECTORIES(./liburing/src/)

add_compile_options(-Wall -O2 -D_GNU_SOURCE -g -ggdb -std=c++11)
link_libraries(pthread uring)
set(CMAKE_CXX_COMPILER "g++")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY bin)

set(CMAKE_CXX_FLAGS_DEBUG "-DDEBUG")
set(CMAKE_CXX_FLAGS_RELEASE "-DNDEBUG -O3")
add_executable(${OUTPUT_NAME} ${SRC_FILES})
