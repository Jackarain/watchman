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

watchman_add_test(notify_event)
watchman_add_test(path_exclusion)
watchman_add_test(platform)

# 端到端的目录监视测试目前只在 Linux 上验证过。
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    watchman_add_test(watch_service)
endif()
