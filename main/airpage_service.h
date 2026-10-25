#pragma once

#include <cstdint>
#include <string>

#include "crossstore/CrossStoreRuntime.h"

// AirPage 的原生服务：Lua 界面（CrossStore 的 airpage 包）只画二维码/按钮/历史列表，
// 设备 ID、上传地址、HTTPS 拉图、BMP/JPEG 解码、历史归档和壁纸落盘都在这里。
//
// 接口形状完全对齐 airpage 0.3.0 的 main.lua：
//   airpage.start / status / url / history / show / hide / page / fetch / live / wallpaper
// 语义按 CrossStore 的 native 参考实现（packages/airpage/native）移植，差别只有：
//   - 没有 MQTT。实时接收退化成"应用开着时按 20s 轮询"，live=on 时打开应用立刻拉一次；
//   - 历史/当前图统一存成 BMP（JPEG 下载后先转 Gray8 BMP），显示走 readerCoverScaleTo；
//   - 壁纸复制成 /AirPage/wallpaper.bmp 再交给 readerSetStandbyBmp，之后的新图不会
//     把已经设好的壁纸偷偷换掉。
//
// 图片目录固定 /AirPage（与 CrossStore 参考实现一致，落到 SD 根下）；设备 ID 和模式
// 位沿用上游的隐藏目录 /.crosspoint。
namespace airpage_service {

struct Grants {
  bool network = false;
  bool device_info = false;
  bool wallpaper = false;
  bool keep_awake = false;
};

// action 是命令里 "airpage." 之后的那一段（"status"、"show"……）。
// 返回值即 Lua 侧 app.system(...) 的第一个返回值。
bool system(const char *action, const char *argument, int64_t index,
            crossstore::ServiceReply &reply, const Grants &grants);

// ── 宿主每拍调一次（在派发 Lua 回调**之前**）──────────────────────────────
// 收下载结果、重建历史页、按 20s 节奏起轮询/拉图。
// 这些动作**不能**放在 airpage.status 里做：CrossStore 给单个 Lua 回调的宿主 I/O
// 预算只有 1.5s，而新建拉图任务会连带第一次 WiFi 初始化（高优先级任务把主任务顶住
// 一两秒）、重建历史页会连开二三十个 BMP 头。放在 Lua 回调外面就没有这道预算；
// 顺序上先 consume 再 rebuild，Lua 的 on_tick 拿到的版本号和条目数才是同一份。
void pump();

// 离开应用：停轮询、收起图片、该断的 WiFi 断掉。任务还在跑时只打放弃标记，
// 由工作任务自己收尾（任务对象不能跨 vTaskDelete 析构，也不该被 UI 直接删）。
void reset();

// 整屏图片是否正在显示；以及要显示的那张 BMP 的路径。
bool overlayActive();
const std::string &overlayPath();

// 版本号：每次装进一张不同的图就 +1。Lua 靠它决定要不要重画；宿主也拿它
// 判断"图片换了，屏上的覆盖层该重推一帧"。
int version();

}  // namespace airpage_service
