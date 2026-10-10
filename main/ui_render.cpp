/*
 * 渲染任务实现：差分 → 区域/波形决策 → epdiy 推屏。
 *
 * 线程模型
 * --------
 *   core0 (app 主任务)                        core1 (ui_render, prio 6)
 *   ui_clear()  ──取一块空闲缓冲──▶ 画 ──┐
 *                                        │ ui_commit() 投递，不等
 *                                        └──────▶ 差分 → 选波形 → 推屏 ──▶ 还缓冲
 *   两块缓冲轮流用，core0 最多领先两帧；两块都占住时在 ui_clear() 里阻塞（背压）。
 *
 * 差分基准是 epdiy 的 back_fb —— "上一次真正驱动到面板上的内容"，本来就是要比的
 * 对象，白拿一块 400KB，不用再维护帧快照。
 *
 * 推屏前把工作缓冲拷进 hl->front_fb（**不改指针**，理由见 copy_to_front）。
 * 阅读器不走这条路：它直接画 front_fb、直接调 epd_hl_update_screen，见文件里
 * copy_to_front 的注释。
 */
#include "ui_render.h"

#include "ui_helpers.h"   // SCREEN_W / SCREEN_H

#include "app_config.h"
#include "board_hw.h"
#include "diff_scan.h"   // fb_scan_diff_bytes：按 64 位字扫差分（逐字节等价，见 tests/host/diff_scan）
#include "display.h"
#include "editor_vk.h"   // editorVkVisible/editorVkTop：虚拟键盘面板顶
#include "e0470_epaper_waveform.h"
#include "epdiy.h"
#include "fb_fast.h"   // fb_rot_from_phys：全仓唯一的旋转方向定义（差分包围盒用）
#include "fb_scan_window.h"   // 逻辑行段 → 物理扫描窗口（差分快路用；主机端穷举对拍过）
#include "hw/input.h"   // input_tick_throttled：等缓冲时补采样（见 acquire_buffer）

// fb_scan_window.h 不 include epdiy（主机端测试要能原样 include），所以那里的 rot 只按
// 数值判断 —— 这里把"数值 ↔ EpdRotation"钉死，哪天上游改了枚举顺序会在编译期炸掉。
static_assert(EPD_ROT_LANDSCAPE == 0 && EPD_ROT_PORTRAIT == 1 &&
                  EPD_ROT_INVERTED_LANDSCAPE == 2 && EPD_ROT_INVERTED_PORTRAIT == 3,
              "fb_scan_window.h 的 rot 取值与 epdiy 的 EpdRotation 不一致");
#include "ime/IME.h"   // g_ime.composing()（区域判定在 core0 侧算，见 ui_render_submit）
#include "settings_manager.h"   // imeCleanMode()（同样只在 core0 侧读，见 ime_clean_policy）
#include "u8g2_shim.h"

#include <atomic>
#include <cstring>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_system.h>   // esp_restart：渲染任务卡死时唯一的自救动作
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>   // xTaskCreatePinnedToCoreWithCaps：把栈放到 PSRAM
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#ifndef E0470_GRAY8_TEXT_WAVEFORM
#define E0470_GRAY8_TEXT_WAVEFORM E0470_GRAY8_WAVEFORM
#endif

static const char *TAG = "ui_render";

static bool uiPerfLogOn() {
    return g_settings.getString("ui_perf_log", "0") == "1";
}

// epdiy 自己有两个 epd_prep 线程跑在 configMAX_PRIORITIES-1；渲染任务必须低于它们，
// 否则行打包抢不到 CPU，送行队列欠载（EPD_DRAW_EMPTY_LINE_QUEUE）。
#define UI_RENDER_CORE 1
#define UI_RENDER_PRIO 6
// 栈走 PSRAM（内部 RAM 常态只剩 ~12KB，这 12KB 是最大的一笔）。必须用
// xTaskCreatePinnedToCoreWithCaps 明确要 SPIRAM：xTaskCreate* 的栈**一定**落在
// 内部 RAM——IDF 的 pvPortMalloc 把 caps 硬编码成 MALLOC_CAP_INTERNAL，而
// CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM 只管 xTaskCreateStatic。见 init。
#define UI_RENDER_STACK 12288
#define UI_RENDER_QUEUE_LEN 4
#define UI_RENDER_TAKE_MS 3000  // 取缓冲的兜底超时：异常时别把 core0 永久卡死
// 空闲轮询周期：只为让 rails_idle_check 有机会到点下电（8s 期限），别省成 portMAX_DELAY。
#define UI_RENDER_IDLE_MS 250

// 跟随 DU（8 帧，FAST 扫描）攒够这么多次就整屏 GC16 清一次残影。
#define FOLLOW_GC16_EVERY 8
// "上屏刷法=快"欠下的正文（跟随表推力只有阈值表的 1/4，字先发灰）**攒着不清**，
// 攒到打字停顿由 ime_clean_tick 一次坐实 —— 一次清残影同时管输入法那两行和正文，
// 不多清一遍。这个次数就是设置项「计数清」那一档的粒度，同时也是**兜底**：句读/上屏
// 都不记的档（计数清）或一直没等到停顿的档，欠账不能无限攒，到次数借当拍记一次。
//
// 为什么不是"每一拍都并进停顿清理"（原来就是那样）：正文差分矩形常从光标行一路到
// 屏幕底（段落重排 + 底栏字数），每停一次就是整屏闪一下 —— 实测一次会话闪 10 遍，
// 而正文那点灰用户看下来"能接受"，全是白闪。
#define FAST_SETTLE_EVERY 8
// IME 候选/编码条局刷的合并窗口：窗口内的连续输入只推最后一次。
// 收到 0（原来 5000）：实测人类连打间隔 150~200ms、键盘自动重复 50ms，这个窗口
// **从不合并任何东西**，每键白等 5ms。defer 分支本身留着 —— 它同时是"面板内的差分
// 走 8 相 FOLLOW DU（90ms）"的路由，拆掉会让面板差分落进整屏 GL16 那条 400ms 的路。
#define IME_DEFER_US 0
// "清编码区+候选区这两行"（外加快档欠下的正文）的停顿阈值：距离最后一次**输入法动作**
// （按键落在输入法条内的那一帧，或一次上屏/句读）超过这么久，才把那块过一遍区域 GC16。
// 清一遍 = 30 相，**耗时由相位数决定、与区域大小无关**（高层刷新 min_y 恒为 0，
// 整块面板本来就要扫一遍，见 display.c），约 300 多毫秒。所以不能每个上屏词都清一次。
// 这个期限和"哪一刻记账"是两件事：**记不记账**由设置项 ime_clean 定（见 ime_clean_policy，
// 句读后 / 上屏后 / 计数清 / 从不清），这里只定期限 —— 期限要钉在"最后一次输入法动作"上，
// 取值要明显大于"连打时两次按键的间隔"（本机上大概 0.2~0.6s），否则每敲一个键都会触发。
#define IME_CLEAN_PAUSE_US 700000
// core0 申报"面板顶线以上一个像素都没动"时的抽检频率：每这么多拍快路里，重算**一次**
// 线上那块的真差分校验申报（第 1 拍也查，见 render_present）。抽检那一拍线上那块照扫；
// 不抽检的拍只扫面板那几行（约 179/684 行 ≈ 26% → ~3ms 而不是 28ms）。
// 抽检不通过（线上真有差分）说明这条结构性不变量被破了 —— 打警告并短期失信
// （s_hint_mistrust 记 8 拍，见 render_present），宁慢不错。
#define HINT_CHECK_EVERY 8

// 打字期间**一次全刷都不做**：键区以前每 7 键把键盘矩形整块过一遍 8 灰阶 GL16 清残影，
// 结果就是打字打到一半莫名其妙闪一下键盘。现在清残影只针对"编码区+候选区那两行"，而且
// **推迟到打字停顿**（见 IME_CLEAN_PAUSE_US 与 ime_clean_tick）—— 记账的那一拍不当场刷，
// 等用户停手再看的那一下清一次。三条打字路径都算：虚拟键盘（s_local_only 分支）、实体键盘
// 快刷（s_fast_partial 分支）、以及那些不开快刷的界面（跟随 DU 那条路）。
// 整屏那次清账顺延到打字结束（键盘收起）后的下一次提交，见 s_gc16_pending 那条。
//
// **哪一刻记账由设置项 ime_clean 定**（ime_clean_policy）：句读后（默认）/ 上屏后 /
// 计数清 / 从不清。句读和上屏原本是"两者都记"—— 一句话里上屏清一遍、敲完句读又清一遍，
// 用户看得见"刷了 2 次"，所以默认只留句读：那是用户天然停手组织下一句的时刻。上屏清适合
// 打长句、很久不敲标点的人；计数清是不挑事件、只按拍数攒 —— **四档都是字面意思，计数
// 兜底只挂在计数清上**（原来除"从不清"外所有档都有兜底，"句读后"也会在没有句读的时候
// 被清一次，用户 2026-10-09 报的就是这条）。
// 这几档都只是**记账**，真正那次区域 GC16 一律由 ime_clean_tick 在停顿时做。
//
// **2026-10-09 补上快档那一处漏网的当场刷**：快档（上屏刷法=快）的句读档原来是**当场清**
// 的，而当场清要并上这一帧的差分 d（"改动行 → 屏底"），并出来就是整屏 —— 一次 377ms 的
// 整屏 GC16，用户每个标点黑闪一次（"打字途中也会闪"）。现在它也走记账这条路，
// 闪只可能落在停顿那一拍。见下面是 2′) 那段。
//
// **2026-10-09 又堵上第二条**：稳档（上屏刷法=稳）那一拍原来是整页 update_display_with，
// 于是每一拍都替 display.c 那份**共享**残影预算 +1，攒够 APP_GC16_EVERY(=14) 被升成整屏
// GC16 —— 打字一秒三四拍，几秒钟就"全屏黑白闪一下"（按方向键十几下就来一次），而且跟
// ime_clean 四档**一点关系都没有**（"从不清"照样闪，那不是输入法那笔账）。现在走
// update_display_typing_du（同一条刷法，不记账），所以"打字期间一次全刷都不做"这句话
// 才真正对所有档成立。

// 正文区（编辑器文本区）的"局刷"档位。区域只用于**限制驱动范围**（少留残影），
// 不省时间：高层刷新 min_y 恒为 0，整块面板本来就要扫一遍，耗时 = 相位数 × 帧周期。
// 所以这里挑画质而不是速度 —— 输入法区要极速（FOLLOW DU 8 帧 ≈89ms），正文区要干净。
#define BODY_PARTIAL_WAVEFORM (&E0470_WAVEFORM)
#define BODY_PARTIAL_MODE MODE_GL16

enum { JOB_PRESENT = 0, JOB_FULL, JOB_FROM_WHITE, JOB_INVALIDATE };

// 「那两行什么时候记账清残影」的两个位（设置项 ime_clean，见 ime_clean_policy）。
// 两位都为 0 = 设置里的「计数清」：不按事件记，只靠 FAST_SETTLE_EVERY 那条计数兜底。
// NO_GC16 是最激进档（"从不清"）：输入法这条路上**永远不做那次区域 GC16**，
// 连那个跟设置无关的计数兜底也一起关掉 —— 打字全程只有快刷（跟随 DU / 8 灰阶
// 正文表），一个字都不会因为"要还账"而多停一帧。代价是快档发灰的正文与键盘残影
// 只能等下一次整屏刷新（翻页全刷/换章/摇一摇/长按全刷/进别的界面）才被打扫。
// 它不是「计数清」的低配：计数清还会在攒够拍数时记一次账，只是不挑事件。
enum { IME_CLEAN_ON_PUNCT = 1, IME_CLEAN_ON_COMMIT = 2, IME_CLEAN_NO_GC16 = 4 };

struct UiJob {
    int idx;         // 工作缓冲下标（-1 = 用 front_fb 直画，降级路径）
    int kind;
    int ime_top;     // IME 面板顶（逻辑 y）；-1 = 未在组合输入
    EpdRect cand;    // 编码区+候选区（逻辑）；width<=0 = 键盘没显示
    bool ime_commit; // 这一帧之前刚上屏过一次（IME::commitSeq 变了）
    bool ime_punct;  // 这一帧刚吐出一个句读（IME::punctSeq 变了）**且策略允许**：记账清这两行
    // 上屏那一拍要不要记账清残影。与 ime_commit 分开：ime_commit 还管着"这一帧把刷新区域
    // 扩到候选区、换 8 灰阶正文表"（那是渲染决策，与清残影无关，不受设置影响）。
    bool ime_clean_commit;
    // "从不清"档（IME_CLEAN_NO_GC16）：这一帧的输入法欠账一律不坐实，连计数兜底也不走。
    // 与 ime_clean_commit 分开：后者管"要不要记这笔账"，它管"这条路还能不能清"。
    bool ime_no_clean;
    // 「计数清」档（ime_clean 的两个事件位都为 0）：不挑事件，攒够 FAST_SETTLE_EVERY 拍
    // 记一次账。**计数兜底只属于这一档**：它原来除"从不清"外对所有档都生效，于是选了
    // "句读后"的人在没有句读的时候也会被清一次 —— 用户侧就是"我一个标点都没打，怎么
    // 打到一半又闪了"（2026-10-09 报的）。
    bool ime_count;
    // 实体键盘打字时正文那一拍走哪条路（设置项 ime_commit_mode，见 ime_commit_fast_policy）：
    // false = 整屏阈值 DU（现在这样，墨实、每拍约 220ms）；true = 只推差分矩形的跟随 DU
    // （约 56ms，但跟随表推力只有阈值表的 1/4，刚上屏的字先发灰，得靠停顿那次区域 GC16
    // 坐实 —— 见 render_present 第 2 条路与 note_ime_clean 的 extra 参数）。
    bool ime_commit_fast;
    bool force_full; // 强制整屏 GC16
    // core0 申报"输入法面板顶线（ime_top）**以上**一个像素都没动"（false = 没申报）。
    // 由 ui_render_note_ime_above_clean() 填（在 ui_render_submit 里取走并清空）。
    // 见 render_present 里那条快路。
    bool above_clean = false;
    // core0 提交这一刻的时间戳。只为计时：渲染任务拿它算"提交 → 推屏开始"之间
    // 排了多久（队列 + 合并窗口）。不用跨模块全局，是因为提交点本来就在 core0。
    int64_t stamp_us;
};

// 设置项 ime_clean → 位掩码。**只在 core0 侧调**（两个 job 组装点），渲染任务不碰 g_settings
// —— 与 ime_top/cand 同一条规矩。认不出的值当默认档（句读后）。菜单里的四项与位：
//
//   punct（句读后，默认）→ ON_PUNCT：**只有**句读那一拍记账
//   commit（上屏后）     → ON_COMMIT：**只有**上屏那一拍记账
//   count（计数清）      → 0：不挑事件，只靠 FAST_SETTLE_EVERY 那条计数（唯一有兜底的一档）
//   never（从不清）      → NO_GC16：一次都不记（连计数兜底也没有）
//
// **四档现在是字面意思**：兜底计数原来只挂在"从不清"以外所有档上，于是"句读后"在没有句读
// 的时候也会清一次（用户 2026-10-09 报的"没打标点怎么打到一半又闪了"）。要兜底就选计数清。
//
// "off" 是 count 的旧键名（语义本来就是"只剩计数兜底"，只是名字会骗人），兼容留着；
// "both" 已从菜单下线，认不出 → 退回默认的句读后 —— 存过 both 的机器要在设置里重选一次。
static uint8_t ime_clean_policy(void) {
    const std::string m = g_settings.imeCleanMode();
    if (m == "commit") return IME_CLEAN_ON_COMMIT;
    if (m == "count" || m == "off") return 0;
    if (m == "never") return IME_CLEAN_NO_GC16;
    return IME_CLEAN_ON_PUNCT;   // "punct" / 认不出
}

