#pragma once
// ── PDF 大缓冲的 PSRAM 分配 ───────────────────────────────────────────────
// 设备上空闲的内部 RAM 只有不到 10 KB（任务栈/Bluedroid/WiFi 已经占满），
// 而 PDF 的 xref 表、xref/内容/对象流解压缓冲、predictor 行、图像行都是
// 几 KB 到几 MB 的一次性缓冲。这些一律从 PSRAM 拿；PSRAM 耗尽时退回普通
// 堆（主机/模拟器上就只剩普通堆）。
//
// 用法：
//   pdfscratch::ByteVec       data(n);          // std::vector<uint8_t>
//   pdfscratch::Vec<int>      nums(n);
// 或者需要裸缓冲时 pdfscratch::alloc(n) / pdfscratch::release(p)。

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <vector>

#if !defined(SIMULATOR) && !defined(CROSSPOINT_EMULATED) && __has_include(<esp_heap_caps.h>)
#include <esp_heap_caps.h>
#define PDF_SCRATCH_HAS_HEAP_CAPS 1
#endif

namespace pdfscratch {

// 可分配性探测（主机上恒为「够」）：调用方在申请大块前先问一句，
// 就能把 OOM 变成可显示的「内存不足」而不是 abort。
inline size_t largestFree() {
#if defined(PDF_SCRATCH_HAS_HEAP_CAPS)
  return heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
  return static_cast<size_t>(-1);
#endif
}

inline size_t freeBytes() {
#if defined(PDF_SCRATCH_HAS_HEAP_CAPS)
  return heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
  return static_cast<size_t>(-1);
#endif
}

inline void *alloc(size_t n) {
  if (n == 0) n = 1;
#if defined(PDF_SCRATCH_HAS_HEAP_CAPS)
  if (void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)) return p;
#endif
  if (void *p = std::malloc(n)) return p;
  // 连 PSRAM 都拿不到时没有可用的降级路径；保持和默认分配器一致的终止行为。
  std::abort();
}

inline void release(void *p) { std::free(p); }

template <typename T>
struct Allocator {
  using value_type = T;
  Allocator() noexcept = default;
  template <typename U>
  Allocator(const Allocator<U> &) noexcept {}

  T *allocate(std::size_t n) { return static_cast<T *>(alloc(n * sizeof(T))); }
  void deallocate(T *p, std::size_t) noexcept { release(p); }

  template <typename U>
  bool operator==(const Allocator<U> &) const noexcept {
    return true;
  }
  template <typename U>
  bool operator!=(const Allocator<U> &) const noexcept {
    return false;
  }
};

template <typename T>
using Vec = std::vector<T, Allocator<T>>;
using ByteVec = Vec<uint8_t>;

// 真正把 vector 的缓冲还给堆。**不要用 shrink_to_fit()**：ESP-IDF 用
// -fno-exceptions 编译，libstdc++ 15 的 __shrink_to_fit_aux 在无异常配置下
// 直接 return false（alloc_traits.h），clear()+shrink_to_fit() 是空操作，
// 大页解码后在 PSRAM 里留下整块的僵尸缓冲。
template <typename V>
inline void releaseVec(V &v) {
  V tmp;
  v.swap(tmp);
}

}  // namespace pdfscratch

#undef PDF_SCRATCH_HAS_HEAP_CAPS
