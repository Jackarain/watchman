//
// wait_queue_test.cpp
// ~~~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2023 Jack (jack.arain at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
//

#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/system/error_code.hpp>

#include <watchman/detail/threaded_pump.hpp>
#include <watchman/detail/wait_queue.hpp>

#include "test_util.hpp"

namespace {

	namespace net = boost::asio;

	using watchman::event_type;
	using watchman::notify_event;
	using watchman::notify_events;

	using namespace std::chrono_literals;

	notify_events make_batch(const std::string& name)
	{
		notify_events batch;
		batch.push_back(notify_event{ event_type::creation, name, {} });

		return batch;
	}

	std::string batch_name(const notify_events& batch)
	{
		return batch.empty() ? std::string{} : batch.front().path_.string();
	}

	// 反复取出 io_context 上就绪的处理函数，直到条件成立。
	bool pump_until(net::io_context& io, const std::function<bool()>& pred,
		std::chrono::milliseconds timeout = 5000ms)
	{
		const auto deadline = std::chrono::steady_clock::now() + timeout;

		while (std::chrono::steady_clock::now() < deadline)
		{
			io.restart();
			io.poll();

			if (pred())
				return true;

			std::this_thread::sleep_for(5ms);
		}

		return pred();
	}

	// 每个等待按先后顺序取走一个事件批次。
	void test_deliver_order()
	{
		net::io_context io;
		watchman::detail::wait_queue queue(io.get_executor());

		std::vector<std::string> got;
		const auto collect = [&got](boost::system::error_code ec,
			notify_events events)
		{
			WATCHMAN_CHECK(!ec);
			got.push_back(batch_name(events));
		};

		queue.push(collect);
		queue.push(collect);

		WATCHMAN_CHECK(queue.size() == 2);

		queue.deliver(make_batch("first"));
		queue.deliver(make_batch("second"));

		WATCHMAN_CHECK(queue.empty());

		io.run();

		WATCHMAN_CHECK(got.size() == 2);
		WATCHMAN_CHECK(got.size() == 2 && got[0] == "first" && got[1] == "second");
	}

	// 没有等待时到达的批次先缓存，下一个等待直接取走。
	void test_buffered_events()
	{
		net::io_context io;
		watchman::detail::wait_queue queue(io.get_executor());

		queue.deliver(make_batch("cached"));

		std::string got;
		const auto id = queue.push([&got](boost::system::error_code ec,
			notify_events events)
			{
				WATCHMAN_CHECK(!ec);
				got = batch_name(events);
			});

		WATCHMAN_CHECK(id == watchman::detail::wait_queue::invalid_id);
		WATCHMAN_CHECK(queue.empty());

		io.run();
		WATCHMAN_CHECK(got == "cached");
	}

	// 缓存批次超过上限时丢弃最早的一批，避免无限增长。
	void test_buffer_limit()
	{
		net::io_context io;
		watchman::detail::wait_queue queue(io.get_executor(), 2);

		queue.deliver(make_batch("one"));
		queue.deliver(make_batch("two"));
		queue.deliver(make_batch("three"));

		std::vector<std::string> got;
		const auto collect = [&got](boost::system::error_code, notify_events events)
		{
			got.push_back(batch_name(events));
		};

		queue.push(collect);
		queue.push(collect);
		queue.push(collect);

		io.run();

		WATCHMAN_CHECK(got.size() == 2);
		WATCHMAN_CHECK(got.size() == 2 && got[0] == "two" && got[1] == "three");
	}

	// 取消槽生效时以 operation_aborted 完成，且不影响其它等待。
	void test_cancel_by_slot()
	{
		net::io_context io;
		watchman::detail::wait_queue queue(io.get_executor());

		net::cancellation_signal signal;
		boost::system::error_code result;
		bool called = false;

		const auto id = queue.push(net::bind_cancellation_slot(signal.slot(),
			[&](boost::system::error_code ec, notify_events events)
			{
				called = true;
				result = ec;

				WATCHMAN_CHECK(events.empty());
			}));

		watchman::detail::assign_cancellation(signal.slot(), [&queue, id]
			{
				queue.abort(id, net::error::operation_aborted);
			});

		// none 不是受支持的取消类型。
		signal.emit(net::cancellation_type::none);
		WATCHMAN_CHECK(!called);
		WATCHMAN_CHECK(queue.size() == 1);

		signal.emit(net::cancellation_type::terminal);

		WATCHMAN_CHECK(queue.empty());

		io.run();

		WATCHMAN_CHECK(called);
		WATCHMAN_CHECK(result == net::error::operation_aborted);
	}

	// 中止全部等待。
	void test_abort_all()
	{
		net::io_context io;
		watchman::detail::wait_queue queue(io.get_executor());

		int aborted = 0;
		const auto collect = [&aborted](boost::system::error_code ec,
			notify_events events)
		{
			WATCHMAN_CHECK(ec == net::error::operation_aborted);
			WATCHMAN_CHECK(events.empty());

			++aborted;
		};

		queue.push(collect);
		queue.push(collect);

		queue.abort_all(net::error::operation_aborted);

		WATCHMAN_CHECK(queue.empty());

		io.run();
		WATCHMAN_CHECK(aborted == 2);
	}

