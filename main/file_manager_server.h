#pragma once

#include <cstdint>
#include <atomic>

bool file_manager_server_start(uint16_t port = 80);
void file_manager_server_stop();
uint16_t file_manager_server_get_port();

// ── 传输进度（给界面显示用）─────────────────────────────────────────────
// 上传/下载都在 httpd 自己的任务里跑，主任务这边屏幕一动不动，传大文件时看着像死机。
// 这里暴露一份只读快照给界面轮询，用来画"正在接收 xxx 1.2/8.4MB"。
//
// 并发约定：httpd 任务写、主任务读，**不加锁**。active 用 release/acquire 发布
// name，进度字段用 relaxed 轮询；最坏情况是进度落后一拍，无所谓。
struct FmXfer {
    std::atomic<int> active{0};      // 0 = 空闲，1 = 传输中
    std::atomic<int> kind{0};        // 0 = 上传(手机→本机)，1 = 下载(本机→手机)，2 = 打包下载
    std::atomic<uint32_t> done{0};   // 已完成字节数
    std::atomic<uint32_t> total{0};  // 总字节数；0 = 未知（打包下载算不出）
    char name[64];            // 文件名（仅 basename）
};

// 返回不变的空闲快照或当前进度快照。永远非空。
const FmXfer *file_manager_get_xfer();
