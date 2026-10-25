#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace crossstore {
enum class DrawKind : uint8_t { Box, Text, Line, Circle, Button, Hit, Qr, Raster };
struct DrawCommand {
  DrawKind kind = DrawKind::Box;
  int16_t x = 0, y = 0, width = 0, height = 0, action = 0;
  uint16_t text = 0;
  uint8_t gray = 0, radius = 0, font = 1, align = 0, style = 0;
  // Raster reuses radius/font as source width/height; text stores 0..F shades.
};

// An activity allocates this once when a script requests a canvas, then reuses
// it. A bounded display list replaces a second framebuffer and keeps Lua off
// the rendering task. Worst case is sizeof(Canvas), about 20 KiB. The 512-command bound fits a full 15 x 15 board
// with separate white fills/outlines, grid lines and a small toolbar.
struct Canvas {
  static constexpr size_t MAX_COMMANDS = 512;
  static constexpr size_t MAX_TEXT = 8192;
  static constexpr size_t MAX_CONTROLS = 24;
  DrawCommand commands[MAX_COMMANDS]{};
  char text[MAX_TEXT]{};
  uint16_t controls[MAX_CONTROLS]{};
  size_t count = 0, textUsed = 0, controlCount = 0;

  void clear() { count = textUsed = controlCount = 0; }
  bool add(DrawCommand command, const char* label) {
    const size_t length = std::strlen(label) + 1;
    const bool interactive = command.kind == DrawKind::Button || command.kind == DrawKind::Hit;
    if (count == MAX_COMMANDS || length > MAX_TEXT - textUsed || command.width <= 0 || command.height <= 0 ||
        (interactive && controlCount == MAX_CONTROLS))
      return false;
    if (interactive) {
      for (size_t i = 0; i < controlCount; ++i) {
        const auto& other = commands[controls[i]];
        if (other.action == command.action) return false;
        // Shared drawing/hit geometry: controls cannot overlap, including an
        // invisible board region and its neighbouring toolbar.
        if (command.x < other.x + other.width && command.x + command.width > other.x &&
            command.y < other.y + other.height && command.y + command.height > other.y)
          return false;
      }
      controls[controlCount++] = static_cast<uint16_t>(count);
    }
    command.text = static_cast<uint16_t>(textUsed);
    std::memcpy(text + textUsed, label, length);
    textUsed += length;
    commands[count++] = command;
    return true;
  }
};
}  // namespace crossstore