// 设置项 ime_commit_mode → "打字那一拍要不要换快档"。同样**只在 core0 侧调**
// （两个 job 组装点）。认不出的值当默认档（稳 = 整屏阈值 DU = 现有行为）。
static bool ime_commit_fast_policy(void) {
    return g_settings.imeCommitMode() == "fast";
}

// ── 两块工作缓冲的所有权 ─────────────────────────────────────────────────
// 信号量记"有几块空闲"，free_list 记"具体是哪几块"（短临界区，不阻塞）。
// 只要渲染任务结束时按它处理的那一块归还，core0 就不会拿到在推的那一块。
static uint8_t *s_fb[2];
static size_t s_fb_size;
static int s_taken = -1;  // core0 正在绘制的那块（-1 = 当前没开帧）
static int s_free_list[2];
static int s_free_n;
static portMUX_TYPE s_free_mux = portMUX_INITIALIZER_UNLOCKED;
static QueueHandle_t s_q;
static SemaphoreHandle_t s_free;  // 计数信号量（初值 2）
static int s_last_idx = -1;       // 渲染任务：最后一次推上屏的缓冲
// core0 自己**最后画过**的那块（在 ui_render_submit 里记 job.idx）。
// 与 s_last_idx 的区别是"谁认领"：s_last_idx 要等渲染任务进 render_present 才更新，
// 队列里压着上一次 90ms 推屏时它会落后一帧。想拿"上一帧的像素"当绘制底，
// 必须用这一块 —— 它从 core0 提交那一刻起就确定，之后没人再往里写过
// （渲染任务只读工作缓冲，别的帧只写自己 acquire 到的那块）。
static int s_last_drawn_idx = -1;
static std::atomic<bool> s_active{false};    // init 完成

// ── 渲染任务私有状态（core0 只写三个开关）────────────────────────────────
static bool s_force_full_next = true;  // 首帧从白底出图，必须整屏 GC16
// 残影计数**不在这里**：整页软刷的预算归 display.c（display_soft_refresh_due/reset），
// 阅读器那条路和我们这条路上的是同一份账 —— 各自记一份会让总残影翻倍、或在别人刚
// 清干净之后又白闪一次。见 display.c 里那段说明。
static int s_follow_partials;
static int s_fast_body_n;     // "快"档连打：距上一次把正文并进停顿清理过了多少拍
static bool s_gc16_pending;   // 打字期间攒下的那次清残影
static bool s_ime_deferred;
static int64_t s_ime_defer_until;
static int s_defer_idx = -1;  // 合并窗口期间扣住不放的缓冲
static uint8_t *s_defer_cur;
static EpdRect s_defer_rect;
static EpdRect s_defer_cand;   // 本次合并窗口的编码区候选区矩形（core0 提交时带上来的）
static bool s_defer_commit;    // 本次合并窗口是"上屏"那一拍（要顺手清一遍候选区）
static int64_t s_defer_stamp_us;  // 扣住那一帧的提交时刻（只为计时）
// 「提交→推屏」拆账用的两个中间时刻（只为计时）：只有"提交"到"推屏开始"之间那三件事
// —— 队列唤醒、diff_bounding_rect、copy_to_front —— 需要分开看谁贵。见 flush_deferred 的日志。
static int64_t s_diag_wake_us;
static int64_t s_diag_diff_us;
// core0 申报的"面板顶线以上没动"（false = 没申报）。core0 写完、ui_render_submit 取走
// 并清空（跨核单次交接，不会有两个帧共用一次申报）。
static bool s_above_clean = false;
// 抽检计数与失信计数（只在渲染任务里动）。失信就是"下一次抽检提前来"：
// 申报连续不靠谱时不装作没看见，直接把快路停 s_hint_mistrust 拍再说。
static int s_hint_n = 0;
static int s_hint_mistrust = 0;
static std::atomic<bool> s_fast_partial{false};
static std::atomic<bool> s_fast_partial_first{false};
static std::atomic<bool> s_local_only{false};
// "菜单型界面"（设置各页、写作/计划主界面、清单）：**一律不记账**，见下面的说明。
static std::atomic<bool> s_menu_only{false};

// ── 策略开关（core0 调用，渲染任务读）────────────────────────────────────
void ui_render_set_fast_partial(bool enable) {
    if (enable && !s_fast_partial.load(std::memory_order_relaxed)) {
        s_fast_partial_first.store(true, std::memory_order_relaxed);
    }
    s_fast_partial.store(enable, std::memory_order_relaxed);
}

void ui_render_set_local_only(bool enable) { s_local_only.store(enable, std::memory_order_relaxed); }

// 菜单型界面：翻列表 / 换选中行 / 切换子页那一类屏（kScreens 的 local_only 列 + 计划模式
// 浏览态）。它们借 local_only 那条 arm 只是为了"一律局刷"（差分常超半屏，按老规则会整屏
// GL16 闪一下），**不是**在打字，所以那套"攒够 N 拍清一次"的残影账在这里全是副作用：
//
//   * 「计数清」那一档按**拍**记账（ime_note_due 的 typing=true）：翻列表按几下方向键
//     就攒够一次 → 停顿处闪一下；
//   * 面板跟随 DU 攒出来的 pending 在这种屏上永远不会当场落地（local_only 开着），
//     它会跟着用户走到**下一个两个开关都关的界面**才炸成整屏 GC16 —— 看着就是
//     "进了别的界面莫名闪一下"；
//   * 菜单的局刷是 **GL16 全像素**（区域内每个像素都被驱动到目标灰阶），天生不自攒残影，
//     这笔账本来就是空的。
//
// 用户 2026-10-09 报的"连蓝牙键盘时进设置就别计数了，闪得厉害"就是这一条。
void ui_render_set_menu_only(bool enable) { s_menu_only.store(enable, std::memory_order_relaxed); }

// ── 缓冲取还 ─────────────────────────────────────────────────────────────
// **等缓冲的时候要补采样**（见 hw/input.h 的 input_tick_throttled）。
//
// 这里等的是核心 1 把上一帧推完。整屏 GC16 一记约 1.8s，区域刷大块也常有几百毫秒——
// 实测主循环里 230~500ms 的"零采样窗口"全部落在这一段。cst836u **没有锁存寄存器**，
// 一次 I2C 读只返回"当前是否按着"，所以这段时间里一次完整点按整个消失，用户侧就是
// "打快了漏键"。拆成小段等、段间补一拍采样：**总时长不变**，只是把等待的空转拿来读
// 了几次 I2C；按键的**按下沿**因此被记进手势状态机，抬手后的那一拍照常投递（晚一拍，
// 但不会丢）。只采样，不投递——键照旧由主循环的 input_poll 取走。
//
// wait=0 时退化成"试一次就走"，与原来同义（pdMS_TO_TICKS(0) 的切片会让循环第一次就
// 到期返回 -1）。
static int acquire_buffer(TickType_t wait) {
    if (!s_active.load(std::memory_order_acquire)) return -1;
    const TickType_t deadline = xTaskGetTickCount() + wait;
    for (;;) {
        TickType_t left = deadline - xTaskGetTickCount();
        if ((int32_t)left <= 0) return -1;
        TickType_t slice = pdMS_TO_TICKS(10);
        if (slice > left) slice = left;
        if (xSemaphoreTake(s_free, slice) == pdTRUE) break;
        input_tick_throttled();
    }
    int idx = -1;
    portENTER_CRITICAL(&s_free_mux);
    if (s_free_n > 0) idx = s_free_list[--s_free_n];
    portEXIT_CRITICAL(&s_free_mux);
    if (idx < 0) xSemaphoreGive(s_free);  // 不该发生：令牌与表项一一对应
    return idx;
}

static void release_buffer(int idx) {
    if (idx < 0 || !s_free) return;
    portENTER_CRITICAL(&s_free_mux);
    if (s_free_n < 2) s_free_list[s_free_n++] = idx;
    portEXIT_CRITICAL(&s_free_mux);
    xSemaphoreGive(s_free);
}

// ── 几何/差分工具 ────────────────────────────────────────────────────────
// 新旧帧缓冲差异的包围盒（**逻辑**像素坐标）。四种旋转都精确：找出变化的字节，
// 再把物理坐标反着映射回逻辑坐标。竖屏下以前一律退化成整屏，虚拟键盘打字就变成
// 每键整屏刷新；这里必须精确。无差异返回空矩形。
//
// 扫描本身交给 fb_scan_diff_bytes（**按 64 位字比**、字不同才展开成字节）—— 整屏
// 415,872 字节、每个 UI 帧和每个阅读器按键都要过一遍，逐字节是白花带宽。实测 32 位
// 41ms、64 位 22ms（见 main/gfx/diff_scan.h 的微基准）；它报出的位置与逐字节扫描
// 逐位相同（主机端对拍在 tests/host/diff_scan/）。
//
// **按行段扫**：y0/y1 是**逻辑**行区间 [y0, y1)（y1 < 0 = 一直到屏底）。给"面板顶线
// 以上一定没动"那条快路用：它只需要线以下那块，于是把 831KB 的整屏扫描缩到那一段
// （省下的就是取数指令本身，见 diff_scan.h 的说明）。传 (0, -1) 就是整屏，即旧口径。
//
// 逻辑行段 → 物理扫描窗口由 fb_scan_window_for_rows 算（纯函数，四种旋转都对，
// 主机端 tests/host/diff_scan/ 穷举对拍过：窗口 = "含至少一个段内像素的字节"的精确集合）。
//
// **一趟扫两个框**：out_all 是 [y0,y1) 整段的（旧口径，四个老调用点照旧），
// out_band 是 [band_y0,band_y1) 单独一个（可传 nullptr = 不要）。要两个框的原因见
// render_present 里"新输入的文字"那段：整段的框是**凸包** —— 一次上屏就是
// 2,9 1208x669（新正文那几行 + 状态栏的字数 + 输入法条被一起圈进去），拿它决定"推哪
// 一块"没问题（区域只限制驱动范围），但拿它去**清残影**就是整屏闪；清残影要的是"刚敲
// 进去那几个字占了哪几行"，那是 band 那个框。两个框在同一次扫描里算完，比再扫一遍
// 省一整趟取数（取数就是这条路的全部成本，见 diff_scan.h）。
static void diff_bounding_rect_rows2(const uint8_t *a, const uint8_t *b, int y0, int y1,
                                     int band_y0, int band_y1,
                                     EpdRect *out_all, EpdRect *out_band) {
    if (out_all) *out_all = EpdRect{0, 0, 0, 0};
    if (out_band) *out_band = EpdRect{0, 0, 0, 0};
    if (!out_all && !out_band) return;

    const int fb_w = epd_width(), fb_h = epd_height();
    const int row_bytes = fb_w / 2;
    const int rot = epd_get_rotation();
    const int sw = SCREEN_W, sh = SCREEN_H;

    if (y1 < 0 || y1 > sh) y1 = sh;
    if (y0 >= y1) return;
    const bool want_band = out_band && band_y0 < band_y1;

    const FbScanWindow win = fb_scan_window_for_rows(rot, fb_w, fb_h, y0, y1);
    if (win.n_rows <= 0) return;
    const int row_off = win.row_off;

    int x0 = sw, ylo = sh, x1 = -1, yhi = -1;
    int bx0 = sw, bylo = sh, bx1 = -1, byhi = -1;
    // 物理 → 逻辑的方向定义在 fb_fast.h（全仓唯一一份，P4）；这里不再手抄 switch。
    // 直接传 rot/fb_w/fb_h（epdiy 现读），不碰 fb_fast 的缓存，线程语义同原样。
    auto add = [&](int px, int py) {
        int lx, ly;
        fb_rot_from_phys(rot, fb_w, fb_h, px, py, &lx, &ly);
        if (ly < y0 || ly >= y1) return;   // 列段/行段都放宽取整过，这里逐像素收口
        if (lx < x0) x0 = lx;
        if (lx > x1) x1 = lx;
        if (ly < ylo) ylo = ly;
        if (ly > yhi) yhi = ly;
        if (want_band && ly >= band_y0 && ly < band_y1) {
            if (lx < bx0) bx0 = lx;
            if (lx > bx1) bx1 = lx;
            if (ly < bylo) bylo = ly;
            if (ly > byhi) byhi = ly;
        }
    };

    // 扫描从 a/b 的第 row_off 行起，回调报的 y 是**相对**行号 —— 加回 row_off 才是物理行
    // （竖屏那两支 row_off 恒为 0，但横屏 180° 那支不是，绝不能漏）。
    fb_scan_diff_bytes_range(a + (size_t)row_off * row_bytes, b + (size_t)row_off * row_bytes,
                             row_bytes, win.n_rows, win.xb0, win.xb1, [&](int xb, int y) {
                                 add(xb * 2, y + row_off);
                                 add(xb * 2 + 1, y + row_off);
                             });

    // 外扩 2px（抗锯齿 + 半字节边界余量）并夹到屏内
    auto finish = [&](int rx0, int rylo, int rx1, int ryhi) {
        rx0 -= 2; if (rx0 < 0) rx0 = 0;
        rx1 += 2; if (rx1 >= sw) rx1 = sw - 1;
        rylo -= 1; if (rylo < 0) rylo = 0;
        ryhi += 1; if (ryhi >= sh) ryhi = sh - 1;
        return EpdRect{rx0, rylo, rx1 - rx0 + 1, ryhi - rylo + 1};
    };
    if (out_all && x1 >= 0) *out_all = finish(x0, ylo, x1, yhi);
    if (want_band && bx1 >= 0) *out_band = finish(bx0, bylo, bx1, byhi);
}

static EpdRect diff_bounding_rect_rows(const uint8_t *a, const uint8_t *b, int y0, int y1) {
    EpdRect all;
    diff_bounding_rect_rows2(a, b, y0, y1, 0, 0, &all, nullptr);
    return all;
}

// 整屏差分包围盒（= 行段 (0, 屏底)）。
static EpdRect diff_bounding_rect(const uint8_t *a, const uint8_t *b) {
    return diff_bounding_rect_rows(a, b, 0, -1);
}

// 两个矩形的并集；空矩形（width<=0）当"没有"处理。用来把"本帧变了的那块"扩到
// 盖住整个键盘：全像素刷会驱动区域内的**每一行**，所以区域必须覆盖所有变化像素，
// 否则区域外的变化就丢了。
static EpdRect rect_union(EpdRect a, EpdRect b) {
    if (a.width <= 0 || a.height <= 0) return b;
    if (b.width <= 0 || b.height <= 0) return a;
    const int x0 = a.x < b.x ? a.x : b.x;
    const int y0 = a.y < b.y ? a.y : b.y;
    const int xa = a.x + a.width, xb = b.x + b.width;
    const int ya = a.y + a.height, yb = b.y + b.height;
    const int x1 = xa > xb ? xa : xb;
    const int y1 = ya > yb ? ya : yb;
    EpdRect r = {x0, y0, x1 - x0, y1 - y0};
    return r;
}

// （原先两条抽检辅助 —— rect_covers / grow_px —— 随"按矩形申报"一起删掉了：
// 现在申报的是"线上没动"这条**行**不变量，抽检只需要问"线上那块差分是不是空的"，
// 不再需要矩形包含关系。）

