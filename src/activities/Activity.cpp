#include "Activity.h"

#include "RefreshPolicy.h"

#include "ActivityManager.h"

void Activity::onEnter() { LOG_DBG("ACT", "Entering activity: %s", name.c_str()); }

void Activity::onExit() { LOG_DBG("ACT", "Exiting activity: %s", name.c_str()); }

void Activity::requestUpdate(bool immediate) {
  // In-activity repaints follow the "in-screen" setting; an explicit promotion by the call site wins.
  renderer.setAutoRefreshThreshold(SETTINGS.getAutoRefreshThresholdPercent());
  if (transitionRefreshPolicy_) refreshpolicy::apply(renderer, SETTINGS.menuTransitionRefresh);
  activityManager.requestUpdate(immediate);
}

bool Activity::requestUpdateAndWait() {
  // Same policy requestUpdate() applies. Without it this path silently bypassed the transition
  // setting and committed whatever the call site's bare displayBuffer() defaulted to.
  renderer.setAutoRefreshThreshold(SETTINGS.getAutoRefreshThresholdPercent());
  if (transitionRefreshPolicy_) refreshpolicy::apply(renderer, SETTINGS.menuTransitionRefresh);
  return activityManager.requestUpdateAndWait();
}

void Activity::requestFullRefresh() {
  renderer.promoteNextRefresh(HalDisplay::FULL_REFRESH);
  activityManager.requestUpdate();
}

void Activity::onGoHome(HomeMenuItem item) { activityManager.goHome(item); }

void Activity::onSelectBook(const std::string& path) { activityManager.goToReader(path); }

void Activity::startActivityForResult(std::unique_ptr<Activity>&& activity, ActivityResultHandler resultHandler) {
  this->resultHandler = std::move(resultHandler);
  activityManager.pushActivity(std::move(activity));
}

void Activity::setResult(ActivityResult&& result) { this->result = std::move(result); }

void Activity::finish() { activityManager.popActivity(); }
