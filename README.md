# Yan Reader（研读）

> 墨水屏上的**阅读 · 写作 · 计划**三合一设备固件。
> 跑在 Read Pico 硬件（ESP32-S3 + 7.5″ 电子纸）上，基于 ESP-IDF v6.1。

阅读器部分基于 vendored 的 [crossmux](components/crossmux) 排版引擎（EPUB / TXT / XTC），
我们在它之上另写了一层、也直接改进了它本身（内嵌字体、弹注、专名线/书名线…），
明细见[对 crossmux 引擎的改动与增强](#对-crossmux-引擎的改动与增强)。
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
- [使用方式](#使用方式)
- [项目结构](#项目结构)
- [架构分层](#架构分层)
- [对 crossmux 引擎的改动与增强](#对-crossmux-引擎的改动与增强)
- [虚拟键盘版式](#虚拟键盘版式)
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
| 排版 | 字号 6 档（可选 40~64px，76/88px 为标题专用）、字体族切换（内建 / SD 字体 / 书内嵌字体）、行距、段距、缩进、对齐、边距，全部可调 |
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

- **虚拟键盘**：全屏软键盘（`main/editor_vk.cpp`），三种模式共用一套。三张键面（字母 / 符号 / 数字）、
  四种键位布局（26 / 14 / 18 / 9 键）、T9 候选面板、候选字大小可调 —— **版式详见[虚拟键盘版式](#虚拟键盘版式)**
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

### 烧录

**从源码**

```sh
idf.py -p /dev/ttyACM0 flash          # 刷完自动硬复位，直接跑新固件
idf.py -p /dev/ttyACM0 flash monitor  # 顺带接串口看日志（Ctrl+] 退出）
```

**从 Release 二进制**（不装 ESP-IDF 也能刷，只要有 `esptool`）

| 发布文件 | 内容 | 烧到 |
|---|---|---|
| `yanreader-<版本>-full.bin` | bootloader + 分区表 + 应用 | `0x0` |
| `pjournal_pico.bin` | 只有应用 | `0x10000` |

```sh
pip install esptool                     # 一次性
esptool.py --chip esp32s3 -b 460800 write_flash 0x0 yanreader-1.0.0-full.bin
# 或：只更新应用、不动 bootloader / 分区表
esptool.py --chip esp32s3 -b 460800 write_flash 0x10000 pjournal_pico.bin
```

镜像里**没有 NVS 分区**，所以刷完**设备设置与 WiFi 凭据都还在**（`settings/` 那些状态文件在
SD 卡上，更不受影响）。要回到"出厂状态"就再补一条
`esptool.py --chip esp32s3 erase_flash`（**这一条会连 NVS 一起抹掉**）。

### 串口

USB 串口走 ESP32-S3 内置 **USB-Serial-JTAG**（`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`）。
若 `/dev/ttyACM0` 权限不足（`root:uucp`），装条 udev 规则或临时 `chmod 666`。
**刷完之后设备会重新枚举 USB**：之前开着的 `monitor` 会失效（文件描述符废了），要重新开一个。

只想看日志、不想装 ESP-IDF 时，直接读也行（空闲时固件不打日志，按下按键才有输出）：

```sh
cat /dev/ttyACM0
```

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
2. 开机动画（研读 · 研墨 · 研行）后**默认进阅读模式**，顶栏 5 个根标签：
   **书架 / 文件 / 笔记 / 设置 / 统计**（微信读书在「设置」标签里，不再单独占一个根标签）。
3. 切到 **`设置` → WiFi 管理**连上 WiFi（对时后统计与待机时钟才准）。
4. 把 `.epub` / `.txt` 放进 `/sdcard/books/`，回书架标签会自动扫描出封面。
5. 装词典走 **`设置` → 词典下载**；要导出标注去 **`笔记` 标签页**搜索栏右端的「导出」按钮
   （导出全部书的标注 / 笔记 / 书签，落到 `/sdcard/exports/`）。
6. **电源键短按**在 阅读 → 写作 → 计划 三个模式间轮换。

---

## 使用方式

### 按键

| 操作 | 作用 |
|---|---|
| **电源键** 短按 | 三个模式轮换：阅读 → 写作 → 计划 → 阅读 |
| **电源键** 长按 8 秒 | 关机 |
| **电源键** | **唯一的唤醒键** —— 待机后只有它能唤醒（电容键、触摸、晃动都唤不醒）|
| 左电容键 单击 / 双击 | 阅读页：上一页 / 上一章 |
| 右电容键 单击 / 双击 | 阅读页：下一页 / 下一章 |
| 中间电容键 按下 / 长按 | 回车确认 / **进入待机** |

> 长按中间键进待机，在阅读器某些"该键是动作键"的子界面里不生效（那时它执行该界面的动作）。
> 不按也没关系：**自动待机**到点自己睡（时长在阅读模式设置里调，0 = 关；三个模式共用这一个计时）。

### 触摸

| 手势 | 作用 |
|---|---|
| 点按 | 回车确认。阅读页再分三区：左 1/3 上一页、右 1/3 下一页、中间 1/3 开阅读菜单 |
| 左右滑 | 上一个 / 下一个 |
| 上下滑 | 整页滚动 |
| 从**屏幕边缘**起划的横划 | 返回 |
| 长按（≥ 600ms 不动） | 长按。阅读页 = 选句（标注 / 笔记 / 查字典）|
| 按住拖动 | 拖动 / 选择文本 |
| 双指捏合 | 缩放（**只有图片查看器认**，其它界面无作用）|

**晃一下机身**（SC7A20H 加速度计）= 全屏重刷清残影。它**不是**唤醒源。

### 三个模式

- **阅读（研读）** —— 顶栏 5 个根标签 **书架 / 文件 / 笔记 / 设置 / 统计**。
  - 阅读页点中间 1/3 出**阅读菜单**：目录、**排版设定 ›**（字号 / 字体 / 行距 / 段距 /
    缩进 / 对齐 / 边距 / 阅读线）、书签、脚注、跳转百分比、二维码、词典、返回书架 / 返回阅读。
  - **设置**标签里：WiFi 管理、WiFi 传书、微信读书、OPDS 书库、词典下载、资源下载、按键映射、
    状态栏、书架风格、样式解析、内嵌字体、刷新策略、全刷频率、翻页动画、阅读器方向、待机表盘、
    自动待机、待机时钟、关于本机。
  - **字号 / 字体不在这里** —— 它们是「这一本书的排版」，在阅读菜单的「排版设定 ›」下。
- **写作（研墨）** —— 主页 6 个图标：**日记 / 查看过往日记 / 同步WebDAV / 大纲写作 / Flomo笔记 / 设置**。
  「设置」分七类：通用、显示与版式、输入法、打字机模式、编辑与保存、网络与同步、AI；
  **键盘布局**在「输入法」下、**蓝牙设备管理**在「通用」下。
- **计划（研行）** —— GTD：任务 / 项目 / 日历 / 便签 / 筛选，可导出 Markdown。

### 输入

- **虚拟键盘**（三个模式共用一套）：底部功能行 `① ② 空格 中/英 回车`，
  `①②` 是**面板切换**（字母 / 符号 / 数字三张面绕圈）、`中/英` 切输入法中英；额外行是
  `布局 Ctl 方向键 Shift 退格`。版式与键位细节见[虚拟键盘版式](#虚拟键盘版式)。
- **键盘布局 26 / 14 / 18 / 9 键**在「设置 → 输入法 → 键盘布局」里换。**14 / 18 / 9 键只在中文拼音态生效**，
  英文态自动回 26 键（切换键面上的布局标签会跟着变，不会出现"写着 14 键、画着 26 键"）。
- **清残影时机**（「设置 → 输入法 → 清残影时机」）：编码区+候选区那两行的残影什么时候清一次区域 GC16
  —— `句读后`（默认）/ `上屏后` / `两者都清` / `不清` / `从不清(只快刷)`。默认只挑句读，因为上屏每词都清会在同一句里
  清两遍（看着就是"刷了两次"）；打长句很久不敲标点的人可以换成「上屏后」或「两者都清」。
  **`不清` 和 `从不清(只快刷)` 不是一回事**：前者只关掉句读/上屏那两笔记账，快档自己欠下的那笔照还；
  后者是全关（连"敲够次数借停顿清一次"的计数兜底也一起关），打字全程一次区域 GC16 都不做 ——
  打出来的字会一直发灰、键盘留残影，直到下一次整屏全刷（翻页 / 换章 / 摇一摇）把它扫掉。
  **阅读模式的虚拟键盘跟随同一套设定**（两边各自记账，但读同一个设置项）。
- **上屏刷法**（「设置 → 输入法 → 上屏刷法」）：正文那一拍怎么刷。`稳（整屏）`（默认，墨实，
  每键约 220ms）/ `快（差分）`（只推差分矩形，约 63ms，刚上屏的字先发灰，敲完标点那次
  清残影一并把它坐实）。
- **蓝牙键盘 / 翻页器**：在「设置 → 通用 → 蓝牙设备管理」里配对。实体键盘上
  `Ctrl+Space` 中英、`Shift+Space` 全角、`Ctrl+Shift+F` 简繁、轻点左 `Shift` 临时英文。
- **阅读模式也有虚拟键盘**（书架搜索、词典、笔记搜索、文件改名、新建文件夹、WiFi / OPDS 密码等），
  右下角键盘图标开合。**连着蓝牙键盘时面板不弹**，编码行 / 候选行改画在正文下方，
  所以用实体键盘打中文也看得见自己打的是什么。

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

## 对 crossmux 引擎的改动与增强

阅读内核基于 vendored 的 [crossmux](components/crossmux)（快照自上游
[0x1abin/crossmux](https://github.com/0x1abin/crossmux) `b5fdc419`，2026-10-01）。
相对那份上游，我们**改了 `lib/` 52 个文件、`src/` 4 个文件**，**新增** `Dictionary/`
`JPEGDEC/` `PNGdec/` `QrCode/` `weread_port/` 五个目录，并删掉了用不上的部分
（`KOReaderSync` / `I18n` / `SoundFeedback` / `HalAudioOutput` / `HalFrontlight` / 上游那套
`SdCardFont` 字体栈…）。下面按"改在引擎里"和"建在引擎外"分开列。

### 一、改在引擎内部的增强（`components/crossmux/` 里我们写的代码）

| 增强 | 做了什么 | 触到的引擎文件 |
|---|---|---|
| **字体栈换芯** | 上游是 `EpdFont` + `SdCardFont` + `FontDecompressor` 那一套**预烘焙/压缩位图字体**；整套换成 `main/font/ttf_font`（stb_truetype，**运行时栅格化**），才撑得起任意 SD 字体、书内嵌字体、CJK 扩展区 B。角色分家（内容面 / UI 面 / 内容备选面）与共享格子模型也在这一层 | 删 `lib/EpdFont/{EpdFont,SdCardFont*,FontDecompressor*}`；保留并改写 `EpdFontFamily`、`EpdFontData` |
| **书内嵌字体（EPUB `@font-face`）** | 扫书自己的 CSS 找正文字体 → 从 zip 里解出字体文件落缓存（`book_font.txt`），阅读时把内容面换成它，退出/换书还原用户全局字体。上游完全没有这条链路 | `Epub/Epub.cpp`（`resolveEmbeddedFont` / `extractEmbeddedFont` / `resolveFontHref`）、`css/CssParser.*`（家族名哈希归一） |
| **古籍专名线 / 书名线** | `border-bottom` 实线 = 专名线、波浪（`wavy` / 多看私有 `duokan-wavyline` / `border-image` / 虚点线）= 书名线；给 span 画线而不打扰块级元素。为此 **`EpdFontFamily::Style` 从 `uint8_t` 加宽到 `uint16_t`**、TextBlock 的 tracker 从 2 条变 3 条（实线下划线遇波浪让位）、新增 `GfxRenderer::drawWavyLine` | `css/CssParser.*`、`css/CssStyle.h`、`EpdFont/EpdFontFamily.h`、`GfxRenderer/GfxRenderer.*`、`Epub/blocks/TextBlock.cpp`、`Epub/Section.cpp` |
| **脚注回引识别** | `localAnchorOf()` 原来只认裸 `#anchor`，calibre 转的书每条内链都带文件名（`part0008.html#m7`）→ 判成跨文件 → 注文页塞满"指回正文的假脚注"、正文/注释角色颠倒。改成认「本章文件名#锚点」 | `Epub/parsers/ChapterHtmlSlimParser.cpp` |
| **CJK 扩展区 B 以上不断行** | `utf8IsCjkBreakable` 漏了 Ext D/E/F/G/H，那些字会连成不可断的超长 token。补齐 | `Utf8/Utf8.h` |
| **内嵌图片"压扁"根治** | `DirectPixelWriter` 是上游 **1bpp** 代码（`1<<(7-x&7)`），而本板 framebuffer 是 epdiy 原生 **4bpp**（2 像素/字节）→ 图片只剩 1/4 宽。改成半字节写入 + `uint32_t` 索引（16 位会回绕）+ 旋转系数对齐 `EPD_ROT_INVERTED_PORTRAIT` | `Epub/Epub/converters/DirectPixelWriter.h`、`Jpeg/PngToFramebufferConverter.cpp` |
| **封面 / 大图缩放上限** | 上游把源图尺寸闸死在 2048×3072（2644×3840 的扫描封面被误判成"太大"→ 没封面），真正的闸应该是**缩放后宽度 vs `MAX_BUFFERED_PIXELS`**；源尺寸上限放宽到 `MAX_SOURCE_DIM = 16384`。另新增**待机整屏封面** `generateStandbyCoverBmp` | `JpegToBmpConverter/*`、`PngToBmpConverter/*`、`Epub/Epub.cpp` |
| **冷启动 TOC 索引** | `TOC → spine` 查表是 O(n²)（324 条 TOC × 91 条 spine ≈ 10.2s）；换成**惰性 RAM 镜像 + spine href 哈希索引**（阈值 8），冷启动 12.6s → **2.9s** | `Epub/BookMetadataCache.*` |
| **TXT 章节识别** | 章节正则从工具链移植进引擎（与 `tools/ebook/…/chapter_detect.py` 同源） | `Txt/TxtChapterIndex.*` |
| **Zip / 解压鲁棒性** | 修 `InflateStream` 的忙等自旋；`ZipFile` 改用 4KB、64 字节对齐的读缓冲（SDMMC 直接读进 DMA 缓冲） | `ZipFile/ZipFile.cpp`、`miniz/src/InflateStream.cpp` |
| **微信读书** | 上游的 `WeReadWebApi` 接进引擎，另配 `weread_port/` shim（`MD5Builder`→`esp_rom_md5`、`mbedtls/md5|sha256`→PSA Crypto、Arduino `String`→`std::string`、WiFi/Serial 桩） | `WeReadWebApi/src/`、`weread_port/`、`src/util/`、`Serialization/ObfuscationUtils.*` |
| **词典正文层** | StarDict 正文格式不统一 → 统一走 `htmlToPlainText`，但**无标签时原样返回**（否则纯文本的换行被压成空格） | `Dictionary/DictionaryText.cpp`、`DictionaryRegistry.cpp` |

> 这些改动**改了缓存线的形状或解析结果**，所以每次都要连版本号一起 bump
> （`SECTION_FILE_VERSION` 88 / `CSS_CACHE_VERSION` 15，见 `Section.cpp`、`CssParser.h`）——
> 老 `.bin` / CSS 缓存不清掉会把旧结果读回来。

### 二、建在引擎之上的一层（`main/`）

阅读模式本身（`screen_reader*.cpp`，**6 个 TU**）以及下面这些**全是本仓库新增**：

| 类别 | 内容 |
|---|---|
| **阅读器壳体** | 书架（4 种风格）/ 阅读页 / 阅读菜单 / 目录 / 百分比跳转 / 图片查看器 / 关于页 |
| **弹注** | 阅读页之上的一层浮窗（不切模式）：`FootnoteEntry` 只给序号 + href，**正文现取**——锚点 → `getPageForAnchor` → 该页逐词拼段。菜单 `Enter` 弹注、`→` 跳转；Esc 先收它 |
| **标注 / 笔记** | 长按选句 → 标注/笔记/查字典；锚点靠**选中文本几何**（`g_pageText`）+ 子串重定位，翻页改字号都不错位 |
| **字体层** | `font_renderer` / `ttf_font`：s_faces 角色分家（内容面 / UI 面）、共享格子模型、缺字替补链、图标字体 |
| **推屏与刷新** | `display.c`（全界面唯一推屏落地点）、core1 双缓冲渲染任务、夜间反色、自定义状态栏、**刷新策略**（全局/局刷/快刷/自适应 + 全刷频率 + 换章强制全刷）、8 灰阶两档波形、待机/时钟表盘 |
| **在线** | OPDS 书库、WiFi 传书（80 端口 + 二维码）、词典清单下载 / 校验 / 安装、字体下载 |
| **统计** | 阅读时长、每日页数、热力图、连续天数、单书详情；标注 / 笔记**全库导出** Markdown |
| **平台移植层** | `hal/`（HalDisplay / HalStorage / HalClock）、`crossmux_platform.h`、`Arduino.h` / `Print.h`、`FsApiConstants.h` —— 把引擎从 Arduino 世界接到 ESP-IDF / epdiy |

写作（研墨）与计划（研行）两个模式、拼音输入法、虚拟键盘、触摸编辑层、GTD 均为本仓库代码，
与引擎无关，不在此列。

---

## 虚拟键盘版式

软键盘是**本仓库唯一的实现**（`main/editor_vk.cpp`，约 1800 行），写作 / 计划 / 阅读三个模式共用一套。
交互语义只有一条：**命中测试把点按翻译成普通键码**（`EVK_NONE` / `KEY_LEFT..RIGHT` / 字符码 / 几个
待发状态），交给宿主既有的输入法分支、回车、退格、ASCII 插入逻辑 —— 键盘自己**不重复实现任何输入逻辑**。

### 面板自上而下

```
┌────────────────────────────────────────┐
│ 上行：编码（拼音串 "ni hao"）            │ ← 候选区（总高 imeCandH()）
│ 下行：候选字（独占整行宽度）             │
├────────────────────────────────────────┤
│ 布局 │ Ctl │ ◀ ▲ ▼ ▶ │ Shift │   ⌫     │ ← 额外行（qwerty 之上固定一行）
├────────────────────────────────────────┤
│                                        │
│            键  区（三张面）              │
│                                        │
├────────────────────────────────────────┤
│ ① │ ② │    空格（5 格，图标）  │中/英│↵ │ ← 功能行，恰 10 格
└────────────────────────────────────────┘
```

- **候选区两行**：原来编码与候选挤在一行，编码占掉左边一截、候选只能从它右边排，横屏一行放不下
  几个词就得翻页；分行后候选独占整行宽度，正好等于输入法自己算页宽用的宽度，页码和"一行放得下几个"终于对上。
- **候选区高度随「候选字大小」设置浮动**（`ime_cand_size`）：候选行行高跟候选字高走，面板顶边随之上下移、
  正文可视区跟着变。四档实测：小 65+54 / 标准 65+65 / 大 76+76 / 特大 88+88。
- **额外行**：横竖屏一致，只按 `keyW` 比例缩放。方向键在输入法有编码时选字翻页、无编码时移光标；
  退格从底部功能行挪到这里（右边最容易点到的高频位）。
- **功能行**：`①面板切换 | ②面板切换 | 空格(5 格) | 中/英 | 回车(2 格)`，一行恰好 10 格。空格仍是整行最长的键。

### 键区：三张面

| 面 | 版式 |
|---|---|
| **字母页** | qwerty 三行（`qwertyuiop` / `asdfghjkl` / `zxcvbnm` + `，。`），第三行 9 格满宽，与上一行对齐 |
| **符号面板** | 3 行 × 10 列，就占字母那三行的位置（行高、列宽、格数全不变）：`-/:;()$&@"` / `.,?!'"#%*+` / `~_^<>=[]{}|`。数字不再在符号表里占一整行 |
| **数字面板** | 四行数字盘，仍占同一条"字母行带"（三行均分成四行，行带总高不变）：左列 `# * - +`，中间 `1..9` + 末行横跨三列的宽 `0`，右列 `⌫ . @ /`。左右两列各 1.5 格 |

`①②` 两个键是**面板切换**，标签随当前面板变 —— 字母页 `①符 ②123`、符号面板 `①ABC ②123`、
数字面板 `①符 ②ABC`。三页正好绕一圈：任意一页按两下都能到另外两页，不存在"按了没反应"的键。

### 一键多字母布局（14 / 18 / 9 键）

在 qwerty 之上的一组替代键位，把 1~4 个字母并到一个键上（键面直接写组字母，如 `qw`、`abc`）。
分组表**不在这份代码里**：组号 / 字母 / 标签 / 行布局一律取自 `IME::ambigRowGroup` 与 `IME::AmbigLayout`
（源自万象拼音），阅读模式的键盘读的是同一份，两个键盘不会走样。

行内 **n 个组等宽平分 10 格**，组宽只跟"这一行有几个组"有关：

| 布局 | 行 0 | 行 1 | 行 2 |
|---|---|---|---|
| **14 键** | `qw er ty ui op` → 5 组 × 2 格 | `as df gh jk l` → 5 组 × 2 格 | `zx cv bn m` → 4 组 × 2.5 格 |
| **18 键** | 7 组 × 1.43 格 | 6 组 × 1.67 格 | 5 组 × 2 格 |
| **9 键** | 三行都按 3 列定宽（3.33 格） | | 末行 2 个键居中 = 九宫格 |

不再按字母数比例分配（那会把 `l` / `m` 这种单字母组压到最窄）—— 组里有几个字母已经写在键面上了。
**这类布局只在中文拼音态生效**：英文态 / 没开输入法一律回到 26 键（组合标签那时是错的，白占一行）。

**9 键另有一整套版式**：左列 + 中间 3×3 九宫格（`1` 分词 + `2..9` 字母）+ 右列功能键（`⌫` / 重输 / `0`）。
左列是**双身份**的 —— 没有组合时是四个标点键（`，。？！`），有组合时换成**分音节选择列**（和 T9 面板左列
同一份内容、同一套算法）。九宫格模式下额外行的 `⌫` 已挪进右列，腾出的 1.25 格给 `Shift`。

### T9 候选面板

组合中**点一下编码行** → 候选区之下整块换成 T9 面板（左列挑读音、右面候选宫格 3×3 每页 9 个）；
再点一次收起，组合结束也会自动收起。面板里上下滑：左列滚读音行（右侧有滚动条）、宫格翻候选页。

### 字体与几何

- 键盘字体**钉死在内置面**（`g_vk_font`）：键帽上的字母 / 符号 / 汉字全用内置字体，键位、键宽、
  图标字形都不随"用户选的字体"漂移。候选行除外 —— 那是输入法正文，走 `g_content_font`，照旧跟用户字体。
- 行高 / 行顶边**逐行现算**（不能写成 `row × H`），绘制与命中测试共用同一份函数，避免两处几何各算一遍慢慢错位。
- 命中判定是**中心优先**：点必须落在键的内切椭圆内，矩形四角是死区；字母/符号三行特意加高（指尖纵向覆盖约 80px），
  把"按 A 出 B"换成少一行正文。

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
