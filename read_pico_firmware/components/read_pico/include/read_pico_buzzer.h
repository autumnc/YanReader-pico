/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * GPIO2 → AO3400A 栅极，高导通低关断。停播必须禁用 PWM 并拉低。
 * 曲谱合成是零中心 PCM；硬件占空比另经 bias/gain/min/max 映射。
 * 高频 PWM 默认不初始化、不上电自测。
 *
 * GPIO2 drives the AO3400A gate: high on, low off. Stop must disable PWM
 * and drive the pin low. Score synthesis is zero-centered PCM; hardware
 * duty is mapped separately via bias/gain/min/max. HF PWM is off by
 * default and is not a power-on self-test.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "buzzer_duty_map.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    READ_PICO_BUZZER_PATH_CLASSIC = 0,
    READ_PICO_BUZZER_PATH_HF = 1,
    READ_PICO_BUZZER_PATH_DIRECT_1BIT = 2,
} read_pico_buzzer_path_t;

typedef enum {
    READ_PICO_BUZZER_PROBE_A_15K = 0,
    READ_PICO_BUZZER_PROBE_B_15K = 1,
    READ_PICO_BUZZER_PROBE_1BIT = 2,
    READ_PICO_BUZZER_PROBE_4K = 3,
} read_pico_buzzer_probe_t;

typedef struct {
    uint32_t carrier_hz;
    uint8_t resolution_bits;
    uint8_t path;
    uint16_t duty_min;
    uint16_t duty_max;
    uint32_t duty_mean_x100;
    uint32_t duty_ac_rms_x100;
    uint32_t duty_clips;
    uint32_t underruns;
    uint32_t samples;
    int32_t pcm_peak;
    uint32_t pcm_rms;
    uint8_t current_ma_valid;
    int16_t current_ma;
    uint32_t tone_hz;
    uint32_t tone_hz_actual;
    int8_t shift_st;
    uint16_t duty_fs;
    uint16_t duty_hw_max;
    uint8_t residual_low_ticks;
    uint8_t voices;
} read_pico_buzzer_out_stats_t;

esp_err_t read_pico_buzzer_tone(uint32_t frequency_hz, uint32_t duration_ms);

// 同上，但额外带音量(0..100)映射到占空比；0 静音(仍占锁一瞬)。供打字音效等用。
esp_err_t read_pico_buzzer_tone_vol(uint32_t frequency_hz, uint32_t duration_ms, uint8_t vol_pct);

// 当前 duty 映射(s_map)下不削波的最大零中心 PCM 幅度。调用方用它把采样归一化到
// 满摆幅(bias±gain 刚好打满 duty 0..1023)。约 4652(bias 512 / gain 3600)。
int32_t read_pico_buzzer_pcm_legal_peak(void);

// —— 打字音 PCM 会话：open 一次、连续 write、最后 close，避免逐键 init/deinit ——
// 与单发播放不同：open 之后 pwm_audio 保持开启并持有 s_lock，直到 close 才淡出/排空/
// 拆掉。连续敲键共用同一会话，省去每次 pwm_audio 初始化/销毁(避免快速输入时
// gptimer/LEDC/GPIO 反复重建导致的不稳)，也让上一声的尾音自然排空、下一声接得上。
// open 忙(曲谱或别的会话在播)返回 ESP_ERR_INVALID_STATE；write 前必须先 open 成功。
esp_err_t read_pico_buzzer_pcm_open(uint32_t sample_rate_hz);

// 写一段零中心单声道 PCM(16-bit)到已打开的会话(实时节流)。需先 open；调用方应先把
// 幅度归一到 read_pico_buzzer_pcm_legal_peak()*音量，否则削波。
esp_err_t read_pico_buzzer_pcm_write(const int16_t *pcm, int frames);

// 关闭会话：淡出到 0、排空环形缓冲、teardown、释放 s_lock。未 open 时为 no-op(返回 OK)。
esp_err_t read_pico_buzzer_pcm_close(void);

// 同 close，但淡出时长可指定(fade_ms=0 用默认 500ms)。空闲收声这种没人注意的时刻
// 传秒级的值：占空比从"最后一个采样"经五次平滑推到 0，峰值加速度 ∝ 1/T²，慢一档
// 就低一档，几秒之后线圈回位在耳朵里就不存在了。fade_ms 只是下界，实际时长取
// max(fade_ms, FADE_N/rate)。阻塞到淡出写完 + 环形缓冲排空。
esp_err_t read_pico_buzzer_pcm_close_fade(uint32_t fade_ms);

esp_err_t read_pico_buzzer_score_play(void);
esp_err_t read_pico_buzzer_score_stop(void);
bool read_pico_buzzer_score_busy(void);
esp_err_t read_pico_buzzer_probe(read_pico_buzzer_probe_t probe);

void read_pico_buzzer_out_stats_get(read_pico_buzzer_out_stats_t *st);

uint32_t read_pico_buzzer_pwm_carrier_hz(void);
uint32_t read_pico_buzzer_pwm_carrier_hz_nominal(void);

void read_pico_buzzer_duty_map_get(buzzer_duty_map_t *map);
esp_err_t read_pico_buzzer_duty_map_set(const buzzer_duty_map_t *map);

esp_err_t read_pico_buzzer_effect(void);

#ifdef __cplusplus
}
#endif
