#include "lcd_driver.h"
#include "epdiy.h"

#include "../output_common/render_method.h"
#include "../output_common/rmt_compat.h"
#include "esp_heap_caps.h"
#include "esp_intr_alloc.h"
#include "hal/gpio_types.h"

#ifdef RENDER_METHOD_LCD

#include <assert.h>
#include <esp_idf_version.h>
#include <esp_log.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <esp_private/periph_ctrl.h>
#include <soc/clk_tree_defs.h>
#include <soc/gpio_sig_map.h>   // SIG_GPIO_OUT_IDX：把矩阵接线接回普通 GPIO 输出
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
#include <esp_clk_tree.h>
// esp-idf-configdep may strip esp_private headers; declare the needed API locally.
esp_err_t esp_clk_tree_enable_src(soc_module_clk_t clk_src, bool enable);
#endif

#include <driver/gpio.h>
#include <esp_check.h>
#include <esp_private/esp_gpio_reserve.h>   // esp_gpio_revoke：配 STV 前先收回自己上一轮占的位
#include <esp_err.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_rgb.h>
#include <esp_private/gdma.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <hal/dma_types.h>
#include <hal/gdma_ll.h>
#include <hal/gpio_hal.h>
#include <hal/lcd_hal.h>
#include <hal/lcd_ll.h>
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
#if __has_include(<rom/cache.h>)
#include <rom/cache.h>
#elif __has_include(<esp32s3/rom/cache.h>)
#include <esp32s3/rom/cache.h>
#endif
typedef struct {
    const shared_periph_module_t module;
    const int irq_id;
    const int data_sigs[LCD_LL_GET(RGB_BUS_WIDTH)];
    const int hsync_sig;
    const int vsync_sig;
    const int pclk_sig;
    const int de_sig;
    const int disp_sig;
} soc_lcd_rgb_signal_desc_t;
extern const soc_lcd_rgb_signal_desc_t soc_lcd_rgb_signals[LCD_LL_GET(RGB_PANEL_NUM)];
#else
#include <rom/cache.h>
#include <soc/lcd_periph.h>
#endif

#include "hal/gpio_hal.h"

gpio_hal_context_t hal = { .dev = GPIO_HAL_GET_HW(GPIO_PORT_0) };

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 5, 0)
#undef __DECLARE_RCC_ATOMIC_ENV
#endif

#define TAG "epdiy"

// In IDF 5.3.2+, lcd_periph_signals was renamed to lcd_periph_rgb_signals
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
#define LCD_PERIPH_SIG(member) soc_lcd_rgb_signals[0].member
#elif ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 3, 2)
#define LCD_PERIPH_SIG(member) lcd_periph_rgb_signals.panels[0].member
#else
#define LCD_PERIPH_SIG(member) lcd_periph_signals.panels[0].member
#endif

static inline int min(int x, int y) {
    return x < y ? x : y;
}
static inline int max(int x, int y) {
    return x > y ? x : y;
}

#define S3_LCD_PIN_NUM_BK_LIGHT -1
// #define S3_LCD_PIN_NUM_MODE           4

#define LINE_BATCH 1000
#define BOUNCE_BUF_LINES 4

#define RMT_CKV_CHAN RMT_COMPAT_CHANNEL_1

#if defined(CONFIG_ESP32S3_DATA_CACHE_LINE_SIZE)
#define EPDIY_DATA_CACHE_LINE_SIZE CONFIG_ESP32S3_DATA_CACHE_LINE_SIZE
#elif defined(CONFIG_DATA_CACHE_LINE_SIZE)
#define EPDIY_DATA_CACHE_LINE_SIZE CONFIG_DATA_CACHE_LINE_SIZE
#else
#define EPDIY_DATA_CACHE_LINE_SIZE 64
#endif

// spinlock for protecting the critical section at frame start
static portMUX_TYPE frame_start_spinlock = portMUX_INITIALIZER_UNLOCKED;

typedef struct {
    lcd_hal_context_t hal;
    intr_handle_t vsync_intr;
    intr_handle_t done_intr;

    frame_done_func_t frame_done_cb;
    line_cb_func_t line_source_cb;
    void* line_cb_payload;
    void* frame_cb_payload;

    int line_length_us;
    int line_time_01us;
    int line_cycles;
    int lcd_res_h;

    /// 实际发给 RMT 的 CKV 高电平时长，单位 0.1us。行时间会随 pclk 变化，
    /// 所以不能直接用 config 里那个按默认 pclk 标定的固定值。
    int ckv_high_time;

    LcdEpdConfig_t config;

    uint8_t* bounce_buffer[2];
    // size of a single bounce buffer
    size_t bb_size;
    size_t bb_eof_count;
    size_t batches;

    // Number of DMA descriptors that used to carry the frame buffer
    size_t num_dma_nodes;
    // DMA channel handle
    gdma_channel_handle_t dma_chan;
    // DMA descriptors pool
    dma_descriptor_t* dma_nodes;

    /// LCD peripheral source clock frequency (Hz), from clock tree when available.
    uint32_t src_clk_hz;

    /// The number of bytes in a horizontal display register line.
    int line_bytes;

    // With 8 bit bus width, we need a dummy cycle before the actual data,
    // because the LCD peripheral behaves weirdly.
    // Also see:
    // https://blog.adafruit.com/2022/06/14/esp32uesday-hacking-the-esp32-s3-lcd-peripheral/
    int dummy_bytes;

    /// The number of lines of the display
    int display_lines;
} s3_lcd_t;

static s3_lcd_t lcd = { 0 };

