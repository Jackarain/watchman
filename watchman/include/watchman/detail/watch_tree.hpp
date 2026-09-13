//
// watch_tree.hpp
// ~~~~~~~~~~~~~~
//
// Copyright (c) 2023 Jack (jack.arain at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
//

#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <boost/filesystem.hpp>
#include <boost/system/error_code.hpp>

#include "watchman/detail/dir_scan.hpp"
#include "watchman/detail/path_exclusion.hpp"
#include "watchman/notify_event.hpp"

namespace watchman {
	namespace detail {

		namespace fs = boost::filesystem;

		// 内核句柄：文件描述符或者平台自己的监视对象地址。
		using watch_handle = std::uintptr_t;

		inline constexpr watch_handle invalid_handle = 0;

		// 平台提供的单个节点监视动作。
		class tree_backend
		{
		public:
			virtual ~tree_backend() = default;

			// 开始监视 path，返回内核句柄；失败时返回 invalid_handle。
			virtual watch_handle attach(const fs::path& path, bool is_dir,
				boost::system::error_code& ec) = 0;

			// 结束对句柄的监视，不会失败。
			virtual void detach(watch_handle handle) noexcept = 0;
		};

		// 目录树监视的公共记账：维护路径与内核句柄的对应关系以及目录快照，
		// 用两次快照的差异还原创建与删除事件。kqueue、event ports 这类只
		// 提供“某个节点发生变化”的平台通过它得到统一的目录事件。
		//
		// 本类不加锁，调用方需要保证串行访问。
		class watch_tree
		{
		public:
			watch_tree(tree_backend& backend,
				std::vector<fs::path> excluded_dirs)
				: m_backend(backend)
				, m_excluded_dirs(std::move(excluded_dirs))
			{}

			watch_tree(const watch_tree&) = delete;
			watch_tree& operator=(const watch_tree&) = delete;

			// 监视 root 及其子树，返回根节点是否登记成功。
			bool watch_root(const fs::path& root, boost::system::error_code& ec)
			{
				return add_tree(root, ec);
			}

			// 释放全部监视。
			void clear() noexcept
			{
				std::vector<watch_handle> handles;

				for (const auto& item : m_nodes)
					handles.push_back(item.first);

				m_nodes.clear();
				m_handles.clear();

				for (const watch_handle handle : handles)
					m_backend.detach(handle);
			}

			bool empty() const noexcept { return m_nodes.empty(); }

			const fs::path* find_path(watch_handle handle) const
			{
				const auto it = m_nodes.find(handle);

				return it == m_nodes.end() ? nullptr : &it->second.path_;
			}

			bool is_dir(watch_handle handle) const
			{
				const auto it = m_nodes.find(handle);

				return it != m_nodes.end() && it->second.is_dir_;
			}

			// 目录内容发生变化：重扫目录，产出创建与删除事件，并同步新增
			// 节点的监视。
			void resync(watch_handle handle, notify_events& events)
			{
				const auto it = m_nodes.find(handle);

				if (it == m_nodes.end())
					return;

				node& item = it->second;
				std::vector<std::string> children;

				if (!list_children(item.path_, children))
				{
					events.push_back(
						make_event(event_type::deletion, item.path_));

					drop_path(item.path_);
					return;
				}

				const dir_entries_diff diff =
					diff_dir_entries(item.children_, children);

				item.children_ = std::move(children);

				report_removed(item.path_, diff.removed, events);
				report_added(item.path_, diff.added, events);
			}

			// 节点被删除或改名：产出事件并递归释放它的监视。
			void drop_node(watch_handle handle, event_type type,
				notify_events& events)
			{
				const auto it = m_nodes.find(handle);

				if (it == m_nodes.end())
					return;

				events.push_back(make_event(type, it->second.path_));

				drop_path(it->second.path_);
			}

			static notify_event make_event(event_type type, const fs::path& path)
			{
				notify_event event;
				event.type_ = type;
				event.path_ = path;

				return event;
			}

		private:
			struct node
			{
				fs::path path_;
				bool is_dir_ = false;
				std::vector<std::string> children_;
			};

			// 监视 path 及其子树，path 自身失败时返回 false。
			bool add_tree(const fs::path& path, boost::system::error_code& ec)
			{
				if (!add_node(path, ec))
					return false;

				remember_children(path);
				watch_children(path);

				return true;
			}

