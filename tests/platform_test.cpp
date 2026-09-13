//
// platform_test.cpp
// ~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2023 Jack (jack.arain at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
//

#include <concepts>
#include <cstdio>
#include <functional>
#include <string>
#include <type_traits>
#include <vector>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/filesystem.hpp>
#include <boost/predef.h>

#include <watchman/watchman.hpp>

#include "test_util.hpp"

// 按操作系统分派到对应的实现。
#if BOOST_OS_WINDOWS
static_assert(std::is_same_v<watchman::watcher, watchman::windows_watch>);
#elif BOOST_OS_LINUX
static_assert(std::is_same_v<watchman::watcher, watchman::linux_watch>);
#elif BOOST_OS_BSD
static_assert(std::is_same_v<watchman::watcher, watchman::bsd_watch>);
#elif BOOST_OS_MACOS
static_assert(std::is_same_v<watchman::watcher, watchman::macos_watch>);
#elif BOOST_OS_SOLARIS
static_assert(std::is_same_v<watchman::watcher, watchman::solaris_watch>);
#endif

namespace {

	namespace net = boost::asio;
	namespace fs = boost::filesystem;

	// 各平台实现必须提供同样的公开接口，避免出现平台差异。
	template <typename Service>
	concept watch_service_interface = requires(
		Service service,
		const fs::path& dir,
		boost::system::error_code& ec,
		std::function<void(boost::system::error_code, watchman::notify_events)> handler)
	{
		typename Service::executor_type;
		{ service.get_executor() } -> std::same_as<typename Service::executor_type>;
		service.open(dir, ec);
		service.open(dir);
		service.close(ec);
		service.close();
		service.cancel(ec);
		service.cancel();
		{ service.is_open() } -> std::same_as<bool>;
		{ service.watch_dir() } -> std::same_as<const fs::path&>;
		{ service.excluded_dirs() } -> std::same_as<const std::vector<fs::path>&>;
		{ service.is_excluded(dir) } -> std::same_as<bool>;
		service.async_wait(handler);
	};

	static_assert(watch_service_interface<watchman::watcher>);

	// 取消语义在各平台上保持一致。
	static_assert(watchman::watcher::supported_cancellation ==
		(net::cancellation_type::terminal | net::cancellation_type::total));

	// 实现可以绑定到具体的执行器类型，而不只是 any_io_executor。
	void test_concrete_executor()
	{
		using concrete_watch =
			watchman::watcher::rebind<net::io_context::executor_type>::other;

		watchman::test::temp_dir temp;
		net::io_context io;

		concrete_watch watch(io.get_executor(), temp.path());

		WATCHMAN_CHECK(watch.is_open());
		WATCHMAN_CHECK(watch.watch_dir() == temp.path());
	}
} // namespace

int main()
{
	WATCHMAN_CHECK(std::string(watchman::to_string(watchman::event_type::rename))
		== "rename");

	test_concrete_executor();

	std::printf("platform: %s\n", BOOST_PLATFORM);

	return watchman::test::summary("platform");
}