// ── 推屏 ─────────────────────────────────────────────────────────────────
// 推屏前把这块工作缓冲**拷进** epdiy 的 front_fb（而不是临时改指针指过去）。
//
// front_fb 必须永远是"屏上当前的画面"，这条不变式有两个外部依赖方：
//   1. 阅读器整条绘制路径（GfxRenderer / HalDisplay::getFrameBuffer）画的就是
//      front_fb，并且自己直接调 epd_hl_update_screen —— 它**绕过**渲染任务。
//      一旦我们把 front_fb 指到工作缓冲上，阅读器就会画进我们的缓冲里。
//   2. 待机保留帧要从"屏上现在的画面"取，也只有 front_fb 靠得住（见 keep_frame）。
// 代价是一次 406KB 的 PSRAM 拷贝（约 5–10ms），换掉一层跨模块的隐式耦合。
static void copy_to_front(EpdiyHighlevelState *hl, uint8_t *src) {
    if (hl->front_fb != src) memcpy(hl->front_fb, src, s_fb_size);
}

// ── 上屏后的"两行清残影"：推迟到打字停顿 ─────────────────────────────────
// 见 IME_CLEAN_PAUSE_US。这里的三个静态量只在渲染任务里读写（render_present 的
// clean_ime_rows 记、ime_clean_tick 清），不跨核。
static EpdRect s_clean_rect = {0, 0, 0, 0};   // 记账那一拍报上来的那两行（上屏 / 句读）
// "上屏刷法=快"攒的正文欠账：两次坐实之间，**每一拍**上屏变过的正文矩形都并进来。
// 少了它，第 8 拍坐实时只能清到"第 8 拍自己的增量差分"，前 7 拍推过又抹掉的位置
// （旧字残影）就永远没人管 —— 差分是拿 back_fb 比的，是增量不是并集。
static EpdRect s_body_dirty = {0, 0, 0, 0};
static bool    s_body_dirty_any = false;
static int64_t s_clean_due_us = 0;            // 早于这个时刻不清
static bool    s_clean_dirty = false;         // 攒了残影没清

// 输入法动过一下（按键落在输入法条内的那一帧 / 一次上屏）：把清理期限往后推。
// **只推期限、不置脏** —— 期限要钉在"最后一次输入法动作"上，不然用户正连着打字时
// 会从中间插进来一次 330ms 的刷屏，比每词清一次还难受。
static void ime_clean_arm(int64_t pause_us) {
    s_clean_due_us = esp_timer_get_time() + pause_us;
}

// 记账：这两行被写脏了，过 IME_CLEAN_PAUSE_US 之后清一遍。**"置脏"和"推期限"是两件事**，
// 这里一次做完 —— 只推期限不置脏，那一次清理就永远等不到（ime_clean_tick 第一行
// 就是 `if (!s_clean_dirty) return;`）。
//
// extra = 除了输入法那两行之外，本帧还要一并坐实的矩形（"上屏刷法=快"那一档拍过的
// 正文区：跟随表推力只有阈值表的 1/4，字先发灰，等停顿这次区域 GC16 把它推实）。
// 与既有的账**取并集**而不是覆盖：一个停顿窗口里可能拍过好几处，覆盖会把先拍的漏掉。
// 并集只在 extra 传进来时才可能变大 —— 默认那些路（只记 cand）并集的就是同一个矩形，
// 行为与原来一致。
static void note_ime_clean(const UiJob &job, const EpdRect *extra = nullptr) {
    EpdRect r = job.cand;
    if (extra) r = rect_union(r, *extra);
    if (r.width > 0 && r.height > 0) {
        s_clean_rect = s_clean_dirty ? rect_union(s_clean_rect, r) : r;
        s_clean_dirty = true;
    }
    // 记账这一刻就把"为什么记、记多大"写下来：真正清的那一行在 ime_clean_tick（同一个
    // ui_perf_log 开关）。两行对着看就知道那次闪是"欠账本来就大"还是"两条路并起来变大"。
    if (uiPerfLogOn())
        ESP_LOGI(TAG, "记账: %s 本次 %d,%d %dx%d（条 %d,%d %dx%d + 欠账 %d,%d %dx%d）",
                 job.ime_punct ? "句读" : ((job.ime_commit && job.ime_clean_commit) ? "上屏" : "计数"),
                 r.x, r.y, r.width, r.height, job.cand.x, job.cand.y, job.cand.width, job.cand.height,
                 extra ? extra->x : 0, extra ? extra->y : 0,
                 extra ? extra->width : 0, extra ? extra->height : 0);
    ime_clean_arm(IME_CLEAN_PAUSE_US);
}

// 整屏全像素刷（GC16 / GL16 整屏 / from-white / 失效重刷）之后，这两行本来就被
// 全像素驱动过一遍了，把记账丢掉，免得停顿时再白清一次。
static void ime_clean_forget(void) {
    s_clean_dirty = false;
    s_clean_rect = EpdRect{0, 0, 0, 0};
    s_body_dirty_any = false;   // 整屏全像素刷过，快档攒的欠账也一并销了
}

// 把"这一帧正文里变过的那块"并进残影账。等打字停顿那次区域 GC16 会把它和输入法那两行
// **一起**清掉（ime_clean_tick 的 rect_union，一次清两处、不为正文多清一遍）。
//
// 为什么稳档也要记：稳档推的是阈值表（墨实、这一拍不发灰），但**残影**照样从这一拍起
// 攒在正文上 —— 差分是拿 back_fb 比的（增量），推过又抹掉的旧字位置不会自己消失。
// 不记的下场就是用户 2026-10-10 报的那样：账里永远只有 `0,505 1216x128`（输入法条那一
// 块），正文一路没人管，"刷新只刷了候选区和编码区"。
//
// **只在正文带够小的时候记**：一条多行正文整体重排（滚动、折行、打字机模式推页）时这
// 条带会一路撑到面板顶，记下来就是"清一次闪半屏"，比残影本身更难接受。那种时候维持原
// 状（只清输入法那两行）—— 反正整块刚被阈值 DU 整页推过，墨是实的。
static void note_body_band(const EpdRect &band) {
    if (band.width <= 0 || band.height <= 0) return;
    const long long area = (long long)band.width * band.height;
    if (area > (long long)SCREEN_W * SCREEN_H / 4) {
        if (uiPerfLogOn())
            ESP_LOGI(TAG, "正文残影账: 跳过（本次 %d,%d %dx%d = %lldKB，超 1/4 屏的 %lldKB）",
                     band.x, band.y, band.width, band.height, area / 1024,
                     (long long)SCREEN_W * SCREEN_H / 4 / 1024);
        return;
    }
    s_body_dirty = s_body_dirty_any ? rect_union(s_body_dirty, band) : band;
    s_body_dirty_any = true;
    if (uiPerfLogOn())
        ESP_LOGI(TAG, "正文残影账: 并入 %d,%d %dx%d → 合计 %d,%d %dx%d",
                 band.x, band.y, band.width, band.height,
                 s_body_dirty.x, s_body_dirty.y, s_body_dirty.width, s_body_dirty.height);
}

// 到点就清。渲染任务主循环每轮都调（空闲节拍 / 每次推屏之后）。
// 走 update_display_area_clean（区域 GC16）—— 为什么不是"8 灰阶正文表 + GL16"见
// display.c 里那段：GL16 表白→白是全保持，只补了一帧白推，压不掉"上一拍已经是白"的
// 墨痕，这就是上屏/句读之后残影还在的根因。相位一样多，区域不省时间，所以不多花时间。
static void ime_clean_tick(void) {
    if (!s_clean_dirty) return;
    if (esp_timer_get_time() < s_clean_due_us) return;
    // 队列里还压着没推完的帧 = 用户又动了（或界面在连刷）。清残影是**观感**上的事，
    // 抢在用户下一帧前面只会让那帧多等 330ms（看着就是打字卡一下）。所以让路：
    // 期限不销，等哪一轮真没帧了再清 —— 顺延到下一次真停手，代价只是残影多留一会。
    if (uxQueueMessagesWaiting(s_q) > 0) return;
    s_clean_dirty = false;
    EpdRect r = s_clean_rect;
    s_clean_rect = EpdRect{0, 0, 0, 0};
    // "上屏刷法=快"欠下的正文在这一刻才并进来（不是在记账那一刻）：记账到现在用户可能
    // 又打了一句，欠账长过记账时的快照 —— 现取才是"两个标点之间"最新的那块。
    // **一次清残影同时管两处**（输入法那两行 + 正文欠账），不为正文多清一遍。
    if (s_body_dirty_any) {
        r = rect_union(r, s_body_dirty);
        s_body_dirty_any = false;
        s_fast_body_n = 0;
    }
    if (r.width <= 0 || r.height <= 0) return;
    EpdiyHighlevelState *hl = board_hl();
    if (!hl) return;
    // 打一行：上屏/句读写的是"账"，真正压在哪儿只有这一行说得清（区域 GC16，见
    // display.c）。查"为什么残影还在"时先看这里有没有出来。
    if (uiPerfLogOn()) ESP_LOGI(TAG, "清输入法两行残影 %d,%d %dx%d", r.x, r.y, r.width, r.height);
    guard_draw_result(hl, update_display_area_clean(hl, r));
}

// 整屏刷新：日常 GL16 差分，周期 GC16 清残影。force_gc16 时无条件整屏 GC16。
static void do_full_refresh(EpdiyHighlevelState *hl, uint8_t *cur, bool force_gc16) {
    copy_to_front(hl, cur);
    ime_clean_forget();   // 整屏全像素刷过一遍，输入法那两行的记账可以销了
    // 残影预算归零：这条路**整屏**逐像素驱动了一遍，跟随 DU 攒的那点账已经一笔勾销。
    // 不归零的话，账还停在"攒了 7 次"，紧接着一次面板跟随 DU 就撞线 —— 本该 8 拍才来
    // 一发的整屏 GC16 会在**这次整屏刷的下一拍**就来，用户看到的是"刚刷完又闪一下"。
    // full_refresh_now / from_white_now / force_full 三条整屏路本来就是这个口径，这里补齐。
    s_follow_partials = 0;
    // 这一行是"屏幕突然整块闪一下"类问题的第一现场记录：整屏刷有四条来路
    // （force_full / 进界面首帧 / 差分高过半个屏 / 打字后的补账），每条都打一行，
    // 顺手记下这次是 GC16（黑白闪）还是 GL16（灰阶、不闪）以及为什么。
    const bool gc16 = force_gc16 || display_soft_refresh_due();
    if (uiPerfLogOn())
        ESP_LOGI(TAG, "整屏刷: %s（force_gc16=%d 预算到点了=%d）", gc16 ? "GC16" : "GL16",
                 (int)force_gc16, (int)display_soft_refresh_due());
    if (gc16) {
        display_soft_refresh_reset();
        guard_draw_result(hl, update_display_full(hl));
    } else {
        // 走 GL16：hl_update 会把这笔软刷记进同一份预算（update_display_with 走的是
        // E0470_WAVEFORM 整页刷，area == NULL，正好在计数口径里）。
        guard_draw_result(hl, update_display_with(hl, &E0470_WAVEFORM, MODE_GL16));
    }
}

// 跟随 DU（8 帧，FAST 扫描）局部刷新一块区域；攒够 FOLLOW_GC16_EVERY 次后
// 整屏 GC16 清残影（打字期间只记账，见 s_gc16_pending）。返回 true = 顺手做了
// 那次整屏 GC16（调用方就不必再补清输入法那两行了，整屏 GC16 本来就全像素）。
static bool follow_du_refresh(EpdiyHighlevelState *hl, uint8_t *cur, EpdRect r) {
    copy_to_front(hl, cur);
    guard_draw_result(hl, update_display_area_with(hl, &E0470_FOLLOW_WAVEFORM, MODE_DU, r));
    if (++s_follow_partials >= FOLLOW_GC16_EVERY) {
        s_follow_partials = 0;
        if (s_local_only.load(std::memory_order_relaxed) || s_fast_partial.load(std::memory_order_relaxed)) {
            // 正在打字（VK 局刷 / 实体键快刷）：只记账，不刷。404ms 的整屏 GC16
            // 会打断输入节奏，且它清的是"打字区之外"的残影，不值得打断用户。
            s_gc16_pending = true;
        } else {
            do_full_refresh(hl, cur, true);
            return true;
        }
    }
    return false;
}

// 丢掉合并窗口里扣住的那块缓冲（走别的推屏分支时调用）。
static void drop_defer() {
    s_ime_deferred = false;
    if (s_defer_idx >= 0) {
        release_buffer(s_defer_idx);
        s_defer_idx = -1;
    }
    s_defer_cur = nullptr;
    s_defer_cand = EpdRect{0, 0, 0, 0};
    s_defer_commit = false;
}

