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

#include <boost/filesystem.hpp>

namespace watchman {
	namespace test {

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
