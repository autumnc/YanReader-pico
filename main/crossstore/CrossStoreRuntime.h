#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>

#include "CrossStoreCanvas.h"

struct lua_State;
struct lua_Debug;

namespace crossstore {

constexpr unsigned API_VERSION = 8;
constexpr size_t MAX_SCRIPT_BYTES = 32768;
constexpr size_t MAX_VM_BYTES = 131072;
constexpr size_t MAX_ROWS = 32;

struct ServiceReply {
  char text[193]{}, extra[77]{};
  int64_t a = 0, b = 0, c = 0;
};

// All callbacks finish before Lua resumes. Implementations must bound IO and
// copy arguments they retain; Lua owns the input pointers only during the call.
class Host {
 public:
  virtual ~Host() = default;
  virtual void* resizeAllocation(void* ptr, size_t bytes) {
    if (!bytes) {
      std::free(ptr);
      return nullptr;
    }
    return std::realloc(ptr, bytes);
  }
  virtual void title(const char* text) = 0;
  virtual void clear() = 0;
  virtual void grid(unsigned columns) = 0;
  virtual void caption(const char* text) = 0;
  virtual bool row(const char* label, const char* value) = 0;
  virtual bool capability(const char* name) const = 0;
  virtual bool chinese() const = 0;
  virtual uint32_t milliseconds() const = 0;
  virtual bool read(const char* key, char* out, size_t capacity) = 0;
  virtual bool write(const char* key, const char* value, size_t size) = 0;
  virtual void input(const char* title, const char* initial) = 0;
  virtual void notify(const char* message) = 0;
  virtual bool open(const char* route) = 0;
  virtual void size(int& width, int& height) const { width = height = 0; }
  virtual bool canvas() { return false; }
  virtual bool draw(const DrawCommand&, const char*) { return false; }
  virtual void exit() {}
  virtual bool asset(const char*, size_t, char*, size_t, size_t&) { return false; }
  virtual void measure(const char*, int, unsigned, int& width, int& height) { width = height = 0; }
  virtual const char* context() const { return ""; }
  virtual bool system(const char*, const char*, int64_t, ServiceReply&) { return false; }
  virtual bool chat(const char*, const char*, int64_t, const char*, ServiceReply&) { return false; }
  virtual bool qr(int, int, int, const char*) { return false; }
  virtual bool sound(const char*) { return false; }
  virtual bool allowed(const char*) const { return false; }
};

class Runtime {
 public:
  explicit Runtime(Host& host) : host_(host) {}
  ~Runtime() { close(); }
  Runtime(const Runtime&) = delete;
  Runtime& operator=(const Runtime&) = delete;
  bool load(const char* source, size_t size, size_t budget = 65536);
  bool event(const char* name, int index = 0, const char* text = "", int x = -1, int y = -1);
  // on_key opts in only with boolean true; older callbacks are handled when
  // present, irrespective of their return value.
  bool handled() const { return handled_; }
  void close();
  const char* error() const { return error_; }
  size_t used() const { return used_; }
  size_t peak() const { return peak_; }

 private:
  Host& host_;
  lua_State* state_ = nullptr;
  size_t used_ = 0, peak_ = 0, budget_ = 0;
  uint32_t instructions_ = 0, deadline_ = 0;
  uint32_t hostIoMs_ = 0;
  char error_[160]{};
  char readBuffer_[2049]{};
  static void* allocate(void*, void*, size_t, size_t);
  static void hook(lua_State*, lua_Debug*);
  static int initialize(lua_State*);
  static int invoke(lua_State*);
  static int dispatch(lua_State*);
  static Runtime& self(lua_State*);
  void arm();
  void accountHostIo(lua_State* state, uint32_t started);
  bool finishCall(int status);
  const char* eventName_ = nullptr;
  const char* eventText_ = nullptr;
  int eventIndex_ = 0;
  int eventX_ = -1, eventY_ = -1;
  bool handled_ = false;
};

}  // namespace crossstore
