#pragma once

#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <stdint.h>

#include <cassert>

#include "DitherUtils.h"  // setNibble() for the .pxc payload

// Direct framebuffer writer that eliminates per-pixel overhead from the image
// rendering hot path.  Pre-computes orientation transform as linear coefficients
// and caches render-mode state so the inner loop is: one multiply, one add,
// one shift, and one AND per pixel — no branches, no method calls.
//
// Caller is responsible for ensuring (outX, outY) are within screen bounds.
// ImageBlock::render() already validates this before entering the pixel loop,
// and the JPEG/PNG callbacks pre-clamp destination ranges to screen bounds.
struct DirectPixelWriter {
  uint8_t* fb;
  GfxRenderer::RenderMode mode;
  uint16_t displayWidthBytes;  // Runtime framebuffer stride (X4: 100, X3: 99)
  // Active write target: for tiled grayscale, fb is the band scratch, originY is
  // the band's top physical row, and clipRows is the band height. Off-band
  // pixels are dropped. With no strip active these collapse to the full frame
  // (originY 0, clipRows panelHeight) so the clip doubles as a bounds guard.
  int originY;
  int clipRows;

  // Orientation is collapsed into a linear transform:
  //   phyX = phyXBase + x * phyXStepX + y * phyXStepY
  //   phyY = phyYBase + x * phyYStepX + y * phyYStepY
  int phyXBase, phyYBase;
  int phyXStepX, phyYStepX;  // per logical-X step
  int phyXStepY, phyYStepY;  // per logical-Y step

  // Row-precomputed: the Y-dependent portion of the physical coords
  int rowPhyXBase, rowPhyYBase;

  void init(GfxRenderer& renderer) {
    fb = renderer.getWriteTarget();
    originY = renderer.getWriteOriginY();
    clipRows = renderer.getWriteRows();
    mode = renderer.getRenderMode();
    displayWidthBytes = renderer.getDisplayWidthBytes();

    const int phyW = renderer.getDisplayWidth();
    const int phyH = renderer.getDisplayHeight();

    // Read Pico port: this framebuffer is epdiy's native one, so the logical→
    // physical map must be *exactly* epdiy's `_rotate()`, otherwise an image is
    // placed 180° away from the text/borders drawn around it with the same
    // logical coordinates. Screen_reader pairs these flags with the board
    // rotation as PortraitInverted ↔ EPD_ROT_INVERTED_PORTRAIT and
    // LandscapeCounterClockwise ↔ EPD_ROT_LANDSCAPE; the other two blocks mirror
    // epdiy for completeness (this port never selects them).
    //   EPD_ROT_LANDSCAPE            : phyX = x,     phyY = y
    //   EPD_ROT_INVERTED_PORTRAIT    : phyX = y,     phyY = (H-1) - x
    //   EPD_ROT_INVERTED_LANDSCAPE   : phyX = (W-1) - x, phyY = (H-1) - y
    //   EPD_ROT_PORTRAIT             : phyX = y,     phyY = (W-1) - x
    // phyW/phyH come from GfxRenderer's panelWidth/Height, which begin() latches
    // while the panel is still in landscape, so they are the physical 1216×684.
    switch (renderer.getOrientation()) {
      case GfxRenderer::Portrait:
        // phyX = y, phyY = (phyW-1) - x
        phyXBase = 0;
        phyYBase = phyW - 1;
        phyXStepX = 0;
        phyYStepX = -1;
        phyXStepY = 1;
        phyYStepY = 0;
        break;
      case GfxRenderer::LandscapeClockwise:
        // phyX = (phyW-1) - x, phyY = (phyH-1) - y
        phyXBase = phyW - 1;
        phyYBase = phyH - 1;
        phyXStepX = -1;
        phyYStepX = 0;
        phyXStepY = 0;
        phyYStepY = -1;
        break;
      case GfxRenderer::PortraitInverted:  // ← epdiy EPD_ROT_INVERTED_PORTRAIT
        // phyX = y, phyY = (phyH-1) - x
        phyXBase = 0;
        phyYBase = phyH - 1;
        phyXStepX = 0;
        phyYStepX = -1;
        phyXStepY = 1;
        phyYStepY = 0;
        break;
      case GfxRenderer::LandscapeCounterClockwise:
        // phyX = x, phyY = y
        phyXBase = 0;
        phyYBase = 0;
        phyXStepX = 1;
        phyYStepX = 0;
        phyXStepY = 0;
        phyYStepY = 1;
        break;
      default:
        // Fallback to LandscapeCounterClockwise (identity transform)
        phyXBase = 0;
        phyYBase = 0;
        phyXStepX = 1;
        phyYStepX = 0;
        phyXStepY = 0;
        phyYStepY = 1;
        break;
    }
  }

