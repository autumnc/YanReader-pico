/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * GPIO2 → Q1(AO3400A) 栅极，R8=100k 下拉。高电平导通，低电平关断。
 * SYS_VDD→蜂鸣器→漏极，D1 续流。不是隔直/低通音频级。
 *
 * tone()：音频频率方波，空闲 duty=0。
 * 曲谱：零中心 PCM 在任务中合成，再映射为占空比，经 pwm_audio 改 10bit 载波。
 * 停播/失败/欠载：禁用 PWM 并输出低，不用 50% 表示停止。
 * 软件限幅不是电流保护。未确认型号、SYS_VDD、线圈电流前，工作点只是实验值。
 *
 * GPIO2 → Q1 (AO3400A) gate, R8=100k pulldown. High on, low off.
 * SYS_VDD → buzzer → drain, D1 freewheel. Not a DC-block / LPF audio stage.
 *
 * tone(): audio-rate square, idle duty=0.
 * Score: zero-centered PCM in a task, mapped to duty, 10-bit carrier via pwm_audio.
 * Stop / fail / underrun: disable PWM and drive low; 50% is not "off".
 * Software clip is not current protection. Until part, SYS_VDD, and coil
 * current are confirmed, the setpoint is experimental.
 */

#include "read_pico_buzzer.h"

#include <inttypes.h>
#include <string.h>

#include "buzzer_1bit.h"
#include "buzzer_score.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/rmt_encoder.h"
#include "driver/rmt_tx.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "pwm_audio.h"
#include "soc/soc.h"

#ifndef APB_CLK_FREQ
#define APB_CLK_FREQ 80000000
#endif

#define TAG "buzzer"

#define BUZZER_GPIO GPIO_NUM_2
#define BUZZER_TIMER LEDC_TIMER_0
#define BUZZER_CHANNEL LEDC_CHANNEL_0
#define PWM_RES LEDC_TIMER_10_BIT
#define CHUNK 512
#define FADE_N 1280
#define RINGBUF_LEN (8 * 1024)
#define WRITE_WAIT_MS 80
#define DRAIN_MS 150
#define STOP_WAIT_MS 2500
#define PROBE_MS 400
#define PROBE_15K_HZ 1500
#define PROBE_4K_HZ 4000
#define MIDI_E5 76
#define LEDC_DUTY_FS 1024
#define PWM_AUDIO_DUTY_MAX 1023
#define PEN_B_DUTY_HI PWM_AUDIO_DUTY_MAX
#define BIT1_WINDOW_US 4000
#define BIT1_RMT_HZ 1000000

#define JOB_SCORE 0
#define JOB_A 1
#define JOB_B 2
#define JOB_1BIT 3
#define JOB_4K 4

static bool s_ledc_ready;
static bool s_pwm_up;
static volatile bool s_score_playing;
static volatile bool s_score_stop;
static uint32_t s_carrier_hz;
static uint32_t s_underrun;
static SemaphoreHandle_t s_lock;
static TaskHandle_t s_score_task;
static bool s_pcm_open;
static uint32_t s_pcm_rate;
// PCM 会话里最后一个写出去的占空比。收声要从"现在真正停在哪"开始淡出，而不是
// 一律从静音电平(bias)开始——否则中途被打断的会话一收声就是一次电流阶跃。
static uint16_t s_pcm_duty;
static int16_t s_chunk[CHUNK];
static int16_t s_hw[CHUNK];
static buzzer_score_stats_t s_stats;
static buzzer_duty_map_t s_map = BUZZER_DUTY_MAP_EXPERIMENTAL;
static uint8_t s_job;
static uint8_t s_1bit_voices = 1;
static buzzer_duty_accum_t s_duty_acc;
static read_pico_buzzer_out_stats_t s_out;
static const uint16_t k_1bit_hz[BUZZER_1BIT_VOICES_MAX] = {
    1500, 1000, 1250, 750, 1875, 600
};

static void lock_once(void) {
    if (s_lock != NULL) {
        return;
    }
    s_lock = xSemaphoreCreateMutex();
}

// 把蜂鸣器脚钉在低电平（MOSFET 关断 → 线圈零电流）。
//
// **不要用 gpio_reset_pin()**：它把脚变成"输出禁用 + 内部上拉"的高阻态。驱动级一关，
// 栅极就靠自身电荷撑着；紧接着上拉把栅极往 3.3V 拽，MOSFET 导通一下，漏极电流一个
// 脉冲 —— 音圈膜片动一次，就是那声"嗒"（空闲 30s 收声之后最清楚，因为那时没有按键
// 声盖着它）。
//
// 正确做法：先把**输出寄存器**写成 0，再让脚听这个寄存器（切到 GPIO 输出）。这样
// 从"被 LEDC 驱动的低电平"到"被 GPIO 驱动的低电平"中间不存在高阻窗口，栅极全程
// 被钉在 0。上下拉也一并关掉，免得别的路径给它上拉。
static void gpio_idle(void) {
    gpio_set_level(BUZZER_GPIO, 0);                        // 先写输出寄存器
    gpio_set_direction(BUZZER_GPIO, GPIO_MODE_OUTPUT);     // 再让脚听它
    gpio_set_pull_mode(BUZZER_GPIO, GPIO_FLOATING);        // 不要内部上下拉
}

