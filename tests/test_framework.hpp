//
// test_framework.hpp
// ~~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2023 Jack (jack.arain at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
//

#pragma once

// 测试源文件要在定义 BOOST_TEST_MODULE 之后第一时间包含本文件，再包含其它
// 头文件。Boost.Test 会引入系统头文件，需要先把它们带来的干扰挡掉：
//
// - Windows 上会引入 <windows.h>，它默认定义 min/max 宏，并且包含次序上要求
//   winsock2.h 在前，否则 asio 会拒绝编译。
// - macOS 的 CoreServices 会把 nil 定义成宏，与 Boost.Test 里的 nil 类型冲突。

#if defined(_WIN32)
#define NOMINMAX
#include <winsock2.h>
#endif

#include <boost/test/included/unit_test.hpp>
