#pragma once

// Display pin definitions for ESP32-S3 RLCD-4.2
// (matching original 11_U8G2_Test naming)
#define LCD_WIDTH      400
#define LCD_HEIGHT     300

#define RLCD_MOSI_PIN  GPIO_NUM_12
#define RLCD_SCK_PIN   GPIO_NUM_11
#define RLCD_DC_PIN    GPIO_NUM_5
#define RLCD_CS_PIN    GPIO_NUM_40
#define RLCD_RST_PIN   GPIO_NUM_41

// I2C pins
#define ESP32_I2C_SDA_PIN  GPIO_NUM_13
#define ESP32_I2C_SCL_PIN  GPIO_NUM_14

// Button pins
#define PIN_BOOT     GPIO_NUM_0
#define PIN_USER_BTN GPIO_NUM_18

// SD card (SDMMC 1-line mode)
#define SDMMC_CLK_PIN  GPIO_NUM_38
#define SDMMC_CMD_PIN  GPIO_NUM_21
#define SDMMC_D0_PIN   GPIO_NUM_39

// Battery ADC (ADC1_CH3 = GPIO4, voltage divider 3:1)
#define BATTERY_ADC_CHAN ADC_CHANNEL_3

// 原 Waveshare S3-RLCD-4.2 板的 ES7210/ES8311 I2S 引脚定义已删除：Read Pico 没有
// 音频编解码器也没有麦克风，唯一发声器件是 GPIO2 蜂鸣器（走 pwm_audio，见
// typing_click.cpp）。原定义里的 8/9/10/16/45 还正好是这块板的 EPD 数据总线
// D4/D5/D6/D12/D15，留着早晚被误用。
