#pragma once
#include <cstdint>

// crossmux 风格的文件打开标志（与 SdFat 的 oflag_t 语义一致）。
//
// 【为什么叫 FS_* 而不是 O_RDONLY/O_RDWR】
// 上游用的是 SdFat/POSIX 那套名字，但这个名字在 ESP-IDF 上会被**宏**遮蔽，且遮蔽发生在
// 单个 TU 内、后定义者胜：#include <sys/socket.h>（WiFi/HTTP 链）会拉到两条定义
//   - picolibc  <sys/fcntl.h>    : O_RDONLY 0 / O_WRONLY 1 / O_RDWR 2     （POSIX 值）
//   - lwIP      <lwip/sockets.h> : O_RDONLY 2 / O_WRONLY 4 / O_RDWR 6
// 只要本头在它们之前被 include（HalStorage.h → FsApiConstants.h 通常就是最早的那批），
// 宏就盖住下面的常量，调用点写 `Storage.open(p, O_RDWR)` 传出去的其实是 2 或 6：
//   - 2 → HalStorage 按本项目语义读成 O_WRONLY → fopen("wb") → **把文件截成 0**
//     （微信读书整本缓存：encoded.part 748 字节 → 0 → 数据校验失败 Error::Integrity）
//   - 6 → 恰好 rd/wr 都成立，蒙对成 "r+b" —— 纯属巧合，不能依赖
// 头文件里 #undef 挡不住"更晚 include 的系统头"，宏又会穿透命名空间（`ns::O_RDWR` 同样被
// 展开）。唯一结构性免疫的办法就是换个不可能撞名的标识符：任何残存的 O_* 用法会直接
// **编译报错**，而不是悄悄把文件截断。
typedef uint8_t oflag_t;

static constexpr oflag_t FS_RDONLY = 0x01;
static constexpr oflag_t FS_WRONLY = 0x02;
static constexpr oflag_t FS_RDWR   = 0x04;
static constexpr oflag_t FS_APPEND = 0x08;
static constexpr oflag_t FS_CREAT  = 0x10;
static constexpr oflag_t FS_EXCL   = 0x20;
static constexpr oflag_t FS_TRUNC  = 0x40;
