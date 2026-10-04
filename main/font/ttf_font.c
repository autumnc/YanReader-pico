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

#include "settings.h"
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
#define TTF_CACHE_RESERVE (1536 * 1024)
// UI 面的字形缓存上限。内建面常驻 flash(font_mem != NULL)，未命中 = memcpy + 光栅，
// 没有 SD I/O —— 淘汰只花 CPU 不花毫秒。一个菜单/设置/对话框屏约需 100-200 个
// 不同 CJK@38-46px(≈2.1KB/位图) ≈ 250-400KB，512KB 留约 2 倍余量。
// 实测某屏反复重光栅化(翻页/进界面偶发卡顿)就把这里提到 768KB，PSRAM 有的是。
#define TTF_UI_CACHE_LIMIT (512 * 1024)
// 次字面(书内第二个家族，如祖堂集的仿宋)的字形缓存上限。它只画注文/引文 —— 一页里
// 的比例小、字面重复度高，1MB 足够罩住一章；真正的额度仍由 cache_reserve() 按空闲量
// 现算，这里只是天花板。
#define TTF_ALT_CACHE_LIMIT (1024 * 1024)
// 次字面的 SD 块缓存与合并读窗口。它是**配角**：正文走内容面，那才是每页的大头，
// 所以这里给 48 槽(192KB)而不是内容面的 128 槽(512KB) —— 主面的字形缓存才是决定
// 翻页速度的东西，不能让它被配角挤掉。
#define TTF_ALT_IO_SLOTS 48
#define TTF_ALT_IO_RUN_MAX 8
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
// 槽位开到 128(512KB PSRAM)先保证整页轮毂一轮读进来后不会立刻被挤掉。
#define TTF_IO_BLOCK 4096
#define TTF_IO_SLOTS 128
// 预取时合并的连续块上限。/ Max contiguous blocks merged in one prefetch read.
#define TTF_IO_RUN_MAX 32
// 相邻被碰块之间最多隔几块也并进同一次读(中间几块一起读掉，用白读换掉一次寻道)。
#define TTF_IO_GAP_BLOCKS 2
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
    f->f_cache_limit = (role == TTF_ROLE_UI)     ? TTF_UI_CACHE_LIMIT
                       : (role == TTF_ROLE_CONTENT_ALT) ? TTF_ALT_CACHE_LIMIT
                                                        : TTF_CACHE_LIMIT;
    f->f_io_slots = (role == TTF_ROLE_CONTENT_ALT) ? TTF_ALT_IO_SLOTS : TTF_IO_SLOTS;
    f->f_io_run_max = (role == TTF_ROLE_CONTENT_ALT) ? TTF_ALT_IO_RUN_MAX : TTF_IO_RUN_MAX;
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

// 本面实际开几个 IO 槽 / 合并窗口多大（面未初始化时退回编译期上限，保证循环不越界）。
static int io_slot_count(void) { return io_slots > 0 ? io_slots : TTF_IO_SLOTS; }
static int io_run_cap(void) { return io_run_max > 0 ? io_run_max : TTF_IO_RUN_MAX; }

