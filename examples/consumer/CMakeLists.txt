#
# CMakeLists.txt
# ~~~~~~~~~~~~~~
#
# Copyright (c) 2023 Jack (jack.arain at gmail dot com)
#
# Distributed under the Boost Software License, Version 1.0. (See accompanying
# file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
#

cmake_minimum_required(VERSION 3.16)

project(watchman_consumer
    DESCRIPTION "Consumer checks for the watchman package"
    LANGUAGES CXX)

set(WATCHMAN_CONSUMER_USE_SUBDIRECTORY OFF CACHE BOOL
    "Use the watchman source tree as a subdirectory instead of an installed package")
set(WATCHMAN_CONSUMER_SOURCE_DIR "" CACHE PATH
    "Path to the watchman source tree, required in subdirectory mode")

if(WATCHMAN_CONSUMER_USE_SUBDIRECTORY)
    if(NOT EXISTS ${WATCHMAN_CONSUMER_SOURCE_DIR}/CMakeLists.txt)
        message(FATAL_ERROR
            "WATCHMAN_CONSUMER_SOURCE_DIR must point to the watchman source tree")
    endif()

    add_subdirectory(${WATCHMAN_CONSUMER_SOURCE_DIR}
        ${CMAKE_CURRENT_BINARY_DIR}/watchman)
else()
    find_package(watchman CONFIG REQUIRED)
endif()

add_executable(watchman_consumer main.cpp)
target_link_libraries(watchman_consumer PRIVATE watchman::watchman)
set_target_properties(watchman_consumer PROPERTIES
    CXX_EXTENSIONS OFF
    CXX_STANDARD_REQUIRED ON)
