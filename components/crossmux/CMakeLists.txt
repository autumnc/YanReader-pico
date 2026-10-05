# crossmux 阅读器组件：vendored 的上游阅读引擎（0x1abin/crossmux b5fdc419）+
# 针对 read_pico（epdiy 4bpp + ttf_font）的 shim。相对上游改了 52 个 lib/ 文件与
# 4 个 src/ 文件、新增 Dictionary/JPEGDEC/PNGdec/QrCode/weread_port 五个目录，
# 差异清单见根 README 的「对 crossmux 引擎的改动与增强」一节。
# 只保留阅读器/微信读书所需的最小面（上游的 KOReaderSync / I18n / SoundFeedback /
# HalAudioOutput / HalFrontlight 等已删）；图片解码用自己 vendor 的 PNGdec / JPEGDEC。

# 注意：GLOB 的结果在**配置期**定死，所以新增/删除 .c/.cpp/.h 之后必须重新配置一次
# （`idf.py reconfigure`，或 touch 本文件再 build）—— 否则新文件被静默漏掉，直到链接期才
# 以 "undefined reference" 现身。**不要**加 CONFIGURE_DEPENDS 图省事：IDF 会把本文件以
# script 模式 include 一遍来提取依赖（component_get_requirements.cmake），那个模式下
# CONFIGURE_DEPENDS 直接报错，整个工程配置不起来。
file(GLOB_RECURSE SOURCES
  "lib/*.c"
  "lib/*.cpp"
  "src/*.c"
  "src/util/*.c"
  "src/*.cpp"
  "src/util/*.cpp"
)
# miniz.c 是 amalgamation，由 lib/miniz/src/miniz_impl.c #include 编译，避免重复符号。
list(REMOVE_ITEM SOURCES "${CMAKE_CURRENT_LIST_DIR}/lib/miniz/third_party/miniz.c")

# 收集所有含头文件的目录作为 include 路径（lib 内大量使用 <> 角括号包含）。
file(GLOB_RECURSE HEADERS
  "*.h"
  "common/*.h"
  "lib/*.h"
  "src/*.h"
)
set(INCLUDE_DIRS "." "common" "src" "lib")
foreach(header ${HEADERS})
  get_filename_component(dir "${header}" DIRECTORY)
  list(APPEND INCLUDE_DIRS "${dir}")
endforeach()
list(REMOVE_DUPLICATES INCLUDE_DIRS)

idf_component_register(
  SRCS ${SOURCES}
  INCLUDE_DIRS ${INCLUDE_DIRS}
  # esp_http_client/esp_wifi/esp_netif/mbedtls 供微信读书联网（WeReadWebApi）使用；
  # 阅读器本身只用前面几个。
  REQUIRES epdiy esp_timer heap log freertos esp_hw_support esp_psram
           esp_http_client esp_wifi esp_netif mbedtls
)

# 逻辑库用到 C++20（Memory.h 的 requires、ReleaseJsonParser.h 的 std::span）。
target_compile_options(${COMPONENT_LIB} PRIVATE
  $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++20>
  # crossmux 原版库在 -Werror 下触发 reorder/parentheses/stringop-truncation/comment 等
  # 告警（非本移植引入），按组件降级为普通告警而非错误。
  -Wno-error
)

# crossmux platformio.ini 的 readpico 环境编译宏，逻辑库以这些宏选择代码路径。
target_compile_definitions(${COMPONENT_LIB} PRIVATE
  FREEINK_DEVICE_READPICO=1
  FREEINK_DEVICE_EEGO_A4=0
  XML_GE=0
  XML_CONTEXT_BYTES=1024
  MINIZ_NO_ZLIB_COMPATIBLE_NAMES=1
  ENABLE_CHINESE_VERSION=1
  # WeReadHttpClient 把版本号拼进 User-Agent。
  CROSSPOINT_VERSION="pico"
)