void read_pico_buzzer_duty_map_get(buzzer_duty_map_t *map) {
    if (map != NULL) {
        *map = s_map;
    }
}

esp_err_t read_pico_buzzer_duty_map_set(const buzzer_duty_map_t *map) {
    if (!buzzer_duty_map_valid(map)) {
        return ESP_ERR_INVALID_ARG;
    }
    s_map = *map;
    return ESP_OK;
}

uint32_t read_pico_buzzer_pwm_carrier_hz_nominal(void) {
    uint32_t freq = (uint32_t)APB_CLK_FREQ / (1u << 10);
    return freq - (freq % 1000u);
}

uint32_t read_pico_buzzer_pwm_carrier_hz(void) {
    return s_carrier_hz;
}

bool read_pico_buzzer_score_busy(void) {
    return s_score_playing;
}

static esp_err_t write_hw(const int16_t *pcm, int n, bool allow_stop);
static esp_err_t ledc_tone_init(void);

typedef int (*pcm_fill_fn)(int16_t *dst, int n, void *ctx);

// 占空比斜坡。total 个采样点，形状可选：
//   线性     —— 开声用。0→bias 之后紧跟一声"嗒"，斜坡本身被掩蔽，形状无所谓。
//   平滑     —— 收声用。用五次多项式 u³(6u²-15u+10)：两端一阶、二阶导都为 0，
//                 即**速度与加速度都连续**。线圈在 bias 上是半通电的，占空比一步跨到
//                 0 等于电流阶跃，膜片被"拽"一下，就是耳里那声"噗"。smoothstep
//                 只保证两端速度为 0，加速度仍有跳变(就是那声闷响的残余)；五次曲线
//                 把加速度也压平，膜片走的是"起-走-停"都没有折点的路。
// 时长同样重要：越慢峰值加速度越低(∝ 1/T²)。收声路径给 500ms 起步，空闲收声
// 更是拉到秒级(见 read_pico_buzzer_pcm_close_fade)。
static esp_err_t write_duty_ramp_n(uint16_t from, uint16_t to, int total, bool smooth, bool allow_stop) {
    if (total < 2) {
        total = 2;
    }
    int done = 0;
    while (done < total) {
        int n = total - done;
        if (n > CHUNK) {
            n = CHUNK;
        }
        for (int i = 0; i < n; i++) {
            int idx = done + i;
            double u = (double)idx / (double)(total - 1);
            if (smooth) {
                // 五次平滑 u³(6u²-15u+10)：u=0/1 处一阶、二阶导都是 0。
                u = u * u * u * (u * (u * 6.0 - 15.0) + 10.0);
            }
            uint16_t d = (uint16_t)((int32_t)from + (int32_t)((double)((int32_t)to - (int32_t)from) * u));
            buzzer_duty_accum_add(&s_duty_acc, d, 0);
            s_hw[i] = buzzer_duty_to_pwm_audio_pcm(d);
        }
        esp_err_t err = write_hw(s_hw, n, allow_stop);
        if (err != ESP_OK) {
            return err;
        }
        done += n;
    }
    s_pcm_duty = to;
    return ESP_OK;
}

static esp_err_t write_duty_ramp(uint16_t from, uint16_t to, bool allow_stop) {
    return write_duty_ramp_n(from, to, FADE_N, false, allow_stop);
}

static esp_err_t write_hw(const int16_t *pcm, int n, bool allow_stop) {
    const uint8_t *p = (const uint8_t *)pcm;
    size_t left = (size_t)n * sizeof(int16_t);
    while (left > 0) {
        if (allow_stop && s_score_stop) {
            return ESP_ERR_INVALID_STATE;
        }
        size_t wrote = 0;
        esp_err_t err = pwm_audio_write((uint8_t *)p, left, &wrote, pdMS_TO_TICKS(WRITE_WAIT_MS));
        if (err != ESP_OK || wrote == 0) {
            s_underrun++;
            if (s_underrun > 8) {
                return err != ESP_OK ? err : ESP_FAIL;
            }
            continue;
        }
        p += wrote;
        left -= wrote;
    }
    return ESP_OK;
}

static esp_err_t submit_pcm(const int16_t *pcm, int n, bool allow_stop) {
    for (int i = 0; i < n; i++) {
        int clip = 0;
        uint16_t d = buzzer_duty_from_sample_ex(&s_map, pcm[i], &clip);
        buzzer_duty_accum_add(&s_duty_acc, d, clip);
        s_hw[i] = buzzer_duty_to_pwm_audio_pcm(d);
        s_pcm_duty = d;   // 收声时从"最后一个采样停在哪"开始淡出
    }
    return write_hw(s_hw, n, allow_stop);
}

