#pragma once
#include <Logging.h>

#include <cassert>
#include <memory>
#include <string>
#include <utility>

#include "ActivityManager.h"  // for using the ActivityManager singleton
#include "ActivityResult.h"
#include "GfxRenderer.h"
#include "MappedInputManager.h"
#include "RenderLock.h"
#include "util/ScreenshotInfo.h"

class Activity {
  friend class ActivityManager;

 protected:
  std::string name;
  // Activities that own a carefully tuned refresh cadence (the readers: DU page turns plus a
  // periodic clean) opt out, so the global in-screen setting cannot silently rewrite their timings.
  bool transitionRefreshPolicy_ = true;
  GfxRenderer& renderer;
  MappedInputManager& mappedInput;

  ActivityResultHandler resultHandler;
  ActivityResult result;

 public:
  explicit Activity(std::string name, GfxRenderer& renderer, MappedInputManager& mappedInput)
      : name(std::move(name)), renderer(renderer), mappedInput(mappedInput) {}
  virtual ~Activity() = default;
  virtual void onEnter();
  virtual void onExit();
  // Last chance to queue activity-owned state before sleep events are drained.
  virtual void prepareForSleep() {}
  virtual void loop() {}

  virtual void render(RenderLock&&) {}
  // r100.5: set by render() when it deliberately drew NOTHING and therefore drove nothing -- the
  // paint request is consumed but no frame reaches the glass. The manager clears it before every
  // paint and refuses to record `paintedVisit` for a paint that set it, because a paint that
  // declines is exactly the shape that made "the dialog is entered but never painted" invisible:
  // the ledger said a frame had arrived, so neither the entry paint debt nor its backstop ever
  // fired again. A declining render must also ask for the paint it still owes (requestUpdate()) --
  // the flag only makes the miss observable and repeatable, it does not fix it by itself.
  bool paintDeclinedFrame = false;
  // r162: the NEXT committed paint of this screen asks for HALF (GC16 over the diff) instead of the
  // FAST lane. Armed by ActivityManager when a pop restores this screen, consumed and cleared by the
  // paint that actually drives. It exists because on a pop onEnter() is NOT re-run
  // (ActivityManager.cpp, the restored-parent branch), so a UiListActivity's own `firstRender` and
  // `pageChanged` tests both read false with the top row unchanged and the repaint would take FAST --
  // which on a grey frame routes into the wipe composite and its from-paper erase prefix: the
  // back-navigation white flash. One shot, so every in-page FAST repaint (tap flash, scroll) keeps
  // its cheap DU drive. Read only by UiListActivity::render(); other screens never consume it.
  bool halfPaintOwed = false;
  void armHalfPaintOwed() { halfPaintOwed = true; }
  bool consumeHalfPaintOwed() {
    const bool owed = halfPaintOwed;
    halfPaintOwed = false;
    return owed;
  }
  // Return true to keep a long press on Back for yourself (the reader opens the file browser with it);
  // false lets the activity manager take it as "go Home from anywhere below Home".
  virtual bool handleLongPressBack() { return false; }

  // Back-key ownership, and the reason a screen has to think about it at all: the manager turns a TAP
  // on Back into one level of navigation for every screen that does NOT declare this -- pop, or Home at
  // the bottom of the stack. That is the global rule, and a new screen inherits it by existing.
  //
  // Return true when this activity reads MappedInputManager::Button::Back in its own loop() because
  // "one level" here means something else: the file browser going up a folder, a transfer being
  // cancelled, a popup dismissed with its cancel result, the reader saving its position on the way out.
  // While this is true the manager never acts on a Back tap here, so there is nothing to double-fire.
  // It does NOT cover the long-press-to-Home gesture: that stays global and is answered before this
  // activity's loop() runs (opt out per gesture with handleLongPressBack()).
  //
  // WHICH EDGE TO ANSWER, and it is not cosmetic: answer the RELEASE. The manager's hold needs the
  // finger down for kLongPressBackHomeMs, and it can only see that if this screen has not navigated
  // yet -- a Back answered on press-down leaves before 1200 ms can ever elapse, so holding Back on
  // that screen never reaches Home. Every screen whose Back means "leave" (the settings leaves, the
  // state screens' dismiss, the wifi picker, the reader) therefore reads wasReleased(). A screen that
  // must interrupt something AT the press -- an armed USB export, a running download, a flashing
  // update, a library walk holding the journal -- stays press-edged deliberately, and trades the hold
  // for a guaranteed cancel; those are the documented exceptions, not a pattern to copy.
  virtual bool ownsBackKey() const { return false; }

  // If immediate is true, the update will be triggered immediately.
  // Otherwise, it will be deferred until the end of the current loop iteration.
  virtual void requestUpdate(bool immediate = false);

  // Request an immediate render and block until it completes. Returns true when
  // a driven paint of this screen actually reached the glass (A-P1-6):
  // REPORTED ONLY -- no caller may refuse USB detach or the power-off park on it,
  // because a merely slow paint must never block them. See
  // ActivityManager::requestUpdateAndWait().
  virtual bool requestUpdateAndWait();

  // Repaint now with a full whole-panel (GC16 from white) drive. This is the ONLY correct choice when
  // the next frame *replaces* the screen rather than editing it: a wholesale screen/state change must
  // be FULL (whole-panel drive); HALF is diff-based on this panel and will ghost. DU also drives only
  // 30 of 256 transitions and never scrubs accumulated grey, so either fast mode lands on top of the
  // previous screen instead of replacing it.
  void requestFullRefresh();

  virtual bool skipLoopDelay() { return false; }
  virtual bool preventAutoSleep() { return false; }
  // Exclusive storage activities suspend global controls and normal activity
  // transitions so no filesystem code races a raw SD-card owner.
  virtual bool requiresExclusiveStorageLoop() const { return false; }
  virtual bool isReaderActivity() const { return false; }
  virtual bool acceptsRemoteInput() const { return false; }
  // Returns true when the activity schedules its own forced refresh.
  virtual bool handleForcedRefresh() { return false; }
  virtual bool isHomeActivity() const { return false; }
  virtual bool handleHomeGesture() { return false; }
  virtual ScreenshotInfo getScreenshotInfo() const { return {}; }

  // Start a new activity without destroying the current one
  // Note: requestUpdate() will be invoked automatically once resultHandler finishes
  void startActivityForResult(std::unique_ptr<Activity>&& activity, ActivityResultHandler resultHandler);

  // Set the result to be passed back to the previous activity when this activity finishes
  void setResult(ActivityResult&& result);

  // Finish this activity and return to the previous one on the stack (if any)
  static void finish();

  // Short forwarders to ActivityManager for the two navigations every screen
  // performs. They are the stable facade, not a transition shim: 22 call sites
  // use them (onGoHome also carries the HomeMenuItem), and each one would
  // otherwise repeat the manager call. Kept deliberately.
  static void onGoHome(HomeMenuItem item = HomeMenuItem::NONE);
  static void onSelectBook(const std::string& path);
};
