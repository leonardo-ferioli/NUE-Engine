// 32-bit x86 only. malloc of the 32-bit C runtime returns 8-byte aligned memory, but bgfx allocates objects that contain 16-byte aligned members
// with plain bx::alloc (BX_NEW ignores alignof), and the compiler then uses aligned SSE stores on them: a crash in the constructor of the renderer.
// Force-included into bx's allocator.cpp only: its malloc/realloc/free become 16-byte aligned versions (defined in bgfx/x86_aligned_alloc.cpp).
#pragma once
#include <stdlib.h>
#ifdef __cplusplus
extern "C" {
#endif
void* nue_malloc16(size_t size);
void* nue_realloc16(void* pointer, size_t size);
void nue_free16(void* pointer);
#ifdef __cplusplus
}
#endif
// object-like on purpose: bx's own allocator classes have members named realloc/free, which are renamed consistently inside this one translation unit
#define malloc nue_malloc16
#define realloc nue_realloc16
#define free nue_free16