/* ---- 与字面无关的全局：不进上面的结构 ---- */
extern const uint8_t builtin_ttf_start[] asm("_binary_builtin_ttf_start");
extern const uint8_t builtin_ttf_end[] asm("_binary_builtin_ttf_end");
// Yan Reader 标志字体：Noto Serif CJK SC 子集，只有标志用到的 27 个字形（5.4KB）。
// 只给开机动画用，见 ttf_font_open_logo()。
extern const uint8_t yanos_logo_ttf_start[] asm("_binary_yanos_logo_ttf_start");
extern const uint8_t yanos_logo_ttf_end[] asm("_binary_yanos_logo_ttf_end");
static bool sd_suspended; // 字体传输期间仅允许内置字体。/ Only built-in fonts while transferring font files.
static ttf_work_t* work;
static bool bench_on;
static ttf_bench_stats_t bench;
static int64_t bench_start_us;

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
    int64_t t0 = bench_on ? esp_timer_get_time() : 0;
    bool ok = false;
    do {
        if (font_file_pos != offset) {
            int64_t ts = bench_on ? esp_timer_get_time() : 0;
            if (lseek(font_fd, (off_t)offset, SEEK_SET) < 0) {
                font_file_pos = UINT32_MAX;
                break;
            }
            if (bench_on) bench.seek_us += esp_timer_get_time() - ts;
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
            if (bench_on) {
                bench.read_calls++;
                bench.read_bytes += (uint32_t)got;
            }
            done += (size_t)got;
            font_file_pos = offset + (uint32_t)done;
        }
        ok = done == n;
    } while (0);
    if (bench_on) bench.read_us += esp_timer_get_time() - t0;
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
        io_data = heap_caps_malloc(
            slots * TTF_IO_BLOCK,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
        );
        if (io_data == NULL) {
            // 不致命(fallback 是逐次直读)，但会静默失去全部预取收益 —— 记一笔，
            // 免得对着"整页预取: 无块可读"猜半天。多半是 PSRAM 碎片化下
            // 要不到这么大一块连续内存。
            ESP_LOGW(
                TAG, "块缓存分配失败(%dKB 连续 PSRAM)，本轮走直读",
                (int)(slots * TTF_IO_BLOCK / 1024)
            );
            return false;
        }
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
    if (io_find(base) >= 0) return true;
    if (io_data == NULL && !io_ensure()) return false;
    int slot = io_victim();
    uint8_t* dst = io_data + (size_t)slot * TTF_IO_BLOCK;
    int64_t t0 = bench_on ? esp_timer_get_time() : 0;
    // 顺序补读(上一块的读正好停在块尾)不必再 seek 一次 —— io_read_raw 一直有这个
    // 快路径，这里漏了。逐块读相邻扇区的场景(预取退化成逐块、大字形跨两块)就省下
    // 一次 FATFS 簇链查找。
    if (font_file_pos != base) {
        int64_t ts = bench_on ? esp_timer_get_time() : 0;
        if (lseek(font_fd, (off_t)base, SEEK_SET) < 0) {
            font_file_pos = UINT32_MAX;
            if (bench_on) bench.read_us += esp_timer_get_time() - t0;
            return false;
        }
        if (bench_on) bench.seek_us += esp_timer_get_time() - ts;
    }
    ssize_t got = read(font_fd, dst, TTF_IO_BLOCK);
    if (bench_on) {
        bench.read_us += esp_timer_get_time() - t0;
        if (got > 0) {
            bench.read_calls++;
            bench.read_bytes += (uint32_t)got;
        }
    }
    if (got <= 0) {
        font_file_pos = UINT32_MAX;
        return false;
    }
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
    if (io_run_buf == NULL
        || !io_read_raw(base, io_run_buf, (size_t)blocks * TTF_IO_BLOCK)) {
        bool ok = true;
        for (int i = 0; i < blocks; i++) {
            if (!io_load_block(base + (uint32_t)i * TTF_IO_BLOCK)) ok = false;
        }
        return ok;
    }
    for (int i = 0; i < blocks; i++) {
        uint32_t b = base + (uint32_t)i * TTF_IO_BLOCK;
        if (io_find(b) >= 0) continue;
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
        if (bench_on) {
            bench.io_runs++;
            if (bench.io_span_min == 0 || touch_blocks[i] < bench.io_span_min) {
                bench.io_span_min = touch_blocks[i];
            }
            if (touch_blocks[j - 1] > bench.io_span_max) {
                bench.io_span_max = touch_blocks[j - 1];
            }
        }
        i = j;
    }
    if (bench_on) bench.io_blocks += (uint32_t)w;
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
        if (packed + len > TTF_GLYF_ARENA) {
            ESP_LOGE(TAG, "glyf arena overflow gid=%d len=%u", gid, (unsigned)len);
            return false;
        }
        uint8_t* blob = arena + packed;
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

