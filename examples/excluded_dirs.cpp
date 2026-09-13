//
// excluded_dirs.cpp
// ~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2023 Jack (jack.arain at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
//

#include "common.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/filesystem.hpp>

#include <iostream>
#include <string>
#include <vector>

#include <cstdlib>

namespace {

	void print_excluded(std::ostream& out, const std::vector<boost::filesystem::path>& dirs)
	{
		out << "excluded:";

		for (const auto& dir : dirs)
			out << ' ' << dir.string();

		out << std::endl;
	}
} // namespace

int main(int argc, char* argv[])
{
	if (argc < 3)
	{
		std::cerr << "usage: " << argv[0] << " <dir> <excluded-dir>..."
			<< std::endl;
		return EXIT_FAILURE;
	}

	const std::string dir = argv[1];
	std::vector<boost::filesystem::path> excluded_dirs;

	for (int i = 2; i < argc; ++i)
		excluded_dirs.emplace_back(argv[i]);

	boost::asio::io_context io;

	try
	{
		watchman::watcher watch(io.get_executor(), dir, excluded_dirs);
		watchman::example::watch_printer printer(watch, std::cout);

		printer.start();

		boost::asio::signal_set signals(io, SIGINT, SIGTERM);
		signals.async_wait([&](boost::system::error_code, int) { io.stop(); });

		std::cout << "watching " << dir << std::endl;
		print_excluded(std::cout, excluded_dirs);

		io.run();
	}
	catch (const std::exception& e)
	{
		std::cerr << "failed to watch " << dir << ": " << e.what() << std::endl;
		return EXIT_FAILURE;
	}

	return EXIT_SUCCESS;
}
