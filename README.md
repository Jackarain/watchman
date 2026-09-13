# 一个基于 boost.asio 的文件监视库

watchman 是一个 header-only 的 C++20 目录监视库，把各平台的文件系统通知
接口统一到 boost.asio 的异步模型上：等待动作支持任意 completion token，
处理函数在自己的关联执行器上被调用，取消使用 asio 的取消槽。

**支持平台**
- Windows：`ReadDirectoryChangesW`（重叠 I/O）
- Linux：`inotify`
- macOS：`FSEvents`
- FreeBSD / OpenBSD / NetBSD / DragonFly：`kqueue`
- Solaris / illumos：event ports

平台实现各自独立，入口统一为 `watchman::watcher`，头文件
`watchman/watchman.hpp` 会根据编译目标自动选择实现。

**快速开始**
```cpp
#include <iostream>

#include <boost/asio/io_context.hpp>

#include <watchman/watchman.hpp>

int main()
{
	boost::asio::io_context io;

	watchman::watcher watch(io.get_executor(), "/tmp/watched");

	watch.async_wait([](boost::system::error_code ec, watchman::notify_events events)
		{
			if (ec)
			{
				std::cerr << "watch error: " << ec.message() << std::endl;
				return;
			}

			for (const auto& event : events)
				std::cout << watchman::to_string(event.type_) << ' ' << event.path_.string() << std::endl;
		});

	io.run();
}
```

**接口**
- 构造：`watcher(ex, dir, excluded_dirs)` 直接开始监视，或 `watcher(ex)` 之后调用 `open(dir)`
- `async_wait(handler)`：等待下一批事件，完成签名为 `void(boost::system::error_code, notify_events)`
- `open` / `close` / `cancel` / `is_open`：与 asio 的 I/O 对象一致，都提供抛异常与 `error_code` 两个版本
- `watch_dir` / `excluded_dirs` / `is_excluded`：监视目录与排除规则
- `rebind<Executor>::other`：把实现绑定到具体的执行器类型，而不只是 `any_io_executor`
- `native_handle()`：平台实现的底层描述符或句柄

**事件**
- `event_type`：`creation` / `deletion` / `modification` / `rename`（无法识别时为 `unknown`）
- `notify_event`：`type_`、`path_`，重命名还会带上 `new_path_`
- `notify_events`：一次等待返回的事件批次（`std::deque<notify_event>`）
- 目录内的重命名尽量给出新旧路径；只能看到单侧时只填 `path_`（移出）或 `path_` 指向新路径（移入）

**异步模型**
- `async_wait` 通过 `async_initiate` 发起，可以使用回调、`use_future`、`use_awaitable` 等任意 token
- 处理函数投递到它的关联执行器上，并沿用关联分配器
- 允许同时发起多个等待，每个等待独立持有自己的状态，按发起顺序消费事件批次
- 取消通过处理函数的关联取消槽生效，支持 `terminal` 与 `total`，被取消的等待以
  `boost::asio::error::operation_aborted` 完成
- 关闭时未完成的等待同样以 `operation_aborted` 完成

**依赖**
- C++20 编译器
- Boost 1.78 及以上（需要 asio 的关联取消槽与 posix 描述符的按操作取消）

**构建与测试**
```console
$ cmake -S . -B build -G Ninja
$ cmake --build build
$ ctest --test-dir build --output-on-failure
```

**作为三方库使用**
- 子目录方式：`add_subdirectory(watchman)` 后链接 `watchman::watchman`
- FetchContent 方式：
```cmake
include(FetchContent)
FetchContent_Declare(watchman GIT_REPOSITORY <repo> GIT_TAG <tag>)
FetchContent_MakeAvailable(watchman)
target_link_libraries(app PRIVATE watchman::watchman)
```
- 安装后查找：`find_package(watchman CONFIG REQUIRED)`，再链接 `watchman::watchman`
- 以三方库方式引入时，`WATCHMAN_BUILD_EXAMPLES`、`WATCHMAN_BUILD_TESTS`、`WATCHMAN_INSTALL` 默认关闭

**平台实现说明**
- Windows：重叠 I/O + `CancelIoEx`，每个等待自带读缓冲区
- Linux：`inotify` 直接接到 asio 的描述符操作上，等待即读事件，支持按操作取消
- macOS：FSEvents 在自己的队列上推送事件，等待动作从内部等待队列取一个批次
- BSD：kqueue 只报告“被监视的节点发生了变化”，目录事件由两次目录扫描的差异还原
- Solaris：每个文件与目录通过 `port_associate` 关联到 port 上，事件一次性，处理完重新关联
- BSD 与 Solaris 后端由后台线程取内核事件，再通过 `post` 完成等待动作，因此不阻塞执行器

**测试**
- `tests/notify_event_test.cpp`、`tests/path_exclusion_test.cpp`：事件类型与路径排除规则
- `tests/wait_queue_test.cpp`：等待队列与后台事件泵（顺序、缓存、取消、关闭）
- `tests/platform_test.cpp`：各平台实现的接口一致性（concept）与具体执行器绑定
- `tests/watch_service_test.cpp`：目录监视的端到端用例，目前只在 Linux 上运行
- `tests/platform/`：BSD 与 Solaris 后端在本地缺少系统头文件时，使用桩接口做编译期检查

**排除规则**
- `excluded_dirs` 中的目录及其子目录不会产生事件，也不会被递归监视
- 规则按字面路径比较，不解析 `..` 与符号链接
- 需要在构造或 `open` 之前准备好被排除的目录

**限制**
- BSD 与 Solaris 后端为每个被监视的文件和目录各占一个内核对象，递归监视大树时资源占用较高
- 重命名事件的配对依赖内核给出的信息，跨批次的重命名可能只报告单侧