static uint32_t align4(uint32_t v) {
    return (v + 3u) & ~3u;
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

    uint32_t cursor = 12 + 7 * 16;
    uint32_t dst_off[7];
    uint32_t total = cursor;
    for (int i = 0; i < 7; i++) {
        dst_off[i] = total;
        total = align4(total + src[i].length);
    }

    uint8_t* data = heap_caps_calloc(1, total, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
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

    uint8_t* head = data + dst_off[2];
    loca_long = be16(head + 50) != 0;
    uint8_t* maxp = data + dst_off[6];
    num_glyphs = be16(maxp + 4);
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
    if (n > 0) {
        ESP_LOGI(TAG, "glyph cache dropped %u entries", (unsigned)n);
    }
}

static void cache_evict_one(void) {
    glyph_entry_t* victim = lru_tail;
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
// 规则：空闲量高于 TTF_CACHE_RESERVE 时，允许长到"刚好把空闲压到留量线"；
// 已经低于留量线就停在当前占用不变 —— **不缩**，缩了下一帧还得重新光栅化。
static size_t cache_effective_limit(void) {
    size_t free_bytes = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    size_t room = free_bytes > TTF_CACHE_RESERVE ? free_bytes - TTF_CACHE_RESERVE : 0;
    size_t room_ceiling = cache_bytes + room;
    return room_ceiling < cache_limit ? room_ceiling : cache_limit;
}

static void cache_reserve(size_t extra) {
    const size_t limit = cache_effective_limit();
    while (lru_tail != NULL && cache_bytes + extra > limit) {
        cache_evict_one();
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
            && entry->weight == (uint16_t)current_weight) {
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
//   当前是正文面(内容/次字面) → 内置面：它覆盖最全(7717 字，ASCII 全在)。
//   当前是内置面(界面外壳)   → 内容面：用户装的字体 CJK 覆盖通常比内置子集更宽。
//   当前是次字面(书内 CSS 第二个家族) → 先内容面：同一本书、同一套观感。
// 返回角色；-1 = 谁都没有(调用方照旧画 .notdef)。
static int fallback_role_for(uint32_t cp) {
    static const int8_t kOrder[TTF_ROLE_COUNT][TTF_ROLE_COUNT] = {
        /* CONTENT     */ {TTF_ROLE_UI, -1, -1},
        /* UI          */ {TTF_ROLE_CONTENT, -1, -1},
        /* CONTENT_ALT */ {TTF_ROLE_CONTENT, TTF_ROLE_UI, -1},
    };
    const int cur = (s_cur == &s_faces[TTF_ROLE_UI])            ? TTF_ROLE_UI
                    : (s_cur == &s_faces[TTF_ROLE_CONTENT_ALT]) ? TTF_ROLE_CONTENT_ALT
                                                                : TTF_ROLE_CONTENT;
    for (int i = 0; i < TTF_ROLE_COUNT; i++) {
        const int role = kOrder[cur][i];
        if (role < 0) break;
        if (role == cur) continue;   // 同一面不必再试：它正是画不出来的那一个
        if (face_has_cp(role, cp)) return role;
    }
    return -1;
}

static glyph_entry_t* rasterize_glyph(uint32_t codepoint, int pixel_height) {
    int gid = stbtt_FindGlyphIndex(&font_info, (int)codepoint);
    if (!pack_glyph_tree(gid)) return NULL;

    float scale = font_scale_px(pixel_height);
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    stbtt_GetGlyphBitmapBox(&font_info, gid, scale, scale, &x0, &y0, &x1, &y1);

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
        stbtt_MakeGlyphBitmap(
            &font_info, entry->bitmap, width, height, width, scale, scale, gid
        );
    }

    int advance = 0;
    int lsb = 0;
    stbtt_GetGlyphHMetrics(&font_info, gid, &advance, &lsb);

    entry->codepoint = codepoint;
    entry->size = (uint8_t)pixel_height;
    entry->weight = (uint16_t)current_weight;
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
    if (bench_on) bench.glyphs++;
    glyph_entry_t* entry = cache_lookup(codepoint, pixel_height);
    if (entry != NULL) {
        if (bench_on) bench.hits++;
        return entry;
    }
    if (bench_on) bench.misses++;
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
    int64_t t0 = bench_on ? esp_timer_get_time() : 0;
    entry = rasterize_glyph(codepoint, pixel_height);
    if (bench_on) bench.raster_us += esp_timer_get_time() - t0;
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
        if (cache_lookup(cp, pixel_height) != NULL) continue;
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

static int measure_width(int pixel_height, const char* text) {
    // 字宽只依赖已驻留的 cmap/hmtx；分页不读取轮廓、不生成整章位图。
    // Resident cmap/hmtx suffice for advances; pagination never reads outlines or rasterizes a chapter.
    const float scale = font_scale_px(pixel_height);
    int width = 0;
    const char* cursor = text;
    while (*cursor != '\0') {
        uint32_t cp = decode_utf8(&cursor);
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
                width += (int)lroundf(advance * fscale);
                continue;
            }
        }
        stbtt_GetGlyphHMetrics(&font_info, gid, &advance, &lsb);
        width += (int)lroundf(advance * scale);
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

void ttf_font_cache_clear(void) {
    cache_reset();
    packed_root = -1;
    packed_weight = -1;
    io_reset();
}

void ttf_bench_begin(void) {
    memset(&bench, 0, sizeof(bench));
    bench_on = true;
    bench_start_us = esp_timer_get_time();
}

void ttf_bench_end(ttf_bench_stats_t* out) {
    bench_on = false;
    bench.total_us = esp_timer_get_time() - bench_start_us;
    // 缓存占用与当前额度：一眼看出"是不是顶到天花板了"。两个都顶到上限还大量未命中，
    // 说明内存已经给足、瓶颈在别处；只顶到 room 额度则说明是 PSRAM 让不出来。
    bench.cache_kb = (uint32_t)(cache_bytes / 1024);
    bench.cache_cap_kb = (uint32_t)(cache_effective_limit() / 1024);
    if (out != NULL) *out = bench;
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

    try_map_table(
        file_glyf_off, file_glyf_len, &glyf_ram, &glyf_ram_off, &glyf_ram_len, "glyf"
    );

    load_variation(header, sizeof(header));
    if (gvar_ready && file_gvar_len > 0) {
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

/* ---- 次字面：书内 CSS 的第二个家族 ---- */
bool ttf_font_alt_ready(void) { return s_faces[TTF_ROLE_CONTENT_ALT].f_font_ready; }

esp_err_t ttf_font_open_alt(const char* path) {
    ttf_face_t* prev = face_enter(TTF_ROLE_CONTENT_ALT);
    esp_err_t e = face_open(path);
    face_leave(prev);
    if (e != ESP_OK) {
        // 次字面打不开**不是错误**：正文照旧，只是那些注文/引文落回主字面渲染。
        // 这里退到"没开"，ttf_set_role(ALT) 随后会自动走内容面。
        face_enter(TTF_ROLE_CONTENT_ALT);
        face_unload();
        face_leave(prev);
    }
    return e;
}

void ttf_font_close_alt(void) {
    ttf_face_t* prev = face_enter(TTF_ROLE_CONTENT_ALT);
    face_unload();
    face_leave(prev);
}

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
    if (s_cur == &s_faces[TTF_ROLE_UI]) return TTF_ROLE_UI;
    if (s_cur == &s_faces[TTF_ROLE_CONTENT_ALT]) return TTF_ROLE_CONTENT_ALT;
    return TTF_ROLE_CONTENT;
}

void ttf_set_role(int role) {
    // 次字面：没打开就静默用内容面（绘制层不必到处判"本书有没有次字体"）。
    if (role == TTF_ROLE_CONTENT_ALT) {
        s_cur = s_faces[TTF_ROLE_CONTENT_ALT].f_font_ready ? &s_faces[TTF_ROLE_CONTENT_ALT]
                                                           : &s_faces[TTF_ROLE_CONTENT];
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
    // 这几项"零不是对的值"，两面都要先过一遍默认值。用 cache_limit 非 0 当"已初始化"
    // 的探针 —— begin() 会被调两次(UI 面与内容面各一次)，第二次不能把字体状态抹掉。
    if (s_faces[TTF_ROLE_CONTENT].f_cache_limit == 0) {
        face_defaults(&s_faces[TTF_ROLE_CONTENT], TTF_ROLE_CONTENT);
        face_defaults(&s_faces[TTF_ROLE_UI], TTF_ROLE_UI);
        face_defaults(&s_faces[TTF_ROLE_CONTENT_ALT], TTF_ROLE_CONTENT_ALT);
    }

    // 1) 内容面先装。try_map_table 以 heap_caps_get_largest_free_block() 为门槛：
    //    先装内建 UI 面会吃掉这部分余量，可能让 SD 字体从"整体映射进 PSRAM"静默
    //    退化成"每个冷字形读一次 SD"。内建面本来常驻 flash(读它就是 memcpy)，
    //    它拿不到映射几乎不损失什么，反过来损失就大了。
    esp_err_t err = ESP_OK;
    if (!s_faces[TTF_ROLE_CONTENT].f_font_ready) {
        ttf_face_t* prev = face_enter(TTF_ROLE_CONTENT);
        const char* path = app_settings_font_path();
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

// 线性抗锯齿的中间灰在这块屏上偏亮。按 TTF_COVER_GAMMA 抬覆盖率，半透明边缘更深。
static uint8_t s_cover[256];

/* ---- 绘制 / Draw ---- */
static void ttf_cover_lut_init(void) {
    static bool ready;
    if (ready) return;
    for (int i = 0; i < 256; i++) {
        float t = (float)i / 255.f;
        int v = (int)(255.f * powf(t, TTF_COVER_GAMMA) + 0.5f);
        if (v < 0) v = 0;
        if (v > 255) v = 255;
        s_cover[i] = (uint8_t)v;
    }
    ready = true;
}

static uint8_t mix_ink(uint8_t alpha, uint8_t fg, uint8_t bg) {
    return (uint8_t)(bg + s_cover[alpha] * ((int)fg - (int)bg) / 255);
}

void ttf_draw_text(
    uint8_t* framebuffer, int x, int y, int size, const char* text,
    enum EpdFontFlags align, uint8_t fg, uint8_t bg
) {
    ttf_draw_text_px(framebuffer, x, y, size_to_px(size), text, align, fg, bg);
}

void ttf_draw_text_px(
    uint8_t* framebuffer, int x, int y, int pixel_height, const char* text,
    enum EpdFontFlags align, uint8_t fg, uint8_t bg
) {
    if (!font_ready || framebuffer == NULL || text == NULL) return;
    ttf_cover_lut_init();
    pixel_height = clamp_px(pixel_height);
    warm_text_io(pixel_height, text);
    fb_fast_sync();   // 本次绘制内旋转/尺寸只查一次

    int cursor_x = x;
    if (align & EPD_DRAW_ALIGN_CENTER) {
        cursor_x = x - measure_width(pixel_height, text) / 2;
    } else if (align & EPD_DRAW_ALIGN_RIGHT) {
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
    enum EpdFontFlags align, uint8_t fg, uint8_t bg
) {
    if (!font_ready || framebuffer == NULL || text == NULL) return;
    pixel_height = clamp_px(pixel_height);
    warm_text_io(pixel_height, text);
    fb_fast_sync();   // 本次绘制内旋转/尺寸只查一次

    int cursor_x = x;
    if (align & EPD_DRAW_ALIGN_CENTER) {
        cursor_x = x - measure_width(pixel_height, text) / 2;
    } else if (align & EPD_DRAW_ALIGN_RIGHT) {
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

