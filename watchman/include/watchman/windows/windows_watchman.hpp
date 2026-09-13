//
// windows_watchman.hpp
// ~~~~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2023 Jack (jack.arain at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
//

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/associated_cancellation_slot.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/windows/overlapped_handle.hpp>
#include <boost/asio/windows/overlapped_ptr.hpp>
#include <boost/filesystem.hpp>
#include <boost/system/error_code.hpp>

#ifndef WIN32_LEAN_AND_MEAN
# define WIN32_LEAN_AND_MEAN
#endif // !WIN32_LEAN_AND_MEAN

#include <windows.h>

#include "watchman/detail/watch_service_base.hpp"
#include "watchman/notify_event.hpp"

namespace watchman {

	namespace net = boost::asio;
	namespace fs = boost::filesystem;

	// Windows 使用 ReadDirectoryChangesW 递归监视目录树。
	//
	// 内核接口本身不支持取消，取消通过 CancelIoEx 完成；每个等待自带缓冲区，
	// 可以并发发起。
	template <typename Executor = net::any_io_executor>
	class windows_watch_service
		: public detail::watch_service_base<windows_watch_service<Executor>, Executor>
	{
	private:
		using base_type =
			detail::watch_service_base<windows_watch_service<Executor>, Executor>;
		using handle_type = net::windows::basic_overlapped_handle<Executor>;

		friend base_type;

		windows_watch_service(const windows_watch_service&) = delete;
		windows_watch_service& operator=(const windows_watch_service&) = delete;

		static constexpr DWORD buffer_size = 8192;

		static constexpr DWORD event_filter =
			FILE_NOTIFY_CHANGE_FILE_NAME |
			FILE_NOTIFY_CHANGE_DIR_NAME |
			FILE_NOTIFY_CHANGE_LAST_WRITE;

		// 每个等待自带缓冲区。
		struct read_state
		{
			windows_watch_service* service_ = nullptr;
			std::unique_ptr<uint8_t[]> bufs_;

			template <typename Handler>
			void complete(Handler handler, boost::system::error_code ec,
				std::size_t /*bytes*/)
			{
				notify_events events;

				if (!ec)
					service_->convert_result(bufs_.get(), events);

				service_->dispatch_completion(std::move(handler), ec,
					std::move(events));
			}
		};

	public:
		template <typename Executor1>
		struct rebind
		{
			using other = windows_watch_service<Executor1>;
		};

		windows_watch_service(const Executor& ex, const fs::path& dir,
			const std::vector<fs::path>& excluded_dirs = {})
			: base_type(ex, excluded_dirs)
			, m_handle(ex)
		{
			this->open(dir);
		}

		explicit windows_watch_service(const Executor& ex,
			const std::vector<fs::path>& excluded_dirs = {})
			: base_type(ex, excluded_dirs)
			, m_handle(ex)
		{}

		~windows_watch_service()
		{
			boost::system::error_code ignore_ec;
			this->close(ignore_ec);
		}

		windows_watch_service(windows_watch_service&&) = default;
		windows_watch_service& operator=(windows_watch_service&&) = default;

		// 底层的目录句柄。
		HANDLE native_handle() const noexcept { return m_handle.native_handle(); }

	private:
		// ---------- watch_service_base 要求的实现 ----------

		void open_impl(const fs::path& dir, boost::system::error_code& ec)
		{
			boost::system::error_code ignore_ec;
			close_impl(ignore_ec);

			const HANDLE handle = ::CreateFileW(dir.wstring().c_str(),
				FILE_LIST_DIRECTORY,
				FILE_SHARE_READ | FILE_SHARE_WRITE,
				nullptr,
				OPEN_EXISTING,
				FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
				nullptr);

			if (handle == INVALID_HANDLE_VALUE)
			{
				ec.assign(static_cast<int>(::GetLastError()),
					boost::system::system_category());

				return;
			}

			m_handle.assign(handle, ec);
		}

		void close_impl(boost::system::error_code& ec)
		{
			m_handle.close(ec);
		}

		void cancel_impl(boost::system::error_code& ec)
		{
			if (!m_handle.is_open())
			{
				ec = net::error::bad_descriptor;
				return;
			}

			// nullptr 表示取消该句柄上所有未完成的请求。
			if (!::CancelIoEx(m_handle.native_handle(), nullptr))
			{
				const DWORD last_error = ::GetLastError();

				if (last_error != ERROR_NOT_FOUND)
				{
					ec.assign(static_cast<int>(last_error),
						boost::system::system_category());
				}
			}
		}

		bool is_open_impl() const noexcept
		{
			return m_handle.is_open();
		}

		template <typename Handler>
		void async_wait_impl(Handler&& handler)
		{
			using handler_type = std::decay_t<Handler>;

			// 处理函数随后会被移动，取消槽需要提前取出。
			const auto slot = net::get_associated_cancellation_slot(handler);

			read_state state;
			state.service_ = this;
			state.bufs_.reset(new uint8_t[buffer_size]);

			auto* buffer = state.bufs_.get();

			net::windows::overlapped_ptr op(m_handle.get_executor(),
				detail::state_handler<handler_type, read_state>(
					std::forward<Handler>(handler), std::move(state),
					this->get_executor()));

			assign_op_cancellation(slot, op.get());

			DWORD transferred = 0;
			const BOOL ok = ::ReadDirectoryChangesW(m_handle.native_handle(),
				buffer, buffer_size, TRUE, event_filter, &transferred,
				op.get(), nullptr);

			if (ok)
			{
				op.release();
				return;
			}

			const DWORD last_error = ::GetLastError();

			if (last_error == ERROR_IO_PENDING || last_error == ERROR_MORE_DATA)
			{
				op.release();
				return;
			}

			op.complete(boost::system::error_code(static_cast<int>(last_error),
				boost::system::system_category()), transferred);
		}

		// 取消通过 CancelIoEx 完成，需要绑定本次操作的 OVERLAPPED。
		void assign_op_cancellation(net::cancellation_slot slot, OVERLAPPED* op)
		{
			if (!slot.is_connected())
				return;

			const HANDLE handle = m_handle.native_handle();

			this->assign_cancellation(slot, [handle, op]
				{
					::CancelIoEx(handle, op);
				});
		}

		// ---------- 事件转换 ----------

		inline constexpr event_type notify_type(DWORD action) const
		{
			switch (action)
			{
			case FILE_ACTION_ADDED:
				return event_type::creation;
			case FILE_ACTION_REMOVED:
				return event_type::deletion;
			case FILE_ACTION_MODIFIED:
				return event_type::modification;
			case FILE_ACTION_RENAMED_OLD_NAME:
			case FILE_ACTION_RENAMED_NEW_NAME:
				return event_type::rename;
			default:
				return event_type::unknown;
			}
		}

		inline void convert_result(uint8_t* data, notify_events& result) const
		{
			const auto* item =
				reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(data);

			while (item != nullptr)
			{
				const auto* next = next_entry(*item);

				if (is_rename_pair(*item, next))
				{
					append_rename(*item, *next, result);
					item = next_entry(*next);
					continue;
				}

				append_event(*item, result);
				item = next;
			}
		}

		// 重命名会被拆成相邻的两条记录。
		static bool is_rename_pair(const FILE_NOTIFY_INFORMATION& item,
			const FILE_NOTIFY_INFORMATION* next) noexcept
		{
			return item.Action == FILE_ACTION_RENAMED_OLD_NAME &&
				next != nullptr &&
				next->Action == FILE_ACTION_RENAMED_NEW_NAME;
		}

		static const FILE_NOTIFY_INFORMATION* next_entry(
			const FILE_NOTIFY_INFORMATION& item) noexcept
		{
			if (item.NextEntryOffset == 0)
				return nullptr;

			return reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(
				reinterpret_cast<const uint8_t*>(&item) + item.NextEntryOffset);
		}

		inline void append_event(const FILE_NOTIFY_INFORMATION& item,
			notify_events& result) const
		{
			notify_event event;
			event.type_ = notify_type(item.Action);
			event.path_ = entry_path(item);

			append(std::move(event), result);
		}

		inline void append_rename(const FILE_NOTIFY_INFORMATION& from,
			const FILE_NOTIFY_INFORMATION& to, notify_events& result) const
		{
			notify_event event;
			event.type_ = event_type::rename;
			event.path_ = entry_path(from);
			event.new_path_ = entry_path(to);

			append(std::move(event), result);
		}

		// 跳过被排除目录中的事件。
		inline void append(notify_event event, notify_events& result) const
		{
			if (event.path_.empty() || this->is_excluded(event.path_))
				return;

			result.push_back(std::move(event));
		}

		inline fs::path entry_path(const FILE_NOTIFY_INFORMATION& item) const
		{
			const std::wstring_view name{ item.FileName,
				item.FileNameLength / sizeof(wchar_t) };

			return this->watch_dir() / name;
		}

	private:
		handle_type m_handle;
	};

	using windows_watch = windows_watch_service<>;
} // namespace watchman
