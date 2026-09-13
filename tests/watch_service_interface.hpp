//
// watch_service_interface.hpp
// ~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2023 Jack (jack.arain at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
//

#pragma once

#include <watchman/notify_event.hpp>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/filesystem.hpp>
#include <boost/system/error_code.hpp>

#include <concepts>
#include <functional>
#include <type_traits>
#include <vector>

namespace watchman {
	namespace test {

		namespace net = boost::asio;
		namespace fs = boost::filesystem;

		// 各平台实现必须提供同样的公开接口，避免出现平台差异。
		template <typename Service>
		concept watch_service_interface = requires(
			Service service,
			const fs::path& dir,
			boost::system::error_code& ec,
			std::function<void(boost::system::error_code,
				watchman::notify_events)> handler)
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
			{ service.async_wait(handler) };
			typename Service::template rebind<net::io_context::executor_type>::other;
		};
		// 等待动作支持协程 token。
		template <typename Service>
		net::awaitable<void> await_watch_events(Service& service)
		{
			const auto events =
				co_await service.async_wait(net::use_awaitable);

			(void)events;
		}
	} // namespace test
} // namespace watchman
