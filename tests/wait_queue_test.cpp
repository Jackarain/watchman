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

#define BOOST_TEST_MODULE wait_queue

// Boost.Test 在 Windows 上会引入 <windows.h>，而 asio 要求在它之前先引入
// winsock2.h，因此把用到 asio 的头文件放在 Boost.Test 之前。
#include <watchman/detail/threaded_pump.hpp>
#include <watchman/detail/wait_queue.hpp>

#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/system/error_code.hpp>

#include <boost/test/included/unit_test.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

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

	// 轮询等待条件成立。
	bool wait_until(const std::function<bool()>& pred,
		std::chrono::milliseconds timeout = 5000ms)
	{
		const auto deadline = std::chrono::steady_clock::now() + timeout;

		while (std::chrono::steady_clock::now() < deadline)
		{
			if (pred())
				return true;

			std::this_thread::sleep_for(1ms);
		}

		return pred();
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
	BOOST_AUTO_TEST_CASE(deliver_order)
	{
		net::io_context io;
		watchman::detail::wait_queue queue(io.get_executor());

		std::vector<std::string> got;
		const auto collect = [&got](boost::system::error_code ec,
			notify_events events)
		{
			BOOST_TEST(!ec);
			got.push_back(batch_name(events));
		};

		queue.push(collect);
		queue.push(collect);

		BOOST_TEST(queue.size() == 2);

		queue.deliver(make_batch("first"));
		queue.deliver(make_batch("second"));

		BOOST_TEST(queue.empty());

		io.run();

		BOOST_TEST(got.size() == 2);
		BOOST_CHECK(got.size() == 2 && got[0] == "first" && got[1] == "second");
	}

	// 没有等待时到达的批次先缓存，下一个等待直接取走。
	BOOST_AUTO_TEST_CASE(buffered_events)
	{
		net::io_context io;
		watchman::detail::wait_queue queue(io.get_executor());

		queue.deliver(make_batch("cached"));

		std::string got;
		const auto id = queue.push([&got](boost::system::error_code ec,
			notify_events events)
			{
				BOOST_TEST(!ec);
				got = batch_name(events);
			});

		BOOST_TEST(id == watchman::detail::wait_queue::invalid_id);
		BOOST_TEST(queue.empty());

		io.run();
		BOOST_TEST(got == "cached");
	}

	// 缓存批次超过上限时丢弃最早的一批，避免无限增长。
	BOOST_AUTO_TEST_CASE(buffer_limit)
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

		int pending_aborted = 0;
		const auto pending = [&pending_aborted](boost::system::error_code ec,
			notify_events)
		{
			BOOST_TEST(ec == net::error::operation_aborted);
			++pending_aborted;
		};

		queue.push(collect);
		queue.push(collect);
		queue.push(pending);

		// 缓存里只有两批，第三个等待会挂起。
		BOOST_TEST(pump_until(io, [&got] { return got.size() == 2; }));
		BOOST_CHECK(got.size() == 2 && got[0] == "two" && got[1] == "three");

		queue.abort_all(net::error::operation_aborted);
		BOOST_TEST(pump_until(io, [&pending_aborted]
			{
				return pending_aborted == 1;
			}));
	}

	// 取消槽生效时以 operation_aborted 完成，且不影响其它等待。
	BOOST_AUTO_TEST_CASE(cancel_by_slot)
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

				BOOST_TEST(events.empty());
			}));

		watchman::detail::assign_cancellation(signal.slot(), [&queue, id]
			{
				queue.abort(id, net::error::operation_aborted);
			});

		// none 不是受支持的取消类型。
		signal.emit(net::cancellation_type::none);
		BOOST_TEST(!called);
		BOOST_TEST(queue.size() == 1);

		signal.emit(net::cancellation_type::terminal);

		BOOST_TEST(queue.empty());

		io.run();

		BOOST_TEST(called);
		BOOST_TEST(result == net::error::operation_aborted);
	}

	// 中止全部等待。
	BOOST_AUTO_TEST_CASE(abort_all)
	{
		net::io_context io;
		watchman::detail::wait_queue queue(io.get_executor());

		int aborted = 0;
		const auto collect = [&aborted](boost::system::error_code ec,
			notify_events events)
		{
			BOOST_TEST(ec == net::error::operation_aborted);
			BOOST_TEST(events.empty());

			++aborted;
		};

		queue.push(collect);
		queue.push(collect);

		queue.abort_all(net::error::operation_aborted);

		BOOST_TEST(queue.empty());

		io.run();
		BOOST_TEST(aborted == 2);
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
	BOOST_AUTO_TEST_CASE(threaded_pump)
	{
		fake_source source;
		net::io_context io;
		watchman::detail::threaded_pump pump(source, io.get_executor());

		pump.start();

		std::vector<std::string> got;
		pump.async_wait([&got](boost::system::error_code ec, notify_events events)
			{
				BOOST_TEST(!ec);
				got.push_back(batch_name(events));
			});

		source.push(make_batch("one"));
		BOOST_TEST(pump_until(io, [&got] { return got.size() == 1; }));

		pump.async_wait([&got](boost::system::error_code ec, notify_events events)
			{
				BOOST_TEST(!ec);
				got.push_back(batch_name(events));
			});

		source.push(make_batch("two"));
		BOOST_TEST(pump_until(io, [&got] { return got.size() == 2; }));

		BOOST_CHECK(got.size() == 2 && got[0] == "one" && got[1] == "two");

		pump.stop({});
	}

	// 事件泵支持按等待取消，也支持服务级取消。
	BOOST_AUTO_TEST_CASE(threaded_pump_cancel)
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
		BOOST_TEST(pump_until(io, [&cancelled] { return cancelled; }));
		BOOST_TEST(cancelled_result == net::error::operation_aborted);
		BOOST_TEST(got.empty());

		// 取消一个等待不会影响后一个等待。
		source.push(make_batch("kept"));
		BOOST_TEST(pump_until(io, [&got] { return !got.empty(); }));
		BOOST_TEST(got == "kept");

		boost::system::error_code aborted_result;
		bool aborted = false;

		pump.async_wait([&](boost::system::error_code ec, notify_events)
			{
				aborted = true;
				aborted_result = ec;
			});

		pump.cancel_all();
		BOOST_TEST(pump_until(io, [&aborted] { return aborted; }));
		BOOST_TEST(aborted_result == net::error::operation_aborted);

		pump.stop(net::error::operation_aborted);
	}

	// 等待挂起期间执行器必须保持有工作，否则 io_context::run() 会提前返回，
	// 之后投递的完成动作就没有线程去执行了。
	BOOST_AUTO_TEST_CASE(pending_wait_keeps_context_alive)
	{
		fake_source source;
		net::io_context io;
		watchman::detail::threaded_pump pump(source, io.get_executor());

		pump.start();

		std::atomic<bool> ran{ false };
		std::atomic<bool> returned{ false };

		pump.async_wait([&ran](boost::system::error_code, notify_events)
			{
				ran = true;
			});

		std::thread thread([&]
			{
				io.run();
				returned = true;
			});

		std::this_thread::sleep_for(200ms);
		BOOST_TEST(!returned.load());

		source.push(make_batch("kept"));
		BOOST_TEST(wait_until([&ran] { return ran.load(); }));

		io.stop();
		thread.join();
		pump.stop({});
	}

	// 关闭事件泵时未完成的等待以传入的错误码完成。
	BOOST_AUTO_TEST_CASE(threaded_pump_stop)
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

		BOOST_TEST(pump_until(io, [&called] { return called; }));
		BOOST_TEST(result == net::error::operation_aborted);

		// 可以重新启动。
		pump.start();

		std::string got;
		pump.async_wait([&got](boost::system::error_code, notify_events events)
			{
				got = batch_name(events);
			});

		source.push(make_batch("restarted"));
		BOOST_TEST(pump_until(io, [&got] { return !got.empty(); }));
		BOOST_TEST(got == "restarted");

		pump.stop({});
	}
} // namespace
