//
// watch_service_base.hpp
// ~~~~~~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2023 Jack (jack.arain at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
//

#pragma once

#include <string>
#include <utility>
#include <vector>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/associated_allocator.hpp>
#include <boost/asio/associated_executor.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/async_result.hpp>
#include <boost/asio/associated_allocator.hpp>
#include <boost/asio/associated_cancellation_slot.hpp>
#include <boost/asio/associated_executor.hpp>
#include <boost/asio/cancellation_type.hpp>
#include <boost/asio/bind_allocator.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/post.hpp>
#include <boost/filesystem.hpp>
#include <boost/system/error_code.hpp>
#include <boost/throw_exception.hpp>

#include "watchman/detail/path_exclusion.hpp"
#include "watchman/notify_event.hpp"

namespace watchman {
	namespace net = boost::asio;

	namespace detail {

		namespace fs = boost::filesystem;

		// 把自定义状态（例如读缓冲区）与处理函数绑在一起，同时保留 asio 的
		// 关联特性（执行器、分配器、取消槽），供需要自己持有内核数据的平台
		// 实现使用。State 需要提供：
		//   template <typename Handler, typename... Args>
		//   void complete(Handler handler, Args&&... args);
		template <typename Handler, typename State,
			typename FallbackExecutor = net::any_io_executor>
		class state_handler
		{
		public:
			template <typename HandlerArg, typename StateArg>
			state_handler(HandlerArg&& handler, StateArg&& state,
				const FallbackExecutor& fallback)
				: m_handler(std::forward<HandlerArg>(handler))
				, m_state(std::forward<StateArg>(state))
				, m_fallback(fallback)
			{}

			using executor_type =
				net::associated_executor_t<Handler, FallbackExecutor>;
			using allocator_type = net::associated_allocator_t<Handler>;
			using cancellation_slot_type =
				net::associated_cancellation_slot_t<Handler>;

			executor_type get_executor() const noexcept
			{
				return net::get_associated_executor(m_handler, m_fallback);
			}

			allocator_type get_allocator() const noexcept
			{
				return net::get_associated_allocator(m_handler);
			}

			cancellation_slot_type get_cancellation_slot() const noexcept
			{
				return net::get_associated_cancellation_slot(m_handler);
			}

			template <typename... Args>
			void operator()(Args&&... args)
			{
				m_state.complete(std::move(m_handler),
					std::forward<Args>(args)...);
			}

		private:
			Handler m_handler;
			State m_state;
			FallbackExecutor m_fallback;
		};

		// 各平台监视实现的公共外观，按 asio 的 I/O 对象约定统一接口：
		//
		//   * async_wait 通过 async_initiate 发起，支持任意 completion token；
		//   * 处理函数在它自己的关联执行器上被调用，错误码使用 asio 的取值
		//     （取消与关闭走 operation_aborted，重复关闭走 bad_descriptor）；
		//   * 取消通过 handler 的关联取消槽生效，本实现支持 terminal 与
		//     total 两种取消类型；
		//   * 允许同时存在多个未完成的等待，每个等待自带状态。
		//
		// 派生类需要实现：
		//   void open_impl(const fs::path& dir, boost::system::error_code& ec);
		//   void close_impl(boost::system::error_code& ec);
		//   void cancel_impl(boost::system::error_code& ec);
		//   bool is_open_impl() const noexcept;
		//   template <typename Handler> void async_wait_impl(Handler&& handler);
		//
		// 派生类需要在自己的析构函数中关闭监视，基类不做这件事。
		template <typename Derived, typename Executor = net::any_io_executor>
		class watch_service_base
		{
		private:
			watch_service_base(const watch_service_base&) = delete;
			watch_service_base& operator=(const watch_service_base&) = delete;

		protected:
			explicit watch_service_base(const Executor& ex,
				std::vector<fs::path> excluded_dirs = {})
				: m_executor(ex)
				, m_excluded_dirs(std::move(excluded_dirs))
			{}

			// 未完成的等待由派生的内核对象持有，移动前必须先结束这些等待。
			watch_service_base(watch_service_base&& other) noexcept
				: m_executor(std::move(other.m_executor))
				, m_watch_dir(std::move(other.m_watch_dir))
				, m_excluded_dirs(std::move(other.m_excluded_dirs))
			{}