static void finish_out_stats(uint8_t path, uint32_t carrier_hz) {
    buzzer_duty_accum_result(
        &s_duty_acc, &s_out.duty_min, &s_out.duty_max, &s_out.duty_mean_x100,
        &s_out.duty_ac_rms_x100, &s_out.samples, &s_out.duty_clips
    );
    s_out.carrier_hz = carrier_hz;
    s_out.resolution_bits = 10;
    s_out.path = path;
    s_out.underruns = s_underrun;
    s_out.pcm_peak = s_stats.peak;
    s_out.pcm_rms = buzzer_score_pcm_rms(&s_stats);
    s_out.current_ma_valid = 0;
    s_out.current_ma = 0;
    s_out.shift_st = buzzer_score_semitone_shift();
    s_out.duty_fs = LEDC_DUTY_FS;
    s_out.duty_hw_max = PWM_AUDIO_DUTY_MAX;
    s_out.residual_low_ticks = path == READ_PICO_BUZZER_PATH_HF ? 1 : 0;
}

static const char *timbre_tag(void) {
    switch (buzzer_score_timbre()) {
        case BUZZER_SCORE_TIMBRE_BRIGHT:
            return "bright";
        case BUZZER_SCORE_TIMBRE_ENHANCE:
            return "enhance";
        default:
            return "soft";
    }
}

static const char *staff_tag(void) {
    uint8_t m = buzzer_score_staff_mask();
    if (m == BUZZER_SCORE_STAFF_RH) {
        return "rh";
    }
    if (m == BUZZER_SCORE_STAFF_LH) {
        return "lh";
    }
    return "both";
}

static const char *env_tag(void) {
    return buzzer_score_env() == BUZZER_SCORE_ENV_HOLD ? "hold" : "piano";
}

static const char *path_tag(void) {
    if (s_out.path == READ_PICO_BUZZER_PATH_DIRECT_1BIT) {
        return "1bit";
    }
    return s_out.path == READ_PICO_BUZZER_PATH_HF ? "hf" : "classic";
}

static uint32_t sounding_hz(uint8_t midi, int8_t shift) {
    int m = (int)midi + (int)shift;
    if (m < 0) {
        m = 0;
    } else if (m > 127) {
        m = 127;
    }
    return (uint32_t)(((uint64_t)buzzer_score_midi_inc((uint8_t)m) * (uint64_t)BUZZER_SCORE_SR) >> 32);
}

static void log_out_stats(const char *tag) {
    ESP_LOGI(
        TAG,
        "%s path=%s shift_st=%d E5_sounding_hz=%" PRIu32
        " tone_hz=%" PRIu32 " tone_hz_actual=%" PRIu32 " voices=%u"
        " pcm_peak=%" PRId32 " pcm_rms=%" PRIu32
        " duty min=%u max=%u fs=%u hw_max=%u residual_low_ticks=%u"
        " mean=%" PRIu32 ".%02" PRIu32 " ac_rms=%" PRIu32 ".%02" PRIu32
        " clips=%" PRIu32 " pwm=%" PRIu32 "Hz/%ubit n=%" PRIu32
        " I=unverified (scope GPIO2/gate; software is not a measurement)",
        tag, path_tag(), (int)s_out.shift_st, sounding_hz(MIDI_E5, s_out.shift_st),
        s_out.tone_hz, s_out.tone_hz_actual, (unsigned)s_out.voices,
        s_out.pcm_peak, s_out.pcm_rms, s_out.duty_min, s_out.duty_max, s_out.duty_fs,
        s_out.duty_hw_max, (unsigned)s_out.residual_low_ticks,
        s_out.duty_mean_x100 / 100, s_out.duty_mean_x100 % 100, s_out.duty_ac_rms_x100 / 100,
        s_out.duty_ac_rms_x100 % 100, s_out.duty_clips, s_out.carrier_hz,
        (unsigned)s_out.resolution_bits, s_out.samples
    );
}

static void score_teardown(void) {
    if (s_pwm_up) {
        pwm_audio_status_t st;
        if (pwm_audio_get_status(&st) == ESP_OK && st == PWM_AUDIO_STATUS_BUSY) {
            pwm_audio_stop();
        }
        pwm_audio_deinit();
        s_pwm_up = false;
    }
    gpio_idle();
    s_ledc_ready = false;
}

static esp_err_t score_setup(uint32_t rate) {
    if (s_ledc_ready) {
        ledc_stop(LEDC_LOW_SPEED_MODE, BUZZER_CHANNEL, 0);
        s_ledc_ready = false;
    }
    gpio_idle();

    pwm_audio_config_t cfg = {
        .gpio_num_left = BUZZER_GPIO,
        .gpio_num_right = -1,
        .ledc_channel_left = LEDC_CHANNEL_0,
        .ledc_channel_right = LEDC_CHANNEL_1,
        .ledc_timer_sel = LEDC_TIMER_0,
        .duty_resolution = PWM_RES,
        .ringbuf_len = RINGBUF_LEN,
    };
    esp_err_t err = pwm_audio_init(&cfg);
    if (err != ESP_OK) {
        gpio_idle();
        return err;
    }
    ledc_set_duty(LEDC_LOW_SPEED_MODE, BUZZER_CHANNEL, 0);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, BUZZER_CHANNEL);

    err = pwm_audio_set_param((int)rate, 16, 1);
    if (err != ESP_OK) {
        pwm_audio_deinit();
        gpio_idle();
        return err;
    }
    pwm_audio_set_volume(0);
    err = pwm_audio_start();
    if (err != ESP_OK) {
        pwm_audio_deinit();
        gpio_idle();
        return err;
    }
    s_carrier_hz = ledc_get_freq(LEDC_LOW_SPEED_MODE, BUZZER_TIMER);
    s_pwm_up = true;
    return ESP_OK;
}