			// 监视单个节点；被排除的路径与符号链接直接跳过。
			bool add_node(const fs::path& path, boost::system::error_code& ec)
			{
				ec.clear();

				if (is_excluded(path) || is_symlink(path))
					return true;

				if (m_handles.count(path) != 0)
					return true;

				boost::system::error_code stat_ec;

				if (!fs::exists(path, stat_ec))
				{
					ec = stat_ec ? stat_ec
						: boost::system::error_code(
							boost::system::errc::no_such_file_or_directory,
							boost::system::generic_category());

					return false;
				}

				const bool is_directory = is_dir(path);
				const watch_handle handle =
					m_backend.attach(path, is_directory, ec);

				if (handle == invalid_handle)
					return false;

				node item;
				item.path_ = path;
				item.is_dir_ = is_directory;

				m_nodes.emplace(handle, std::move(item));
				m_handles.emplace(path, handle);

				return true;
			}

			// 记录目录当前的条目，作为下一次比对的基准。
			void remember_children(const fs::path& dir)
			{
				node* item = find_node(dir);

				if (item == nullptr || !item->is_dir_)
					return;

				list_children(dir, item->children_);
			}

			// 子条目可能在扫描与监视之间被删除，这里的失败不再上报。
			void watch_children(const fs::path& dir)
			{
				const node* item = find_node(dir);

				if (item == nullptr)
					return;

				const std::vector<std::string> children = item->children_;

				for (const auto& name : children)
				{
					boost::system::error_code ignore_ec;
					add_tree(dir / name, ignore_ec);
				}
			}

			// 递归释放 path 及其子节点的监视。
			void drop_path(const fs::path& path)
			{
				const auto it = m_handles.find(path);

				if (it == m_handles.end())
					return;

				const watch_handle handle = it->second;
				m_handles.erase(it);

				std::vector<std::string> children;
				const auto node_it = m_nodes.find(handle);

				if (node_it != m_nodes.end())
				{
					children = std::move(node_it->second.children_);
					m_nodes.erase(node_it);
				}

				m_backend.detach(handle);

				for (const auto& name : children)
					drop_path(path / name);
			}

			void report_removed(const fs::path& dir,
				const std::vector<std::string>& names, notify_events& events)
			{
				for (const auto& name : names)
				{
					events.push_back(
						make_event(event_type::deletion, dir / name));

					drop_path(dir / name);
				}
			}

			void report_added(const fs::path& dir,
				const std::vector<std::string>& names, notify_events& events)
			{
				for (const auto& name : names)
				{
					events.push_back(
						make_event(event_type::creation, dir / name));

					boost::system::error_code ignore_ec;
					add_tree(dir / name, ignore_ec);
				}
			}

			bool list_children(const fs::path& dir,
				std::vector<std::string>& children) const
			{
				if (!list_dir_entries(dir, children))
					return false;

				filter_excluded(dir, children);

				return true;
			}

			void filter_excluded(const fs::path& dir,
				std::vector<std::string>& children) const
			{
				const auto excluded = [this, &dir](const std::string& name)
				{
					return detail::is_excluded(m_excluded_dirs, dir / name);
				};

				children.erase(
					std::remove_if(children.begin(), children.end(), excluded),
					children.end());
			}

			node* find_node(const fs::path& path)
			{
				const auto it = m_handles.find(path);

				if (it == m_handles.end())
					return nullptr;

				const auto node_it = m_nodes.find(it->second);

				return node_it == m_nodes.end() ? nullptr : &node_it->second;
			}

			bool is_excluded(const fs::path& path) const
			{
				return detail::is_excluded(m_excluded_dirs, path);
			}

			static bool is_symlink(const fs::path& path)
			{
				boost::system::error_code ignore_ec;
				return fs::is_symlink(path, ignore_ec);
			}

			static bool is_dir(const fs::path& path)
			{
				boost::system::error_code ignore_ec;
				return fs::is_directory(path, ignore_ec);
			}

		private:
			tree_backend& m_backend;
			std::vector<fs::path> m_excluded_dirs;
			std::map<watch_handle, node> m_nodes;
			std::map<fs::path, watch_handle> m_handles;
		};
	} // namespace detail
} // namespace watchman
