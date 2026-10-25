#pragma once

#include <algorithm>
#include <cstdio>
#include <iterator>

#include "CrossStoreCanvas.h"

namespace crossstore {
// Stream the existing display list; no framebuffer copy or whole-document
// allocation. Controls are excluded so applications can export their artwork.
template <class Writer>
bool writeSvg(const Canvas& canvas, int width, int height, Writer&& write) {
  char buffer[256];
  auto format = [&](const char* pattern, auto... args) {
    const int size = std::snprintf(buffer, sizeof(buffer), pattern, args...);
    return size >= 0 && static_cast<size_t>(size) < sizeof(buffer) && write(buffer, size);
  };
  if (width < 1 || height < 1 || width > 4096 || height > 4096 ||
      !format("<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 %d %d\">\n", width, height) ||
      !format("<rect width=\"%d\" height=\"%d\" fill=\"white\"/>\n", width, height))
    return false;
  for (size_t i = 0; i < canvas.count; ++i) {
    const auto& c = canvas.commands[i];
    const unsigned shade = c.gray * 17u;
    switch (c.kind) {
      case DrawKind::Box:
      case DrawKind::Circle: {
        const int radius = c.kind == DrawKind::Circle ? std::min(c.width, c.height) / 2 : c.radius;
        if (!format("<rect x=\"%d\" y=\"%d\" width=\"%d\" height=\"%d\" rx=\"%d\" ", c.x, c.y, c.width, c.height,
                    radius))
          return false;
        if (c.style) {
          if (!format("fill=\"none\" stroke=\"#%02x%02x%02x\" stroke-width=\"%u\"/>\n", shade, shade, shade, c.style))
            return false;
        } else if (!format("fill=\"#%02x%02x%02x\"/>\n", shade, shade, shade))
          return false;
        break;
      }
      case DrawKind::Line:
        if (!format("<path d=\"M%d %dL%d %d\" fill=\"none\" stroke=\"#%02x%02x%02x\" stroke-width=\"%u\"/>\n", c.x,
                    c.y + (c.radius ? c.height - 1 : 0), c.x + c.width - 1, c.y + (c.radius ? 0 : c.height - 1), shade,
                    shade, shade, std::max<unsigned>(1, c.style)))
          return false;
        break;
      case DrawKind::Text: {
        constexpr unsigned fonts[] = {18, 24, 30, 46};
        constexpr const char* anchors[] = {"start", "middle", "end"};
        if (c.font >= std::size(fonts) || c.align >= std::size(anchors)) return false;
        if (!format("<text x=\"%d\" y=\"%d\" font-family=\"sans-serif\" font-size=\"%u\" text-anchor=\"%s\" "
                    "fill=\"#%02x%02x%02x\">",
                    c.x + c.width * c.align / 2, c.y + fonts[c.font], fonts[c.font], anchors[c.align], shade, shade,
                    shade))
          return false;
        const char* text = canvas.text + c.text;
        for (; *text; ++text) {
          if (static_cast<unsigned char>(*text) < 32 && *text != '\n' && *text != '\r' && *text != '\t') return false;
          switch (*text) {
            case '&':
              if (!write("&amp;", 5)) return false;
              break;
            case '<':
              if (!write("&lt;", 4)) return false;
              break;
            case '>':
              if (!write("&gt;", 4)) return false;
              break;
            default:
              if (!write(text, 1)) return false;
              break;
          }
        }
        if (!write("</text>\n", 8)) return false;
        break;
      }
      case DrawKind::Button:
      case DrawKind::Hit:
        break;
      case DrawKind::Qr:
      case DrawKind::Raster:
        // QR modules are host-owned; silently dropping one would corrupt an export.
        return false;
    }
  }
  return write("</svg>\n", 7);
}
}  // namespace crossstore
