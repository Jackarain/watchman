//
// bsd_watchman.hpp
// ~~~~~~~~~~~~~~~~
//
// Copyright (c) 2023 Jack (jack.arain at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
//

#pragma once

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <mutex>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/event.h>
#include <unistd.h>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/error.hpp>
#include <boost/filesystem.hpp>
#include <boost/system/error_code.hpp>

#include "watchman/detail/threaded_pump.hpp"
#include "watchman/detail/watch_service_base.hpp"
#include "watchman/detail/watch_tree.hpp"
#include "watchman/notify_event.hpp"

namespace watchman {

	namespace net = boost::asio;
	namespace fs = boost::filesystem;

	// FreeBSD/OpenBSD/NetBSD/DragonFly 使用 kqueue 监视文件系统。
	//
	// kqueue 只告知被监视的节点发生了变化，不给出目录里变化的条目名，
	// 因此目录事件由两次目录扫描的差异还原；每个被监视的文件与目录各占
	// 一个描述符，递归监视大树时会占用较多描述符。
	template <typename Executor = net::any_io_executor>
	class bsd_watch_service
		: public detail::watch_service_base<bsd_watch_service<Executor>, Executor>
		, private detail::event_source
		, private detail::tree_backend
	{
	private:
		using base_type =
			detail::watch_service_base<bsd_watch_service<Executor>, Executor>;
		using watch_handle = detail::watch_handle;

		friend base_type;

		bsd_watch_service(const bsd_watch_service&) = delete;
		bsd_watch_service& operator=(const bsd_watch_service&) = delete;

		// 用于唤醒阻塞在 kevent 上的工作线程。
		static constexpr uintptr_t wake_ident = 0x7ffffffe;
		static constexpr int event_batch_size = 128;

		static constexpr std::uint32_t node_flags = NOTE_WRITE | NOTE_EXTEND |
			NOTE_ATTRIB | NOTE_LINK | NOTE_DELETE | NOTE_RENAME | NOTE_REVOKE;

	public:
		template <typename Executor1>
		struct rebind
		{
			using other = bsd_watch_service<Executor1>;
		};

		bsd_watch_service(const Executor& ex, const fs::path& dir,
			const std::vector<fs::path>& excluded_dirs = {})
			: base_type(ex, excluded_dirs)
			, m_tree(*this, excluded_dirs)
			, m_pump(*this, ex)
		{
			this->open(dir);
		}

		explicit bsd_watch_service(const Executor& ex,
			const std::vector<fs::path>& excluded_dirs = {})
			: base_type(ex, excluded_dirs)
			, m_tree(*this, excluded_dirs)
			, m_pump(*this, ex)
		{}

		~bsd_watch_service()
		{
			boost::system::error_code ignore_ec;
			this->close(ignore_ec);
		}

		// 后台线程与内核对象都由本对象持有，不可拷贝、不可移动。
		bsd_watch_service(bsd_watch_service&&) = delete;
		bsd_watch_service& operator=(bsd_watch_service&&) = delete;

		// 底层的 kqueue 描述符。
		int native_handle() const noexcept { return m_kqueue.load(); }

	private:
		// ---------- watch_service_base 要求的实现 ----------

		void open_impl(const fs::path& dir, boost::system::error_code& ec)
		{
			boost::system::error_code ignore_ec;
			close_impl(ignore_ec);

			ec.clear();

			if (!open_kqueue(ec))
				return;

			{
				std::lock_guard<std::mutex> lock(m_mtx);

				if (!m_tree.watch_root(dir, ec))
				{
					m_tree.clear();
					close_kqueue(ignore_ec);
					return;
				}
			}

			m_pump.start();
		}

		void close_impl(boost::system::error_code& ec)
		{
			ec.clear();

			// 先结束工作线程，再释放内核对象。
			m_pump.stop(net::error::operation_aborted);

			std::lock_guard<std::mutex> lock(m_mtx);

			m_tree.clear();
			close_kqueue(ec);
		}

		void cancel_impl(boost::system::error_code& ec)
		{
			ec.clear();
			m_pump.cancel_all();
		}

		bool is_open_impl() const noexcept { return m_kqueue.load() >= 0; }

		template <typename Handler>
		void async_wait_impl(Handler&& handler)
		{
			m_pump.async_wait(std::forward<Handler>(handler));
		}

		// ---------- event_source ----------

		// 只在有等待动作时被调用，返回时要么带上一批事件，要么报告被打断。
		boost::system::error_code wait(notify_events& events) override
		{
			struct kevent items[event_batch_size];

			for (;;)
			{
				const int kqueue = m_kqueue.load();

				if (kqueue < 0)
					return net::error::operation_aborted;

				const int count = ::kevent(kqueue, nullptr, 0, items,
					event_batch_size, nullptr);

				if (count < 0)
				{
					if (errno == EINTR)
						continue;

					return boost::system::error_code(errno,
						boost::system::generic_category());
				}

				bool woken = false;

				{
					std::lock_guard<std::mutex> lock(m_mtx);
					woken = convert_events(items, count, events);
				}

				if (!events.empty())
					return {};

				if (woken)
					return net::error::operation_aborted;

				// 事件都被排除规则过滤掉时继续等待。
			}
		}

		void interrupt() noexcept override
		{
			trigger_wake();
		}

		// ---------- tree_backend ----------

		watch_handle attach(const fs::path& path, bool /*is_dir*/,
			boost::system::error_code& ec) override
		{
			ec.clear();

			const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);

			if (fd < 0)
			{
				ec.assign(errno, boost::system::generic_category());
				return detail::invalid_handle;
			}

			if (!register_node(fd, ec))
			{
				::close(fd);
				return detail::invalid_handle;
			}

			return static_cast<watch_handle>(fd);
		}