// 一帧的完整推屏决策。cur/rel_idx 是这一帧的内容与它占的缓冲（-1 = front_fb 直画）。
static void render_present(const UiJob &job, uint8_t *cur, int rel_idx) {
    s_diag_wake_us = esp_timer_get_time();   // 计时：到这里 = 队列唤醒开销（见 flush_deferred）
    // 先认领：本函数一返回，这块缓冲的内容就是"最新一帧"。ui_render_begin_overlay()
    // 靠它做叠加 —— 认领得早，叠加就不用等这次推屏做完。
    if (rel_idx >= 0) s_last_idx = rel_idx;
    EpdiyHighlevelState *hl = board_hl();

    // 白底参考帧纪律的第二消费点：阅读器退出前若在面板上留下了中灰（插图页/图片查看器），
    // 那笔账留在 display.c。退出阅读模式这一帧由 ui_invalidate_snapshot → s_force_full_next
    // 强制整屏 GC16（每个像素都重驱动一遍），本来就等价于"从已知态出下一屏"，所以这里
    // **只销账、不铺白** —— 若不销，这笔账会一直留到用户下次进阅读模式、且刚好走到差分
    // 档时才被消费，凭空多一次白闪。
    if (display_take_white_exit())
        ESP_LOGI(TAG, "灰度面板交回 UI：本帧强制整屏 GC16，无需再铺白");

    if (job.force_full || s_force_full_next) {
        if (uiPerfLogOn())
            ESP_LOGI(TAG, "整屏刷: 强制（job.force_full=%d 失效重刷=%d）",
                     (int)job.force_full, (int)s_force_full_next);
        s_force_full_next = false;
        drop_defer();
        display_soft_refresh_reset();   // 残影预算在 display.c（下面就是整屏 GC16，它也归零）
        s_follow_partials = 0;
        s_fast_body_n = 0;
        s_gc16_pending = false;
        do_full_refresh(hl, cur, true);   // 内部销掉输入法那两行的记账
        release_buffer(rel_idx);
        return;
    }

    // ── 句读那一拍：给"编码区+候选区"记一笔脏账，**不当场刷** ──────────────
    //
    // 敲完 ，。！？ 之后输入法条自己就会变空（组合结束、候选行清掉），这一刻用户正停在
    // 这儿组织下一句，是清这两行的好时机。但**不能在这一拍把这两行并进刷新区域当场清**：
    //
    //   这一拍正文里刚生出来的那个标点也要写出去，而**差分矩形是"光标这一行往下那一整段"**
    //   ——段落后面的正文整体重排了。把它和整幅宽的输入法条并成一个矩形，清残影的区域就
    //   成了"光标以下直到状态栏的一大块"（实测：差分 282,104 530x572 ∪ 输入法条
    //   0,505 1216x128 → 并出 0,104 1216x572），GC16 一走整块黑白闪一次。用户看到的
    //   就是"清的何止编码候选那两行，整块正文都闪了"。
    //   而且一帧只能调一次推屏（区域不省时间，两区两波形就是两遍扫描、时间相加），
    //   真想"当场只清这两行"就得再来一遍 ≈330ms 的 GC16，每个句读卡一下，更难受。
    //
    // 所以和上屏那一拍走完全一样的一条路：只记账。note_ime_clean 置脏 + 把清理期限推到
    // 最后一次输入法动作之后 IME_CLEAN_PAUSE_US，由 ime_clean_tick 用区域 GC16 清这两行。
    // 句读是天然的停顿点（用户在组织下一句），期限一到就清；真连着往下打，就和别的残影
    // 一起攒到下一次停顿 —— 这正是"按打字停顿清"这条既定策略。
    // 这一拍的正文与候选照常由下面那条差分路写出去（区域 = d，本来就把清空的候选行盖着）。
    // **句读现在是默认档**（设置项 ime_clean 的 "punct"）：只在上屏那拍也记账（"both"）时
    // 才会出现"一句清两遍"。
    if (job.ime_punct) note_ime_clean(job);

    // 差分基准 = 面板上实际的内容（epdiy 的 back_fb）。
    //
    // **快路：core0 申报"输入法面板顶线以上一个像素都没动"时，只扫线以下那一段。**
    // 这一条不是"猜"，是**结构性**的：任何差分够到面板顶线以上的帧都不满足下面的推迟条件
    // （`d.y >= job.ime_top`），会当场驱动、不进合并窗口，也就不会被丢弃；而被丢弃的帧
    // 里留下的、没驱动的差异，只可能在面板顶线**以下**（输入法条本身 + 它下面的状态栏）。
    // 所以"线以上是空的"对推迟/丢弃帧恒成立。
    //
    // 于是这里只需要：
    //   * 扫 [ime_top, 屏底) —— 输入法条 + 状态栏那 26%（~179/684 行 → 约 3ms，不是 28ms）；
    //   * 每 HINT_CHECK_EVERY 拍（第 1 拍也查）把 [0, ime_top) 那段也扫一遍核对申报，
    //     线以上真有差分就告警 + 快路停 HINT_CHECK_EVERY 拍，并把那段并进本次驱动矩形
    //     （这一拍照旧干净）。7/8 × (28−3)ms ≈ 22ms 是净收益。
    if (s_hint_mistrust > 0) s_hint_mistrust--;
    EpdRect d;
    // 本帧差分里落在"正文带"（新输入文字所在的那几行，见 diff_bounding_rect_rows2 的
    // band 参数）里的那一块。**只给残影账用**（note_body_band），不参与"推哪一块"。
    // 快路（hinted）那一支不填 —— 那一支成立的前提就是"正文一个像素都没动"。
    EpdRect body_d = {0, 0, 0, 0};
    const bool hinted = job.above_clean && job.ime_top >= 0 && job.ime_top <= SCREEN_H &&
                        s_hint_mistrust == 0;
    if (hinted) {
        d = diff_bounding_rect_rows(cur, hl->back_fb, job.ime_top, -1);
        if ((s_hint_n++ % HINT_CHECK_EVERY) == 0) {
            const EpdRect above = diff_bounding_rect_rows(cur, hl->back_fb, 0, job.ime_top);
            if (above.width > 0 && above.height > 0) {
                ESP_LOGW(TAG,
                         "输入法条申报失效：面板顶线 %d 以上仍有差分 %d,%d %dx%d（申报说没有）"
                         " —— 快路停 %d 拍",
                         job.ime_top, above.x, above.y, above.width, above.height,
                         HINT_CHECK_EVERY);
                s_hint_mistrust = HINT_CHECK_EVERY;
                d = rect_union(d, above);   // 这一拍照旧要画对
            }
        }
    } else {
        // **一趟扫两个框**：d = 本帧变了的那块（推哪一块），body_d = 其中落在"正文带"
        // 里的那一块（清残影时要含进来的，见 note_body_band）。带上沿取 0 —— 正文首行
        // 之上只有提示词那几行（进入编辑器后恒不变），把标识放进框里的代价只是偶尔多盖
        // 两行字；下沿取**输入法条上沿**（= drawEditor 的正文底边，见 editorBodyBottomY：
        // 那里就是 imeBarPanelTopY），正文永远在它上面，状态栏在它下面（不算正文）。
        // 没在组合时 ime_top 是 -1、cand 可能是空的，那就退回整屏当带（下面的记账有
        // cand.width > 0 这道闸，空带不会真的记进去）。
        const int band_bottom = (job.ime_top >= 0) ? job.ime_top
                                                   : (job.cand.y > 0 ? job.cand.y : SCREEN_H);
        diff_bounding_rect_rows2(cur, hl->back_fb, 0, -1, 0, band_bottom, &d, &body_d);
    }
    s_diag_diff_us = esp_timer_get_time();   // 计时：到这里 = 整屏差分开销（见 flush_deferred）
    if (d.width <= 0 || d.height <= 0) {
        // 无变化：墨水屏双稳态，不刷。
        release_buffer(rel_idx);
        return;
    }

    // （句读那一拍在这里不再单独收口，见上面 note_ime_clean 那段：它和上屏那一拍一样
    //   只记账，真正那次区域 GC16 由 ime_clean_tick 在打字停顿时做。）

    // 1) 变化全落在 IME 面板（编码行 + 候选行 + 底栏）内 → 合并窗口。
    //    窗口内的连续输入只推最后一次，避免每键一次 8 帧跟随 DU 排队。
    //    这块缓冲要扣到窗口到期（flush 时还要用它的内容），期间不还。
    if (job.ime_top >= 0 && d.y >= job.ime_top && rel_idx >= 0) {
        drop_defer();
        s_defer_idx = rel_idx;
        s_defer_cur = cur;
        s_defer_rect = d;
        s_defer_cand = job.cand;      // flush 时顺手把这一帧的候选行一起写出去
        s_defer_stamp_us = job.stamp_us;   // 只为计时（见 UiJob.stamp_us）
        // 上屏那一拍 flush 时把区域扩到候选区、换 8 灰阶正文表。**这不是清残影**
        // （GL16 白→白不驱动，见 display.c），清残影一律走 ime_clean_tick 的区域 GC16。
        s_defer_commit = job.ime_commit;
        ime_clean_arm(IME_CLEAN_PAUSE_US);   // 输入法条动过一下 = 用户还在打字，清理期限往后推
        if (!s_ime_deferred) {
            s_ime_deferred = true;
            s_ime_defer_until = esp_timer_get_time() + IME_DEFER_US;
        }
        return;
    }

    drop_defer();  // 非 IME 条变化：覆盖任何 pending 的局刷

    // 打字状态已结束（实体键/VK 都收起），而打字期间攒下过一次没做的清残影：
    // 这次提交无论多小都整屏 GC16 一次补上，并把两个计数归零重新攒。
    if (s_gc16_pending && !s_local_only.load(std::memory_order_relaxed) &&
        !s_fast_partial.load(std::memory_order_relaxed)) {
        s_gc16_pending = false;
        display_soft_refresh_reset();   // 残影预算在 display.c（下面就是整屏 GC16，它也归零）
        s_follow_partials = 0;
        s_fast_body_n = 0;
        if (uiPerfLogOn())
            ESP_LOGI(TAG, "整屏刷: 打字结束后补账（local_only=%d fast_partial=%d）",
                     (int)s_local_only.load(std::memory_order_relaxed),
                     (int)s_fast_partial.load(std::memory_order_relaxed));
        do_full_refresh(hl, cur, true);
        release_buffer(rel_idx);
        return;
    }

    // 上屏后**编码区+候选区这两行**要单独过一遍清残影刷，但**不当场做**：
    // 只把矩形和"脏了"记下来，等打字停顿（IME_CLEAN_PAUSE_US 内没有新的输入法动作）
    // 由 ime_clean_tick 清一次。当场清的话每个上屏词就是 300 多毫秒的卡顿。
    // 代价仍然是"多一遍扫描"，不是把整屏升级成全刷 —— 那样正文也会被驱动一遍，
    // 每上屏一次闪一屏。走整屏 GC16 / 整屏 GL16 的那几条路不必补（它们本来就是全像素，
    // 由 do_full_refresh 里的 ime_clean_forget 销账）。
    //
    // **要不要记这一笔由设置项 ime_clean 定**（job.ime_clean_commit，见 ime_clean_policy）：
    // 默认只记句读那一拍 —— 两者都记的话，一句话里上屏清一遍、敲完标点又清一遍，用户
    // 看得见"刷了 2 次"。
    //
    // **虚拟键盘那条路以前不推迟**（当时以为"上屏那一拍并进同一次区域刷"就等于清过了）：
    // 那次并进去的刷是 8 灰阶正文表 + GL16，而 GL16 在白底上**没有驱动**（见 display.c
    // 的 update_display_area_clean），等于没清 —— 现在它也和别人一样记账、等停顿走 GC16。
    // 那一拍的渲染仍然用 8 灰阶表（正文是黑白像素，少 7 个相的梯子看不错、省 80ms），
    // 只是它不再兼任"清残影"。
    // ── 这一拍要不要给残影记一笔账 ────────────────────────────────────────
    // **完全按设置项 ime_clean 的字面意思**（见 ime_clean_policy 那张表）：
    //   punct（句读后）→ 只有句读那一拍（job.ime_punct 已在组装点按策略过滤过）
    //   commit（上屏后）→ 只有上屏那一拍（job.ime_commit 是"这一帧之前刚上屏过一次"）
    //   count（计数清）→ 不挑事件，攒够 FAST_SETTLE_EVERY 拍记一次
    //   never（从不清）→ 一次都不记
    // **计数兜底只挂在"计数清"这一档**：它原来（除从不清外）对所有档都生效，于是选了
    // "句读后"的人在没有句读的时候也会被清一次 —— 用户侧正是"我一个标点都没打，怎么
    // 打到一半又闪了""这条规则怎么对所有策略都生效"（2026-10-09 报的）。
    // 攒拍的那个计数器只在计数清档推进（别的档根本不看它，省得它带着旧值跨档），而且
    // **只在打字那三条路上推**（typing=true）：计数清的口径是"打字拍数"，菜单那种一按
    // 一拍的界面不归它管（那些由 display.c 的预算与进出界面的整屏刷负责）。
    const auto ime_note_due = [&](bool typing) -> bool {
        if (job.ime_no_clean) return false;
        if (job.ime_punct) return true;
        if (job.ime_commit && job.ime_clean_commit) return true;
        if (typing && job.ime_count && ++s_fast_body_n >= FAST_SETTLE_EVERY) {
            s_fast_body_n = 0;
            return true;
        }
        return false;
    };
    // 记账（推迟到打字停顿，见 ime_clean_tick）。调用方已经判过"这一拍该记"。
    const auto clean_ime_rows = [&](const EpdRect *extra = nullptr) {
        if (rel_idx < 0) {
            // 降级路径（渲染任务没起来，core0 自己同步推）：没有空闲节拍帮我们补，
            // 只能当场清。extra 也要并进去 —— 快档下那是发灰的正文所在的地方。
            EpdRect r = extra ? rect_union(job.cand, *extra) : job.cand;
            if (r.width <= 0 || r.height <= 0) return;
            guard_draw_result(hl, update_display_area_clean(hl, r));
            return;
        }
        // extra 非空 = 快档那条路：除了输入法那两行，还要把跟随表欠推的**正文**一起坐实
        // （跟随表推力只有阈值表的 1/4，且 back_fb 已经认成目标灰阶、再拍也补不动，
        // 只有 GC16 能坐实）—— 一笔记两处，不为正文多清一遍。
        note_ime_clean(job, extra);
    };

    if (s_fast_partial.load(std::memory_order_relaxed) &&
        !s_fast_partial_first.load(std::memory_order_relaxed)) {
        // 2) 编辑器实体键快刷：DU 差分，只驱动本帧真正变化的像素。
        //    **这一路（两条刷法都一样）不碰 display.c 那份共享残影预算** —— 它只吃
        //    ime_clean 那笔账。原来稳档走整页 update_display_with 会把每一拍记进预算、
        //    攒够 APP_GC16_EVERY 就被升成整屏 GC16（打字几秒一次全屏黑白闪），
        //    2026-10-09 改走 update_display_typing_du（同一条刷法，不记账）。
        // 跟随表的欠账要用"停顿那次区域 GC16"坐实，而区域 GC16 的相位数是固定的
        // —— **那一下闪多大，全看账有多大**。所以这一拍能不能走快档，不看这一帧的差分，
        // 而看**它会把那次清残影撑到多大**：把这一帧的差分、已有欠账、以及输入法那两行
        // 并起来（那两行一定会被并进去，见 ime_clean_tick），超过 1/4 屏就退回稳档
        // —— 整页阈值 DU，墨实、不留账。1/4 屏（≈208KB）≈ 满宽 170 行：底栏那两行
        // （1216x128）在里面，再加正文几行也还在里面；一整页的差分一定在外面。
        //
        // 两次实测（2026-10-09，用户报"大清屏"）：
        //   /tmp/bigflash1.log  滚动那一拍差分 0,8 1216x670 → 欠账=整页 → 清残影 0,8 1216x670
        //   /tmp/bigflash2.log  只按帧差分判（当时的规则）时，370x450（166KB，20% 屏）
        //                       这一档溜了进来 → 欠账 450,227 370x450 → 清残影 0,227 1216x450
        // 两次都是"账撑大了、坐实它的那一下跟着变大"。按并起来的大小判，两条一起堵上。
        // 附带好处：整页的区域 GC16 会把 18MHz 打回 12MHz（PSRAM 抢总线 → line buffer
        // underrun，日志里 `line queue underrun` 就是它），账小了这种大活也少了。
        EpdRect blink = rect_union(job.cand, d);
        if (s_body_dirty_any) blink = rect_union(blink, s_body_dirty);
        const bool blink_ok =
            (long long)blink.width * blink.height <= (long long)SCREEN_W * SCREEN_H / 4;
        if (job.ime_commit_fast && blink_ok) {
            // 2′) "上屏刷法=快"：推**差分矩形** + 跟随 DU（8 相 @FAST ≈56ms），
            //     而不是整屏阈值 DU 的 20 相 @FULL ≈220ms。跟随表推力只有阈值表的 1/4，
            //     欠下的那股推力（发灰的正文 + 输入法那两行的残影）由第二拍还：
            //     ime_clean_tick 在**打字停顿**处做一次区域 GC16，把这两处一起坐实
            //     —— 一次清残影管两处，不额外多清一遍。**什么时候记这笔账由设置项定**：
            //
            //       punct（句读后，默认）→ 每个句读都记：那是用户天然停手组织下一句的时刻。
            //       count（计数清）      → 不按事件记，只靠计数兜底：攒够 FAST_SETTLE_EVERY
            //                              拍记一次（欠账是快档自己生的，不记就会一直脏）。
            //       commit（上屏后）     → 快档下与计数清同一条路（本档的账本来就是按拍
            //                              攒的，没有"单拍记账"这回事）；稳档才是一拍一记。
            //       never（从不清）      → 连计数兜底都没有，全程只快刷。
            //
            //     **句读那一拍以前是当场清的**（"用户要的就是即时干净"）：那正是
            //     "打字途中有时候也会闪"的来源 —— 当场清要并上这一帧的差分 d，而 d 天生
            //     是"改动行 → 屏底"（段落后面的正文整体重排 + 底栏字数，见上面 :518 那段），
            //     并出来就是整屏：实测 0,9 1216x669，一次 377ms 的整屏 GC16，每敲一个标点
            //     黑闪一次。2026-10-09 起改成与别的拍同一条路（只记账），账交给停顿那次清
            //     —— 那时候用户本来就在停手看结果，同样的钱花在不用等响应的一拍上。
            //
            //     **记账只做两件事**（note_ime_clean）：把矩形并进 s_clean_rect、把清理
            //     期限推到最后一次输入法动作之后（ime_clean_arm）。期限钉在"最后一次动作"
            //     上，连打中间才不会插进一次 330ms 的刷屏。
            //
            //     **攒的必须是并集，不能只记这一拍的 d**：差分是拿 back_fb 比的（增量），
            //     还账时若只清最后一拍的增量，前几拍推过又抹掉的位置（旧字残影）
            //     就永远没人管。所以见 s_body_dirty 那条注释。
            //
            //     **这里刻意不用 follow_du_refresh**：那个函数除了推屏，还给这批跟随
            //     推送记一份"攒够 8 次就来一发整屏 GC16"的账（跟随表的推送 display.c
            //     自己不计数，见那边的注释）。可这些推送的坐实已经由第二拍负责了，
            //     两份账叠在一起 = 同一个停顿里闪两遍全屏。
            copy_to_front(hl, cur);
            guard_draw_result(hl,
                              update_display_area_with(hl, &E0470_FOLLOW_WAVEFORM, MODE_DU, d));
            s_body_dirty = s_body_dirty_any ? rect_union(s_body_dirty, d) : d;
            s_body_dirty_any = true;
            // 这一行是"快档到底在攒什么"的原始记录（每拍一行，只在性能日志开着时打）：
            // 调那个 1/4 屏的上限时就靠它看典型差分有多大。
            if (uiPerfLogOn())
                ESP_LOGI(TAG, "快档推: 差分 %d,%d %dx%d 欠账 %d,%d %dx%d（并起来 %d,%d %dx%d）",
                         d.x, d.y, d.width, d.height,
                         s_body_dirty.x, s_body_dirty.y, s_body_dirty.width, s_body_dirty.height,
                         blink.x, blink.y, blink.width, blink.height);
            // 这笔账什么时候交给停顿那次清（见 s_clean_dirty / ime_clean_tick）：按设置项的
            // 字面意思来 —— 句读后只认句读、上屏后只认上屏、计数清按拍数攒、从不清一次
            // 都不记（判据在 ime_note_due）。**记账一定带上 s_body_dirty**：快档欠下的那股
            // 推力在正文上，只记输入法那两行的话它永远发灰。
            if (ime_note_due(/*typing=*/true)) clean_ime_rows(&s_body_dirty);
        } else {
            // 3′) "上屏刷法=稳"：整屏阈值 DU（墨实，每拍约 220ms；MODE_DU 只驱动本帧真正
            //     变化的像素，整页只是驱动范围）。
            //     **这条走 update_display_typing_du，不记 display.c 那份共享残影预算**：
            //     照常走 update_display_with 的话，每按一次键就替预算 +1，攒够
            //     APP_GC16_EVERY(=14) 这一拍被整个升成整屏 GC16 —— 打字一秒三四拍，
            //     几秒钟就"全屏黑白闪一下"（按方向键更明显：按十几下就来一次），而且
            //     与 ime_clean 四档无关。那份预算的尺度是给菜单、翻页那类低频整页刷定的。
            // 快档退回来的那一拍打一行：看日志时能分清"这一拍为什么慢了 160ms、为什么
            // 没记那笔账"，以及是"这一帧自己就大"还是"并上旧账之后才大"。
            if (job.ime_commit_fast && uiPerfLogOn())
                ESP_LOGI(TAG, "快档退稳档: 差分 %d,%d %dx%d + 旧欠账 %d,%d %dx%d = %d,%d %dx%d"
                              "（%lldKB，超 1/4 屏的 %lldKB）",
                         d.x, d.y, d.width, d.height,
                         s_body_dirty_any ? s_body_dirty.x : 0, s_body_dirty_any ? s_body_dirty.y : 0,
                         s_body_dirty_any ? s_body_dirty.width : 0,
                         s_body_dirty_any ? s_body_dirty.height : 0,
                         blink.x, blink.y, blink.width, blink.height,
                         (long long)blink.width * blink.height / 1024,
                         (long long)SCREEN_W * SCREEN_H / 4 / 1024);
            copy_to_front(hl, cur);
            guard_draw_result(hl, update_display_typing_du(hl));
            // 稳档推的是阈值表、这一拍的墨是实的，但**残影**从这一拍起攒在正文上了：
            // 把"刚敲进去那几个字"那块并进账，等停顿那次清残影和输入法那两行一起清。
            // 不并的话，用户侧看到的就是"每次只清候选区编码区，正文一路没人管"
            // （2026-10-10 报的 —— 账里永远只有 0,505 1216x128）。
            if (!job.ime_no_clean) note_body_band(body_d);
            // 这一笔账记的是 job.cand（输入法那两行）+ 上面刚并进来的正文欠账。
            if (ime_note_due(/*typing=*/true)) clean_ime_rows(&s_body_dirty);
        }
    } else {
        if (s_fast_partial_first.exchange(false, std::memory_order_relaxed)) {
            // 写作模式这一拍是**新一次打字会话的第一帧**（刚进编辑器 / 蓝牙键盘刚连上并
            // 收起虚拟键盘）。记账是"打字会话内"的概念，**不从别的界面继承**：
            // 用户在设置里翻列表、在书架里翻页攒下的那点账，不该在进编辑器的第一帧上
            // 兑现 —— 那一帧是整屏刷，账一到点就会把它升成整屏 GC16（用户看到的是
            // "一进日记就黑闪一下"，他 2026-10-09 的原话："感觉是不是和从主界面进入
            // 日记功能之前的计数有关"，就是这一条）。这里归零的四个都是"上一条会话的
            // 进度"：display.c 那份共享残影预算、面板跟随 DU 的拍数、打字期间攒下的
            // 待补账、以及快档计数兜底攒的拍数。归零本身不刷屏，下面的整屏刷照走。
            if (uiPerfLogOn())
                ESP_LOGI(TAG, "记账: 写作会话第一帧，清掉继承的账（跟随拍=%d 待补=%d 预算到点=%d）",
                         s_follow_partials, (int)s_gc16_pending, (int)display_soft_refresh_due());
            display_soft_refresh_reset();
            s_follow_partials = 0;
            s_gc16_pending = false;
            s_fast_body_n = 0;
        }
        // 3) 局刷判定。虚拟键盘打字（s_local_only）时只可能是"正文区也在变"：差分顶边
        //    落在输入法区之上的那几拍（打字本身只动输入法区，上面就拦下走合并窗口了）。
        //    用 GL16 局刷，画质优先（区域只限制驱动范围，不省时间 —— 见文件头）。
        //    其余界面（设置项选中、单行高亮等）沿用原来的"小变化局刷、大半屏整屏"。
        bool small = d.height <= SCREEN_H / 2;
        if (s_local_only.load(std::memory_order_relaxed)) {
            // 虚拟键盘打字：**一律局刷，绝不整屏**。打字中途差分只落在候选条那一小条，
            // 而上屏那一拍从正文一路跨到候选条，高度常超半屏 —— 再按 small 判就会掉进
            // 整屏 GL16（≈410ms），每上屏一次闪一屏。区域只限制驱动范围、画质优先。
            //
            // 上屏那一拍（IME::commit 刚记过一次）把区域扩到编码区候选区、换 8 灰阶
            // 正文表过一遍（省 80ms，正文是黑白像素看不出来）。注意**这不是清残影** ——
            // GL16 在白底上不驱动，清残影走下面 clean_ime_rows() 的停顿 GC16。
            EpdRect r = d;
            const bool commit = job.ime_commit && job.cand.width > 0 && job.cand.height > 0;
            if (commit) r = rect_union(r, job.cand);
            copy_to_front(hl, cur);
            guard_draw_result(hl, update_display_area_with(
                hl, commit ? &E0470_GRAY8_TEXT_WAVEFORM : BODY_PARTIAL_WAVEFORM,
                BODY_PARTIAL_MODE, r));
            // 局刷大块打一行：这条 arm 用的是 GL16**全像素**（区域内每个像素都过一遍灰阶
            // 梯子），一块一大屏发灰 —— 用户嘴里的"闪一下"有一半是它，不是 GC16。
            // 记下来才能分清"闪"是哪一种（整屏 GC16 = 黑闪 / 区域 GL16 大块 = 发灰的闪）。
            if (uiPerfLogOn() && (long long)r.width * r.height > (long long)SCREEN_W * SCREEN_H / 4)
                ESP_LOGI(TAG, "区域刷: 局刷大块 %d,%d %dx%d（菜单=%d 并上候选区=%d）",
                         r.x, r.y, r.width, r.height,
                         (int)s_menu_only.load(std::memory_order_relaxed), (int)commit);
            // 账只记"编码候选那两行"（正文走 GL16 局刷，不发灰）；是否记由 ime_note_due 按
            // 设置项定 —— 上屏后那一拍在下面 clean_ime_rows 里认的也是这个 jod.ime_commit。
            // **菜单型界面不记**（见 ui_render_set_menu_only）：这条 arm 是给打字用的，
            // 菜单借它走只是要"一律局刷"，翻列表的拍数不该记成打字账。
            if (!s_menu_only.load(std::memory_order_relaxed) && ime_note_due(/*typing=*/true))
                clean_ime_rows();
        } else if (small) {
            const bool ok = follow_du_refresh(hl, cur, d);
            // 打字会话（输入法条在场）里这一拍也是跟随表推的 —— 推力只有阈值表的 1/4，
            // 刚敲进去那几个字先发灰，要等停顿那次区域 GC16 坐实。所以正文那块也得记账
            // （见 note_body_band）。菜单/列表类界面不记：它们的局刷是 GL16 全像素，天生
            // 不自攒残影（ui_render_set_menu_only）。
            if (job.cand.width > 0 && !job.ime_no_clean &&
                !s_menu_only.load(std::memory_order_relaxed))
                note_body_band(body_d);
            if (!ok && ime_note_due(/*typing=*/false)) clean_ime_rows(&s_body_dirty);
        } else {
            if (uiPerfLogOn())
                ESP_LOGI(TAG, "整屏刷: 差分高过半个屏 %d,%d %dx%d（快路没开：local_only=%d "
                              "fast_partial=%d 菜单=%d）",
                         d.x, d.y, d.width, d.height,
                         (int)s_local_only.load(std::memory_order_relaxed),
                         (int)s_fast_partial.load(std::memory_order_relaxed),
                         (int)s_menu_only.load(std::memory_order_relaxed));
            do_full_refresh(hl, cur, false);
        }
    }
    release_buffer(rel_idx);
}

