/*
 * port.h
 * ~~~~~~
 *
 * 仅供在非 Solaris 平台上对 event ports 后端做编译期检查使用，
 * 取自 Solaris/illumos 的公开接口。
 */

#ifndef WATCHMAN_STUB_PORT_H
#define WATCHMAN_STUB_PORT_H

#include <stdint.h>
#include <sys/types.h>
#include <sys/time.h>

typedef struct timespec timestruc_t;

typedef struct file_obj
{
	timestruc_t fo_atime;
	timestruc_t fo_mtime;
	timestruc_t fo_ctime;
	uintptr_t fo_pad[3];
	char* fo_name;
} file_obj_t;

typedef struct port_event
{
	int portev_events;
	unsigned short portev_source;
	unsigned short portev_pad;
	uintptr_t portev_object;
	void* portev_user;
} port_event_t;

#define PORT_SOURCE_AIO		1
#define PORT_SOURCE_TIMER	2
#define PORT_SOURCE_USER	3
#define PORT_SOURCE_FD		4
#define PORT_SOURCE_ALERT	5
#define PORT_SOURCE_MQ		6
#define PORT_SOURCE_FILE	7

#define FILE_ACCESS		0x00000001
#define FILE_MODIFIED		0x00000002
#define FILE_ATTRIB		0x00000004
#define FILE_TRUNC		0x00100000
#define FILE_NOFOLLOW		0x10000000
#define FILE_DELETE		0x00000010
#define FILE_RENAME_TO		0x00000020
#define FILE_RENAME_FROM	0x00000040
#define UNMOUNTED		0x20000000
#define MOUNTEDOVER		0x40000000

int port_create(void);

int port_associate(int port, int source, uintptr_t object, int events,
	void* user);

int port_dissociate(int port, int source, uintptr_t object);

int port_send(int port, int events, void* user);

int port_getn(int port, port_event_t* list, unsigned int max,
	unsigned int* nget, const struct timespec* timeout);

#endif /* WATCHMAN_STUB_PORT_H */
