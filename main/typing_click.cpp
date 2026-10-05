// 按键反馈音：Read Pico 板无 ES8311 DAC/功放，但板载 GPIO2 蜂鸣器是线圈+MOSFET
// (SYS_VDD→线圈→漏极)，经 pwm_audio 高频 PWM 载波 + duty 映射可还原零中心 PCM
// (见 read_pico_buzzer.c 的 score 链路)。
// 本文件在会话期间把 duty 映射换成**零偏置**(静音 = 占空比 0 = 线圈不通电)，
// 详见下面 swapZeroBiasMap 的说明 —— 默认那张偏向映射的静音电平是"半通电"，
// 收声等于撤掉直流，那一次膜片回位就是空闲后听到的"嗒"。
//
// 只有一种反馈音：一声 12ms 的合成"嗒"。没有音色档、没有采样包、没有随机抖动
// ——同一个波形每次都一模一样，这正是输入法反馈音该有的可预期感（也不再需要
// "试听用固定种子"那套：正常打字与试听走同一条路径）。
//
// play() 只把请求塞进队列就返回，合成与写 PWM 由一个后台任务做，UI 线程不会被
// 阻塞。忙(上一声未放完)则丢弃本次，避免打字快时音效排队落后输入。

#include "typing_click.h"

#include <string.h>
#include <atomic>
#include <cmath>
#include <vector>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>   // xTaskCreateWithCaps：把常驻任务栈放到 PSRAM
#include <freertos/queue.h>
#include <freertos/task.h>

#include "settings_manager.h"
#include "read_pico_buzzer.h"

// 这里**不能**再自己弱声明 read_pico_buzzer_pcm_* 那几个函数来探测"实现有没有链进来"：
// read_pico_buzzer.h 本身就声明了它们（esp_err_t 返回，本机这份是 pcm_write:pcm_close_fade），
// 再声明一遍就是 conflicting declaration；而且 read_pico 组件在本工程是无条件编译的，
// GCC 看得见定义，`f != nullptr` 这种判空一律吃 -Werror=address。
// "PCM 会话开不出来"这条退化路径由下面的 open 失败退避负责，与链接期无关。

