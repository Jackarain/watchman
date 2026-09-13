//
// solaris_watchman.hpp
// ~~~~~~~~~~~~~~~~~~~~
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
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <port.h>
#include <sys/stat.h>
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

	// Solaris/illumos 使用 event ports 监视文件系统。
	//
	// 每个被监视的文件与目录都通过 port_associate 关联到 port 上，事件里
	// 带有对应节点的句柄，因此文件修改可以直接上报；目录内容的变化同样
	// 由两次目录扫描的差异还原；事件是一次性的，处理完需要重新关联。
	template <typename Executor = net::any_io_executor>
	class solaris_watch_service
		: public detail::watch_service_base<solaris_watch_service<Executor>, Executor>
		, private detail::event_source
		, private detail::tree_backend
	{
	private:
		using base_type =
			detail::watch_service_base<solaris_watch_service<Executor>, Executor>;
		using watch_handle = detail::watch_handle;

		friend base_type;

		solaris_watch_service(const solaris_watch_service&) = delete;
		solaris_watch_service& operator=(const solaris_watch_service&) = delete;

		static constexpr int event_batch_size = 128;

		static constexpr int node_events =
			FILE_MODIFIED | FILE_ATTRIB | FILE_DELETE |
			FILE_RENAME_TO | FILE_RENAME_FROM | FILE_NOFOLLOW;

		// event ports 关联的是 file_obj_t，其中保存路径字符串的地址，因此
		// 字符串必须与关联同生命周期。
		class port_file
		{
		public:
			explicit port_file(const fs::path& path)
				: path_(path.string())
			{
				object_.fo_name = reinterpret_cast<decltype(object_.fo_name)>(
					path_.c_str());

				struct stat info{};

				if (::stat(path_.c_str(), &info) == 0)
				{
					object_.fo_atime = info.st_atim;
					object_.fo_mtime = info.st_mtim;
					object_.fo_ctime = info.st_ctim;
				}
			}

			file_obj_t& object() noexcept { return object_; }

		private:
			std::string path_;
			file_obj_t object_{};
		};

	public:
		template <typename Executor1>
		struct rebind
		{
			using other = solaris_watch_service<Executor1>;
		};

		solaris_watch_service(const Executor& ex, const fs::path& dir,
			const std::vector<fs::path>& excluded_dirs = {})
			: base_type(ex, excluded_dirs)
			, m_tree(*this, excluded_dirs)
			, m_pump(*this, ex)
		{
			this->open(dir);
		}

		explicit solaris_watch_service(const Executor& ex,
			const std::vector<fs::path>& excluded_dirs = {})
			: base_type(ex, excluded_dirs)
			, m_tree(*this, excluded_dirs)
			, m_pump(*this, ex)
		{}

		~solaris_watch_service()
		{
			boost::system::error_code ignore_ec;
			this->close(ignore_ec);
		}

		// 后台线程与内核对象都由本对象持有，不可拷贝、不可移动。
		solaris_watch_service(solaris_watch_service&&) = delete;
		solaris_watch_service& operator=(solaris_watch_service&&) = delete;

		// 底层的 port 描述符。
		int native_handle() const noexcept { return m_port.load(); }

	private:
		// ---------- watch_service_base 要求的实现 ----------

		void open_impl(const fs::path& dir, boost::system::error_code& ec)
		{
			boost::system::error_code ignore_ec;
			close_impl(ignore_ec);

			ec.clear();

			if (!open_port(ec))
				return;

			{
				std::lock_guard<std::mutex> lock(m_mtx);

				if (!m_tree.watch_root(dir, ec))
				{
					m_tree.clear();
					close_port(ignore_ec);
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
			close_port(ec);
		}

		void cancel_impl(boost::system::error_code& ec)
		{
			ec.clear();
			m_pump.cancel_all();
		}

		bool is_open_impl() const noexcept { return m_port.load() >= 0; }

		template <typename Handler>
		void async_wait_impl(Handler&& handler)
		{
			m_pump.async_wait(std::forward<Handler>(handler));
		}

		// ---------- event_source ----------

		boost::system::error_code wait(notify_events& events) override
		{
			port_event_t items[event_batch_size];

			for (;;)
			{
				const int port = m_port.load();

				if (port < 0)
					return net::error::operation_aborted;

				// 输入为 1 表示至少等到一个事件，阻塞期间由 port_send 唤醒。
				unsigned int count = 1;

				if (::port_getn(port, items, event_batch_size, &count,
						nullptr) != 0)
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
			}
		}

		void interrupt() noexcept override
		{
			const int port = m_port.load();

			if (port < 0)
				return;

			// PORT_SOURCE_USER 事件用于唤醒阻塞中的 port_getn。
			::port_send(port, 0, nullptr);
		}

		// ---------- tree_backend ----------

		watch_handle attach(const fs::path& path, bool /*is_dir*/,
			boost::system::error_code& ec) override
		{
			ec.clear();

			auto file = std::make_unique<port_file>(path);

			if (!associate(*file, ec))
				return detail::invalid_handle;

			const watch_handle handle =
				reinterpret_cast<watch_handle>(file.get());

			m_files.emplace(handle, std::move(file));

			return handle;
		}

		void detach(watch_handle handle) noexcept override
		{
			const auto it = m_files.find(handle);

			if (it == m_files.end())
				return;

			const int port = m_port.load();

			if (port >= 0)
			{
				::port_dissociate(port, PORT_SOURCE_FILE,
					reinterpret_cast<uintptr_t>(&it->second->object()));
			}

			m_files.erase(it);
		}

		// ---------- event ports ----------

		bool open_port(boost::system::error_code& ec)
		{
			const int fd = ::port_create();

			if (fd < 0)
			{
				ec.assign(errno, boost::system::generic_category());
				return false;
			}

			::fcntl(fd, F_SETFD, FD_CLOEXEC);
			m_port.store(fd);

			return true;
		}

		void close_port(boost::system::error_code& ec) noexcept
		{
			const int port = m_port.exchange(-1);

			if (port < 0)
				return;

			if (::close(port) != 0)
				ec.assign(errno, boost::system::generic_category());
		}

		bool associate(port_file& file, boost::system::error_code& ec)
		{
			const int port = m_port.load();

			if (port < 0)
			{
				ec = net::error::bad_descriptor;
				return false;
			}

			if (::port_associate(port, PORT_SOURCE_FILE,
					reinterpret_cast<uintptr_t>(&file.object()),
					node_events, &file) == 0)
				return true;

			ec.assign(errno, boost::system::generic_category());
			return false;
		}

		// ---------- 事件转换（调用者需持有 m_mtx） ----------

		// 返回是否收到了唤醒事件。
		bool convert_events(const port_event_t* items, unsigned int count,
			notify_events& events)
		{
			bool woken = false;

			for (unsigned int i = 0; i < count; ++i)
			{
				if (items[i].portev_source == PORT_SOURCE_USER)
				{
					woken = true;
					continue;
				}

				if (items[i].portev_source != PORT_SOURCE_FILE)
					continue;

				on_node_changed(
					reinterpret_cast<watch_handle>(items[i].portev_user),
					static_cast<std::uint32_t>(items[i].portev_events),
					events);
			}

			return woken;
		}

		void on_node_changed(watch_handle handle, std::uint32_t flags,
			notify_events& events)
		{
			const fs::path* path = m_tree.find_path(handle);

			if (path == nullptr)
				return;

			if ((flags & (FILE_DELETE | UNMOUNTED | MOUNTEDOVER)) != 0)
			{
				m_tree.drop_node(handle, event_type::deletion, events);
				return;
			}

			if ((flags & FILE_RENAME_FROM) != 0)
			{
				m_tree.drop_node(handle, event_type::rename, events);
				return;
			}

			if (m_tree.is_dir(handle))
			{
				m_tree.resync(handle, events);
			}
			else
			{
				events.push_back(detail::watch_tree::make_event(
					event_type::modification, *path));
			}

			// 事件是一次性的，处理完必须重新关联。
			reassociate(handle);
		}

		void reassociate(watch_handle handle) noexcept
		{
			const auto it = m_files.find(handle);

			if (it == m_files.end())
				return;

			boost::system::error_code ignore_ec;

			// 文件可能已经被删除，下一个目录事件会把它清理掉。
			associate(*it->second, ignore_ec);
		}

	private:
		detail::watch_tree m_tree;
		detail::threaded_pump m_pump;

		std::atomic<int> m_port{ -1 };
		std::mutex m_mtx;
		std::map<watch_handle, std::unique_ptr<port_file>> m_files;
	};

	using solaris_watch = solaris_watch_service<>;
} // namespace watchman