// 合并窗口到期：把扣住的那块缓冲推出去。
//
// 平时走 8 相跟随 DU（FAST 扫描 ≈89ms，只驱动变化矩形）。**打字中途不再有任何全刷**
// —— 以前每 VK_CLEAN_EVERY 键把整个键盘矩形过一遍 8 灰阶 GL16（30 相 ≈330ms）压键帽
// 残影，代价是打字打到一半闪一下键盘，规律得能数出来。
//
// 现在清残影只在**上屏那一拍**做，而且只清**编码区候选区**（不是整个键盘）：那一刻
// 用户刚停手看结果，候选栏从"一排候选"变成"上屏后的新一排"，正是残影要处理的时候。
// 用 8 灰阶正文表（带一帧 15→15 白推）全像素过一遍那块矩形 —— GL16 在这里是 *_full，
// 区域内每一行都被驱动到目标灰阶，压得干净；区域之外的正文一个像素都不动，不闪。
//
// 没弹虚拟键盘时（"输入法条"形态）cand 就是 drawIMEUI 上报的那块面板（编码行+候选行），
// 与虚拟键盘那条路清的是同一件事——实体键盘打字留下的残影一样攒在这里。
static void flush_deferred() {
    if (!s_ime_deferred) return;
    s_ime_deferred = false;
    const int idx = s_defer_idx;
    uint8_t *cur = s_defer_cur;
    s_defer_idx = -1;
    s_defer_cur = nullptr;
    const EpdRect cand = s_defer_cand;
    const bool commit = s_defer_commit;
    s_defer_cand = EpdRect{0, 0, 0, 0};
    s_defer_commit = false;
    if (idx < 0 || !cur) return;
    EpdiyHighlevelState *hl = board_hl();
    copy_to_front(hl, cur);

    const int64_t t0 = esp_timer_get_time();
    // 「提交→推屏」三段拆账（只为计时）：排队 = 队列唤醒（渲染任务何时被调度到），
    // 差分 = diff_bounding_rect（整屏 831KB 比对），拷贝 = copy_to_front（406KB memcpy）。
    // 三段都是 core0 提交之后、面板开始驱动之前花的钱。
    const long long q_ms = (long long)((s_diag_wake_us - s_defer_stamp_us) / 1000);
    const long long diff_ms = (long long)((s_diag_diff_us - s_diag_wake_us) / 1000);
    const long long copy_ms = (long long)((t0 - s_diag_diff_us) / 1000);
    if (commit && cand.width > 0 && cand.height > 0) {
        const EpdRect r = rect_union(s_defer_rect, cand);
        guard_draw_result(hl, update_display_area_with(hl, &E0470_GRAY8_TEXT_WAVEFORM, MODE_GL16, r));
        if (uiPerfLogOn())
            ESP_LOGI(TAG, "打字帧: 上屏 %d,%d %dx%d 刷屏 %lldms（提交→推屏 %lld = 排队 %lld + 差分 %lld + 拷贝 %lld）",
                     r.x, r.y, r.width, r.height, (long long)((esp_timer_get_time() - t0) / 1000),
                     (long long)((t0 - s_defer_stamp_us) / 1000), q_ms, diff_ms, copy_ms);
    } else {
        guard_draw_result(hl, update_display_area_with(hl, &E0470_FOLLOW_WAVEFORM, MODE_DU, s_defer_rect));
        // "提交→推屏"就是队列 + 差分开销。刷屏那一截是面板的物理时间（8 相 @FAST），
        // 和区域大小无关；要判断"还能不能快"看的是**提交之前**那段（见编辑器的「打字耗时」）。
        if (uiPerfLogOn())
            ESP_LOGI(TAG, "打字帧: 输入法条 %d,%d %dx%d 刷屏 %lldms（提交→推屏 %lld = 排队 %lld + 差分 %lld + 拷贝 %lld）",
                     s_defer_rect.x, s_defer_rect.y, s_defer_rect.width, s_defer_rect.height,
                     (long long)((esp_timer_get_time() - t0) / 1000),
                     (long long)((t0 - s_defer_stamp_us) / 1000), q_ms, diff_ms, copy_ms);
    }
    // 菜单型界面不记账（见 ui_render_set_menu_only）：设置页的输入法条也会走到这条 flush，
    // 但那种屏攒出来的账不留给后面的界面 —— 它自己永远等不到"两个开关都关"的那一拍，
    // 只会跟着用户走到别的界面才炸成一次整屏 GC16。
    if (!s_menu_only.load(std::memory_order_relaxed) &&
        ++s_follow_partials >= FOLLOW_GC16_EVERY) {
        s_follow_partials = 0;
        if (s_local_only.load(std::memory_order_relaxed) || s_fast_partial.load(std::memory_order_relaxed)) {
            s_gc16_pending = true;
            if (uiPerfLogOn())
                ESP_LOGI(TAG, "残影预算: 面板跟随 DU 攒够 %d 拍 → 记账，等打字结束再补",
                         FOLLOW_GC16_EVERY);
        } else {
            do_full_refresh(hl, cur, true);
        }
    }
    release_buffer(idx);
}

