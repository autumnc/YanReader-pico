#pragma once

#include <cstdint>
#include <string>

// BLE 键 → 阅读动作 的映射表（对应 crossmux 的 BleButtonMapActivity）。
//
// 用途：把蓝牙键盘上任意一个键、或蓝牙遥控器/翻页器的消费类键（音量、播放…，
// 键码 KEY_CONSUMER_BASE|usage），绑到翻页/确认/返回/上下左右。遥控器只发消费类
// 报告，不映射就完全没反应（见 bt_keyboard.cpp 的 handleConsumerReport）。
//
// **只在阅读模式生效**：写作模式的编辑器/输入法必须原样收到每个键，不能被改道，
// 所以 main.cpp 的 key 分发不做翻译，翻译在 screen_reader_handle 里做。
//
// 持久化：/sdcard/settings/bt_keymap，行格式 "<动作序号> <源键码十进制>"。

enum class BleAct : uint8_t {
  PageForward = 0,  // 下一页
  PageBack,         // 上一页
  Confirm,          // 确认
  Back,             // 返回
  Up,
  Down,
  Left,
  Right,
  Count
};

#define BLE_ACT_COUNT 8

// 从 SD 卡读入映射表。可重复调用（覆盖内存表）。
void bleKeymapLoad();

// 动作的中文名（"下一页"/"返回"…）。
const char *bleKeymapActLabel(BleAct a);

// 动作对应的 pjournal 键码（翻译后交给界面，见 ble_keymap.cpp）。
int bleKeymapActKey(BleAct a);

// 该动作当前绑定的源键码；0 = 未绑定。
int bleKeymapSrcOf(BleAct a);

// 绑定。源键与动作各自唯一：源键改绑到新动作即从旧动作解绑。
// 返回 false = 源键非法（0，或 0xFFFF 哨兵）。成功后立即落盘。
bool bleKeymapBind(int srcKey, BleAct a);

// 解绑。成功后立即落盘。
bool bleKeymapClear(BleAct a);

// 翻译：源键码 → 动作键码；未绑定返回 0。
int bleKeymapTranslate(int srcKey);

// 源键码的可读名字（"↑" / "a" / "空格" / "消费键 播放"）。
std::string bleKeymapKeyName(int key);
