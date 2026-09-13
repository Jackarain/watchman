//
// main.cpp
// ~~~~~~~~
//
// Copyright (c) 2023 Jack (jack.arain at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
//

// 以三方库方式使用 watchman 的最小程序：安装后通过 find_package 查找，或把
// 源码目录作为子目录引入，两种方式都必须能编译并收到通知。

#include <watchman/watchman.hpp>

#include <boost/asio/io_context.hpp>
#include <boost/filesystem.hpp>

#include <chrono>
#include <fstream>
#include <iostream>
#include <thread>

#include <cstdlib>

namespace {

	namespace fs = boost::filesystem;

	fs::path make_temp_dir()
	{
		const fs::path dir = fs::temp_directory_path() /
			fs::unique_path("watchman-consumer-%%%%-%%%%-%%%%.tmp");

		fs::create_directories(dir);
		return dir;
	}

	// 等监视动作挂到内核之后再写入文件。
	void write_later(const fs::path& file)
	{
		std::this_thread::sleep_for(std::chrono::milliseconds{ 200 });

		std::ofstream out(file.string(), std::ios::binary | std::ios::trunc);
		out << "data";
	}
} // namespace

int main()
{
	boost::asio::io_context io;
	const fs::path dir = make_temp_dir();

	bool notified = false;

	watchman::watcher watch(io.get_executor(), dir);

	watch.async_wait([&](boost::system::error_code ec, watchman::notify_events events)
		{
			notified = !ec && !events.empty();
		});

	std::thread writer(write_later, dir / "file.txt");
	io.run();
	writer.join();

	boost::system::error_code ignore_ec;
	fs::remove_all(dir, ignore_ec);

	if (!notified)
	{
		std::cerr << "watchman consumer: no notification received" << std::endl;
		return EXIT_FAILURE;
	}

	std::cout << "watchman consumer: ok" << std::endl;
	return EXIT_SUCCESS;
}
