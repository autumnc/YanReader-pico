#include "CrossStoreRuntime.h"

#include <esp_log.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

namespace crossstore {
namespace {
constexpr const char* TAG = "LuaRT";
constexpr unsigned INSTRUCTION_STEP = 1000;
constexpr unsigned INSTRUCTION_LIMIT = 100000;
constexpr uint32_t CALLBACK_MS = 100;
constexpr uint32_t HOST_IO_MS = 1500;
enum class Function {
  Title,
  Clear,
  Grid,
  Caption,
  Row,
  Has,
  Tr,
  Millis,
  Read,
  Write,
  Input,
  Notify,
  Open,
  Size,
  Canvas,
  Box,
  Text,
  Line,
  Circle,
  Button,
  Hit,
  Exit,
  Sound,
  Allowed,
  Asset,
  Chat,
  Context,
  Qr,
  Measure,
  System,
  Raster
};
struct Binding {
  const char* name;
  Function function;
};
constexpr Binding BINDINGS[] = {
    {"title", Function::Title},     {"clear", Function::Clear},     {"row", Function::Row},
    {"has", Function::Has},         {"tr", Function::Tr},           {"millis", Function::Millis},
    {"read", Function::Read},       {"write", Function::Write},     {"input", Function::Input},
    {"notify", Function::Notify},   {"open", Function::Open},       {"grid", Function::Grid},
    {"caption", Function::Caption}, {"size", Function::Size},       {"canvas", Function::Canvas},
    {"box", Function::Box},         {"text", Function::Text},       {"line", Function::Line},
    {"circle", Function::Circle},   {"button", Function::Button},   {"hit", Function::Hit},
    {"exit", Function::Exit},       {"sound", Function::Sound},     {"allowed", Function::Allowed},
    {"asset", Function::Asset},     {"chat", Function::Chat},       {"context", Function::Context},
    {"qr", Function::Qr},           {"measure", Function::Measure}, {"system", Function::System},
    {"raster", Function::Raster}};

const char* checked(lua_State* state, int index, size_t maximum) {
  size_t size = 0;
  const char* value = luaL_checklstring(state, index, &size);
  if (size > maximum || std::memchr(value, 0, size)) luaL_error(state, "invalid string length");
  return value;
}
int16_t coordinate(lua_State* state, int index) {
  const auto value = luaL_checknumber(state, index);
  if (!std::isfinite(value) || value < -4096 || value > 4096) luaL_error(state, "invalid drawing coordinate");
  return static_cast<int16_t>(std::floor(value));
}
uint8_t option(lua_State* state, int index, unsigned maximum, unsigned fallback) {
  const auto value = luaL_optinteger(state, index, fallback);
  if (value < 0 || static_cast<unsigned long long>(value) > maximum) luaL_error(state, "invalid drawing option");
  return static_cast<uint8_t>(value);
}
}  // namespace

void* Runtime::allocate(void* user, void* ptr, size_t oldSize, size_t newSize) {
  auto& runtime = *static_cast<Runtime*>(user);
  if (!ptr) oldSize = 0;  // Lua passes a type tag in osize for new objects.
  if (!newSize) {
    runtime.host_.resizeAllocation(ptr, 0);
    runtime.used_ -= oldSize;
    return nullptr;
  }
  if (newSize > runtime.budget_ || runtime.used_ - oldSize > runtime.budget_ - newSize) return nullptr;
  // Lua objects have variable sizes and must survive callbacks. Every VM
  // allocation shares the declared hard ceiling; no persistent global VM exists.
  void* result = runtime.host_.resizeAllocation(ptr, newSize);
  if (result) {
    runtime.used_ = runtime.used_ - oldSize + newSize;
    runtime.peak_ = std::max(runtime.peak_, runtime.used_);
  }
  return result;
}

Runtime& Runtime::self(lua_State* state) { return **static_cast<Runtime**>(lua_getextraspace(state)); }

void Runtime::hook(lua_State* state, lua_Debug*) {
  auto& runtime = self(state);
  runtime.instructions_ += INSTRUCTION_STEP;
  if (runtime.instructions_ >= INSTRUCTION_LIMIT ||
      static_cast<int32_t>(runtime.host_.milliseconds() - runtime.deadline_) >= 0)
    luaL_error(state, "application execution budget exceeded");
}

int Runtime::initialize(lua_State* state) {
  luaL_requiref(state, "_G", luaopen_base, 1);
  lua_pop(state, 1);
  luaL_requiref(state, "table", luaopen_table, 1);
  lua_pop(state, 1);
  luaL_requiref(state, "string", luaopen_string, 1);
  lua_pop(state, 1);
  luaL_requiref(state, "math", luaopen_math, 1);
  lua_pop(state, 1);
  luaL_requiref(state, "utf8", luaopen_utf8, 1);
  lua_pop(state, 1);
  // Catching hook errors in Lua would let an app repeatedly exceed its fuel.
  for (const char* key : {"dofile", "loadfile", "load", "collectgarbage", "pcall", "xpcall", "print"}) {
    lua_pushnil(state);
    lua_setglobal(state, key);
  }
  lua_getglobal(state, "string");
  // C pattern matching cannot be interrupted by Lua's instruction hook.
  for (const char* key : {"dump", "find", "match", "gmatch", "gsub"}) {
    lua_pushnil(state);
    lua_setfield(state, -2, key);
  }
  lua_pop(state, 1);
  lua_newtable(state);
  for (const auto& binding : BINDINGS) {
    lua_pushinteger(state, static_cast<int>(binding.function));
    lua_pushcclosure(state, dispatch, 1);
    lua_setfield(state, -2, binding.name);
  }
  lua_setglobal(state, "app");
  return 0;
}

int Runtime::dispatch(lua_State* state) {
  auto& runtime = self(state);
  Host& host = runtime.host_;
  switch (static_cast<Function>(lua_tointeger(state, lua_upvalueindex(1)))) {
    case Function::Title:
      host.title(checked(state, 1, 95));
      return 0;
    case Function::Clear:
      host.clear();
      return 0;
    case Function::Grid: {
      const auto columns = luaL_checkinteger(state, 1);
      if (columns < 1 || columns > 6) return luaL_error(state, "grid columns must be 1..6");
      host.grid(static_cast<unsigned>(columns));
      return 0;
    }
    case Function::Caption:
      host.caption(checked(state, 1, 191));
      return 0;
    case Function::Row:
      lua_pushboolean(state, host.row(checked(state, 1, 95), lua_gettop(state) > 1 ? checked(state, 2, 191) : ""));
      return 1;
    case Function::Has:
      lua_pushboolean(state, host.capability(checked(state, 1, 31)));
      return 1;
    case Function::Tr:
      lua_pushvalue(state, host.chinese() ? 2 : 1);
      return 1;
    case Function::Millis:
      lua_pushinteger(state, host.milliseconds());
      return 1;
    case Function::Read: {
      const char* key = checked(state, 1, 31);
      const auto started = host.milliseconds();
      const bool ok = host.read(key, runtime.readBuffer_, sizeof(runtime.readBuffer_));
      runtime.accountHostIo(state, started);
      if (ok)
        lua_pushstring(state, runtime.readBuffer_);
      else
        lua_pushnil(state);
      return 1;
    }
    case Function::Write: {
      const char* key = checked(state, 1, 31);
      const char* value = checked(state, 2, 2048);
      const auto started = host.milliseconds();
      const bool ok = host.write(key, value, std::strlen(value));
      runtime.accountHostIo(state, started);
      lua_pushboolean(state, ok);
      return 1;
    }
    case Function::Input:
      host.input(checked(state, 1, 95), lua_gettop(state) > 1 ? checked(state, 2, 191) : "");
      return 0;
    case Function::Notify:
      host.notify(checked(state, 1, 191));
      return 0;
    case Function::Open:
      lua_pushboolean(state, host.open(checked(state, 1, 31)));
      return 1;
    case Function::Measure: {
      const char* text = checked(state, 1, 255);
      const int maximum = coordinate(state, 2);
      if (maximum < 1) return luaL_error(state, "invalid text width");
      int width = 0, height = 0;
      const auto font = option(state, 3, 3, 1);
      const auto started = host.milliseconds();
      host.measure(text, maximum, font, width, height);
      runtime.accountHostIo(state, started);
      lua_pushinteger(state, width);
      lua_pushinteger(state, height);
      return 2;
    }
    case Function::Context:
      lua_pushstring(state, host.context());
      return 1;
    case Function::Qr:
      if (!host.qr(coordinate(state, 1), coordinate(state, 2), coordinate(state, 3), checked(state, 4, 384)))
        return luaL_error(state, "QR code cannot fit");
      return 0;
    case Function::System:
    case Function::Chat: {
      const char* command = checked(state, 1, 31);
      const bool systemCall = static_cast<Function>(lua_tointeger(state, lua_upvalueindex(1))) == Function::System;
      const size_t argumentLimit = systemCall && !std::strcmp(command, "radio.play") ? 511 : 95;
      const char* argument = lua_gettop(state) >= 2 ? checked(state, 2, argumentLimit) : "";
      const auto value = luaL_optinteger(state, 3, 0);
      if (value < 0 || static_cast<unsigned long long>(value) > UINT32_MAX)
        return luaL_error(state, "invalid chat index");
      const char* text = lua_gettop(state) >= 4 ? checked(state, 4, 192) : "";
      ServiceReply reply;
      const auto started = host.milliseconds();
      const bool ok = systemCall ? host.system(command, argument, static_cast<int64_t>(value), reply)
                                 : host.chat(command, argument, static_cast<int64_t>(value), text, reply);
      runtime.accountHostIo(state, started);
      lua_pushboolean(state, ok);
      lua_pushstring(state, reply.text);
      lua_pushinteger(state, reply.a);
      lua_pushinteger(state, reply.b);
      lua_pushinteger(state, reply.c);
      lua_pushstring(state, reply.extra);
      return 6;
    }
    case Function::Asset: {
      const char* path = checked(state, 1, 127);
      const auto offset = luaL_optinteger(state, 2, 0), length = luaL_optinteger(state, 3, 2048);
      if (offset < 0 || offset > 32768 || length < 1 || length > 2048) return luaL_error(state, "invalid asset range");
      size_t bytes = 0;
      const auto started = host.milliseconds();
      const bool ok =
          host.asset(path, static_cast<size_t>(offset), runtime.readBuffer_, static_cast<size_t>(length), bytes);
      runtime.accountHostIo(state, started);
      if (ok && bytes <= static_cast<size_t>(length))
        lua_pushlstring(state, runtime.readBuffer_, bytes);
      else
        lua_pushnil(state);
      return 1;
    }
    case Function::Sound: {
      const char* cue = checked(state, 1, 31);
      const auto started = host.milliseconds();
      const bool ok = host.sound(cue);
      runtime.accountHostIo(state, started);
      lua_pushboolean(state, ok);
      return 1;
    }
    case Function::Allowed:
      lua_pushboolean(state, host.allowed(checked(state, 1, 31)));
      return 1;
    case Function::Size: {
      int width, height;
      host.size(width, height);
      lua_pushinteger(state, width);
      lua_pushinteger(state, height);
      return 2;
    }
    case Function::Canvas:
      if (!host.canvas()) return luaL_error(state, "canvas unavailable");
      return 0;
    case Function::Exit:
      host.exit();
      return 0;
    case Function::Box:
    case Function::Text:
    case Function::Line:
    case Function::Circle:
    case Function::Raster:
    case Function::Button:
    case Function::Hit: {
      DrawCommand command;
      const auto function = static_cast<Function>(lua_tointeger(state, lua_upvalueindex(1)));
      const bool interactive = function == Function::Button || function == Function::Hit;
      const int offset = interactive ? 1 : 0;
      if (interactive) {
        const auto id = luaL_checkinteger(state, 1);
        if (id < 1 || id > 32767) return luaL_error(state, "invalid control id");
        command.action = static_cast<int16_t>(id);
      }
      command.x = coordinate(state, offset + 1);
      command.y = coordinate(state, offset + 2);
      command.width = coordinate(state, offset + 3);
      command.height = coordinate(state, offset + 4);
      const char* label = "";
      switch (function) {
        case Function::Text:
          command.kind = DrawKind::Text;
          label = checked(state, 5, 255);
          command.font = option(state, 6, 3, 1);
          command.align = option(state, 7, 2, 0);
          command.gray = option(state, 8, 15, 0);
          break;
        case Function::Raster:
          command.kind = DrawKind::Raster;
          command.radius = option(state, 5, 128, 0);
          command.font = option(state, 6, 128, 0);
          label = checked(state, 7, Canvas::MAX_TEXT - 1);
          if (!command.radius || !command.font || std::strlen(label) != command.radius * command.font)
            return luaL_error(state, "invalid raster size");
          for (const char* shade = label; *shade; ++shade)
            if (!((*shade >= '0' && *shade <= '9') || (*shade >= 'A' && *shade <= 'F')))
              return luaL_error(state, "invalid raster shade");
          break;
        case Function::Button:
          command.kind = DrawKind::Button;
          label = checked(state, 6, 95);
          command.style = option(state, 7, 3, 0);
          break;
        case Function::Hit:
          command.kind = DrawKind::Hit;
          break;
        case Function::Box:
          command.kind = DrawKind::Box;
          break;
        case Function::Line:
          command.kind = DrawKind::Line;
          break;
        case Function::Circle:
          command.kind = DrawKind::Circle;
          break;
        default:
          return luaL_error(state, "invalid drawing operation");
      }
      if (function == Function::Box || function == Function::Circle || function == Function::Line) {
        command.gray = option(state, 5, 15, 0);
        command.radius = option(state, 6, 64, 0);
        command.style = option(state, 7, 4, 0);
      }
      if (!host.draw(command, label)) return luaL_error(state, "drawing capacity or control geometry exceeded");
      return 0;
    }
  }
  return luaL_error(state, "unknown host function");
}

void Runtime::arm() {
  instructions_ = 0;
  hostIoMs_ = 0;
  deadline_ = host_.milliseconds() + CALLBACK_MS;
  lua_sethook(state_, hook, LUA_MASKCOUNT, INSTRUCTION_STEP);
}

void Runtime::accountHostIo(lua_State* state, uint32_t started) {
  // Bounded HAL calls can wait on SD, font caches or device queues. Their
  // latency is not Lua CPU time, but has a separate cumulative callback cap.
  // Called after the host returns: no C++ RAII spans a possible Lua longjmp.
  const uint32_t elapsed = host_.milliseconds() - started;
  if (elapsed > HOST_IO_MS - hostIoMs_) luaL_error(state, "application I/O budget exceeded");
  hostIoMs_ += elapsed;
  deadline_ += elapsed;
}

bool Runtime::finishCall(int status) {
  if (status == LUA_OK) return true;
  const char* message = lua_type(state_, -1) == LUA_TSTRING ? lua_tostring(state_, -1) : nullptr;
  std::snprintf(error_, sizeof(error_), "%s", message ? message : "application failed");
  // 应用报错原来只上屏、不进日志：串口里看不出是哪个回调、哪一行炸的。
  // Lua 的 luaL_error 会在消息前面带上 "main.lua:45:"，这一行就是现场。
  ESP_LOGE(TAG, "回调 %s 报错: %s", eventName_ ? eventName_ : "load", error_);
  close();
  return false;
}

bool Runtime::load(const char* source, size_t size, size_t budget) {
  close();
  error_[0] = 0;
  peak_ = 0;
  if (!source || !size || size > MAX_SCRIPT_BYTES || budget < 16384 || budget > MAX_VM_BYTES) {
    std::strcpy(error_, "invalid application limits");
    return false;
  }
  budget_ = budget;
  state_ = lua_newstate(allocate, this);
  if (!state_) {
    std::strcpy(error_, "not enough application memory");
    return false;
  }
  *static_cast<Runtime**>(lua_getextraspace(state_)) = this;
  lua_pushcfunction(state_, initialize);
  if (!finishCall(lua_pcall(state_, 0, 0, 0))) return false;
  arm();
  if (!finishCall(luaL_loadbufferx(state_, source, size, "@main.lua", "t"))) return false;
  if (!finishCall(lua_pcall(state_, 0, 1, 0))) return false;
  if (!lua_istable(state_, -1)) {
    std::strcpy(error_, "main.lua must return an application table");
    close();
    return false;
  }
  // Keep the callback table at stack index 1 throughout the activity lifetime.
  return event("on_start");
}

int Runtime::invoke(lua_State* state) {
  auto& runtime = self(state);
  // Raw lookup avoids running a package-supplied __index outside pcall.
  lua_pushstring(state, runtime.eventName_);
  lua_rawget(state, 1);
  if (lua_isnil(state, -1)) return 0;
  if (!lua_isfunction(state, -1)) return luaL_error(state, "invalid application callback");
  const bool keyEvent = std::strcmp(runtime.eventName_, "on_key") == 0;
  runtime.handled_ = !keyEvent;
  lua_pushinteger(state, runtime.eventIndex_);
  lua_pushstring(state, runtime.eventText_);
  lua_pushinteger(state, runtime.eventX_);
  lua_pushinteger(state, runtime.eventY_);
  lua_call(state, 4, keyEvent ? 1 : 0);
  if (keyEvent) {
    runtime.handled_ = lua_isboolean(state, -1) && lua_toboolean(state, -1);
    lua_pop(state, 1);
  }
  return 0;
}

bool Runtime::event(const char* name, int index, const char* text, int x, int y) {
  if (!state_) return false;
  handled_ = false;
  eventName_ = name;
  eventIndex_ = index;
  eventText_ = text;
  eventX_ = x;
  eventY_ = y;
  lua_pushcfunction(state_, invoke);
  lua_pushvalue(state_, 1);
  arm();
  const bool ok = finishCall(lua_pcall(state_, 1, 0, 0));
  // 单个回调累计的宿主 I/O 离上限（1.5s）有多近。超限会直接崩应用，
  // 所以在半程就留一笔，能看出是哪个回调、哪类调用吃掉的。
  if (ok && hostIoMs_ >= 500) ESP_LOGW(TAG, "回调 %s 宿主 I/O 已用 %u ms", name, static_cast<unsigned>(hostIoMs_));
  return ok;
}

void Runtime::close() {
  if (state_) lua_close(state_);
  state_ = nullptr;
}
}  // namespace crossstore