void IRAM_ATTR epd_lcd_line_source_cb(line_cb_func_t line_source, void* payload) {
    lcd.line_source_cb = line_source;
    lcd.line_cb_payload = payload;
}

void IRAM_ATTR epd_lcd_frame_done_cb(frame_done_func_t cb, void* payload) {
    lcd.frame_done_cb = cb;
    lcd.frame_cb_payload = payload;
}

static IRAM_ATTR bool fill_bounce_buffer(uint8_t* buffer) {
    bool task_awoken = false;

    for (int i = 0; i < BOUNCE_BUF_LINES; i++) {
        if (lcd.line_source_cb != NULL) {
            // 8-bit needs a true dummy byte in FIFO; 16-bit still needs a dummy cycle but the
            // first FIFO byte is already correct (read_pico / epdiy historical behavior).
            int buffer_offset = i * (lcd.line_bytes + lcd.dummy_bytes) + (lcd.dummy_bytes % 2);
            task_awoken |= lcd.line_source_cb(lcd.line_cb_payload, &buffer[buffer_offset]);
        } else {
            memset(&buffer[i * lcd.line_bytes], 0x00, lcd.line_bytes);
        }
    }
    return task_awoken;
}

static void IRAM_ATTR start_ckv_cycles(int cycles) {
    rmt_compat_tx_configure_finite_loop(RMT_CKV_CHAN, cycles);
    rmt_compat_tx_reset_mem(RMT_CKV_CHAN);
    rmt_compat_tx_start(RMT_CKV_CHAN);
}

/**
 * Build the RMT signal according to the timing set in the lcd object.
 */
// RMT 的时间单位是 0.1us（80MHz 时钟 8 分频）。config 里的 ckv_high_time 是按
// 默认 pclk 的行时间标定的，pclk 调高后行时间变短，高电平会顶满甚至超过一整行，
// 低电平算出负数——RMT 的 duration 是无符号字段，信号会彻底跑飞，面板收不到行
// 时钟，表现是画面完全不动而 MCU 侧毫无报错。所以这里给低电平留出下限，
// 让高电平跟着行时间一起收缩。
#define CKV_MIN_LOW_TIME 9

static void rebuild_line_geometry(int pclk_mhz) {
    if (pclk_mhz < 1) pclk_mhz = 1;
    const LcdLineTiming_t* t = &lcd.config.line;
    int end = t->line_end > 0 ? t->line_end : 4;
    lcd.line_cycles = lcd.lcd_res_h + t->le_high_time + t->line_front_porch + end;
    lcd.line_time_01us = (lcd.line_cycles * 10 + pclk_mhz / 2) / pclk_mhz;
    if (lcd.line_time_01us < 1) lcd.line_time_01us = 1;
    lcd.line_length_us = (lcd.line_time_01us + 9) / 10;
}

static void ckv_rmt_build_signal() {
    int total_time = lcd.line_time_01us;
    // 行时间还没算出来（init 阶段），等 epd_lcd_set_pixel_clock_MHz 再来构造
    if (total_time < 2 * CKV_MIN_LOW_TIME) {
        return;
    }

    int high_time = lcd.config.line.ckv_high_time;
    if (high_time > total_time - CKV_MIN_LOW_TIME) {
        high_time = total_time - CKV_MIN_LOW_TIME;
    }
    if (high_time < CKV_MIN_LOW_TIME) {
        high_time = CKV_MIN_LOW_TIME;
    }
    lcd.ckv_high_time = high_time;

    rmt_compat_write_single_item(
        RMT_CKV_CHAN, high_time, true, total_time - high_time, false, true
    );
}

/**
 * Configure the RMT peripheral for use as the CKV clock.
 */
static void init_ckv_rmt() {
    rmt_compat_reset_module();
    rmt_compat_enable_module(true);

    rmt_compat_enable_periph_clock(true);
    rmt_compat_set_group_clock_src(RMT_CKV_CHAN);
    rmt_compat_set_clock_div(RMT_CKV_CHAN, 8);
    rmt_compat_set_mem_blocks(RMT_CKV_CHAN, 2);
    rmt_compat_enable_mem_access_nonfifo(true);
    rmt_compat_tx_set_idle_level(RMT_CKV_CHAN, 0, true);
    rmt_compat_tx_enable_carrier(RMT_CKV_CHAN, false);
    rmt_compat_tx_enable_loop(RMT_CKV_CHAN, true);

    rmt_compat_connect_gpio(RMT_CKV_CHAN, lcd.config.bus.ckv);

    ckv_rmt_build_signal();
}

/**
 * Reset the CKV RMT configuration.
 */
static void deinit_ckv_rmt() {
    rmt_compat_reset_module();
    rmt_compat_enable_periph_clock(false);
    rmt_compat_enable_module(false);
    gpio_reset_pin(lcd.config.bus.ckv);
}

