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

#define BOOST_TEST_MODULE notify_event
#include <boost/test/included/unit_test.hpp>

#include <watchman/notify_event.hpp>

#include <string>

namespace {

	using namespace watchman;

	BOOST_AUTO_TEST_CASE(event_type_names)
	{
		BOOST_TEST(std::string(to_string(event_type::unknown)) == "unknown");
		BOOST_TEST(std::string(to_string(event_type::creation)) == "creation");
		BOOST_TEST(std::string(to_string(event_type::deletion)) == "deletion");
		BOOST_TEST(std::string(to_string(event_type::modification)) == "modification");
		BOOST_TEST(std::string(to_string(event_type::rename)) == "rename");

		// 未识别的取值按 unknown 处理。
		BOOST_TEST(std::string(to_string(static_cast<event_type>(42))) == "unknown");
	}

	BOOST_AUTO_TEST_CASE(notify_event_fields)
	{
		notify_event empty;
		BOOST_TEST(empty.path_.empty());
		BOOST_TEST(empty.new_path_.empty());

		notify_event renamed{ event_type::rename, "/tmp/old", "/tmp/new" };
		BOOST_CHECK(renamed.type_ == event_type::rename);
		BOOST_TEST(renamed.path_.string() == "/tmp/old");
		BOOST_TEST(renamed.new_path_.string() == "/tmp/new");

		notify_events events{ empty, renamed };
		BOOST_TEST(events.size() == 2);
	}
} // namespace
