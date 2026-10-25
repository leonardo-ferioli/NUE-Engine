// 32-bit Windows x86 only. malloc of the 32-bit C runtime returns 8-byte aligned memory, but bgfx allocates objects that contain 16-byte aligned members with
// plain bx::alloc (BX_NEW ignores alignof), and the compiler then uses aligned SSE stores on them: a crash in the constructor of the renderer.
// Force-included (-include) into bx's allocator.cpp only by cmake/NueGfx.cmake: its malloc/realloc/free become 16-byte aligned versions (x86_aligned_alloc.cpp).
// Same workaround as src/legacy/d3d9/dll/x86_aligned_alloc.h (legacy-d3d9 session) with different symbol names, so both can exist in one link.
#pragma once
#include <stdlib.h>
#ifdef __cplusplus
extern "C" {
#endif
void* nue_gfx_malloc16(size_t size);
void* nue_gfx_realloc16(void* pointer, size_t size);
void nue_gfx_free16(void* pointer);
#ifdef __cplusplus
}
#endif
// object-like on purpose: bx's own allocator classes have members named realloc/free, which are renamed consistently inside this one translation unit
#define malloc nue_gfx_malloc16
#define realloc nue_gfx_realloc16
#define free nue_gfx_free16
