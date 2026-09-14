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

#define BOOST_TEST_MODULE path_exclusion
#include "test_framework.hpp"

#include <watchman/detail/path_exclusion.hpp>

#include <vector>

namespace {

	namespace fs = boost::filesystem;

	using watchman::detail::is_excluded;

	BOOST_AUTO_TEST_CASE(empty_exclusion_list)
	{
		// 未配置排除目录时，任何路径都不被排除。
		BOOST_TEST(!is_excluded({}, "/tmp/a/b"));
	}

	BOOST_AUTO_TEST_CASE(excluded_subtrees)
	{
		const std::vector<fs::path> excluded{
			"/tmp/watch/a",
			"/tmp/watch/b/c",
		};

		BOOST_TEST(is_excluded(excluded, "/tmp/watch/a"));
		BOOST_TEST(is_excluded(excluded, "/tmp/watch/a/file.txt"));
		BOOST_TEST(is_excluded(excluded, "/tmp/watch/a/sub/file.txt"));

		BOOST_TEST(is_excluded(excluded, "/tmp/watch/b/c/file.txt"));
		BOOST_TEST(!is_excluded(excluded, "/tmp/watch/b"));

		// 前缀相同的兄弟目录不属于排除范围。
		BOOST_TEST(!is_excluded(excluded, "/tmp/watch/ab/file.txt"));
		BOOST_TEST(!is_excluded(excluded, "/tmp/watch"));

		BOOST_TEST(!is_excluded(excluded, "/tmp/other/file.txt"));

		// 仅做字面比较，不对 ".." 做规范化处理。
		BOOST_TEST(!is_excluded(excluded, "/tmp/watch/a/../a/file.txt"));
	}
} // namespace
