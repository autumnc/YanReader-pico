/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 可变 TTF：卡上按需读扇区，glyf/gvar 能装下就整表进 PSRAM；字形按
 * codepoint/字号/字重缓存。内置字体是 ChillDuanSans 子集。
 *
 * Variable TTF: sector I/O from the card; glyf/gvar map into PSRAM when
 * they fit. Glyphs are cached by codepoint, size and weight. The built-in
 * font is a ChillDuanSans subset.
 */

#include "ttf_font.h"

#include "fb_fast.h"   // 字形逐像素直写：绕开 epd_draw_pixel 的逐像素旋转/边界开销
// 注：下面几个定义的 align 形参写 int 而不是 `enum EpdFontFlags` —— 声明和定义必须一致，
// 而 ttf_font.h 刻意不带 epdiy（见那边的 P1.3 说明），对齐标志用它自己的 TTF_ALIGN_*。

#include <dirent.h>
#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "font_store.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

static void* ttf_malloc(size_t size, void* userdata) {
    (void)userdata;
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static void ttf_free(void* ptr, void* userdata) {
    (void)userdata;
    heap_caps_free(ptr);
}

#define STBTT_malloc(x, u) ttf_malloc((x), (u))
#define STBTT_free(x, u) ttf_free((x), (u))
#define STBTT_assert(x) do { if (!(x)) ESP_LOGE("ttf", "assert %s", #x); } while (0)
// 不定义 STBTT_STATIC：让 stb 实现保持 extern。read_pico 原版只有 ttf_font.c
// 一个 TU 用 stb，故定义为 static；本移植新增 gfx/icon_font.c 也调用 stbtt_*，
// 须共享这一份实现（仍用 ttf_malloc 的 PSRAM 分配器），否则链接缺符号。
#define STB_TRUETYPE_IMPLEMENTATION
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "stb_truetype.h"
#pragma GCC diagnostic pop

/* ---- 调参 / Tunables ---- */
// 光栅化位图 LRU 的**上限**(PSRAM)。真正能用多少由 cache_reserve() 现算：
// 上限 ≠ 实际额度，因为这块内存同时是阅读器图片解码、解压窗口、网络缓冲的地盘。
// 为什么从 1536KB 抬上来：实测翻页时 15-54/页 未命中，每页要额外读 50-140ms 的
// 散块 I/O(一页字形块撒在 15-23MB 范围里，块级合并救不了，只能靠少去读)。
// 缓存能装下的字数大致就是未命中的来源，所以这里是现在唯一还有用的杠杆。
#define TTF_CACHE_LIMIT (3 * 1024 * 1024)
// 给别的模块留的 PSRAM：缓存只在"空闲量高于这个"时继续长，低于就停在当前大小
// (只淘汰到刚好放下新字形，不缩回)——缩回去只会让图像反复重光栅化。
// 1.5MB 是阅读器的经验水位(图片/解压/OPDS 缓冲)。参考 cache_reserve()。
//
// ⚠ 上面那句"不缩"只描述了**留量线以上**。线上以下 room 归零，cache_reserve() 的
//   判据是"cache_bytes + extra > limit"，而 limit 恰好等于进入时的 cache_bytes ——
//   于是**每插入一个字形就淘汰掉同等字节**；缓存只剩 2K(=一个字形)时等于每次插入都
//   整块冲光。这不是 bug 而是那条判据的自然后果，但"不缩"这个说法让人以为缓存会
//   冻住在当前大小，实际是冻住在**零**。要治它得给缓存一个地板(见 ttf_font.c 的
//   TTF_PAGE_BITMAP_BYTES 那段讨论)，本轮先只把"装不下就别暖"这条止损做掉。
#define TTF_CACHE_RESERVE (1536 * 1024)
// 一页字形在位图缓存里的**总字节** —— 判"这一趟栅格化预热划不划算"的线。
//
// 为什么需要这条线：设一页去重后 N 个字形、缓存装得下 K 个，那么
//   热身+绘制 = N（热身时缓存是空的，全 miss）+ (N−K)（被热身挤出去的那些绘制时要重来）
//   不热身   = N
// **K < N 时热身严格更亏**，亏 N−K 次栅格化。不是"收益打折"，是净亏。
//
// 数值是**量出来的**，不是推的：内嵌关那一档稳态缓存 634–712–794K、位图命中 96%、
// 每页去重后约 440 字 → 约 1.45KB/字。也就是说内嵌关那档的稳态缓存**正好就是一页**，
// 这就是它 96% 命中的来源。取 440×1.45K ≈ 640K。
// 判据用"≥ 一整页"，因为真正的需求是**整页**都要装下：预热是按 384 字节一块分很多次
// 调的，只判单块(约 130 字 ≈ 190K)会把"装得下半页"误判成够用。
// ⚠ 与未来的地板必须成对调：地板 < 这条线时预热永远是白做，地板 ≥ 这条线时预热才回本
//   （那时 warm_raster_skip 必须回到 0，这是一个自洽的双向检验点）。
#define TTF_PAGE_BITMAP_BYTES (640 * 1024)
// UI 面的字形缓存上限。内建面常驻 flash(font_mem != NULL)，未命中 = memcpy + 光栅，
// 没有 SD I/O —— 淘汰只花 CPU 不花毫秒。一个菜单/设置/对话框屏约需 100-200 个
// 不同 CJK@38-46px(≈2.1KB/位图) ≈ 250-400KB，512KB 留约 2 倍余量。
// 实测某屏反复重光栅化(翻页/进界面偶发卡顿)就把这里提到 768KB，PSRAM 有的是。
#define TTF_UI_CACHE_LIMIT (512 * 1024)
// 次字面(书内第二个家族，如祖堂集的仿宋)的字形缓存上限。它只画注文/引文 —— 一页里
// 的比例小、字面重复度高，1MB 足够罩住一章；真正的额度仍由 cache_reserve() 按空闲量
// 现算，这里只是天花板。
#define TTF_ALT_CACHE_LIMIT (1024 * 1024)
// 两个"书内家族"面(次家族/三家族)的 SD 块缓存与合并读窗口。它们都是**配角**：正文
// 走内容面，那才是每页的大头，所以给的比内容面(128 槽/512KB)小得多 —— 主面的字形
// 缓存才是决定翻页速度的东西，不能让它被配角挤掉。
//
// 数值是**量出来的**，判据是新加的 `轮峰`(= io_flush_touches 去重之后的单轮载入峰值，
// 见 ttf_face_stats_t)：一轮超过槽数的部分就是最早载入的那几块**当场被自己挤走**，
// 下一趟绘制再当白读重来 —— 正文面 96→128 当年修的就是这个毛病，别在配角上重犯。
// 实测(/tmp/turn_font6_20261014.log；晋书 307925051 两面 + 1190208750)：
//   次家族 轮峰 max 8（n=3 轮）/ 三家族 轮峰 max 4（n=10 轮）——两者都只有 48 的零头。
// 于是各留 3~4 倍余量：次家族 24 槽、三家族 16 槽。合计 2×224KB → 112+80KB，省下
// 256KB **全部落进"开书那一刻的 room"**，也就是位图缓存能不能长到一页
// (TTF_PAGE_BITMAP_BYTES) 的那笔钱，而那正是绘制的开销来源：同机实测晋书的位图缓存
// 被冻在 142K、另一本冻在 473K，绘制的 1000ms vs 700ms 差的就是它。
// ⚠ 样本薄：次家族只在晋书里出现，只有 3 轮。所以是"留 3 倍"而不是贴着 8 给。
//   `轮峰` 超过对应槽数就是挤掉过的直接证据，再往下砍之前先看那个数。
#define TTF_ALT_IO_SLOTS 24
#define TTF_ALT_IO_RUN_MAX 4
#define TTF_ALT2_IO_SLOTS 16
#define TTF_ALT2_IO_RUN_MAX 4
// 哈希桶数。/ Hash buckets.
#define TTF_CACHE_BUCKETS 256
// 两档字号的像素高，对齐 ui_kit 正文/标题。/ Small/large px, matches ui_kit body/title.
#define TTF_PIXEL_SMALL 43
#define TTF_PIXEL_LARGE 67
// 打开字体时顺读进 PSRAM 的 glyf 切片上限。/ Max glyf slice mapped into PSRAM at open.
#define TTF_GLYF_ARENA 131072
// 复合字形展开深度。/ Composite glyph walk limit.
#define TTF_TREE_MAX 48
// 简单轮廓点数；变体表多 4 个幻点。/ Simple-glyph points; gvar adds 4 phantom points.
#define TTF_MAX_PTS 512
#define TTF_MAX_VAR_PTS (TTF_MAX_PTS + 4)
// 单字形 gvar 切片缓冲。/ Per-glyph gvar slice buffer.
#define TTF_GVAR_SLICE_MAX 2048
// wght 轴：ChillDuanSans 常用区间，默认最轻。/ wght axis used by ChillDuanSans; default is lightest.
#define TTF_WGHT_MIN 300
#define TTF_WGHT_MAX 800
#define TTF_WGHT_DEF 300
// FatFs 扇区是 4KB；块缓存装的是"最近被碰过的扇区"。
// 实测(SD 卡 30MB 字体，34px 正文)：一页正文的轮毂工作集约 70 块/278KB，而每次读
// 平均只有 ~5KB、其中一半时间花在 lseek 上(FATFS 在 30MB 文件上要走簇链)。也就是说
// 瓶颈是"随机小读的次数"，不是带宽。所以这里的目标是**让每次预取多带几块回来**：
// 槽位要够装下"整页轮毂一轮读进来"（70 块 + 合并读填的白读空隙，合并规则把填空压到
// 需求块数的一半以内 → 一轮最多约 105 块），否则刚读进来的块立刻被自己挤掉，
// 白花一次 SD 读。
//
// **为什么是 128 而不是更小**：这样定过一次 96(512KB → 384KB，想给字体工作缓冲省连续
// PSRAM)，上机一量就退回来了 —— 那本书上 143 轮整页预取里有 1 轮载入 **103 块**(去重后
// 89 块，可见超出的 14 块全是合并读填的白读空隙)，96 < 103，轮内 LRU 就得挤出最早载入
// 的 7 块(恰好可能是最早那几个真用到的)，切回去要重读一次 SD。103 也正好印证上面那条
// "一轮最多约 105 块"的界，所以 128(≥105) 才是覆盖这个界的那一档，96 从一开始就低于它。
// 省下的 128KB 当时想省给字体工作缓冲，但那条路已被 glyf 整表映射修好(见下)，实测
// PSRAM free/largest 都是 3.8MB，这 128KB 攒着没处用 —— 而代价是**翻页变慢**，
// 按用户口径(点下去到屏幕开始变那一段)不能吃这个亏。要再动这个数，先照下面那句量：
// 上机翻页时看 `io_flush_touches` 那行(现在已收回，要复现就临时加回 ESP_LOGI)，
// N 逼近或超过 slots 就说明挤掉过。
#define TTF_IO_BLOCK 4096
#define TTF_IO_SLOTS 128
// 预取时合并的连续块上限。/ Max contiguous blocks merged in one prefetch read.
#define TTF_IO_RUN_MAX 32
// 相邻被碰块之间最多隔几块也并进同一次读(中间几块一起读掉，用白读换掉一次寻道)。
#define TTF_IO_GAP_BLOCKS 2
// 块缓存分配失败之后的退避窗口：这段时间内不再反复尝试(与不再反复打 WARN)。
// 取 2 秒与主循环里"翻页提速缓冲要不到就隔 2s 重试"同一套口径 —— 那几块要的是
// 一整块连续 PSRAM，失败原因也同源(碎片化)，所以用同一个时间尺度。
#define TTF_IO_RETRY_US (2 * 1000 * 1000)
#define TTF_PREFETCH_MAX 512
#define TTF_GATHER_MAX 512
// glyf+gvar 整表映射的预留与硬顶。/ Reserve and hard cap for whole-table glyf+gvar maps.
#define TTF_MAP_RESERVE (2560u * 1024u)
#define TTF_MAP_MAX (5u * 1024u * 1024u)

static const char* TAG = "ttf_font";

// 字体目录。目录名大小写不敏感：本配置下 FAT 区分大小写，SD 卡上可能是
// Fonts/FONTS 而非 fonts，所以不写死字面路径，改为枚举父目录按 strcasecmp 匹配。
#define TTF_FONT_DIR_MAX 4
static char s_font_dirs[TTF_FONT_DIR_MAX][TTF_FONT_PATH_MAX];
static int s_font_dir_n;

static void add_font_dir(const char* path) {
    if (s_font_dir_n >= TTF_FONT_DIR_MAX) return;
    for (int i = 0; i < s_font_dir_n; i++) {
        if (strcmp(s_font_dirs[i], path) == 0) return;
    }
    strlcpy(s_font_dirs[s_font_dir_n], path, TTF_FONT_PATH_MAX);
    s_font_dir_n++;
}

// 在 parent 下找名为 want 的子目录（大小写不敏感），命中则写完整路径。
static bool resolve_child_dir(
    const char* parent, const char* want, char* out, size_t out_sz
) {
    DIR* dir = opendir(parent);
    if (dir == NULL) return false;
    bool found = false;
    struct dirent* ent;
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        if (strcasecmp(ent->d_name, want) != 0) continue;
        char cand[TTF_FONT_PATH_MAX * 2];
        snprintf(cand, sizeof(cand), "%s/%s", parent, ent->d_name);
        if (strlen(cand) >= out_sz) continue;  // 过长，无法表示
        struct stat sb;
        if (stat(cand, &sb) != 0 || !S_ISDIR(sb.st_mode)) continue;
        strlcpy(out, cand, out_sz);
        found = true;
        break;
    }
    closedir(dir);
    return found;
}

// 收集字体目录：/sdcard/<assets>/<fonts> 与 /sdcard/<fonts>，两段都大小写不敏感。
// 末尾再追加字面路径兜底（大小写不敏感的文件系统上会与枚举结果指向同一目录，
// 重复项由 catalog_has_name 按字体名去重）。
static void build_font_dirs(void) {
    s_font_dir_n = 0;
    char p[TTF_FONT_PATH_MAX];
    if (resolve_child_dir("/sdcard", "assets", p, sizeof(p))) {
        char q[TTF_FONT_PATH_MAX];
        if (resolve_child_dir(p, "fonts", q, sizeof(q))) add_font_dir(q);
    }
    if (resolve_child_dir("/sdcard", "fonts", p, sizeof(p))) add_font_dir(p);
    add_font_dir("/sdcard/assets/fonts");
    add_font_dir("/sdcard/fonts");
}

typedef struct glyph_entry {
    uint32_t codepoint;
    uint8_t size;
    uint16_t weight;
    // 这张位图是"合成加粗多少"栅出来的（见 synth_dw_now）。缓存键的一部分：同一码点同一
    // 字号在"正文粗/外壳标准"两种作用域下是两张不同的位图，必须分开存。
    int16_t synth_dw;
    int16_t width;
    int16_t height;
    int16_t left;
    int16_t top;
    int16_t advance_x;
    uint8_t* bitmap;
    size_t bitmap_bytes;
    struct glyph_entry* hash_next;
    struct glyph_entry* lru_prev;
    struct glyph_entry* lru_next;
} glyph_entry_t;

typedef struct {
    float scale;
    int ascent;
} ttf_size_metrics_t;

typedef struct {
    char tag[4];
    uint32_t offset;
    uint32_t length;
} sfnt_table_t;

/* ---- SD 分块 IO / SD block I/O ---- */
// 字形/gvar 仍按需从文件取。FatFs 扇区是 4KB，但每个新字往往落在不同扇区，
// 1-bit SD 上一次随机读就要好几毫秒。这里用 32 个 4KB 槽把最近扇区留在
// PSRAM；画一行字之前按文件偏移把用到的块排序合并再读。glyf/gvar 若装得
// 下，打开字体时顺读进 PSRAM，之后冷启动不再碰卡。
// Glyphs/gvar are still fetched on demand. FatFs sectors are 4KB, but each
// new glyph usually sits on a different sector; a random 1-bit SD read costs
// several milliseconds. 32×4KB PSRAM slots keep recent sectors. Before a
// line is drawn, needed blocks are sorted and merged. If glyf/gvar fit,
// they are streamed into PSRAM at open so a cold start does not touch the card.
// 光栅化脚手架(缓冲都在 PSRAM 堆上)。它**不属于任何字面**：单线程、一次只画一个字，
// 两面共用一份即可 —— 换面只发生在两个字形之间，绝不会发生在一次光栅化中间。
typedef struct {
    uint8_t slice[TTF_GVAR_SLICE_MAX];
    int16_t x[TTF_MAX_PTS];
    int16_t y[TTF_MAX_PTS];
    int16_t endpts[64];
    uint8_t on[TTF_MAX_PTS];
    uint8_t flags[TTF_MAX_PTS];
    float dx[TTF_MAX_PTS];
    float dy[TTF_MAX_PTS];
    float tdx[TTF_MAX_PTS];
    float tdy[TTF_MAX_PTS];
    uint8_t setx[TTF_MAX_PTS];
    uint8_t sety[TTF_MAX_PTS];
    int16_t xdel[TTF_MAX_VAR_PTS];
    int16_t ydel[TTF_MAX_VAR_PTS];
    uint16_t pts[TTF_MAX_VAR_PTS];
    uint16_t shared_pts[TTF_MAX_VAR_PTS];
} ttf_work_t;

/* ══════════════════════════════════════════════════════════════════════════
 * 字面(face)：一份"当前字体"的全部状态。
 *
 * 从前这些是一个个文件级 static，全工程只有一份，于是换字体 = 换整屏的字。
 * 现在分成两份角色：CONTENT(用户所选字体) 与 UI(恒为内置)。两者靠 s_cur 切换。
 *
 * ── 下面那一片 #define 是什么 ──
 * 本文件有约 2000 行代码，通篇直接读写这些名字(font_fd = ... / if (!font_ready))。
 * 把成员收进结构后，为了**一行都不用改**那些代码，用同名的宏把 `name` 重定向到
 * `s_cur->name`。C 预处理器对"宏自身替换表里出现的同名记号"会涂蓝(painted blue)，
 * 不再二次展开 —— 所以 `#define font_fd (s_cur->font_fd)` 不会自递归。
 *
 * ── 三条铁律，改动本文件前必须知道 ──
 * 1. `s_cur` 只在 face_enter() 与 ttf_set_role() 两处被赋值，别处一律不许写。
 * 2. 本文件内部**永远不调用 ttf_set_role()**(那是给绘制层的)。这是"一次光栅化
 *    中间绝不会换面"的全部保证 —— 文件里好几处跨调用持有指向字面内部的指针
 *    (apply_gvar_* 的 work->slice、pack_glyph_tree 的 font_data + work_glyf_off、
 *    try_map_table 的 &glyf_ram、ttf_draw_text_px 里的 glyph)，全靠这条。
 * 3. 新增一个"每个字体一份"的 static，必须搬进下面这个结构并补一条同名宏；
 *    只有"与字体无关"的(目录表/清单/bench/字面本身)才留在外面。
 *    另：宏名不能与既有结构成员撞车 —— glyph_entry_t(codepoint/size/weight/width/
 *    height/left/top/advance_x/bitmap)、sfnt_table_t(tag/offset/length)、
 *    ttf_size_metrics_t(scale/ascent) 里出现过的名字一律不能用。
 * ══════════════════════════════════════════════════════════════════════════ */
// ASCII(<0x80) 步进表的项数。表本身见 ttf_face_t 的 f_ascii_adv。
#define TTF_ASCII_ADV_N 128

