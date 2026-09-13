//
// wait_queue.hpp
// ~~~~~~~~~~~~~~
//
// Copyright (c) 2023 Jack (jack.arain at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
//

#pragma once

#include "watchman/notify_event.hpp"

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/associated_allocator.hpp>
#include <boost/asio/associated_cancellation_slot.hpp>
#include <boost/asio/associated_executor.hpp>
#include <boost/asio/bind_allocator.hpp>
#include <boost/asio/cancellation_type.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/post.hpp>
#include <boost/system/error_code.hpp>

#include <algorithm>
#include <atomic>
#include <deque>
#include <memory>
#include <type_traits>
#include <utility>

#include <cstddef>
#include <cstdint>

namespace watchman {
	namespace net = boost::asio;

	namespace detail {

		// 本实现支持的取消类型。
		inline constexpr net::cancellation_type supported_cancellation_types =
			net::cancellation_type::terminal |
			net::cancellation_type::total;

		// 把取消槽接到中止动作上，只响应受支持的取消类型。
		template <typename Cancel>
		inline void assign_cancellation(net::cancellation_slot slot, Cancel cancel)
		{
			if (!slot.is_connected())
				return;

			slot.assign([cancel = std::move(cancel)](
				net::cancellation_type type) mutable
				{
					if (net::cancellation_type::none ==
						(type & supported_cancellation_types))
						return;

					cancel();
				});
		}

		// 取消通知可能在等待对象销毁之后到达，用共享的存活标记兜住这种时序。
		class alive_token
		{
		public:
			alive_token() = default;

			alive_token(const alive_token&) = delete;
			alive_token& operator=(const alive_token&) = delete;

			~alive_token()
			{
				m_alive->store(false, std::memory_order_relaxed);
			}

			std::shared_ptr<const std::atomic<bool>> get() const noexcept
			{
				return m_alive;
			}

		private:
			std::shared_ptr<std::atomic<bool>> m_alive =
				std::make_shared<std::atomic<bool>>(true);
		};

		// 等待动作队列：事件批次按先后顺序交给排队的等待，没有等待时先缓存
		// 批次，等下一次等待动作取走，避免事件在内核之外丢失。
		//
		// 队列本身不加锁，调用方需要保证串行访问；完成动作统一投递到处理
		// 函数的关联执行器上，因此可以在任意线程调用 deliver/abort。
		class wait_queue
		{
		public:
			using wait_id = std::uint64_t;
			static constexpr wait_id invalid_id = 0;

			explicit wait_queue(net::any_io_executor fallback,
				std::size_t max_buffered_batches = 64)
				: m_fallback(std::move(fallback))
				, m_max_buffered(std::max<std::size_t>(1, max_buffered_batches))
			{}

			wait_queue(wait_queue&&) = delete;
			wait_queue& operator=(wait_queue&&) = delete;

			// 发起一次等待；已有缓存批次时立即完成并返回 invalid_id。
			template <typename Handler>
			wait_id push(Handler&& handler)
			{
				if (!m_buffered.empty())
				{
					notify_events events = std::move(m_buffered.front());
					m_buffered.pop_front();

					complete(std::forward<Handler>(handler),
						boost::system::error_code{}, std::move(events));

					return invalid_id;
				}

				const wait_id id = ++m_next_id;

				m_ops.push_back(
					entry{ id, make_op(std::forward<Handler>(handler)) });

				return id;
			}

			// 交出一个事件批次：有等待时完成最早的等待，否则缓存批次。
			void deliver(notify_events events)
			{
				if (events.empty())
					return;

				if (m_ops.empty())
				{
					buffer(std::move(events));
					return;
				}

				entry item = std::move(m_ops.front());
				m_ops.pop_front();

				item.complete(boost::system::error_code{}, std::move(events));
			}

			// 中止指定等待，已经完成的等待会被忽略。
			void abort(wait_id id, boost::system::error_code ec)
			{
				for (auto it = m_ops.begin(); it != m_ops.end(); ++it)
				{
					if (it->id_ != id)
						continue;

					entry item = std::move(*it);
					m_ops.erase(it);

					item.complete(ec, notify_events{});

					return;
				}
			}

			// 中止全部等待。
			void abort_all(boost::system::error_code ec)
			{
				std::deque<entry> pending;
				pending.swap(m_ops);

				for (auto& item : pending)
					item.complete(ec, notify_events{});
			}

			// 丢弃缓存的事件批次（例如监视已被关闭）。
			void clear() noexcept
			{
				m_buffered.clear();
			}

			bool empty() const noexcept { return m_ops.empty(); }
			std::size_t size() const noexcept { return m_ops.size(); }

		private:
			struct op
			{
				virtual ~op() = default;

				virtual void complete(boost::system::error_code ec,
					notify_events events) = 0;
			};

			template <typename Handler>
			class op_impl : public op
			{
			public:
				op_impl(net::any_io_executor executor, Handler handler)
					: m_executor(std::move(executor))
					, m_handler(std::move(handler))
				{}

				void complete(boost::system::error_code ec,
					notify_events events) override
				{
					const auto allocator =
						net::get_associated_allocator(m_handler);

					net::post(m_executor,
						net::bind_allocator(allocator,
							[handler = std::move(m_handler), ec,
								events = std::move(events)]() mutable
							{
								std::move(handler)(ec, std::move(events));
							}));
				}

			private:
				net::any_io_executor m_executor;
				Handler m_handler;
			};

			struct entry
			{
				wait_id id_;
				std::unique_ptr<op> op_;

				void complete(boost::system::error_code ec, notify_events events)
				{
					op_->complete(ec, std::move(events));
				}
			};

			template <typename Handler>
			std::unique_ptr<op> make_op(Handler&& handler)
			{
				net::any_io_executor executor =
					net::get_associated_executor(handler, m_fallback);

				return std::make_unique<op_impl<std::decay_t<Handler>>>(
					std::move(executor), std::forward<Handler>(handler));
			}

			template <typename Handler>
			void complete(Handler&& handler, boost::system::error_code ec,
				notify_events events)
			{
				make_op(std::forward<Handler>(handler))
					->complete(ec, std::move(events));
			}

			void buffer(notify_events events)
			{
				while (m_buffered.size() >= m_max_buffered)
					m_buffered.pop_front();

				m_buffered.push_back(std::move(events));
			}

		private:
			net::any_io_executor m_fallback;
			std::size_t m_max_buffered;
			wait_id m_next_id = invalid_id;
			std::deque<entry> m_ops;
			std::deque<notify_events> m_buffered;
		};
	} // namespace detail
} // namespace watchman