namespace {

static const char* TAG = "Click";

const uint32_t TC_SAMPLE_RATE = 16000;
const int TC_MAX_N = 8;                 // 待播声数上限(防异常长文本把声音拖远)
const int TC_GAP_MS = 30;               // 多声之间的静音间隔(固定，不抖动)
// 连续敲击间隔超过这个就关掉 PCM 会话，把 gptimer/LEDC 还回去(省电)。
// **这条路径现在听不见了**：会话用的是零偏置映射(见下面 swapZeroBiasMap)，静音
// 就是占空比 0、线圈本来就没通电，收声只是从最后一个采样滑到 0，没有"断电"这回事。
// (以前用默认的 bias=512 映射，静音是"线圈半通电"，收声等于把直流撤掉 —— 那次
// 膜片回位就是空闲 30s 后那声"嗒"；把淡出从 500ms 拉到 2s 也压不掉，说明问题不在
// 加速度峰值，而在直流本身。)
const int TC_IDLE_MS = 30000;
// 空闲收声的淡出时长。零偏置之后这里已经没有直流可淡出，随便给个短的就行；
// 留着这个常数是为了被打断的收声(s_abort/typingClickRelease)也走同一条路。
const int TC_CLOSE_FADE_MS = 500;

const int TC_STACK = 6144;
// PCM 会话开不出来之后，别每敲一键再试一次：内部 RAM 紧张时那条失败路径会漏句柄
// (见 read_pico_firmware/components/pwm_audio/pwm_audio.c，已在那边修，这里是双保险)，
// 而且每次都要白试一次 8KB 连续内部 RAM 的分配。退避这段时间直接丢弃音效。
const int TC_OPEN_RETRY_MS = 10000;
const uint32_t TC_MIN_INT_FREE = 12288;  // 低于这个就别开 PCM 会话(它要 8KB 连续内部 RAM)

// ── 单声反馈音的合成参数(全是固定值，无随机) ─────────────────────────────
// 一声"嗒" = 一层下滑音 + 一层起振噪声：
//   · 正弦从 900Hz×1.9 在 ~1.5ms 内滑到 900Hz，振幅按 2.8ms 指数衰减。频率下坠是"咔"
//     的关键——没有它，纯正弦听起来是"哔"不是敲击。
//   · 起振叠 1ms 的白噪声，给宽带瞬态(敲击的"t")。这段过线圈后基本被压没了，
//     靠下面的预加重捞回来，正好补成清脆的边缘。
// 两个频率都压在 1.6kHz 以内：线圈 L/R 极点以上抬多少都是白抬，只会吃动态范围。
const double TC_HIT_S = 0.012;      // 总长 12ms：短到连打不糊，长到能听出"嗒"
const double TC_F0 = 900.0;         // 落定频率
const double TC_GLIDE = 0.90;       // 起振比落定高 90%
const double TC_TAU_G = 0.0015;     // 频率下滑的时间常数(s)
const double TC_TAU_A = 0.0028;     // 振幅衰减时间常数(s)
const double TC_NOISE = 0.55;       // 起振噪声幅度
const double TC_TAU_N = 0.0010;     // 噪声衰减时间常数(s)
const double TC_ATTACK_S = 0.0003;  // 起振斜坡，防 PCM 起点直流阶跃

// 预加重(高频提升)：蜂鸣器是低边开关驱动线圈，平均电流 ∝ 占空比，但线圈自身的 L/R
// 极点(这类蜂鸣器典型几百 Hz)把声音按 -6dB/oct 往下压 —— 这就是"闷"的来源。补偿取
// 300Hz 起的一阶 +6dB/oct 搁架，形状与滚降互逆。固定值，不做成档位。
const double TC_PREEMPH_FC = 300.0;
const double TC_PREEMPH_G = 1.0;

// 一次播放请求：响几声。
struct TcReq {
    int count;
};

static QueueHandle_t s_q = nullptr;    // 每项 = 一次 play 请求
static TaskHandle_t s_task = nullptr;
static std::atomic<bool> s_abort{false};  // 请求中断当前连发
static portMUX_TYPE s_pend_mux = portMUX_INITIALIZER_UNLOCKED;
static int s_pending = 0;              // 已排队待播的声数

static int64_t s_open_retry_at_us = 0;  // 开会话失败后的退避截止时刻

// 只看开关，不看输入模式：按键反馈音也是虚拟键盘的按键反馈，非打字机模式下同样该能开。
bool enabled() {
    return g_settings.typingClickEnabled();
}

int clamp16(long v) { return v > 32767 ? 32767 : v < -32767 ? -32767 : (int)v; }

// ── 会话期间专用的 duty 映射：零偏置(乙类/半波) ────────────────────────────
// 默认映射(BUZZER_DUTY_MAP_EXPERIMENTAL)是**甲类**：静音 = bias 512 = 线圈半通电，
// 声音是这个直流工作点上下的摆动。它的毛病是"停"这件事本身：bias→0 是一次直流撤除，
// 音圈上的膜片会回位一次，就是那声"嗒"(空闲 30s 自动收声时最清楚)。淡出只是把它推迟、
// 变慢，撤直流这件事还在，所以改两轮都没消掉。
//
// 换成 bias=0(静音 = 占空比 0 = 线圈真正断电)之后就没有"撤直流"这一步了：收声不过是
// 从最后一个采样滑到 0，而波形自己的包络本来就在往 0 走。代价是负半周被 duty_min=0
// 削掉，成了半波驱动 —— 但半波整流的**基波幅度只有全波的一半**，把增益翻倍之后基波
// 幅度与原来完全相同(听感音量不变)，多出来的只是几个偶次谐波(听感更"脆"一点)。
// 全 app 只有按键音走 PCM 会话，所以整段会话期间换这张表对别人没有影响；会话一关
// 立刻还原(restoreMap)，tone/曲谱那些走 s_map 的路径看不到它。
static buzzer_duty_map_t s_base_map;
static bool s_map_swapped = false;

void swapZeroBiasMap() {
    if (s_map_swapped) return;
    buzzer_duty_map_t base;
    read_pico_buzzer_duty_map_get(&base);
    buzzer_duty_map_t z = base;
    z.bias = 0;
    if (z.gain <= BUZZER_DUTY_GAIN_MAX / 2) z.gain = (uint16_t)(z.gain * 2);
    if (read_pico_buzzer_duty_map_set(&z) != ESP_OK) return;  // 换不成→照旧(顶多还是老毛病)
    s_base_map = base;
    s_map_swapped = true;
}

void restoreMap() {
    if (!s_map_swapped) return;
    read_pico_buzzer_duty_map_set(&s_base_map);
    s_map_swapped = false;
}

// 零偏置映射下不能直接用 read_pico_buzzer_pcm_legal_peak()：它取正负两侧可用范围的
// 较小者，而零偏置时负半周没有任何可用范围(dn=0)，它会返回 0。这里只取正半周那一侧。
int zeroBiasLegalPeak() {
    buzzer_duty_map_t z;
    read_pico_buzzer_duty_map_get(&z);
    if (z.gain <= 0) return 4652;
    const int up = ((int)z.duty_max - (int)z.bias) * 32768 / (int)z.gain;
    return up > 0 ? up : 4652;
}

// 合成一声，输出 int16 PCM。整段归一化到 legal_peak(≈4652，duty 映射的满摆幅)后
// 乘音量，所以音量 100% 也不会撞占空比两端削波。
void makeHit(int legal_peak, int vol, std::vector<int16_t> &out) {
    const int n = (int)(TC_HIT_S * TC_SAMPLE_RATE);
    const int att = (int)(TC_ATTACK_S * TC_SAMPLE_RATE);
    std::vector<double> buf((size_t)n);

    // 固定种子的局部 LCG：噪声每次完全一致，不做随机化(要的就是可预期)。
    uint32_t rnd = 0x9E3779B9u;
    auto next01 = [&rnd]() {
        rnd = rnd * 1664525u + 1013904223u;
        return (double)rnd / 4294967296.0;
    };

    double phase = 0.0;
    for (int i = 0; i < n; i++) {
        const double t = (double)i / TC_SAMPLE_RATE;
        const double f = TC_F0 * (1.0 + TC_GLIDE * exp(-t / TC_TAU_G));
        phase += 2.0 * M_PI * f / TC_SAMPLE_RATE;
        double v = sin(phase) * exp(-t / TC_TAU_A);
        v += TC_NOISE * (next01() * 2.0 - 1.0) * exp(-t / TC_TAU_N);
        if (i < att) v *= (double)i / (double)att;  // 起振斜坡
        buf[i] = v;
    }

    // 预加重：一阶搁架。lp 是拐点 TC_PREEMPH_FC 的一阶低通，y = x + g*(x - lp) 在拐点
    // 以上给出平坦的 +g 提升、以下保持原样，正好是线圈 -6dB/oct 滚降的逆。
    {
        const double b = exp(-2.0 * M_PI * TC_PREEMPH_FC / (double)TC_SAMPLE_RATE);
        const double a = 1.0 - b;
        double lp = buf[0];
        for (int i = 0; i < n; i++) {
            lp += a * (buf[i] - lp);
            buf[i] += TC_PREEMPH_G * (buf[i] - lp);
        }
    }

    // 归一化到满摆幅(乘音量)。
    double pk = 0.0;
    for (int i = 0; i < n; i++) {
        const double a = fabs(buf[i]);
        if (a > pk) pk = a;
    }
    const double scale = (pk > 0.0 ? legal_peak / pk : 0.0) * vol / 100.0;

    out.resize((size_t)n);
    for (int i = 0; i < n; i++)
        out[i] = (int16_t)clamp16(lround(buf[i] * scale));
}

// 常驻线程：PCM 会话在此 open/保持/close。连续敲键共用同一会话，避免逐键
// pwm_audio init/deinit(快速输入时 gptimer/LEDC/GPIO 反复重建是崩溃的主嫌)，也让
// 上一声的尾音自然排空、下一声接得上。play() 只入队就返回，UI 线程不被阻塞。
void clickTask(void *) {
    bool open = false;
    for (;;) {
        TcReq req;
        TickType_t wait = open ? pdMS_TO_TICKS(TC_IDLE_MS) : portMAX_DELAY;
        if (xQueueReceive(s_q, &req, wait) != pdTRUE) {
            // 空闲超时：关掉会话，释放 PWM 与锁(省电)。
            if (open) {
                // 空闲收声。零偏置映射下这里已经没有直流要淡出，纯粹是把 gptimer/LEDC
                // 还回去；记一笔方便对上"空闲后那声"到底还在不在(听不见就对了)。
                ESP_LOGI(TAG, "会话关闭(空闲 %dms)", TC_IDLE_MS);
                read_pico_buzzer_pcm_close_fade(TC_CLOSE_FADE_MS);
                restoreMap();
                open = false;
            }
            continue;
        }
        const int n = req.count;

        portENTER_CRITICAL(&s_pend_mux);
        s_pending -= n;
        if (s_pending < 0) s_pending = 0;
        portEXIT_CRITICAL(&s_pend_mux);

        s_abort.store(false, std::memory_order_release);

        const int vol = g_settings.typingClickVolume();
        if (vol <= 0) {  // 静音档
            if (open) {
                read_pico_buzzer_pcm_close();
                restoreMap();
                open = false;
            }
            continue;
        }

        if (!open) {
            const int64_t now_us = esp_timer_get_time();
            if (now_us < s_open_retry_at_us) continue;  // 退避中：丢弃本批，别重试
            if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL) < TC_MIN_INT_FREE) {
                s_open_retry_at_us = now_us + (int64_t)TC_OPEN_RETRY_MS * 1000;
                continue;  // 内部 RAM 不够，开了也白开
            }
            // 先换成零偏置映射再开会话：open 里那段 0→bias 的启始斜坡跟着新表走
            // (bias=0 时是空操作)，会话里所有 submit_pcm 也都按它算占空比。
            swapZeroBiasMap();
            if (read_pico_buzzer_pcm_open(TC_SAMPLE_RATE) != ESP_OK) {
                restoreMap();  // 没开成，别把映射留在零偏置上
                s_open_retry_at_us = now_us + (int64_t)TC_OPEN_RETRY_MS * 1000;
                continue;  // 忙(曲谱/别的音在播)：丢弃这批，别阻塞
            }
            open = true;
        }

