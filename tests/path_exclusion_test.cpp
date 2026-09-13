//
// path_exclusion_test.cpp
// ~~~~~~~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2023 Jack (jack.arain at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
//

#include "test_util.hpp"

#include <watchman/detail/path_exclusion.hpp>

#include <vector>

int main()
{
	using namespace watchman;
	namespace fs = boost::filesystem;

	// 未配置排除目录时，任何路径都不被排除。
	WATCHMAN_CHECK(!detail::is_excluded({}, "/tmp/a/b"));

	const std::vector<fs::path> excluded{
		"/tmp/watch/a",
		"/tmp/watch/b/c",
	};

	WATCHMAN_CHECK(detail::is_excluded(excluded, "/tmp/watch/a"));
	WATCHMAN_CHECK(detail::is_excluded(excluded, "/tmp/watch/a/file.txt"));
	WATCHMAN_CHECK(detail::is_excluded(excluded, "/tmp/watch/a/sub/file.txt"));

	WATCHMAN_CHECK(detail::is_excluded(excluded, "/tmp/watch/b/c/file.txt"));
	WATCHMAN_CHECK(!detail::is_excluded(excluded, "/tmp/watch/b"));

	// 前缀相同的兄弟目录不属于排除范围。
	WATCHMAN_CHECK(!detail::is_excluded(excluded, "/tmp/watch/ab/file.txt"));
	WATCHMAN_CHECK(!detail::is_excluded(excluded, "/tmp/watch"));

	WATCHMAN_CHECK(!detail::is_excluded(excluded, "/tmp/other/file.txt"));

	// 仅做字面比较，不对 ".." 做规范化处理。
	WATCHMAN_CHECK(!detail::is_excluded(excluded, "/tmp/watch/a/../a/file.txt"));

	return test::summary("path_exclusion");
}
