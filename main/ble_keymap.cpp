#include "ble_keymap.h"

#include <cstdio>
#include <cstring>

#include <esp_log.h>

#include "pjournal_app.h"
#include "safe_file.h"

static const char *TAG = "BleKeymap";
#define KEYMAP_PATH "/sdcard/settings/bt_keymap"

static int s_src[BLE_ACT_COUNT] = {0};  // 动作 → 源键码（0 = 未绑定）

// 动作 → 交给界面的键码。全是各界面都认的通用导航码，所以映射后的键在
// 菜单/列表/对话框里同样有效，不必逐屏适配。
static const int kActKey[BLE_ACT_COUNT] = {
    KEY_PAGE_DOWN,  // PageForward
    KEY_PAGE_UP,    // PageBack
    '\n',           // Confirm
    0x1B,           // Back
    KEY_UP,
    KEY_DOWN,
    KEY_LEFT,
    KEY_RIGHT,
};

const char *bleKeymapActLabel(BleAct a) {
  switch (a) {
    case BleAct::PageForward: return "下一页";
    case BleAct::PageBack: return "上一页";
    case BleAct::Confirm: return "确认";
    case BleAct::Back: return "返回";
    case BleAct::Up: return "上移";
    case BleAct::Down: return "下移";
    case BleAct::Left: return "左移";
    case BleAct::Right: return "右移";
    default: return "?";
  }
}

int bleKeymapActKey(BleAct a) {
  int i = static_cast<int>(a);
  return (i >= 0 && i < BLE_ACT_COUNT) ? kActKey[i] : 0;
}

int bleKeymapSrcOf(BleAct a) {
  int i = static_cast<int>(a);
  return (i >= 0 && i < BLE_ACT_COUNT) ? s_src[i] : 0;
}

static bool save() {
  char line[32];
  std::string content;
  for (int i = 0; i < BLE_ACT_COUNT; i++) {
    if (s_src[i] == 0) continue;
    snprintf(line, sizeof(line), "%d %d\n", i, s_src[i]);
    content += line;
  }
  if (content.empty()) content = "\n";  // 全清空时留一个空文件，避免"文件不存在"与"没有绑定"混淆
  if (!safeWriteFile(KEYMAP_PATH, content)) {
    ESP_LOGE(TAG, "保存按键映射失败(SD 卡写入错误)");
    return false;
  }
  return true;
}

void bleKeymapLoad() {
  memset(s_src, 0, sizeof(s_src));
  std::string body = readWholeFile(KEYMAP_PATH);
  if (body.empty()) return;
  size_t pos = 0;
  int n = 0;
  while (pos < body.size()) {
    size_t nl = body.find('\n', pos);
    if (nl == std::string::npos) nl = body.size();
    int act = -1, key = 0;
    if (sscanf(body.substr(pos, nl - pos).c_str(), "%d %d", &act, &key) == 2 && act >= 0 &&
        act < BLE_ACT_COUNT && key != 0) {
      // 同一个源键只认第一次出现的动作，避免改坏文件后一个键触发两件事。
      bool dupSrc = false;
      for (int i = 0; i < BLE_ACT_COUNT; i++)
        if (s_src[i] == key && i < act) dupSrc = true;
      if (!dupSrc) { s_src[act] = key; n++; }
    }
    pos = nl + 1;
  }
  ESP_LOGI(TAG, "已载入 %d 条按键映射", n);
}

bool bleKeymapBind(int srcKey, BleAct a) {
  int act = static_cast<int>(a);
  if (srcKey == 0 || act < 0 || act >= BLE_ACT_COUNT) return false;
  for (int i = 0; i < BLE_ACT_COUNT; i++) {
    if (s_src[i] == srcKey) s_src[i] = 0;  // 同一个源键只能绑一个动作
  }
  s_src[act] = srcKey;
  return save();
}

bool bleKeymapClear(BleAct a) {
  int act = static_cast<int>(a);
  if (act < 0 || act >= BLE_ACT_COUNT) return false;
  s_src[act] = 0;
  return save();
}

int bleKeymapTranslate(int srcKey) {
  if (srcKey == 0) return 0;
  for (int i = 0; i < BLE_ACT_COUNT; i++) {
    if (s_src[i] == srcKey) return kActKey[i];
  }
  return 0;
}

// 常见消费类 usage 的中文名（遥控器/翻页器高频的那几个）。
struct ConsumerName {
  uint16_t usage;
  const char *label;
};
static const ConsumerName kConsumerNames[] = {
    {0x00B0, "播放"},     {0x00B1, "暂停"},     {0x00B3, "快进"},
    {0x00B4, "快退"},     {0x00B5, "下一曲"},   {0x00B6, "上一曲"},
    {0x00B7, "停止"},     {0x00CD, "播放/暂停"}, {0x00E2, "静音"},
    {0x00E9, "音量+"},    {0x00EA, "音量-"},    {0x0223, "主页"},
};

std::string bleKeymapKeyName(int key) {
  if (key >= KEY_CONSUMER_BASE) {
    uint16_t u = static_cast<uint16_t>(key & 0x1FFF);
    for (const auto &c : kConsumerNames)
      if (c.usage == u) return std::string("消费键 ") + c.label;
    char b[32];
    snprintf(b, sizeof(b), "消费键 0x%02X", static_cast<unsigned>(u));
    return b;
  }
  switch (key) {
    case KEY_UP: return "↑";
    case KEY_DOWN: return "↓";
    case KEY_LEFT: return "←";
    case KEY_RIGHT: return "→";
    case KEY_PAGE_UP: return "PageUp";
    case KEY_PAGE_DOWN: return "PageDown";
    case KEY_HOME: return "Home";
    case KEY_END: return "End";
    case '\n': return "回车";
    case ' ': return "空格";
    case '\t': return "Tab";
    case 0x1B: return "Esc";
    case 0x08: return "退格";
    default: break;
  }
  if (key >= 0x20 && key <= 0x7E) {
    char b[8];
    snprintf(b, sizeof(b), "%c", static_cast<char>(key));
    return b;
  }
  char b[24];
  snprintf(b, sizeof(b), "键码 0x%X", static_cast<unsigned>(key));
  return b;
}