typedef struct ttf_face {
    int f_font_fd;
    const uint8_t* f_font_mem;
    uint32_t f_font_mem_len;
    uint32_t f_font_file_pos;
    char f_font_path[TTF_FONT_PATH_MAX];
    int f_packed_root;
    int f_packed_weight;
    uint32_t f_file_glyf_off;
    uint32_t f_file_glyf_len;
    uint8_t* f_file_loca;
    uint32_t f_file_loca_len;
    bool f_loca_long;
    int f_num_glyphs;
    uint8_t* f_glyf_ram;
    uint32_t f_glyf_ram_off;
    uint32_t f_glyf_ram_len;
    uint8_t* f_gvar_ram;
    uint32_t f_gvar_ram_off;
    uint32_t f_gvar_ram_len;
    uint8_t* f_io_data;
    uint8_t* f_io_run_buf;
    // 块缓存上次**没要到**之后，允许再试的时刻(us；0 = 随时可以试)。见 io_ensure()。
    // 为什么是"退避"而不是"放弃"：io_data==NULL 的读路径每读一块都会调回 io_ensure()，
    // 不挡住就是低内存下每次块读一条 WARN；但**永久放弃的代价比日志刷屏大得多** ——
    // 这一轮要不到多半只是临时的碎片化(一页图片解码/一段网络缓冲正占着 largest)，
    // 永久放弃等于把整本书退化到逐块直读，那是每页几十~几百 ms，而且没有任何提示。
    // ttf_face_purge_io() 把它清回 0，因为那是一次**显式**释放，之后当然该立刻再试。
    int64_t f_io_retry_at_us;
    uint32_t f_io_base[TTF_IO_SLOTS];
    uint16_t f_io_fill[TTF_IO_SLOTS];
    uint16_t f_io_age[TTF_IO_SLOTS];
    uint16_t f_io_clock;
    // 本面实际开几个槽/合并窗口多大。数组按 TTF_IO_SLOTS 编译期开满(512B 内部 RAM)，
    // 但**分配和轮转只走到这里** —— 次字面要小得多，不能照抄内容面的 640KB。
    uint16_t f_io_slots;
    uint16_t f_io_run_max;
    // 待预取块号表。放 PSRAM(与 io_data 同一次分配、同样永不释放)：一页预取要去重
    // 几百块，512 个 uint32 直接摆进结构里会把这 4KB 的内部 RAM 预算顶穿
    // (见下面的 _Static_assert)。
    uint32_t* f_touch_blocks;
    // 预取时的码点去重表(码点 → 已收)。同样是几百项，放 PSRAM：TTF_GATHER_MAX 从
    // 96 抬到 512 后摆在栈上要 2KB，而每个 ttf_draw_text_px 都会走一趟这里。
    uint32_t* f_warm_cps;
    int f_touch_n;
    uint8_t* f_font_data;
    uint32_t f_work_loca_off;
    uint32_t f_work_glyf_off;
    stbtt_fontinfo f_font_info;
    bool f_font_ready;
    ttf_size_metrics_t f_size_metrics[2];
    int f_raw_ascent_units;
    int f_current_weight;
    int f_wght_min;
    int f_wght_def;
    int f_wght_max;
    uint32_t f_file_gvar_off;
    uint32_t f_file_gvar_len;
    uint32_t f_gvar_data_array_off;
    uint32_t* f_gvar_glyph_off;
    int16_t f_shared_tuple_f2dot14[8];
    int f_shared_tuple_count;
    int f_gvar_axis_count;
    bool f_gvar_ready;
    glyph_entry_t* f_cache_buckets[TTF_CACHE_BUCKETS];
    glyph_entry_t* f_lru_head;
    glyph_entry_t* f_lru_tail;
    size_t f_cache_bytes;
    size_t f_cache_limit;   // 本面字形缓存上限，见 cache_reserve()
    // ASCII 步进表：见下面的 ascii_adv_ensure()。256B/面、三面共 768B，占内部 RAM
    // (与结构体其他字段一样在 .bss)，所以若哪天顶穿 _Static_assert 就先把它改成
    // uint8_t[128]（px 上限 120，步进一定 < 256）。
    int16_t f_ascii_adv[TTF_ASCII_ADV_N];
    int f_ascii_adv_px;     // 建表用的像素高；0 = 失效(真实 px ≥ 12，不会撞上)
} ttf_face_t;

// 结构本身要占内部 DRAM(不进 PSRAM，.bss 段)。超了就先砍 cache_buckets 再说。
_Static_assert(sizeof(ttf_face_t) < 4096, "ttf_face_t 超出内部 RAM 预算");

// 注意：成员一律带 `f_` 前缀 —— 上面那片 #define 是对象式宏，名字与成员同名，
// 若成员就叫 font_fd，则任何显式的 `s_faces[..].font_fd` 都会被展开成
// `s_faces[..].(s_cur->f_font_fd)` 而语法报错。加前缀后宏名与成员名永不相撞。
// BSS 清零后 font_fd/-1、font_file_pos/UINT32_MAX、packed_root/-1、current_weight
// 这几项都是"零不是对的值"，必须显式设一遍。
static void face_defaults(ttf_face_t* f, int role) {
    memset(f, 0, sizeof(*f));
    f->f_font_fd = -1;
    f->f_font_file_pos = UINT32_MAX;
    f->f_packed_root = -1;
    f->f_packed_weight = -1;
    f->f_wght_min = TTF_WGHT_MIN;
    f->f_wght_def = TTF_WGHT_DEF;
    f->f_wght_max = TTF_WGHT_MAX;
    f->f_current_weight = TTF_WGHT_DEF;
    // 两个"书内家族"槽**字形缓存**同待遇（都是零星出现的注文/引文，量比正文小）；
    // **io 槽位不同档**，见 TTF_ALT_IO_SLOTS —— 那个数只能按量出来的 `轮峰` 给。
    const bool is_book_alt = (role == TTF_ROLE_CONTENT_ALT || role == TTF_ROLE_CONTENT_ALT2);
    f->f_cache_limit = (role == TTF_ROLE_UI)     ? TTF_UI_CACHE_LIMIT
                       : is_book_alt             ? TTF_ALT_CACHE_LIMIT
                                                 : TTF_CACHE_LIMIT;
    f->f_io_slots   = role == TTF_ROLE_CONTENT_ALT  ? TTF_ALT_IO_SLOTS
                      : role == TTF_ROLE_CONTENT_ALT2 ? TTF_ALT2_IO_SLOTS
                                                      : TTF_IO_SLOTS;
    f->f_io_run_max = role == TTF_ROLE_CONTENT_ALT  ? TTF_ALT_IO_RUN_MAX
                      : role == TTF_ROLE_CONTENT_ALT2 ? TTF_ALT2_IO_RUN_MAX
                                                      : TTF_IO_RUN_MAX;
}

static ttf_face_t s_faces[TTF_ROLE_COUNT];
static ttf_face_t* s_cur = &s_faces[TTF_ROLE_CONTENT];

// 每个字面成员一条，拼写与成员名相同 —— 于是 2000 行既有代码原样可用。
#define font_fd               (s_cur->f_font_fd)
#define font_mem              (s_cur->f_font_mem)
#define font_mem_len          (s_cur->f_font_mem_len)
#define font_file_pos         (s_cur->f_font_file_pos)
#define font_path             (s_cur->f_font_path)
#define packed_root           (s_cur->f_packed_root)
#define packed_weight         (s_cur->f_packed_weight)
#define file_glyf_off         (s_cur->f_file_glyf_off)
#define file_glyf_len         (s_cur->f_file_glyf_len)
#define file_loca             (s_cur->f_file_loca)
#define file_loca_len         (s_cur->f_file_loca_len)
#define loca_long             (s_cur->f_loca_long)
#define num_glyphs            (s_cur->f_num_glyphs)
#define glyf_ram              (s_cur->f_glyf_ram)
#define glyf_ram_off          (s_cur->f_glyf_ram_off)
#define glyf_ram_len          (s_cur->f_glyf_ram_len)
#define gvar_ram              (s_cur->f_gvar_ram)
#define gvar_ram_off          (s_cur->f_gvar_ram_off)
#define gvar_ram_len          (s_cur->f_gvar_ram_len)
#define io_data               (s_cur->f_io_data)
#define io_run_buf            (s_cur->f_io_run_buf)
#define io_retry_at_us        (s_cur->f_io_retry_at_us)
#define io_base               (s_cur->f_io_base)
#define io_fill               (s_cur->f_io_fill)
#define io_age                (s_cur->f_io_age)
#define io_clock              (s_cur->f_io_clock)
#define io_slots              (s_cur->f_io_slots)
#define io_run_max            (s_cur->f_io_run_max)
#define touch_blocks          (s_cur->f_touch_blocks)
#define warm_cps              (s_cur->f_warm_cps)
#define touch_n               (s_cur->f_touch_n)
#define font_data             (s_cur->f_font_data)
#define work_loca_off         (s_cur->f_work_loca_off)
#define work_glyf_off         (s_cur->f_work_glyf_off)
#define font_info             (s_cur->f_font_info)
#define font_ready            (s_cur->f_font_ready)
#define size_metrics          (s_cur->f_size_metrics)
#define raw_ascent_units      (s_cur->f_raw_ascent_units)
#define current_weight        (s_cur->f_current_weight)
#define wght_min              (s_cur->f_wght_min)
#define wght_def              (s_cur->f_wght_def)
#define wght_max              (s_cur->f_wght_max)
#define file_gvar_off         (s_cur->f_file_gvar_off)
#define file_gvar_len         (s_cur->f_file_gvar_len)
#define gvar_data_array_off   (s_cur->f_gvar_data_array_off)
#define gvar_glyph_off        (s_cur->f_gvar_glyph_off)
#define shared_tuple_f2dot14  (s_cur->f_shared_tuple_f2dot14)
#define shared_tuple_count    (s_cur->f_shared_tuple_count)
#define gvar_axis_count       (s_cur->f_gvar_axis_count)
#define gvar_ready            (s_cur->f_gvar_ready)
#define cache_buckets         (s_cur->f_cache_buckets)
#define lru_head              (s_cur->f_lru_head)
#define lru_tail              (s_cur->f_lru_tail)
#define cache_bytes           (s_cur->f_cache_bytes)
#define cache_limit           (s_cur->f_cache_limit)
#define ascii_adv             (s_cur->f_ascii_adv)
#define ascii_adv_px          (s_cur->f_ascii_adv_px)

// 本面实际开几个 IO 槽 / 合并窗口多大（面未初始化时退回编译期上限，保证循环不越界）。
static int io_slot_count(void) { return io_slots > 0 ? io_slots : TTF_IO_SLOTS; }
static int io_run_cap(void) { return io_run_max > 0 ? io_run_max : TTF_IO_RUN_MAX; }

/* ---- 临时诊断：字形路径的计数（只给 reader_perf_log 那条日志用）------------
 * 内嵌多字体的书翻页慢，量出来的每一笔都指向"每画一个字就从 SD 取一次轮廓"
 * （见 ttf_perf_line() 的注释）。这组计数就是为了把这句话证实/证伪：一次翻页
 * 里 打包(真要读轮廓) / 块命中·未命中(块缓存) / 段读·白读(合并读进来其实已在
 * 缓存里的块) / 位图命中·未命中(字形位图缓存) 各是多少次。
 *
 * 全部只在取数时读，绘制与缓存逻辑一行不改；逐字面分开记，因为内嵌多字体那一
 * 档怀疑就在内容面，而替补面(家族面/内建)会把这个数摊薄。
 * ────────────────────────────────────────────────────────────────────────── */
typedef struct {
    uint32_t pack_read;    // pack_glyph_tree 真去 file_read_at 取轮廓
    uint32_t pack_memo;    // pack_glyph_tree 单格备忘命中（省掉这次取）
    uint32_t io_hit;       // io_load_block: 块已在缓存
    uint32_t io_miss;      // io_load_block: 真的读了一个 4KB 块
    uint32_t run_reads;    // io_load_run: 无条件整段读的次数（span ≥ 2）
    uint32_t run_blocks;   // io_load_run: 这些整段读一共读进多少块
    uint32_t run_waste;    // 其中**本来就在缓存里**的块数 —— 纯白读
    uint32_t bmp_hit;      // get_glyph: 字形位图缓存命中（不碰轮廓）
    uint32_t bmp_miss;     // get_glyph: 走 rasterize_glyph
    uint32_t warm_skip;    // warm_text_io: 位图命中，跳过（连块都不查）
    uint32_t warm_touch;   // warm_text_io: 位图没命中，要去碰块
    // 位图缓存装不下一页时 ttf_raster_text_px 早退的次数（见 TTF_PAGE_BITMAP_BYTES）。
    // 它是"缓存装不下一页"的直接证据：0 = 有缓存可暖（那一趟热身是回本的），
    // 每趟 +1 = 整场都在白烧 CPU。地板改上去之后它必须回到 0。
    uint32_t warm_raster_skip;
    // io_flush_touches 去重之后一轮最多载入多少块的历史峰值。判"槽位够不够装下一轮"
    // 只能靠它：同一轮里 io_load_run 给新块最新 age，所以超过槽数的部分就是**最早载入
    // 的那几块当场被自己挤走**，下一趟绘制再当白读重来（96→128 那次修的就是这个）。
    // 注意 段N读M块 是**累计和**（N 次 run 共读 M 块），不是单轮峰值，别拿它判槽位。
    uint16_t flush_peak;
} ttf_face_stats_t;

static ttf_face_stats_t s_stats[TTF_ROLE_COUNT];
#define STATS (s_stats[s_cur - s_faces])

/* ---- 与字面无关的全局：不进上面的结构 ---- */
extern const uint8_t builtin_ttf_start[] asm("_binary_builtin_ttf_start");
extern const uint8_t builtin_ttf_end[] asm("_binary_builtin_ttf_end");
// Yan Reader 标志字体：Noto Serif CJK SC 子集，只有标志用到的 27 个字形（5.4KB）。
// 只给开机动画用，见 ttf_font_open_logo()。
extern const uint8_t yanos_logo_ttf_start[] asm("_binary_yanos_logo_ttf_start");
extern const uint8_t yanos_logo_ttf_end[] asm("_binary_yanos_logo_ttf_end");
static bool sd_suspended; // 字体传输期间仅允许内置字体。/ Only built-in fonts while transferring font files.
static ttf_work_t* work;

/* ---- 正文墨色：排版设定里的「字重」「对比度」---------------------------------
 * 这两个旋钮**只作用于书的正文**，所以做成"作用域"而不是常驻状态：调用方在画正文的前后
 * begin/end 一次（见 screen_reader.cpp 的 RdBodyInkScope），界面外壳画在作用域之外。
 *
 *   对比度 = 覆盖率曲线 gamma。作用在**已缓存**的字形位图上（mix_ink 逐像素查表），
 *            所以改它不必碰字形缓存 —— 与 KOReader 把 gamma 放在 cr_correct_gamma_buf
 *            的位置一致（那边也是"栅格化时算一次、之后一直用"）。
 *   字重   = 有 wght 轴的面走真实变体（作用域内临时改 f_current_weight，end 时还原）；
 *            没有轴的面走合成（见 synth_dw_now/rasterize_glyph）。字形缓存的键里带上
 *            合成量，所以改档自然命中新位图，不必清缓存。
 * 两者都**不改步进与度量**（见 cp_advance 那段说明）：改它们绝不重排正文。
 */
static bool s_body_on;                        // 正文墨色作用域是否打开
static int s_body_dw;                         // 字重增量（CSS 字重单位，只增不减）
static float s_body_gamma = TTF_COVER_GAMMA;  // 对比度（覆盖率 gamma）
static int s_body_saved_w[TTF_ROLE_COUNT];    // 真实变体：进作用域时备份的字重
static bool s_body_saved_ok[TTF_ROLE_COUNT];

// 本面此刻该用多少"合成"加粗量。有 wght 轴的面由真实变体负责，这里恒 0 —— 否则会叠成
// 双倍粗（真实变体已经是 Medium/Bold 了，再膨胀一遍就过了）。
static int synth_dw_now(void) {
    if (!s_body_on || gvar_ready) return 0;
    return s_body_dw;
}

// 选定字面，返回原字面(配 face_leave 还原)。给"进某面做一件固定的事再回来"用
// (加载/卸载/清缓存)。铁律 1：s_cur 只在这里和 ttf_set_role 里被写。
static ttf_face_t* face_enter(int role) {
    ttf_face_t* prev = s_cur;
    s_cur = (role >= 0 && role < TTF_ROLE_COUNT) ? &s_faces[role] : &s_faces[TTF_ROLE_CONTENT];
    return prev;
}
static void face_leave(ttf_face_t* prev) { s_cur = prev; }

// 内容面是否就是内置字体(内置面无 fd、font_mem 指向 rodata)。
static bool content_is_builtin(void) {
    return s_faces[TTF_ROLE_CONTENT].f_font_ready &&
           s_faces[TTF_ROLE_CONTENT].f_font_mem != NULL;
}

/* ---- 字节序与文件读 / Endian and file I/O ---- */
static uint16_t be16(const uint8_t* p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}

static uint32_t be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
        | ((uint32_t)p[2] << 8) | p[3];
}

static void put_be16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void put_be32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static bool io_read_raw(uint32_t offset, void* dst, size_t n) {
    if (font_mem != NULL) {
        if ((uint64_t)offset + n > font_mem_len) return false;
        memcpy(dst, font_mem + offset, n);
        return true;
    }
    if (font_fd < 0) return false;
    bool ok = false;
    do {
        if (font_file_pos != offset) {
            if (lseek(font_fd, (off_t)offset, SEEK_SET) < 0) {
                font_file_pos = UINT32_MAX;
                break;
            }
            font_file_pos = offset;
        }
        uint8_t* out = dst;
        size_t done = 0;
        while (done < n) {
            size_t chunk = n - done;
            if (chunk > 16384) chunk = 16384;
            ssize_t got = read(font_fd, out + done, chunk);
            if (got <= 0) {
                font_file_pos = UINT32_MAX;
                break;
            }
            done += (size_t)got;
            font_file_pos = offset + (uint32_t)done;
        }
        ok = done == n;
    } while (0);
    return ok;
}

static void io_reset(void) {
    const int n = io_slot_count();
    for (int i = 0; i < n; i++) {
        io_base[i] = UINT32_MAX;
        io_fill[i] = 0;
        io_age[i] = 0;
    }
    io_clock = 0;
    touch_n = 0;
}

static void io_unmap(void) {
    heap_caps_free(glyf_ram);
    glyf_ram = NULL;
    glyf_ram_off = 0;
    glyf_ram_len = 0;
    heap_caps_free(gvar_ram);
    gvar_ram = NULL;
    gvar_ram_off = 0;
    gvar_ram_len = 0;
}

