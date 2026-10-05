# Yan Reader（研读）

> 墨水屏上的**阅读 · 写作 · 计划**三合一设备固件。
> 跑在 Read Pico 硬件（ESP32-S3 + 7.5″ 电子纸）上，基于 ESP-IDF v6.1。

阅读器部分复用 vendored 的 [crossmux](components/crossmux) 排版引擎（EPUB / TXT / XTC），
写作与计划部分来自 `pjournal-esp32`（BLE 键盘 + 拼音输入法 + GTD 大纲），
屏幕推屏与字体层复用 [read_pico_firmware](../read_pico_firmware) 的 epdiy 与 ttf_font。

开机三面「**研读 · 研墨 · 研行**」对应三个模式，**电源键短按轮换**。

---

## 目录

- [功能总览](#功能总览)
- [硬件](#硬件)
- [构建 / 烧录](#构建--烧录)
- [SD 卡目录布局](#sd-卡目录布局)
- [首次开机](#首次开机)
- [项目结构](#项目结构)
- [架构分层](#架构分层)
- [主机端测试](#主机端测试)
- [资源生成脚本](#资源生成脚本)
- [关键约束（改代码前先看）](#关键约束改代码前先看)
- [已知限制 / 待办](#已知限制--待办)

---

## 功能总览

### 📖 阅读模式（研读）

| 类别 | 内容 |
|---|---|
| 格式 | EPUB（含内嵌图片 / CSS / 脚注 / 目录）、TXT（GBK 自动识别转码）、XTC |
| 排版 | 字号 9 档（正文到 88px 标题）、字体族切换（内建 / SD 字体 / 书内嵌字体）、行距、段距、缩进、对齐、边距，全部可调 |
| 样式 | 「书籍内嵌 CSS」/「强制指定」两种解析口径；支持 EPUB 的 `font-size`、`border-bottom` 专名线、`wavy` 书名线 |
| 翻页 | 左右电容键单击翻页、双击跨章；触摸左右三分区；上下滑动；蓝牙翻页器 |
| 阅读工具 | 目录、书签、百分比跳转、脚注就地弹注、长按选句（标注 / 笔记 / 查字典）；阅读菜单里的排版项收在「排版设定 ›」子菜单下 |
| 搜索 | 书架按书名过滤、笔记按原文 / 笔记正文 / 书名过滤（**没有书内全文搜索**，见[待办](#已知限制--待办)）|
| 词典 | StarDict 词库，本地查询；从 crossmux 词典清单下载 / 校验 / 安装 |
| 统计 | 阅读时长、每日页数、热力图、连续天数、单书详情、阅读档案 |
| 导出 | 笔记标签页搜索栏右端「导出」按钮 → **全部书**的标注 / 笔记 / 书签合成一份 Markdown 到 SD |
| 其他 | 夜间反色、横竖屏切换、自定义状态栏、图片查看器、二维码、关于页 |
| 在线 | OPDS 书库浏览下载、WiFi 传书（浏览器上传）、微信读书缓存（**未实机验证**） |

### ✍️ 写作模式（研墨）

日记 / 自由写作（提示词随写随加，不再分两个入口）、大纲写作、灵感库、AI 润色（DeepSeek）、
[Flomo](https://flomoapp.com) 笔记同步、WebDAV 同步、历史版本、Markdown 渲染、竖排书写。

### ✅ 计划模式（研行）

GTD 任务管理：任务 / 项目 / 日历 / 便签 / 筛选，支持导出 Markdown。

### 输入

- **虚拟键盘**：全屏软键盘（`editor_vk.cpp`），三种模式共用一套
- **拼音输入法**：整句 DP 断词、歧义布局（一键多字母）、T9、用户词库与学习、繁体转换、联想
- **BLE 键盘 / 遥控器**：HID 主机，支持消费类按键映射（翻页器 / 遥控器）
- **触摸**：点按、长按、拖动、横滑返回、双指捏合

---

## 硬件

| | |
|---|---|
| 主控 | ESP32-S3（双核 240MHz，16MB flash，8MB Octal PSRAM @120MHz）|
| 屏幕 | E0470A01 电子纸 **1216 × 684**，16-bit 总线 @24MHz，epdiy 4bpp 帧缓冲 |
| 按键 | 3 个电容键（左 / 中 / 右）+ 电源键 |
| 触摸 | CST836U 电容触摸 |
| 传感器 | SC7A20H 加速度计（晃动机身 = 全刷清残影） |
| 时钟 | PCF85063 RTC + PMU RTC，NTP 对时 |
| PMU | read_pico PMU（电量、电源键事件、软关机） |
| 无线 | WiFi STA（OPDS / WebDAV / Flomo / DeepSeek / 传书）+ BLE HID 主机 |
| 存储 | microSD（FAT，UTF-8 长文件名） |

> 方向约定：panel 原生 **1216 宽 × 684 高 = 横屏**。竖屏时逻辑宽高互换
> （684 × 1216），书架列数、统计卡列数等按 `getScreenWidth()` 自适应。

---

## 构建 / 烧录

### 依赖

- **ESP-IDF v6.1**（read_pico 组件用到 v6 头文件）
- **`read_pico_firmware` 仓库**，与 `pjournal-pico` 放在**同级目录**：
  ```
  parent/
  ├── pjournal-pico/          ← 本仓库
  └── read_pico_firmware/     ← 提供 epdiy / read_pico / read_pico_pmu / 波形 / 触摸驱动
  ```
  放在别处就用 `-DREAD_PICO_DIR=<path>` 指定。

### 构建

```sh
source $IDF_PATH/export.sh          # 例如 source ~/esp-idf/export.sh
idf.py build
```

### 烧录 + 串口

```sh
idf.py -p /dev/ttyACM0 flash monitor
```

USB 串口走 ESP32-S3 内置 **USB-Serial-JTAG**（`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`）。
若 `/dev/ttyACM0` 权限不足（`root:uucp`），装条 udev 规则或临时 `chmod 666`。

### 分区表

`partitions_16M.csv`：

| 分区 | 偏移 | 大小 |
|---|---|---|
| `nvs` | 0x9000 | 20 KB |
| `phy_init` | 0xE000 | 4 KB |
| `factory`（app） | 0x10000 | 12 MB |
| `storage`（FAT） | — | 3 MB |

当前固件约 10 MB，app 分区余 ~17%，**OTA 放不下**（见[待办](#已知限制--待办)）。

### 构建选项（`main/CMakeLists.txt`）

| 选项 | 默认 | 说明 |
|---|---|---|
| `PJOURNAL_IME_ENABLE_PINYIN` | ON | 拼音输入法（需 `main/ime/ime_table_pinyin.bin`）|
| `PJOURNAL_IME_ENABLE_LIANGFEN` | ON | 两分输入法（需 `main/ime/liangfen.bin`）|
| `PJOURNAL_IME_ENABLE_WUBI` | OFF | 五笔 |
| `PJOURNAL_IME_ENABLE_SHUANGPIN` | OFF | 双拼 |
| `PJOURNAL_IME_PERF_LOG` | OFF | 输入法慢查询日志 |

> ⚠️ 拼音 / 两分的 `.bin` 是**外部生成产物**，不在仓库里。缺文件时把上述选项关掉，
> 或先跑[资源生成脚本](#资源生成脚本)。

---

## SD 卡目录布局

```
/sdcard/
├── books/                    # 书籍（EPUB / TXT / XTC），也在 /sdcard/Books 下扫描
├── fonts/                    # 用户字体 <name>.ttf
├── dictionaries/<id>/        # 已安装词典（StarDict），含标记 .crossmux-resource
├── settings/                 # 设备设置与状态（22+ 个文件）
│   ├── bt_keymap             # BLE 按键映射表
│   ├── bt_paired             # 已配对蓝牙设备
│   ├── clipboard.txt         # 跨模式剪贴板
│   ├── sync_state.txt        # WebDAV 同步状态
│   └── userdict*.txt         # 输入法用户词库
├── pjournal/                 # 日记正文 + flomo/inspiration 缓存
├── outline/                  # 大纲 / 灵感库 inspiration.json / 导出
├── gtd/                      # 计划模式数据 + 导出
├── flomo/                    # Flomo 本地库 memos.json
├── exports/                  # 阅读标注导出（标注导出_<时间戳>.md，全部书合一份）
├── .quick_history/           # 编辑器历史版本
├── .crossmux/                # 各书的排版 / 封面缓存
├── reader_progress.txt       # 阅读进度
├── reader_notes.txt          # 标注 / 笔记
└── reader_stats.json         # 阅读统计
```

> `.` 开头的目录是内部缓存，删除只会导致重新生成（重排版 / 重抓封面）。
> 书的排版缓存按书独立，**删书请走书架菜单的「删除本书」**，它会连带清缓存。

---

## 首次开机

1. 插一张 FAT32 microSD，插入设备。
2. 开机动画（研读 · 研墨 · 研行）后**默认进阅读模式**，顶栏三个根标签：**书架 / 微读 / 设置**。
3. 切到 **`设置` → WiFi 管理**连上 WiFi（对时后统计与待机时钟才准）。
4. 把 `.epub` / `.txt` 放进 `/sdcard/books/`，回书架标签会自动扫描出封面。
5. 装词典走 **`设置` → 词典下载**；要导出标注去 **`笔记` 标签页**搜索栏右端的「导出」按钮
   （导出全部书的标注 / 笔记 / 书签，落到 `/sdcard/exports/`）。
6. **电源键短按**在 阅读 → 写作 → 计划 三个模式间轮换。

---

## 项目结构

```
pjournal-pico/
├── CMakeLists.txt              # 顶层：挂 read_pico 的 EXTRA_COMPONENT_DIRS
├── partitions_16M.csv          # 16MB flash 分区表
├── sdkconfig.defaults          # 板级 + 网络 + 内存调优（每行都有为什么）
├── main/                       # 主组件（见下）
├── components/
│   ├── crossmux/               # vendored 阅读引擎（Epub / Txt / Xtc / GfxRenderer / Dictionary / WeRead）
│   └── e0470_page_turn/        # E0470 翻页专用波形
├── tests/host/                 # 主机端测试（Linux 上跑真代码，不上机）
├── scripts/                    # 资源生成（IME 字库 / 图标字体 / 简繁表）
├── tools/                      # make_logo_font.py
└── docs/
    └── 架构评审-2026-10-04.md   # 分层评审 + 整改进度
```

### `main/` 主要模块

```
main.cpp                   # app_main：主循环 + 状态轮换 + 待机/唤醒 + 分配失败黑匣子
pjournal_app.cpp           # 主菜单 + 浏览器 + 查看器 + 历史（Screen 表里的几屏）
ui/screen.h                # ★ Screen 契约：每屏一行（enter/handle/leave/idle_ms/vk_host）
pjournal_app.h             # AppState / ScreenContext / KEY_* 键码

── 三个模式 ──
screen_reader*.cpp         # 阅读模式（拆成 6 个 TU：主体 + 统计 + 子界面 + 文件 + 脚注 + 微读）
screen_editor.cpp          # 写作：编辑器
screen_gtd.cpp             # 计划：GTD

── 共享服务 ──
ui_render.cpp              # core1 渲染任务 + 双缓冲
display.c                  # ★ 所有界面的推屏落地点（波形 / 反残影 / 夜间反色）
ui_helpers.cpp             # 通用 UI 绘制 + toast + 确认框 + WiFi 助手
editor_vk.cpp              # 唯一的虚拟键盘实现
ime/IME.cpp                # 拼音 / 两分 / 歧义 / T9 / 用户词库 / 整句 DP
ui/ime_field.cpp           # 字段算术层（按值传，只做 UTF-8 运算不落串）
text_sel.cpp / clipboard / edit_menu.cpp   # 触摸编辑共享层
net/http.cpp               # 全仓唯一的 HTTP 出口
hw/board.cpp hw/input.cpp  # 板级 bring-up + 输入事件源（触摸 / 键 / 摇一摇 / 电源键）
gfx/ font/                 # u8g2 shim + 图标字体 + ttf_font（stb_truetype）
```

---

## 架构分层

来自 [`docs/架构评审-2026-10-04.md`](docs/架构评审-2026-10-04.md)（P0–P3 已完成）：

```
应用层      main.cpp ──→ 各 screen_*.cpp（单向依赖，屏之间不互相 include 实现）
交互层      hw/input.cpp（事件源）· ui_helpers · text_sel · edit_menu · clipboard
输入法层    ime/IME.cpp（单例，handleKey 纯函数式主路径）· editor_vk.cpp
基础层      crossmux（排版/渲染/存储）· gfx · font · display.c · net/http
```

### 关键设计

- **`Screen` 契约**（`ui/screen.h`）：主循环不再是一堆 `static bool xxxInited`，
  而是一张表 `kScreens[]`，每屏一行声明 `enter` / `handle` / `leave` / `idle_ms` /
  `vk_host` / `local_only`。生命周期只有**进入**和**离开**两个事件。
- **推屏单一路径**：所有界面的推屏落地点是 `display.c`；`HalDisplay` 只管阅读器一族。
  夜间反色在 `display.c` 里对 front/back 帧缓冲**成对**取反再翻回来。
- **渲染任务**：core1 上跑 `ui_render`（双缓冲），阅读器**刻意绕过**它自己直绘。
- **字段算术层 `ime_field`**：把「往字段里插字 / 退格 / 取光标」从输入法里抽出，
  按值传（两个指针），只做 UTF-8 算术，宿主（编辑器 / GTD / 阅读器）各自保留策略。

---

## 主机端测试

`tests/host/` 用普通 `g++` 在 Linux x86-64 上跑**真实的固件源码**，不需要 ESP-IDF、
不上机、不烧录。改完相关模块先跑一遍再构建。

```sh
tests/host/ime/run.sh            # 拼音输入法：bug 复现 + 回归（zhege→这个 / jiushi→就是）
tests/host/ime/run.sh --verify-fix   # 证明回归测试在修复前的 IME.cpp 上会失败
tests/host/rotation/run.sh       # 旋转映射等价性对拍（665 万个点）
```

`tests/host/ime/README.md` 详细说明了桩、`#define private public` 的用法与新增用例的方法。

---

## 资源生成脚本

| 脚本 | 作用 |
|---|---|
| `scripts/generate_ime_bin.py` | 从词库源码生成拼音/两分 `.bin` |
| `scripts/build_ime_source.py` | 构建 IME 词库源 |
| `scripts/generate_s2t.py` | 生成简繁对照表 |
| `scripts/subset_icon_font.py` | 从 Nerd Font 子集化图标字体（`assets/icon_font.ttf`）|
| `tools/make_logo_font.py` | 生成开机动画用的「研」标志字体（`assets/yanos_logo.ttf`）|

---

## 关键约束（改代码前先看）

这些都是踩过坑写进注释里的，**违反会静默出错**：

- **内存**：内部 RAM 极紧张（进阅读模式后常剩 ~20KB）。默认分配优先 PSRAM
  （`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=0`）；真正要内部 / DMA 的缓冲必须显式
  `MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA`。**FreeRTOS 任务栈永远在内部 RAM**
  （要 PSRAM 栈得 `xTaskCreateWithCaps`，且该任务不能写 flash）。
- **SD DMA**：RTC fast RAM 必须禁止当堆用（`CONFIG_ESP_SYSTEM_ALLOW_RTC_FAST_MEM_AS_HEAP`
  未设），否则 SDMMC 按对齐判 DMA 会把该区缓冲写坏。
- **CMake GLOB**：`components/crossmux` 用 `file(GLOB_RECURSE)`，加 / 删源文件后必须
  `idf.py reconfigure`，否则链接期 undefined reference。
- **改阅读页 y 坐标**：`el->yPos` 是**行顶**，`TextBlock::render` 的 y 是**基线**，
  必须补 `getFontAscenderSize(行自己的字体号)`；漏了会导致大字号首行切顶 + 选词偏一行。
- **显示**：`g_rd.begin()` 必须在**横屏**下调用；行内反色必须 `drawText(..., true)`，
  u8g2 的 draw color 对 TTF 渲染器无效。
- **区域刷新不省时间**：只有相位数省时间，两个区域两次波形 = 两遍扫描。

更多（字体架构、epdiy 波形、任务栈、BLE 生命周期、输入法分词…）见仓库内的
`docs/` 与代码注释——**注释密度很高，改动前先读目标文件顶部的块注释**。

---

## 已知限制 / 待办

- **OTA 未做**：flash 布局受限（app 分区 12MB，固件已 ~10MB）。
- **KOReader 同步未做**：需要自建同步服务器 + 账号，**等使用者决定是否要做**。
- **微信读书链路未实机验证**：扫码登录 → 书架同步 → 整本缓存 → 打开阅读，
  一次都没跑过，且依赖先连 WiFi。
- **书内全文搜索未做**：做过一版（逐章解压 + 去标签建索引、跳转逐页比对排版），
  但整本过一遍是秒级且同步跑在主任务上，**实测会卡**，已移除。要重做必须先解决
  "索引从哪来"（后台任务 + 落盘索引），不能再走同步扫正文的路。
- **成就系统未做**（阅读统计的时长 / 页数 / 热力图 / 连续天数都在）。
- **i18n 未做**：本固件为纯中文界面。

---

## 相关仓库

- [`read_pico_firmware`](../read_pico_firmware) — 板级 bring-up、epdiy、波形、字体层
- [crossmux](components/crossmux) — 阅读引擎（本仓库内 vendored）
- `pjournal-esp32` — 写作 / 计划模式的来源（BLE 键盘、拼音输入法、GTD）
