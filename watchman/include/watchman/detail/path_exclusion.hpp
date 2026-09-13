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

#include <string>
#include <vector>

#include <boost/filesystem.hpp>

namespace watchman {
	namespace detail {

		namespace fs = boost::filesystem;

		// 判断 path 是否位于 excluded_dirs 中的任一目录之下。
		inline bool is_excluded(const std::vector<fs::path>& excluded_dirs,
			const fs::path& path)
		{
			if (excluded_dirs.empty())
				return false;

			for (const auto& excluded : excluded_dirs)
			{
				// lexically_relative 返回从 excluded 到 path 的相对路径。
				// 若 path 在 excluded 目录下（如 excluded=/a, path=/a/b/c），
				// 则返回 "b/c"（不以 ".." 开头）；否则返回 "../..." 或空路径。
				auto rel = path.lexically_relative(excluded);
				if (!rel.empty() && !rel.string().starts_with(".."))
					return true;
			}
			return false;
		}
	} // namespace detail
} // namespace watchman
