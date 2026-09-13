//
// macos_watchman.hpp
// ~~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2023 Jack (jack.arain at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
//

#pragma once

#include "watchman/detail/fsevents_events.hpp"
#include "watchman/detail/path_remap.hpp"
#include "watchman/detail/wait_queue.hpp"
#include "watchman/detail/watch_service_base.hpp"
#include "watchman/notify_event.hpp"

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/associated_cancellation_slot.hpp>
#include <boost/asio/error.hpp>
#include <boost/filesystem.hpp>
#include <boost/system/error_code.hpp>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <CoreServices/CoreServices.h>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <dispatch/dispatch.h>

namespace watchman {
	namespace net = boost::asio;
	namespace fs = boost::filesystem;

	// macOS 使用 FSEvents 监视目录树。
	//
	// FSEvents 在自己的 dispatch 队列上推送事件，事件的先后顺序由系统的
	// 事件编号保证；每次等待从等待队列里取一个事件批次，没有等待时到达
	// 的事件先缓存下来。
	//
	// FSEvents 不跟随符号链接，上报的也是解析过符号链接的真实路径，因此
	// 建流时用真实路径，事件路径再换回注册时的路径形式。
	//
	// FSEvents 会把短时间内同一路径上的多次变化合并上报，合并后的事件仍然
	// 带着创建标志，因此只在路径第一次出现时报告创建。
	template <typename Executor = net::any_io_executor>
	class macos_watch_service
		: public detail::watch_service_base<macos_watch_service<Executor>, Executor>
	{
	private:
		using base_type =
			detail::watch_service_base<macos_watch_service<Executor>, Executor>;

		friend base_type;

		macos_watch_service(const macos_watch_service&) = delete;
		macos_watch_service& operator=(const macos_watch_service&) = delete;

	public:
		template <typename Executor1>
		struct rebind
		{
			using other = macos_watch_service<Executor1>;
		};

		macos_watch_service(const Executor& ex, const fs::path& dir,
			const std::vector<fs::path>& excluded_dirs = {})
			: base_type(ex, excluded_dirs)
			, m_queue(ex)
		{
			this->open(dir);
		}

		explicit macos_watch_service(const Executor& ex,
			const std::vector<fs::path>& excluded_dirs = {})
			: base_type(ex, excluded_dirs)
			, m_queue(ex)
		{}

		~macos_watch_service()
		{
			boost::system::error_code ignore_ec;
			this->close(ignore_ec);
		}

		// FSEvents 的回调持有本对象的指针，对象不可拷贝、不可移动。
		macos_watch_service(macos_watch_service&&) = delete;
		macos_watch_service& operator=(macos_watch_service&&) = delete;

	private:
		// ---------- watch_service_base 要求的实现 ----------

		void open_impl(const fs::path& dir, boost::system::error_code& ec)
		{
			boost::system::error_code ignore_ec;
			close_impl(ignore_ec);

			m_stream_dir = resolve_dir(dir);
			m_stream = create_stream(m_stream_dir, ec);

			if (m_stream == nullptr)
				return;

			m_fsevents_queue =
				dispatch_queue_create("watchman_fsevents", nullptr);

			FSEventStreamSetDispatchQueue(m_stream, m_fsevents_queue);

			if (FSEventStreamStart(m_stream))
				return;

			ec.assign(EIO, boost::system::generic_category());
			close_impl(ignore_ec);
		}

		void close_impl(boost::system::error_code& /*ec*/)
		{
			stop_stream();

			// 关闭之后不再有等待可以完成。
			std::lock_guard<std::mutex> lock(m_mtx);
			m_queue.abort_all(net::error::operation_aborted);
			m_queue.clear();
			m_filter.clear();
		}

		void cancel_impl(boost::system::error_code& ec)
		{
			ec.clear();

			std::lock_guard<std::mutex> lock(m_mtx);
			m_queue.abort_all(net::error::operation_aborted);
		}

		bool is_open_impl() const noexcept
		{
			return m_stream != nullptr;
		}

		template <typename Handler>
		void async_wait_impl(Handler&& handler)
		{
			const auto slot = net::get_associated_cancellation_slot(handler);
			const auto alive = m_alive.get();

			std::lock_guard<std::mutex> lock(m_mtx);

			const auto id = m_queue.push(std::forward<Handler>(handler));

			// 队列里已有缓存的批次，等待已经完成，不需要再取消。
			if (id == detail::wait_queue::invalid_id)
				return;

			detail::assign_cancellation(slot, [this, alive, id]
				{
					if (!alive->load(std::memory_order_relaxed))
						return;

					std::lock_guard<std::mutex> lock(m_mtx);
					m_queue.abort(id, net::error::operation_aborted);
				});
		}

		// ---------- FSEvents ----------

		// FSEvents 的合并窗口，取小值让事件尽快上报。
		static constexpr CFTimeInterval stream_latency = 0.01;

		FSEventStreamRef create_stream(const fs::path& dir,
			boost::system::error_code& ec)
		{
			CFStringRef dir_ref = CFStringCreateWithCString(
				nullptr, dir.c_str(), kCFStringEncodingUTF8);

			if (dir_ref == nullptr)
			{
				ec.assign(errno, boost::system::generic_category());
				return nullptr;
			}

			CFArrayRef paths = CFArrayCreate(nullptr,
				reinterpret_cast<const void**>(&dir_ref), 1,
				&kCFTypeArrayCallBacks);

			CFRelease(dir_ref);

			if (paths == nullptr)
			{
				ec.assign(errno, boost::system::generic_category());
				return nullptr;
			}

			m_stream_ctx.version = 0;
			m_stream_ctx.info = this;
			m_stream_ctx.retain = nullptr;
			m_stream_ctx.release = nullptr;
			m_stream_ctx.copyDescription = nullptr;

			const FSEventStreamCreateFlags flags =
				kFSEventStreamCreateFlagFileEvents |
				kFSEventStreamCreateFlagNoDefer |
				kFSEventStreamCreateFlagUseExtendedData |
				kFSEventStreamCreateFlagUseCFTypes;

			FSEventStreamRef stream = FSEventStreamCreate(nullptr,
				&macos_watch_service<Executor>::fsevents_callback,
				&m_stream_ctx,
				paths,
				kFSEventStreamEventIdSinceNow,
				stream_latency,
				flags);

			CFRelease(paths);

			if (stream == nullptr)
				ec.assign(errno, boost::system::generic_category());

			return stream;
		}

		void stop_stream() noexcept
		{
			if (m_stream != nullptr)
			{
				FSEventStreamStop(m_stream);
				FSEventStreamInvalidate(m_stream);
				FSEventStreamRelease(m_stream);
				m_stream = nullptr;
			}

			if (m_fsevents_queue != nullptr)
			{
				dispatch_release(m_fsevents_queue);
				m_fsevents_queue = nullptr;
			}
		}

		static void fsevents_callback(ConstFSEventStreamRef /*stream*/,
			void* client_info,
			size_t num_events,
			void* event_paths,
			const FSEventStreamEventFlags event_flags[],
			const FSEventStreamEventId /*event_ids*/[])
		{
			auto* self = static_cast<macos_watch_service<Executor>*>(client_info);

			std::lock_guard<std::mutex> lock(self->m_mtx);

			self->m_queue.deliver(
				self->convert_events(event_paths, event_flags, num_events));
		}

		// 把一批 FSEvents 回调参数转换成统一的通告事件。
		notify_events convert_events(void* event_paths,
			const FSEventStreamEventFlags event_flags[], size_t num_events)
		{
			notify_events batch;

			CFArrayRef event_array = static_cast<CFArrayRef>(event_paths);

			for (size_t i = 0; i < num_events; ++i)
			{
				std::string reported;

				if (!extract_path(event_array, i, reported))
					continue;

				fs::path path;

				if (!to_entry_path(reported, path))
					continue;

				m_filter.add_event(batch, to_event_info(event_flags[i]), path,
					fs::exists(path));
			}

			m_filter.flush_renames(batch);

			return batch;
		}

		// 把 FSEvents 的标志换算成与平台无关的事件信息。
		static detail::fsevents_event to_event_info(
			FSEventStreamEventFlags flags)
		{
			detail::fsevents_event event;

			event.created =
				(flags & kFSEventStreamEventFlagItemCreated) != 0;
			event.removed =
				(flags & kFSEventStreamEventFlagItemRemoved) != 0;
			event.renamed =
				(flags & kFSEventStreamEventFlagItemRenamed) != 0;
			event.modified = (flags & modification_flags) != 0;

			return event;
		}

		// 取监视目录的真实路径，失败时退回原路径。
		static fs::path resolve_dir(const fs::path& dir)
		{
			boost::system::error_code ec;
			const fs::path real = fs::canonical(dir, ec);

			return ec ? dir : real;
		}

		// 把 FSEvents 报出的真实路径换回注册时的路径形式，返回 false 表示
		// 事件与监视目录下的条目无关。FSEvents 一般上报解析过符号链接的
		// 真实路径，这里先按建流路径换算，再兼容未解析的形式。
		bool to_entry_path(const std::string& reported, fs::path& path) const
		{
			const fs::path full(reported);
			const fs::path& dir = this->watch_dir();

			if (!detail::remap_under(m_stream_dir, dir, full, path)
				&& !detail::remap_under(dir, dir, full, path))
				return false;

			// 目录自身的变化不产生条目事件。
			if (path == dir)
				return false;

			return !this->is_excluded(path);
		}

		// 从扩展数据字典里取出事件路径；取不到时返回 false。
		static bool extract_path(CFArrayRef event_array, size_t index,
			std::string& path)
		{
			auto* dict = static_cast<CFDictionaryRef>(
				CFArrayGetValueAtIndex(event_array,
					static_cast<CFIndex>(index)));

			auto* cf_path = static_cast<CFStringRef>(CFDictionaryGetValue(
				dict, kFSEventStreamEventExtendedDataPathKey));

			if (cf_path == nullptr)
				return false;

			return to_string(cf_path, path);
		}

		// CFStringGetCStringPtr 可能返回空指针，这里统一用 CFStringGetCString。
		static bool to_string(CFStringRef cf_path, std::string& path)
		{
			const CFIndex length = CFStringGetLength(cf_path);
			const CFIndex max_size = CFStringGetMaximumSizeForEncoding(
				length, kCFStringEncodingUTF8) + 1;

			std::string buffer;
			buffer.resize(static_cast<std::string::size_type>(max_size));

			if (!CFStringGetCString(cf_path, buffer.data(), max_size,
					kCFStringEncodingUTF8))
				return false;

			path.assign(buffer.c_str());
			return true;
		}

		// FSEvents 用一组标志表示内容与元数据的变化，这里统一算作修改。
		static constexpr FSEventStreamEventFlags modification_flags =
			kFSEventStreamEventFlagItemModified |
			kFSEventStreamEventFlagItemInodeMetaMod |
			kFSEventStreamEventFlagItemChangeOwner |
			kFSEventStreamEventFlagItemFinderInfoMod |
			kFSEventStreamEventFlagItemXattrMod;

	private:
		FSEventStreamContext m_stream_ctx{};
		FSEventStreamRef m_stream = nullptr;
		dispatch_queue_t m_fsevents_queue = nullptr;

		// 建流时使用的真实路径。
		fs::path m_stream_dir;

		// 事件类型的换算与重命名配对。
		detail::fsevents_event_filter m_filter;

		std::mutex m_mtx;
		detail::wait_queue m_queue;
		detail::alive_token m_alive;
	};

	using macos_watch = macos_watch_service<>;
} // namespace watchman