// ── 阅读模式的虚拟键盘打字帧 ─────────────────────────────────────────────
// 阅读器（screen_reader.cpp）虚拟键盘的每一帧原来走 HalDisplay::displayBuffer(HALF)：
// 整屏 GL16，而 display.c 的 hl_update() 有一条"GL16 恒全像素"的规矩 —— 于是**每按
// 一个键整屏全像素驱动一遍**（≈410ms，还整屏闪）。用户侧就是"按一个按键就全刷一次"。
// 写作模式的虚拟键盘不是这样：它按"变化落在哪儿"挑区域与波形（见上面 render_present
// 的第 1/3 两条路）。这里把那套判据原样搬给阅读器 —— 阅读器整条绘制路径绕过渲染任务
// （直画 front_fb、自己同步推屏），所以这条不能复用 render_present，只能把决策重走一遍。
//
// 判据（与 render_present 一一对应）：
//   * 变化全在键盘面板内（编码行、候选行、键帽按下反色）→ 跟随表 DU 只推**差异矩形**
//     （8 相 ≈89ms，差分：只有真变了的像素被驱动，屏上其余部分一个像素都不动）。
//     写作模式那条路是同一个 FOLLOW DU，只多一个 5ms 合并窗口 —— 阅读器是手指点按，
//     两次之间本来就有几百毫秒，合并没有意义。
//   * 变化越出面板顶边（正文/文本输入区也变了 = 一次上屏）→ 8 灰阶正文表在
//     "差异矩形 ∪ 编码候选两行"上过一遍。区域扩到候选两行是为了把**这一拍换掉的那整排
//     候选**一次写清楚（区域只限制驱动范围，不额外花时间），**不是为了清残影** ——
//     这张表在白底上不驱动，清残影得用区域 GC16，见下面那段注释。
//   （原来还有第三条：面板局刷攒够 20 次就把这一拍换成"面板整块的 8 灰阶正文表"，
//   想用来清键帽反色攒下的灰痕。**2026-10-05 删掉** —— 那张表在"上一拍已经是白"的
//   像素上根本不驱动（GL16 全保持，只补一帧白推，见 display.c 的
//   update_display_area_clean），等于每 20 个面板帧白花一次 30 相 ≈330ms 的卡顿，
//   什么都没清掉。阅读器的 VK 帧不在渲染任务里，用不了 ime_clean_tick 那套
//   "记账 + 空闲节拍"，要真清得先想清楚清哪儿、再另开一个记账口——就是下面这个。）
//
// ── 阅读器虚拟键盘的"上屏快档"欠账（上面那个记账口）─────────────────────────
// 设置项 ime_commit_mode = fast（"上屏刷法=快"）时，上屏那一拍的正文区改用跟随 DU 推：
// 8 相 @FAST ≈100ms，替掉 8 灰阶正文表 + GL16 的 30 相 ≈336ms。和编辑器实体键那条快档
// 是同一笔交易 —— **跟随表推力只有阈值表的 1/4**，刚上屏的字先发灰、旧字还留残影，
// 得靠"第二拍"坐实。编辑器那条路的第二拍由渲染任务按 ime_clean 的策略还账（见
// render_present 的 2′ 与 note_ime_clean）；阅读器整条路在 core0、不经渲染任务，
// 所以这里另开一份**不跨核**的账，**口径与编辑器那套一一对齐**（用户要求：
// "清残影策略虚拟键盘也应该跟随实体键盘的设定"）：
//
//   设置项 ime_clean     快档的欠账怎么记（都在停手后清）  稳档的编码候选两行
//   ────────────────────────────────────────────────────────────────────────────
//   punct（句读后，默认）只有句读那一拍记一笔                不动（稳档不做句读清）
//   commit（上屏后）     只有上屏那一拍记                    上屏那一拍记账 → 停手后清
//   count（计数清）      不挑事件，只按拍数攒（FAST_SETTLE_EVERY）  不动（稳档不记计数）
//   never（从不清）      一次都不记                          不动
//
// **四档都是字面意思**（2026-10-09 与编辑器一起改）：计数兜底原来除"从不清"外对所有档
// 都生效，于是选了"句读后"的人在没有句读时也会被清一次 —— 用户报的"我一个标点都没打，
// 怎么打到一半又闪了"。要兜底就选计数清。
//
// **句读那一档 2026-10-09 从"同步清"改成"记账"**（与编辑器同一个改动、同一个理由：
// 当场清要并上本帧的差分矩形，正文一重排就是一大块，每个标点黑闪一次）。
//
// 两笔账为什么要并集、不能只记最后一拍，同 s_body_dirty 那段：差分是拿 back_fb 比的
// （增量），只清最后一拍会漏掉前几拍推过又抹掉的位置（旧字残影）。
static EpdRect s_rvk_dirty = {0, 0, 0, 0};   // 被跟随表欠推过的区域（并集）
static bool    s_rvk_dirty_any = false;
static bool    s_rvk_owed = false;            // 有账要清：攒够了就等停手
static int64_t s_rvk_due_us = 0;              // 清账期限（每次打字往后推）
static uint8_t s_rvk_body_n = 0;              // 快档计数兜底（同编辑器的 s_fast_body_n）
static uint32_t s_rvk_seen_punct = 0;         // 句读认帧（同编辑器的 take_ime_punct）

// 只在 core0 调（reader_vk_present 这条路上）——读 g_settings 与渲染任务不碰它的规矩一致。
static bool rvk_fast_mode(void) { return g_settings.imeCommitMode() == "fast"; }

// 累计"被欠推过"的区域。**只记不问**：什么时候真清由 s_rvk_owed/期限定。
static void rvk_accum(EpdRect r) {
    s_rvk_dirty = s_rvk_dirty_any ? rect_union(s_rvk_dirty, r) : r;
    s_rvk_dirty_any = true;
}

// 记账：到停手（IME_CLEAN_PAUSE_US 内不再有输入法动作）就清一次当时累计的全部区域。
static void rvk_owe(void) {
    s_rvk_owed = true;
    s_rvk_due_us = esp_timer_get_time() + IME_CLEAN_PAUSE_US;
}

// 清完把账销干净。**不清 s_rvk_seen_punct** —— 那是"认帧"的状态，不是欠账。
static void rvk_clear_account(void) {
    s_rvk_owed = false;
    s_rvk_dirty_any = false;
    s_rvk_dirty = EpdRect{0, 0, 0, 0};
    s_rvk_body_n = 0;
}

// 句读认帧：IME::punctSeq 变了就是"这一拍刚吐出一个句读"。每次推屏都取走（不管走哪条
// 路），免得一段旧账留到后面的帧上被误判成本拍的句读。
static bool rvk_take_punct(void) {
    const uint32_t s = g_ime.punctSeq();
    if (s == s_rvk_seen_punct) return false;
    s_rvk_seen_punct = s;
    return true;
}

// 到点就把账坐实（区域 GC16，见 update_display_area_clean）。阅读器帧循环每轮调一次
// （空转帧也算 —— 那正是"用户停手了"的证据）。front_fb 就是屏上现在的内容（阅读器直画
// 它、自己同步推屏），所以这一次重推既坐实灰阶又把残影清掉，不需要重画。
void ui_render_reader_vk_settle_tick(void) {
    if (!s_rvk_owed || esp_timer_get_time() < s_rvk_due_us) return;
    // 改设置改到"从不清"档时，攒着的那笔账就作废 —— 那一档承诺这条路一次区域 GC16 都没有。
    if (ime_clean_policy() & IME_CLEAN_NO_GC16) { rvk_clear_account(); return; }
    EpdRect r = s_rvk_dirty;
    rvk_clear_account();
    EpdiyHighlevelState *hl = board_hl();
    if (!hl || r.width <= 0 || r.height <= 0) return;
    guard_draw_result(hl, update_display_area_clean(hl, r));
    if (uiPerfLogOn()) ESP_LOGI(TAG, "阅读器键盘: 欠账坐实 %d,%d %dx%d", r.x, r.y, r.width, r.height);
}

// 整屏全像素刷过一遍（进阅读器的 invalidate、翻页整屏全刷/换章）之后这块已经被驱动
// 干净了，账作废 —— 不然停顿时会再白闪一次同样的地块。顺手把句读认帧对齐到当下：
// 新的这一段从这里开始，别把别处（写作模式的实体键盘）敲过的句读带进来。
void ui_render_reader_vk_settle_forget(void) {
    rvk_clear_account();
    s_rvk_seen_punct = g_ime.punctSeq();
}

void reader_vk_present(int panel_top, int cand_h) {
    EpdiyHighlevelState *hl = board_hl();
    if (!hl) return;
    // 句读认帧：**任何一条路都取走**（面板帧、早退帧都算），这样一段旧账不会留到后面的
    // 帧上被误判成"本拍吐了个句读"。
    const bool punctFrame = rvk_take_punct();
    // 虚拟键盘还弹着 = 用户还在打字：把坐实期限往后推，别从连打中间插进来一次 336ms 的
    // 清残影（同渲染任务那边"落在输入法条内的帧也推期限"）。下面记账那一拍会重新钉期限。
    if (s_rvk_owed) s_rvk_due_us = esp_timer_get_time() + IME_CLEAN_PAUSE_US;
    // 差分基准是 epdiy 的 back_fb："上一次真正驱动到面板上的内容"。阅读器直画 front_fb，
    // 所以差异就是 front_fb vs back_fb（同 render_present）。
    EpdRect d = diff_bounding_rect(hl->front_fb, hl->back_fb);
    if (d.width <= 0 || d.height <= 0) return;   // 无变化：墨水屏双稳态，不刷
    // 每条路都打一行：区域 → 几十毫秒，整屏 → 四百毫秒。这是"按一个键还闪不闪"的
    // 唯一现场证据（帧数/耗时对不上时，先看这条日志选了哪条路、矩形多大）。
    const int64_t t0 = esp_timer_get_time();
    auto done = [&](const char *what, enum EpdDrawError err, EpdRect r) {
        guard_draw_result(hl, err);
        if (uiPerfLogOn()) {
            ESP_LOGI(TAG, "键盘帧: %s 区域 %d,%d %dx%d 刷屏 %lldms", what, r.x, r.y, r.width,
                     r.height, (long long)((esp_timer_get_time() - t0) / 1000));
        }
    };
    if (panel_top <= 0) {
        // 面板几何没拿到（调用方没把键盘顶边递下来）：退回整屏局刷，别拿 0 当分区线
        // —— 那会把整屏都判成"面板内"，区域刷就退化成一次整屏 DU，画质反而更差。
        done("面板几何缺失→整屏局刷", update_display_with(hl, &E0470_WAVEFORM, MODE_GL16),
             EpdRect{0, 0, SCREEN_W, SCREEN_H});
        return;
    }
    if (d.y >= panel_top) {
        // 只动键盘面板：快刷那一块。
        done("键盘面板快刷", update_display_area_with(hl, &E0470_FOLLOW_WAVEFORM, MODE_DU, d), d);
        return;
    }
    // 正文/文本输入区变了 = 一次上屏（或一次句读直通落进正文）。区域扩到盖住编码候选
    // 两行（同 render_present 的 commit 那一拍），把上屏那一拍整排换字的候选栏一次写清楚。
    EpdRect r = d;
    if (cand_h > 0) r = rect_union(r, EpdRect{0, panel_top, SCREEN_W, cand_h});
    const uint8_t pol = ime_clean_policy();
    if (rvk_fast_mode()) {
        // 上屏刷法=快：推差分矩形 + 跟随 DU（8 相 ≈100ms），欠下的推力（发灰的正文、
        // 旧字残影）记账，等停手由 ui_render_reader_vk_settle_tick 用一次区域 GC16 坐实。
        //
        // **句读那一拍以前是同步清的**（"敲完标点当场干净"）：这一帧本来就欠一次推屏，
        // 拿它来清确实不多花一帧，但矩形要并上这一帧的差分（正文重排 → 一大块），
        // 一次 ≈336ms 的 GC16 = 用户每个标点看见一次黑闪。2026-10-09 与编辑器那边
        // 一并改成"只记账、等停顿那次清"（那里实测矩形 0,9 1216x669 ≈ 整屏）。
        done("正文+候选快刷(跟随DU)",
             update_display_area_with(hl, &E0470_FOLLOW_WAVEFORM, MODE_DU, r), r);
        rvk_accum(r);
        // 记账的时机与编辑器逐条对齐（那张表就是"口径一一对齐"），而且是**字面意思**：
        //   句读后 → 只有句读那一拍（punctFrame）；上屏后 → 只有上屏那一拍（能走到这儿
        //   就说明正文变了 = 一次上屏）；计数清 → 只按拍数攒；从不清 → 一次都不记。
        // **计数只属于计数清档**：它原来（除从不清外）对所有档都生效，选了"句读后"的人
        // 在没有句读时也会被清一次 —— 用户报的"没打标点怎么打到一半又闪了"。
        if (!(pol & IME_CLEAN_NO_GC16)) {
            const bool by_punct = punctFrame && (pol & IME_CLEAN_ON_PUNCT);
            const bool by_commit = (pol & IME_CLEAN_ON_COMMIT) != 0;
            bool by_count = false;
            if (pol == 0 && ++s_rvk_body_n >= FAST_SETTLE_EVERY) {
                s_rvk_body_n = 0;
                by_count = true;
            }
            if (by_punct || by_commit || by_count) rvk_owe();
        }
        return;
    }
    // 稳档：正文与候选两行都是 GL16 局刷（画质优先）。上屏那一拍按设置记一笔**编码候选
    // 两行**的账 —— GL16 在白底上是全保持、不驱动（见 display.c），上屏前后那两行的旧字
    // 残影它擦不掉，只有停顿那次区域 GC16 能擦。这与编辑器虚拟键盘那条路同一条规矩
    // （render_present 第 3 条路里的 clean_ime_rows）。稳档不看句读那一档（用户拍板）——
    // 稳档的正文是阈值表推的，本来就不发灰，不需要为它记账。
    if ((pol & IME_CLEAN_ON_COMMIT) && cand_h > 0) {
        rvk_accum(EpdRect{0, panel_top, SCREEN_W, cand_h});
        rvk_owe();
    }
    done("正文+候选局刷", update_display_area_with(hl, &E0470_GRAY8_TEXT_WAVEFORM, MODE_GL16, r), r);
}

// ── 阅读器列表/菜单帧的局部推屏 ────────────────────────────────────────────
//
// 书架、目录、书签、笔记、设置那些界面一帧只动一小块：滚动的列表、跟着重画的
// 标题/页脚、走动的选中条。以前一律整屏 HALF 推过去，代价两笔 ——
//   1. 整屏 GL16 撞上 display.c「GL16 恒全像素」那条规则（hl_update 第一行），
//      一次滚动就是一次整屏全像素驱动，每个像素都过一遍 LUT；
//   2. 它记进**共享**残影预算（APP_GC16_EVERY，见 display.c 的 hl_update）——
//      于是连滚十几下就升一次整屏 GC16，用户侧正是"连按十几下整屏黑白闪一下"
//      （社区固件 rc55 也是为这条改的）。
//
// 改成只驱动本帧的**差分包围盒**：矩形拿 front_fb 与 back_fb 比出来
// （diff_bounding_rect，与上面 reader_vk_present 同一个——那边是这套做法的第一个
// 用户，实测好用），凡是这一帧改过的地方都在里面。好处是不需要任何渲染函数自己
// 申报区域，也就没有"某个界面忘了申报 → 那块永远是旧像素"这种错法：区域按定义
// 就是"这一帧到底改了哪儿"，不是"我以为我改了哪儿"。
//
// 区域刷**不进**共享预算（hl_update 只在 area == NULL 时记账）：这正是要的，滚动
// 不该吃掉翻页该得的清账机会。代价是这块地方得自己有人清，所以这里带自己的小预算：
// 攒够 LIST_GC16_EVERY 次区域刷，用一次**区域** GC16 把它坐实。为什么清残影必须是
// GC16 而不是 GL16，见 display.c update_display_area_clean 上面那段实测（白→白在
// GL16 表里是全保持，压不掉"现在白、上一拍也白"的墨痕）。与输入法那两行的
// ime_clean_tick 是同一条规矩、同一个原语，区别只是**不推迟**：滚动本来就是一格一格
// 看的，插一帧黑白摆动比打字中间插一帧好受得多，没有"停手"这个概念可等。
//
// 只给列表/菜单帧：正文翻页的整屏差分是残影预算的主来源，把它收窄等于把那套账拆了
// （判据在调用方 screen_reader.cpp，顺带也在那里挡掉面板还留着中灰的帧）。
// 自己的旋钮，刻意**不**复用 APP_GC16_EVERY：那个档有个 0 = 关闭的语义，跟着它走会
// 变成"关掉自动清残影 → 每一帧列表都当场清一次"（0 次就满足 >= 0）。数取 14 与它一致，
// 只是"同样的手感"，不是同一份账。
#define LIST_GC16_EVERY 14
static int s_list_n = 0;                  // 自上次坐实以来的区域刷次数
static EpdRect s_list_dirty = {0, 0, 0, 0};
static bool s_list_dirty_any = false;