static bool io_ensure(void) {
    const size_t slots = (size_t)io_slot_count();
    const size_t run_max = (size_t)io_run_cap();
    if (io_data == NULL) {
        // 要过一次没要来就退避(见 f_io_retry_at_us)：io_data==NULL 的读路径
        // (io_load_block / file_read_at / io_flush_touches / warm_text_io)每读一块都会
        // 调回这里，不挡就是"每次块读一条 WARN"，也在拿一次注定失败的分配去磨堆。
        // **但不永久放弃** —— 失败多半是临时的碎片化，放弃一整本书的代价远大于一条日志。
        const int64_t now = esp_timer_get_time();
        if (io_retry_at_us != 0 && now < io_retry_at_us) return false;
        io_data = heap_caps_malloc(
            slots * TTF_IO_BLOCK,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
        );
        if (io_data == NULL) {
            // 不致命(fallback 是逐次直读)，但会静默失去全部预取收益 —— 记一笔，
            // 免得对着"整页预取: 无块可读"猜半天。多半是 PSRAM 碎片化下
            // 要不到这么大一块连续内存。频率由退避窗口兜住(每 2 秒最多一条)。
            ESP_LOGW(
                TAG, "块缓存分配失败(%dKB 连续 PSRAM)，稍后重试，其间走直读",
                (int)(slots * TTF_IO_BLOCK / 1024)
            );
            io_retry_at_us = now + TTF_IO_RETRY_US;
            return false;
        }
        io_retry_at_us = 0;
        io_reset();
    }
    if (io_run_buf == NULL) {
        io_run_buf = heap_caps_malloc(
            run_max * TTF_IO_BLOCK,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
        );
    }
    // 与 io_data 同寿命(一次分配、永不释放)：这两个表只在有 io_data 时才有意义。
    if (touch_blocks == NULL) {
        touch_blocks = heap_caps_malloc(
            (size_t)TTF_PREFETCH_MAX * sizeof(uint32_t),
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
        );
    }
    if (warm_cps == NULL) {
        warm_cps = heap_caps_malloc(
            (size_t)TTF_GATHER_MAX * sizeof(uint32_t),
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
        );
    }
    return true;
}

// 放掉一个字面的预取块缓存。这几块**全是可重建的缓存**：释放之后 io_ensure 会重新
// 分配，分配不到就走逐次直读（那条路本来就写着"不致命"）。返回实际放掉的字节数。
static size_t ttf_face_purge_io(ttf_face_t* f) {
    size_t freed = 0;
    if (f->f_io_data) {
        freed += (size_t)(f->f_io_slots ? f->f_io_slots : TTF_IO_SLOTS) * TTF_IO_BLOCK;
        heap_caps_free(f->f_io_data);
        f->f_io_data = NULL;
    }
    if (f->f_io_run_buf) {
        freed += (size_t)(f->f_io_run_max ? f->f_io_run_max : TTF_IO_RUN_MAX) * TTF_IO_BLOCK;
        heap_caps_free(f->f_io_run_buf);
        f->f_io_run_buf = NULL;
    }
    if (f->f_touch_blocks) {
        freed += (size_t)TTF_PREFETCH_MAX * sizeof(uint32_t);
        heap_caps_free(f->f_touch_blocks);
        f->f_touch_blocks = NULL;
    }
    if (f->f_warm_cps) {
        freed += (size_t)TTF_GATHER_MAX * sizeof(uint32_t);
        heap_caps_free(f->f_warm_cps);
        f->f_warm_cps = NULL;
    }
    // 槽位表跟着作废：留着旧块号，下一次分配回来时会拿它当"已经在缓存里"用。
    f->f_touch_n = 0;
    f->f_io_clock = 0;
    // 这是一次**显式**释放（关书/低内存兜底），不是"要不到就没了"：把退避窗口清掉，
    // 下一次 io_ensure 立刻就能再试一次。见 f_io_retry_at_us 的说明。
    f->f_io_retry_at_us = 0;
    for (int i = 0; i < TTF_IO_SLOTS; i++) {
        f->f_io_base[i] = UINT32_MAX;
        f->f_io_fill[i] = 0;
        f->f_io_age[i] = 0;
    }
    return freed;
}

// 放掉**全部**字面的预取缓存，返回放掉的字节数（0 = 本来就没有）。
//
// 为什么需要它：这几块是 PSRAM 里最大的一批常驻块（内容面 512KB+128KB，两个次字面
// 各 192KB+32KB，一本带内嵌字体的书合计 ~1.06MB），而且**一次分配、永不释放**
// （见 io_ensure 的注释）。它们各自要一整块连续 PSRAM，于是把空闲区切成
// 512/415/192/128KB 的碎块 —— 实测开过几本书之后，PSRAM 还空着 1.5MB，最大连续块
// 却塌到 272KB。那块 494KB 的字体工作缓冲（大 CJK 字体）就要不到了，字体只能回落
// 内建，用户看到的是"排版设定里的字体锁死在内建"。
//
// 谁丢得起一目了然：预取缓存丢了是"SD 读慢一点"，字体丢了是"字不能换"。
static size_t ttf_purge_io_caches(void) {
    size_t freed = 0;
    for (int i = 0; i < TTF_ROLE_COUNT; i++) freed += ttf_face_purge_io(&s_faces[i]);
    return freed;
}

static bool io_from_map(uint32_t off, size_t n, void* dst) {
    if (glyf_ram != NULL && off >= glyf_ram_off
        && off + n <= glyf_ram_off + glyf_ram_len) {
        memcpy(dst, glyf_ram + (off - glyf_ram_off), n);
        return true;
    }
    if (gvar_ram != NULL && off >= gvar_ram_off
        && off + n <= gvar_ram_off + gvar_ram_len) {
        memcpy(dst, gvar_ram + (off - gvar_ram_off), n);
        return true;
    }
    return false;
}

static bool io_mapped_covers(uint32_t off, uint32_t len) {
    if (len == 0) return true;
    if (glyf_ram != NULL && off >= glyf_ram_off
        && off + len <= glyf_ram_off + glyf_ram_len) {
        return true;
    }
    if (gvar_ram != NULL && off >= gvar_ram_off
        && off + len <= gvar_ram_off + gvar_ram_len) {
        return true;
    }
    return false;
}

static int io_find(uint32_t base) {
    const int n = io_slot_count();
    for (int i = 0; i < n; i++) {
        if (io_base[i] == base) {
            io_age[i] = ++io_clock;
            return i;
        }
    }
    return -1;
}

static int io_victim(void) {
    int empty = -1;
    int best = 0;
    const int n = io_slot_count();
    for (int i = 0; i < n; i++) {
        if (io_base[i] == UINT32_MAX) {
            empty = i;
            break;
        }
        if (io_age[i] < io_age[best]) best = i;
    }
    return empty >= 0 ? empty : best;
}

static bool io_load_block(uint32_t base) {
    if (io_find(base) >= 0) {
        STATS.io_hit++;
        return true;
    }
    if (io_data == NULL && !io_ensure()) return false;
    int slot = io_victim();
    uint8_t* dst = io_data + (size_t)slot * TTF_IO_BLOCK;
    // 顺序补读(上一块的读正好停在块尾)不必再 seek 一次 —— io_read_raw 一直有这个
    // 快路径，这里漏了。逐块读相邻扇区的场景(预取退化成逐块、大字形跨两块)就省下
    // 一次 FATFS 簇链查找。
    if (font_file_pos != base) {
        if (lseek(font_fd, (off_t)base, SEEK_SET) < 0) {
            font_file_pos = UINT32_MAX;
            return false;
        }
    }
    ssize_t got = read(font_fd, dst, TTF_IO_BLOCK);
    if (got <= 0) {
        font_file_pos = UINT32_MAX;
        return false;
    }
    STATS.io_miss++;
    font_file_pos = base + (uint32_t)got;
    io_base[slot] = base;
    io_fill[slot] = (uint16_t)got;
    io_age[slot] = ++io_clock;
    return true;
}

static bool file_read_at(uint32_t offset, void* dst, size_t n) {
    if (n == 0) return true;
    if (font_mem != NULL) {
        if (io_from_map(offset, n, dst)) return true;
        return io_read_raw(offset, dst, n);
    }
    if (font_fd < 0) return false;
    if (io_from_map(offset, n, dst)) return true;
    if (io_data == NULL && !io_ensure()) return io_read_raw(offset, dst, n);

    uint8_t* out = dst;
    uint32_t pos = offset;
    size_t left = n;
    while (left > 0) {
        uint32_t base = pos & ~(uint32_t)(TTF_IO_BLOCK - 1);
        uint32_t skip = pos - base;
        if (!io_load_block(base)) return false;
        int slot = io_find(base);
        if (slot < 0 || skip >= io_fill[slot]) return false;
        size_t take = (size_t)io_fill[slot] - skip;
        if (take > left) take = left;
        memcpy(out, io_data + (size_t)slot * TTF_IO_BLOCK + skip, take);
        out += take;
        pos += (uint32_t)take;
        left -= take;
    }
    return true;
}

static void io_touch(uint32_t off, uint32_t len) {
    if (len == 0 || io_mapped_covers(off, len)) return;
    // 表还没分配说明这个字面压根没开过块缓存(font_mem 直读) —— 直接跳过。
    if (touch_blocks == NULL) return;
    uint32_t a = off & ~(uint32_t)(TTF_IO_BLOCK - 1);
    uint32_t end = off + len;
    while (a < end && touch_n < TTF_PREFETCH_MAX) {
        touch_blocks[touch_n++] = a;
        a += TTF_IO_BLOCK;
    }
}

static int u32_cmp(const void* a, const void* b) {
    uint32_t x = *(const uint32_t*)a;
    uint32_t y = *(const uint32_t*)b;
    if (x < y) return -1;
    if (x > y) return 1;
    return 0;
}

static bool io_load_run(uint32_t base, int blocks) {
    if (blocks <= 1) return io_load_block(base);
    if (blocks > io_run_cap()) blocks = io_run_cap();
    // io_data 这一问**目前恒假**(io_data 与 io_run_buf 同生同死：都在 io_ensure 里分配、
    // 都由 ttf_face_purge_io 一起放掉)，但这两块内存由两个函数管，而下面那几行 memcpy
    // 是直接写 io_data 的 —— 留着它，免得以后哪条路只放 io_data 就变成空指针写。
    if (io_data == NULL
        || io_run_buf == NULL
        || !io_read_raw(base, io_run_buf, (size_t)blocks * TTF_IO_BLOCK)) {
        bool ok = true;
        for (int i = 0; i < blocks; i++) {
            if (!io_load_block(base + (uint32_t)i * TTF_IO_BLOCK)) ok = false;
        }
        return ok;
    }
    STATS.run_reads++;
    STATS.run_blocks += (uint32_t)blocks;
    for (int i = 0; i < blocks; i++) {
        uint32_t b = base + (uint32_t)i * TTF_IO_BLOCK;
        if (io_find(b) >= 0) {
            STATS.run_waste++;   // 刚从 SD 读回来的这一块，本来就在缓存里
            continue;
        }
        int slot = io_victim();
        memcpy(
            io_data + (size_t)slot * TTF_IO_BLOCK,
            io_run_buf + (size_t)i * TTF_IO_BLOCK,
            TTF_IO_BLOCK
        );
        io_base[slot] = b;
        io_fill[slot] = TTF_IO_BLOCK;
        io_age[slot] = ++io_clock;
    }
    return true;
}

static void io_flush_touches(void) {
    if (touch_n <= 0) return;
    if (io_data == NULL && !io_ensure()) {
        touch_n = 0;
        return;
    }
    qsort(touch_blocks, (size_t)touch_n, sizeof(touch_blocks[0]), u32_cmp);
    int w = 1;
    for (int i = 1; i < touch_n; i++) {
        if (touch_blocks[i] != touch_blocks[w - 1]) {
            touch_blocks[w++] = touch_blocks[i];
        }
    }
    // 去重之后**这一轮**要去碰多少块 —— 槽位够不够的判据只能是它，不能拿
    // `段N读M块` 那个**累计和**去判（N 次 run 共读 M 块，均值 2~3 块/轮，看均值会
    // 以为槽位绰绰有余）。超过槽数的部分是同轮内被自己挤走的，下一趟绘制再当白读重来。
    if (w > (int)STATS.flush_peak) STATS.flush_peak = (uint16_t)w;
    int i = 0;
    while (i < w) {
        int j = i + 1;
        // 合并规则(三条都要满足)：
        //  · 相邻两块之间隔不超过 TTF_IO_GAP_BLOCKS 块 —— 中间那几块一起读掉，
        //    用一点白读换掉一次 lseek+一次短读(实测 lseek 占单次读的一半时间)；
        //  · 整段跨度不超过 io_run_cap() 块 —— io_run_buf 就那么大；
        //  · **白读的填空块不超过真正需要的块数的一半**(跨度 ≤ 1.5×块数) ——
        //    这条是最重要的：两块离得远了就宁可另起一次读，也不要把中间几 MB
        //    一起吞进来。有了它，合并再激进也不会把读放大约 1.5 倍以上。
        while (j < w) {
            uint32_t nb = touch_blocks[j];
            uint32_t span = (nb - touch_blocks[i]) / TTF_IO_BLOCK + 1;
            uint32_t cnt = (uint32_t)(j - i + 1);
            if (nb - touch_blocks[j - 1] > (uint32_t)TTF_IO_GAP_BLOCKS * TTF_IO_BLOCK) break;
            if (span > (uint32_t)io_run_cap()) break;
            if (span * 2 > cnt * 3) break;
            j++;
        }
        // 注意这里是**跨度**不是个数：块号有间隔时 j-i < 跨度，按个数读会漏掉
        // 后面那几个被碰过的块(读进来的是中间的填空块，真正要的反而没到)。
        int span = (int)((touch_blocks[j - 1] - touch_blocks[i]) / TTF_IO_BLOCK) + 1;
        io_load_run(touch_blocks[i], span);
        i = j;
    }
    touch_n = 0;
}

static bool try_map_table(
    uint32_t off, uint32_t len, uint8_t** ram, uint32_t* ram_off,
    uint32_t* ram_len, const char* name
) {
    if (*ram != NULL || len < 4096 || len > TTF_MAP_MAX) return false;
    size_t largest = heap_caps_get_largest_free_block(
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
    );
    if (largest < (size_t)len + TTF_MAP_RESERVE) {
        ESP_LOGI(
            TAG, "%s %u KB, skip map (largest %u KB)",
            name, (unsigned)(len / 1024), (unsigned)(largest / 1024)
        );
        return false;
    }
    uint8_t* p = heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p == NULL) return false;
    int64_t t0 = esp_timer_get_time();
    if (!io_read_raw(off, p, len)) {
        heap_caps_free(p);
        return false;
    }
    *ram = p;
    *ram_off = off;
    *ram_len = len;
    ESP_LOGI(
        TAG, "mapped %s %u KB in %dms",
        name, (unsigned)(len / 1024),
        (int)((esp_timer_get_time() - t0) / 1000)
    );
    return true;
}

static const char* font_basename(const char* path) {
    const char* slash = strrchr(path, '/');
    return slash != NULL ? slash + 1 : path;
}

// Finder 拷到 FAT 会留下 ._xxx.ttf（AppleDouble）和 .DS_Store，不是真字体。
static bool is_junk_name(const char* name) {
    return name[0] == '.' || strncmp(name, "._", 2) == 0;
}

static bool is_ttf_name(const char* name) {
    if (is_junk_name(name)) return false;
    size_t len = strlen(name);
    return len >= 4 && strcasecmp(name + len - 4, ".ttf") == 0;
}

static void font_stem(const char* path, char* out, size_t n) {
    const char* slash = strrchr(path, '/');
    const char* name = slash != NULL ? slash + 1 : path;
    strlcpy(out, name, n);
    size_t len = strlen(out);
    if (len >= 4 && strcasecmp(out + len - 4, ".ttf") == 0) {
        out[len - 4] = '\0';
    }
}

static int font_item_cmp(const void* a, const void* b) {
    return strcasecmp(
        ((const ttf_font_item_t*)a)->name, ((const ttf_font_item_t*)b)->name
    );
}

static bool catalog_has_name(
    const ttf_font_item_t* items, int n, const char* name
) {
    for (int i = 0; i < n; i++) {
        if (strcasecmp(items[i].name, name) == 0) return true;
    }
    return false;
}

static ttf_font_item_t s_catalog[TTF_FONT_MAX];
static int s_catalog_n;

int ttf_font_scan(void) {
    s_catalog_n = 0;
    build_font_dirs();
    for (int d = 0; d < s_font_dir_n; d++) {
        DIR* dir = opendir(s_font_dirs[d]);
        if (dir == NULL) continue;
        struct dirent* ent;
        while ((ent = readdir(dir)) != NULL && s_catalog_n < TTF_FONT_MAX) {
            if (!is_ttf_name(ent->d_name)) continue;
            char name[TTF_FONT_NAME_MAX];
            font_stem(ent->d_name, name, sizeof(name));
            if (name[0] == '\0' || catalog_has_name(s_catalog, s_catalog_n, name)) {
                continue;
            }
            // 手工拼接而非 snprintf：GCC 对未知长度的 "%s/%s" 会报
            // -Werror=format-truncation，这里显式校验长度。
            const size_t dl = strlen(s_font_dirs[d]);
            const size_t nl = strlen(ent->d_name);
            if (dl + 1 + nl >= sizeof(s_catalog[s_catalog_n].path)) continue;
            memcpy(s_catalog[s_catalog_n].path, s_font_dirs[d], dl);
            s_catalog[s_catalog_n].path[dl] = '/';
            memcpy(s_catalog[s_catalog_n].path + dl + 1, ent->d_name, nl + 1);
            strlcpy(s_catalog[s_catalog_n].name, name, sizeof(s_catalog[0].name));
            s_catalog_n++;
        }
        closedir(dir);
    }
    if (s_catalog_n > 1) {
        qsort(s_catalog, (size_t)s_catalog_n, sizeof(s_catalog[0]), font_item_cmp);
    }
    return s_catalog_n;
}

int ttf_font_count(void) {
    return s_catalog_n;
}

const ttf_font_item_t* ttf_font_item(int index) {
    if (index < 0 || index >= s_catalog_n) return NULL;
    return &s_catalog[index];
}

static int try_open_path(const char* path) {
    int fd = open(path, O_RDONLY);
    if (fd >= 0) {
        strlcpy(font_path, path, sizeof(font_path));
        ESP_LOGI(TAG, "using %s", path);
    }
    return fd;
}

static int open_font_file(const char* preferred) {
    if (preferred == NULL || ttf_font_path_is_builtin(preferred)
        || is_junk_name(font_basename(preferred))) {
        return -1;
    }
    return try_open_path(preferred);
}

// 下面这几个"读用户选了哪个字体"的接口**一律钉在内容面**，不能用宏 ——
// 它们常在上一次绘制是 UI 面时被调用(设置页要显示"字体: X")，用宏就会读到
// UI 面然后回答"内建"，菜单就撒谎了。
const char* ttf_font_path(void) {
    return s_faces[TTF_ROLE_CONTENT].f_font_path;
}

bool ttf_font_path_is_builtin(const char* path) {
    return path == NULL || path[0] == '\0' || strcmp(path, TTF_FONT_BUILTIN) == 0;
}

bool ttf_font_is_builtin(void) {
    return content_is_builtin();
}

const char* ttf_font_display_name(void) {
    static char name[TTF_FONT_NAME_MAX];
    const char* path = s_faces[TTF_ROLE_CONTENT].f_font_path;
    if (content_is_builtin() || ttf_font_path_is_builtin(path)) return "内建";
    if (path[0] == '\0') return "";
    font_stem(path, name, sizeof(name));
    return name;
}

static bool find_sfnt_table(
    const uint8_t* header, size_t header_len, const char* tag,
    uint32_t* offset, uint32_t* length
) {
    if (header_len < 12) return false;
    uint16_t count = be16(header + 4);
    for (uint16_t i = 0; i < count; i++) {
        size_t rec = 12 + (size_t)i * 16;
        if (rec + 16 > header_len) return false;
        if (memcmp(header + rec, tag, 4) == 0) {
            *offset = be32(header + rec + 8);
            *length = be32(header + rec + 12);
            return true;
        }
    }
    return false;
}