        // 归一化基准跟着当前(零偏置)映射走：半波驱动下要打满的是正半周那一侧。
        const int legal_peak = zeroBiasLegalPeak();

        // 静音间隔写成 PCM 0：零偏置映射下就是占空比 0，线圈断电(甲类那张表里
        // PCM 0 才是占空比 50%，所以这个常数是跟着 swapZeroBiasMap 一起变的)。
        const int gap_n = (int)(TC_GAP_MS * TC_SAMPLE_RATE / 1000);
        std::vector<int16_t> gap((size_t)gap_n, 0);
        std::vector<int16_t> hit;
        int frames = 0;
        const int64_t t0 = esp_timer_get_time();
        for (int k = 0; k < n && !s_abort.load(std::memory_order_acquire); k++) {
            makeHit(legal_peak, vol, hit);
            read_pico_buzzer_pcm_write(hit.data(), (int)hit.size());
            frames += (int)hit.size();
            if (k + 1 < n && !s_abort.load(std::memory_order_acquire)) {
                read_pico_buzzer_pcm_write(gap.data(), gap_n);
                frames += gap_n;
            }
        }
        const int64_t t1 = esp_timer_get_time();
        // 埋点：实际喂给蜂鸣器的帧数就是这一声的真实时长，用来核对听感与代码是否一致
        // (PCM 会话每帧 1/TC_SAMPLE_RATE 秒，与 pwm_audio 的采样率同一路)。
        if (frames > 0) {
            ESP_LOGI(
                TAG, "反馈音 %d 声 %d 帧 %.0fms(合成+写入 %lldms)",
                n, frames, frames * 1000.0 / TC_SAMPLE_RATE,
                (long long)((t1 - t0) / 1000)
            );
        }
        // 被打断(typingClickRelease)：立刻关会话。
        if (s_abort.load(std::memory_order_acquire)) {
            read_pico_buzzer_pcm_close();
            restoreMap();
            open = false;
        }
    }
}

