#pragma once

// 三模式共享的「编辑按钮条」与「粘贴板列表」——和 clipboard 一样是底层件，
// 分工照 editor_vk：模块持有自己的状态、只管几何/标签/绘制/命中，
// **动作由宿主执行**（模块不知道宿主是编辑器正文还是计划模式的输入框）。
//
// 按钮条：有选区 = 复制/剪切/粘贴/全选/润色；无选区 = 粘贴/全选/取消。
// 贴在宿主给的"正文区底边"之上——编辑器传自己的 contentEndY，于是它正好落在
// 虚拟键盘上面那条（不藏键盘：藏了要写 editor_vk 的粘性覆盖标记，得不偿失）。
//
// EM_FIELD 是给**单行输入框**（计划/灵感/设置/flomo 那一族）用的：它们没有润色、
// 也少有"先选一段"的场景，所以只有复制/剪切/粘贴/全选四项。
enum EditMenuKind { EM_NONE = 0, EM_EDIT = 1, EM_PASTE_ONLY = 2, EM_FIELD = 3 };

// ── 按钮条 ──────────────────────────────────────────────────────────────
void editMenuOpen(EditMenuKind kind);
void editMenuClose();
bool editMenuActive();
EditMenuKind editMenuKind();
// 正文区底边 y。宿主每次重绘前设一次；<=0 表示贴屏底。
void editMenuSetBottom(int bottomY);
void editMenuDraw();
int editMenuHit(int x, int y);   // 按钮下标 / -1
int editMenuCount();
int editMenuSel();
void editMenuSetSel(int i);
void editMenuMove(int delta);
const char *editMenuLabel(int i);

// ── 粘贴板列表（从历史里挑一条）─────────────────────────────────────────
void editMenuOpenPicker();
void editMenuClosePicker();
bool editMenuPickerActive();
int editMenuPickerSel();                 // 粘贴板下标；-1 = 空/未定
void editMenuPickerSetSel(int i);
void editMenuPickerMove(int delta);
void editMenuPickerDraw();
int editMenuPickerHit(int x, int y);     // 粘贴板下标 / -1
