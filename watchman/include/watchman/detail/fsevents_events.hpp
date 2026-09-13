//
// fsevents_events.hpp
// ~~~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2023 Jack (jack.arain at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
//

#pragma once

#include "watchman/notify_event.hpp"

#include <boost/filesystem.hpp>

#include <algorithm>
#include <set>
#include <utility>
#include <vector>

#include <cstddef>

namespace watchman {
	namespace detail {

		// 一条 FSEvents 事件里与换算有关的信息。
		struct fsevents_event
		{
			bool created = false;
			bool removed = false;
			bool renamed = false;
			bool modified = false;
		};

		// 把 FSEvents 事件换算成统一的事件类型。
		//
		// FSEvents 会把短时间内同一路径上的多次变化合并上报，合并后的事件
		// 仍然带着创建标志，因此只在路径第一次出现时才报告创建；它也不给出
		// 重命名两侧的对应关系，同一批里靠路径是否还存在来配对。
		class fsevents_event_filter
		{
		public:
			// 换算一条事件。path_exists 表示路径当前是否还在，只有重命名
			// 事件会用到；重命名先攒着，等 flush_renames 再配对。
			void add_event(notify_events& batch, const fsevents_event& event,
				const fs::path& path, bool path_exists)
			{
				const event_type type = classify(event, path, path_exists);

				if (type == event_type::unknown || type == event_type::rename)
					return;

				notify_event item;
				item.type_ = type;
				item.path_ = path;

				batch.push_back(std::move(item));
			}

			// 取出攒下的重命名：成对的合成一个带 new_path_ 的事件，落单的
			// 按单侧事件报告。
			void flush_renames(notify_events& batch)
			{
				const std::size_t paired =
					std::min(m_sources.size(), m_targets.size());

				for (std::size_t i = 0; i < paired; ++i)
					batch.push_back(make_rename(m_sources[i], m_targets[i]));

				for (std::size_t i = paired; i < m_sources.size(); ++i)
					batch.push_back(make_rename(m_sources[i], {}));

				for (std::size_t i = paired; i < m_targets.size(); ++i)
					batch.push_back(make_rename(m_targets[i], {}));

				m_sources.clear();
				m_targets.clear();
			}

			void clear() noexcept
			{
				m_known_paths.clear();
				m_sources.clear();
				m_targets.clear();
			}

		private:
			event_type classify(const fsevents_event& event,
				const fs::path& path, bool exists)
			{
				if (event.renamed)
				{
					record_rename(path, exists);
					return event_type::rename;
				}

				if (event.removed)
				{
					m_known_paths.erase(path);
					return event_type::deletion;
				}

				if (event.created)
				{
					// 已经出现过的路径再次带着创建标志上报，说明是合并
					// 进来的修改。
					return m_known_paths.insert(path).second
						? event_type::creation
						: event_type::modification;
				}

				if (event.modified)
				{
					m_known_paths.insert(path);
					return event_type::modification;
				}

				return event_type::unknown;
			}

			void record_rename(const fs::path& path, bool exists)
			{
				if (exists)
				{
					m_known_paths.insert(path);
					m_targets.push_back(path);
					return;
				}

				m_known_paths.erase(path);
				m_sources.push_back(path);
			}

			static notify_event make_rename(const fs::path& path,
				const fs::path& new_path)
			{
				notify_event event;
				event.type_ = event_type::rename;
				event.path_ = path;
				event.new_path_ = new_path;

				return event;
			}

		private:
			std::set<fs::path> m_known_paths;
			std::vector<fs::path> m_sources;
			std::vector<fs::path> m_targets;
		};
	} // namespace detail
} // namespace watchman
