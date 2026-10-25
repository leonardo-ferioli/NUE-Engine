/*
 * NUE engine - base definitions: export macros, platform detection, version, result codes, handles,
 * struct-size convention, allocator hooks and small shared value types.
 *
 * Conventions used by every NUE header (see docs/API.md for the long form):
 *   - Pure C99 header, usable from C++; prefix nue_ for functions/types, NUE_ for macros/enums.
 *   - Calling convention is cdecl on every platform (NUE_CALL). 32-bit and 64-bit builds expose the same API.
 *   - Fallible functions return nue_result: NUE_OK (0) or > 0 for success-with-information, < 0 on error.
 *     After a failure nue_get_last_error_message() (core.h) returns a human-readable detail for the calling thread.
 *   - No exceptions cross the ABI. Callbacks must not throw/longjmp out into the engine.
 *   - Strings are UTF-8, NUL terminated. Output strings go into caller buffers (see NUE_STRING_OUT below).
 *   - Every struct passed across the ABI starts with `uint32_t struct_size`. Callers set it to sizeof(the struct
 *     they compiled against) (NUE_STRUCT_INIT); the engine reads/writes only the bytes that fit inside struct_size.
 *     New fields are only ever appended. Reserved fields must be zero.
 *   - Handles are opaque. Pointer handles (nue_window, nue_mixer...) are NULL when invalid. Handles that a
 *     recompiled game may need to expose as 32-bit values (files, voices) are 32-bit ids (index + generation), 0 = invalid.
 *   - Thread rules are written next to each group of functions with the tags:
 *       [any]    callable from any thread, thread safe
 *       [init]   only between nue_init and nue_shutdown, not concurrently with other [init] calls
 *       [event]  must be called from the event thread (the thread that pumps events, see app.h)
 *       [audio]  runs on an audio thread: no blocking, no allocation, no engine calls other than the documented ones
 */
#ifndef NUE_BASE_H
#define NUE_BASE_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------------------------------ platform */

#if defined(_WIN32)
#  define NUE_PLATFORM_WINDOWS 1
#elif defined(__ANDROID__)
#  define NUE_PLATFORM_ANDROID 1
#elif defined(__APPLE__)
#  include <TargetConditionals.h>
#  if TARGET_OS_IPHONE
#    define NUE_PLATFORM_IOS 1
#  else
#    define NUE_PLATFORM_MACOS 1
#  endif
#elif defined(__linux__)
#  define NUE_PLATFORM_LINUX 1
#else
#  error "NUE: unsupported platform"
#endif

#if defined(_M_X64) || defined(__x86_64__)
#  define NUE_ARCH_X64 1
#elif defined(_M_IX86) || defined(__i386__)
#  define NUE_ARCH_X86 1
#elif defined(_M_ARM64) || defined(__aarch64__)
#  define NUE_ARCH_ARM64 1
#elif defined(_M_ARM) || defined(__arm__)
#  define NUE_ARCH_ARM32 1
#else
#  define NUE_ARCH_UNKNOWN 1
#endif

#define NUE_POINTER_SIZE ((int)sizeof(void*))

/* ------------------------------------------------------------------------------------------------ linkage */

#if defined(_WIN32)
#  define NUE_CALL __cdecl
#  if defined(NUE_BUILD_SHARED)
#    define NUE_API __declspec(dllexport)
#  elif defined(NUE_USE_SHARED)
#    define NUE_API __declspec(dllimport)
#  else
#    define NUE_API
#  endif
#else
#  define NUE_CALL
#  if defined(NUE_BUILD_SHARED) || defined(NUE_USE_SHARED)
#    define NUE_API __attribute__((visibility("default")))
#  else
#    define NUE_API
#  endif
#endif

/* printf-style checking */
#if defined(__GNUC__) || defined(__clang__)
#  define NUE_PRINTF_LIKE(fmt_index, first_arg) __attribute__((format(printf, fmt_index, first_arg)))
#else
#  define NUE_PRINTF_LIKE(fmt_index, first_arg)
#endif

/* ------------------------------------------------------------------------------------------------ version */

#define NUE_MAKE_VERSION(major, minor, patch) \
    ((((uint32_t)(major)) << 24) | (((uint32_t)(minor)) << 12) | ((uint32_t)(patch)))
#define NUE_VERSION_MAJOR 0
#define NUE_VERSION_MINOR 1
#define NUE_VERSION_PATCH 3
/* Version of this header set. Passed to nue_init() so the library can refuse an incompatible caller. */
#define NUE_API_VERSION NUE_MAKE_VERSION(NUE_VERSION_MAJOR, NUE_VERSION_MINOR, NUE_VERSION_PATCH)
#define NUE_VERSION_GET_MAJOR(v) (((uint32_t)(v) >> 24) & 0xFFu)
#define NUE_VERSION_GET_MINOR(v) (((uint32_t)(v) >> 12) & 0xFFFu)
#define NUE_VERSION_GET_PATCH(v) ((uint32_t)(v) & 0xFFFu)

/* ------------------------------------------------------------------------------------------------ basic types */

typedef uint8_t nue_bool;
#define NUE_FALSE ((nue_bool)0)
#define NUE_TRUE  ((nue_bool)1)

typedef int32_t nue_result;

/* Success is >= 0, failure < 0. nue_result_name() (core.h) maps a code to its identifier. */
enum {
    NUE_OK                      = 0,
    NUE_OK_PENDING              = 1,   /* started; completion is reported later (event or callback) */
    NUE_OK_PARTIAL              = 2,   /* done, but less than requested (short read/write, fewer items listed) */
    NUE_OK_NOOP                 = 3,   /* nothing to do (already in the requested state) */

