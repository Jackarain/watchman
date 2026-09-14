//
// path_remap_test.cpp
// ~~~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2023 Jack (jack.arain at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
//

#define BOOST_TEST_MODULE path_remap
#include "test_framework.hpp"

#include <watchman/detail/path_remap.hpp>

#include <boost/filesystem.hpp>

namespace {

	namespace fs = boost::filesystem;

	// 用 generic_string 比较，避免 Windows 上的路径分隔符差异。
	std::string remap(const fs::path& real_root, const fs::path& base,
		const fs::path& path)
	{
		fs::path result;

		if (!watchman::detail::remap_under(real_root, base, path, result))
			return {};

		return result.generic_string();
	}

	BOOST_AUTO_TEST_CASE(same_root)
	{
		BOOST_TEST(remap("/watch", "/watch", "/watch") == "/watch");
		BOOST_TEST(remap("/watch", "/watch", "/watch/a/b.txt")
			== "/watch/a/b.txt");
	}

	// macOS 上 /var 是指向 /private/var 的符号链接，FSEvents 上报的是解析过
	// 符号链接的真实路径。
	BOOST_AUTO_TEST_CASE(symlinked_root)
	{
		BOOST_TEST(remap("/private/var/tmp/w", "/var/tmp/w",
			"/private/var/tmp/w/file.txt") == "/var/tmp/w/file.txt");

		BOOST_TEST(remap("/private/var/tmp/w", "/var/tmp/w",
			"/private/var/tmp/w/sub/file.txt") == "/var/tmp/w/sub/file.txt");

		BOOST_TEST(remap("/private/var/tmp/w", "/var/tmp/w",
			"/private/var/tmp/w") == "/var/tmp/w");
	}

	BOOST_AUTO_TEST_CASE(outside_root)
	{
		// 前缀相同的兄弟目录不属于监视范围。
		BOOST_TEST(remap("/watch", "/registered", "/watch-other/a").empty());
		BOOST_TEST(remap("/watch", "/registered", "/other/a").empty());
		BOOST_TEST(remap("/watch", "/registered", "/watch") == "/registered");
	}

	BOOST_AUTO_TEST_CASE(remap_identity)
	{
		// 上报路径已经是注册形式时，real_root 与 base 相同，结果保持不变。
		BOOST_TEST(remap("/registered", "/registered",
			"/registered/a/b.txt") == "/registered/a/b.txt");
	}
} // namespace