	// 可手动触发的事件源，用于验证后台事件泵。
	class fake_source : public watchman::detail::event_source
	{
	public:
		void push(notify_events batch)
		{
			{
				std::lock_guard<std::mutex> lock(m_mtx);
				m_batches.push_back(std::move(batch));
			}

			m_cv.notify_all();
		}

		boost::system::error_code wait(notify_events& events) override
		{
			std::unique_lock<std::mutex> lock(m_mtx);

			m_cv.wait(lock, [this]
				{
					return m_interrupted || !m_batches.empty();
				});

			if (m_interrupted)
			{
				m_interrupted = false;
				return net::error::operation_aborted;
			}

			events = std::move(m_batches.front());
			m_batches.pop_front();

			return {};
		}

		void interrupt() noexcept override
		{
			{
				std::lock_guard<std::mutex> lock(m_mtx);
				m_interrupted = true;
			}

			m_cv.notify_all();
		}

	private:
		std::mutex m_mtx;
		std::condition_variable m_cv;
		std::deque<notify_events> m_batches;
		bool m_interrupted = false;
	};

	// 事件源的事件批次按先后顺序完成排队的等待。
	void test_threaded_pump()
	{
		fake_source source;
		net::io_context io;
		watchman::detail::threaded_pump pump(source, io.get_executor());

		pump.start();

		std::vector<std::string> got;
		pump.async_wait([&got](boost::system::error_code ec, notify_events events)
			{
				WATCHMAN_CHECK(!ec);
				got.push_back(batch_name(events));
			});

		source.push(make_batch("one"));
		WATCHMAN_CHECK(pump_until(io, [&got] { return got.size() == 1; }));

		pump.async_wait([&got](boost::system::error_code ec, notify_events events)
			{
				WATCHMAN_CHECK(!ec);
				got.push_back(batch_name(events));
			});

		source.push(make_batch("two"));
		WATCHMAN_CHECK(pump_until(io, [&got] { return got.size() == 2; }));

		WATCHMAN_CHECK(got.size() == 2 && got[0] == "one" && got[1] == "two");

		pump.stop({});
	}

	// 事件泵支持按等待取消，也支持服务级取消。
	void test_threaded_pump_cancel()
	{
		fake_source source;
		net::io_context io;
		watchman::detail::threaded_pump pump(source, io.get_executor());

		pump.start();

		net::cancellation_signal signal;
		boost::system::error_code cancelled_result;
		bool cancelled = false;

		pump.async_wait(net::bind_cancellation_slot(signal.slot(),
			[&](boost::system::error_code ec, notify_events)
			{
				cancelled = true;
				cancelled_result = ec;
			}));

		std::string got;
		pump.async_wait([&got](boost::system::error_code, notify_events events)
			{
				got = batch_name(events);
			});

		signal.emit(net::cancellation_type::all);
		WATCHMAN_CHECK(pump_until(io, [&cancelled] { return cancelled; }));
		WATCHMAN_CHECK(cancelled_result == net::error::operation_aborted);
		WATCHMAN_CHECK(got.empty());

		// 取消一个等待不会影响后一个等待。
		source.push(make_batch("kept"));
		WATCHMAN_CHECK(pump_until(io, [&got] { return !got.empty(); }));
		WATCHMAN_CHECK(got == "kept");

		boost::system::error_code aborted_result;
		bool aborted = false;

		pump.async_wait([&](boost::system::error_code ec, notify_events)
			{
				aborted = true;
				aborted_result = ec;
			});

		pump.cancel_all();
		WATCHMAN_CHECK(pump_until(io, [&aborted] { return aborted; }));
		WATCHMAN_CHECK(aborted_result == net::error::operation_aborted);

		pump.stop(net::error::operation_aborted);
	}

	// 关闭事件泵时未完成的等待以传入的错误码完成。
	void test_threaded_pump_stop()
	{
		fake_source source;
		net::io_context io;
		watchman::detail::threaded_pump pump(source, io.get_executor());

		pump.start();

		boost::system::error_code result;
		bool called = false;

		pump.async_wait([&](boost::system::error_code ec, notify_events)
			{
				called = true;
				result = ec;
			});

		pump.stop(net::error::operation_aborted);

		WATCHMAN_CHECK(pump_until(io, [&called] { return called; }));
		WATCHMAN_CHECK(result == net::error::operation_aborted);

		// 可以重新启动。
		pump.start();

		std::string got;
		pump.async_wait([&got](boost::system::error_code, notify_events events)
			{
				got = batch_name(events);
			});

		source.push(make_batch("restarted"));
		WATCHMAN_CHECK(pump_until(io, [&got] { return !got.empty(); }));
		WATCHMAN_CHECK(got == "restarted");

		pump.stop({});
	}
} // namespace

int main()
{
	test_deliver_order();
	test_buffered_events();
	test_buffer_limit();
	test_cancel_by_slot();
	test_abort_all();
	test_threaded_pump();
	test_threaded_pump_cancel();
	test_threaded_pump_stop();

	return watchman::test::summary("wait_queue");
}
