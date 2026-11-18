#pragma once
#include <string>

#include "activities/Activity.h"

class Bitmap;
class HalFile;

class SleepActivity final : public Activity {
 public:
  explicit SleepActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, bool fromTimeout = false)
      : Activity("Sleep", renderer, mappedInput), fromTimeout(fromTimeout) {}
  void onEnter() override;
  // The sleep screen is woken by main's own loop, never by a navigation: a Back press here must do
  // nothing, exactly as before the global rule existed.
  bool ownsBackKey() const override { return true; }

 private:
  void renderDefaultSleepScreen() const;
  void renderCustomSleepScreen() const;
  void renderCoverSleepScreen() const;
  void renderBitmapSleepScreen(const Bitmap& bitmap, bool preserveBackground = false) const;
  bool renderSleepOverlayFile(HalFile& file, const char* pathForLog) const;
  bool renderTransparentOverlayPng(const std::string& path) const;
  bool renderSleepOverlayPath(const std::string& path) const;
  void renderLastScreenSleepScreen() const;
  void renderTransparentCustomSleepScreen() const;
  void renderBlankSleepScreen() const;
  // r97 C2: the transparent custom overlay paints onto a cleared (WHITE) frame
  // and is driven ONCE from white -- never composited over the page that was on
  // the glass. Logs the mode it used so the morning capture can confirm which
  // drive the overlay actually got.
  void driveSleepOverlayFromWhite() const;

  bool fromTimeout = false;
  // True while the paint on the glass is a custom overlay that must own its
  // drive (set for TRANSPARENT_CUSTOM around renderTransparentCustomSleepScreen,
  // cleared after). Everything else -- cover art, CUSTOM wallpapers, the default
  // screen -- keeps its existing one-drive rules untouched.
  mutable bool overlayOwnDrive = false;
  // Cover + Transparent: the cover under the overlay, and the same cover while its gray planes are redrawn.
  mutable const Bitmap* overlayCover = nullptr;
  mutable const Bitmap* overlayGrayCover = nullptr;
};