static uint32_t file_glyph_off(int gid) {
    if (gid < 0 || gid > num_glyphs) return 0;
    if (loca_long) return be32(file_loca + (size_t)gid * 4);
    return (uint32_t)be16(file_loca + (size_t)gid * 2) * 2u;
}

static uint32_t file_glyph_len(int gid) {
    if (gid < 0 || gid >= num_glyphs) return 0;
    uint32_t a = file_glyph_off(gid);
    uint32_t b = file_glyph_off(gid + 1);
    return b > a ? b - a : 0;
}

static void set_work_loca(int gid, uint32_t off) {
    uint8_t* loca = font_data + work_loca_off;
    if (loca_long) {
        put_be32(loca + (size_t)gid * 4, off);
    } else {
        put_be16(loca + (size_t)gid * 2, (uint16_t)(off / 2));
    }
}

static bool enqueue_composite_children(
    const uint8_t* blob, uint32_t blob_len, int* tree, int* tree_n
) {
    if (blob_len < 10) return true;
    if ((int16_t)be16(blob) >= 0) return true;

    size_t p = 10;
    while (p + 4 <= blob_len) {
        uint16_t flags = be16(blob + p);
        int gidx = (int)be16(blob + p + 2);
        p += 4;
        if (flags & 1) {
            if (p + 4 > blob_len) break;
            p += 4;
        } else {
            if (p + 2 > blob_len) break;
            p += 2;
        }
        if (flags & (1 << 3)) p += 2;
        else if (flags & (1 << 6)) p += 4;
        else if (flags & (1 << 7)) p += 8;
        if (gidx >= 0 && gidx < num_glyphs) {
            bool seen = false;
            for (int i = 0; i < *tree_n; i++) {
                if (tree[i] == gidx) {
                    seen = true;
                    break;
                }
            }
            if (!seen) {
                if (*tree_n >= TTF_TREE_MAX) return false;
                tree[(*tree_n)++] = gidx;
            }
        }
        if ((flags & (1 << 5)) == 0) break;
    }
    return true;
}

/* ---- gvar 变体 / gvar variation ---- */
static int clamp_weight(int wght) {
    if (wght < wght_min) return wght_min;
    if (wght > wght_max) return wght_max;
    return wght;
}

static float f2dot14(int16_t v) {
    return (float)v / 16384.0f;
}

static float norm_wght(void) {
    int w = current_weight;
    if (w == wght_def) return 0.0f;
    if (w < wght_def) {
        if (wght_def == wght_min) return 0.0f;
        return (float)(w - wght_def) / (float)(wght_def - wght_min);
    }
    if (wght_max == wght_def) return 0.0f;
    return (float)(w - wght_def) / (float)(wght_max - wght_def);
}

static float tuple_scalar(float peak, float start, float end, bool intermediate) {
    float n = norm_wght();
    if (intermediate) {
        if (n < start || n > end) return 0.0f;
        if (n == peak) return 1.0f;
        if (n < peak) {
            if (peak == start) return 1.0f;
            return (n - start) / (peak - start);
        }
        if (peak == end) return 1.0f;
        return (end - n) / (end - peak);
    }
    if (peak == 0.0f) return 1.0f;
    if (n == peak) return 1.0f;
    if (peak > 0.0f && (n < 0.0f || n > peak)) return 0.0f;
    if (peak < 0.0f && (n > 0.0f || n < peak)) return 0.0f;
    return n / peak;
}

static bool read_packed_points(
    const uint8_t** pp, const uint8_t* end, uint16_t* pts, int* n_out
) {
    if (*pp >= end) return false;
    uint8_t b = *(*pp)++;
    if (b == 0) {
        *n_out = -1;
        return true;
    }
    int count = b;
    if (b & 0x80) {
        if (*pp >= end) return false;
        count = ((b & 0x7F) << 8) | *(*pp)++;
    }
    if (count > TTF_MAX_VAR_PTS) return false;
    int n = 0;
    uint16_t last = 0;
    while (n < count) {
        if (*pp >= end) return false;
        uint8_t ctrl = *(*pp)++;
        int words = ctrl & 0x80;
        int run = (ctrl & 0x7F) + 1;
        for (int i = 0; i < run && n < count; i++) {
            uint16_t delta;
            if (words) {
                if (*pp + 2 > end) return false;
                delta = (uint16_t)(((*pp)[0] << 8) | (*pp)[1]);
                *pp += 2;
            } else {
                if (*pp >= end) return false;
                delta = *(*pp)++;
            }
            last = (uint16_t)(last + delta);
            pts[n++] = last;
        }
    }
    *n_out = n;
    return true;
}

static bool read_packed_deltas(
    const uint8_t** pp, const uint8_t* end, int count, int16_t* out
) {
    int n = 0;
    while (n < count) {
        if (*pp >= end) return false;
        uint8_t ctrl = *(*pp)++;
        int run = (ctrl & 0x3F) + 1;
        if (ctrl & 0x80) {
            for (int i = 0; i < run && n < count; i++) out[n++] = 0;
        } else if (ctrl & 0x40) {
            for (int i = 0; i < run && n < count; i++) {
                if (*pp + 2 > end) return false;
                out[n++] = (int16_t)(((*pp)[0] << 8) | (*pp)[1]);
                *pp += 2;
            }
        } else {
            for (int i = 0; i < run && n < count; i++) {
                if (*pp >= end) return false;
                out[n++] = (int8_t)(*(*pp)++);
            }
        }
    }
    return true;
}

// TrueType IUP：按原坐标在两个已赋值点之间插值未赋值点。
static void iup_contour(float* delta, const int16_t* org, int begin, int end) {
    int n = end - begin + 1;
    if (n <= 0) return;
    int first = -1;
    for (int i = 0; i < n; i++) {
        if (!isnan(delta[begin + i])) {
            first = i;
            break;
        }
    }
    if (first < 0) {
        for (int i = 0; i < n; i++) delta[begin + i] = 0.0f;
        return;
    }
    int touched = 0;
    for (int i = 0; i < n; i++) {
        if (!isnan(delta[begin + i])) touched++;
    }
    if (touched == 1) {
        float v = delta[begin + first];
        for (int i = 0; i < n; i++) delta[begin + i] = v;
        return;
    }

    int prev = first;
    for (int step = 1; step <= n; step++) {
        int i = (first + step) % n;
        if (isnan(delta[begin + i]) && step < n) continue;
        int next = i;
        if (step == n) next = first;
        int ia = begin + prev;
        int ib = begin + next;
        int16_t ca = org[ia];
        int16_t cb = org[ib];
        float da = delta[ia];
        float db = delta[ib];
        int j = (prev + 1) % n;
        while (j != next) {
            int ij = begin + j;
            int16_t c = org[ij];
            if (ca == cb) {
                delta[ij] = da;
            } else if ((c <= ca && c <= cb) || (c >= ca && c >= cb)) {
                int16_t da_abs = c >= ca ? (int16_t)(c - ca) : (int16_t)(ca - c);
                int16_t db_abs = c >= cb ? (int16_t)(c - cb) : (int16_t)(cb - c);
                delta[ij] = da_abs <= db_abs ? da : db;
            } else {
                delta[ij] = da + (db - da) * (float)(c - ca) / (float)(cb - ca);
            }
            j = (j + 1) % n;
        }
        prev = next;
        if (step == n) break;
    }
}

static void iup_axis(
    float* delta, const int16_t* org, int n_pts, const int16_t* endpts, int n_contours
) {
    int start = 0;
    for (int c = 0; c < n_contours; c++) {
        int last = endpts[c];
        if (last >= n_pts) last = n_pts - 1;
        if (last >= start) iup_contour(delta, org, start, last);
        start = last + 1;
    }
    for (int i = 0; i < n_pts; i++) {
        if (isnan(delta[i])) delta[i] = 0.0f;
    }
}

static bool decode_simple_xy(
    const uint8_t* blob, uint32_t len,
    int16_t* x, int16_t* y, uint8_t* on, int16_t* endpts,
    int* n_pts, int* n_contours, const uint8_t** ins, uint16_t* ins_len
) {
    if (len < 10) return false;
    int16_t contours = (int16_t)be16(blob);
    if (contours <= 0 || contours > 64) return false;
    if ((uint32_t)(10 + contours * 2 + 2) > len) return false;
    for (int i = 0; i < contours; i++) endpts[i] = (int16_t)be16(blob + 10 + i * 2);
    int n = endpts[contours - 1] + 1;
    if (n <= 0 || n > TTF_MAX_PTS) return false;
    uint16_t ilen = be16(blob + 10 + contours * 2);
    size_t p = (size_t)(12 + contours * 2 + ilen);
    if (p > len) return false;
    *ins = blob + 12 + contours * 2;
    *ins_len = ilen;

    uint8_t* flags = work->flags;
    int i = 0;
    while (i < n) {
        if (p >= len) return false;
        uint8_t f = blob[p++];
        flags[i] = f;
        i++;
        if (f & 0x08) {
            if (p >= len) return false;
            int rep = blob[p++];
            while (rep-- > 0 && i < n) flags[i++] = f;
        }
    }
    int16_t acc = 0;
    for (i = 0; i < n; i++) {
        uint8_t f = flags[i];
        if (f & 0x02) {
            if (p >= len) return false;
            int16_t d = blob[p++];
            acc = (int16_t)(acc + ((f & 0x10) ? d : -d));
        } else if (!(f & 0x10)) {
            if (p + 2 > len) return false;
            acc = (int16_t)(acc + (int16_t)be16(blob + p));
            p += 2;
        }
        x[i] = acc;
        on[i] = (uint8_t)(f & 0x01);
    }
    acc = 0;
    for (i = 0; i < n; i++) {
        uint8_t f = flags[i];
        if (f & 0x04) {
            if (p >= len) return false;
            int16_t d = blob[p++];
            acc = (int16_t)(acc + ((f & 0x20) ? d : -d));
        } else if (!(f & 0x20)) {
            if (p + 2 > len) return false;
            acc = (int16_t)(acc + (int16_t)be16(blob + p));
            p += 2;
        }
        y[i] = acc;
    }
    *n_pts = n;
    *n_contours = contours;
    return true;
}

static uint32_t encode_simple_xy(
    uint8_t* out, uint32_t cap,
    const int16_t* x, const int16_t* y, const uint8_t* on,
    const int16_t* endpts, int n_pts, int n_contours,
    const uint8_t* ins, uint16_t ins_len
) {
    uint32_t need = (uint32_t)(10 + n_contours * 2 + 2 + ins_len + n_pts * 5);
    if (need > cap) return 0;
    put_be16(out, (uint16_t)n_contours);
    int16_t xmin = x[0], xmax = x[0], ymin = y[0], ymax = y[0];
    for (int i = 1; i < n_pts; i++) {
        if (x[i] < xmin) xmin = x[i];
        if (x[i] > xmax) xmax = x[i];
        if (y[i] < ymin) ymin = y[i];
        if (y[i] > ymax) ymax = y[i];
    }
    put_be16(out + 2, (uint16_t)xmin);
    put_be16(out + 4, (uint16_t)ymin);
    put_be16(out + 6, (uint16_t)xmax);
    put_be16(out + 8, (uint16_t)ymax);
    for (int i = 0; i < n_contours; i++) {
        put_be16(out + 10 + i * 2, (uint16_t)endpts[i]);
    }
    put_be16(out + 10 + n_contours * 2, ins_len);
    size_t p = (size_t)(12 + n_contours * 2);
    if (ins_len > 0) memcpy(out + p, ins, ins_len);
    p += ins_len;
    for (int i = 0; i < n_pts; i++) out[p++] = (uint8_t)(on[i] ? 0x01 : 0x00);
    int16_t prev = 0;
    for (int i = 0; i < n_pts; i++) {
        int16_t d = (int16_t)(x[i] - prev);
        if (d == 0) {
            out[12 + n_contours * 2 + ins_len + i] |= 0x10;
        } else {
            put_be16(out + p, (uint16_t)d);
            p += 2;
        }
        prev = x[i];
    }
    prev = 0;
    for (int i = 0; i < n_pts; i++) {
        int16_t d = (int16_t)(y[i] - prev);
        if (d == 0) {
            out[12 + n_contours * 2 + ins_len + i] |= 0x20;
        } else {
            put_be16(out + p, (uint16_t)d);
            p += 2;
        }
        prev = y[i];
    }
    return (uint32_t)p;
}

static bool apply_gvar_deltas(
    float* dx, float* dy, const int16_t* ox, const int16_t* oy,
    int n_outline, int n_var, const int16_t* endpts, int n_contours,
    const uint8_t* data, uint32_t data_len
) {
    if (data_len < 4) return false;
    uint16_t tvc = be16(data);
    int tuple_n = tvc & 0x0FFF;
    bool share_pts = (tvc & 0x8000) != 0;
    uint16_t data_off = be16(data + 2);
    if (data_off > data_len) return false;

    const uint8_t* hp = data + 4;
    const uint8_t* end = data + data_len;
    const uint8_t* sp = data + data_off;
    uint16_t* shared_pts = work->shared_pts;
    int shared_n = -2;
    if (share_pts) {
        if (!read_packed_points(&sp, end, shared_pts, &shared_n)) return false;
    }

    int16_t* xdel = work->xdel;
    int16_t* ydel = work->ydel;
    uint16_t* pts = work->pts;

    for (int t = 0; t < tuple_n; t++) {
        if (hp + 4 > end) return false;
        uint16_t ti = be16(hp + 2);
        hp += 4;
        float peak = 0.0f, start = 0.0f, endc = 0.0f;
        bool intermediate = (ti & 0x4000) != 0;
        if (ti & 0x8000) {
            if (hp + 2 > end) return false;
            peak = f2dot14((int16_t)be16(hp));
            hp += 2;
        } else {
            int idx = ti & 0x0FFF;
            if (idx >= shared_tuple_count) return false;
            peak = f2dot14(shared_tuple_f2dot14[idx]);
        }
        if (intermediate) {
            if (hp + 4 > end) return false;
            start = f2dot14((int16_t)be16(hp));
            endc = f2dot14((int16_t)be16(hp + 2));
            hp += 4;
        }
        float scalar = tuple_scalar(peak, start, endc, intermediate);
        int pn = -1;
        if (ti & 0x2000) {
            if (!read_packed_points(&sp, end, pts, &pn)) return false;
        } else if (share_pts) {
            pn = shared_n;
            if (pn > 0) memcpy(pts, shared_pts, (size_t)pn * sizeof(pts[0]));
        }
        int delta_n = (pn < 0) ? n_var : pn;
        if (delta_n > TTF_MAX_VAR_PTS) return false;
        if (!read_packed_deltas(&sp, end, delta_n, xdel)) return false;
        if (!read_packed_deltas(&sp, end, delta_n, ydel)) return false;
        if (scalar == 0.0f) continue;

        float* tdx = work->tdx;
        float* tdy = work->tdy;
        uint8_t* setx = work->setx;
        uint8_t* sety = work->sety;
        memset(setx, 0, (size_t)n_outline);
        memset(sety, 0, (size_t)n_outline);
        memset(tdx, 0, sizeof(work->tdx));
        memset(tdy, 0, sizeof(work->tdy));
        if (pn < 0) {
            for (int i = 0; i < n_outline && i < delta_n; i++) {
                tdx[i] = (float)xdel[i] * scalar;
                tdy[i] = (float)ydel[i] * scalar;
                setx[i] = 1;
                sety[i] = 1;
            }
        } else {
            for (int i = 0; i < pn; i++) {
                int pi = pts[i];
                if (pi < n_outline) {
                    tdx[pi] = (float)xdel[i] * scalar;
                    tdy[pi] = (float)ydel[i] * scalar;
                    setx[pi] = 1;
                    sety[pi] = 1;
                }
            }
            for (int i = 0; i < n_outline; i++) {
                if (!setx[i]) tdx[i] = NAN;
                if (!sety[i]) tdy[i] = NAN;
            }
            iup_axis(tdx, ox, n_outline, endpts, n_contours);
            iup_axis(tdy, oy, n_outline, endpts, n_contours);
        }
        for (int i = 0; i < n_outline; i++) {
            dx[i] += tdx[i];
            dy[i] += tdy[i];
        }
        (void)n_var;
    }
    return true;
}

static bool apply_gvar_simple(uint8_t* blob, uint32_t* len, uint32_t cap, int gid) {
    uint32_t a = gvar_glyph_off[gid];
    uint32_t b = gvar_glyph_off[gid + 1];
    if (b <= a || b - a > TTF_GVAR_SLICE_MAX) return true;
    uint8_t* slice = work->slice;
    if (!file_read_at(file_gvar_off + gvar_data_array_off + a, slice, b - a)) {
        return false;
    }

    int16_t* x = work->x;
    int16_t* y = work->y;
    int16_t* endpts = work->endpts;
    uint8_t* on = work->on;
    int n_pts = 0, n_contours = 0;
    const uint8_t* ins = NULL;
    uint16_t ins_len = 0;
    if (!decode_simple_xy(blob, *len, x, y, on, endpts, &n_pts, &n_contours, &ins, &ins_len)) {
        return true;
    }
    int n_var = n_pts + 4;
    float* dx = work->dx;
    float* dy = work->dy;
    memset(dx, 0, sizeof(work->dx));
    memset(dy, 0, sizeof(work->dy));
    if (!apply_gvar_deltas(
            dx, dy, x, y, n_pts, n_var, endpts, n_contours, slice, b - a
        )) {
        return true;
    }
    for (int i = 0; i < n_pts; i++) {
        x[i] = (int16_t)lroundf((float)x[i] + dx[i]);
        y[i] = (int16_t)lroundf((float)y[i] + dy[i]);
    }
    uint32_t encoded = encode_simple_xy(
        blob, cap, x, y, on, endpts, n_pts, n_contours, ins, ins_len
    );
    if (encoded == 0) return false;
    *len = encoded;
    return true;
}

