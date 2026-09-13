//
// linux_watchman.hpp
// ~~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2023 Jack (jack.arain at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
//

#pragma once

#include <array>
#include <cerrno>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/asio/posix/stream_descriptor.hpp>
#include <boost/bimap.hpp>
#include <boost/filesystem.hpp>
#include <boost/system/error_code.hpp>

#include <sys/inotify.h>

#include "watchman/detail/watch_service_base.hpp"
#include "watchman/notify_event.hpp"

namespace watchman {

	namespace net = boost::asio;
	namespace fs = boost::filesystem;

	inline const size_t read_buffer_size = 8192;

	// Linux 使用 inotify 递归监视目录树。
	//
	// 等待动作直接交给 asio 的描述符操作，因此按操作取消、关联执行器等
	// 语义都由 asio 提供；每个等待自带缓冲区，可以并发发起。
	template <typename Executor = net::any_io_executor>
	class linux_watch_service
		: public detail::watch_service_base<linux_watch_service<Executor>, Executor>
	{
	private:
		using base_type =
			detail::watch_service_base<linux_watch_service<Executor>, Executor>;
		using descriptor_type = net::posix::basic_stream_descriptor<Executor>;
		using watch_descriptors = boost::bimap<int, fs::path>;

		friend base_type;

		linux_watch_service(const linux_watch_service&) = delete;
		linux_watch_service& operator=(const linux_watch_service&) = delete;

		static constexpr std::uint32_t watch_mask =
			static_cast<std::uint32_t>(IN_CREATE | IN_MODIFY |
				IN_MOVED_FROM | IN_MOVED_TO | IN_DELETE);

		// 每个等待自带缓冲区，因此可以并发发起。
		struct read_state
		{
			linux_watch_service* service_ = nullptr;
			std::unique_ptr<std::array<char, read_buffer_size>> bufs_;

			template <typename Handler>
			void complete(Handler handler, boost::system::error_code ec,
				std::size_t bytes)
			{
				notify_events events;

				if (!ec)
					service_->read_events(
						std::string_view(bufs_->data(), bytes), events);

				std::move(handler)(ec, std::move(events));
			}
		};

	public:
		template <typename Executor1>
		struct rebind
		{
			using other = linux_watch_service<Executor1>;
		};

		linux_watch_service(const Executor& ex, const fs::path& dir,
			const std::vector<fs::path>& excluded_dirs = {})
			: base_type(ex, excluded_dirs)
			, m_descriptor(ex)
		{
			this->open(dir);
		}

		explicit linux_watch_service(const Executor& ex,
			const std::vector<fs::path>& excluded_dirs = {})
			: base_type(ex, excluded_dirs)
			, m_descriptor(ex)
		{}

		~linux_watch_service()
		{
			boost::system::error_code ignore_ec;
			this->close(ignore_ec);
		}

		// 内部持有等待状态与互斥量，对象不可拷贝、不可移动。
		linux_watch_service(linux_watch_service&&) = delete;
		linux_watch_service& operator=(linux_watch_service&&) = delete;

		// 底层的 inotify 描述符。
		int native_handle() const noexcept { return m_descriptor.native_handle(); }

	private:
		// ---------- watch_service_base 要求的实现 ----------

		void open_impl(const fs::path& dir, boost::system::error_code& ec)
		{
			boost::system::error_code ignore_ec;
			close_impl(ignore_ec);

			m_descriptor.assign(inotify_init1(IN_CLOEXEC | IN_NONBLOCK), ec);

			if (ec)
				return;

			{
				std::lock_guard<std::mutex> lock(m_mtx);

				if (add_tree(dir, ec))
					return;
			}

			m_descriptor.close(ignore_ec);
			clear_state();
		}

		void close_impl(boost::system::error_code& ec)
		{
			m_descriptor.close(ec);
			clear_state();
		}

		void cancel_impl(boost::system::error_code& ec)
		{
			// 描述符本身保持打开，只中止未完成的等待。
			m_descriptor.cancel(ec);
		}

		bool is_open_impl() const noexcept
		{
			return m_descriptor.is_open();
		}

		template <typename Handler>
		void async_wait_impl(Handler&& handler)
		{
			using handler_type = std::decay_t<Handler>;

			read_state state;
			state.service_ = this;
			state.bufs_ = std::make_unique<std::array<char, read_buffer_size>>();

			auto* buffer = state.bufs_->data();

			m_descriptor.async_read_some(
				net::buffer(buffer, read_buffer_size),
				detail::state_handler<handler_type, read_state>(
					std::forward<Handler>(handler), std::move(state),
					this->get_executor()));
		}

		// ---------- inotify 事件解析 ----------

		void read_events(std::string_view data, notify_events& events)
		{
			std::lock_guard<std::mutex> lock(m_mtx);

			m_bufs_pending.append(data);

			// 内核数据可能停在事件中间，剩下的留到下一次读取。
			while (has_complete_event())
			{
				const inotify_event* event = current_event();

				if (event->mask & IN_IGNORED)
				{
					drop_current_event();
					continue;
				}

				parse_event(*event, events);
				drop_current_event();
			}

			// 同一批数据中没有配对到移入的移出事件，按原路径上报。
			flush_pending_renames(events);
		}

		void parse_event(const inotify_event& event, notify_events& events)
		{
			const fs::path path = event_path(event);

			if (event.mask & IN_MOVED_FROM)
			{
				on_moved_from(event, path);
				return;
			}

			if (event.mask & IN_MOVED_TO)
			{
				on_moved_to(event, path, events);
				return;
			}

			events.push_back(make_event(notify_type(event.mask), path));
			update_watches(event.mask, path);
		}

		// 移出事件先记下 cookie，等同一批数据中的移入事件来配对。
		void on_moved_from(const inotify_event& event, const fs::path& path)
		{
			if (event.mask & IN_ISDIR)
				remove_watch_tree(path);

			m_pending_renames.emplace(event.cookie, path);
		}

		void on_moved_to(const inotify_event& event, const fs::path& path,
			notify_events& events)
		{
			const auto it = m_pending_renames.find(event.cookie);

			if (it == m_pending_renames.end())
			{
				events.push_back(make_event(event_type::rename, path));
			}
			else
			{
				notify_event notify = make_event(event_type::rename, it->second);
				notify.new_path_ = path;
				events.push_back(notify);

				m_pending_renames.erase(it);
			}

			if (event.mask & IN_ISDIR)
				add_tree_best_effort(path);
		}

		void flush_pending_renames(notify_events& events)
		{
			for (const auto& item : m_pending_renames)
				events.push_back(make_event(event_type::rename, item.second));

			m_pending_renames.clear();
		}

		void update_watches(std::uint32_t mask, const fs::path& path)
		{
			if ((mask & (IN_CREATE | IN_ISDIR)) == (IN_CREATE | IN_ISDIR))
			{
				add_tree_best_effort(path);
				return;
			}

			if ((mask & (IN_DELETE | IN_ISDIR)) == (IN_DELETE | IN_ISDIR))
				remove_watch_tree(path);
		}

		static event_type notify_type(std::uint32_t mask) noexcept
		{
			if (mask & IN_CREATE)
				return event_type::creation;

			if (mask & IN_DELETE)
				return event_type::deletion;

			if (mask & IN_MODIFY)
				return event_type::modification;

			if (mask & (IN_MOVED_FROM | IN_MOVED_TO))
				return event_type::rename;

			return event_type::unknown;
		}

		const inotify_event* current_event() const noexcept
		{
			return reinterpret_cast<const inotify_event*>(m_bufs_pending.data());
		}

		bool has_complete_event() const noexcept
		{
			if (m_bufs_pending.size() < sizeof(inotify_event))
				return false;

			return m_bufs_pending.size() >=
				sizeof(inotify_event) + current_event()->len;
		}

		void drop_current_event()
		{
			m_bufs_pending.erase(0, sizeof(inotify_event) + current_event()->len);
		}

		fs::path event_path(const inotify_event& event) const
		{
			const auto dir = find_dir(event.wd);

			if (dir)
				return *dir / event.name;

			// 目录本身已被移除时退化为条目名。
			return event.name;
		}

		std::optional<fs::path> find_dir(int wd) const
		{
			const auto it = m_watch_descriptors.left.find(wd);

			if (it == m_watch_descriptors.left.end())
				return {};

			return it->second;
		}

		// ---------- 监视树 ----------
		//
		// 以下函数都会读写 m_watch_descriptors，调用者需持有 m_mtx。

		// 监视 dir 及其所有子目录，返回根目录是否注册成功。
		bool add_tree(const fs::path& dir, boost::system::error_code& ec)
		{
			if (!add_directory(dir, ec))
				return false;

			add_sub_directories(dir);
			return true;
		}

		// 新增目录时尽力登记，失败通常意味着目录已经被删除。
		void add_tree_best_effort(const fs::path& dir)
		{
			boost::system::error_code ignore_ec;
			add_directory(dir, ignore_ec);
			add_sub_directories(dir);
		}

		// 被排除的目录与符号链接直接跳过，不算失败。
		bool add_directory(const fs::path& dir, boost::system::error_code& ec)
		{
			ec.clear();

			if (this->is_excluded(dir) || is_symlink(dir))
				return true;

			if (!fs::is_directory(dir, ec))
			{
				if (!ec)
					ec.assign(boost::system::errc::not_a_directory,
						boost::system::generic_category());

				return false;
			}

			if (m_watch_descriptors.right.count(dir) != 0)
				return true;

			const int wd = inotify_add_watch(m_descriptor.native_handle(),
				dir.c_str(), watch_mask);

			if (wd < 0)
			{
				ec.assign(errno, boost::system::generic_category());
				return false;
			}

			m_watch_descriptors.insert(watch_descriptors::value_type(wd, dir));
			return true;
		}

		void add_sub_directories(const fs::path& dir)
		{
			boost::system::error_code ec;
			fs::directory_iterator end;

			for (fs::directory_iterator it(dir, ec); !ec && it != end; it.increment(ec))
			{
				const fs::path child = it->path();

				if (this->is_excluded(child) || !is_directory(child))
					continue;

				boost::system::error_code ignore_ec;
				add_directory(child, ignore_ec);
				add_sub_directories(child);
			}
		}

		// 尚未收到 IN_IGNORED 的子目录需要按路径前缀一起清理。
		void remove_watch_tree(const fs::path& dir)
		{
			std::vector<int> stale;

			for (const auto& item : m_watch_descriptors.right)
			{
				if (detail::is_under(dir, item.first))
					stale.push_back(item.second);
			}

			for (const int wd : stale)
			{
				inotify_rm_watch(m_descriptor.native_handle(), wd);
				m_watch_descriptors.left.erase(wd);
			}
		}

		void clear_state()
		{
			std::lock_guard<std::mutex> lock(m_mtx);

			m_watch_descriptors.clear();
			m_bufs_pending.clear();
			m_pending_renames.clear();
		}

		static bool is_symlink(const fs::path& path)
		{
			boost::system::error_code ignore_ec;
			return fs::is_symlink(path, ignore_ec);
		}

		static bool is_directory(const fs::path& path)
		{
			boost::system::error_code ignore_ec;
			return fs::is_directory(path, ignore_ec);
		}

		static notify_event make_event(event_type type, const fs::path& path)
		{
			notify_event event;
			event.type_ = type;
			event.path_ = path;

			return event;
		}

	private:
		descriptor_type m_descriptor;
		std::mutex m_mtx;
		watch_descriptors m_watch_descriptors;
		std::string m_bufs_pending;
		std::map<std::uint32_t, fs::path> m_pending_renames;
	};

	using linux_watch = linux_watch_service<>;
} // namespace watchman
