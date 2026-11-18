#pragma once

#include <algorithm>
#include <cstdint>

#include "PixFormat.h"

// Glyph rasterizer that resolves orientation, clipping and framebuffer
// addressing once per glyph instead of once per pixel. It has no dependency
// on GfxRenderer so it can be unit-tested on the host.
namespace glyphBitmap {

// Framebuffer plane a glyph is painted into.
enum class Plane : uint8_t { BW, GrayLSB, GrayMSB };

// Position of glyph pixel (0, 0) plus the step for one move along the glyph's
// x and y axes, all in one coordinate space. The axes must describe an
// orthogonal unit rotation: each is +/-1 on exactly one component.
struct Frame {
  int x;
  int y;
  int dxX;
  int dxY;
  int dyX;
  int dyY;
};

// Where a glyph lands in physical framebuffer space.
struct Target {
  uint8_t* buffer;  // First byte of row originY (framebuffer or strip scratch)
  int width;        // Physical panel width in pixels
  int stride;       // Bytes per physical row
  int originY;      // First physical row held by buffer
  int rows;         // Number of physical rows held by buffer
  Frame frame;      // Physical placement of the glyph
  // 4 bpp canvas (CP_RENDER_4BPP): one byte holds two pixels, even x in the low
  // nibble, and a source coverage level is written as an optical level instead
  // of a bit. false is the 1 bpp path, which is what the tiled-grayscale strip
  // scratch always is — that scratch holds controller *planes*, not a canvas.
  bool fourBit = false;
  // r161 MENU AA: paint the glyph as a BINARY mask instead of a coverage ramp.
  // Every covered pixel (level 1..3, exactly what the 1 bpp path's `levels =
  // 0x0e` painted) becomes ink and nothing in between is written, so a frame
  // built with this set carries no intermediate optical level and the panel's
  // binary DU table can render it exactly. false = today's AA blit.
  bool binary = false;
};

// Half-open rectangle in glyph-local pixel coordinates.
struct Clip {
  int left;
  int top;
  int right;
  int bottom;
};

// Narrow [start, end) on one glyph axis so that base + i * step stays within
// [lower, upper). step is +1 or -1 because the transform is orthogonal.
inline void clipAxis(int base, int step, int lower, int upper, int& start, int& end) {
  if (step > 0) {
    start = std::max(start, lower - base);
    end = std::min(end, upper - base);
  } else {
    start = std::max(start, base - upper + 1);
    end = std::min(end, base - lower + 1);
  }
}

// Narrow a glyph-local clip so every kept pixel maps inside the half-open
// rectangle [left, right) x [top, bottom) of the space frame is expressed in.
// Which glyph axis runs along that space's x depends on the rotation:
// dxX != 0 means glyph x does. Runs twice per glyph, so it stays inline
// under -Os.
__attribute__((always_inline)) inline void clipToRect(const Frame& frame, int left, int top, int right, int bottom,
                                                      Clip& clip) {
  if (frame.dxX != 0) {
    clipAxis(frame.x, frame.dxX, left, right, clip.left, clip.right);
    clipAxis(frame.y, frame.dyY, top, bottom, clip.top, clip.bottom);
  } else {
    clipAxis(frame.x, frame.dyX, left, right, clip.top, clip.bottom);
    clipAxis(frame.y, frame.dxY, top, bottom, clip.left, clip.right);
  }
}

// Keep pixel writes inline when decoding a group of four pixels.
__attribute__((always_inline)) inline void paint(uint8_t* buffer, int destination, uint8_t ink, uint8_t levels,
                                                 bool clearBits) {
  if ((levels & (1u << ink)) == 0) return;
  const uint8_t mask = 0x80u >> (destination & 7);
  if (clearBits)
    buffer[destination >> 3] &= static_cast<uint8_t>(~mask);
  else
    buffer[destination >> 3] |= mask;
}

// Store one 4 bpp pixel. idx is a flat PIXEL index (row * (stride*2) + col), so
// the caller can keep the same axis-stepping arithmetic it uses in 1 bpp mode.
__attribute__((always_inline)) inline void paint4(uint8_t* buffer, int idx, uint8_t nibble) {
  uint8_t* q = buffer + (idx >> 1);
  if (idx & 1)
    *q = static_cast<uint8_t>((*q & 0x0F) | static_cast<uint8_t>(nibble << 4));
  else
    *q = static_cast<uint8_t>((*q & 0xF0) | nibble);
}

// 4 bpp glyph paint. The coverage levels keep their MEANING here (0 white,
// 1 light, 2 dark, 3 black) instead of being masked onto a plane, which is the
// whole point of the format: a 25%-covered edge pixel becomes 0xA, not ink.
// `state == false` paints the glyph's shape in paper (white-on-black overlays),
// so the mapping is inverted rather than skipped — the 1 bpp path does the same
// thing by setting bits instead of clearing them.
inline void draw4(const uint8_t* bitmap, int width, int height, bool twoBit, bool state, const Target& target,
                  const Clip& clipIn) {
  Clip clip = clipIn;
  clip.left = std::max(clip.left, 0);
  clip.top = std::max(clip.top, 0);
  clip.right = std::min(clip.right, width);
  clip.bottom = std::min(clip.bottom, height);
  clipToRect(target.frame, 0, target.originY, target.width, target.originY + target.rows, clip);
  if (clip.left >= clip.right || clip.top >= clip.bottom) return;

  const Frame& frame = target.frame;
  const int stridePx = target.stride * 2;  // pixels per row in 4 bpp
  const int stepX = frame.dxY * stridePx + frame.dxX;
  const int stepY = frame.dyY * stridePx + frame.dyX;
  int rowIdx = (frame.y - target.originY) * stridePx + frame.x + clip.left * stepX + clip.top * stepY;
  for (int y = clip.top; y < clip.bottom; ++y, rowIdx += stepY) {
    int source = y * width + clip.left;
    int destination = rowIdx;
    for (int x = clip.left; x < clip.right; ++x, ++source, destination += stepX) {
      uint8_t level;
      if (twoBit) {
        level = (bitmap[source >> 2] >> (6 - (source & 3) * 2)) & 0x3;
        // A glyph bitmap is a MASK, not a picture: level 0 is "no coverage" and
        // must leave the canvas alone. Painting it would erase whatever the
        // glyph sits on (and the 1 bpp path never painted it either — that is
        // what `levels = 0x0e` spells out).
        if (level == 0) continue;
      } else {
        if (((bitmap[source >> 3] >> (7 - (source & 7))) & 1) == 0) continue;
        level = 3;
      }
      const uint8_t nib = target.binary ? pixfmt::kInk : pixfmt::glyphLevelToNibble(level);
      paint4(target.buffer, destination, state ? nib : static_cast<uint8_t>(0x0F - nib));
    }
  }
}

// Paint a packed glyph into target.
//
// bitmap: rows are contiguous, MSB first, 1 or 2 bits per pixel; widths need
//   not be byte-aligned. 2bpp values are 0=white, 1=light gray, 2=dark gray,
//   3=black. 1bpp value 1 is ink.
// plane: which 2bpp values are painted. BW paints every non-white value and
//   honours state (true clears the bit, i.e. black). The gray planes only ever
//   set bits, because there 0 means leave alone and 1 means update: MSB paints
//   both grays, LSB paints dark gray only. 1bpp glyphs paint ink with state on
//   every plane.
// Clipping happens before pixel decoding, so fully hidden glyphs cost nothing.
inline void draw(const uint8_t* bitmap, int width, int height, bool twoBit, Plane plane, bool state,
                 const Target& target, Clip clip) {
  // 4 bpp canvas: there is only one surface, so `plane` has no meaning — every
  // coverage level lands in the same nibble. The grayscale bit-plane modes stay
  // on the 1 bpp path because their consumer (writeGrayscalePlaneStrip) wants
  // packed planes.
  if (target.fourBit) {
    draw4(bitmap, width, height, twoBit, state, target, clip);
    return;
  }

  // Bit n of levels set means source value n is painted.
  uint8_t levels = 0x02;
  bool clearBits = state;
  if (twoBit) {
    levels = plane == Plane::BW ? 0x0e : plane == Plane::GrayMSB ? 0x06 : 0x04;
    if (plane != Plane::BW) clearBits = false;
  }

  // Intersect with the glyph bounds, then with the panel width and the
  // buffered row band.
  clip.left = std::max(clip.left, 0);
  clip.top = std::max(clip.top, 0);
  clip.right = std::min(clip.right, width);
  clip.bottom = std::min(clip.bottom, height);
  clipToRect(target.frame, 0, target.originY, target.width, target.originY + target.rows, clip);
  if (clip.left >= clip.right || clip.top >= clip.bottom) return;

  // Walk the framebuffer as a flat bit index. One glyph column advances
  // stepX bits and one glyph row advances stepY bits; each is +/-1 for the
  // axis that maps to physical x, or +/-strideBits for physical y.
  const Frame& frame = target.frame;
  const int strideBits = target.stride * 8;
  const int stepX = frame.dxY * strideBits + frame.dxX;
  const int stepY = frame.dyY * strideBits + frame.dyX;
  int rowBit = (frame.y - target.originY) * strideBits + frame.x + clip.left * stepX + clip.top * stepY;
  for (int y = clip.top; y < clip.bottom; ++y, rowBit += stepY) {
    int source = y * width + clip.left;
    int destination = rowBit;
    int remaining = clip.right - clip.left;
    if (twoBit) {
      // Rows are bit-contiguous, not byte-padded. Align after any clipped prefix.
      while (remaining && (source & 3)) {
        paint(target.buffer, destination, (bitmap[source >> 2] >> (6 - (source & 3) * 2)) & 3, levels, clearBits);
        ++source;
        destination += stepX;
        --remaining;
      }
      while (remaining >= 4) {
        const uint8_t packed = bitmap[source >> 2];
        paint(target.buffer, destination, packed >> 6, levels, clearBits);
        paint(target.buffer, destination + stepX, (packed >> 4) & 3, levels, clearBits);
        paint(target.buffer, destination + 2 * stepX, (packed >> 2) & 3, levels, clearBits);
        paint(target.buffer, destination + 3 * stepX, packed & 3, levels, clearBits);
        source += 4;
        destination += 4 * stepX;
        remaining -= 4;
      }
    }
    // One-bit glyphs and the clipped tail use scalar decoding.
    while (remaining--) {
      const uint8_t ink = twoBit ? ((bitmap[source >> 2] >> (6 - (source & 3) * 2)) & 3)
                                 : ((bitmap[source >> 3] >> (7 - (source & 7))) & 1);
      paint(target.buffer, destination, ink, levels, clearBits);
      ++source;
      destination += stepX;
    }
  }
}

}  // namespace glyphBitmap