  // Call once per row before the column loop.
  // Pre-computes the Y-dependent portion so writePixel() only needs the X part.
  inline void beginRow(int logicalY) {
    rowPhyXBase = phyXBase + logicalY * phyXStepY;
    rowPhyYBase = phyYBase + logicalY * phyYStepY;
  }

  // For the current row (set via beginRow), narrow [colStart, colEnd) to the
  // columns whose pixels fall inside the active strip band. writePixel() would
  // clip the rest anyway, but on a strip pass that is most of a full-page image
  // (only ~one strip-height worth of columns survive in portrait); skipping them
  // here avoids the per-pixel unpack+transform entirely. For full-frame passes
  // (clipRows == panel height) the range is unchanged. xBase is the logical X of
  // column 0; the band test mirrors writePixel(): 0 <= phyY - originY < clipRows.
  inline void bandColRange(int xBase, int width, int& colStart, int& colEnd) const {
    // init() only ever sets phyYStepX to 0, +1, or -1; the +1/-1 solve below
    // relies on that.
    assert(phyYStepX == 0 || phyYStepX == 1 || phyYStepX == -1);
    colStart = 0;
    colEnd = width;
    if (phyYStepX == 0) {
      // phyY is constant across the row: the whole row is in-band or out.
      const int sy = rowPhyYBase - originY;
      if (static_cast<unsigned>(sy) >= static_cast<unsigned>(clipRows)) colEnd = 0;
      return;
    }
    // phyY = rowPhyYBase + logicalX * phyYStepX (phyYStepX is +1 or -1).
    // Solve originY <= phyY <= originY + clipRows - 1 for logicalX.
    const int loY = originY;
    const int hiY = originY + clipRows - 1;
    int xLo, xHi;
    if (phyYStepX > 0) {
      xLo = loY - rowPhyYBase;
      xHi = hiY - rowPhyYBase;
    } else {
      xLo = rowPhyYBase - hiY;
      xHi = rowPhyYBase - loY;
    }
    const int cs = xLo - xBase;
    const int ce = xHi - xBase + 1;  // exclusive
    if (cs > colStart) colStart = cs;
    if (ce < colEnd) colEnd = ce;
    if (colStart < 0) colStart = 0;
    if (colEnd > width) colEnd = width;
    if (colStart > colEnd) colStart = colEnd;
  }

  // Write a single 2-bit dithered pixel value to the framebuffer.
  // Must be called after beginRow() for the current row.
  // No bounds checking — caller guarantees coordinates are valid.
  inline void writePixel(int logicalX, uint8_t pixelValue, bool writeWhiteInBw = false) const {
    auto pixel = GfxRenderer::mapTwoBitPixel(mode, pixelValue);
    if (mode == GfxRenderer::BW && writeWhiteInBw && pixelValue >= 3) pixel = {true, false};
    if (!pixel.draw) return;

    const int phyX = rowPhyXBase + logicalX * phyXStepX;
    const int phyY = rowPhyYBase + logicalX * phyYStepX;

    // Band-local row. The unsigned compare drops both off-band pixels (strip
    // mode) and any out-of-frame row (full-frame mode) in one branch.
    const int sy = phyY - originY;
    if (static_cast<unsigned>(sy) >= static_cast<unsigned>(clipRows)) return;

    // Physical column guard: a logical row whose phyX falls outside the panel
    // would otherwise run off the end of the framebuffer. displayWidthBytes is
    // the physical row stride (epd_width()/2), so *2 is epd_width().
    if (static_cast<unsigned>(phyX) >= static_cast<unsigned>(displayWidthBytes) * 2u) return;

    // Read Pico's framebuffer is epdiy's native 4bpp, NOT the one-bit-per-pixel
    // buffer the rest of this file was written against: one byte holds two
    // pixels, the LOW nibble is the even physical column and the HIGH nibble the
    // odd one (see epdiy's epd_draw_pixel() and main/gfx/fb_fast.h). Packing a
    // bit per pixel here collapsed eight logical columns into the two pixels of
    // one byte, so every inline image came out squeezed to a quarter of its box
    // width ("压扁") while text — drawn through fb_fast — stayed correct.
    //
    // The index is uint32_t, NOT uint16_t: it has to cover the whole
    // framebuffer, `displayWidthBytes * panelHeight` = 415,872 bytes here. On
    // the boards this was written for it happened to fit in 16 bits (X4: 100 x
    // 480 -> 47,999 max; X3: 99 x 528 -> 52,271 max), so the narrower type was
    // invisible; on the Pico every index past 65,535 wrapped to an earlier row.
    const uint32_t byteIndex = static_cast<uint32_t>(sy) * displayWidthBytes + static_cast<uint32_t>(phyX >> 1);
    const uint8_t ink = GfxRenderer::framebufferState(mode, pixel.state) ? 0x00 : 0x0F;  // 0=黑 15=白

    if (phyX & 1) {
      fb[byteIndex] = static_cast<uint8_t>((fb[byteIndex] & 0x0F) | (ink << 4));
    } else {
      fb[byteIndex] = static_cast<uint8_t>((fb[byteIndex] & 0xF0) | ink);
    }
  }