static void note_pcm(int16_t s) {
    int32_t a = s < 0 ? -s : s;
    if (a > s_stats.peak) {
        s_stats.peak = a;
    }
    s_stats.sum_sq += (uint64_t)((int32_t)s * (int32_t)s);
    s_stats.samples++;
}

static esp_err_t play_hf_pcm_loop(pcm_fill_fn fill, void *ctx, bool count_pcm) {
    esp_err_t err = score_setup(BUZZER_SCORE_SR);
    if (err != ESP_OK || s_score_stop) {
        return err != ESP_OK ? err : ESP_ERR_INVALID_STATE;
    }
    err = write_duty_ramp(0, s_map.bias, true);
    while (err == ESP_OK && !s_score_stop) {
        int n = fill(s_chunk, CHUNK, ctx);
        if (n <= 0) {
            break;
        }
        if (count_pcm) {
            for (int i = 0; i < n; i++) {
                note_pcm(s_chunk[i]);
            }
        }
        err = submit_pcm(s_chunk, n, true);
    }
    return err;
}

static int fill_score(int16_t *dst, int n, void *ctx) {
    (void)ctx;
    if (!buzzer_score_render(dst, n, &s_stats)) {
        return 0;
    }
    return n;
}

static void play_classic(uint32_t hz, const char *tag) {
    gpio_idle();
    if (s_ledc_ready) {
        ledc_stop(LEDC_LOW_SPEED_MODE, BUZZER_CHANNEL, 0);
        s_ledc_ready = false;
    }
    esp_err_t err = ledc_tone_init();
    if (err != ESP_OK) {
        gpio_idle();
        ESP_LOGE(TAG, "%s %s", tag, esp_err_to_name(err));
        return;
    }
    ledc_set_freq(LEDC_LOW_SPEED_MODE, BUZZER_TIMER, hz);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, BUZZER_CHANNEL, LEDC_DUTY_FS / 2);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, BUZZER_CHANNEL);
    uint32_t actual = ledc_get_freq(LEDC_LOW_SPEED_MODE, BUZZER_TIMER);
    ESP_LOGI(
        TAG,
        "%s classic 50%% square req_hz=%" PRIu32 " actual_hz=%" PRIu32
        " duty=512/%u (LEDC allows 0..%u; 1023 is not constant high)",
        tag, hz, actual, (unsigned)LEDC_DUTY_FS, (unsigned)LEDC_DUTY_FS
    );
    if (!s_score_stop) {
        vTaskDelay(pdMS_TO_TICKS(PROBE_MS));
    }
    ledc_set_duty(LEDC_LOW_SPEED_MODE, BUZZER_CHANNEL, 0);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, BUZZER_CHANNEL);
    gpio_idle();
    s_ledc_ready = false;

    buzzer_duty_accum_reset(&s_duty_acc);
    buzzer_duty_accum_add(&s_duty_acc, LEDC_DUTY_FS / 2, 0);
    finish_out_stats(READ_PICO_BUZZER_PATH_CLASSIC, actual);
    s_out.tone_hz = hz;
    s_out.tone_hz_actual = actual;
    s_out.voices = 1;
    s_out.samples = (uint32_t)PROBE_MS * actual / 1000u;
    s_out.pcm_peak = 0;
    s_out.pcm_rms = 0;
    s_out.residual_low_ticks = 0;
    log_out_stats(tag);
    if (hz == PROBE_4K_HZ) {
        ESP_LOGI(TAG, "4k ref: not an efficiency comparison vs 1.5k A/B");
    }
}

static void play_score_job(void) {
    buzzer_score_init();
    buzzer_score_reset(&s_stats);
    buzzer_duty_accum_reset(&s_duty_acc);
    s_underrun = 0;

    int32_t gain = buzzer_score_prepare(&s_map, &s_score_stop);
    int32_t target = buzzer_duty_legal_pcm(&s_map);
    ESP_LOGI(
        TAG,
        "prepare timbre=%s env=%s staff=%s shift_st=%d E5_sounding_hz=%" PRIu32
        " raw_peak=%" PRId32 " gain_q12=%" PRId32 " pcm_target=%" PRId32
        " (pcm ±target already near full duty swing; not raising gain)"
        " duty_fs=%u hw_max=%u residual_low_ticks=1",
        timbre_tag(), env_tag(), staff_tag(), (int)buzzer_score_semitone_shift(),
        sounding_hz(MIDI_E5, buzzer_score_semitone_shift()),
        buzzer_score_raw_peak(), gain, target, (unsigned)LEDC_DUTY_FS,
        (unsigned)PWM_AUDIO_DUTY_MAX
    );
    if (s_score_stop || gain <= 0) {
        finish_out_stats(READ_PICO_BUZZER_PATH_HF, s_carrier_hz);
        log_out_stats("score");
        return;
    }
    buzzer_score_reset(&s_stats);
    esp_err_t err = play_hf_pcm_loop(fill_score, NULL, false);

    const bool emergency = (s_score_stop || (err != ESP_OK && err != ESP_ERR_INVALID_STATE));
    if (!emergency && s_pwm_up) {
        (void)write_duty_ramp(s_map.bias, 0, false);
        vTaskDelay(pdMS_TO_TICKS(DRAIN_MS));
    }
    uint32_t carrier = s_carrier_hz;
    score_teardown();
    finish_out_stats(READ_PICO_BUZZER_PATH_HF, carrier);
    s_out.pcm_peak = s_stats.peak;
    s_out.pcm_rms = buzzer_score_pcm_rms(&s_stats);
    s_out.tone_hz = sounding_hz(MIDI_E5, buzzer_score_semitone_shift());
    s_out.tone_hz_actual = s_out.tone_hz;
    s_out.voices = 0;
    log_out_stats("score");
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "score %s", esp_err_to_name(err));
    }
}