			watch_service_base& operator=(watch_service_base&& other) noexcept
			{
				if (this != &other)
				{
					m_executor = std::move(other.m_executor);
					m_watch_dir = std::move(other.m_watch_dir);
					m_excluded_dirs = std::move(other.m_excluded_dirs);
				}
				return *this;
			}

			~watch_service_base() = default;

		public:
			using executor_type = Executor;

			executor_type get_executor() const noexcept { return m_executor; }

			// 本实现支持的取消类型。
			static constexpr net::cancellation_type supported_cancellation =
				net::cancellation_type::terminal |
				net::cancellation_type::total;

			void open(const fs::path& dir, boost::system::error_code& ec)
			{
				m_watch_dir = dir;
				derived().open_impl(dir, ec);
			}

			void open(const fs::path& dir)
			{
				boost::system::error_code ec;
				open(dir, ec);
				throw_on_error(ec);
			}

			void close(boost::system::error_code& ec)
			{
				ec.clear();
				derived().close_impl(ec);
			}

			void close()
			{
				boost::system::error_code ec;
				close(ec);
				throw_on_error(ec);
			}

			void cancel(boost::system::error_code& ec)
			{
				ec.clear();
				derived().cancel_impl(ec);
			}

			void cancel()
			{
				boost::system::error_code ec;
				cancel(ec);
				throw_on_error(ec);
			}

			// 等待下一次事件，完成处理函数的签名为
			// void(boost::system::error_code, notify_events)。
			template <typename Handler>
			auto async_wait(Handler&& handler)
			{
				return net::async_initiate<Handler,
					void(boost::system::error_code, notify_events)>
					([this](auto&& token) mutable
						{
							using HandlerType =
								std::decay_t<decltype(token)>;

							derived().async_wait_impl(
								std::forward<HandlerType>(token));
						}, handler);
			}

			bool is_open() const noexcept { return derived().is_open_impl(); }

			const fs::path& watch_dir() const noexcept { return m_watch_dir; }

			const std::vector<fs::path>& excluded_dirs() const noexcept
			{
				return m_excluded_dirs;
			}

			bool is_excluded(const fs::path& path) const
			{
				return detail::is_excluded(m_excluded_dirs, path);
			}

			static void throw_on_error(const boost::system::error_code& ec,
				boost::source_location const& loc = BOOST_CURRENT_LOCATION)
			{
				if (ec)
					boost::throw_exception(boost::system::system_error{ ec }, loc);
			}

		protected:
			// 把取消槽接到中止动作上，只响应本实现支持的取消类型。
			template <typename Cancel>
			static void assign_cancellation(net::cancellation_slot slot,
				Cancel cancel)
			{
				if (!slot.is_connected())
					return;

				slot.assign([cancel = std::move(cancel)](
					net::cancellation_type type) mutable
					{
						if (net::cancellation_type::none ==
							(type & supported_cancellation))
							return;

						cancel();
					});
			}

			// 在后台线程上完成处理函数：按 asio 约定投递到处理函数的关联
			// 执行器，并沿用它的关联分配器。
			template <typename Handler>
			void post_completion(Handler&& handler,
				boost::system::error_code ec, notify_events events)
			{
				auto executor = net::get_associated_executor(handler, m_executor);
				auto allocator = net::get_associated_allocator(handler);

				net::post(executor,
					net::bind_allocator(allocator,
						[handler = std::forward<Handler>(handler), ec,
							events = std::move(events)]() mutable
						{
							std::move(handler)(ec, std::move(events));
						}));
			}

			// 在别的操作内部完成处理函数时使用：投递到它的关联执行器。
			template <typename Handler>
			void dispatch_completion(Handler&& handler,
				boost::system::error_code ec, notify_events events)
			{
				auto executor = net::get_associated_executor(handler, m_executor);
				auto allocator = net::get_associated_allocator(handler);

				net::dispatch(executor,
					net::bind_allocator(allocator,
						[handler = std::forward<Handler>(handler), ec,
							events = std::move(events)]() mutable
						{
							std::move(handler)(ec, std::move(events));
						}));
			}

			Executor m_executor;

		private:
			Derived& derived() noexcept
			{
				return static_cast<Derived&>(*this);
			}

			const Derived& derived() const noexcept
			{
				return static_cast<const Derived&>(*this);
			}

		protected:
			fs::path m_watch_dir;
			std::vector<fs::path> m_excluded_dirs;
		};
	} // namespace detail
} // namespace watchman
