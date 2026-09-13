//
// common.hpp
// ~~~~~~~~~~
//
// Copyright (c) 2023 Jack (jack.arain at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
//

#pragma once

#include <watchman/watchman.hpp>

#include <iostream>
#include <utility>

namespace watchman {
	namespace example {

		inline void print_event(const notify_event& event, std::ostream& out)
		{
			out << to_string(event.type_) << ' ' << event.path_.string();

			if (!event.new_path_.empty())
				out << " -> " << event.new_path_.string();

			out << std::endl;
		}

		// 持续等待事件并打印，直到等待被取消或出错。
		class watch_printer
		{
		public:
			watch_printer(watcher& watch, std::ostream& out)
				: m_watch(watch)
				, m_out(out)
			{}

			void start()
			{
				wait_next();
			}

		private:
			void wait_next()
			{
				m_watch.async_wait(
					[this](boost::system::error_code ec, notify_events events)
					{
						on_events(ec, std::move(events));
					});
			}

			void on_events(boost::system::error_code ec, notify_events events)
			{
				if (ec)
				{
					m_out << "watch error: " << ec.message() << std::endl;
					return;
				}

				for (const auto& event : events)
					print_event(event, m_out);

				wait_next();
			}

			watcher& m_watch;
			std::ostream& m_out;
		};
	} // namespace example
} // namespace watchman
