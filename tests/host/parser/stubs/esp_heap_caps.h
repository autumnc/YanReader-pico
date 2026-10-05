#pragma once
// 主机端桩：把 heap_caps_* 直接落到 malloc/free。
#include <cstdlib>
#include <cstddef>
#define MALLOC_CAP_SPIRAM 0
#define MALLOC_CAP_DMA 0
#define MALLOC_CAP_INTERNAL 0
#define MALLOC_CAP_8BIT 0
static inline void* heap_caps_malloc(size_t n, int) { return std::malloc(n); }
static inline void* heap_caps_calloc(size_t n, size_t sz, int) { return std::calloc(n, sz); }
static inline void* heap_caps_realloc(void* p, size_t n, int) { return std::realloc(p, n); }
static inline void* heap_caps_aligned_alloc(size_t align, size_t n, int) {
  void* p = nullptr;
  if (posix_memalign(&p, align, n) != 0) return nullptr;
  return p;
}
static inline void heap_caps_free(void* p) { std::free(p); }
