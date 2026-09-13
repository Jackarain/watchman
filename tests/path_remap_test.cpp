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

#include "test_util.hpp"

#include <watchman/detail/path_remap.hpp>

#include <boost/filesystem.hpp>

namespace {

	namespace fs = boost::filesystem;

	std::string remap(const fs::path& real_root, const fs::path& base,
		const fs::path& path)
	{
		fs::path result;

		if (!watchman::detail::remap_under(real_root, base, path, result))
			return {};

		return result.string();
	}

	void test_same_root()
	{
		WATCHMAN_CHECK(remap("/watch", "/watch", "/watch") == "/watch");
		WATCHMAN_CHECK(remap("/watch", "/watch", "/watch/a/b.txt")
			== "/watch/a/b.txt");
	}

	// macOS 上 /var 是指向 /private/var 的符号链接，FSEvents 上报的是解析过
	// 符号链接的真实路径。
	void test_symlinked_root()
	{
		WATCHMAN_CHECK(remap("/private/var/tmp/w", "/var/tmp/w",
			"/private/var/tmp/w/file.txt") == "/var/tmp/w/file.txt");

		WATCHMAN_CHECK(remap("/private/var/tmp/w", "/var/tmp/w",
			"/private/var/tmp/w/sub/file.txt") == "/var/tmp/w/sub/file.txt");

		WATCHMAN_CHECK(remap("/private/var/tmp/w", "/var/tmp/w",
			"/private/var/tmp/w") == "/var/tmp/w");
	}

	void test_outside_root()
	{
		// 前缀相同的兄弟目录不属于监视范围。
		WATCHMAN_CHECK(remap("/watch", "/registered", "/watch-other/a").empty());
		WATCHMAN_CHECK(remap("/watch", "/registered", "/other/a").empty());
		WATCHMAN_CHECK(remap("/watch", "/registered", "/watch") == "/registered");
	}

	void test_remap_identity()
	{
		// 上报路径已经是注册形式时，real_root 与 base 相同，结果保持不变。
		WATCHMAN_CHECK(remap("/registered", "/registered",
			"/registered/a/b.txt") == "/registered/a/b.txt");
	}
} // namespace

int main()
{
	test_same_root();
	test_symlinked_root();
	test_outside_root();
	test_remap_identity();

	return watchman::test::summary("path_remap");
}
