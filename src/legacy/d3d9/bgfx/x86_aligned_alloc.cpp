// See dll/x86_aligned_alloc.h: 16-byte aligned allocation for bgfx's default allocator on 32-bit x86.
#include <malloc.h>
#include <stddef.h>

extern "C" {
void* nue_malloc16(size_t size) { return _aligned_malloc(size, 16); }
void* nue_realloc16(void* pointer, size_t size) { return _aligned_realloc(pointer, size, 16); }
void nue_free16(void* pointer) { _aligned_free(pointer); }
}