    NUE_ERR_FAILED              = -1,  /* generic failure; see nue_get_last_error_message() */
    NUE_ERR_INVALID_ARGUMENT    = -2,
    NUE_ERR_INVALID_HANDLE      = -3,  /* handle is NULL, stale (generation mismatch) or of the wrong kind */
    NUE_ERR_NOT_INITIALIZED     = -4,
    NUE_ERR_ALREADY_INITIALIZED = -5,
    NUE_ERR_UNSUPPORTED         = -6,  /* not available on this platform/backend (query capabilities first) */
    NUE_ERR_OUT_OF_MEMORY       = -7,
    NUE_ERR_NOT_FOUND           = -8,
    NUE_ERR_ALREADY_EXISTS      = -9,
    NUE_ERR_ACCESS_DENIED       = -10,
    NUE_ERR_IO                  = -11,
    NUE_ERR_TIMEOUT             = -12,
    NUE_ERR_BUFFER_TOO_SMALL    = -13, /* output buffer too small; the needed size is returned through an out parameter */
    NUE_ERR_WRONG_THREAD        = -14,
    NUE_ERR_BAD_STATE           = -15, /* object not in a state where the call is legal */
    NUE_ERR_VERSION_MISMATCH    = -16,
    NUE_ERR_DEVICE_LOST         = -17,
    NUE_ERR_CANCELLED           = -18,
    NUE_ERR_END_OF_DATA         = -19, /* end of file/stream reached with nothing read */
    NUE_ERR_BUSY                = -20,
    NUE_ERR_WOULD_BLOCK         = -21,
    NUE_ERR_CORRUPT             = -22, /* integrity check failed, malformed data */
    NUE_ERR_BACKEND             = -23, /* the OS/SDL/bgfx call failed; detail in the last-error message */
    NUE_ERR_OVERFLOW            = -24  /* value or queue overflow */
};

#define NUE_SUCCEEDED(r) ((r) >= 0)
#define NUE_FAILED(r)    ((r) < 0)

/* Opaque pointer handle: NUE_DEFINE_HANDLE(nue_window) -> typedef struct nue_window_s* nue_window; */
#define NUE_DEFINE_HANDLE(name) typedef struct name##_s* name

/* Struct-size convention. Use on every struct whose first member is `uint32_t struct_size`. */
#define NUE_STRUCT_INIT(ptr) \
    do { memset((ptr), 0, sizeof(*(ptr))); *(uint32_t*)(void*)(ptr) = (uint32_t)sizeof(*(ptr)); } while (0)
/* True when the struct the caller passed is large enough to contain `field` of `Type` (used inside the engine). */
#define NUE_STRUCT_HAS(ptr, Type, field) \
    ((size_t)(ptr)->struct_size >= offsetof(Type, field) + sizeof(((Type*)0)->field))

/* Output string convention (NUE_STRING_OUT): functions that return text take `char* buffer, size_t capacity`
 * and, where noted, `size_t* out_size`. The text is always NUL terminated when capacity > 0 (truncated if needed).
 * out_size receives the length needed INCLUDING the terminator. With buffer == NULL or capacity == 0 only out_size is
 * written (size query). NUE_ERR_BUFFER_TOO_SMALL is returned only by functions that cannot truncate (binary data). */

typedef struct nue_point_i { int32_t x, y; } nue_point_i;
typedef struct nue_size_i  { int32_t w, h; } nue_size_i;
typedef struct nue_rect_i  { int32_t x, y, w, h; } nue_rect_i;
typedef struct nue_rect_f  { float x, y, w, h; } nue_rect_f;
typedef struct nue_vec3    { float x, y, z; } nue_vec3;
typedef struct nue_guid    { uint8_t data[16]; } nue_guid;

/* Native OS handle of a window/surface, for attaching to a window the host (a recompiled game) already owns and for
 * handing the surface to a renderer. 64-bit wide so the layout is identical in 32- and 64-bit builds. */
typedef enum nue_native_kind {
    NUE_NATIVE_NONE = 0,
    NUE_NATIVE_WIN32_HWND,          /* window = HWND */
    NUE_NATIVE_X11_WINDOW,          /* window = X11 Window id, display = Display* */
    NUE_NATIVE_WAYLAND_SURFACE,     /* window = wl_surface*, display = wl_display* */
    NUE_NATIVE_ANDROID_WINDOW,      /* window = ANativeWindow* */
    NUE_NATIVE_UIKIT_WINDOW,        /* window = UIWindow*, extra = CAMetalLayer* (when the window was created for Metal) */
    NUE_NATIVE_COCOA_WINDOW         /* window = NSWindow*, extra = CAMetalLayer* */
} nue_native_kind;

typedef struct nue_native_window {
    uint32_t struct_size;
    nue_native_kind kind;
    uint64_t window;                /* uintptr_t value, zero-extended */
    uint64_t display;
    uint64_t extra;
} nue_native_window;

/* ------------------------------------------------------------------------------------------------ allocator */

/* Optional allocator hooks, passed to nue_init() (core.h). When set, every allocation made by the engine goes through
 * them; they must be thread safe. realloc(NULL, n) acts as alloc; realloc(p, 0) may free. Default: the C runtime. */
typedef struct nue_allocator {
    uint32_t struct_size;
    void* userdata;
    void* (NUE_CALL *alloc)(void* userdata, size_t size, size_t alignment);   /* alignment is a power of two, >= 1 */
    void* (NUE_CALL *realloc)(void* userdata, void* ptr, size_t old_size, size_t new_size, size_t alignment);
    void  (NUE_CALL *free)(void* userdata, void* ptr, size_t size);
} nue_allocator;

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* NUE_BASE_H */
