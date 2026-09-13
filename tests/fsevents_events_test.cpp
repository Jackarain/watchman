//
// fsevents_events_test.cpp
// ~~~~~~~~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2023 Jack (jack.arain at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
//

#include "test_util.hpp"

#include <watchman/detail/fsevents_events.hpp>

#include <boost/filesystem.hpp>

#include <string>
#include <vector>

namespace {

	namespace fs = boost::filesystem;

	using watchman::event_type;
	using watchman::notify_events;
	using watchman::detail::fsevents_event;
	using watchman::detail::fsevents_event_filter;

	struct event_input
	{
		fsevents_event event;
		const char* path;
		bool exists;
	};

	fsevents_event make_event(bool created, bool removed, bool renamed,
		bool modified)
	{
		fsevents_event event;
		event.created = created;
		event.removed = removed;
		event.renamed = renamed;
		event.modified = modified;

		return event;
	}

	notify_events convert(fsevents_event_filter& filter,
		const std::vector<event_input>& inputs)
	{
		notify_events batch;

		for (const auto& item : inputs)
			filter.add_event(batch, item.event, item.path, item.exists);

		filter.flush_renames(batch);

		return batch;
	}

	bool has_event(const notify_events& batch, event_type type,
		const fs::path& path, const fs::path& new_path = {})
	{
		for (const auto& event : batch)
		{
			if (event.type_ == type && event.path_ == path
				&& event.new_path_ == new_path)
				return true;
		}

		return false;
	}

	// 同一路径第一次带创建标志的事件是创建，之后的是合并进来的修改。
	void test_created_then_modified()
	{
		fsevents_event_filter filter;
		const fsevents_event coalesced = make_event(true, false, false, true);

		notify_events batch = convert(filter, {
			{ coalesced, "/watch/file.txt", true },
		});

		WATCHMAN_CHECK(batch.size() == 1);
		WATCHMAN_CHECK(has_event(batch, event_type::creation, "/watch/file.txt"));

		batch = convert(filter, {
			{ coalesced, "/watch/file.txt", true },
			{ make_event(false, false, false, true), "/watch/file.txt", true },
		});

		WATCHMAN_CHECK(batch.size() == 2);
		WATCHMAN_CHECK(has_event(batch, event_type::modification,
			"/watch/file.txt"));
	}

	// 删除之后同名路径重新出现，应当再次报告创建。
	void test_removed_forgets_path()
	{
		fsevents_event_filter filter;

		notify_events batch = convert(filter, {
			{ make_event(true, false, false, true), "/watch/file.txt", true },
			{ make_event(false, true, false, false), "/watch/file.txt", false },
			{ make_event(true, false, false, true), "/watch/file.txt", true },
		});

		WATCHMAN_CHECK(batch.size() == 3);
		WATCHMAN_CHECK(has_event(batch, event_type::creation, "/watch/file.txt"));
		WATCHMAN_CHECK(has_event(batch, event_type::deletion, "/watch/file.txt"));
	}

	void test_modified_without_create()
	{
		fsevents_event_filter filter;

		const notify_events batch = convert(filter, {
			{ make_event(false, false, false, true), "/watch/file.txt", true },
			{ make_event(false, false, false, false), "/watch/file.txt", true },
		});

		WATCHMAN_CHECK(batch.size() == 1);
		WATCHMAN_CHECK(has_event(batch, event_type::modification,
			"/watch/file.txt"));
	}

	// 同一批里的两侧重命名配对成一个带新路径的事件。
	void test_rename_pair()
	{
		fsevents_event_filter filter;

		const notify_events batch = convert(filter, {
			{ make_event(false, false, true, false), "/watch/from.txt", false },
			{ make_event(false, false, true, false), "/watch/to.txt", true },
		});

		WATCHMAN_CHECK(batch.size() == 1);
		WATCHMAN_CHECK(has_event(batch, event_type::rename, "/watch/from.txt",
			"/watch/to.txt"));
	}

	// 只有一侧的重命名按单侧事件报告。
	void test_rename_single_side()
	{
		fsevents_event_filter filter;

		notify_events batch = convert(filter, {
			{ make_event(false, false, true, false), "/watch/moved.txt", false },
		});

		WATCHMAN_CHECK(batch.size() == 1);
		WATCHMAN_CHECK(has_event(batch, event_type::rename, "/watch/moved.txt"));

		filter.clear();
		batch = convert(filter, {
			{ make_event(false, false, true, false), "/watch/back.txt", true },
		});

		WATCHMAN_CHECK(batch.size() == 1);
		WATCHMAN_CHECK(has_event(batch, event_type::rename, "/watch/back.txt"));
	}

	// 合并上报时重命名标志优先于创建标志。
	void test_rename_before_created()
	{
		fsevents_event_filter filter;

		const notify_events batch = convert(filter, {
			{ make_event(true, false, true, true), "/watch/to.txt", true },
		});

		WATCHMAN_CHECK(batch.size() == 1);
		WATCHMAN_CHECK(has_event(batch, event_type::rename, "/watch/to.txt"));

		// 重命名到的新路径已经记下来，之后的修改不再算创建。
		const notify_events next = convert(filter, {
			{ make_event(true, false, false, true), "/watch/to.txt", true },
		});

		WATCHMAN_CHECK(next.size() == 1);
		WATCHMAN_CHECK(has_event(next, event_type::modification, "/watch/to.txt"));
	}
} // namespace

int main()
{
	test_created_then_modified();
	test_removed_forgets_path();
	test_modified_without_create();
	test_rename_pair();
	test_rename_single_side();
	test_rename_before_created();

	return watchman::test::summary("fsevents_events");
}