__attribute__((optimize("O3"))) IRAM_ATTR static void lcd_isr_vsync(void* args) {
    bool need_yield = false;

    uint32_t intr_status = lcd_ll_get_interrupt_status(lcd.hal.dev);
    lcd_ll_clear_interrupt_status(lcd.hal.dev, intr_status);

    if (intr_status & LCD_LL_EVENT_VSYNC_END) {
        // start_frame() already kicked off the first batch. This counter is the
        // number of *additional* full LINE_BATCH chunks still needed after that
        // first batch (integer division, not ceil). Using ceil made short panels
        // (e.g. 688 < 1000) run a duplicate second batch and roughly 2x frame time.
        int batches_needed = lcd.display_lines / LINE_BATCH;
        if (lcd.batches >= batches_needed) {
            lcd_ll_stop(lcd.hal.dev);
            if (lcd.frame_done_cb != NULL) {
                (*lcd.frame_done_cb)(lcd.frame_cb_payload);
            }
        } else {
            int ckv_cycles = 0;
            // last batch
            if (lcd.batches == batches_needed - 1) {
                int last_lines = lcd.display_lines % LINE_BATCH;
                if (last_lines == 0) {
                    last_lines = LINE_BATCH;
                }
                lcd_ll_enable_auto_next_frame(lcd.hal.dev, false);
                lcd_ll_set_vertical_timing(lcd.hal.dev, 1, 0, last_lines, 10);
                ckv_cycles = last_lines + 10;
            } else {
                lcd_ll_set_vertical_timing(lcd.hal.dev, 1, 0, LINE_BATCH, 1);
                ckv_cycles = LINE_BATCH + 1;
            }
            // apparently, this is needed for the new timing to take effect.
            lcd_ll_start(lcd.hal.dev);

            // skip the LCD front porch line, which is not actual data
            esp_rom_delay_us(lcd.line_length_us);
            start_ckv_cycles(ckv_cycles);
        }

        lcd.batches += 1;
    }

    if (need_yield) {
        portYIELD_FROM_ISR();
    }
};

// ISR handling bounce buffer refill
static IRAM_ATTR bool lcd_rgb_panel_eof_handler(
    gdma_channel_handle_t dma_chan, gdma_event_data_t* event_data, void* user_data
) {
    (void)dma_chan;
    (void)event_data;
    (void)user_data;

    int bb = lcd.bb_eof_count % 2;
    lcd.bb_eof_count++;
    return fill_bounce_buffer(lcd.bounce_buffer[bb]);
}

static esp_err_t init_dma_trans_link() {
    lcd.dma_nodes[0].dw0.suc_eof = 1;
    lcd.dma_nodes[0].dw0.size = lcd.bb_size;
    lcd.dma_nodes[0].dw0.length = lcd.bb_size;
    lcd.dma_nodes[0].dw0.owner = DMA_DESCRIPTOR_BUFFER_OWNER_CPU;
    lcd.dma_nodes[0].buffer = lcd.bounce_buffer[0];

    lcd.dma_nodes[1].dw0.suc_eof = 1;
    lcd.dma_nodes[1].dw0.size = lcd.bb_size;
    lcd.dma_nodes[1].dw0.length = lcd.bb_size;
    lcd.dma_nodes[1].dw0.owner = DMA_DESCRIPTOR_BUFFER_OWNER_CPU;
    lcd.dma_nodes[1].buffer = lcd.bounce_buffer[1];

    // loop end back to start
    lcd.dma_nodes[0].next = &lcd.dma_nodes[1];
    lcd.dma_nodes[1].next = &lcd.dma_nodes[0];

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
    gdma_channel_alloc_config_t dma_chan_config = { 0 };
    ESP_RETURN_ON_ERROR(
        gdma_new_ahb_channel(&dma_chan_config, &lcd.dma_chan, NULL), TAG, "alloc DMA channel failed"
    );
    // IDF 6.0's gdma_connect no longer implicitly resets the channel (removed).
    // Do it explicitly to match IDF 5.4 behavior.
    gdma_reset(lcd.dma_chan);
#else
    // alloc DMA channel and connect to LCD peripheral
    gdma_channel_alloc_config_t dma_chan_config = {
        .direction = GDMA_CHANNEL_DIRECTION_TX,
    };
    ESP_RETURN_ON_ERROR(
        gdma_new_channel(&dma_chan_config, &lcd.dma_chan), TAG, "alloc DMA channel failed"
    );
#endif
    gdma_trigger_t trigger = GDMA_MAKE_TRIGGER(GDMA_TRIG_PERIPH_LCD, 0);
    ESP_RETURN_ON_ERROR(gdma_connect(lcd.dma_chan, trigger), TAG, "dma connect error");
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
    gdma_strategy_config_t dma_strategy = {
        .eof_till_data_popped = false,
    };
    gdma_apply_strategy(lcd.dma_chan, &dma_strategy);

    gdma_transfer_config_t trans_cfg = {
        .max_data_burst_size = 64,
        .access_ext_mem = true,
    };
    ESP_RETURN_ON_ERROR(gdma_config_transfer(lcd.dma_chan, &trans_cfg), TAG, "dma setup error");
#else
    gdma_transfer_ability_t ability = {
        .psram_trans_align = 64,
        .sram_trans_align = 4,
    };
    ESP_RETURN_ON_ERROR(gdma_set_transfer_ability(lcd.dma_chan, &ability), TAG, "dma setup error");
#endif

    gdma_tx_event_callbacks_t cbs = {
        .on_trans_eof = lcd_rgb_panel_eof_handler,
    };
    ESP_RETURN_ON_ERROR(
        gdma_register_tx_event_callbacks(lcd.dma_chan, &cbs, NULL), TAG, "dma setup error"
    );

    return ESP_OK;
}

void deinit_dma_trans_link() {
    gdma_reset(lcd.dma_chan);
    gdma_disconnect(lcd.dma_chan);
    gdma_del_channel(lcd.dma_chan);
}