		void detach(watch_handle handle) noexcept override
		{
			// 关闭描述符会同时撤销 kqueue 上的登记。
			::close(static_cast<int>(handle));
		}

		// ---------- kqueue ----------

		bool open_kqueue(boost::system::error_code& ec)
		{
			const int fd = ::kqueue();

			if (fd < 0)
			{
				ec.assign(errno, boost::system::generic_category());
				return false;
			}

			::fcntl(fd, F_SETFD, FD_CLOEXEC);
			m_kqueue.store(fd);

			if (register_wake())
				return true;

			close_kqueue(ec);

			return false;
		}

		void close_kqueue(boost::system::error_code& ec) noexcept
		{
			const int kqueue = m_kqueue.exchange(-1);

			if (kqueue < 0)
				return;

			if (::close(kqueue) != 0)
				ec.assign(errno, boost::system::generic_category());
		}

		// 登记用于唤醒阻塞中 kevent 的用户事件。
		bool register_wake()
		{
			struct kevent change;
			EV_SET(&change, wake_ident, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0,
				nullptr);

			return ::kevent(m_kqueue.load(), &change, 1, nullptr, 0,
				nullptr) == 0;
		}

		bool register_node(int fd, boost::system::error_code& ec)
		{
			struct kevent change;
			EV_SET(&change, static_cast<uintptr_t>(fd), EVFILT_VNODE,
				EV_ADD | EV_CLEAR, node_flags, 0, nullptr);

			if (::kevent(m_kqueue.load(), &change, 1, nullptr, 0, nullptr) == 0)
				return true;

			ec.assign(errno, boost::system::generic_category());
			return false;
		}

		void trigger_wake() noexcept
		{
			const int kqueue = m_kqueue.load();

			if (kqueue < 0)
				return;

			struct kevent change;
			EV_SET(&change, wake_ident, EVFILT_USER, 0, NOTE_TRIGGER, 0,
				nullptr);

			::kevent(kqueue, &change, 1, nullptr, 0, nullptr);
		}

		// ---------- 事件转换（调用者需持有 m_mtx） ----------

		// 返回是否收到了唤醒事件。
		bool convert_events(const struct kevent* items, int count,
			notify_events& events)
		{
			bool woken = false;

			for (int i = 0; i < count; ++i)
			{
				if (items[i].filter == EVFILT_USER)
				{
					woken = true;
					continue;
				}

				if (items[i].filter != EVFILT_VNODE)
					continue;

				on_node_changed(static_cast<watch_handle>(items[i].ident),
					static_cast<std::uint32_t>(items[i].fflags), events);
			}

			return woken;
		}

		void on_node_changed(watch_handle handle, std::uint32_t flags,
			notify_events& events)
		{
			const fs::path* path = m_tree.find_path(handle);

			if (path == nullptr)
				return;

			if ((flags & (NOTE_DELETE | NOTE_REVOKE)) != 0)
			{
				m_tree.drop_node(handle, event_type::deletion, events);
				return;
			}

			if ((flags & NOTE_RENAME) != 0)
			{
				m_tree.drop_node(handle, event_type::rename, events);
				return;
			}

			if (m_tree.is_dir(handle))
			{
				m_tree.resync(handle, events);
				return;
			}

			events.push_back(
				detail::watch_tree::make_event(event_type::modification, *path));
		}

	private:
		detail::watch_tree m_tree;
		detail::threaded_pump m_pump;

		std::atomic<int> m_kqueue{ -1 };
		std::mutex m_mtx;
	};

	using bsd_watch = bsd_watch_service<>;
} // namespace watchman