static void play_pen_b(void) {
    buzzer_duty_accum_reset(&s_duty_acc);
    s_underrun = 0;
    memset(&s_stats, 0, sizeof(s_stats));

    esp_err_t err = score_setup(BUZZER_SCORE_SR);
    uint32_t carrier = s_carrier_hz;
    ESP_LOGI(
        TAG,
        "probeB hf duty-square req_hz=%u sr=%u carrier=%" PRIu32
        "Hz duty 0 <-> %u (LEDC full-on is %u; pwm_audio max %u leaves residual_low=1 tick/period)",
        (unsigned)PROBE_15K_HZ, (unsigned)BUZZER_SCORE_SR, carrier, (unsigned)PEN_B_DUTY_HI,
        (unsigned)LEDC_DUTY_FS, (unsigned)PWM_AUDIO_DUTY_MAX
    );

    uint32_t left = (uint32_t)BUZZER_SCORE_SR * PROBE_MS / 1000u;
    uint32_t phase = 0;
    uint32_t inc = (uint32_t)(((uint64_t)PROBE_15K_HZ << 32) / (uint64_t)BUZZER_SCORE_SR);
    while (err == ESP_OK && !s_score_stop && left > 0) {
        int n = CHUNK;
        if ((uint32_t)n > left) {
            n = (int)left;
        }
        for (int i = 0; i < n; i++) {
            uint16_t d = (phase < 0x80000000u) ? PEN_B_DUTY_HI : 0;
            buzzer_duty_accum_add(&s_duty_acc, d, 0);
            s_hw[i] = buzzer_duty_to_pwm_audio_pcm(d);
            s_chunk[i] = d != 0 ? 32767 : -32767;
            note_pcm(s_chunk[i]);
            phase += inc;
        }
        err = write_hw(s_hw, n, true);
        left -= (uint32_t)n;
    }

    score_teardown();
    finish_out_stats(READ_PICO_BUZZER_PATH_HF, carrier);
    s_out.tone_hz = PROBE_15K_HZ;
    s_out.tone_hz_actual = PROBE_15K_HZ;
    s_out.voices = 1;
    s_out.pcm_peak = s_stats.peak;
    s_out.pcm_rms = buzzer_score_pcm_rms(&s_stats);
    s_out.residual_low_ticks = 1;
    log_out_stats("probeB");
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "probeB %s", esp_err_to_name(err));
    }
}

static void rmt_put(rmt_symbol_word_t *out, int *o, int max_out, int lvl0, uint32_t d0, int lvl1, uint32_t d1) {
    if (*o >= max_out) {
        return;
    }
    if (d0 == 0) {
        d0 = 1;
    }
    if (d1 == 0) {
        d1 = 1;
    }
    if (d0 > 32767u) {
        d0 = 32767u;
    }
    if (d1 > 32767u) {
        d1 = 32767u;
    }
    out[*o].level0 = (uint32_t)lvl0;
    out[*o].duration0 = (uint16_t)d0;
    out[*o].level1 = (uint32_t)lvl1;
    out[*o].duration1 = (uint16_t)d1;
    (*o)++;
}

static int sym_to_rmt(const buzzer_1bit_sym_t *in, int n, rmt_symbol_word_t *out, int max_out) {
    int o = 0;
    for (int i = 0; i < n && o < max_out; i++) {
        uint32_t hi = in[i].high_us;
        uint32_t lo = in[i].low_us;
        while (hi > 32767u && o < max_out) {
            rmt_put(out, &o, max_out, 1, 32767u, 1, 1);
            hi -= (hi > 32768u) ? 32768u : hi;
        }
        if (hi > 0 && o < max_out) {
            uint32_t b = lo > 32767u ? 32767u : lo;
            if (b == 0) {
                b = 1;
            } else {
                lo -= b;
            }
            rmt_put(out, &o, max_out, 1, hi, 0, b);
        }
        while (lo > 0 && o < max_out) {
            uint32_t b = lo > 32767u ? 32767u : lo;
            lo -= b;
            rmt_put(out, &o, max_out, 0, b, 0, 1);
        }
    }
    if (o > 0 && (o & 1)) {
        rmt_put(out, &o, max_out, 0, 1, 0, 1);
    }
    return o;
}

