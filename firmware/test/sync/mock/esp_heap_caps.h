#pragma once
#include <cstdlib>
#include <cstddef>
#define MALLOC_CAP_INTERNAL (1 << 11)
#define MALLOC_CAP_SPIRAM   (1 << 10)
#define MALLOC_CAP_8BIT     (1 << 2)
// The simulation sets these to model a healthy or a broken-up internal heap.
extern size_t g_fake_internal_free, g_fake_internal_block;
inline void* heap_caps_malloc(size_t n, unsigned) { return malloc(n); }
inline void* heap_caps_realloc(void* p, size_t n, unsigned) { return realloc(p, n); }
inline void heap_caps_free(void* p) { free(p); }
inline size_t heap_caps_get_free_size(unsigned caps) { return caps & MALLOC_CAP_SPIRAM ? 2000000 : g_fake_internal_free; }
inline size_t heap_caps_get_largest_free_block(unsigned) { return g_fake_internal_block; }
inline size_t heap_caps_get_minimum_free_size(unsigned) { return g_fake_internal_free; }
