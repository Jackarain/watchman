/*
 * dispatch/dispatch.h
 * ~~~~~~~~~~~~~~~~~~~
 *
 * 仅供在非 macOS 平台上对 FSEvents 后端做编译期检查使用。
 */

#ifndef WATCHMAN_STUB_DISPATCH_H
#define WATCHMAN_STUB_DISPATCH_H

typedef struct dispatch_queue_s* dispatch_queue_t;

dispatch_queue_t dispatch_queue_create(const char* label, void* attr);

void dispatch_release(dispatch_queue_t object);

#endif /* WATCHMAN_STUB_DISPATCH_H */
