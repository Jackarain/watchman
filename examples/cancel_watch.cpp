//
// cancel_watch.cpp
// ~~~~~~~~~~~~~~~~
//
// Copyright (c) 2023 Jack (jack.arain at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
//

#include "common.hpp"

#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>

#include <chrono>
#include <iostream>
#include <string>
#include <utility>

#include <cstdlib>

namespace {

	void on_timeout(boost::asio::cancellation_signal& signal)
	{
		std::cout << "cancelling wait" << std::endl;
		signal.emit(boost::asio::cancellation_type::all);
	}

	void on_events(boost::system::error_code ec, watchman::notify_events events)
	{
		if (ec == boost::asio::error::operation_aborted)
		{
			std::cout << "wait cancelled" << std::endl;
			return;
		}

		if (ec)
		{
			std::cerr << "watch error: " << ec.message() << std::endl;
			return;
		}

		for (const auto& event : events)
			watchman::example::print_event(event, std::cout);
	}
} // namespace

int main(int argc, char* argv[])
{
	if (argc < 2)
	{
		std::cerr << "usage: " << argv[0] << " <dir> [seconds]" << std::endl;
		return EXIT_FAILURE;
	}

	const std::string dir = argv[1];
	const auto seconds = (argc > 2)
		? std::chrono::seconds(std::atoi(argv[2]))
		: std::chrono::seconds(5);

	boost::asio::io_context io;
	boost::asio::cancellation_signal signal;

	try
	{
		watchman::watcher watch(io.get_executor(), dir);

		// 超时后取消尚未完成的等待动作。
		boost::asio::steady_timer timer(io, seconds);
		timer.async_wait([&](boost::system::error_code) { on_timeout(signal); });

		watch.async_wait(boost::asio::bind_cancellation_slot(signal.slot(),
			[](boost::system::error_code ec, watchman::notify_events events)
			{
				on_events(ec, std::move(events));
			}));

		std::cout << "watching " << dir << " for "
			<< seconds.count() << " seconds" << std::endl;

		io.run();
	}
	catch (const std::exception& e)
	{
		std::cerr << "failed to watch " << dir << ": " << e.what() << std::endl;
		return EXIT_FAILURE;
	}

	return EXIT_SUCCESS;
}
