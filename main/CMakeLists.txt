# pjournal-pico 主组件：保留 pjournal-esp32 的状态机/屏幕/IME/BLE/WiFi，
# 硬件后端替换为 read_pico(epdiy 4bpp framebuffer + ttf_font 抗锯齿 + PMU/触摸/3键)。
#
# 与原 pjournal 的差异：
#   - 移除 u8g2 / u8g2_st7305 / esp_adc / esp_websocket_client / esp_codec_dev /
#     esp_audio_codec / efuse（渲染由 gfx/u8g2_shim + font/ttf_font 接管；
#     电量走 PMU soc_permille；语音/音效首版 stub）。
#   - 新增 hw/board、hw/input、gfx/u8g2_shim、gfx/icon_font、display、settings。

# 拼音/两分字库 .bin 是外部生成产物（不在仓库），提供前先关闭以让首版能编译烧录。
# / The pinyin/liangfen .bin dictionaries are external artifacts; keep OFF until provided.
option(PJOURNAL_IME_ENABLE_PINYIN "Build the pinyin IME module" ON)
option(PJOURNAL_IME_ENABLE_LIANGFEN "Embed and build the liangfen IME module" ON)
option(PJOURNAL_IME_ENABLE_WUBI "Build optional wubi IME support" OFF)
option(PJOURNAL_IME_ENABLE_SHUANGPIN "Build optional shuangpin IME support" OFF)
option(PJOURNAL_IME_PERF_LOG "Enable slow IME lookup timing logs" OFF)

# 嵌入资源。IME 的 ime_table_pinyin.bin / liangfen.bin 是构建期生成的字库产物，
# 不在源码仓库里（gitignore），构建前须由上游工具生成或手工放置到 ime/ 下。
set(PJOURNAL_EMBED_FILES
    "assets/builtin.ttf"
    "assets/icon_font.ttf"
    # Yan Reader 标志字体：Noto Serif CJK SC 子集，只有标志用到的 27 个字形(5.4KB)，
    # 只给开机动画用。生成脚本见 tools/make_logo_font.py。
    "assets/yanos_logo.ttf"
    "ime/english_words.txt"
)

if(PJOURNAL_IME_ENABLE_PINYIN)
    list(APPEND PJOURNAL_EMBED_FILES "ime/ime_table_pinyin.bin")
endif()

if(PJOURNAL_IME_ENABLE_LIANGFEN)
    list(APPEND PJOURNAL_EMBED_FILES "ime/liangfen.bin")
endif()

idf_component_register(SRCS
    "main.cpp"
    "pjournal_app.cpp"
    "ui_helpers.cpp"
    "ui_render.cpp"
    "safe_file.cpp"
    "clipboard.cpp"
    "edit_menu.cpp"
    "text_sel.cpp"
    "ui/ime_field.cpp"
    "net/http.cpp"
    "screen_editor.cpp"
    "editor_vk.cpp"
    "screen_settings.cpp"
    "quick_edit.cpp"
    "typing_click.cpp"
    "screen_bt_manage.cpp"
    "screen_file_manager.cpp"
    "file_manager_server.cpp"
    "font_renderer.cpp"
    "bt_keyboard.cpp"
    "journal_storage.cpp"
    "settings_manager.cpp"
    "settings_backup.cpp"  # 配置+阅读记录备份/恢复到 TF 卡（设置·通用 分类里的两项）
    "wifi_manager.cpp"
    "webdav_client.cpp"
    "opds_client.cpp"
    "dictionary_store.cpp"
    "ble_keymap.cpp"
    "flomo_client.cpp"
    "deepseek_client.cpp"
    "pcf85063.cpp"
    "chinese_almanac.cpp"
    "standby_clock.cpp"
    "usb_msc.cpp"          # U盘模式：TinyUSB MSC 把 SD 卡整卡暴露给电脑
    "boot_splash.cpp"
    "json_parser.cpp"
    "json_utils.cpp"
    "ime/IME.cpp"
    "ime/yong_dict.cpp"
    "ime/yong_pinyin.cpp"
    "screen_gtd.cpp"
    "screen_outline.cpp"
    "screen_inspiration.cpp"
    "screen_polish.cpp"
    "screen_polish_prompt.cpp"
    "screen_flomo.cpp"
    "flomo_json.cpp"
    "flomo_file.cpp"
    "flomo_api.cpp"
    "flomo_db.cpp"
    "markdown_render.cpp"
    "vertical_layout.cpp"
    "screen_reader.cpp"
    "screen_reader_weread.cpp"   # 微信读书子应用（P3b 从 screen_reader.cpp 拆出）
    "screen_reader_stats.cpp"    # 「统计」标签与 8 个子界面（P3b 从 screen_reader.cpp 拆出）
    "screen_reader_subpages.cpp" # 设置标签下的叶子子界面（P3b 从 screen_reader.cpp 拆出）
    "screen_reader_files.cpp"    # 「文件」标签：浏览器/文件菜单/图片查看器（P3b 拆出）
    "screen_reader_footnotes.cpp" # 正文脚注引擎（P3b 从 screen_reader.cpp 拆出）
    "reading_stats.cpp"
    # 硬件适配层
    "hw/board.cpp"
    "hw/input.cpp"
    "gfx/u8g2_shim.c"
    "gfx/icon_font.c"
    "font/ttf_font.c"
    "display.c"
    "font_store.c"
    INCLUDE_DIRS
        "."
        "hw"
        "gfx"
        "font"
        "ime"
        "ui"
        "net"
    EMBED_FILES
        ${PJOURNAL_EMBED_FILES}
    REQUIRES
        read_pico
        read_pico_pmu
        epdiy
        cst836u
        esp_tinyusb
        sdmmc
        esp_driver_sdmmc
        e0470_epaper_waveform
        e0470_page_turn
        esp_hid
        bt
        nvs_flash
        esp_wifi
        esp_netif
        esp-tls
        esp_http_client
        esp_http_server
        esp_event
        esp_pm
        esp_timer
        lwip
        mbedtls
        esp_rom
        fatfs
        driver
        crossmux
)

# 本工程是独立项目，版本号不复用 pjournal 那条线：从 1.0.0 重新起算。
target_compile_definitions(${COMPONENT_LIB} PRIVATE YAN_READER_VERSION="1.3.0")
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    PJOURNAL_IME_ENABLE_PINYIN=$<BOOL:${PJOURNAL_IME_ENABLE_PINYIN}>
    PJOURNAL_IME_ENABLE_LIANGFEN=$<BOOL:${PJOURNAL_IME_ENABLE_LIANGFEN}>
    PJOURNAL_IME_ENABLE_WUBI=$<BOOL:${PJOURNAL_IME_ENABLE_WUBI}>
    PJOURNAL_IME_ENABLE_SHUANGPIN=$<BOOL:${PJOURNAL_IME_ENABLE_SHUANGPIN}>
    PJOURNAL_IME_PERF_LOG=$<BOOL:${PJOURNAL_IME_PERF_LOG}>
)