void reader_list_present(void) {
    EpdiyHighlevelState *hl = board_hl();
    if (!hl) return;
    // 差分基准是 epdiy 的 back_fb："上一次真正驱动到面板上的内容"。阅读器直画 front_fb，
    // 所以差异就是 front_fb vs back_fb（同 reader_vk_present）。
    EpdRect d = diff_bounding_rect(hl->front_fb, hl->back_fb);
    if (d.width <= 0 || d.height <= 0) return;   // 无变化：墨水屏双稳态，不刷
    // 坐实那一拍的矩形要盖住**自上次坐实以来驱动过的所有像素**（残影长在那儿），
    // 所以累计的是并集、不是本帧这一个矩形——选中条从列表底走到顶时两者差得最明显，
    // 只清当前这一格会把一路走过留下的墨痕全漏掉。
    s_list_dirty = s_list_dirty_any ? rect_union(s_list_dirty, d) : d;
    s_list_dirty_any = true;
    const int64_t t0 = esp_timer_get_time();
    if (++s_list_n >= LIST_GC16_EVERY) {
        s_list_n = 0;
        const EpdRect r = s_list_dirty;
        s_list_dirty = EpdRect{0, 0, 0, 0};
        s_list_dirty_any = false;
        guard_draw_result(hl, update_display_area_clean(hl, r));
        // 每条路都打一行：区域 → 几十毫秒，整屏 → 几百毫秒。这是"连滚十几下还闪不闪"
        // 的唯一现场证据（帧数/矩形对不上时，先看这条日志选了哪条路、矩形多大）。
        if (uiPerfLogOn()) {
            ESP_LOGI(TAG, "列表帧: 区域坐实 %d,%d %dx%d 刷屏 %lldms", r.x, r.y, r.width, r.height,
                     (long long)((esp_timer_get_time() - t0) / 1000));
        }
        return;
    }
    // 波形/模式与整屏那条 HALF 路**完全一致**（display.c 默认档：E0470_WAVEFORM +
    // MODE_GL16），只是把驱动范围收窄 —— 画质不该因为收窄而变，变的只有"闪多大"。
    guard_draw_result(hl, update_display_area_with(hl, &E0470_WAVEFORM, MODE_GL16, d));
    if (uiPerfLogOn()) {
        ESP_LOGI(TAG, "列表帧: 区域差分 %d,%d %dx%d 刷屏 %lldms (%d/%d)", d.x, d.y, d.width,
                 d.height, (long long)((esp_timer_get_time() - t0) / 1000), s_list_n,
                 LIST_GC16_EVERY);
    }
}

// 立即整屏 GC16（长按全刷 / 休眠提示）。
static void full_refresh_now(uint8_t *cur, int rel_idx) {
    if (!cur) return;
    EpdiyHighlevelState *hl = board_hl();
    s_force_full_next = false;
    drop_defer();
    display_soft_refresh_reset();   // 残影预算在 display.c（下面就是整屏 GC16，它也归零）
    s_follow_partials = 0;
    s_fast_body_n = 0;
    s_gc16_pending = false;
    if (rel_idx >= 0) s_last_idx = rel_idx;
    do_full_refresh(hl, cur, true);   // 内部销掉输入法那两行的记账
    release_buffer(rel_idx);
}

// 休眠唤醒：面板刚被物理清成白底（main.cpp 的 epd_clear），把当前缓冲里的画面
// 用 from-white 重推一遍：只重置旧帧基准，内容还是缓冲里那份（不会变白）。
static void from_white_now(uint8_t *cur, int rel_idx) {
    if (!cur) return;
    EpdiyHighlevelState *hl = board_hl();
    s_force_full_next = false;
    drop_defer();
    display_soft_refresh_reset();   // 残影预算在 display.c（下面就是整屏 GC16，它也归零）
    s_follow_partials = 0;
    s_fast_body_n = 0;
    s_gc16_pending = false;
    ime_clean_forget();   // 整屏 from-white 重推，两行也是全像素
    display_take_white_exit();   // 面板刚被物理清成白底，阅读器那笔"面板是灰"的账作废
    if (rel_idx >= 0) s_last_idx = rel_idx;
    copy_to_front(hl, cur);
    update_display_from_white(hl);
    release_buffer(rel_idx);
}

// 丢弃参考帧：下一帧无条件整屏 GC16。只在渲染任务里调（见 ui_render_invalidate）。
static void invalidate_state(void) {
    s_force_full_next = true;
    drop_defer();
    display_soft_refresh_reset();   // 残影预算在 display.c（下面就是整屏 GC16，它也归零）
    s_follow_partials = 0;
    s_fast_body_n = 0;
    s_gc16_pending = false;
    ime_clean_forget();   // 下一帧就是整屏 GC16，输入法那两行的记账不用留
    display_take_white_exit();   // 参考帧都丢了，阅读器留下的"面板是灰"的账也没有意义了
    s_rvk_dirty_any = false;   // 阅读器快档那份欠账同理：进阅读器/丢参考帧 = 整屏重推
}

// ── 渲染任务 ─────────────────────────────────────────────────────────────
static void ui_render_task(void *) {
    UiJob job;
    for (;;) {
        // 空闲时不能真的 portMAX_DELAY 睡死：rails_idle_check 在本循环末尾，
        // 它负责到点给 HV 轨下电（display.c 的 8s 期限）。阅读模式/联网期间
        // core0 不调它了，这里再睡死就永远不下电。
        TickType_t wait = pdMS_TO_TICKS(UI_RENDER_IDLE_MS);
        if (s_ime_deferred) {
            int64_t left_us = s_ime_defer_until - esp_timer_get_time();
            int left_ms = (int)((left_us + 999) / 1000);
            if (left_ms < 0) left_ms = 0;  // 到点就立刻推（窗口已收到 0，见 IME_DEFER_US）
            wait = pdMS_TO_TICKS(left_ms);
        }
        if (s_clean_dirty) {
            // 上屏后攒着的那两行还没清：睡到清理时刻，别等满 250ms 的空闲节拍
            // （否则"停手"到"清干净"之间会多出小半秒）。
            int64_t left_us = s_clean_due_us - esp_timer_get_time();
            int left_ms = (int)((left_us + 999) / 1000);
            if (left_ms < 1) left_ms = 1;
            TickType_t w = pdMS_TO_TICKS(left_ms);
            if (w < wait) wait = w;
        }
        if (xQueueReceive(s_q, &job, wait) == pdTRUE) {
            uint8_t *cur = (job.idx >= 0) ? s_fb[job.idx] : board_hl()->front_fb;
            switch (job.kind) {
                case JOB_PRESENT: render_present(job, cur, job.idx); break;
                case JOB_FULL: full_refresh_now(cur, job.idx); break;
                case JOB_FROM_WHITE: from_white_now(cur, job.idx); break;
                case JOB_INVALIDATE: invalidate_state(); break;
            }
        } else {
            flush_deferred();  // 没到期就是空转，函数自己会立刻返回
        }
        // 上屏后那两行的残影清理：到点（用户停手 IME_CLEAN_PAUSE_US 没有再输入）
        // 就把编码区+候选区全像素过一遍。每轮都查 —— 连着打字时队列不会空，
        // 只在 else 分支里查会永远轮不到。
        ime_clean_tick();
        // HV 轨空闲下电：和 epd_poweron 共用一路硬件，必须同任务。
        rails_idle_check((int64_t)(esp_timer_get_time() / 1000));
    }
}

// ── core0 侧接口 ─────────────────────────────────────────────────────────
// 双缓冲没起来（内存/任务创建失败，或取缓冲超时）时的降级：画在 front_fb 上，
// 提交时由 core0 同步跑一遍决策。
static uint8_t *s_sync_fb;

static int ime_top_now(void) {
    int top = -1;
    if (g_ime.composing()) {
        int st = imeStatusPanelTopY();
        int full = imeFullscreenPanelTopY();
        top = (full < st) ? full : st;
    }
    // 虚拟键盘面板也要算进来。面板顶 = 候选行顶，是最靠上的那块"输入法区域"；
    // 不含它的话，一次按键的差分框会从候选行（y≈面板顶）一路跨到按键高亮，
    // 顶边落在 IME 条之上 → 不走合并窗口 → 掉进下面"半屏以上整屏 GL16"那条规则，
    // 表现就是**每按一次闪一屏**。含进来之后，整块键盘区的变化都走
    // FOLLOW DU（8 帧 FAST 扫描 ≈89ms、只驱动变化矩形），不闪。
    // 注意：这里读的是 core0 侧的状态（editorVkVisible 只是 core0 改写的一个 bool），
    // 而 ime_top_now() 只在 core0 的提交路径上调用，不跨核。
    if (editorVkVisible()) {
        int vt = editorVkTop();
        if (top < 0 || vt < top) top = vt;
    }
    return top;
}

// 输入法条（编码行 + 候选行）画在哪儿——由 ui_helpers 的 drawIMEUI 上报，见
// ui_render.h 的 ui_render_note_ime_panel。空矩形 = 还没画过。
static EpdRect s_ime_panel_rect = {0, 0, 0, 0};

void ui_render_note_ime_panel(int x, int y, int w, int h) {
    s_ime_panel_rect = EpdRect{x, y, w, h};
}

void ui_render_note_ime_above_clean(void) {
    // 申报"输入法面板顶线（提交时算的 ime_top）**以上**一个像素都没动"。调用方
    // （screen_editor 组合期只重画输入法条那条路）必须真的如此：正文/标题/顶栏一个都没碰，
    // 只在打底像素上补了底部那条输入法条。面板顶线**以下**（输入法条 + 状态栏）随便变，
    // 那一段渲染任务照扫。
    //
    // 为什么只报"线以上"而不是报一个矩形：状态栏在输入法条**下面**（y≈633 而条顶 505），
    // 按矩形申报会把它漏在外面 —— 上屏那拍丢弃的推迟帧可以把状态栏的字留在旧值上，
    // 那一刻矩形申报就是假的。按行申报天然把状态栏圈进"要扫的那一段"，少一类谎。
    s_above_clean = true;
}

// 编码区+候选区矩形（逻辑坐标）—— 屏幕底部那两行，上屏时清的就是这一块。
// 两种形态都认：
//   * 虚拟键盘弹着 → 面板顶部的两行（不含键帽：键帽是"按下反白"这种一眼就换掉的
//     像素，不值得为它清一遍；候选行才是反复整排换字、残影攒着的地方）；
//   * 没弹键盘（实体键/蓝牙键，或手动收起）→ drawIMEUI 画的输入法条，位置由它自己
//     上报。**不能拿 g_ime.composing() 现推**：上屏那一拍组合已经结束、面板这一帧
//     根本没画，而那正是要清这两行的时候。
// 和 ime_top_now() 一样只能在 core0 的提交路径上读（那些状态都是 core0 在改的）。
static EpdRect cand_rect_now(void) {
    EpdRect r = s_ime_panel_rect;
    if (editorVkVisible()) {
        const int top = editorVkTop();
        if (top < 0 || top >= SCREEN_H) return EpdRect{0, 0, 0, 0};
        int h = editorVkCandH();
        if (h <= 0) return EpdRect{0, 0, 0, 0};
        if (top + h > SCREEN_H) h = SCREEN_H - top;
        return EpdRect{0, top, SCREEN_W, h};
    }
    if (r.width <= 0 || r.height <= 0) return EpdRect{0, 0, 0, 0};
    if (r.y < 0) { r.height += r.y; r.y = 0; }
    if (r.y + r.height > SCREEN_H) r.height = SCREEN_H - r.y;
    if (r.height <= 0) return EpdRect{0, 0, 0, 0};
    return r;
}

// 上屏认帧：IME::commit 每上屏一次给它自己的计数器 +1，这里比较前后值就知道
// "core0 刚上过屏"（渲染任务不该碰 g_ime，所以判定留在 core0 侧，只把 bool 传下去）。
// 每次提交都取走，避免把很久以前的某次上屏带到后面的帧上。
static uint32_t s_seen_commit_seq;
static bool take_ime_commit(void) {
    const uint32_t s = g_ime.commitSeq();
    if (s == s_seen_commit_seq) return false;
    s_seen_commit_seq = s;
    return true;
}

// 同上，认的是"这一帧刚吐出一个句读"（IME::punctSeq，见 IME.cpp 的 handleKey 外壳）。
// 两个计数器各记各的 seen，互不干扰；两次 take 都必须无条件调用，别短路。
static uint32_t s_seen_punct_seq;
static bool take_ime_punct(void) {
    const uint32_t s = g_ime.punctSeq();
    if (s == s_seen_punct_seq) return false;
    s_seen_punct_seq = s;
    return true;
}

static void submit_sync(uint8_t *fb, bool force_full) {
    UiJob job = {};
    job.kind = JOB_PRESENT;
    job.idx = -1;
    job.force_full = force_full;
    job.ime_top = ime_top_now();
    job.cand = cand_rect_now();
    job.ime_commit = take_ime_commit();
    const uint8_t pol = ime_clean_policy();
    job.ime_punct = take_ime_punct() && (pol & IME_CLEAN_ON_PUNCT);
    job.ime_clean_commit = (pol & IME_CLEAN_ON_COMMIT) != 0;
    job.ime_no_clean = (pol & IME_CLEAN_NO_GC16) != 0;
    job.ime_count = (pol == 0);   // 「计数清」：不挑事件，只按打字拍数攒（唯一的兜底档）
    job.ime_commit_fast = ime_commit_fast_policy();
    render_present(job, fb, -1);
}

// 初始化中途失败时的清理：把已分配的缓冲/队列/信号量全收回去。两块工作缓冲各
// ~406KB PSRAM，要是留着不放，退回单缓冲直画后它们再没人用，而阅读器本身就在抢
// PSRAM —— 一次开机期的分配失败就白占 812KB，属于"失败之后还得干净"的那种路径。
static void ui_render_release_partial(void) {
    for (int i = 0; i < 2; i++) {
        if (s_fb[i]) { heap_caps_free(s_fb[i]); s_fb[i] = nullptr; }
    }
    if (s_q) { vQueueDelete(s_q); s_q = nullptr; }
    if (s_free) { vSemaphoreDelete(s_free); s_free = nullptr; }
}

