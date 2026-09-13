#
# examples/CMakeLists.txt
# ~~~~~~~~~~~~~~~~~~~~~~~
#
# Copyright (c) 2023 Jack (jack.arain at gmail dot com)
#
# Distributed under the Boost Software License, Version 1.0. (See accompanying
# file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
#

function(watchman_add_example name)
    add_executable(watchman_example_${name} ${name}.cpp)
    target_link_libraries(watchman_example_${name} PRIVATE watchman::watchman)
    set_target_properties(watchman_example_${name} PROPERTIES
        CXX_EXTENSIONS OFF
        CXX_STANDARD_REQUIRED ON)
endfunction()

# consumer 子目录是一个独立工程，用来验证安装后 find_package 与直接
# add_subdirectory 两种引入方式，不参与这里的示例构建。
watchman_add_example(simple_watch)
watchman_add_example(excluded_dirs)
watchman_add_example(cancel_watch)
