// 主机侧独立的 stb_image 实现单元。
//
// 设备上那份是 StbImageImpl.cpp，它把 stb 的分配钉在 PSRAM（STBI_MALLOC →
// heap_caps_malloc(... MALLOC_CAP_SPIRAM ...)）并且 #include <esp_heap_caps.h>，
// **主机上编不过**。STB_IMAGE_IMPLEMENTATION 又只能出现在一个 TU 里，所以这里
// 另立一份、用默认 malloc。宏配置要与设备那份保持一致（STBI_ONLY_JPEG /
// STBI_NO_STDIO / STBI_NO_LINEAR），否则参考解出来的像素跟设备不是同一套。
//
// 两份共用同一个 stb_image.h（v2.30），所以版本不会飘。
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#include "stb_image.h"