static void play_1bit(void) {
    uint8_t n = s_1bit_voices;
    if (n != 2 && n != 4 && n != 6) {
        n = 1;
    }
    uint16_t w[BUZZER_1BIT_VOICES_MAX];
    for (int i = 0; i < BUZZER_1BIT_VOICES_MAX; i++) {
        w[i] = 256;
    }
    buzzer_1bit_cfg_t cfg = BUZZER_1BIT_CFG_DEFAULT;
    buzzer_1bit_sym_t ev[BUZZER_1BIT_SYM_MAX];
    buzzer_1bit_stats_t st;
    int nev = buzzer_1bit_build(k_1bit_hz, w, n, &cfg, BIT1_WINDOW_US, ev, BUZZER_1BIT_SYM_MAX, &st);

    static rmt_symbol_word_t items[BUZZER_1BIT_SYM_MAX * 2];
    int nitem = sym_to_rmt(ev, nev, items, (int)(sizeof(items) / sizeof(items[0])));

    ESP_LOGI(
        TAG,
        "1bit voices=%u hz=%u%s pulse_us=%u max_slip_us=%u window_us=%u"
        " emitted=%" PRIu32 " dropped=%" PRIu32 " serialized=%" PRIu32
        " merged=%" PRIu32 " high_us=%" PRIu32 "/%" PRIu32,
        (unsigned)n, (unsigned)k_1bit_hz[0], n > 1 ? "+…" : "", (unsigned)cfg.pulse_us,
        (unsigned)cfg.max_slip_us, (unsigned)BIT1_WINDOW_US, st.emitted, st.dropped,
        st.serialized, st.merged_high, st.high_us, st.window_us
    );
    for (uint8_t i = 0; i < n; i++) {
        ESP_LOGI(TAG, "1bit voice%u %uHz", (unsigned)i, (unsigned)k_1bit_hz[i]);
    }

    gpio_idle();
    if (nitem <= 0) {
        ESP_LOGE(TAG, "1bit empty schedule");
        return;
    }

    const int rmt_ram = 48;
    bool use_dma = nitem > rmt_ram;
    if (use_dma) {
        ESP_LOGW(TAG, "1bit nitem=%d > RMT RAM %d, trying DMA", nitem, rmt_ram);
    }

    rmt_channel_handle_t chan = NULL;
    rmt_encoder_handle_t enc = NULL;
    rmt_tx_channel_config_t tcfg = {
        .gpio_num = BUZZER_GPIO,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = BIT1_RMT_HZ,
        .mem_block_symbols = use_dma ? 128 : (size_t)rmt_ram,
        .trans_queue_depth = 4,
        .flags = {
            .with_dma = use_dma ? 1u : 0u,
            .init_level = 0,
        },
    };
    esp_err_t err = rmt_new_tx_channel(&tcfg, &chan);
    if (err != ESP_OK && use_dma) {
        ESP_LOGW(TAG, "1bit DMA %s, truncate to %d (no bit-bang)", esp_err_to_name(err), rmt_ram);
        nitem = rmt_ram;
        if (nitem & 1) {
            nitem--;
        }
        use_dma = false;
        tcfg.mem_block_symbols = (size_t)rmt_ram;
        tcfg.flags.with_dma = 0;
        err = rmt_new_tx_channel(&tcfg, &chan);
    }
    if (err != ESP_OK) {
        gpio_idle();
        ESP_LOGE(TAG, "1bit rmt_new_tx_channel %s (no bit-bang fallback)", esp_err_to_name(err));
        return;
    }
    rmt_copy_encoder_config_t ecfg = {};
    err = rmt_new_copy_encoder(&ecfg, &enc);
    if (err != ESP_OK) {
        rmt_del_channel(chan);
        gpio_idle();
        ESP_LOGE(TAG, "1bit encoder %s", esp_err_to_name(err));
        return;
    }
    err = rmt_enable(chan);
    if (err != ESP_OK) {
        rmt_del_encoder(enc);
        rmt_del_channel(chan);
        gpio_idle();
        ESP_LOGE(TAG, "1bit enable %s", esp_err_to_name(err));
        return;
    }

    rmt_transmit_config_t xcfg = {
        .loop_count = -1,
        .flags = { .eot_level = 0 },
    };
    err = rmt_transmit(chan, enc, items, (size_t)nitem * sizeof(items[0]), &xcfg);
    if (err == ESP_OK && !s_score_stop) {
        vTaskDelay(pdMS_TO_TICKS(PROBE_MS));
    }
    rmt_disable(chan);
    rmt_del_encoder(enc);
    rmt_del_channel(chan);
    gpio_idle();

    uint32_t duty_x1000 = st.window_us ? (st.high_us * 1000u / st.window_us) : 0;
    buzzer_duty_accum_reset(&s_duty_acc);
    finish_out_stats(READ_PICO_BUZZER_PATH_DIRECT_1BIT, BIT1_RMT_HZ);
    s_out.tone_hz = k_1bit_hz[0];
    s_out.tone_hz_actual = k_1bit_hz[0];
    s_out.voices = n;
    s_out.pcm_peak = 0;
    s_out.pcm_rms = 0;
    s_out.duty_min = 0;
    s_out.duty_max = 1;
    s_out.duty_mean_x100 = duty_x1000 / 10;
    s_out.residual_low_ticks = 0;
    s_out.resolution_bits = 1;
    log_out_stats("1bit");
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "1bit %s", esp_err_to_name(err));
    }

    if (s_1bit_voices == 1) {
        s_1bit_voices = 2;
    } else if (s_1bit_voices == 2) {
        s_1bit_voices = 4;
    } else if (s_1bit_voices == 4) {
        s_1bit_voices = 6;
    } else {
        s_1bit_voices = 1;
    }
}

