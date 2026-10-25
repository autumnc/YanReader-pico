# Lua 5.4.9 运行库（CrossStore 应用的沙箱宿主）。
#
# 上游源码**未修改**，取自 https://www.lua.org/ftp/lua-5.4.9.tar.gz
# （SHA-256 见 UPSTREAM.md）。只编译语言核心 + CrossStore 允许暴露的五个库
# （base/table/string/math/utf8）；按上游 library.json 的 srcFilter 排除
# lua.c/luac.c（独立解释器）与 linit/liolib/loslib/loadlib/ldblib/lcorolib
# —— 沙箱不需要 os/io/package/debug/coroutine，宿主也不调用 luaL_openlibs。
#
# LUAI_MAXCCALLS=50：C 调用嵌套上限压到 50（上游默认 200）。Lua 的 C 栈在
# ESP32 任务的 8KB 栈上很小，这个值由 CrossStore 上游钉死，别改大。

file(GLOB LUA_SOURCES "${CMAKE_CURRENT_LIST_DIR}/src/*.c")
list(REMOVE_ITEM LUA_SOURCES
    "${CMAKE_CURRENT_LIST_DIR}/src/lua.c"
    "${CMAKE_CURRENT_LIST_DIR}/src/luac.c"
    "${CMAKE_CURRENT_LIST_DIR}/src/linit.c"
    "${CMAKE_CURRENT_LIST_DIR}/src/liolib.c"
    "${CMAKE_CURRENT_LIST_DIR}/src/loslib.c"
    "${CMAKE_CURRENT_LIST_DIR}/src/loadlib.c"
    "${CMAKE_CURRENT_LIST_DIR}/src/ldblib.c"
    "${CMAKE_CURRENT_LIST_DIR}/src/lcorolib.c"
)

idf_component_register(
    SRCS ${LUA_SOURCES}
    INCLUDE_DIRS "src"
)

# 上游 C 代码按自己的风格写，-Werror 下会报 comment/unused 一类噪音；
# 只给本组件的编译加关掉这些警告的开关，不改源码。
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-error
    -Wno-unused-parameter
    -Wno-sign-compare
    -Wno-comment
)
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    LUAI_MAXCCALLS=50
)