  // Write one 16-level gray value (0 = 全墨 … 15 = 全白) to the native 4bpp
  // framebuffer. Same addressing and the same two guards as writePixel() above
  // (band/row clip, physical column bound) — deliberately *not* routed through
  // fb_fast_set_gray(), which owns a core0-side static cache and its own rotation
  // table and therefore has no band semantics. Skips mapTwoBitPixel()/renderMode
  // entirely: the byte is epdiy's own pixel format, exactly what
  // epd_draw_pixel(x, y, gray << 4) writes.
  //
  // ⚠ byteIndex must stay uint32_t for the reason documented above (415,872 B
  // framebuffer on this panel).
  inline void writeGray16(int logicalX, uint8_t level) const {
    const int phyX = rowPhyXBase + logicalX * phyXStepX;
    const int phyY = rowPhyYBase + logicalX * phyYStepX;

    const int sy = phyY - originY;
    if (static_cast<unsigned>(sy) >= static_cast<unsigned>(clipRows)) return;
    if (static_cast<unsigned>(phyX) >= static_cast<unsigned>(displayWidthBytes) * 2u) return;

    const uint32_t byteIndex = static_cast<uint32_t>(sy) * displayWidthBytes + static_cast<uint32_t>(phyX >> 1);
    const uint8_t ink = static_cast<uint8_t>(level & 0x0F);
    // 偶列低半字节、奇列高半字节（epdiy epd_draw_pixel 的约定）。
    if (phyX & 1) {
      fb[byteIndex] = static_cast<uint8_t>((fb[byteIndex] & 0x0F) | (ink << 4));
    } else {
      fb[byteIndex] = static_cast<uint8_t>((fb[byteIndex] & 0xF0) | ink);
    }
  }
};

// Direct cache writer that eliminates per-pixel overhead from PixelCache::setPixel().
// Pre-computes row pointer so the inner loop is just byte index + bit manipulation.
//
// The cache buffer is a small streaming band (e.g. 16 rows), not the full image,
// so a band-relative row/column that lands outside it would corrupt adjacent
// heap. This writer therefore bounds-checks every access: beginRow() invalidates
// the row when it falls outside the band, and writePixel() drops out-of-range
// columns. This path only runs during the single decode that populates the
// cache, never on the screen render hot path, so the checks are cheap.
struct DirectCacheWriter {
  uint8_t* buffer;
  int bytesPerRow;
  int bandRows;
  int originX;
  uint8_t* rowPtr;  // Pre-computed for current row; nullptr if row is out of band

  void init(uint8_t* cacheBuffer, int cacheBytesPerRow, int cacheBandRows, int cacheOriginX) {
    buffer = cacheBuffer;
    bytesPerRow = cacheBytesPerRow;
    bandRows = cacheBandRows;
    originX = cacheOriginX;
    rowPtr = nullptr;
  }

  // Call once per row before the column loop. Drops rows outside the band.
  inline void beginRow(int screenY, int cacheOriginY) {
    const int localRow = screenY - cacheOriginY;
    rowPtr = (static_cast<unsigned>(localRow) < static_cast<unsigned>(bandRows))
                 ? buffer + (size_t)localRow * bytesPerRow
                 : nullptr;
  }

  // Write a 2-bit pixel value. Drops the write if the row is out of band or the
  // column is out of range.
  inline void writePixel(int screenX, uint8_t value) const {
    if (!rowPtr) return;
    const int localX = screenX - originX;
    const int byteIdx = localX >> 2;  // localX / 4
    if (static_cast<unsigned>(byteIdx) >= static_cast<unsigned>(bytesPerRow)) return;
    const int bitShift = 6 - (localX & 3) * 2;  // MSB first: pixel 0 at bits 6-7
    rowPtr[byteIdx] = (rowPtr[byteIdx] & ~(0x03 << bitShift)) | ((value & 0x03) << bitShift);
  }

  // Write one 16-level gray value (0 = 全墨 … 15 = 全白) into the pixel cache.
  // The cache is 4bpp like the framebuffer itself (two pixels per byte, LOW
  // nibble = even column), so the payload can be memcpy'd back pixel-for-pixel
  // and the cached image is no longer re-quantized on every render.
  inline void writeGray16(int screenX, uint8_t level) const {
    if (!rowPtr) return;
    const int localX = screenX - originX;
    // The unsigned compare covers both ends (a negative localX wraps huge).
    if (static_cast<unsigned>(localX) >= static_cast<unsigned>(bytesPerRow) * 2u) return;
    setNibble(rowPtr, localX, static_cast<uint8_t>(level & 0x0F));
  }
};