static void score_task(void *arg) {
    (void)arg;
    if (s_lock == NULL || xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) != pdTRUE) {
        s_score_playing = false;
        s_score_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    memset(&s_out, 0, sizeof(s_out));
    memset(&s_stats, 0, sizeof(s_stats));
    s_underrun = 0;
    buzzer_score_init();

    if (s_job == JOB_A) {
        play_classic(PROBE_15K_HZ, "probeA");
    } else if (s_job == JOB_B) {
        play_pen_b();
    } else if (s_job == JOB_1BIT) {
        play_1bit();
    } else if (s_job == JOB_4K) {
        play_classic(PROBE_4K_HZ, "probe4k");
    } else {
        play_score_job();
    }

    s_score_playing = false;
    s_score_task = NULL;
    xSemaphoreGive(s_lock);
    vTaskDelete(NULL);
}

static esp_err_t start_job(uint8_t job) {
    lock_once();
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (xSemaphoreTake(s_lock, 0) != pdTRUE) {
        return s_score_playing ? ESP_OK : ESP_ERR_INVALID_STATE;
    }
    if (s_score_playing) {
        xSemaphoreGive(s_lock);
        return ESP_OK;
    }
    s_job = job;
    s_score_stop = false;
    s_score_playing = true;
    xSemaphoreGive(s_lock);
    BaseType_t ok = xTaskCreatePinnedToCore(score_task, "bz_job", 6144, NULL, 4, &s_score_task, 1);
    if (ok != pdPASS) {
        s_score_playing = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t read_pico_buzzer_score_play(void) {
    return start_job(JOB_SCORE);
}

esp_err_t read_pico_buzzer_probe(read_pico_buzzer_probe_t probe) {
    switch (probe) {
        case READ_PICO_BUZZER_PROBE_A_15K:
            return start_job(JOB_A);
        case READ_PICO_BUZZER_PROBE_B_15K:
            return start_job(JOB_B);
        case READ_PICO_BUZZER_PROBE_1BIT:
            return start_job(JOB_1BIT);
        case READ_PICO_BUZZER_PROBE_4K:
            return start_job(JOB_4K);
        default:
            return ESP_ERR_INVALID_ARG;
    }
}

void read_pico_buzzer_out_stats_get(read_pico_buzzer_out_stats_t *st) {
    if (st != NULL) {
        *st = s_out;
    }
}

esp_err_t read_pico_buzzer_score_stop(void) {
    if (!s_score_playing) {
        return ESP_OK;
    }
    s_score_stop = true;
    TickType_t t0 = xTaskGetTickCount();
    while (s_score_playing) {
        if ((xTaskGetTickCount() - t0) > pdMS_TO_TICKS(STOP_WAIT_MS)) {
            gpio_idle();
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(1);
    }
    return ESP_OK;
}

esp_err_t read_pico_buzzer_effect(void) {
    return read_pico_buzzer_score_play();
}

static esp_err_t ledc_tone_init(void) {
    if (s_ledc_ready) {
        return ESP_OK;
    }
    gpio_idle();
    const ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = BUZZER_TIMER,
        .freq_hz = 2700,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    esp_err_t err = ledc_timer_config(&timer);
    if (err != ESP_OK) {
        gpio_idle();
        return err;
    }
    const ledc_channel_config_t channel = {
        .gpio_num = BUZZER_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = BUZZER_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = BUZZER_TIMER,
        .duty = 0,
        .hpoint = 0,
    };
    err = ledc_channel_config(&channel);
    if (err != ESP_OK) {
        gpio_idle();
        return err;
    }
    s_ledc_ready = true;
    return ESP_OK;
}

static esp_err_t tone_impl(uint32_t frequency_hz, uint32_t duration_ms, uint32_t duty) {
    if (frequency_hz == 0 || duration_ms == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_score_playing) {
        return ESP_ERR_INVALID_STATE;
    }
    lock_once();
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (xSemaphoreTake(s_lock, 0) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = ledc_tone_init();
    if (err != ESP_OK) {
        xSemaphoreGive(s_lock);
        return err;
    }
    if (frequency_hz < 70) {
        frequency_hz = 70;
    }
    if (frequency_hz > 6000) {
        frequency_hz = 6000;
    }
    ledc_set_freq(LEDC_LOW_SPEED_MODE, BUZZER_TIMER, frequency_hz);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, BUZZER_CHANNEL, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, BUZZER_CHANNEL);
    vTaskDelay(pdMS_TO_TICKS(duration_ms));
    ledc_set_duty(LEDC_LOW_SPEED_MODE, BUZZER_CHANNEL, 0);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, BUZZER_CHANNEL);
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t read_pico_buzzer_tone(uint32_t frequency_hz, uint32_t duration_ms) {
    return tone_impl(frequency_hz, duration_ms, 512);
}

esp_err_t read_pico_buzzer_tone_vol(uint32_t frequency_hz, uint32_t duration_ms, uint8_t vol_pct) {
    // 10-bit 占空比：512 = 50% 方波 = 基频幅度最大。音量按占空比线性缩小到 0..512。
    uint32_t duty = (vol_pct >= 100) ? 512 : (512 * (uint32_t)vol_pct) / 100;
    return tone_impl(frequency_hz, duration_ms, duty);
}

int32_t read_pico_buzzer_pcm_legal_peak(void) {
    return buzzer_duty_legal_pcm(&s_map);
}

esp_err_t read_pico_buzzer_pcm_open(uint32_t rate) {
    if (rate == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    lock_once();
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (xSemaphoreTake(s_lock, 0) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;  // 忙(曲谱/别的会话在播)
    }
    s_score_stop = false;  // 清掉上一轮 stop 留下的标志，本次从头开始
    s_pcm_rate = rate;
    esp_err_t err = score_setup(rate);
    if (err != ESP_OK) {
        xSemaphoreGive(s_lock);
        return err;
    }
    s_pcm_open = true;
    // 开声也要一段平滑的 0→bias。score_setup 把占空比归 0，会话里第一个采样又回到
    // bias(静音电平)，中间就是一次电流阶跃 —— 和收声那声"噗"是同一回事，只是符号
    // 相反。25ms 的 smoothstep 短到听不出延迟(第一声"嗒"只晚 25ms)，两端斜率又为 0，
    // 听感上就是"没有"。
    (void)write_duty_ramp_n(0, s_map.bias, (int)(rate / 40), true, false);
    return ESP_OK;
}

esp_err_t read_pico_buzzer_pcm_write(const int16_t *pcm, int frames) {
    if (!s_pcm_open) {
        return ESP_ERR_INVALID_STATE;  // 没 open，或已被 close/打断
    }
    if (pcm == NULL || frames <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = ESP_OK;
    int off = 0;
    while (err == ESP_OK && !s_score_stop && off < frames) {
        int n = frames - off;
        if (n > CHUNK) {
            n = CHUNK;
        }
        err = submit_pcm(pcm + off, n, true);  // allow_stop: score_stop 可提前打断
        off += n;
    }
    if (err == ESP_OK && s_score_stop) {
        err = ESP_ERR_INVALID_STATE;  // 被外部 stop 打断
    }
    return err;
}

esp_err_t read_pico_buzzer_pcm_close(void) {
    return read_pico_buzzer_pcm_close_fade(0);
}

// fade_ms = 0 取默认 500ms；空闲收声传秒级的值，把"膜片回位"压到听不见。
// 淡出起点是会话里最后写出的那个占空比(s_pcm_duty)，不是静音电平——被打断的
// 会话可能停在别处，从 bias 起淡出会先来一次向上的阶跃。
esp_err_t read_pico_buzzer_pcm_close_fade(uint32_t fade_ms) {
    if (!s_pcm_open) {
        return ESP_OK;  // 已经关着，no-op
    }
    // 淡出到 0，再等足满环形缓冲排空(最坏 8KB/2=4096 帧)。淡出本身也占缓冲，
    // 所以排空上界就是 4096 帧 / rate；这里再加一点余量，避免截断爆音。
    //
    // 形状用五次平滑(见 write_duty_ramp_n)：bias 是半通电状态，到 0 是一次电流
    // 阶跃，在音圈上就是一声"噗"。两端速度、加速度都连续之后膜片只是慢慢回位。
    // (写这些帧本身就吃掉整段时长 —— write_hw 会等环形缓冲腾地方，所以它自己
    // 就是节拍器，不需要额外 delay。)
    int fade_n = (fade_ms == 0) ? (int)(s_pcm_rate / 2) : (int)((uint64_t)s_pcm_rate * fade_ms / 1000);
    if (fade_n < FADE_N) {
        fade_n = FADE_N;
    }
    // 埋点：淡出起点是"会话最后一个采样停在哪"。零偏置映射下静音本来就是占空比 0，
    // 所以这个值应该贴着 0（±几十）。要是日志里它是 500 上下，说明映射没换成零偏置
    // (甲类的静音电平 = bias = 512)，那这 500→0 的淡出本身就是一次直流撤除 —— 空闲
    // 后那声"嗒"就是它。
    const uint16_t from = s_pcm_duty;
    ESP_LOGI(TAG, "pcm close: fade %u->0 n=%d bias=%u gain=%u", (unsigned)from, fade_n,
             (unsigned)s_map.bias, (unsigned)s_map.gain);
    (void)write_duty_ramp_n(from, 0, fade_n, true, false);
    uint32_t drain_ms = (RINGBUF_LEN / 2) * 1000u / s_pcm_rate + 20;
    vTaskDelay(pdMS_TO_TICKS(drain_ms));
    score_teardown();
    s_pcm_open = false;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}