// 数据线在总线上按 2bit/钟的排布（每对内部高低位互换、且低位的线号在前），
// 下标与 LCD 外设的 data_sigs[] 一一对应。init_bus_gpio 与 epd_lcd_bus_park 共用
// 这一份 —— 收线/接线的引脚表一旦分家，睡醒后的第一帧就会推错位。
static inline gpio_num_t bus_data_line(const size_t i) {
    const gpio_num_t DATA_LINES[16] = {
        lcd.config.bus.data[14], lcd.config.bus.data[15], lcd.config.bus.data[12],
        lcd.config.bus.data[13], lcd.config.bus.data[10], lcd.config.bus.data[11],
        lcd.config.bus.data[8],  lcd.config.bus.data[9],  lcd.config.bus.data[6],
        lcd.config.bus.data[7],  lcd.config.bus.data[4],  lcd.config.bus.data[5],
        lcd.config.bus.data[2],  lcd.config.bus.data[3],  lcd.config.bus.data[0],
        lcd.config.bus.data[1],
    };
    return DATA_LINES[i];
}

/**
 * Configure LCD peripheral and auxiliary GPIOs
 */
static esp_err_t init_bus_gpio() {
    // 接线之前先解掉**从上一次运行继承下来**的锁（epd_lcd_bus_park 的 gpio_hold_en）。
    //
    // USB/软件复位不给外设掉电，hold 也不在复位范围内 —— 机器是"空闲下电收过线"的状态
    // 被刷机复位打断的（read_pico_board.c 的 i2c_bus_recover 为同一个理由做过同样的事），
    // 这一开机就继承了一整套锁在低电平上的总线脚。而**锁住时写不进配置**（见 park_bus_pin
    // 的第 2 条），下面这些 gpio_hal_func_sel / gpio_set_direction / gpio_config 会静默
    // 失效：脚一直停在 GPIO 输出 0，XLE 不翻，面板一个像素都收不到。
    //
    // 症状是"开机动画推了 1.6s（LCD 外设照跑、耗时是真的），屏上却仍是复位前那一屏"，
    // 一直要等到下一次 park→unpark 才解开 —— 而 display.c 那份 s_bus_parked 开机是 false，
    // present_begin 的 unpark 直接 return，所以第一次 park（空闲下电那一拍）之前根本没人解。
    // 2026-10-07 晚上给"空闲下电"也加上 park 之后，闲置的机器都处在这个状态，于是每次
    // 刷机复位都这样（之前 park 只在浅睡前，机器醒着时总线是自由的，所以没露过面）。
    // / Release holds inherited from a previous run before wiring the bus. USB/soft reset
    // does not power-cycle, and gpio_hold_en survives it; pin config writes while held fail
    // silently, leaving the bus lines stuck at 0 so the panel latches nothing.
    for (size_t i = (16 - lcd.config.bus_width); i < 16; i++) {
        gpio_hold_dis(bus_data_line(i));
    }
    gpio_hold_dis(lcd.config.bus.leh);
    gpio_hold_dis(lcd.config.bus.clock);
    gpio_hold_dis(lcd.config.bus.start_pulse);
    gpio_hold_dis(lcd.config.bus.stv);
    gpio_hold_dis(lcd.config.bus.ckv);

    // connect peripheral signals via GPIO matrix
    for (size_t i = (16 - lcd.config.bus_width); i < 16; i++) {
        gpio_hal_func_sel(&hal, bus_data_line(i), PIN_FUNC_GPIO);
        gpio_set_direction(bus_data_line(i), GPIO_MODE_OUTPUT);
        esp_rom_gpio_connect_out_signal(bus_data_line(i), LCD_PERIPH_SIG(data_sigs[i]), false, false);
    }
    gpio_hal_func_sel(&hal, lcd.config.bus.leh, PIN_FUNC_GPIO);
    gpio_set_direction(lcd.config.bus.leh, GPIO_MODE_OUTPUT);
    gpio_hal_func_sel(&hal, lcd.config.bus.clock, PIN_FUNC_GPIO);
    gpio_set_direction(lcd.config.bus.clock, GPIO_MODE_OUTPUT);
    gpio_hal_func_sel(&hal, lcd.config.bus.start_pulse, PIN_FUNC_GPIO);
    gpio_set_direction(lcd.config.bus.start_pulse, GPIO_MODE_OUTPUT);

    esp_rom_gpio_connect_out_signal(lcd.config.bus.leh, LCD_PERIPH_SIG(hsync_sig), false, false);
    esp_rom_gpio_connect_out_signal(lcd.config.bus.clock, LCD_PERIPH_SIG(pclk_sig), false, false);
    esp_rom_gpio_connect_out_signal(
        lcd.config.bus.start_pulse, LCD_PERIPH_SIG(de_sig), false, false
    );

    // STV 由 GPIO 直驱（不挂 LCD 外设的矩阵信号），所以这一脚得走 gpio_config —— 它会顺手
    // 把引脚记进 IDF 的"谁占了这个脚"表（esp_gpio_reserve）。unpark 复用本函数做第二次
    // 接线时，同一脚会被自己再占一次，驱动认不出是自己人，于是每次都打一条
    // `W gpio: conflict found for GPIO[47]`（本机 STV 就是 GPIO47）。配之前先把上一轮的
    // 所有权收回来 —— esp_gpio_revoke 只清那张表的位，不碰引脚状态。
    // / init_bus_gpio() is called again by unpark(); gpio_config() reserves STV on each call
    // and warns on the second, so drop our own claim first (bookkeeping only).
    esp_gpio_revoke(1ull << lcd.config.bus.stv);
    gpio_config_t vsync_gpio_conf = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = 1ull << lcd.config.bus.stv,
    };
    gpio_config(&vsync_gpio_conf);
    gpio_set_level(lcd.config.bus.stv, 1);
    return ESP_OK;
}

/**
 * Reset bus GPIO pin functions.
 */
