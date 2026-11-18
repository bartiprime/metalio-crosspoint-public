#pragma once
// HOST-ONLY SHIM for the token-table harness. The real header pulls
// <BoardConfig.h> (Arduino / ESP-IDF), which cannot be compiled on the host.
// scale() == 1.0 means the theme tables are read in DESIGN space, which is what
// a paper design review compares.
#include <cmath>
namespace uiDensity {
inline constexpr int kDesignCanvasWidth = 480;
inline constexpr int kDesignCanvasHeight = 800;
inline constexpr float kDesignCanvasPpi = 235.0f;
inline float scale() { return 1.0f; }
inline int px(int v) { return v; }
}  // namespace uiDensity
