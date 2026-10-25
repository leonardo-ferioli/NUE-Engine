// Minimal stand-ins for the Apple SDK, ONLY to syntax-check platform_ios.mm against the real NUE headers on a Windows host.
// The signatures mirror what the code uses (from memory of the SDK): this is a typo / type check, NOT a proof of SDK compatibility.
#pragma once
#include <stddef.h>
#include <stdint.h>

#define NS_ASSUME_NONNULL_BEGIN
#define NS_ASSUME_NONNULL_END
typedef signed char BOOL;
#define YES ((BOOL)1)
#define NO ((BOOL)0)
typedef long NSInteger;
typedef unsigned long NSUInteger;
typedef unsigned long NSSearchPathDirectory;
enum { NSDocumentDirectory = 9, NSApplicationSupportDirectory = 14, NSCachesDirectory = 13 };
typedef unsigned long NSSearchPathDomainMask;
enum { NSUserDomainMask = 1 };
typedef NSInteger NSProcessInfoThermalState;

@class NSString, NSError, NSURL, NSNumber;
typedef NSString* NSURLResourceKey;
extern NSURLResourceKey NSURLIsRegularFileKey;
extern NSURLResourceKey NSURLFileSizeKey;

typedef struct { unsigned long state; id __unsafe_unretained* itemsPtr; unsigned long* mutationsPtr; unsigned long extra[5]; } NSFastEnumerationState;
@protocol NSFastEnumeration
- (NSUInteger)countByEnumeratingWithState:(NSFastEnumerationState*)state objects:(id __unsafe_unretained [])buffer count:(NSUInteger)len;
@end

__attribute__((objc_root_class)) @interface NSObject
+ (instancetype)new;
@end
@interface NSString : NSObject
+ (instancetype)stringWithUTF8String:(const char*)s;
- (const char*)UTF8String;
@property(readonly) NSUInteger length;
- (NSString*)substringFromIndex:(NSUInteger)i;
@end
@interface NSNumber : NSObject
- (BOOL)boolValue;
- (unsigned long long)unsignedLongLongValue;
@end
@interface NSArray<ObjectType> : NSObject <NSFastEnumeration>
+ (instancetype)arrayWithObjects:(const ObjectType*)o count:(NSUInteger)c;
@property(readonly, nullable) ObjectType firstObject;
@end
@interface NSError : NSObject
@end
@interface NSURL : NSObject
+ (instancetype)URLWithString:(NSString*)s;
+ (instancetype)fileURLWithPath:(NSString*)p isDirectory:(BOOL)d;
@property(readonly) BOOL isFileURL;
@property(readonly, copy) NSString* path;
@property(readonly, copy) NSString* absoluteString;
- (BOOL)startAccessingSecurityScopedResource;
- (void)stopAccessingSecurityScopedResource;
- (NSURL*)URLByResolvingSymlinksInPath;
- (BOOL)getResourceValue:(id _Nullable*)value forKey:(NSURLResourceKey)key error:(NSError* _Nullable*)error;
@end
@interface NSEnumerator<ObjectType> : NSObject <NSFastEnumeration>
@end
@interface NSDirectoryEnumerator<ObjectType> : NSEnumerator<ObjectType>
@end
@interface NSFileManager : NSObject
+ (NSFileManager*)defaultManager;
- (BOOL)createDirectoryAtPath:(NSString*)p withIntermediateDirectories:(BOOL)i attributes:(id _Nullable)a error:(NSError* _Nullable*)e;
- (BOOL)fileExistsAtPath:(NSString*)p isDirectory:(BOOL*)d;
- (NSDirectoryEnumerator<NSURL*>*)enumeratorAtURL:(NSURL*)url includingPropertiesForKeys:(NSArray<NSURLResourceKey>*)keys options:(NSUInteger)o
                                     errorHandler:(BOOL (^)(NSURL*, NSError*))h;
