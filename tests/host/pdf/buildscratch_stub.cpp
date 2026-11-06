// Host stub for the framebuffer-loan pool: on the host there is no lent
// framebuffer, so claim() always returns nullptr and InflateStream falls back
// to the heap. Keeps the probe free of Arduino/esp_heap_caps dependencies.
#include <BuildScratch.h>

namespace buildscratch {

void lend(uint8_t *, size_t) {}
void reclaim() {}
uint8_t *claim(size_t, size_t *) { return nullptr; }
void release(const uint8_t *) {}

}  // namespace buildscratch