void ui_render_init(void) {
    if (s_active.load(std::memory_order_acquire)) return;
    s_fb_size = (size_t)(epd_width() / 2) * epd_height();
    for (int i = 0; i < 2; i++) {
        s_fb[i] = (uint8_t *)heap_caps_malloc(s_fb_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_fb[i]) {
            ESP_LOGE(TAG, "工作缓冲 %d 分配失败，退回单缓冲直画", i);
            for (int j = 0; j < i; j++) { heap_caps_free(s_fb[j]); s_fb[j] = nullptr; }
            return;
        }
        memset(s_fb[i], 0xFF, s_fb_size);
    }
    s_q = xQueueCreate(UI_RENDER_QUEUE_LEN, sizeof(UiJob));
    s_free = xSemaphoreCreateCounting(2, 2);
    if (!s_q || !s_free) {
        ESP_LOGE(TAG, "队列/信号量创建失败，退回单缓冲直画");
        ui_render_release_partial();
        return;
    }
    s_free_list[0] = 0;
    s_free_list[1] = 1;
    s_free_n = 2;
    s_force_full_next = true;
    s_active.store(true, std::memory_order_release);
    // 栈放 PSRAM（TCB 按 IDF 规定仍在内部 RAM）。这是"外部栈任务"的硬约束换来的：
    // 它绝不能自己写 flash（NVS 提交、固件写入会临时禁掉 cache，那一刻栈就读不到了）。
    // 本任务只做差分/选波形/推屏，没有任何 NVS/SD/OTA 操作，符合约束。任务常驻不退出，
    // 所以用不着 vTaskDeleteWithCaps（WithCaps 创建的栈只有它才能释放）。
    BaseType_t ok = xTaskCreatePinnedToCoreWithCaps(ui_render_task, "ui_render", UI_RENDER_STACK, nullptr,
                                                    UI_RENDER_PRIO, nullptr, UI_RENDER_CORE,
                                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "渲染任务创建失败，退回单缓冲直画");
        s_active.store(false, std::memory_order_release);
        ui_render_release_partial();
        return;
    }
    ESP_LOGI(TAG, "渲染任务已启动（core%d, prio %d），双工作缓冲 %u B ×2",
             UI_RENDER_CORE, UI_RENDER_PRIO, (unsigned)s_fb_size);
}

uint8_t *ui_render_begin_frame(void) {
    if (!g_u8g2) return nullptr;
    if (s_taken >= 0) return s_fb[s_taken];  // 同一帧重复 ui_clear：沿用同一块
    int idx = acquire_buffer(pdMS_TO_TICKS(UI_RENDER_TAKE_MS));
    if (idx < 0 && s_active.load(std::memory_order_acquire)) {
        // 再宽限一轮：队列里可能正排着两帧整屏 GC16（每帧 ≤1s），不是卡住。
        ESP_LOGW(TAG, "取渲染缓冲超时，再等一轮");
        idx = acquire_buffer(pdMS_TO_TICKS(UI_RENDER_TAKE_MS));
    }
    if (idx < 0) {
        if (s_active.load(std::memory_order_acquire)) {
            // 渲染任务活着、却连着两轮（6 秒）没还回缓冲 —— 判定它卡在 epdiy 里了
            // （最常见是等一个再也不会来的 LCD VSYNC，见 render_lcd.c 的
            // `xSemaphoreTake(frame_done, portMAX_DELAY)`）。
            //
            // 此时**绝不能**在 core0 自己跑一遍 epdiy：驱动只有一份全局
            // render_context 和一根 frame_done 信号量，两个任务撞进去会把驱动状态和
            // 画面一起踩坏；更要命的是 core0 会卡在同一根信号量上——两个任务都是
            // **阻塞**而不是自旋，两核的 idle 任务照常喂狗，看门狗根本不会复位，结果
            // 就是屏幕和系统一起永久死住，只能抠电池。能自救的动作只有一个：重启。
            ESP_LOGE(TAG, "渲染任务连续 6 秒未归还缓冲，判定卡死，重启恢复");
            esp_restart();
        }
        // 没有渲染任务（创建失败/未初始化）：core0 直接画 front_fb 是安全的，
        // 提交时由 core0 同步跑一遍决策。**不能**沿用 u8g2 当前指向 —— 那可能正是
        // 渲染任务手里那块。
        s_sync_fb = board_hl()->front_fb;
        u8g2_set_fb(g_u8g2, s_sync_fb);
        return s_sync_fb;
    }
    s_taken = idx;
    s_sync_fb = nullptr;
    u8g2_set_fb(g_u8g2, s_fb[idx]);
    return s_fb[idx];
}

void ui_render_submit(bool force_full) {
    if (!g_u8g2) return;
    // 申报"面板顶线以上没动"是**一次性交接**：这里先取走再清空，免得下面任何一条早退
    // （降级同步推、队列满丢帧）把它漏给下一帧。
    const bool above_clean = s_above_clean;
    s_above_clean = false;
    if (s_taken < 0) {
        // 没走 ui_clear 就提交。降级为同步推（内容就在当前 u8g2 缓冲里）。
        // 正常路径不该出现 —— 每个界面都是 ui_clear → 画 → ui_commit。
        static int warned;
        if (s_sync_fb) {
            uint8_t *fb = s_sync_fb;
            s_sync_fb = nullptr;
            submit_sync(fb, force_full);
        } else if (warned++ < 8) {
            ESP_LOGW(TAG, "提交了一帧没开过的帧（缺 ui_clear？）");
        }
        return;
    }
    UiJob job = {};
    job.idx = s_taken;
    job.kind = JOB_PRESENT;
    job.force_full = force_full;
    job.above_clean = above_clean;  // "面板顶线以上没动"的申报
    // IME 面板顶在这里算：core0 侧才能安全读 g_ime / g_font（渲染任务不该碰它们）。
    job.ime_top = ime_top_now();
    job.cand = cand_rect_now();
    job.ime_commit = take_ime_commit();
    const uint8_t pol = ime_clean_policy();
    job.ime_punct = take_ime_punct() && (pol & IME_CLEAN_ON_PUNCT);
    job.ime_clean_commit = (pol & IME_CLEAN_ON_COMMIT) != 0;
    job.ime_no_clean = (pol & IME_CLEAN_NO_GC16) != 0;
    job.ime_count = (pol == 0);   // 「计数清」：不挑事件，只按打字拍数攒（唯一的兜底档）
    job.ime_commit_fast = ime_commit_fast_policy();
    job.stamp_us = esp_timer_get_time();
    const int idx = s_taken;
    s_last_drawn_idx = idx;   // 供 ui_render_begin_frame_seeded 取"上一帧的像素"（见其声明处）
    s_taken = -1;
    if (xQueueSend(s_q, &job, pdMS_TO_TICKS(UI_RENDER_TAKE_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "推屏队列满，丢弃本帧");
        release_buffer(idx);
    }
}

// 保留帧占着一块缓冲（见 ui_render_keep_frame），空闲缓冲的目标数要相应减一。
static int s_kept_idx = -1;

void ui_render_drain(void) {
    if (!s_active.load(std::memory_order_acquire)) return;
    const int target = (s_kept_idx >= 0) ? 1 : 2;
    // 队列空 + 空闲缓冲数到位 = 已提交的帧全推完了。
    for (int i = 0; i < 2000; i++) {
        if (uxQueueMessagesWaiting(s_q) == 0 && uxSemaphoreGetCount(s_free) >= target) return;
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    ESP_LOGW(TAG, "drain 超时");
}

bool ui_render_idle(void) {
    if (!g_u8g2 || !s_active.load(std::memory_order_acquire)) return false;
    if (s_taken >= 0 || s_sync_fb) return false;   // 已经有帧开着（还没 ui_commit）
    if (s_last_drawn_idx < 0) return false;        // 还没画过任何一帧：没有底
    if (!s_q || uxQueueMessagesWaiting(s_q) != 0) return false;   // 队列里还压着没推的帧
    // 空闲缓冲数到位 = core1 手上没有正在推的那一块（保留帧占着一块时目标数减一）。
    const int target = (s_kept_idx >= 0) ? 1 : 2;
    return uxSemaphoreGetCount(s_free) >= target;
}

void ui_render_invalidate(void) {
    // 必须投给渲染任务做，不能在 core0 直接改：这些 static 里有正在飞的缓冲
    // （drop_defer 会还缓冲），和 core1 的推屏撞上就是缓冲被两边同时用。
    if (s_active.load(std::memory_order_acquire) && s_q) {
        UiJob job = {};
        job.kind = JOB_INVALIDATE;
        job.idx = -1;
        if (xQueueSend(s_q, &job, pdMS_TO_TICKS(UI_RENDER_TAKE_MS)) == pdTRUE) return;
    }
    invalidate_state();
}

static void post_last_frame(int kind);

void ui_render_full_refresh(void) {
    if (!g_u8g2) return;
    if (s_sync_fb) {  // 降级路径：当前帧还没推，直接按 GC16 同步推掉
        uint8_t *fb = s_sync_fb;
        s_sync_fb = nullptr;
        submit_sync(fb, true);
        return;
    }
    if (!s_active.load(std::memory_order_acquire)) return;
    if (s_taken >= 0) {  // 当前有一帧开着：把这块直接按 GC16 推掉
        ui_render_submit(true);
        ui_render_drain();
        return;
    }
    post_last_frame(JOB_FULL);
}

void ui_render_restore(void) {
    if (!g_u8g2) return;
    if (s_sync_fb) {
        uint8_t *fb = s_sync_fb;
        s_sync_fb = nullptr;
        submit_sync(fb, true);
        return;
    }
    if (!s_active.load(std::memory_order_acquire)) return;
    if (s_taken >= 0) ui_render_submit(false);  // 唤醒时不该有开着的帧
    post_last_frame(JOB_FROM_WHITE);
}

// 把"最后一次推上屏的那块缓冲"的内容投一个推屏作业，推完才返回。
// 当前没有开着的帧时用（有开着的帧走 ui_render_submit 那条路）。
static void post_last_frame(int kind) {
    if (!s_active.load(std::memory_order_acquire) || !s_q) return;
    ui_render_drain();
    const int idx = acquire_buffer(pdMS_TO_TICKS(UI_RENDER_TAKE_MS));
    if (idx < 0) return;
    if (s_last_idx >= 0 && s_last_idx != idx) memcpy(s_fb[idx], s_fb[s_last_idx], s_fb_size);
    UiJob job = {};
    job.kind = kind;
    job.idx = idx;
    if (xQueueSend(s_q, &job, pdMS_TO_TICKS(UI_RENDER_TAKE_MS)) != pdTRUE) {
        release_buffer(idx);
        return;
    }
    ui_render_drain();
}

int ui_render_begin_frame_seeded_try(uint32_t wait_ms) {
    // "接着上一帧往下画"：把 **core0 自己最后画过的那帧** 拷进一块空闲缓冲当绘制目标，
    // 于是只改其中一小块的界面（编辑器组合期的输入法条 / 候选两行 + 键帽）不必整屏重画。
    //
    // 与 ui_render_begin_overlay 的差别只在底从哪来：那边取 s_last_idx（渲染任务最后
    // **认领**的帧），提交完到认领之间有个窗口，队里压着上一次推屏时最大能差一帧 ——
    // 拿它打底会把上屏前的旧正文铺回去。这里取 s_last_drawn_idx，提交那一刻就定死。
    //
    // 返回 >= 0 = 已开帧且以上一帧打底（值就是缓冲下标）；-1 = **没开帧**，调用方按老路
    // 走 ui_clear() + 整屏重画。宁可返回 -1 也绝不能让调用方在没打底的缓冲上"只画一小块"
    // —— 那会画出一张正文全白的帧。
    //
    // wait_ms 由调用方定：0 = 绝不阻塞（差分路，撞上推屏背压就退回整屏重画）。
    if (!g_u8g2 || !s_active.load(std::memory_order_acquire)) return -1;
    if (s_taken >= 0 || s_sync_fb) return -1;  // 已经开着帧：交给老路（它会沿用同一块）
    const int src = s_last_drawn_idx;
    if (src < 0) return -1;  // 还没画过任何一帧（首帧）：没有底可打
    const int idx = acquire_buffer(pdMS_TO_TICKS(wait_ms));
    if (idx < 0) return -1;  // 取不到缓冲：老路的 begin_frame 会去做宽限/重启那套
    if (src != idx) memcpy(s_fb[idx], s_fb[src], s_fb_size);
    s_taken = idx;
    u8g2_set_fb(g_u8g2, s_fb[idx]);
    return idx;
}

bool ui_render_begin_frame_seeded(void) {
    return ui_render_begin_frame_seeded_try(UI_RENDER_TAKE_MS) >= 0;
}

void ui_render_begin_overlay(void) {
    // "在现有画面上叠加"（休眠提示那种只补画一角、不整屏重画的路径）。
    // 把已推上屏的那帧内容复制进一块空闲缓冲当绘制目标：叠出来的仍是"当前画面"，
    // 而且这块缓冲是 core0 独占的，撞不上推屏。
    // 读 s_fb[s_last_idx] 与渲染任务并发也是安全的 —— 双方都只读。
    if (!g_u8g2 || !s_active.load(std::memory_order_acquire)) return;
    if (s_taken >= 0 || s_sync_fb) return;  // 已经开着帧，直接往那块上画
    const int idx = acquire_buffer(pdMS_TO_TICKS(UI_RENDER_TAKE_MS));
    if (idx < 0) return;
    if (s_last_idx >= 0) {
        const int src = s_last_idx;
        if (src != idx) memcpy(s_fb[idx], s_fb[src], s_fb_size);
    } else {
        memset(s_fb[idx], 0xFF, s_fb_size);  // 还没推过任何一帧：白底
    }
    s_taken = idx;
    u8g2_set_fb(g_u8g2, s_fb[idx]);
}

// ── 保留帧（待机时钟 / 休眠）─────────────────────────────────────────────
// standbyClockDraw() 之前调一次：把"屏上现在的画面"留一块副本，等唤醒或预览结束后
// 用 ui_render_restore_kept() 原样推回去。期间这块工作缓冲一直占着（休眠时本来就
// 不画别的东西；预览那 3 秒也够用另一块）。
// 取的是 hl->front_fb 而不是某个工作缓冲：阅读器整条路径直接画 front_fb，
// "屏上现在的画面"只有它一定对（见 copy_to_front）。
void ui_render_keep_frame(void) {
    if (!s_active.load(std::memory_order_acquire) || s_kept_idx >= 0) return;
    ui_render_drain();  // 推完在飞的那一帧，front_fb 才是稳定的
    EpdiyHighlevelState *hl = board_hl();
    if (!hl || !hl->front_fb) return;
    const int idx = acquire_buffer(pdMS_TO_TICKS(UI_RENDER_TAKE_MS));
    if (idx < 0) return;
    memcpy(s_fb[idx], hl->front_fb, s_fb_size);
    s_kept_idx = idx;
}

void ui_render_restore_kept(void) {
    if (!s_active.load(std::memory_order_acquire) || s_kept_idx < 0) {
        ui_render_restore();  // 没保留过：退化成"把当前缓冲推一遍"
        return;
    }
    ui_render_drain();
    const int idx = s_kept_idx;
    s_kept_idx = -1;
    UiJob job = {};
    job.kind = JOB_FROM_WHITE;  // 面板刚被物理清成白底，基准要重置
    job.idx = idx;
    if (xQueueSend(s_q, &job, pdMS_TO_TICKS(UI_RENDER_TAKE_MS)) != pdTRUE) {
        release_buffer(idx);
        return;
    }
    ui_render_drain();  // 推完再返回：唤醒时下一帧就叠在这上面
}
