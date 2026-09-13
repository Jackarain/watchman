//
// notify_event_test.cpp
// ~~~~~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2023 Jack (jack.arain at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
//

#include <string>

#include <watchman/notify_event.hpp>

#include "test_util.hpp"

int main()
{
	using namespace watchman;

	WATCHMAN_CHECK(std::string(to_string(event_type::unknown)) == "unknown");
	WATCHMAN_CHECK(std::string(to_string(event_type::creation)) == "creation");
	WATCHMAN_CHECK(std::string(to_string(event_type::deletion)) == "deletion");
	WATCHMAN_CHECK(std::string(to_string(event_type::modification)) == "modification");
	WATCHMAN_CHECK(std::string(to_string(event_type::rename)) == "rename");

	// 未识别的取值按 unknown 处理。
	WATCHMAN_CHECK(std::string(to_string(static_cast<event_type>(42))) == "unknown");

	notify_event empty;
	WATCHMAN_CHECK(empty.path_.empty());
	WATCHMAN_CHECK(empty.new_path_.empty());

	notify_event renamed{ event_type::rename, "/tmp/old", "/tmp/new" };
	WATCHMAN_CHECK(renamed.type_ == event_type::rename);
	WATCHMAN_CHECK(renamed.path_.string() == "/tmp/old");
	WATCHMAN_CHECK(renamed.new_path_.string() == "/tmp/new");

	notify_events events{ empty, renamed };
	WATCHMAN_CHECK(events.size() == 2);

	return test::summary("notify_event");
}
