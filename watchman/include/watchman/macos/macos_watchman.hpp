//
// macos_watchman.hpp
// ~~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2023 Jack (jack.arain at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
//

#pragma once

#include <cstring>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/error.hpp>
#include <boost/filesystem.hpp>
#include <boost/system/error_code.hpp>

#include <CoreServices/CoreServices.h>

#include "watchman/detail/watch_service_base.hpp"
#include "watchman/notify_event.hpp"

namespace watchman {
    namespace net = boost::asio;
    namespace fs = boost::filesystem;

    // macOS 使用 FSEvents 监视目录树。
    //
    // FSEvents 在独立的 dispatch 队列上回调，事件先进入内部队列，等待动作
    // 只是取走已经攒下的事件，因此取消等待没有实际动作。
    template <typename Executor = net::any_io_executor>
    class macos_watch_service
        : public detail::watch_service_base<macos_watch_service<Executor>, Executor>
    {
    private:
        using base_type =
            detail::watch_service_base<macos_watch_service<Executor>, Executor>;

        friend base_type;

        macos_watch_service(const macos_watch_service&) = delete;
        macos_watch_service& operator=(const macos_watch_service&) = delete;

    public:
        template <typename Executor1>
        struct rebind
        {
            using other = macos_watch_service<Executor1>;
        };

        macos_watch_service(const Executor& ex, const fs::path& dir,
            const std::vector<fs::path>& excluded_dirs = {})
            : base_type(ex, excluded_dirs)
        {
            this->open(dir);
        }

        explicit macos_watch_service(const Executor& ex,
            const std::vector<fs::path>& excluded_dirs = {})
            : base_type(ex, excluded_dirs)
        {}

        ~macos_watch_service()
        {
            boost::system::error_code ignore_ec;
            this->close(ignore_ec);
        }

        // 自定义移动语义：关闭源对象的流后转移所有权，避免原始指针双重释放。
        macos_watch_service(macos_watch_service&& other) noexcept
            : base_type(std::move(other))
            , m_stream(other.m_stream)
            , m_fsevents_queue(other.m_fsevents_queue)
            , m_events(std::move(other.m_events))
        {
            other.m_stream = nullptr;
            other.m_fsevents_queue = nullptr;
        }

        macos_watch_service& operator=(macos_watch_service&& other) noexcept
        {
            if (this != &other)
            {
                boost::system::error_code ignore_ec;
                this->close(ignore_ec);

                base_type::operator=(std::move(other));

                m_stream = other.m_stream;
                m_fsevents_queue = other.m_fsevents_queue;
                m_events = std::move(other.m_events);

                other.m_stream = nullptr;
                other.m_fsevents_queue = nullptr;
            }
            return *this;
        }

    private:
        // ---------- watch_service_base 要求的实现 ----------

        void open_impl(const fs::path& dir, boost::system::error_code& ec)
        {
            boost::system::error_code ignore_ec;
            close_impl(ignore_ec);

            CFStringRef dir_ref = CFStringCreateWithCString(
                nullptr, dir.c_str(), kCFStringEncodingUTF8);

            if (!dir_ref)
            {
                ec.assign(errno, boost::system::generic_category());
                return;
            }

            CFArrayRef paths =
                CFArrayCreate(nullptr,
                    reinterpret_cast<const void**>(&dir_ref),
                    1,
                    &kCFTypeArrayCallBacks);

            CFRelease(dir_ref);

            if (!paths)
            {
                ec.assign(errno, boost::system::generic_category());
                return;
            }

            auto context = &m_stream_ctx;

            context->version = 0;
            context->info = this;
            context->retain = nullptr;
            context->release = nullptr;
            context->copyDescription = nullptr;

            FSEventStreamCreateFlags streamFlags =
                kFSEventStreamCreateFlagFileEvents;
            streamFlags |= kFSEventStreamCreateFlagNoDefer;
            streamFlags |= kFSEventStreamCreateFlagUseExtendedData;
            streamFlags |= kFSEventStreamCreateFlagUseCFTypes;

            m_stream = FSEventStreamCreate(nullptr,
                &macos_watch_service<Executor>::fsevents_callback,
                context,
                paths,
                kFSEventStreamEventIdSinceNow,
                1,
                streamFlags);

            CFRelease(paths);

            if (!m_stream)
            {
                ec.assign(errno, boost::system::generic_category());
                return;
            }

            m_fsevents_queue = dispatch_queue_create("fswatch_event_queue", nullptr);
            FSEventStreamSetDispatchQueue(m_stream, m_fsevents_queue);

            if (FSEventStreamStart(m_stream))
                return;

            ec.assign(EIO, boost::system::generic_category());
            close_impl(ignore_ec);
        }

        void close_impl(boost::system::error_code& /*ec*/)
        {
            if (m_stream != nullptr)
            {
                FSEventStreamStop(m_stream);
                FSEventStreamInvalidate(m_stream);
                FSEventStreamRelease(m_stream);
                m_stream = nullptr;
            }

            if (m_fsevents_queue != nullptr)
            {
                dispatch_release(m_fsevents_queue);
                m_fsevents_queue = nullptr;
            }
        }

        // 等待动作本身不会阻塞在内核上，没有需要取消的等待。
        void cancel_impl(boost::system::error_code& ec)
        {
            ec.clear();
        }

        bool is_open_impl() const noexcept
        {
            return m_stream != nullptr;
        }

        template <typename Handler>
        void async_wait_impl(Handler&& handler)
        {
            notify_events events;

            {
                std::lock_guard<std::mutex> lock(m_event_mtx);
                events.swap(m_events);
            }

            // 投递给处理函数的关联执行器，避免在发起线程上直接回调。
            this->post_completion(std::forward<Handler>(handler),
                boost::system::error_code{}, std::move(events));
        }

        static void fsevents_callback(ConstFSEventStreamRef streamRef,
                                  void* clientCallBackInfo,
                                  size_t numEvents,
                                  void* eventPaths,
                                  const FSEventStreamEventFlags eventFlags[],
                                  const FSEventStreamEventId eventIds[])
        {
            using self_type = macos_watch_service<Executor>;
            auto* fse_monitor = static_cast<self_type*>(clientCallBackInfo);

            CFArrayRef event_array = static_cast<CFArrayRef>(eventPaths);
            std::vector<notify_event> batch;
            batch.reserve(numEvents);

            for (size_t i = 0; i < numEvents; ++i)
            {
                auto path_info_dict = static_cast<CFDictionaryRef>(
                    CFArrayGetValueAtIndex(event_array, i));

                auto path_cfstr = static_cast<CFStringRef>(
                    CFDictionaryGetValue(path_info_dict,
                        kFSEventStreamEventExtendedDataPathKey));

                if (!path_cfstr)
                    continue;

                // 安全地将 CFString 转换为 std::string。
                // CFStringGetCStringPtr 可能返回 NULL，使用 CFStringGetCString 替代。
                CFIndex length = CFStringGetLength(path_cfstr);
                CFIndex max_size = CFStringGetMaximumSizeForEncoding(
                    length, kCFStringEncodingUTF8) + 1;

                std::string path_str;
                path_str.resize(static_cast<std::string::size_type>(max_size));

                if (!CFStringGetCString(path_cfstr, &path_str[0],
                        max_size, kCFStringEncodingUTF8))
                {
                    continue;
                }

                // 按实际长度调整（不含空终止符）。
                path_str.resize(std::strlen(path_str.c_str()));

                // 跳过被排除目录中的事件。
                if (fse_monitor->is_excluded(path_str))
                    continue;

                notify_event event;
                event.path_ = std::move(path_str);
                event.type_ = event_type_from_flags(eventFlags[i]);

                batch.push_back(std::move(event));
            }

            if (batch.empty())
                return;

            std::lock_guard<std::mutex> lock(fse_monitor->m_event_mtx);

            // 限制事件队列大小，防止无限增长。
            constexpr std::size_t max_events = 100000;
            if (fse_monitor->m_events.size() > max_events)
            {
                fse_monitor->m_events.erase(
                    fse_monitor->m_events.begin(),
                    fse_monitor->m_events.begin() +
                        (fse_monitor->m_events.size() - max_events));
            }

            fse_monitor->m_events.insert(
                fse_monitor->m_events.end(),
                std::make_move_iterator(batch.begin()),
                std::make_move_iterator(batch.end()));
        }

        static event_type event_type_from_flags(
            FSEventStreamEventFlags flags) noexcept
        {
            if (flags & kFSEventStreamEventFlagItemCreated)
                return event_type::creation;

            if (flags & kFSEventStreamEventFlagItemRemoved)
                return event_type::deletion;

            if (flags & kFSEventStreamEventFlagItemRenamed)
                return event_type::rename;

            if (flags & kFSEventStreamEventFlagItemModified)
                return event_type::modification;

            return event_type::unknown;
        }

    private:
        FSEventStreamContext m_stream_ctx{};
        FSEventStreamRef m_stream = nullptr;
        dispatch_queue_t m_fsevents_queue = nullptr;
        std::mutex m_event_mtx;
        notify_events m_events;
    };

    using macos_watch = macos_watch_service<>;
} // namespace watchman