bool ensureTask() {
    if (s_task != nullptr) return true;
    if (s_q == nullptr) s_q = xQueueCreate(TC_MAX_N, sizeof(TcReq));
    if (s_q == nullptr) return false;
    // 栈放 PSRAM：这个常驻任务只在首次播放按键音效时建一次、此后永不销毁，
    // 6KB 内部 RAM 一旦占上就再也回不来（内部 RAM 常态只剩 ~12KB）。它只合成 PCM
    // 写蜂鸣器、读设置，从不写 flash，满足"外部栈任务不得在禁用 cache 期间运行"。
    if (xTaskCreateWithCaps(clickTask, "click", TC_STACK, nullptr, 2, &s_task,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        s_task = nullptr;
        return false;
    }
    return true;
}

void enqueue(int count) {
    if (!enabled()) return;
    if (count < 1) count = 1;
    if (count > TC_MAX_N) count = TC_MAX_N;
    if (!ensureTask()) return;

    portENTER_CRITICAL(&s_pend_mux);
    const bool room = s_pending + count <= TC_MAX_N;
    if (room) s_pending += count;
    portEXIT_CRITICAL(&s_pend_mux);
    if (!room) return;  // 已排满：丢掉这次

    const TcReq req = {count};
    if (xQueueSend(s_q, &req, 0) != pdTRUE) {
        portENTER_CRITICAL(&s_pend_mux);
        s_pending -= count;
        if (s_pending < 0) s_pending = 0;
        portEXIT_CRITICAL(&s_pend_mux);
    }
}

}  // namespace

void typingClickPlay(int count) { enqueue(count); }

// 试听：和正常打字完全同一条路径(合成里已经没有随机量，两个入口本来就一模一样)，
// 只是默认响三声。走同一条队列，所以不会阻塞 UI 线程 —— 设置页按下瞬间就返回重绘。
void typingClickAudition(int count) { enqueue(count); }

void typingClickRelease() {
    if (s_task == nullptr) return;
    s_abort.store(true, std::memory_order_release);  // 让任务尽快收尾当前连发、并关掉 PCM 会话
    xQueueReset(s_q);
    portENTER_CRITICAL(&s_pend_mux);
    s_pending = 0;
    portEXIT_CRITICAL(&s_pend_mux);
}
