//
// simple_watch.cpp
// ~~~~~~~~~~~~~~~~
//
// Copyright (c) 2023 Jack (jack.arain at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
//

#include <cstdlib>
#include <iostream>
#include <string>

#include <boost/asio/io_context.hpp>
#include <boost/asio/signal_set.hpp>

#include "common.hpp"

int main(int argc, char* argv[])
{
	if (argc < 2)
	{
		std::cerr << "usage: " << argv[0] << " <dir>" << std::endl;
		return EXIT_FAILURE;
	}

	const std::string dir = argv[1];
	boost::asio::io_context io;

	try
	{
		watchman::watcher watch(io.get_executor(), dir);
		watchman::example::watch_printer printer(watch, std::cout);

		printer.start();

		boost::asio::signal_set signals(io, SIGINT, SIGTERM);
		signals.async_wait([&](boost::system::error_code, int) { io.stop(); });

		std::cout << "watching " << dir << ", press Ctrl-C to quit" << std::endl;

		io.run();
	}
	catch (const std::exception& e)
	{
		std::cerr << "failed to watch " << dir << ": " << e.what() << std::endl;
		return EXIT_FAILURE;
	}

	return EXIT_SUCCESS;
}