@end
@interface NSThread : NSObject
+ (BOOL)isMainThread;
@end
@interface NSProcessInfo : NSObject
+ (NSProcessInfo*)processInfo;
@property(readonly) NSProcessInfoThermalState thermalState;
@property(readonly, getter=isLowPowerModeEnabled) BOOL lowPowerModeEnabled;
@end
extern NSArray<NSString*>* NSSearchPathForDirectoriesInDomains(NSSearchPathDirectory d, NSSearchPathDomainMask m, BOOL expand);
extern NSString* NSTemporaryDirectory(void);


// UIKit
typedef NSInteger UIDeviceBatteryState;
@interface UIDevice : NSObject
+ (UIDevice*)currentDevice;
@property(nonatomic, getter=isBatteryMonitoringEnabled) BOOL batteryMonitoringEnabled;
@property(readonly) UIDeviceBatteryState batteryState;
@property(readonly) float batteryLevel;
@end

// libdispatch
typedef void (^dispatch_block_t)(void);
typedef struct dispatch_queue_s* dispatch_queue_t;
extern dispatch_queue_t dispatch_get_main_queue(void);
extern void dispatch_async(dispatch_queue_t q, dispatch_block_t b);
extern void dispatch_sync(dispatch_queue_t q, dispatch_block_t b);

// os/log
typedef struct os_log_s* os_log_t;
typedef uint8_t os_log_type_t;
extern os_log_t os_log_create(const char* subsystem, const char* category);
extern void os_log_with_type(os_log_t log, os_log_type_t type, const char* fmt, ...);

// pthread / QoS
typedef struct _opaque_pthread_t* pthread_t;
typedef enum { QOS_CLASS_USER_INTERACTIVE = 0x21, QOS_CLASS_USER_INITIATED = 0x19, QOS_CLASS_DEFAULT = 0x15, QOS_CLASS_UTILITY = 0x11, QOS_CLASS_BACKGROUND = 0x09, QOS_CLASS_UNSPECIFIED = 0 } qos_class_t;
extern int pthread_set_qos_class_self_np(qos_class_t c, int rel);
extern int pthread_get_qos_class_np(pthread_t t, qos_class_t* c, int* rel);
extern int pthread_threadid_np(pthread_t t, uint64_t* id);
extern int pthread_setname_np(const char* name);
extern pthread_t pthread_self(void);
struct sched_param { int sched_priority; };
#define SCHED_OTHER 1
extern int pthread_setschedparam(pthread_t t, int policy, const struct sched_param* p);

// execinfo / unistd / fcntl
extern int backtrace(void** buf, int size);
extern void backtrace_symbols_fd(void* const* buf, int size, int fd);
extern int open(const char* path, int flags, ...);
extern long write(int fd, const void* b, size_t n);
extern int fsync(int fd);
extern int close(int fd);
#define O_WRONLY 1
#define O_CREAT 0x200
#define O_TRUNC 0x400

// signals
#define SIGILL 4
#define SIGTRAP 5
#define SIGABRT 6
#define SIGFPE 8
#define SIGBUS 10
#define SIGSEGV 11
#define SIGSYS 12
#define SA_SIGINFO 0x40
#define SA_ONSTACK 0x1
typedef struct { void* ss_sp; size_t ss_size; int ss_flags; } stack_t;
typedef struct { int si_signo; int si_code; void* si_addr; } siginfo_t;
typedef unsigned int sigset_t;
struct sigaction { void (*sa_sigaction)(int, siginfo_t*, void*); sigset_t sa_mask; int sa_flags; };
extern int sigaction(int, const struct sigaction*, struct sigaction*);
extern int sigaltstack(const stack_t*, stack_t*);
extern int sigemptyset(sigset_t*);
extern int raise(int);
struct __darwin_x86_thread_state64 { uint64_t __rip; uint64_t __pc; };
struct __darwin_mcontext { struct __darwin_x86_thread_state64 __ss; };
typedef struct { struct __darwin_mcontext* uc_mcontext; } ucontext_t;
#define nil nullptr
extern int setenv(const char*, const char*, int);