static void deinit_bus_gpio() {
    for (size_t i = (16 - lcd.config.bus_width); i < 16; i++) {
        gpio_reset_pin(lcd.config.bus.data[i]);
    }

    gpio_reset_pin(lcd.config.bus.leh);
    gpio_reset_pin(lcd.config.bus.clock);
    gpio_reset_pin(lcd.config.bus.start_pulse);
    gpio_reset_pin(lcd.config.bus.stv);
}

// 一根总线脚：从外设信号上摘下来、推低、锁住。
//
// 三件事缺一不可：
//   1) 矩阵里的 out_sel 还指着 LCD/RMT 的 tx 信号（gpio_set_direction 只动 IO_MUX 和
//      输出使能，**不会**断开矩阵接线），所以要先把它接回 SIG_GPIO_OUT_IDX，电平才由
//      GPIO_OUT_REG 说了算；
//   2) gpio_hold_dis 必须在最前 —— 引脚被锁住时写不了配置；
//   3) 先设方向再设电平、最后才 hold（hold 锁的是"此刻"的状态）。
//
// hold 在浅睡里其实只是保险（数字 IO 域不断电，推低之后本来就会保持；IDF 文档里"数字
// 脚变高阻"那条只针对深睡），但它是"万一哪天改成深睡"时唯一能守住电平的东西，顺手就
// 带上。S3 上引脚 0~21 走 RTC 那套 hold、22 以上走数字 hold 掩码，本机总线用到的
// GPIO3~GPIO21 与 GPIO45~48 两条路都覆盖得到。
static void park_bus_pin(const gpio_num_t pin) {
    gpio_hold_dis(pin);
    gpio_hal_func_sel(&hal, pin, PIN_FUNC_GPIO);
    gpio_set_direction(pin, GPIO_MODE_OUTPUT);
    esp_rom_gpio_connect_out_signal(pin, SIG_GPIO_OUT_IDX, false, false);
    gpio_set_level(pin, 0);
    gpio_hold_en(pin);
}

/**
 * 把所有面板总线引脚收成确定的低电平并保持，浅睡前调（唤醒后 epd_lcd_bus_unpark）。
 *
 * 为什么：面板下电后总线上那些仍挂在 LCD/RMT 输出上的引脚，会带着最后一帧的残余电平；
 * 浅睡时数字 IO 不再驱动，就成了漏电/偏置源，给已经断电的源极线一个缓慢漂移的电位 ——
 * 面板上的残余电荷因此更不均匀（待机久了底色发花）。显式推低并 hold，等于让所有源极线
 * 保持同一个确定电位，也让"待机时长"不再影响画面。
 *
 * 只动 GPIO 矩阵的接线，**不碰** LCD/RMT 外设本身（DMA、外设时钟、RMT 配置都不动）：
 * 浅睡不复位外设，唤醒时把接线接回去即可，不需要重建任何外设状态，也就没有"重建失败
 * 就是死屏"的风险（对比 epd_lcd_deinit/epd_lcd_init 那一对）。
 */
void epd_lcd_bus_park(void) {
    for (size_t i = (16 - lcd.config.bus_width); i < 16; i++) {
        park_bus_pin(bus_data_line(i));
    }
    park_bus_pin(lcd.config.bus.leh);
    park_bus_pin(lcd.config.bus.clock);
    park_bus_pin(lcd.config.bus.start_pulse);
    park_bus_pin(lcd.config.bus.stv);
    park_bus_pin(lcd.config.bus.ckv);   // CKV 的 RMT 信号同样从矩阵上摘掉；unpark 里接回
}

/**
 * epd_lcd_bus_park() 的逆操作：解锁 → 重建数据/时钟/LEH/XSTL/STV 的接线（与开机
 * init_bus_gpio 同一段代码，不另写一份）→ 把 CKV 挂回 RMT 通道。
 *
 * RMT 只重新接一次线，配置不动：CKV 的相位表本来就在每次推帧前重建
 * （start_ckv_cycles）。CKV 的空闲电平是低（init_ckv_rmt 里设的 idle level 0），
 * 所以第一次刷新之前这条线也停在低电平，与 park 期间一致。
 */
void epd_lcd_bus_unpark(void) {
    for (size_t i = (16 - lcd.config.bus_width); i < 16; i++) {
        gpio_hold_dis(bus_data_line(i));
    }
    gpio_hold_dis(lcd.config.bus.leh);
    gpio_hold_dis(lcd.config.bus.clock);
    gpio_hold_dis(lcd.config.bus.start_pulse);
    gpio_hold_dis(lcd.config.bus.stv);
    gpio_hold_dis(lcd.config.bus.ckv);

    if (init_bus_gpio() != ESP_OK) {
        ESP_LOGE(TAG, "unpark: 总线接线重建失败");
    }
    rmt_compat_connect_gpio(RMT_CKV_CHAN, lcd.config.bus.ckv);
}

/**
 * Check if the PSRAM cache is properly configured.
 */
static void check_cache_configuration() {
    if (EPDIY_DATA_CACHE_LINE_SIZE < 64) {
        ESP_LOGE(
            "epdiy",
            "cache line size is set to %d (< 64B)! This will degrade performance, please update "
            "this option in menuconfig.",
            EPDIY_DATA_CACHE_LINE_SIZE
        );
        ESP_LOGE(
            "epdiy",
            "If you are on arduino, you can't set this option yourself, you'll need to use a lower "
            "speed."
        );
        ESP_LOGE(
            "epdiy",
            "Reducing the pixel clock from %d MHz to %d MHz for now!",
            lcd.config.pixel_clock / 1000 / 1000,
            lcd.config.pixel_clock / 1000 / 1000 / 2
        );
        lcd.config.pixel_clock = lcd.config.pixel_clock / 2;

        // fixme: this would be nice, but doesn't work :(
        // uint32_t d_autoload = Cache_Suspend_DCache();
        /// Cache_Set_DCache_Mode(CACHE_SIZE_FULL, CACHE_4WAYS_ASSOC, CACHE_LINE_SIZE_32B);
        // Cache_Invalidate_DCache_All();
        // Cache_Resume_DCache(d_autoload);
    }
}

