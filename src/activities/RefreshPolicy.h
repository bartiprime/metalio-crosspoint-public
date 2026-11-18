#pragma once

#include <CrossPointSettings.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>

// One place that turns a SCREEN_REFRESH setting into a promotion on the renderer.
namespace refreshpolicy {

// Apply `setting` for the next paint, unless a call site already chose a mode for it (an explicit
// requestFullRefresh always wins).
inline void apply(GfxRenderer& renderer, const uint8_t setting) {
  if (renderer.hasPromotedRefresh()) return;
  switch (setting) {
    case CrossPointSettings::REFRESH_FAST:
      renderer.promoteNextRefresh(HalDisplay::FAST_REFRESH);
      break;
    case CrossPointSettings::REFRESH_HALF:
      renderer.promoteNextRefresh(HalDisplay::HALF_REFRESH);
      break;
    case CrossPointSettings::REFRESH_GL16:
      renderer.promoteNextRefresh(HalDisplay::GL16_REFRESH);
      break;
    case CrossPointSettings::REFRESH_FULL:
      renderer.promoteNextRefresh(HalDisplay::FULL_REFRESH);
      break;
    case CrossPointSettings::REFRESH_AUTO:
    default:
      renderer.promoteAutoRefresh();
      break;
  }
}

}  // namespace refreshpolicy
