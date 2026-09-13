/*
 * CoreServices/CoreServices.h
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~
 *
 * 仅供在非 macOS 平台上对 FSEvents 后端做编译期检查使用，
 * 取自 macOS SDK 的公开接口。
 */

#ifndef WATCHMAN_STUB_CORESERVICES_H
#define WATCHMAN_STUB_CORESERVICES_H

#include <stddef.h>
#include <stdint.h>

#include <dispatch/dispatch.h>

typedef unsigned char Boolean;
typedef unsigned int UInt32;
typedef unsigned long long UInt64;
typedef long CFIndex;
typedef unsigned int CFStringEncoding;
typedef double CFTimeInterval;

typedef const void* CFTypeRef;
typedef const struct __CFAllocator* CFAllocatorRef;
typedef const struct __CFString* CFStringRef;
typedef const struct __CFArray* CFArrayRef;
typedef const struct __CFDictionary* CFDictionaryRef;

typedef struct
{
	CFIndex version;
} CFArrayCallBacks;

extern const CFArrayCallBacks kCFTypeArrayCallBacks;

enum
{
	kCFStringEncodingUTF8 = 0x08000100
};

CFStringRef CFStringCreateWithCString(CFAllocatorRef allocator,
	const char* cStr, CFStringEncoding encoding);

CFIndex CFStringGetLength(CFStringRef theString);

CFIndex CFStringGetMaximumSizeForEncoding(CFIndex length,
	CFStringEncoding encoding);

Boolean CFStringGetCString(CFStringRef theString, char* buffer,
	CFIndex bufferSize, CFStringEncoding encoding);

CFArrayRef CFArrayCreate(CFAllocatorRef allocator, const void** values,
	CFIndex numValues, const CFArrayCallBacks* callBacks);

const void* CFArrayGetValueAtIndex(CFArrayRef theArray, CFIndex idx);

const void* CFDictionaryGetValue(CFDictionaryRef theDict, const void* key);

void CFRelease(CFTypeRef cf);

typedef struct __FSEventStream* FSEventStreamRef;
typedef const struct __FSEventStream* ConstFSEventStreamRef;

typedef UInt64 FSEventStreamEventId;
typedef UInt32 FSEventStreamCreateFlags;
typedef UInt32 FSEventStreamEventFlags;

typedef struct FSEventStreamContext
{
	CFIndex version;
	void* info;
	const void* (*retain)(const void* info);
	void (*release)(const void* info);
	CFStringRef (*copyDescription)(const void* info);
} FSEventStreamContext;

typedef void (*FSEventStreamCallback)(ConstFSEventStreamRef streamRef,
	void* clientCallBackInfo, size_t numEvents, void* eventPaths,
	const FSEventStreamEventFlags eventFlags[],
	const FSEventStreamEventId eventIds[]);

FSEventStreamRef FSEventStreamCreate(CFAllocatorRef allocator,
	FSEventStreamCallback callback, FSEventStreamContext* context,
	CFArrayRef pathsToWatch, FSEventStreamEventId sinceWhen,
	CFTimeInterval latency, FSEventStreamCreateFlags flags);

Boolean FSEventStreamStart(FSEventStreamRef streamRef);

void FSEventStreamStop(FSEventStreamRef streamRef);

void FSEventStreamInvalidate(FSEventStreamRef streamRef);

void FSEventStreamRelease(FSEventStreamRef streamRef);

void FSEventStreamSetDispatchQueue(FSEventStreamRef streamRef,
	dispatch_queue_t q);

enum
{
	kFSEventStreamCreateFlagNone = 0x00000000,
	kFSEventStreamCreateFlagUseCFTypes = 0x00000001,
	kFSEventStreamCreateFlagNoDefer = 0x00000002,
	kFSEventStreamCreateFlagWatchRoot = 0x00000004,
	kFSEventStreamCreateFlagIgnoreSelf = 0x00000008,
	kFSEventStreamCreateFlagFileEvents = 0x00000010,
	kFSEventStreamCreateFlagMarkSelf = 0x00000020,
	kFSEventStreamCreateFlagUseExtendedData = 0x00000040,

	kFSEventStreamEventFlagItemCreated = 0x00000100,
	kFSEventStreamEventFlagItemRemoved = 0x00000200,
	kFSEventStreamEventFlagItemInodeMetaMod = 0x00000400,
	kFSEventStreamEventFlagItemRenamed = 0x00000800,
	kFSEventStreamEventFlagItemModified = 0x00001000
};

#define kFSEventStreamEventIdSinceNow ((FSEventStreamEventId)0xFFFFFFFFFFFFFFFFULL)

extern const CFStringRef kFSEventStreamEventExtendedDataPathKey;

#endif /* WATCHMAN_STUB_CORESERVICES_H */
