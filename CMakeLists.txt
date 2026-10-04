# pjournal-pico: pjournal-esp32 移植到 Read Pico 墨水屏板
# 复用 read_pico_firmware 的图形库(epdiy)与字体层(ttf_font)，保留 pjournal 的
# app_main 状态机 + BLE 键盘 + 拼音输入法。
#
# 构建要求: ESP-IDF v6.1 (read_pico 组件依赖 v6 头文件)。
cmake_minimum_required(VERSION 3.22)

# 复用 read_pico_firmware 的组件(epdiy/read_pico/read_pico_pmu/...)。
# 可通过 -DREAD_PICO_DIR=<path> 覆盖默认的兄弟目录。
if(NOT DEFINED READ_PICO_DIR)
    get_filename_component(_pico_root "${CMAKE_CURRENT_LIST_DIR}/../read_pico_firmware" ABSOLUTE)
    set(READ_PICO_DIR "${_pico_root}" CACHE PATH "read_pico_firmware 根目录")
endif()
set(EXTRA_COMPONENT_DIRS "${READ_PICO_DIR}/components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(pjournal_pico)
