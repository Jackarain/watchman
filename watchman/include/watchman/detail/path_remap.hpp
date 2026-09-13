//
// path_remap.hpp
// ~~~~~~~~~~~~~~
//
// Copyright (c) 2023 Jack (jack.arain at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
//

#pragma once

#include "watchman/detail/path_exclusion.hpp"

#include <boost/filesystem.hpp>

namespace watchman {
	namespace detail {

		// 把位于 real_root 之下的 path 换算成 base 之下的等价路径，成功返回
		// true。系统上报的路径可能是解析过符号链接的真实路径，用它换算回注
		// 册时的路径形式。
		inline bool remap_under(const fs::path& real_root, const fs::path& base,
			const fs::path& path, fs::path& result)
		{
			if (!is_under(real_root, path))
				return false;

			const fs::path rel = path.lexically_relative(real_root);

			result = (rel == fs::path(".")) ? base : base / rel;

			return true;
		}
	} // namespace detail
} // namespace watchman
