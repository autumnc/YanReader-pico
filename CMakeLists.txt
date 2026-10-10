# pjournal-pico: pjournal-esp32 移植到 Read Pico 墨水屏板
# 复用 read_pico_firmware 的图形库(epdiy)与字体层(ttf_font)，保留 pjournal 的
# app_main 状态机 + BLE 键盘 + 拼音输入法。
#
# 构建要求: ESP-IDF v6.1 (read_pico 组件依赖 v6 头文件)。
cmake_minimum_required(VERSION 3.22)

# 复用 read_pico_firmware 的组件(epdiy/read_pico/read_pico_pmu/...)。
# 默认用本仓随附的只读副本 read_pico_firmware/（vendored：上游 MindReset/read_pico_firmware
# 的一份快照，本机 git 身份对它 403 推不上去，所以随项目一起版本化 —— 见该目录所对应的
# 提交说明）。可用 -DREAD_PICO_DIR=<path> 覆盖成自己的兄弟目录副本。
#
# 注意：READ_PICO_DIR 是 CACHE 变量，已配置过的 build/ 会沿用缓存里的旧路径。要让这里的
# 默认值生效，需删掉 build/CMakeCache.txt（或换一个 -B 目录）重新配置。
if(NOT DEFINED READ_PICO_DIR)
    set(READ_PICO_DIR "${CMAKE_CURRENT_LIST_DIR}/read_pico_firmware" CACHE PATH "read_pico_firmware 根目录")
endif()
set(EXTRA_COMPONENT_DIRS "${READ_PICO_DIR}/components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(pjournal_pico)
