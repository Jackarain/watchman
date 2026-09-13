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

project(watchman
    VERSION 0.1.0
    DESCRIPTION "A file watching library based on boost.asio"
    LANGUAGES CXX)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(WATCHMAN_IS_TOP_LEVEL ON)
else()
    set(WATCHMAN_IS_TOP_LEVEL OFF)
endif()

option(WATCHMAN_BUILD_EXAMPLES "Build the watchman examples" ${WATCHMAN_IS_TOP_LEVEL})
option(WATCHMAN_BUILD_TESTS "Build the watchman tests" ${WATCHMAN_IS_TOP_LEVEL})
option(WATCHMAN_INSTALL "Generate the watchman install targets" ${WATCHMAN_IS_TOP_LEVEL})

include(GNUInstallDirs)

# Boost 自带 BoostConfig.cmake，使用 CONFIG 模式查找以避免依赖已被移除的
# FindBoost 模块（CMake 3.30 起）。
if(POLICY CMP0167)
    cmake_policy(SET CMP0167 NEW)
endif()

# 等待动作依赖 asio 的关联取消槽，Linux 后端还依赖 posix 描述符的按操作
# 取消，因此要求 Boost 1.78 及以上。
find_package(Boost 1.78 REQUIRED CONFIG COMPONENTS filesystem system)
find_package(Threads REQUIRED)

add_library(watchman INTERFACE)
add_library(watchman::watchman ALIAS watchman)

target_compile_features(watchman INTERFACE cxx_std_20)
target_include_directories(watchman INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/watchman/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

target_link_libraries(watchman
    INTERFACE
        Boost::filesystem
        Boost::system
        Threads::Threads)

if(APPLE)
    # macOS 实现基于 FSEvents。
    target_link_libraries(watchman INTERFACE "-framework CoreServices")
endif()

if(WATCHMAN_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(WATCHMAN_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(WATCHMAN_INSTALL)
    include(CMakePackageConfigHelpers)

    set(WATCHMAN_CMAKE_INSTALL_DIR ${CMAKE_INSTALL_LIBDIR}/cmake/watchman)

    install(TARGETS watchman EXPORT watchmanTargets)

    install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/watchman/include/
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

    install(EXPORT watchmanTargets
        NAMESPACE watchman::
        DESTINATION ${WATCHMAN_CMAKE_INSTALL_DIR})

    configure_package_config_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/watchmanConfig.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/watchmanConfig.cmake
        INSTALL_DESTINATION ${WATCHMAN_CMAKE_INSTALL_DIR})

    write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/watchmanConfigVersion.cmake
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion)

    install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/watchmanConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/watchmanConfigVersion.cmake
        DESTINATION ${WATCHMAN_CMAKE_INSTALL_DIR})
endif()
