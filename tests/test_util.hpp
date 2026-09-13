//
// test_util.hpp
// ~~~~~~~~~~~~~
//
// Copyright (c) 2023 Jack (jack.arain at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
//

#pragma once

#include <cstdio>
#include <string>

#include <boost/filesystem.hpp>

namespace watchman {
	namespace test {

		inline int& failures() noexcept
		{
			static int count = 0;
			return count;
		}

		inline void check(bool ok, const char* expr, const char* file, int line)
		{
			if (ok)
				return;

			++failures();
			std::fprintf(stderr, "%s:%d: check failed: %s\n", file, line, expr);
		}

		inline int summary(const char* name)
		{
			if (failures() == 0)
			{
				std::printf("[ PASSED ] %s\n", name);
				return 0;
			}

			std::fprintf(stderr, "[ FAILED ] %s (%d failed check(s))\n",
				name, failures());

			return 1;
		}

		// 测试用的临时目录，析构时自动清理。
		class temp_dir
		{
		public:
			temp_dir()
				: path_(boost::filesystem::temp_directory_path() /
					boost::filesystem::unique_path("watchman-%%%%-%%%%-%%%%.tmp"))
			{
				boost::filesystem::create_directories(path_);
			}

			~temp_dir()
			{
				boost::system::error_code ignore_ec;
				boost::filesystem::remove_all(path_, ignore_ec);
			}

			temp_dir(const temp_dir&) = delete;
			temp_dir& operator=(const temp_dir&) = delete;

			const boost::filesystem::path& path() const noexcept { return path_; }

		private:
			boost::filesystem::path path_;
		};
	} // namespace test
} // namespace watchman

#define WATCHMAN_CHECK(expr) \
	::watchman::test::check(!!(expr), #expr, __FILE__, __LINE__)
