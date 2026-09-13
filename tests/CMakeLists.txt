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

# 无法在本机运行的平台后端只做编译期检查：在对应的真实平台上直接使用
# 系统头文件，其它情况使用 tests/platform/stub 下的桩接口。
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
            PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/platform/stub)
    endif()
endfunction()

watchman_add_test(notify_event)
watchman_add_test(path_exclusion)
watchman_add_test(platform)
watchman_add_test(wait_queue)

watchman_add_platform_compile_check(bsd "BSD|DragonFly")
watchman_add_platform_compile_check(solaris "SunOS")

# 端到端的目录监视测试目前只在 Linux 上验证过。
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    watchman_add_test(watch_service)
endif()
