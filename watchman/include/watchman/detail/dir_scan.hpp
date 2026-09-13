//
// dir_scan.hpp
// ~~~~~~~~~~~~
//
// Copyright (c) 2023 Jack (jack.arain at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
//

#pragma once

#include <algorithm>
#include <iterator>
#include <string>
#include <vector>

#include <boost/filesystem.hpp>

namespace watchman {
	namespace detail {

		namespace fs = boost::filesystem;

		// 列出目录下的条目名（不递归），结果按名字排序。
		// 目录不存在或不可读时返回 false。
		inline bool list_dir_entries(const fs::path& dir,
			std::vector<std::string>& entries)
		{
			entries.clear();

			boost::system::error_code ec;
			fs::directory_iterator end;

			for (fs::directory_iterator it(dir, ec); !ec && it != end; it.increment(ec))
				entries.push_back(it->path().filename().string());

			if (ec)
			{
				entries.clear();
				return false;
			}

			std::sort(entries.begin(), entries.end());
			return true;
		}

		struct dir_entries_diff
		{
			std::vector<std::string> added;
			std::vector<std::string> removed;
		};

		// 比较两份已排序的目录条目，得到新增与删除的条目名。
		inline dir_entries_diff diff_dir_entries(
			const std::vector<std::string>& before,
			const std::vector<std::string>& after)
		{
			dir_entries_diff diff;

			std::set_difference(after.begin(), after.end(),
				before.begin(), before.end(),
				std::back_inserter(diff.added));

			std::set_difference(before.begin(), before.end(),
				after.begin(), after.end(),
				std::back_inserter(diff.removed));

			return diff;
		}
	} // namespace detail
} // namespace watchman