/**
 * Assign LCD configuration parameters from a given configuration, without allocating memory or
 * touching the LCD peripheral config.
 */
static void assign_lcd_parameters_from_config(
    const LcdEpdConfig_t* config, int display_width, int display_height
) {
    // copy over the configuraiton object
    memcpy(&lcd.config, config, sizeof(LcdEpdConfig_t));

    // Make sure the bounce buffers divide the display height evenly.
    lcd.display_lines = (((display_height + 7) / 8) * 8);

    lcd.line_bytes = display_width / 4;
    lcd.lcd_res_h = lcd.line_bytes / (lcd.config.bus_width / 8);

    // With 8 bit bus width, we need a dummy cycle before the actual data,
    // because the LCD peripheral behaves weirdly.
    // Also see:
    // https://blog.adafruit.com/2022/06/14/esp32uesday-hacking-the-esp32-s3-lcd-peripheral/
    lcd.dummy_bytes = lcd.config.bus_width / 8;

    // each bounce buffer holds a number of lines with data + dummy bytes each
    lcd.bb_size = BOUNCE_BUF_LINES * (lcd.line_bytes + lcd.dummy_bytes);

    check_cache_configuration();

    ESP_LOGI(TAG, "using resolution %dx%d", lcd.lcd_res_h, lcd.display_lines);
}

/**
 * Allocate buffers for LCD driver operation.
 */
static esp_err_t allocate_lcd_buffers() {
    uint32_t dma_flags = MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA;

    // allocate bounce buffers
    for (int i = 0; i < 2; i++) {
        lcd.bounce_buffer[i] = heap_caps_aligned_calloc(4, 1, lcd.bb_size, dma_flags);
        ESP_RETURN_ON_FALSE(lcd.bounce_buffer[i], ESP_ERR_NO_MEM, TAG, "install interrupt failed");
    }

    // So far, I haven't seen any displays with > 4096 pixels per line,
    // so we only need one DMA node for now.
    assert(lcd.bb_size < DMA_DESCRIPTOR_BUFFER_MAX_SIZE);
    lcd.dma_nodes = heap_caps_calloc(1, sizeof(dma_descriptor_t) * 2, dma_flags);
    ESP_RETURN_ON_FALSE(lcd.dma_nodes, ESP_ERR_NO_MEM, TAG, "no mem for dma nodes");
    return ESP_OK;
}

static void free_lcd_buffers() {
    for (int i = 0; i < 2; i++) {
        uint8_t* buf = lcd.bounce_buffer[i];
        if (buf != NULL) {
            heap_caps_free(buf);
            lcd.bounce_buffer[i] = NULL;
        }
    }

    if (lcd.dma_nodes != NULL) {
        heap_caps_free(lcd.dma_nodes);
        lcd.dma_nodes = NULL;
    }
}

/**
 * Initialize the LCD peripheral itself and install interrupts.
 */
static esp_err_t init_lcd_peripheral() {
    esp_err_t ret = ESP_OK;

    // enable APB to access LCD registers
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
    PERIPH_RCC_ACQUIRE_ATOMIC(soc_lcd_rgb_signals[0].module, ref_count) {
        if (ref_count == 0) {
            lcd_ll_enable_bus_clock(0, true);
            lcd_ll_reset_register(0);
        }
    }
#else
    periph_module_enable(PERIPH_LCD_CAM_MODULE);
    periph_module_reset(PERIPH_LCD_CAM_MODULE);
#endif

    lcd_hal_init(&lcd.hal, 0);
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
    // IDF 6 requires explicit clock-tree enable; otherwise PCLK can fall back to a slow source.
    ESP_RETURN_ON_ERROR(
        esp_clk_tree_enable_src((soc_module_clk_t)LCD_CLK_SRC_PLL240M, true),
        TAG,
        "enable lcd clk src failed"
    );
    ESP_RETURN_ON_ERROR(
        esp_clk_tree_src_get_freq_hz(
            (soc_module_clk_t)LCD_CLK_SRC_PLL240M,
            ESP_CLK_TREE_SRC_FREQ_PRECISION_CACHED,
            &lcd.src_clk_hz
        ),
        TAG,
        "get lcd clk freq failed"
    );
    PERIPH_RCC_ATOMIC() {
        lcd_ll_enable_clock(lcd.hal.dev, true);
        lcd_ll_select_clk_src(lcd.hal.dev, LCD_CLK_SRC_PLL240M);
    }
#else
    lcd.src_clk_hz = 240000000;
    lcd_ll_enable_clock(lcd.hal.dev, true);
    lcd_ll_select_clk_src(lcd.hal.dev, LCD_CLK_SRC_PLL240M);
#endif
    ESP_LOGI(TAG, "lcd src clk: %u Hz", (unsigned)lcd.src_clk_hz);
    ESP_RETURN_ON_ERROR(ret, TAG, "set source clock failed");

    lcd_ll_fifo_reset(lcd.hal.dev);
    lcd_ll_reset(lcd.hal.dev);

    // install interrupt service, (LCD peripheral shares the interrupt source with Camera by
    // different mask)
    int flags = ESP_INTR_FLAG_IRAM | ESP_INTR_FLAG_INTRDISABLED | ESP_INTR_FLAG_SHARED
                | ESP_INTR_FLAG_LOWMED;

    int source = LCD_PERIPH_SIG(irq_id);
    uint32_t status = (uint32_t)lcd_ll_get_interrupt_status_reg(lcd.hal.dev);
    ret = esp_intr_alloc_intrstatus(
        source, flags, status, LCD_LL_EVENT_VSYNC_END, lcd_isr_vsync, NULL, &lcd.vsync_intr
    );
    ESP_RETURN_ON_ERROR(ret, TAG, "install interrupt failed");

    status = (uint32_t)lcd_ll_get_interrupt_status_reg(lcd.hal.dev);
    ret = esp_intr_alloc_intrstatus(
        source, flags, status, LCD_LL_EVENT_TRANS_DONE, lcd_isr_vsync, NULL, &lcd.done_intr
    );
    ESP_RETURN_ON_ERROR(ret, TAG, "install interrupt failed");

    // pixel clock phase and polarity
    lcd_ll_set_clock_idle_level(lcd.hal.dev, false);
    lcd_ll_set_pixel_clock_edge(lcd.hal.dev, false);

    // enable RGB mode and set data width
    lcd_ll_enable_rgb_mode(lcd.hal.dev, true);
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 3, 0)
    lcd_ll_set_dma_read_stride(lcd.hal.dev, lcd.config.bus_width);
    lcd_ll_set_data_wire_width(lcd.hal.dev, lcd.config.bus_width);
