// 16-byte aligned allocation for bgfx's default allocator on 32-bit Windows (see x86/x86_aligned_alloc.h). Empty elsewhere.
#if defined(_WIN32) && !defined(_WIN64)
#include <malloc.h>
#include <stddef.h>

extern "C" {
void* nue_gfx_malloc16(size_t size) { return _aligned_malloc(size, 16); }
void* nue_gfx_realloc16(void* pointer, size_t size) { return _aligned_realloc(pointer, size, 16); }
void nue_gfx_free16(void* pointer) { _aligned_free(pointer); }
}
#else
namespace nue_gfx_no_x86_alloc {}
#endif
