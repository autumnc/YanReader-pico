#pragma once

// 待机表盘：设备空闲休眠（light sleep）时铺满整屏的时钟画面。
//
// 与 pjournal 的 UI 框架同源：直接画进 epdiy framebuffer（u8g2 shim，0=黑 1=白），
// 文字走 g_font（TTF 抗锯齿）。墨水屏双稳态，画面在整个休眠期间零功耗保持。
//
// 时间来源：g_rtc（PMU 维护的 RTC，由「设置 → 网络同步时间」写入）。未对时
// 时表盘显示占位并提示去对时。

enum class StandbyFace {
    Off = 0,     // 关闭：不画任何东西，屏上原样保留休眠前的画面
    Clock = 1,   // 简约时钟：大号七段数字 HH:MM + 日期 + 农历
    Almanac = 2, // 老黄历：公历日 + 农历干支 + 二十四节气 + 宜忌
    Cover = 3,   // 书籍封面：最后阅读那本的封面 + 时刻 + 书名/进度
    Image = 4,   // 图片：用户自己选的一张图铺满整屏（文件管理里「设为待机画面」）
};

// 设置键 clock_face 的取值 <-> 枚举（未知/空串按 Off）。
StandbyFace standbyFaceFromKey(const char *key);
const char *standbyFaceKey(StandbyFace f);
const char *standbyFaceLabel(StandbyFace f); // "关闭" / "简约时钟" / "老黄历" / "书籍封面" / "图片"

// 整屏绘制表盘并提交（整屏 GC16）。**表盘都不画底部工具栏**（不留唤醒提示、
// 不留状态栏/电量）——唤醒源只有电源键，屏上不留提示。face==Off 时什么都不画，
// 屏上原样保留休眠前的画面。
// Cover 面读的是「最后阅读的那本书」的封面（reader_progress.txt 表头那本，见
// screen_reader.h 的 readerLastBookCover）——本书没封面文件时画占位框，没读过书时
// 只剩时刻与日期。
// Image 面读的是用户在文件管理里选的那张图（设置键 standby_image，解码缓存见
// screen_reader.h 的 readerStandbyImage）——**只有图**，不叠时刻/状态栏；没选图或
// 缓存还没做出来时画占位提示。
// 进入前会 ui_render_keep_frame() 把当前画面留一份，因此唤醒后
// ui_restore_snapshot() 能把休眠前的画面推回来。
void standbyClockDraw(StandbyFace face);

// 关机页：长按电源键满 8s（PMU KEY_FORCE_OFF）时铺「已关机」，抢在满 10s 的硬断电
// 之前把画面定稿。**不同于表盘**：这页不保留/不恢复任何帧，它是断电前屏上的最后一帧，
// 断电后就靠墨水屏双稳态停在屏上。调用方负责随后把主循环挡住（见 main.cpp）。
void standbyShutdownDraw();

// 预览：绘制表盘 → 停留 ms 毫秒 → 用保留帧恢复原画面（不进入休眠）。
// 供设置界面即时预览用。
void standbyClockPreview(StandbyFace face, int ms);

// 立即进入待机：按设置绘制表盘并 light sleep，任意键唤醒（阻塞到唤醒为止）。
// 定义在 main.cpp（复用自动休眠的 enterLightSleep 全流程：关 BLE / 断 EPD /
// 唤醒后重连）。供阅读菜单「待机时钟」手动触发。
void app_enterStandby();
