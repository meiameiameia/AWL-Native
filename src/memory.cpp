#include "awl/memory.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#ifdef _WIN32
#include <malloc.h>
#endif

/* Simple heap-based memory management */
/* In the full port, this will manage a pre-allocated arena like the GameCube */

static u8*  s_arena_base = nullptr;
static u8*  s_arena_lo = nullptr;
static u8*  s_arena_hi = nullptr;
static u32  s_arena_size = 0;
static u32  s_allocated = 0;

void awl_memory_init(void) {
    if (s_arena_base) {
        std::free(s_arena_base);
    }
    /* Default: 64 MB arena (GameCube has 24 MB MEM1 + 16 MB MEM2 = 40 MB total) */
    s_arena_size = 64 * 1024 * 1024;
    s_arena_base = (u8*)std::malloc(s_arena_size);
    s_arena_lo = s_arena_base;
    s_arena_hi = s_arena_base ? s_arena_base + s_arena_size : nullptr;
    s_allocated = 0;
}

void awl_memory_shutdown(void) {
    if (s_arena_base) {
        std::free(s_arena_base);
        s_arena_base = nullptr;
        s_arena_lo = nullptr;
        s_arena_hi = nullptr;
    }
    s_arena_size = 0;
    s_allocated = 0;
}

/*
 * NOTE: Current Memory Limitations
 * - awl_malloc() uses system malloc and only approximates allocation tracking.
 * - awl_free() does not currently decrement s_allocated.
 * - _aligned_malloc() allocations must be freed with awl_aligned_free(), not awl_free().
 */

void* awl_malloc(u32 size) {
    void* allocation = std::malloc(size);
    if (allocation) {
        if (size > s_arena_size - std::min(s_allocated, s_arena_size)) {
            s_allocated = s_arena_size;
        } else {
            s_allocated += size;
        }
    }
    return allocation;
}

void awl_free(void* ptr) {
    // Note: s_allocated is not decremented here currently.
    std::free(ptr);
}

void* awl_memalign(u32 alignment, u32 size) {
    #ifdef _WIN32
    return _aligned_malloc(size, alignment);
    #else
    void* ptr = nullptr;
    posix_memalign(&ptr, alignment, size);
    return ptr;
    #endif
}

void awl_aligned_free(void* ptr) {
    #ifdef _WIN32
    _aligned_free(ptr);
    #else
    std::free(ptr);
    #endif
}

u32 awl_memory_free(void) {
    return s_allocated < s_arena_size ? s_arena_size - s_allocated : 0;
}

void* awl_arena_lo(void) {
    return s_arena_lo;
}

void* awl_arena_hi(void) {
    return s_arena_hi;
}

void awl_set_arena_lo(void* lo) {
    s_arena_lo = (u8*)lo;
}

void awl_set_arena_hi(void* hi) {
    s_arena_hi = (u8*)hi;
}
