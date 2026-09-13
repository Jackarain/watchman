//
// watch_service_test.cpp
// ~~~~~~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2023 Jack (jack.arain at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
//

#include <chrono>
#include <condition_variable>
#include <fstream>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>

#include <watchman/watchman.hpp>

#include "test_util.hpp"

namespace {

	namespace net = boost::asio;
	namespace fs = boost::filesystem;

	using namespace std::chrono_literals;
	using notify_events = watchman::notify_events;

	void write_file(const fs::path& path, const std::string& content)
	{
		std::ofstream out(path.string(), std::ios::binary | std::ios::trunc);
		out << content;
	}

	bool has_event(const notify_events& events, const fs::path& path,
		watchman::event_type type)
	{
		for (const auto& event : events)
		{
			if (event.type_ == type && event.path_ == path)
				return true;
		}
		return false;
	}

	bool has_path(const notify_events& events, const fs::path& path)
	{
		for (const auto& event : events)
		{
			if (event.path_ == path)
				return true;
		}
		return false;
	}

	// 收集异步等待返回的事件，供测试线程按条件等待。
	class event_collector
	{
	public:
		void add(boost::system::error_code ec, notify_events new_events)
		{
			{
				std::lock_guard<std::mutex> lock(m_mtx);
				m_error = ec;

				for (auto& event : new_events)
					m_events.push_back(std::move(event));
			}

			m_cv.notify_all();
		}

		bool wait_for(const std::function<bool(const notify_events&)>& pred,
			std::chrono::milliseconds timeout = 10s)
		{
			std::unique_lock<std::mutex> lock(m_mtx);
			return m_cv.wait_for(lock, timeout, [&] { return pred(m_events); });
		}

		bool wait_for_event(const fs::path& path, watchman::event_type type)
		{
			return wait_for([&](const notify_events& all)
				{
					return has_event(all, path, type);
				});
		}

		// 在 quiet 时间内不应出现该路径的事件。
		bool wait_for_no_event(const fs::path& path,
			std::chrono::milliseconds quiet = 500ms)
		{
			const bool found = wait_for([&](const notify_events& all)
				{
					return has_path(all, path);
				}, quiet);

			return !found;
		}

	private:
		std::mutex m_mtx;
		std::condition_variable m_cv;
		notify_events m_events;
		boost::system::error_code m_error;
	};

	// 监视临时目录，并在 io_context 线程上持续等待事件。
	class watch_bed
	{
	public:
		watch_bed() = default;

		~watch_bed()
		{
			stop();
		}

		const fs::path& dir() const noexcept { return m_temp.path(); }
		event_collector& collector() noexcept { return m_collector; }

		void exclude(std::vector<fs::path> dirs)
		{
			m_excluded = std::move(dirs);
		}

		void start()
		{
			m_watch = std::make_unique<watchman::watcher>(
				m_io.get_executor(), dir(), m_excluded);

			m_work = std::make_unique<work_guard_type>(net::make_work_guard(m_io));

			wait_next();
			m_thread = std::thread([this] { m_io.run(); });
		}

		void stop()
		{
			if (m_work)
				m_work->reset();

			m_io.stop();

			if (m_thread.joinable())
				m_thread.join();

			m_watch.reset();
		}

	private:
		using work_guard_type =
			net::executor_work_guard<net::io_context::executor_type>;

		void wait_next()
		{
			m_watch->async_wait(
				[this](boost::system::error_code ec, notify_events events)
				{
					m_collector.add(ec, std::move(events));

					if (!ec)
						wait_next();
				});
		}

		watchman::test::temp_dir m_temp;
		std::vector<fs::path> m_excluded;
		net::io_context m_io;
		event_collector m_collector;
		std::unique_ptr<watchman::watcher> m_watch;
		std::unique_ptr<work_guard_type> m_work;
		std::thread m_thread;
	};

	void test_create_modify_delete()
	{
		watch_bed bed;
		bed.start();

		const fs::path file = bed.dir() / "file.txt";

		write_file(file, "hello");
		WATCHMAN_CHECK(bed.collector().wait_for_event(file,
			watchman::event_type::creation));

		write_file(file, "hello watchman");
		WATCHMAN_CHECK(bed.collector().wait_for_event(file,
			watchman::event_type::modification));

		fs::remove(file);
		WATCHMAN_CHECK(bed.collector().wait_for_event(file,
			watchman::event_type::deletion));
	}

	void test_sub_directory()
	{
		watch_bed bed;
		bed.start();

		const fs::path sub = bed.dir() / "sub";
		fs::create_directory(sub);
		WATCHMAN_CHECK(bed.collector().wait_for_event(sub,
			watchman::event_type::creation));

		// 新建目录下的文件同样应被监视。
		const fs::path inner = sub / "inner.txt";
		write_file(inner, "data");
		WATCHMAN_CHECK(bed.collector().wait_for_event(inner,
			watchman::event_type::creation));

		write_file(inner, "data2");
		WATCHMAN_CHECK(bed.collector().wait_for_event(inner,
			watchman::event_type::modification));
	}

	void test_excluded_dirs()
	{
		watch_bed bed;
		const fs::path skip = bed.dir() / "skip";
		const fs::path kept = bed.dir() / "kept";

		fs::create_directories(skip);
		fs::create_directories(kept);

		bed.exclude({ skip });
		bed.start();

		const fs::path dropped = skip / "file.txt";
		const fs::path created = kept / "file.txt";

		write_file(created, "kept");
		write_file(dropped, "dropped");

		WATCHMAN_CHECK(bed.collector().wait_for_event(created,
			watchman::event_type::creation));

		WATCHMAN_CHECK(bed.collector().wait_for_no_event(dropped));
		WATCHMAN_CHECK(bed.collector().wait_for_no_event(skip));
	}

	void test_cancel_wait()
	{
		watchman::test::temp_dir temp;
		net::io_context io;

		watchman::watcher watch(io.get_executor(), temp.path());

		net::cancellation_signal signal;
		std::promise<boost::system::error_code> promise;
		auto future = promise.get_future();

		watch.async_wait(net::bind_cancellation_slot(signal.slot(),
			[&promise](boost::system::error_code ec, notify_events)
			{
				promise.set_value(ec);
			}));

		std::thread thread([&] { io.run(); });

		// 等待动作已在 io_context 线程上挂起后再取消。
		std::this_thread::sleep_for(200ms);
		signal.emit(net::cancellation_type::all);

		WATCHMAN_CHECK(future.wait_for(10s) == std::future_status::ready);
		WATCHMAN_CHECK(future.get() == net::error::operation_aborted);

		io.stop();
		thread.join();
	}
} // namespace

int main()
{
	test_create_modify_delete();
	test_sub_directory();
	test_excluded_dirs();
	test_cancel_wait();

	return watchman::test::summary("watch_service");
}
