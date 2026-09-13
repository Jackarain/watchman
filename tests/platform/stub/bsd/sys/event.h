/*
 * sys/event.h
 * ~~~~~~~~~~~
 *
 * 仅供在非 BSD 平台上对 kqueue 后端做编译期检查使用，取自 BSD 的公开接口。
 */

#ifndef WATCHMAN_STUB_SYS_EVENT_H
#define WATCHMAN_STUB_SYS_EVENT_H

#include <stdint.h>
#include <sys/types.h>
#include <sys/time.h>

#define EVFILT_READ		(-1)
#define EVFILT_WRITE		(-2)
#define EVFILT_AIO		(-3)
#define EVFILT_VNODE		(-4)
#define EVFILT_PROC		(-5)
#define EVFILT_SIGNAL		(-6)
#define EVFILT_TIMER		(-7)
#define EVFILT_USER		(-10)

#define EV_ADD			0x0001
#define EV_DELETE		0x0002
#define EV_ENABLE		0x0004
#define EV_DISABLE		0x0008
#define EV_ONESHOT		0x0010
#define EV_CLEAR		0x0020
#define EV_RECEIPT		0x0040
#define EV_DISPATCH		0x0080
#define EV_ERROR		0x4000
#define EV_EOF			0x8000

#define NOTE_DELETE		0x00000001
#define NOTE_WRITE		0x00000002
#define NOTE_EXTEND		0x00000004
#define NOTE_ATTRIB		0x00000008
#define NOTE_LINK		0x00000010
#define NOTE_RENAME		0x00000020
#define NOTE_REVOKE		0x00000040
#define NOTE_TRIGGER		0x01000000

struct kevent
{
	uintptr_t ident;
	short filter;
	unsigned short flags;
	unsigned int fflags;
	intptr_t data;
	void* udata;
};

#define EV_SET(kevp_, a, b, c, d, e, f) \
	do { \
		struct kevent* kevp = (kevp_); \
		kevp->ident = (a); \
		kevp->filter = (b); \
		kevp->flags = (c); \
		kevp->fflags = (d); \
		kevp->data = (e); \
		kevp->udata = (f); \
	} while (0)

int kqueue(void);

int kevent(int kq, const struct kevent* changelist, int nchanges,
	struct kevent* eventlist, int nevents, const struct timespec* timeout);

#endif /* WATCHMAN_STUB_SYS_EVENT_H */