static bool apply_gvar_composite(uint8_t* blob, uint32_t len, int gid) {
    uint32_t a = gvar_glyph_off[gid];
    uint32_t b = gvar_glyph_off[gid + 1];
    if (b <= a || b - a > TTF_GVAR_SLICE_MAX) return true;
    uint8_t* slice = work->slice;
    if (!file_read_at(file_gvar_off + gvar_data_array_off + a, slice, b - a)) {
        return false;
    }

    typedef struct {
        size_t arg_off;
        bool words;
        int16_t x;
        int16_t y;
    } comp_t;
    comp_t comps[16];
    int ncomp = 0;
    size_t p = 10;
    while (p + 4 <= len && ncomp < 16) {
        uint16_t flags = be16(blob + p);
        p += 4;
        comps[ncomp].arg_off = p;
        comps[ncomp].words = (flags & 1) != 0;
        if (flags & 1) {
            if (p + 4 > len) break;
            comps[ncomp].x = (int16_t)be16(blob + p);
            comps[ncomp].y = (int16_t)be16(blob + p + 2);
            p += 4;
        } else {
            if (p + 2 > len) break;
            comps[ncomp].x = (int8_t)blob[p];
            comps[ncomp].y = (int8_t)blob[p + 1];
            p += 2;
        }
        if (flags & (1 << 3)) p += 2;
        else if (flags & (1 << 6)) p += 4;
        else if (flags & (1 << 7)) p += 8;
        ncomp++;
        if ((flags & (1 << 5)) == 0) break;
    }
    if (ncomp == 0) return true;

    // 复合 gvar 点序：4 个幻影点 + 每分量 4 点，前两个是原点 x/y。
    int n_var = 4 + 4 * ncomp;
    const uint8_t* data = slice;
    uint32_t data_len = b - a;
    if (data_len < 4) return true;
    uint16_t tvc = be16(data);
    int tuple_n = tvc & 0x0FFF;
    bool share_pts = (tvc & 0x8000) != 0;
    uint16_t data_off = be16(data + 2);
    if (data_off > data_len) return true;
    const uint8_t* hp = data + 4;
    const uint8_t* end = data + data_len;
    const uint8_t* sp = data + data_off;
    uint16_t* shared_pts = work->shared_pts;
    int shared_n = -2;
    if (share_pts && !read_packed_points(&sp, end, shared_pts, &shared_n)) return true;

    int16_t* xdel = work->xdel;
    int16_t* ydel = work->ydel;
    uint16_t* pts = work->pts;
    float cdx[16] = {0};
    float cdy[16] = {0};

    for (int t = 0; t < tuple_n; t++) {
        if (hp + 4 > end) break;
        uint16_t ti = be16(hp + 2);
        hp += 4;
        float peak = 0.0f, start = 0.0f, endc = 0.0f;
        bool intermediate = (ti & 0x4000) != 0;
        if (ti & 0x8000) {
            if (hp + 2 > end) break;
            peak = f2dot14((int16_t)be16(hp));
            hp += 2;
        } else {
            int idx = ti & 0x0FFF;
            if (idx >= shared_tuple_count) break;
            peak = f2dot14(shared_tuple_f2dot14[idx]);
        }
        if (intermediate) {
            if (hp + 4 > end) break;
            start = f2dot14((int16_t)be16(hp));
            endc = f2dot14((int16_t)be16(hp + 2));
            hp += 4;
        }
        float scalar = tuple_scalar(peak, start, endc, intermediate);
        int pn = -1;
        if (ti & 0x2000) {
            if (!read_packed_points(&sp, end, pts, &pn)) break;
        } else if (share_pts) {
            pn = shared_n;
            if (pn > 0) memcpy(pts, shared_pts, (size_t)pn * sizeof(pts[0]));
        }
        int delta_n = (pn < 0) ? n_var : pn;
        if (delta_n > TTF_MAX_VAR_PTS) break;
        if (!read_packed_deltas(&sp, end, delta_n, xdel)) break;
        if (!read_packed_deltas(&sp, end, delta_n, ydel)) break;
        if (scalar == 0.0f) continue;

        if (pn < 0) {
            for (int i = 0; i < ncomp; i++) {
                int pi = 4 + 4 * i;
                if (pi < delta_n) cdx[i] += (float)xdel[pi] * scalar;
                if (pi + 1 < delta_n) cdy[i] += (float)ydel[pi] * scalar;
            }
        } else {
            for (int i = 0; i < pn; i++) {
                int pi = pts[i];
                if (pi < 4) continue;
                int ci = (pi - 4) / 4;
                int which = (pi - 4) % 4;
                if (ci < 0 || ci >= ncomp) continue;
                if (which == 0) cdx[ci] += (float)xdel[i] * scalar;
                if (which == 1) cdy[ci] += (float)ydel[i] * scalar;
            }
        }
    }

    for (int i = 0; i < ncomp; i++) {
        int16_t nx = (int16_t)lroundf((float)comps[i].x + cdx[i]);
        int16_t ny = (int16_t)lroundf((float)comps[i].y + cdy[i]);
        uint8_t* arg = blob + comps[i].arg_off;
        if (comps[i].words) {
            put_be16(arg, (uint16_t)nx);
            put_be16(arg + 2, (uint16_t)ny);
        } else {
            if (nx < -128) nx = -128;
            if (nx > 127) nx = 127;
            if (ny < -128) ny = -128;
            if (ny > 127) ny = 127;
            arg[0] = (uint8_t)nx;
            arg[1] = (uint8_t)ny;
        }
    }
    return true;
}