#else
    lcd_ll_set_data_width(lcd.hal.dev, lcd.config.bus_width);
#endif
    lcd_ll_set_phase_cycles(lcd.hal.dev, 0, (lcd.dummy_bytes > 0), 1);  // enable data phase only

    // number of data cycles is controlled by DMA buffer size
    lcd_ll_enable_output_always_on(lcd.hal.dev, true);
    lcd_ll_set_idle_level(lcd.hal.dev, false, true, true);

    // configure blank region timing
    // RGB panel always has a front and back blank (porch region)
    lcd_ll_set_blank_cycles(lcd.hal.dev, 1, 1);

    // output hsync even in porch region?
    lcd_ll_enable_output_hsync_in_porch_region(lcd.hal.dev, false);
    // send next frame automatically in stream mode
    lcd_ll_enable_auto_next_frame(lcd.hal.dev, false);

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 5, 0)
    PERIPH_RCC_ATOMIC() {
        lcd_ll_enable_interrupt(lcd.hal.dev, LCD_LL_EVENT_VSYNC_END, true);
        lcd_ll_enable_interrupt(lcd.hal.dev, LCD_LL_EVENT_TRANS_DONE, true);
    }
#else
    lcd_ll_enable_interrupt(lcd.hal.dev, LCD_LL_EVENT_VSYNC_END, true);
    lcd_ll_enable_interrupt(lcd.hal.dev, LCD_LL_EVENT_TRANS_DONE, true);
#endif

    // clear any stale interrupt events before enabling the ISR
    lcd_ll_clear_interrupt_status(lcd.hal.dev, UINT32_MAX);
    // enable intr
    esp_intr_enable(lcd.vsync_intr);
    esp_intr_enable(lcd.done_intr);
    return ret;
}

static void deinit_lcd_peripheral() {
    // disable and free interrupts
    esp_intr_disable(lcd.vsync_intr);
    esp_intr_disable(lcd.done_intr);
    esp_intr_free(lcd.vsync_intr);
    esp_intr_free(lcd.done_intr);

    lcd_ll_stop(lcd.hal.dev);
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
    PERIPH_RCC_ATOMIC() {
        lcd_ll_enable_clock(lcd.hal.dev, false);
    }
#else
    lcd_ll_enable_clock(lcd.hal.dev, false);
#endif

    lcd_ll_fifo_reset(lcd.hal.dev);
    lcd_ll_reset(lcd.hal.dev);

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
    PERIPH_RCC_RELEASE_ATOMIC(soc_lcd_rgb_signals[0].module, ref_count) {
        if (ref_count == 0) {
            lcd_ll_enable_bus_clock(0, false);
        }
    }
#else
    periph_module_reset(PERIPH_LCD_CAM_MODULE);
    periph_module_disable(PERIPH_LCD_CAM_MODULE);
#endif
}

/**
 * Configure the LCD driver for epdiy.
 */
void epd_lcd_init(const LcdEpdConfig_t* config, int display_width, int display_height) {
    esp_err_t ret = ESP_OK;
    assign_lcd_parameters_from_config(config, display_width, display_height);

    ret = allocate_lcd_buffers();
    ESP_GOTO_ON_ERROR(ret, err, TAG, "lcd buffer allocation failed");

    ret = init_lcd_peripheral();
    ESP_GOTO_ON_ERROR(ret, err, TAG, "lcd peripheral init failed");

    ret = init_dma_trans_link();
    ESP_GOTO_ON_ERROR(ret, err, TAG, "install DMA failed");

    ret = init_bus_gpio();
    ESP_GOTO_ON_ERROR(ret, err, TAG, "configure GPIO failed");

    init_ckv_rmt();

    // setup driver state
    epd_lcd_set_pixel_clock_MHz(lcd.config.pixel_clock / 1000 / 1000);
    epd_lcd_line_source_cb(NULL, NULL);

    ESP_LOGI(TAG, "LCD init done.");
    return;
err:
    ESP_LOGE(TAG, "LCD initialization failed!");
    abort();
}

/**
 * Deinitializue the LCD driver, i.e., free resources and peripherals.
 */
