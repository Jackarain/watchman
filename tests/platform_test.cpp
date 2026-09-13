//
// platform_test.cpp
// ~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2023 Jack (jack.arain at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
//

#include <string>
#include <type_traits>

#include <boost/predef.h>

#include <watchman/watchman.hpp>

#include "test_util.hpp"

// 按操作系统分派到对应的实现。
#if BOOST_OS_WINDOWS
static_assert(std::is_same_v<watchman::watcher, watchman::windows_watch>);
#elif BOOST_OS_LINUX
static_assert(std::is_same_v<watchman::watcher, watchman::linux_watch>);
#elif BOOST_OS_MACOS
static_assert(std::is_same_v<watchman::watcher, watchman::macos_watch>);
#endif

int main()
{
	WATCHMAN_CHECK(std::string(watchman::to_string(watchman::event_type::rename))
		== "rename");

	WATCHMAN_CHECK(sizeof(watchman::watcher) > 0);

	std::printf("platform: %s\n", BOOST_PLATFORM);

	return watchman::test::summary("platform");
}