static bool ensure_work(void) {
    if (work != NULL) return true;
    work = heap_caps_calloc(1, sizeof(ttf_work_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return work != NULL;
}

static bool apply_gvar_to_blob(uint8_t* blob, uint32_t* len, uint32_t cap, int gid) {
    if (!gvar_ready || current_weight == wght_def || gid < 0 || gid >= num_glyphs) {
        return true;
    }
    if (!ensure_work()) return false;
    if (*len < 2) return true;
    int16_t contours = (int16_t)be16(blob);
    if (contours > 0) return apply_gvar_simple(blob, len, cap, gid);
    if (contours < 0) return apply_gvar_composite(blob, *len, gid);
    return true;
}

static void reset_variation(void) {
    gvar_ready = false;
    heap_caps_free(gvar_glyph_off);
    gvar_glyph_off = NULL;
    file_gvar_off = 0;
    file_gvar_len = 0;
    gvar_data_array_off = 0;
    shared_tuple_count = 0;
    gvar_axis_count = 0;
    wght_min = TTF_WGHT_MIN;
    wght_def = TTF_WGHT_DEF;
    wght_max = TTF_WGHT_MAX;
    current_weight = TTF_WGHT_DEF;
}

static bool load_variation(const uint8_t* header, size_t header_len) {
    reset_variation();
    uint32_t fvar_off = 0, fvar_len = 0;
    uint32_t gvar_off = 0, gvar_len = 0;
    if (!find_sfnt_table(header, header_len, "fvar", &fvar_off, &fvar_len)
        || !find_sfnt_table(header, header_len, "gvar", &gvar_off, &gvar_len)) {
        ESP_LOGW(TAG, "no fvar/gvar, weight locked to default");
        return false;
    }
    file_gvar_len = gvar_len;
    uint8_t fvar[32];
    if (!file_read_at(fvar_off, fvar, sizeof(fvar))) return false;
    uint16_t axis_off = be16(fvar + 4);
    uint16_t axis_n = be16(fvar + 8);
    uint16_t axis_sz = be16(fvar + 10);
    if (axis_n < 1 || axis_sz < 20) return false;
    uint8_t axis[20];
    if (!file_read_at(fvar_off + axis_off, axis, 20)) return false;
    if (memcmp(axis, "wght", 4) != 0) {
        ESP_LOGW(TAG, "first axis is not wght");
    }
    wght_min = (int)lroundf((float)((int32_t)be32(axis + 4)) / 65536.0f);
    wght_def = (int)lroundf((float)((int32_t)be32(axis + 8)) / 65536.0f);
    wght_max = (int)lroundf((float)((int32_t)be32(axis + 12)) / 65536.0f);

    uint8_t gh[20];
    if (!file_read_at(gvar_off, gh, sizeof(gh))) return false;
    gvar_axis_count = be16(gh + 4);
    shared_tuple_count = be16(gh + 6);
    uint32_t shared_off = be32(gh + 8);
    uint16_t gc = be16(gh + 12);
    uint16_t flags = be16(gh + 14);
    gvar_data_array_off = be32(gh + 16);
    const bool gvar_long = (flags & 1u) != 0;
    if (gc != (uint16_t)num_glyphs || gvar_axis_count < 1) {
        ESP_LOGW(TAG, "gvar header mismatch gc=%u flags=%u", gc, flags);
        return false;
    }
    if (shared_tuple_count > 8) shared_tuple_count = 8;
    if (shared_tuple_count > 0) {
        uint8_t st[16];
        if (!file_read_at(gvar_off + shared_off, st, (size_t)shared_tuple_count * 2)) {
            return false;
        }
        for (int i = 0; i < shared_tuple_count; i++) {
            shared_tuple_f2dot14[i] = (int16_t)be16(st + i * 2);
        }
    }
    gvar_glyph_off = heap_caps_malloc(
        (size_t)(num_glyphs + 1) * 4, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
    );
    if (gvar_glyph_off == NULL) return false;
    // 短偏移是 16 位字偏移（×2）；子集 TTF 常走短格式，源 VF 是长格式。
    const size_t off_bytes = (size_t)(num_glyphs + 1) * (gvar_long ? 4u : 2u);
    if (!file_read_at(gvar_off + 20, gvar_glyph_off, off_bytes)) {
        heap_caps_free(gvar_glyph_off);
        gvar_glyph_off = NULL;
        return false;
    }
    uint8_t* raw = (uint8_t*)gvar_glyph_off;
    if (gvar_long) {
        for (int i = 0; i <= num_glyphs; i++) {
            gvar_glyph_off[i] = be32(raw + (size_t)i * 4);
        }
    } else {
        for (int i = num_glyphs; i >= 0; i--) {
            gvar_glyph_off[i] = (uint32_t)be16(raw + (size_t)i * 2) * 2u;
        }
    }
    file_gvar_off = gvar_off;
    gvar_ready = true;
    current_weight = 400;
    ESP_LOGI(
        TAG, "gvar wght %d..%d def %d, %d shared tuples",
        wght_min, wght_max, wght_def, shared_tuple_count
    );
    return true;
}

// 把当前字形及其复合引用从 SD 填进工作字体的 glyf 窗口，并改写 loca。
static bool pack_glyph_tree(int root_gid) {
    if (root_gid == packed_root && current_weight == packed_weight) {
        STATS.pack_memo++;
        return true;
    }
    if (root_gid < 0 || root_gid >= num_glyphs) return true;

    int tree[TTF_TREE_MAX];
    int tree_n = 0;
    tree[tree_n++] = root_gid;

    uint8_t* arena = font_data + work_glyf_off;
    uint32_t packed = 0;
    for (int i = 0; i < tree_n; i++) {
        int gid = tree[i];
        uint32_t len = file_glyph_len(gid);
        // 64 位比较：len 是两张 loca 偏移之差，畸形 loca 能给到接近 2^32，`packed + len`
        // 在 32 位下会回绕（packed=4、len=0xFFFFFFFC 得 0）从而骗过检查，随后
        // file_read_at 把数据写进这个 128KB 窗口之外。
        if ((uint64_t)packed + len > TTF_GLYF_ARENA) {
            ESP_LOGE(TAG, "glyf arena overflow gid=%d len=%u", gid, (unsigned)len);
            return false;
        }
        uint8_t* blob = arena + packed;
        if (len > 0) STATS.pack_read++;
        if (len > 0
            && !file_read_at(file_glyf_off + file_glyph_off(gid), blob, len)) {
            return false;
        }
        uint32_t used = len;
        if (len > 0) {
            uint32_t varied = len;
            if (apply_gvar_to_blob(
                    blob, &varied, TTF_GLYF_ARENA - packed, gid
                )) {
                used = varied;
            }
        }
        set_work_loca(gid, packed);
        packed += used;
        packed = (packed + 3u) & ~3u;
        if (packed > TTF_GLYF_ARENA) return false;
        set_work_loca(gid + 1, packed);
        if (used >= 2 && !enqueue_composite_children(blob, used, tree, &tree_n)) {
            return false;
        }
    }
    packed_root = root_gid;
    packed_weight = current_weight;
    return true;
}

static bool copy_table(
    uint8_t* dst, uint32_t dst_off, uint32_t file_off, uint32_t length
) {
    return file_read_at(file_off, dst + dst_off, length);
}

static uint8_t* build_working_font(const uint8_t* header, size_t header_len) {
    const char* tags[] = { "cmap", "glyf", "head", "hhea", "hmtx", "loca", "maxp" };
    sfnt_table_t src[7];
    for (int i = 0; i < 7; i++) {
        memcpy(src[i].tag, tags[i], 4);
        if (memcmp(tags[i], "glyf", 4) == 0) {
            uint32_t glyf_len = 0;
            if (!find_sfnt_table(header, header_len, "glyf", &file_glyf_off, &glyf_len)) {
                return NULL;
            }
            file_glyf_len = glyf_len;
            src[i].offset = 0;
            src[i].length = TTF_GLYF_ARENA;
            continue;
        }
        if (!find_sfnt_table(
                header, header_len, tags[i], &src[i].offset, &src[i].length
            )) {
            ESP_LOGE(TAG, "missing table %.4s", tags[i]);
            return NULL;
        }
    }

    // 表长/表偏移是文件**自报**的（find_sfnt_table 原样采信）。畸形 .ttf 里一个接近
    // 2^32 的 length 会让下面的 4 字节对齐累加回绕、total 反而变小：data 按
    // 小 total 分配，copy_table 却往里写 length 字节 → 堆越界写。先逐表卡上限，再用
    // 64 位累加核对总量。
    const uint32_t kTableMaxBytes = 16u * 1024 * 1024;
    for (int i = 0; i < 7; i++) {
        if (src[i].length > kTableMaxBytes) {
            ESP_LOGE(TAG, "table %.4s 长度异常 (%u)", src[i].tag, (unsigned)src[i].length);
            return NULL;
        }
    }

    const uint32_t cursor = 12 + 7 * 16;
    uint32_t dst_off[7];
    uint64_t total64 = cursor;
    for (int i = 0; i < 7; i++) {
        dst_off[i] = (uint32_t)total64;
        total64 = ((total64 + src[i].length) + 3u) & ~(uint64_t)3u;
        if (total64 > 64u * 1024 * 1024) {
            ESP_LOGE(TAG, "工作字体过大 (%llu)", (unsigned long long)total64);
            return NULL;
        }
    }
    const uint32_t total = (uint32_t)total64;

    uint8_t* data = heap_caps_calloc(1, total, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (data == NULL) {
        // 这一块要**连续** total 字节（大 CJK 字体几百 KB），而 PSRAM 碎片化之后
        // 明明还空着一两 MB 却要不到 —— 占着坑的正是各字面那几块永不释放的预取缓存
        // （见 ttf_purge_io_caches）。它们丢得起，字体丢不起：放掉再试一次。
        const size_t freed = ttf_purge_io_caches();
        if (freed > 0) {
            ESP_LOGW(
                TAG, "字体工作缓冲 %u KB 要不到连续 PSRAM，先放掉 %u KB 预取缓存再试",
                (unsigned)(total / 1024), (unsigned)(freed / 1024)
            );
            data = heap_caps_calloc(1, total, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        }
    }
    if (data == NULL) return NULL;

    put_be32(data, 0x00010000);
    put_be16(data + 4, 7);
    put_be16(data + 6, 64);
    put_be16(data + 8, 2);
    put_be16(data + 10, 32);

    for (int i = 0; i < 7; i++) {
        uint8_t* rec = data + 12 + i * 16;
        memcpy(rec, src[i].tag, 4);
        put_be32(rec + 8, dst_off[i]);
        put_be32(rec + 12, src[i].length);
        if (memcmp(src[i].tag, "glyf", 4) == 0) continue;
        if (!copy_table(data, dst_off[i], src[i].offset, src[i].length)) {
            heap_caps_free(data);
            return NULL;
        }
    }

    work_loca_off = dst_off[5];
    work_glyf_off = dst_off[1];
    file_loca_len = src[5].length;
    file_loca = heap_caps_malloc(file_loca_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (file_loca == NULL) {
        heap_caps_free(data);
        return NULL;
    }
    memcpy(file_loca, data + work_loca_off, file_loca_len);

    // head/maxp 是目录里靠后的表，声明长度可能短于我们要读的字段（head 读偏移 50、
    // maxp 读偏移 4）——那读的就是 data 缓冲区**之外**。先卡这两张表的最小长度。
    if (src[2].length < 54 || src[6].length < 6) {
        ESP_LOGE(TAG, "head/maxp 表过短 (%u/%u)", (unsigned)src[2].length, (unsigned)src[6].length);
        heap_caps_free(file_loca);
        file_loca = NULL;
        heap_caps_free(data);
        return NULL;
    }
    uint8_t* head = data + dst_off[2];
    loca_long = be16(head + 50) != 0;
    uint8_t* maxp = data + dst_off[6];
    num_glyphs = be16(maxp + 4);

    // loca 表长度必须装得下 (num_glyphs+1) 个偏移：file_glyph_off(gid) 允许
    // gid == num_glyphs（末项），而数组是按 loca 表**自报**的长度分配的 —— 表被声明得
    // 很短、maxp 里 numGlyphs 却很大时，就是越界读 loca。
    const size_t loca_need = ((size_t)num_glyphs + 1) * (loca_long ? 4u : 2u);
    if (loca_need > file_loca_len) {
        ESP_LOGE(TAG, "loca 表过短: 需要 %u, 实际 %u", (unsigned)loca_need, (unsigned)file_loca_len);
        heap_caps_free(file_loca);
        file_loca = NULL;
        heap_caps_free(data);
        return NULL;
    }
    return data;
}

static uint32_t decode_utf8(const char** cursor) {
    const uint8_t* p = (const uint8_t*)*cursor;
    if (p[0] == 0) return 0;
    if (p[0] < 0x80) {
        *cursor += 1;
        return p[0];
    }
    if ((p[0] & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) {
        *cursor += 2;
        return ((uint32_t)(p[0] & 0x1F) << 6) | (p[1] & 0x3F);
    }
    if ((p[0] & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
        *cursor += 3;
        return ((uint32_t)(p[0] & 0x0F) << 12)
            | ((uint32_t)(p[1] & 0x3F) << 6)
            | (p[2] & 0x3F);
    }
    if ((p[0] & 0xF8) == 0xF0 && (p[1] & 0xC0) == 0x80
        && (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80) {
        *cursor += 4;
        return ((uint32_t)(p[0] & 0x07) << 18)
            | ((uint32_t)(p[1] & 0x3F) << 12)
            | ((uint32_t)(p[2] & 0x3F) << 6)
            | (p[3] & 0x3F);
    }
    *cursor += 1;
    return 0xFFFD;
}

static int clamp_size(int size) {
    return size == TTF_SIZE_LARGE ? TTF_SIZE_LARGE : TTF_SIZE_SMALL;
}

static int size_to_px(int size) {
    return clamp_size(size) == TTF_SIZE_LARGE ? TTF_PIXEL_LARGE : TTF_PIXEL_SMALL;
}

static int clamp_px(int pixel_height) {
    if (pixel_height < 12) return 12;
    if (pixel_height > 120) return 120;
    return pixel_height;
}

/* ---- 字形缓存 / Glyph cache ---- */
static unsigned cache_bucket(uint32_t codepoint, int size) {
    return (unsigned)((codepoint * 33u + (uint32_t)size) % TTF_CACHE_BUCKETS);
}

// 新条目 calloc 后 prev/next 都是 NULL，但并不在链表里。若把「prev==NULL」
// 当成「我就是头」，会把整条 LRU 掐掉，只剩刚插入的那一个；unload / 超限
// 淘汰都只走 LRU，于是旧字形永远留在哈希桶里，换字体后还会被命中。
static void lru_detach(glyph_entry_t* entry) {
    if (entry->lru_prev != NULL) {
        entry->lru_prev->lru_next = entry->lru_next;
    } else if (lru_head == entry) {
        lru_head = entry->lru_next;
    }
    if (entry->lru_next != NULL) {
        entry->lru_next->lru_prev = entry->lru_prev;
    } else if (lru_tail == entry) {
        lru_tail = entry->lru_prev;
    }
    entry->lru_prev = NULL;
    entry->lru_next = NULL;
}

static void lru_touch(glyph_entry_t* entry) {
    if (lru_head == entry) return;
    lru_detach(entry);
    entry->lru_next = lru_head;
    if (lru_head != NULL) lru_head->lru_prev = entry;
    lru_head = entry;
    if (lru_tail == NULL) lru_tail = entry;
}

static void cache_reset(void) {
    size_t n = 0;
    for (unsigned i = 0; i < TTF_CACHE_BUCKETS; i++) {
        glyph_entry_t* entry = cache_buckets[i];
        cache_buckets[i] = NULL;
        while (entry != NULL) {
            glyph_entry_t* next = entry->hash_next;
            heap_caps_free(entry->bitmap);
            heap_caps_free(entry);
            entry = next;
            n++;
        }
    }
    lru_head = NULL;
    lru_tail = NULL;
    cache_bytes = 0;
    // ASCII 步进表也得作废：它由本面的 cmap/hmtx 算出来，换字体后就不对了。放在这里
    // 是因为**每一条**换字体/卸载的路都经过 cache_reset —— face_unload() 直接调它，
    // ttf_font_cache_clear()(→ reloadFont) 也是。像素高变化不走这里，只换表键(见 ascii_adv_ensure)。
    ascii_adv_px = 0;
    if (n > 0) {
        ESP_LOGI(TAG, "glyph cache dropped %u entries", (unsigned)n);
    }
}

static void cache_evict(glyph_entry_t* victim) {
    if (victim == NULL) return;
    lru_detach(victim);

    unsigned bucket = cache_bucket(victim->codepoint, victim->size);
    glyph_entry_t** slot = &cache_buckets[bucket];
    while (*slot != NULL) {
        if (*slot == victim) {
            *slot = victim->hash_next;
            break;
        }
        slot = &(*slot)->hash_next;
    }

    cache_bytes -= victim->bitmap_bytes + sizeof(*victim);
    heap_caps_free(victim->bitmap);
    heap_caps_free(victim);
}

// 当前这一刻的字形缓存额度。静态上限只是天花板；实际额度还要看 PSRAM 真剩多少，
// 因为图片解码/解压窗口/网络缓冲跟我们抢的是同一块内存（阅读器一页 XTC 能要 4MB）。
// 规则：空闲量高于 TTF_CACHE_RESERVE 时，允许长到"刚好把空闲压到留量线"。
//
// ⚠ 低于留量线时 room 归零、limit 正好等于当前占用，于是"腾地方"就成了每次插入都要
//   做的事 —— 这时候**怎么腾**决定生死，见 cache_reserve() 那段。
//
// 算式只此一份，对**任意**字面求值（不依赖 s_cur）：ttf_perf_line() 要逐字面报"额度"，
// 在那儿照抄一遍迟早与本处分家。cache_effective_limit() 就是它的 s_cur 版。
static size_t cache_effective_limit_of(const ttf_face_t* f) {
    const size_t free_bytes = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    const size_t room = free_bytes > TTF_CACHE_RESERVE ? free_bytes - TTF_CACHE_RESERVE : 0;
    const size_t room_ceiling = f->f_cache_bytes + room;
    return room_ceiling < f->f_cache_limit ? room_ceiling : f->f_cache_limit;
}

static size_t cache_effective_limit(void) { return cache_effective_limit_of(s_cur); }

// 位图缓存**还能再装多少**。判"这一趟栅格化预热值不值得做"只能用这个：
// cache_effective_limit() 在 room==0 时恰好等于 cache_bytes，"额度 ≥ 当前占用"恒真
// —— 拿它直接比等于没判（这条早退一直没做出来就是因为这一点）。
static size_t cache_headroom(void) {
    const size_t limit = cache_effective_limit();
    return limit > cache_bytes ? limit - cache_bytes : 0;
}

// 按**字节**挑一个牺牲品：腾出来的量最接近还差多少(need)。只给零余量那条路用，见下。
// 从最老的一端(lru_tail)往新扫，同样接近时先淘汰老的 —— 同尺寸条目上这条退化成纯 LRU，
// 而 CJK 正文绝大多数条目尺寸相近，所以常见情况下与老行为一致。
// 找不到单个够大的就退到"挑最大的"，差额下一轮回补。
static glyph_entry_t* cache_pick_bestfit(size_t need) {
    glyph_entry_t* best = NULL;
    size_t best_size = 0;
    for (glyph_entry_t* e = lru_tail; e != NULL; e = e->lru_prev) {
        const size_t sz = e->bitmap_bytes + sizeof(*e);
        if (sz < need) continue;
        if (best == NULL || sz < best_size) {
            best = e;
            best_size = sz;
            if (sz == need) break;
        }
    }
    if (best != NULL) return best;
    size_t max_size = 0;
    for (glyph_entry_t* e = lru_tail; e != NULL; e = e->lru_prev) {
        const size_t sz = e->bitmap_bytes + sizeof(*e);
        if (sz > max_size) {
            max_size = sz;
            best = e;
        }
    }
    return best;
}

static void cache_reserve(size_t extra) {
    const size_t limit = cache_effective_limit();
    if (cache_bytes + extra <= limit) return;   // 有余量，直接插（正常那一档）
    if (limit <= cache_bytes) {
        // 零余量：额度正好等于当前占用，插一个就得腾出等量字节。**这里不能按 LRU 整条腾**：
        // 条目是 1.8KB 级的 CJK，而新字形常常只有一两百字节（标点/拉丁/空白），按条目腾
        // 每插一个净缩水 v−extra，一拍就能啃掉一两百 KB，最后只剩一个格子、位图命中归 0
        // （实测 275K→210K→66K→2K→0K，位图 103/157→0/166，绘制 668→1380ms）。
        // 触发它的不是别人，是**我们自己的帧缓冲**：常驻首帧 2×415872 + 预渲染备用帧缓冲
        // 415872 = 1.24MB，落在 free≈1.5MB 的窗口里就把 free 压到留量线以下。
        // 所以这里改成按字节 best-fit 腾：Σv ≈ extra，缓存**保持原大小并且还能继续轮换**
        // （比"冻住不插"好：新页的字形照样进得来，命中的那部分跟着页走）。
        while (lru_tail != NULL && cache_bytes + extra > limit) {
            const size_t need = cache_bytes + extra - limit;
            glyph_entry_t* victim = cache_pick_bestfit(need);
            if (victim == NULL) break;
            cache_evict(victim);
        }
        return;
    }
    // 有增长空间：按 LRU 腾到装得下为止（这条路的命中率是 96–99% 那档，别动它）。
    while (lru_tail != NULL && cache_bytes + extra > limit) {
        cache_evict(lru_tail);
    }
}

// 字号 → stb 缩放系数。**必须按 em 归一**，不能用 stbtt_ScaleForPixelHeight：
// 那个是 pixels/(hhea.ascender - hhea.descender)，而这个差值与 em 的比值每个字体
// 都不一样（Noto CJK 是 1448/1000，很多汉字字体 1200/1000，也有正好 1000/1000 的）。
// 于是同样标称 46 号：比值 1000 的字体画满 46px，比值 1448 的只画到 32px ——
// 用户看到的就是"不同字体同一个字号大小差很多"。
// 按 em 归一后，"46 号"对所有字体都是"em 框 46px"，这才是字号该有的语义。
// 内置字体的 hhea 当初就是特意改成 880/-120（差=em 1000）的，所以它一分不变。
static float font_scale_px(int pixel_height) {
    return stbtt_ScaleForMappingEmToPixels(&font_info, (float)pixel_height);
}

static glyph_entry_t* cache_lookup(uint32_t codepoint, int size) {
    unsigned bucket = cache_bucket(codepoint, size);
    for (glyph_entry_t* entry = cache_buckets[bucket]; entry != NULL; entry = entry->hash_next) {
        if (entry->codepoint == codepoint && entry->size == (uint8_t)size
            && entry->weight == (uint16_t)current_weight
            && entry->synth_dw == (int16_t)synth_dw_now()) {
            lru_touch(entry);
            return entry;
        }
    }
    return NULL;
}

/* ── 缺字替补 / Missing-glyph fallback ────────────────────────────────────
 * 一个面画不出某个码点时(FindGlyphIndex 给 .notdef)，去别的面找一个能画出它的。
 * 这是"界面文本改用用户字体"之后必须补上的一环：用户的字体未必覆盖内置那 7717 字，
 * 一个菜单里的生僻字就能整块变成豆腐块。字形与缓存都记在**替补那一面**上。
 * 替补面各自的缓存独立，所以本面缓存不被污染；下次遇到同一个码点仍会先在本面
 * 查一次(未命中)再走这里，代价只有一次 cmap 查表，且只发生在缺字时。
 */
static bool face_has_cp(int role, uint32_t cp) {
    const ttf_face_t* f = &s_faces[role];
    if (!f->f_font_ready) return false;
    return stbtt_FindGlyphIndex(&f->f_font_info, (int)cp) != 0;
}

// 替补顺序：**与当前面互补的那一面优先**。
//   当前是正文面(CONTENT)          → 先这本书自己的家族面(ALT/ALT2)、再内置面。
//   当前是内置面(界面外壳)         → 内容面：用户装的字体 CJK 覆盖通常比内置子集更宽。
//   当前是书内某个家族面(ALT/ALT2) → 先内容面、再**另一个家族面**：同一本书、同一套观感；
//                                    最后才轮到内置面（覆盖最全，兜底）。
// 未打开的面 face_has_cp 立刻返回 false，所以给两家族的书多带一列、多试一个家族面不花时间。
// 返回角色；-1 = 谁都没有(调用方照旧画 .notdef)。
//
// ★ 正文面的第一替补是**书里的家族面**，不是内置面 —— 这条曾经是反的，代价是整块注文变
// 空白。内置面那 7717 字是"通用汉字最全"，但一本学术书的字表可以整块落在通用表之外：
// 莊子校詮 正文面是书里 2965 字形的书宋子集，注文那 5743 个字里有 2848 个不在它里面，
// 也不在内置面里（内建只cover 该书的 2815/5824），而书里那份 6117 字形的注文字面
// (家族 st4) 覆盖 5752/5824 —— 它当时根本没被装进 ALT 槽（选面口径所限，见 Epub.cpp 的
// resolveEmbeddedFonts），就算装进来了，正文面也只会去问内置面，问不到手上就有的那一份。
static int fallback_role_for(uint32_t cp) {
    // 行 = 当前面，列 = 依次尝试的替补面，-1 表示到此为止。**四个角色各一行**，
    // TTF_ROLE_COUNT 变了这里必须跟着变（列数=角色数）。
    static const int8_t kOrder[TTF_ROLE_COUNT][TTF_ROLE_COUNT] = {
        /* CONTENT      */ {TTF_ROLE_CONTENT_ALT, TTF_ROLE_CONTENT_ALT2, TTF_ROLE_UI, -1},
        /* UI           */ {TTF_ROLE_CONTENT, -1, -1, -1},
        /* CONTENT_ALT  */ {TTF_ROLE_CONTENT, TTF_ROLE_CONTENT_ALT2, TTF_ROLE_UI, -1},
        /* CONTENT_ALT2 */ {TTF_ROLE_CONTENT, TTF_ROLE_CONTENT_ALT, TTF_ROLE_UI, -1},
    };
    // 用指针差算下标，别再写比较链 —— 加面时漏一行的表现是"替补悄悄串到别的面"，
    // 那种 bug 在屏上只是偶尔一个错字形，很难查。
    const int cur = (int)(s_cur - s_faces);
    for (int i = 0; i < TTF_ROLE_COUNT; i++) {
        const int role = kOrder[cur][i];
        if (role < 0) break;
        if (role == cur) continue;   // 同一面不必再试：它正是画不出来的那一个
        if (face_has_cp(role, cp)) return role;
    }
    return -1;
}

/* ---- 合成加粗 / Synthetic emboldening ------------------------------------
 * 内置字体（Noto Sans CJK SC Regular）只有 400 一份、没有 fvar/gvar，用户装的静态字体
 * 多半也没有 —— 这些面只能靠"按 em 比例膨胀笔画"来假装更粗，口径与 KOReader/crengine
 * 的 FT_Outline_Embolden 一致：字形总宽增加 δ = em·Δw/6400，每边各让 δ/2。
 *
 * stb 的公开 API 没有 FT_Outline_Embolden 的对应物（那是对**轮廓**做几何外扩），所以
 * 做法是把同一字形按亚像素偏移栅 5 张（中心 + 上下左右各 r），逐点取最大值 —— 覆盖率的
 * 逐点 max 正是几何并集：笔画外扩 r，边缘照样带抗锯齿。并集框比原框大约 ⌈r⌉/边，left/top
 * 跟着往外挪，正好等价于 crengine 用 _synth_weight_half_strength 做的重心补偿：字形在
 * 自己的步进格子里居中，不掉墨、也不改步进。
 *
 * r 上限 2px（再大也会吃掉相邻字的字身，而面板只有 16 级灰）；小于 0.08px 按 0 处理
 * （那是量化噪声，不值得为它多栅 4 张 —— 排档第一档 +20 就落在这条线以下，正文各号都
 * 画成原样，正好当"不加粗"使）。**只有加粗这一个方向**：stb 没有轮廓腐蚀的公开 API，
 * "取最小"会把细笔画整条抹掉，那是"字发虚"不是"变细"，所以排版设定干脆不提供细档。
 */
#define TTF_SYNTH_DW_PER_PX 12800.0f   // 每 1px 半径对应的字重增量（= 6400×2，即 δ/2）
#define TTF_SYNTH_R_MAX 2.0f
#define TTF_SYNTH_R_MIN 0.08f

// 5 个采样点的亚像素偏移（r = 0 时只用中心那一个）。stb 的 shift 是**画布像素**单位
// （见 GetGlyphBitmapBoxSubpixel 里的 x0*scale_x + shift_x），正是这里要的。
static void synth_offsets(float r, float* ox, float* oy) {
    ox[0] = 0.0f;  oy[0] = 0.0f;
    ox[1] = r;     oy[1] = 0.0f;
    ox[2] = -r;    oy[2] = 0.0f;
    ox[3] = 0.0f;  oy[3] = r;
    ox[4] = 0.0f;  oy[4] = -r;
}

// 复用的临时位图（一张采样的覆盖图）。只增长不释放：与 f_touch_blocks 同一套内存策略 ——
// 加粗档是用户常开的状态，每个字形两次 malloc/free 没必要；它也不属于任何字面。
static uint8_t* s_synth_scratch;
static size_t s_synth_cap;

// 把 ns 张采样并进 dst（dst 是并集框尺寸 w×h、原点在 (dst_x0,dst_y0) 的那块，**进来时已清 0**）。
// 失败（scratch 分配不出来）返回 false，调用方退回单采样 —— 宁可不加粗，也不能画不出字。
static bool synth_merge(int gid, float scale, const float* ox, const float* oy, int ns,
                        uint8_t* dst, int w, int h, int dst_x0, int dst_y0) {
    const size_t need = (size_t)(w + 2) * (size_t)(h + 2);   // 单张的框 ≤ 并集框 + 1px/边
    if (s_synth_cap < need) {
        uint8_t* p = (uint8_t*)heap_caps_malloc(need, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (p == NULL) return false;
        if (s_synth_scratch != NULL) heap_caps_free(s_synth_scratch);
        s_synth_scratch = p;
        s_synth_cap = need;
    }
    for (int i = 0; i < ns; i++) {
        int bx0 = 0, by0 = 0, bx1 = 0, by1 = 0;
        stbtt_GetGlyphBitmapBoxSubpixel(
            &font_info, gid, scale, scale, ox[i], oy[i], &bx0, &by0, &bx1, &by1
        );
        const int sw = bx1 - bx0;
        const int sh = by1 - by0;
        if (sw <= 0 || sh <= 0) continue;                     // 这一张没墨（空格之类）
        if ((size_t)sw * (size_t)sh > s_synth_cap) continue;
        stbtt_MakeGlyphBitmapSubpixel(
            &font_info, s_synth_scratch, sw, sh, sw, scale, scale, ox[i], oy[i], gid
        );
        const int dx = bx0 - dst_x0;
        const int dy = by0 - dst_y0;
        for (int y = 0; y < sh; y++) {
            const int ty = dy + y;
            if (ty < 0 || ty >= h) continue;
            const uint8_t* srow = s_synth_scratch + (size_t)y * (size_t)sw;
            uint8_t* drow = dst + (size_t)ty * (size_t)w;
            for (int x = 0; x < sw; x++) {
                const int tx = dx + x;
                if (tx >= 0 && tx < w && srow[x] > drow[tx]) drow[tx] = srow[x];
            }
        }
    }
    return true;
}

static glyph_entry_t* rasterize_glyph(uint32_t codepoint, int pixel_height) {
    STATS.bmp_miss++;
    int gid = stbtt_FindGlyphIndex(&font_info, (int)codepoint);
    if (!pack_glyph_tree(gid)) return NULL;

    float scale = font_scale_px(pixel_height);

    // 本张位图要多少合成加粗（0 = 原样，默认档就是它）。
    const int synth_dw = synth_dw_now();
    float r = 0.0f;
    if (synth_dw > 0) {
        r = (float)pixel_height * (float)synth_dw / TTF_SYNTH_DW_PER_PX;
        if (r > TTF_SYNTH_R_MAX) r = TTF_SYNTH_R_MAX;
        if (r < TTF_SYNTH_R_MIN) r = 0.0f;
    }
    const int ns = (r > 0.0f) ? 5 : 1;
    float ox[5], oy[5];
    synth_offsets(r, ox, oy);

    // 框取 5 张采样的并集；r = 0 时就是原框，与加这个功能之前逐值一致。
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    for (int i = 0; i < ns; i++) {
        int bx0 = 0, by0 = 0, bx1 = 0, by1 = 0;
        stbtt_GetGlyphBitmapBoxSubpixel(
            &font_info, gid, scale, scale, ox[i], oy[i], &bx0, &by0, &bx1, &by1
        );
        if (i == 0) {
            x0 = bx0; y0 = by0; x1 = bx1; y1 = by1;
        } else {
            if (bx0 < x0) x0 = bx0;
            if (by0 < y0) y0 = by0;
            if (bx1 > x1) x1 = bx1;
            if (by1 > y1) y1 = by1;
        }
    }

    int width = x1 - x0;
    int height = y1 - y0;
    if (width < 0) width = 0;
    if (height < 0) height = 0;

    size_t bitmap_bytes = (size_t)width * (size_t)height;
    cache_reserve(bitmap_bytes + sizeof(glyph_entry_t));

    glyph_entry_t* entry = heap_caps_calloc(
        1, sizeof(*entry), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
    );
    if (entry == NULL) return NULL;

    if (bitmap_bytes > 0) {
        entry->bitmap = heap_caps_malloc(
            bitmap_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
        );
        if (entry->bitmap == NULL) {
            heap_caps_free(entry);
            return NULL;
        }
        if (ns == 1) {
            stbtt_MakeGlyphBitmap(
                &font_info, entry->bitmap, width, height, width, scale, scale, gid
            );
        } else {
            // max 合并要求底子是 0；兜底路径也靠这一记清零（它只写自己那块子矩形）。
            memset(entry->bitmap, 0, bitmap_bytes);
            if (!synth_merge(
                    gid, scale, ox, oy, ns, entry->bitmap, width, height, x0, y0
                )) {
                // 兜底：scratch 分配不出来时不加粗也得把字画出来。单采样那张的框比并集框
                // 小一点，得写进它在并集框里的位置，不能直接铺满整块（那样会挪位）。
                int ux0 = 0, uy0 = 0, ux1 = 0, uy1 = 0;
                stbtt_GetGlyphBitmapBox(
                    &font_info, gid, scale, scale, &ux0, &uy0, &ux1, &uy1
                );
                const int uw = ux1 - ux0;
                const int uh = uy1 - uy0;
                if (uw > 0 && uh > 0) {
                    uint8_t* dst = entry->bitmap + (size_t)(uy0 - y0) * (size_t)width
                                   + (size_t)(ux0 - x0);
                    stbtt_MakeGlyphBitmap(
                        &font_info, dst, uw, uh, width, scale, scale, gid
                    );
                }
            }
        }
    }

    int advance = 0;
    int lsb = 0;
    stbtt_GetGlyphHMetrics(&font_info, gid, &advance, &lsb);

    entry->codepoint = codepoint;
    entry->size = (uint8_t)pixel_height;
    entry->weight = (uint16_t)current_weight;
    entry->synth_dw = (int16_t)synth_dw;
    entry->width = (int16_t)width;
    entry->height = (int16_t)height;
    entry->left = (int16_t)x0;
    entry->top = (int16_t)(-y0);
    entry->advance_x = (int16_t)lroundf(advance * scale);
    entry->bitmap_bytes = bitmap_bytes;

    unsigned bucket = cache_bucket(codepoint, pixel_height);
    entry->hash_next = cache_buckets[bucket];
    cache_buckets[bucket] = entry;
    cache_bytes += bitmap_bytes + sizeof(*entry);
    lru_touch(entry);
    return entry;
}

static const glyph_entry_t* get_glyph(uint32_t codepoint, int pixel_height) {
    if (!font_ready) return NULL;
    pixel_height = clamp_px(pixel_height);
    glyph_entry_t* entry = cache_lookup(codepoint, pixel_height);
    if (entry != NULL) {
        STATS.bmp_hit++;
        return entry;
    }
    // 本面没有这个字形(.notdef)：别把豆腐块栅进本面缓存，换个能画出它的面
    // (见 fallback_role_for)，字形与缓存都记在**那一面**上。条目跨 face_leave
    // 依然有效(缓存挂在字面结构上，与 s_cur 无关)，寿命与本面字形完全一样 ——
    // 后续栅格化把它挤出 LRU 时指针一样失效，而调用方一律是"取到就画"。
    if (stbtt_FindGlyphIndex(&font_info, (int)codepoint) == 0) {
        const int role = fallback_role_for(codepoint);
        if (role >= 0) {
            ttf_face_t* prev = face_enter(role);
            entry = cache_lookup(codepoint, pixel_height);
            if (entry == NULL) entry = rasterize_glyph(codepoint, pixel_height);
            face_leave(prev);
            if (entry != NULL) return entry;
        }
    }
    entry = rasterize_glyph(codepoint, pixel_height);
    return entry;
}

static void warm_text_io(int pixel_height, const char* text) {
    // 内建面(font_mem != NULL)从 flash 直读，没有"随机 SD 读延迟"要藏，预取纯属浪费
    // ——而且它会经 io_ensure 把 512KB 的 io_data 分配出来。直接跳过。
    if (text == NULL || glyf_ram != NULL || font_mem != NULL) return;
    // 去重表与块缓存同一份内存策略：走 SD 流式读的字体在 open 时就已经 io_ensure 过，
    // 这里只是兜底(表真没分配就现开一次，开不出来就放弃预取，不影响正确性)。
    if (touch_blocks == NULL || warm_cps == NULL) {
        if (!io_ensure()) return;
        if (touch_blocks == NULL || warm_cps == NULL) return;
    }
    touch_n = 0;
    const char* cursor = text;
    uint32_t* cps = warm_cps;
    int seen = 0;
    pixel_height = clamp_px(pixel_height);
    while (*cursor != '\0' && seen < TTF_GATHER_MAX) {
        uint32_t cp = decode_utf8(&cursor);
        if (cp == 0) break;
        bool dup = false;
        for (int i = 0; i < seen; i++) {
            if (cps[i] == cp) {
                dup = true;
                break;
            }
        }
        if (dup) continue;
        cps[seen++] = cp;
        if (cache_lookup(cp, pixel_height) != NULL) {
            STATS.warm_skip++;
            continue;
        }
        STATS.warm_touch++;
        int gid = stbtt_FindGlyphIndex(&font_info, (int)cp);
        if (gid < 0 || gid >= num_glyphs) continue;
        io_touch(file_glyf_off + file_glyph_off(gid), file_glyph_len(gid));
        if (gvar_ready && current_weight != wght_def && gvar_glyph_off != NULL) {
            uint32_t a = gvar_glyph_off[gid];
            uint32_t b = gvar_glyph_off[gid + 1];
            if (b > a) {
                io_touch(file_gvar_off + gvar_data_array_off + a, b - a);
            }
        }
    }
    io_flush_touches();
}

// 单码点步进，直查 cmap/hmtx（不改任何缓存；表就是由它填出来的）。
static int cp_advance_raw(int pixel_height, uint32_t cp) {
    const float scale = font_scale_px(pixel_height);
    int advance = 0, lsb = 0;
    int gid = stbtt_FindGlyphIndex(&font_info, (int)cp);
    // 缺字时和 get_glyph 走同一套替补：字体会在替补面上取字形，步进量就得跟着
    // 走，否则量出来的宽度和画出来的字对不上(内置 .notdef 与 CJK 全角差得最远)。
    // 只读另一面的 cmap/hmtx，不触轮廓，分页开销的量级不变。
    if (gid == 0) {
        const int role = fallback_role_for(cp);
        if (role >= 0) {
            ttf_face_t* prev = face_enter(role);
            const float fscale = font_scale_px(pixel_height);
            const int fgid = stbtt_FindGlyphIndex(&font_info, (int)cp);
            stbtt_GetGlyphHMetrics(&font_info, fgid, &advance, &lsb);
            face_leave(prev);
            return (int)lroundf(advance * fscale);
        }
    }
    stbtt_GetGlyphHMetrics(&font_info, gid, &advance, &lsb);
    return (int)lroundf(advance * scale);
}

/* ---- ASCII(<0x80) 步进表 / ASCII advance table ----
 * FontRenderer::charWidth() 现在返回**真实步进**（装了外置字体时拉丁不再恒为半格），
 * 而调用它的 buildVrows() 每次按键都要把整篇文档重排一遍 —— 单个 ASCII 码点查一次
 * cmap(二分)+hmtx 约 150ns，万字笔记每键就是 1.5ms 纯开销。表把这一步变成一次数组读。
 *
 * 表里的值**就是** cp_advance_raw() 对该码点的结果（同一个函数算的），所以「量的」与
 * 「画的」相等是构造保证，不是两处人肉对齐。
 *
 * 键只按像素高：步进与字重无关(hmtx/gvar 都不改它)。像素高变了**只换键不清表** ——
 * FontScope 让正文/界面格子每帧来回切，清表等于每帧重测 128 次。作废只发生在
 * cache_reset()(换字体/卸载)，因为值来自本面的 cmap/hmtx。
 */
static void ascii_adv_ensure(int pixel_height) {
    if (ascii_adv_px == pixel_height) return;
    for (int i = 0; i < TTF_ASCII_ADV_N; i++) {
        ascii_adv[i] = (int16_t)cp_advance_raw(pixel_height, (uint32_t)i);
    }
    ascii_adv_px = pixel_height;
}

// 单码点步进(px)。measure_width / ttf_text_width_px 与 FontRenderer::charWidth 都走这里，
// 所以「量」和「画」不可能分家。
static int cp_advance(int pixel_height, uint32_t cp) {
    if (cp < TTF_ASCII_ADV_N) {
        ascii_adv_ensure(pixel_height);
        return ascii_adv[cp];
    }
    return cp_advance_raw(pixel_height, cp);
}

static int measure_width(int pixel_height, const char* text) {
    // 字宽只依赖已驻留的 cmap/hmtx；分页不读取轮廓、不生成整章位图。
    // Resident cmap/hmtx suffice for advances; pagination never reads outlines or rasterizes a chapter.
    // 先夹一次 px：表的键必须是规范值，否则同一个有效字号会反复重建(见 ascii_adv_ensure)。
    pixel_height = clamp_px(pixel_height);
    int width = 0;
    const char* cursor = text;
    while (*cursor != '\0') {
        uint32_t cp = decode_utf8(&cursor);
        width += cp_advance(pixel_height, cp);
    }
    return width;
}

bool ttf_font_ready(void) {
    return s_faces[TTF_ROLE_CONTENT].f_font_ready;
}

void ttf_set_weight(int wght) {
    s_faces[TTF_ROLE_CONTENT].f_current_weight = clamp_weight(wght);
}

int ttf_get_weight(void) {
    return s_faces[TTF_ROLE_CONTENT].f_current_weight;
}

int ttf_ascender(int size) {
    return ttf_ascender_px(size_to_px(size));
}

int ttf_ascender_px(int pixel_height) {
    if (!font_ready) return 0;
    pixel_height = clamp_px(pixel_height);
    return (int)lroundf(raw_ascent_units * font_scale_px(pixel_height));
}

// 临时诊断：把上一步的字形路径计数取出来、清零、格式化成一行。
// **读的就是"上一次调用到现在"这一段**，所以调用点就是分段点（一趟预渲染 / 一次翻页）。
// 逐字面分开：内嵌多字体那一档怀疑就在"正文"面（st），家族面/内建面会把数摊薄。
// 末尾带上 PSRAM 余量与 cache_reserve 之上的空档（room）—— 位图缓存的额度就是它，
// room 塌到 0 意味着任何一次插入都把整个字形缓存冲掉（见 cache_effective_limit）。
void ttf_perf_line(char* out, size_t n) {
    if (out == NULL || n == 0) return;
    out[0] = '\0';
    size_t off = 0;
    static const char* const kRoleName[TTF_ROLE_COUNT] = {"正文", "界面", "次家族", "三家族"};
    for (int r = 0; r < TTF_ROLE_COUNT; r++) {
        ttf_face_stats_t* s = &s_stats[r];
        const bool any = s->pack_read || s->pack_memo || s->io_hit || s->io_miss
                         || s->run_reads || s->bmp_hit || s->bmp_miss || s->warm_skip
                         || s->warm_touch || s->warm_raster_skip || s->flush_peak;
        if (!any) continue;
        char seg[288];
        // 末尾两个数是判"缓存饿死"的直读：**额度 == 缓存 就是零余量**（room 归零），
        // 此时 ttf_raster_text_px 已早退、绘制里每个字形都重新栅格化。`轮峰` 见
        // ttf_face_stats_t 的说明（判槽位只能用去重后的单轮峰值，不能用段N读M块那个累计和）。
        snprintf(
            seg, sizeof(seg),
            "%s%s 打包%u(备忘%u) 块%u中%u 段%u读%u块白读%u 位图%u中%u 预热跳%u碰%u弃%u 轮峰%u 缓存%uK/额度%uK",
            off ? " | " : "", kRoleName[r], (unsigned)s->pack_read, (unsigned)s->pack_memo,
            (unsigned)(s->io_hit + s->io_miss), (unsigned)s->io_hit, (unsigned)s->run_reads,
            (unsigned)s->run_blocks, (unsigned)s->run_waste,
            (unsigned)(s->bmp_hit + s->bmp_miss), (unsigned)s->bmp_hit,
            (unsigned)s->warm_skip, (unsigned)s->warm_touch, (unsigned)s->warm_raster_skip,
            (unsigned)s->flush_peak,
            (unsigned)(s_faces[r].f_cache_bytes / 1024),
            (unsigned)(cache_effective_limit_of(&s_faces[r]) / 1024)
        );
        const size_t l = strlen(seg);
        // 清零放在"装不下就 break"**之前**：装不下只是这一行日志里少一段，计数不能留着
        // 不带走的 —— 留着下一次会当成新发生的再报一遍，那段数就再也分不清是哪一段的了。
        memset(s, 0, sizeof(*s));
        if (off + l + 1 > n) break;
        memcpy(out + off, seg, l + 1);
        off += l;
    }
    const size_t freeb = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    const size_t room = freeb > TTF_CACHE_RESERVE ? freeb - TTF_CACHE_RESERVE : 0;
    // `room` 是**全局**的（free − 留量线），单看它会读错：room 归零 ≠ 位图缓存没用上。
    // 正文面**还能再装多少**才是判据（= 该面的 额度 − 缓存，与 P2 早退用的是同一个量），
    // 它 < TTF_PAGE_BITMAP_BYTES 时那一趟栅格化预热已经被早退掉（见 warm_raster_skip）。
    const size_t ch = s_faces[TTF_ROLE_CONTENT].f_cache_bytes;
    const size_t clim = cache_effective_limit_of(&s_faces[TTF_ROLE_CONTENT]);
    char tail[96];
    snprintf(tail, sizeof(tail), "%sPSRAM free %uK room %uK 正文还装%uK", off ? " | " : "",
             (unsigned)(freeb / 1024), (unsigned)(room / 1024),
             (unsigned)((clim > ch ? clim - ch : 0) / 1024));
    const size_t tl = strlen(tail);
    if (off + tl + 1 <= n) memcpy(out + off, tail, tl + 1);
}

void ttf_font_cache_clear(void) {
    cache_reset();
    packed_root = -1;
    packed_weight = -1;
    io_reset();
}

static void abandon_font_source(void) {
    if (font_fd >= 0) {
        close(font_fd);
        font_fd = -1;
    }
    font_mem = NULL;
    font_mem_len = 0;
}

// 卸载"当前字面"。名字带 face_ 是因为它作用于 s_cur —— 公开的 ttf_font_unload()
// 是它的内容面钉壳。内部一律调这个，绝不许调公开壳(见 ttf_set_role 的说明)。
static void face_unload(void) {
    font_ready = false;
    cache_reset();
    io_unmap();
    io_reset();
    abandon_font_source();
    font_file_pos = UINT32_MAX;
    heap_caps_free(file_loca);
    file_loca = NULL;
    heap_caps_free(font_data);
    font_data = NULL;
    reset_variation();
    packed_root = -1;
    packed_weight = -1;
    num_glyphs = 0;
    file_glyf_off = 0;
    file_glyf_len = 0;
    file_gvar_off = 0;
    file_gvar_len = 0;
    file_loca_len = 0;
    loca_long = false;
    raw_ascent_units = 0;
    memset(&font_info, 0, sizeof(font_info));
    memset(size_metrics, 0, sizeof(size_metrics));
}

static esp_err_t load_opened_font(void) {
    font_file_pos = UINT32_MAX;
    // 内建字体常驻 flash(font_mem != NULL)，file_read_at 走内存直读，根本用不上
    // io_data。无条件调用会白分配 128+32KB PSRAM —— 单面就 160KB，两面 320KB。
    if (font_mem == NULL) io_ensure();
    io_reset();
    io_unmap();

    uint8_t header[12 + 32 * 16];
    if (!file_read_at(0, header, sizeof(header))) {
        abandon_font_source();
        return ESP_FAIL;
    }

    font_data = build_working_font(header, sizeof(header));
    if (font_data == NULL) {
        abandon_font_source();
        ESP_LOGE(TAG, "build working font failed");
        return ESP_ERR_NO_MEM;
    }

    // 整表映射只对**SD 上的**字体有意义（把 glyf 搬进 PSRAM 是为了免掉一页几百次的
    // 小读 + lseek）。内建字体在 flash rodata 里（font_mem != NULL），io_read_raw 走的
    // 就是内存直读，映射一份进 PSRAM 一点好处都没有 —— 却在开机那一刻**随机**吃掉最多
    // 1927KB（内建的 glyf 表），而且它能不能进来只看当时 largest 够不够 4487KB
    // （TTF_MAP_RESERVE）。这是 PSRAM 里最大的一笔可变占用，也是"这次开机宽裕、下次
    // 开机就紧"的来源，白给。跳过。（下面那处 4KB 的小表同理。）
    if (font_mem == NULL) {
        try_map_table(
            file_glyf_off, file_glyf_len, &glyf_ram, &glyf_ram_off, &glyf_ram_len, "glyf"
        );
    }

    load_variation(header, sizeof(header));
    if (font_mem == NULL && gvar_ready && file_gvar_len > 0) {
        try_map_table(
            file_gvar_off, file_gvar_len,
            &gvar_ram, &gvar_ram_off, &gvar_ram_len, "gvar"
        );
    }

    if (!stbtt_InitFont(&font_info, font_data, 0)) {
        io_unmap();
        heap_caps_free(file_loca);
        heap_caps_free(font_data);
        file_loca = NULL;
        font_data = NULL;
        abandon_font_source();
        ESP_LOGE(TAG, "stbtt_InitFont failed");
        return ESP_ERR_INVALID_RESPONSE;
    }

    int raw_ascent = 0, raw_descent = 0, raw_gap = 0;
    stbtt_GetFontVMetrics(&font_info, &raw_ascent, &raw_descent, &raw_gap);
    raw_ascent_units = raw_ascent;
    const int pixel_heights[2] = { TTF_PIXEL_SMALL, TTF_PIXEL_LARGE };
    for (int i = 0; i < 2; i++) {
        size_metrics[i].scale = font_scale_px(pixel_heights[i]);
        size_metrics[i].ascent = (int)lroundf(raw_ascent * size_metrics[i].scale);
    }

    font_ready = true;
    ESP_LOGI(
        TAG, "%s %s, %d glyphs, glyf %u KB%s, gvar %u KB%s, working %u KB",
        glyf_ram != NULL ? "mapped" : "stream",
        font_path, num_glyphs,
        (unsigned)(file_glyf_len / 1024), glyf_ram != NULL ? " ram" : "",
        (unsigned)(file_gvar_len / 1024), gvar_ram != NULL ? " ram" : "",
        (unsigned)((file_loca_len + TTF_GLYF_ARENA) / 1024)
    );
    return ESP_OK;
}

// 把内置字体装进"当前字面"。⚠ 内部一律调 face_unload()，不许调公开的
// ttf_font_unload() —— 惰性加载 UI 面时 s_cur 正是 UI，调公开壳会重新钉回内容面
// 并把用户字体卸掉，静默且破坏性。
static esp_err_t face_open_builtin(void) {
    face_unload();
    if (!ensure_work()) return ESP_ERR_NO_MEM;

    font_mem = builtin_ttf_start;
    font_mem_len = (uint32_t)(builtin_ttf_end - builtin_ttf_start);
    font_fd = -1;
    strlcpy(font_path, TTF_FONT_BUILTIN, sizeof(font_path));
    ESP_LOGI(TAG, "using builtin (%u KB)", (unsigned)(font_mem_len / 1024));
    return load_opened_font();
}

esp_err_t ttf_font_suspend_sd(bool suspend) {
    sd_suspended = suspend;
    if (!suspend) return ESP_OK;
    // 次字面也是 SD 上的文件，挂起期间它的 fd 同样不能再用 —— 必须一起卸。
    // 恢复时由调用方(阅读器)按 st 里记的路径重开，这里不替它记。
    ttf_font_close_alt();
    if (!s_faces[TTF_ROLE_CONTENT].f_font_ready || !content_is_builtin()) {
        return ttf_font_open_builtin();
    }
    return ESP_OK;
}

static esp_err_t face_open(const char* path) {
    if (ttf_font_path_is_builtin(path)) {
        return face_open_builtin();
    }
    if (sd_suspended) return ESP_ERR_INVALID_STATE;

    char prev[TTF_FONT_PATH_MAX];
    strlcpy(prev, font_path, sizeof(prev));
    bool had = font_ready;
    bool prev_file = had && !ttf_font_path_is_builtin(prev);

    face_unload();
    if (!ensure_work()) return ESP_ERR_NO_MEM;

    font_fd = open_font_file(path);
    if (font_fd < 0) {
        ESP_LOGW(TAG, "TTF missing: %s", path);
        if (prev_file) {
            font_fd = try_open_path(prev);
            if (font_fd >= 0) return load_opened_font();
        }
        return face_open_builtin() == ESP_OK ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    }

    esp_err_t err = load_opened_font();
    if (err != ESP_OK) {
        if (prev_file && strcmp(prev, path) != 0) {
            face_unload();
            font_fd = try_open_path(prev);
            if (font_fd >= 0 && load_opened_font() == ESP_OK) return err;
        }
        return face_open_builtin() == ESP_OK ? err : ESP_FAIL;
    }
    return ESP_OK;
}

/* ---- 公开接口：一律钉在内容面上 ---- */
void ttf_font_unload(void) {
    ttf_face_t* prev = face_enter(TTF_ROLE_CONTENT);
    face_unload();
    face_leave(prev);
}

esp_err_t ttf_font_open_builtin(void) {
    ttf_face_t* prev = face_enter(TTF_ROLE_CONTENT);
    esp_err_t e = face_open_builtin();
    face_leave(prev);
    return e;
}

esp_err_t ttf_font_open(const char* path) {
    ttf_face_t* prev = face_enter(TTF_ROLE_CONTENT);
    esp_err_t e = face_open(path);
    face_leave(prev);
    return e;
}

/* ---- 书内家族面：CSS 的第二 / 第三个家族 --------------------------------
 * 两槽行为完全一样（同尺寸、同字形缓存上限、同失败语义），只有角色号不同，所以走同一
 * 对辅助函数 —— 免得哪天只给其中一个补了修补，另一个还带着老毛病。
 */
static esp_err_t book_alt_open(int role, const char* path) {
    ttf_face_t* prev = face_enter(role);
    esp_err_t e = face_open(path);
    face_leave(prev);
    if (e != ESP_OK) {
        // 家族面打不开**不是错误**：正文照旧，只是那些注文/引文落回主字面渲染。
        // 这里退到"没开"，ttf_set_role(该面) 随后会自动走内容面。
        face_enter(role);
        face_unload();
        // 同 book_alt_close：面都卸了，它可能已经要到的块缓存不该留在这儿 ——
        // 否则"打开失败"这一支会把一次分配漏到下一次开书(close 那份责任)。
        ttf_face_purge_io(s_cur);
        face_leave(prev);
    }
    return e;
}

static void book_alt_close(int role) {
    ttf_face_t* prev = face_enter(role);
    face_unload();
    // 家族面是配角：它那 96KB(24×4KB + 4×4KB) + 两张 2KB 表的块缓存在 face_unload 里
    // **不会被放掉** —— io_unmap 只管 glyf/gvar 的整表映射，io_reset 只清槽位表。
    // 于是关掉之后这块一直攥着，两面合计 ~200KB，而"开书那一刻的 room"本来就只够
    // 让位图缓存长到一半(实测晋书 142K vs 同机另一本 473K)。这是一整场开机"位图命中
    // 偏低"的起点，也是"重启才快"的正面解 —— 换书/退出阅读必须走这里，重开时
    // io_ensure() 会重新要一块。
    //
    // 复用 ttf_face_purge_io 而不是就地 free：它连槽位表一起作废(否则重开时会拿旧块号
    // 当"已经在缓存里")，也顺手清掉分配失败留下的退避窗口(下一次 io_ensure 立刻能试)。
    // ⚠ 不要挪进 face_unload()：内容面也走那条路(face_open 一进来就 face_unload)，
    // 那会让换字体时 640KB 反复 alloc/free；build_working_font 的 OOM 兜底也刚好在
    // 同一条路上，那边要的是"先腾出来、后面自己再长回去"。
    ttf_face_purge_io(s_cur);
    face_leave(prev);
}

bool ttf_font_alt_ready(void) { return s_faces[TTF_ROLE_CONTENT_ALT].f_font_ready; }
bool ttf_font_alt2_ready(void) { return s_faces[TTF_ROLE_CONTENT_ALT2].f_font_ready; }

esp_err_t ttf_font_open_alt(const char* path) {
    return book_alt_open(TTF_ROLE_CONTENT_ALT, path);
}
esp_err_t ttf_font_open_alt2(const char* path) {
    return book_alt_open(TTF_ROLE_CONTENT_ALT2, path);
}

void ttf_font_close_alt(void) { book_alt_close(TTF_ROLE_CONTENT_ALT); }
void ttf_font_close_alt2(void) { book_alt_close(TTF_ROLE_CONTENT_ALT2); }

esp_err_t ttf_font_open_logo(void) {
    ttf_face_t* prev = face_enter(TTF_ROLE_CONTENT_ALT);
    face_unload();
    esp_err_t e = ensure_work() ? ESP_OK : ESP_ERR_NO_MEM;
    if (e == ESP_OK) {
        // 与 face_open_builtin() 同一条路，只是数据源换成了另一个内嵌 blob：
        // font_mem != NULL 就是"内存直读"，不分配 io_data 那 160KB。
        font_mem = yanos_logo_ttf_start;
        font_mem_len = (uint32_t)(yanos_logo_ttf_end - yanos_logo_ttf_start);
        font_fd = -1;
        // 名字沿用内建：ttf_font_path_is_builtin() 是查名字的，而"内存直读"那一整套
        // 分支挂在 font_mem 上 —— 两边都按内建走，行为与内建面完全一致。
        strlcpy(font_path, TTF_FONT_BUILTIN, sizeof(font_path));
        e = load_opened_font();
    }
    face_leave(prev);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "logo font open failed (%d)", (int)e);
        face_enter(TTF_ROLE_CONTENT_ALT);
        face_unload();
        face_leave(prev);
    }
    return e;
}

int ttf_get_role(void) {
    // s_cur 恒指向 s_faces 里的一员（只由 face_enter/ttf_set_role 写），所以下标就是答案。
    return (int)(s_cur - s_faces);
}

void ttf_set_role(int role) {
    // 书内家族面：没打开就静默用内容面（绘制层不必到处判"本书有没有这个家族"）。
    if (role == TTF_ROLE_CONTENT_ALT || role == TTF_ROLE_CONTENT_ALT2) {
        s_cur = s_faces[role].f_font_ready ? &s_faces[role] : &s_faces[TTF_ROLE_CONTENT];
        return;
    }
    // 内容面就是内置字体时，UI 面与它完全等价 —— 直接用内容面，不必另开一份。
    // 默认设置(不选外置字体)永远走这条路，于是默认路径内存零增长。
    if (role != TTF_ROLE_UI || content_is_builtin()) {
        s_cur = &s_faces[TTF_ROLE_CONTENT];
        return;
    }
    if (!s_faces[TTF_ROLE_UI].f_font_ready) {
        ttf_face_t* prev = face_enter(TTF_ROLE_UI);
        if (face_open_builtin() != ESP_OK) {
            // 降级：UI 面没装起来就继续用内容面。否则 s_cur 停在一个 font_ready=false
            // 的面上，每个 draw 都在 if(!font_ready) 处提前返回 —— 菜单、状态栏、
            // 标题会**集体消失**。这是本次改动最坏的失败模式，这行是它唯一的防线。
            ESP_LOGE(TAG, "UI face load failed; chrome falls back to content face");
            s_cur = &s_faces[TTF_ROLE_CONTENT];
            (void)prev;
            return;
        }
        (void)prev;
    }
    s_cur = &s_faces[TTF_ROLE_UI];
}

esp_err_t ttf_font_init(void) {
    // BSS 清零后 font_fd/-1、font_file_pos/UINT32_MAX、packed_root/-1、current_weight
    // 这几项"零不是对的值"，每个面都要先过一遍默认值。用 cache_limit 非 0 当"已初始化"
    // 的探针 —— begin() 会被调两次(UI 面与内容面各一次)，第二次不能把字体状态抹掉。
    if (s_faces[TTF_ROLE_CONTENT].f_cache_limit == 0) {
        for (int role = 0; role < TTF_ROLE_COUNT; role++) face_defaults(&s_faces[role], role);
    }

    // 1) 内容面先装。try_map_table 以 heap_caps_get_largest_free_block() 为门槛：
    //    先装内建 UI 面会吃掉这部分余量，可能让 SD 字体从"整体映射进 PSRAM"静默
    //    退化成"每个冷字形读一次 SD"。内建面本来常驻 flash(读它就是 memcpy)，
    //    它拿不到映射几乎不损失什么，反过来损失就大了。
    esp_err_t err = ESP_OK;
    if (!s_faces[TTF_ROLE_CONTENT].f_font_ready) {
        ttf_face_t* prev = face_enter(TTF_ROLE_CONTENT);
        const char* path = font_store_get_path();
        err = ttf_font_path_is_builtin(path) ? face_open_builtin() : face_open(path);
        if (err != ESP_OK && font_ready) err = ESP_OK;   // 与今日同样的容忍度
        face_leave(prev);
    }

    // 2) UI 面：内建。内容面已是内建时二者等价，不必另开一份(默认路径零开销)。
    if (!content_is_builtin() && !s_faces[TTF_ROLE_UI].f_font_ready) {
        ttf_face_t* p2 = face_enter(TTF_ROLE_UI);
        if (face_open_builtin() != ESP_OK) {
            ESP_LOGE(TAG, "UI builtin face failed; chrome will use the content face");
        }
        face_leave(p2);
    }
    return err;
}

void ttf_warm_text_px(int pixel_height, const char* text) {
    if (!font_ready || text == NULL) return;
    warm_text_io(pixel_height, text);
}

void ttf_raster_text_px(int pixel_height, const char* text) {
    if (!font_ready || text == NULL) return;
    // 装不下一页就别栅：K < N 时热身是**净亏**（亏 N−K 次栅格化），推导见
    // TTF_PAGE_BITMAP_BYTES。实测那一档 预热跳2碰372 之后紧接着绘制 位图425中0 ——
    // 刚暖的 372 个字形一个都没留下，而它们花的 CPU 是实打实的（约 3.3ms/字）。
    //
    // 判据用"这一页装得下吗"而不是"现在还有余量吗"：后者在 room==0 时恒真（见
    // cache_headroom）。所以这里量的是 cache_headroom()，它已经是"额度 − 当前占用"。
    //
    // **只砍位图这一半，io 预热（ttf_warm_text_px）一个字都不动**：块缓存不受留量线
    // 影响（128 槽常驻），实测 块722中698 = 96.7% 命中，那 698 次命中正是 io 预热
    // 换来的 —— 砍了它，下面的绘制就退化成上百次带 lseek 的随机 4KB 读。
    if (cache_headroom() < TTF_PAGE_BITMAP_BYTES) {
        STATS.warm_raster_skip++;
        return;
    }
    pixel_height = clamp_px(pixel_height);
    const char* cursor = text;
    // 不做去重表：同一个码点第二次进来就是一次 cache_lookup 命中，比维护去重表便宜，
    // 也不会跟 warm_text_io 抢 warm_cps 那张暂存（阅读器整页预热里两个函数挨着调）。
    while (*cursor != '\0') {
        uint32_t cp = decode_utf8(&cursor);
        if (cp == 0) break;
        // get_glyph 先查缓存、命中直接返回（同码点/像素高/字重/合成加粗），缺字替补也
        // 在里面 —— 这里不另做一套判定，图的就是"与绘制路径逐一对齐"。
        // 返回的指针不持有：缓存是 LRU，指针跨条目失效是设计内的（调用方一律"取到就画"）。
        (void)get_glyph(cp, pixel_height);
    }
}

void ttf_measure_line(int size, const char* text, int* above, int* below) {
    ttf_measure_line_px(size_to_px(size), text, above, below);
}

void ttf_measure_line_px(
    int pixel_height, const char* text, int* above, int* below
) {
    int max_above = 0;
    int max_below = 0;
    if (font_ready && text != NULL) {
        pixel_height = clamp_px(pixel_height);
        warm_text_io(pixel_height, text);
        const char* cursor = text;
        while (*cursor != '\0') {
            uint32_t cp = decode_utf8(&cursor);
            const glyph_entry_t* glyph = get_glyph(cp, pixel_height);
            if (glyph == NULL) continue;
            if (glyph->top > max_above) max_above = glyph->top;
            int under = (int)glyph->height - glyph->top;
            if (under > max_below) max_below = under;
        }
    }
    if (above != NULL) *above = max_above;
    if (below != NULL) *below = max_below;
}

int ttf_text_width_px(int pixel_height, const char* text) {
    if (!font_ready || text == NULL) return 0;
    return measure_width(clamp_px(pixel_height), text);
}

int ttf_char_advance_px(int role, int pixel_height, uint32_t codepoint) {
    // role 必须显式传，不能读 s_cur：FontRenderer::setSize()/setGridPx() 量完 ascent
    // 会把 s_cur 留在 UI 面上不还原，靠 s_cur 就会拿内置面去量用户的拉丁字。
    // face_enter/face_leave 是本文件允许的跨面手法(见铁律 1、2)。
    ttf_face_t* prev = face_enter(role);
    const int width = font_ready ? cp_advance(clamp_px(pixel_height), codepoint) : 0;
    face_leave(prev);
    return width;
}

// 线性抗锯齿的中间灰在这块屏上偏亮。按 gamma 抬覆盖率，半透明边缘更深。
//
// **两张表**：base 给界面外壳（γ = TTF_COVER_GAMMA，就是本功能出现之前的固定观感），
// body 给书的正文（排版设定里的「对比度」）。s_cover_cur 指向当下该用哪张，只有这里和
// ttf_body_ink_begin/end 会写它。
static uint8_t s_cover_base[256];
static uint8_t s_cover_body[256];
static uint8_t* s_cover_cur = s_cover_base;
static float s_body_lut_gamma = -1.0f;  // 当前 body 表是按哪个 gamma 建的（-1 = 还没建）

// 建一张覆盖率表：cover' = 255·(cover/255)^gamma。256 次 powf，只在设置真的变了之后的
// 第一帧发生一次。
static void build_cover(uint8_t* tbl, float gamma) {
    for (int i = 0; i < 256; i++) {
        float t = (float)i / 255.f;
        int n = (int)(255.f * powf(t, gamma) + 0.5f);
        if (n < 0) n = 0;
        if (n > 255) n = 255;
        tbl[i] = (uint8_t)n;
    }
}

/* ---- 绘制 / Draw ---- */
static void ttf_cover_lut_init(void) {
    static bool ready;
    if (ready) return;
    build_cover(s_cover_base, TTF_COVER_GAMMA);
    build_cover(s_cover_body, TTF_COVER_GAMMA);
    s_body_lut_gamma = TTF_COVER_GAMMA;
    ready = true;
}

static uint8_t mix_ink(uint8_t alpha, uint8_t fg, uint8_t bg) {
    return (uint8_t)(bg + s_cover_cur[alpha] * ((int)fg - (int)bg) / 255);
}

void ttf_body_ink_begin(int dw, float gamma) {
    ttf_cover_lut_init();
    if (gamma < 0.15f) gamma = 0.15f;
    if (gamma > 4.0f) gamma = 4.0f;

    // 只有 gamma 一个旋钮要进表（字重走的是字形本身：合成加粗在 rasterize_glyph 里
    // 把位图膨胀一圈，真变体在下面改字面字重，都不碰覆盖率曲线）。
    if (gamma != s_body_lut_gamma) {
        build_cover(s_cover_body, gamma);
        s_body_lut_gamma = gamma;
    }

    s_body_on = true;
    s_body_dw = dw;
    s_body_gamma = gamma;
    s_cover_cur = s_cover_body;

    // 真实变体：只对**自己有 wght 轴**的面做（有轴就用字体里的 Medium/Bold，比合成准得多；
    // 没有轴的面交给合成，见 synth_dw_now）。形状是相对各面自己的默认值偏移，与 KOReader
    // 的 font_base_weight 同口径：+100 = Medium(500)、+300 = Bold(700)。
    // 记下原值，end 时还原 —— 外壳与正文常常同一个面，不还原就把菜单也一起改了。
    for (int role = 0; role < TTF_ROLE_COUNT; role++) {
        s_body_saved_ok[role] = false;
        if (dw == 0) continue;
        ttf_face_t* prev = face_enter(role);
        if (font_ready && gvar_ready) {
            s_body_saved_w[role] = current_weight;
            s_body_saved_ok[role] = true;
            current_weight = clamp_weight(wght_def + dw);
        }
        face_leave(prev);
    }
}

void ttf_body_ink_end(void) {
    s_body_on = false;
    s_cover_cur = s_cover_base;
    for (int role = 0; role < TTF_ROLE_COUNT; role++) {
        if (!s_body_saved_ok[role]) continue;
        s_body_saved_ok[role] = false;
        ttf_face_t* prev = face_enter(role);
        current_weight = s_body_saved_w[role];
        face_leave(prev);
    }
}

void ttf_draw_text(
    uint8_t* framebuffer, int x, int y, int size, const char* text,
    int align, uint8_t fg, uint8_t bg
) {
    ttf_draw_text_px(framebuffer, x, y, size_to_px(size), text, align, fg, bg);
}

void ttf_draw_text_px(
    uint8_t* framebuffer, int x, int y, int pixel_height, const char* text,
    int align, uint8_t fg, uint8_t bg
) {
    if (!font_ready || framebuffer == NULL || text == NULL) return;
    ttf_cover_lut_init();
    pixel_height = clamp_px(pixel_height);
    warm_text_io(pixel_height, text);
    fb_fast_sync();   // 本次绘制内旋转/尺寸只查一次

    int cursor_x = x;
    if (align & TTF_ALIGN_CENTER) {
        cursor_x = x - measure_width(pixel_height, text) / 2;
    } else if (align & TTF_ALIGN_RIGHT) {
        cursor_x = x - measure_width(pixel_height, text);
    }

    const char* cursor = text;
    while (*cursor != '\0') {
        uint32_t cp = decode_utf8(&cursor);
        const glyph_entry_t* glyph = get_glyph(cp, pixel_height);
        if (glyph == NULL) continue;

        if (glyph->bitmap != NULL) {
            for (int gy = 0; gy < glyph->height; gy++) {
                int yy = y - glyph->top + gy;
                for (int gx = 0; gx < glyph->width; gx++) {
                    uint8_t alpha = glyph->bitmap[gy * glyph->width + gx];
                    if (alpha == 0) continue;
                    fb_fast_pixel(
                        framebuffer, cursor_x + glyph->left + gx, yy,
                        (uint8_t)(mix_ink(alpha, fg, bg) << 4)
                    );
                }
            }
        }
        cursor_x += glyph->advance_x;
    }
}

void ttf_draw_text_px_bw(
    uint8_t* framebuffer, int x, int y, int pixel_height, const char* text,
    int align, uint8_t fg, uint8_t bg
) {
    if (!font_ready || framebuffer == NULL || text == NULL) return;
    pixel_height = clamp_px(pixel_height);
    warm_text_io(pixel_height, text);
    fb_fast_sync();   // 本次绘制内旋转/尺寸只查一次

    int cursor_x = x;
    if (align & TTF_ALIGN_CENTER) {
        cursor_x = x - measure_width(pixel_height, text) / 2;
    } else if (align & TTF_ALIGN_RIGHT) {
        cursor_x = x - measure_width(pixel_height, text);
    }

    const char* cursor = text;
    while (*cursor != '\0') {
        uint32_t cp = decode_utf8(&cursor);
        const glyph_entry_t* glyph = get_glyph(cp, pixel_height);
        if (glyph == NULL) continue;

        if (glyph->bitmap != NULL) {
            for (int gy = 0; gy < glyph->height; gy++) {
                int yy = y - glyph->top + gy;
                for (int gx = 0; gx < glyph->width; gx++) {
                    uint8_t alpha = glyph->bitmap[gy * glyph->width + gx];
                    uint8_t ink = alpha >= 128 ? fg : bg;
                    if (ink == bg) continue;
                    fb_fast_pixel(framebuffer, cursor_x + glyph->left + gx, yy, (uint8_t)(ink << 4));
                }
            }
        }
        cursor_x += glyph->advance_x;
    }
}

