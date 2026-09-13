#
# tests/CMakeLists.txt
# ~~~~~~~~~~~~~~~~~~~~
#
# Copyright (c) 2023 Jack (jack.arain at gmail dot com)
#
# Distributed under the Boost Software License, Version 1.0. (See accompanying
# file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
#

function(watchman_add_test name)
    add_executable(watchman_test_${name} ${name}_test.cpp)
    target_link_libraries(watchman_test_${name} PRIVATE watchman::watchman)
    set_target_properties(watchman_test_${name} PROPERTIES
        CXX_EXTENSIONS OFF
        CXX_STANDARD_REQUIRED ON)

    add_test(NAME ${name} COMMAND watchman_test_${name})
    set_tests_properties(${name} PROPERTIES TIMEOUT 60)
endfunction()

# 无法在本机运行的平台后端只做编译期检查：在自带对应内核头文件的平台上直接
# 使用系统头文件，其它情况使用 tests/platform/stub/<name> 下的桩接口。桩接口
# 按平台分开存放，避免被 asio 等公共头文件包含到而互相干扰。
function(watchman_add_platform_compile_check name system_pattern)
    add_library(watchman_compile_check_${name} OBJECT
        platform/${name}_compile_test.cpp)

    target_link_libraries(watchman_compile_check_${name}
        PRIVATE watchman::watchman)

    target_include_directories(watchman_compile_check_${name}
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

    set_target_properties(watchman_compile_check_${name} PROPERTIES
        CXX_EXTENSIONS OFF
        CXX_STANDARD_REQUIRED ON)

    if(NOT CMAKE_SYSTEM_NAME MATCHES "${system_pattern}")
        target_include_directories(watchman_compile_check_${name}
            PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/platform/stub/${name})
    endif()
endfunction()

watchman_add_test(notify_event)
watchman_add_test(path_exclusion)
watchman_add_test(path_remap)
watchman_add_test(platform)
watchman_add_test(wait_queue)

# 桩接口只补系统头文件，不补 Windows 上缺失的 POSIX 接口，所以这些检查只在
# 类 Unix 宿主上运行。
if(NOT WIN32)
    # kqueue 后端的头文件在 BSD 与 macOS 上都有，不需要桩接口。
    watchman_add_platform_compile_check(bsd "BSD|DragonFly|Darwin")
    watchman_add_platform_compile_check(macos "Darwin")

    # Apple 的 struct stat 没有 st_atim 字段，无法在这里检查 event ports 后端。
    if(NOT APPLE)
        watchman_add_platform_compile_check(solaris "SunOS")
    endif()
endif()

# 端到端的目录监视测试在带有监视后端的平台上运行。
if(CMAKE_SYSTEM_NAME MATCHES "Linux|Darwin|Windows|FreeBSD|OpenBSD|NetBSD|DragonFly|SunOS")
    watchman_add_test(watch_service)
endif()
