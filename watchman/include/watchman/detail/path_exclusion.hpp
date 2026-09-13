//
// path_exclusion.hpp
// ~~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2023 Jack (jack.arain at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
//

#pragma once

#include <boost/filesystem.hpp>

#include <string>
#include <vector>

namespace watchman {
	namespace detail {

		namespace fs = boost::filesystem;

		// 判断 path 是否位于 dir 之下（含 dir 自身）。
		//
		// lexically_relative 返回从 dir 到 path 的相对路径：若 path 在 dir
		// 之下（如 dir=/a, path=/a/b/c）则返回 "b/c"（不以 ".." 开头）；
		// 否则返回 "../..." 或空路径。这里只做字面比较，不解析 ".."。
		inline bool is_under(const fs::path& dir, const fs::path& path)
		{
			const auto rel = path.lexically_relative(dir);

			return !rel.empty() && !rel.string().starts_with("..");
		}

		// 判断 path 是否位于 excluded_dirs 中的任一目录之下。
		inline bool is_excluded(const std::vector<fs::path>& excluded_dirs,
			const fs::path& path)
		{
			for (const auto& excluded : excluded_dirs)
			{
				if (is_under(excluded, path))
					return true;
			}

			return false;
		}
	} // namespace detail
} // namespace watchman
