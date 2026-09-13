//
// macos_compile_test.cpp
// ~~~~~~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2023 Jack (jack.arain at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
//

// FSEvents 后端的编译期检查：本地没有 macOS SDK 时使用 tests/platform/stub
// 下的桩接口，保证这一份代码不会因为长期不被编译而失效。

#include <type_traits>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/filesystem.hpp>

#include <watchman/macos/macos_watchman.hpp>

#include "watch_service_interface.hpp"

namespace {

	namespace net = boost::asio;
	namespace fs = boost::filesystem;

	using macos_watch = watchman::macos_watch;

	static_assert(std::is_same_v<macos_watch,
		watchman::macos_watch_service<net::any_io_executor>>);

	static_assert(watchman::test::watch_service_interface<macos_watch>);

	static_assert(macos_watch::supported_cancellation ==
		(net::cancellation_type::terminal | net::cancellation_type::total));

	using concrete_watch =
		macos_watch::rebind<net::io_context::executor_type>::other;

	static_assert(watchman::test::watch_service_interface<concrete_watch>);

	// 该函数不会被调用，只为实例化各平台的等待入口。
	void use_macos_watch(const fs::path& dir)
	{
		net::io_context io;
		macos_watch watch(io.get_executor(), dir);

		watch.async_wait([](boost::system::error_code, watchman::notify_events)
			{
			});

		watch.async_wait(net::use_future);
		watch.cancel();
		watch.close();
	}

	// 协程 token 同样可以作为完成令牌。
	void await_macos_watch(macos_watch& watch)
	{
		static_cast<void>(watchman::test::await_watch_events(watch));
	}
} // namespace

int main()
{
	use_macos_watch(fs::path{});
	return 0;
}
