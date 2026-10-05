# tests/host/layout — 折行几何的主机端验证

验证「**让 ASCII 按字体真实字宽排版**（废弃「ASCII = 半格」，见
`/home/ywz/.claude/plans/federated-herding-wave.md`）」没有把版式弄坏。纯 `g++`，
不需要 ESP-IDF、不需要 `builtin.ttf`、不需要设备。

被测的是**折行几何**：`charAdvancePx` / `byteToX` / `xToByte` / `mdIndentPx` /
`buildVrows`（从 `main/ui_helpers.cpp` 原样抄进 `layout_equiv.cpp`），对照物是改造前
那版（`gen_old.py` 从 `git HEAD:main/ui_helpers.cpp` 自动抽，见下）。

不复用真 TU 的理由与 `tests/host/rotation/rot_equiv.cpp` 相同：`main/ui_helpers.cpp`
拖着 wifi / bt / display / epdiy / FreeRTOS 一大串，桩件量级远超被测代码本身。
几何算法可以整段搬，搬过来的字节与原文件一致。

## 跑

```sh
tests/host/layout/run.sh                 # 默认档 px=45 屏宽 960
tests/host/layout/run.sh --all           # 再跑一遍 px=50（偶数档 → [4] 的严格等价路径）
tests/host/layout/run.sh --px 41 --screen 684
```

退出码非 0 = 有断言挂。只写 `tests/host/layout/build/`（已 gitignore）。

## 查什么

| | 断言 |
|---|---|
| [1] | 量画自洽：`Σ charAdvancePx(cp) == g_font.textWidth(s)`。自由函数那份规则不许与实例版分家 |
| [2] | 往返：任意 `start/end` 窗口下 `xToByte(line, s, e, byteToX(line, s)) == s` |
| [3] | 折行不变量：每条 vrow 装得下、**极大**（空格断行除外）、首尾相接铺满整行、pos 必前进 |
| [4] | 内置面下与改造前逐条对拍（**格高偶数时严格相同**；奇数档只许变紧，见下） |
| [5] | 边界：空行 / 纯空格 / 一个字比整行宽 / 首行缩进模式 |
| [6] | 外置面下新旧**必须**有分歧 —— 护栏只证「没坏」，这条证「改了」 |
| [7] | 输入法编码行的例外：装了外置字体，`latin_builtin_` 实例的拉丁仍走半格 |
| [8] | 记号 lockstep：折行预留 `mdIndentPx` == 渲染前缀 `mdPrefixAdvancePx` |

## 两条容易误读的结论

**一、[4] 在奇数格高下不是"逐像素零变化"。** 旧口径 CJK = 2 格 = `2×⌊h/2⌋` px，新口径
CJK = `cjkAdvance()` = `h` px，两者只在 `h` 为偶数时相等。`h=45`（20pt 默认）与 `h=41`
（18pt）都是奇数，所以每个汉字差 1px，屏幕预算 946px —— 贴着右缘的行可能**提前一个
字符换行**（实测：屏宽 960 语料 0 行、屏宽 684 语料 2 行）。这不是新引入的偏差，而是
把旧代码里就存在的量画分家收拢了：旧代码**画**汉字用 `charWidth(CJK) = line_height`，
**量**折行却按 2 格算，wrap 每个汉字少数 1px。[4] 的奇数档因此只断言"只许变紧"。

**二、`gen_old.py` 让对照物不会过期。** 对照物每次从 `git HEAD` 重新抽。手抄一份放在
测试里，改了正文忘了同步，护栏会一直"绿"得没有意义。git 不可用时它删掉 `.inc`，
`layout_equiv.cpp` 的 `__has_include` 落空 → [4]/[6] 自动跳过并打印一声。

## 文件

| 文件 | 作用 |
|---|---|
| `layout_equiv.cpp` | 桩环境（假 FontRenderer / settings / ttf 步进表）+ 抄来的被测几何 + 八组断言 |
| `gen_old.py` | 从 `git HEAD` 抽改造前的折行几何 → `build/old_layout.inc` |
| `run.sh` | 生成对照物 → 编译 → 跑 |

`layout_equiv.cpp` 里 `StubFont::charWidth/textWidth/utf8Decode` 是
`main/font_renderer.cpp` 的**逐字抄写**（只去掉程序化状态图标那两支）—— [1] 靠这一点
才有意义：它比的是"自由函数那份规则"与"实例那份规则"，两边都是真身正文。

## 没有覆盖的

* 真 TTF 的步进值（这里是一张假的比例表；真值来自 `main/font/ttf_font.c` 的
  `f_ascii_adv`）。要测真值得在设备上或挂 `builtin.ttf`。
* `mdClassifyLines`（本文件里是桩）、折叠标题（测试一律传 `foldedHeadings=nullptr`）。
* 光标 / 选区 / 触摸命中的 x→字节（那些在 `screen_editor.cpp`，只验了它们依赖的
  `byteToX/xToByte` 本身）。
