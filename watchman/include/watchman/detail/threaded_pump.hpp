//
// threaded_pump.hpp
// ~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2023 Jack (jack.arain at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
//

#pragma once

#include "watchman/detail/wait_queue.hpp"
#include "watchman/notify_event.hpp"

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/associated_cancellation_slot.hpp>
#include <boost/asio/error.hpp>
#include <boost/system/error_code.hpp>

#include <condition_variable>
#include <mutex>
#include <thread>
#include <utility>

namespace watchman {
	namespace detail {

		// 阻塞式事件源：由平台实现向内核取事件，例如 kqueue 或 event ports。
		class event_source
		{
		public:
			virtual ~event_source() = default;

			// 阻塞等待一批事件；收到 interrupt() 时返回 operation_aborted。
			virtual boost::system::error_code wait(notify_events& events) = 0;

			// 打断正在进行的 wait()，可从任意线程调用。
			virtual void interrupt() noexcept = 0;
		};

		// 把阻塞式事件源接入 asio 的异步模型：后台线程只在有等待动作时
		// 查询内核，每批事件完成一个等待，完成动作投递到处理函数的关联
		// 执行器上。
		class threaded_pump
		{
		public:
			using wait_id = wait_queue::wait_id;

			threaded_pump(event_source& source, net::any_io_executor fallback)
				: m_source(source)
				, m_queue(std::move(fallback))
			{}

			~threaded_pump()
			{
				stop(boost::system::error_code{});
			}

			threaded_pump(const threaded_pump&) = delete;
			threaded_pump& operator=(const threaded_pump&) = delete;

			// 启动工作线程，重复调用无效。
			void start()
			{
				std::lock_guard<std::mutex> lock(m_mtx);

				if (m_thread.joinable())
					return;

				m_stop = false;
				m_thread = std::thread([this] { run(); });
			}

			// 结束工作线程，未完成的等待以 ec 完成。
			void stop(boost::system::error_code ec)
			{
				{
					std::lock_guard<std::mutex> lock(m_mtx);
					m_stop = true;
				}

				m_source.interrupt();
				m_cv.notify_all();

				if (m_thread.joinable())
					m_thread.join();

				std::lock_guard<std::mutex> lock(m_mtx);
				m_queue.abort_all(ec);
				m_queue.clear();
			}

			template <typename Handler>
			void async_wait(Handler&& handler)
			{
				const auto slot = net::get_associated_cancellation_slot(handler);
				const auto alive = m_alive.get();

				std::lock_guard<std::mutex> lock(m_mtx);

				const wait_id id = m_queue.push(std::forward<Handler>(handler));

				// 队列里已有缓存的批次，等待已经完成，不需要再取消。
				if (id == wait_queue::invalid_id)
					return;

				detail::assign_cancellation(slot, [this, alive, id]
					{
						if (alive->load(std::memory_order_relaxed))
							cancel(id);
					});

				m_cv.notify_all();
			}

			// 中止全部等待（服务级取消）。
			void cancel_all()
			{
				{
					std::lock_guard<std::mutex> lock(m_mtx);
					m_queue.abort_all(net::error::operation_aborted);
				}

				m_source.interrupt();
			}

		private:
			void cancel(wait_id id)
			{
				bool empty = false;

				{
					std::lock_guard<std::mutex> lock(m_mtx);
					m_queue.abort(id, net::error::operation_aborted);
					empty = m_queue.empty();
				}

				// 没有等待动作时，让工作线程从阻塞等待中返回。
				if (empty)
					m_source.interrupt();
			}

			void run()
			{
				std::unique_lock<std::mutex> lock(m_mtx);

				for (;;)
				{
					m_cv.wait(lock, [this]
						{
							return m_stop || !m_queue.empty();
						});

					if (m_stop)
						return;

					lock.unlock();
					notify_events events;
					const boost::system::error_code ec = m_source.wait(events);
					lock.lock();

					if (m_stop)
						return;

					if (ec == net::error::operation_aborted)
						continue;

					if (ec)
					{
						m_queue.abort_all(ec);
						continue;
					}

					m_queue.deliver(std::move(events));
				}
			}

		private:
			event_source& m_source;
			wait_queue m_queue;
			alive_token m_alive;

			std::mutex m_mtx;
			std::condition_variable m_cv;
			std::thread m_thread;
			bool m_stop = false;
		};
	} // namespace detail
} // namespace watchman