void epd_lcd_deinit() {
    epd_lcd_line_source_cb(NULL, NULL);

    deinit_bus_gpio();
    deinit_lcd_peripheral();
    deinit_dma_trans_link();
    free_lcd_buffers();
    deinit_ckv_rmt();

    ESP_LOGI(TAG, "LCD deinitialized.");
}

void epd_lcd_set_line_timing(const LcdLineTiming_t* timing) {
    if (timing == NULL) return;
    lcd.config.line = *timing;
}

void epd_lcd_set_pixel_clock_MHz(int frequency) {
    lcd.config.pixel_clock = frequency * 1000 * 1000;
    if (lcd.src_clk_hz == 0) {
        lcd.src_clk_hz = 240000000;
    }

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 3, 0)
    hal_utils_clk_div_t clk_div = {};
/**
 * There was a change in the parameters of this function in this commit:
 * https://github.com/espressif/esp-idf/commit/d39388fe4f4c5bfb0b52df9177307b1688f41016#diff-2df607d77e3f6e350bab8eb31cfd914500ae42744564e1640cec47006cc17a9c
 * There are different builds with the same IDF minor version, some with, some without the commit.
 * So we try to select the correct one by checking if the flag value is defined.
 */
#ifdef LCD_HAL_PCLK_FLAG_ALLOW_EQUAL_SYSCLK
    uint32_t freq
        = lcd_hal_cal_pclk_freq(&lcd.hal, lcd.src_clk_hz, lcd.config.pixel_clock, 0, &clk_div);
#else
    uint32_t freq
        = lcd_hal_cal_pclk_freq(&lcd.hal, lcd.src_clk_hz, lcd.config.pixel_clock, &clk_div);
#endif
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
    PERIPH_RCC_ATOMIC() {
        lcd_ll_set_group_clock_coeff(
            lcd.hal.dev, (int)clk_div.integer, (int)clk_div.denominator, (int)clk_div.numerator
        );
    }
#else
    lcd_ll_set_group_clock_coeff(
        &LCD_CAM, (int)clk_div.integer, (int)clk_div.denominator, (int)clk_div.numerator
    );
#endif
#else
    uint32_t freq = lcd_hal_cal_pclk_freq(&lcd.hal, lcd.src_clk_hz, lcd.config.pixel_clock, 0);
#endif

    ESP_LOGI(TAG, "pclk freq: %u Hz (src %u Hz)", (unsigned)freq, (unsigned)lcd.src_clk_hz);
    rebuild_line_geometry(frequency);
    ckv_rmt_build_signal();
    ESP_LOGI(
        TAG, "line width: %d.%dus, %d cycles, ckv high %d.%dus", lcd.line_time_01us / 10,
        lcd.line_time_01us % 10, lcd.line_cycles, lcd.ckv_high_time / 10, lcd.ckv_high_time % 10
    );
}

void IRAM_ATTR epd_lcd_start_frame() {
    int initial_lines = min(LINE_BATCH, lcd.display_lines);

    // hsync: pulse width, back porch, active width, front porch
    const LcdLineTiming_t* t = &lcd.config.line;
    int end_line = lcd.line_cycles - lcd.lcd_res_h - t->le_high_time - t->line_front_porch;
    lcd_ll_set_horizontal_timing(
        lcd.hal.dev,
        t->le_high_time - (lcd.dummy_bytes > 0),
        t->line_front_porch,
        // a dummy byte is neeed in 8 bit mode to work around LCD peculiarities
        lcd.lcd_res_h + (lcd.dummy_bytes > 0),
        end_line
    );
    lcd_ll_set_vertical_timing(lcd.hal.dev, 1, 0, initial_lines, 1);

    // generate the hsync at the very beginning of line
    lcd_ll_set_hsync_position(lcd.hal.dev, 1);

    // reset FIFO of DMA and LCD, incase there remains old frame data
    gdma_reset(lcd.dma_chan);
    lcd_ll_stop(lcd.hal.dev);
    lcd_ll_reset(lcd.hal.dev);
    lcd_ll_fifo_reset(lcd.hal.dev);
    lcd_ll_enable_auto_next_frame(lcd.hal.dev, true);

    lcd.batches = 0;
    lcd.bb_eof_count = 0;
    fill_bounce_buffer(lcd.bounce_buffer[0]);
    fill_bounce_buffer(lcd.bounce_buffer[1]);

    // the start of DMA should be prior to the start of LCD engine
    gdma_start(lcd.dma_chan, (intptr_t)&lcd.dma_nodes[0]);

    // enter a critical section to ensure the frame start timing is correct
    taskENTER_CRITICAL(&frame_start_spinlock);

    // delay 1us is sufficient for DMA to pass data to LCD FIFO
    // in fact, this is only needed when LCD pixel clock is set too high
    gpio_set_level(lcd.config.bus.stv, 0);
    // esp_rom_delay_us(1);
    //  for picture clarity, it seems to be important to start CKV at a "good"
    //  time, seemingly start or towards end of line.
    start_ckv_cycles(initial_lines + 5);
    esp_rom_delay_us(lcd.line_length_us);
    gpio_set_level(lcd.config.bus.stv, 1);
    esp_rom_delay_us(lcd.line_length_us);
    esp_rom_delay_us(lcd.ckv_high_time / 10);

    // start LCD engine
    lcd_ll_start(lcd.hal.dev);

    taskEXIT_CRITICAL(&frame_start_spinlock);
}

#else

/// Dummy implementation to link on the old ESP32
void epd_lcd_init(const LcdEpdConfig_t* config, int display_width, int display_height) {
    assert(false);
}

#endif  // S3 Target
