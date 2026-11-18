#include "ButtonRemapActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/UiAppHelpers.h"
#include "components/UITheme.h"

namespace fui = freeink::ui;

namespace {
// UI steps correspond to logical roles in order: Back, Confirm, Left, Right.
constexpr uint8_t kRoleCount = 4;
// Marker used when a role has not been assigned yet.
constexpr uint8_t kUnassigned = 0xFF;
// Duration to show temporary error text when reassigning a button.
constexpr unsigned long kErrorDisplayMs = 1500;
}  // namespace

void ButtonRemapActivity::onEnter() {
  Activity::onEnter();

  // Start with all roles unassigned to avoid duplicate blocking.
  currentStep = 0;
  tempMapping[0] = kUnassigned;
  tempMapping[1] = kUnassigned;
  tempMapping[2] = kUnassigned;
  tempMapping[3] = kUnassigned;
  errorMessage.clear();
  errorUntil = 0;
  for (uint8_t i = 0; i < kRoleCount; ++i) {
    rowItems[i].label = getRoleName(i);
  }
  resetUi();
  app.setScreen(&ButtonRemapActivity::screenTrampoline, this);
  requestUpdate();
}

void ButtonRemapActivity::loop() {
  // Back leaves without touching the mapping. It HAS to be taken here: this screen captures every raw
  // front-button press as a mapping target (getPressedFrontButton() includes BTN_BACK), so letting the
  // ActivityManager's global Back tap act would assign Back to a role on the press and then leave on
  // the release. Same effect as Down below -- exit, persist nothing. Answered on the RELEASE, so a
  // HOLD still reaches Home through ActivityManager (a press-down answer would leave first).
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  // Clear any temporary warning after its timeout.
  if (errorUntil > 0 && millis() > errorUntil) {
    errorMessage.clear();
    errorUntil = 0;
    requestUpdate();
    return;
  }

  // Side buttons:
  // - Up: reset mapping to defaults and exit.
  // - Down: cancel without saving.
  if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
    // Persist default mapping immediately so the user can recover quickly.
    SETTINGS.frontButtonBack = CrossPointSettings::FRONT_HW_BACK;
    SETTINGS.frontButtonConfirm = CrossPointSettings::FRONT_HW_CONFIRM;
    SETTINGS.frontButtonLeft = CrossPointSettings::FRONT_HW_LEFT;
    SETTINGS.frontButtonRight = CrossPointSettings::FRONT_HW_RIGHT;
    SETTINGS.saveToFile();
    finish();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
    // Exit without changing settings.
    finish();
    return;
  }

  {
    // Make sure UI done rendering before accepting another assignment.
    // This avoids rapid double-presses that can advance the step without a visible redraw.
    RenderLock lock(*this);

    // Wait for a front button press to assign to the current role.
    const int pressedButton = mappedInput.getPressedFrontButton();
    if (pressedButton < 0) {
      return;
    }

    // Update temporary mapping and advance the remap step.
    // Only accept the press if this hardware button isn't already assigned elsewhere.
    if (!validateUnassigned(static_cast<uint8_t>(pressedButton))) {
      requestUpdate();
      return;
    }
    tempMapping[currentStep] = static_cast<uint8_t>(pressedButton);
    currentStep++;

    if (currentStep >= kRoleCount) {
      // All roles assigned; save to settings and exit.
      applyTempMapping();
      SETTINGS.saveToFile();
      finish();
      return;
    }

    requestUpdate();
  }
}

void ButtonRemapActivity::render(RenderLock&&) {
  const auto labelForHardware = [&](uint8_t hardwareIndex) -> const char* {
    for (uint8_t i = 0; i < kRoleCount; i++) {
      if (tempMapping[i] == hardwareIndex) {
        return getRoleName(i);
      }
    }
    return "-";
  };

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, UITheme::getInstance().getTitledHeaderHeight()}, tr(STR_REMAP_FRONT_BUTTONS));
  GUI.drawSubHeader(renderer, Rect{0, metrics.topPadding + UITheme::getInstance().getTitledHeaderHeight(), pageWidth, metrics.tabBarHeight},
                    tr(STR_REMAP_PROMPT));

  int topOffset = metrics.topPadding + UITheme::getInstance().getTitledHeaderHeight() + metrics.tabBarHeight + metrics.verticalSpacing;
  renderUi();

  // Temporary warning banner for duplicates.
  if (!errorMessage.empty()) {
    GUI.drawHelpText(renderer,
                     Rect{0, pageHeight - metrics.buttonHintsHeight - metrics.contentSidePadding - 15, pageWidth, 20},
                     errorMessage.c_str());
  }

  // Provide side button actions at the bottom of the screen (split across two lines).
  // The pitch comes from buildScreen() (the theme's row measure when it owns one,
  // metrics.listRowHeight otherwise) so the hints sit under the LAST row, not
  // inside a row that the theme made taller.
  const int hintTop = topOffset + static_cast<int>(kRoleCount) * rowPitch_;
  GUI.drawHelpText(renderer,
                   Rect{0, hintTop + 4 * metrics.verticalSpacing, pageWidth, 20},
                   tr(STR_REMAP_RESET_HINT));
  GUI.drawHelpText(renderer,
                   Rect{0, hintTop + 5 * metrics.verticalSpacing + 20, pageWidth, 20},
                   tr(STR_REMAP_CANCEL_HINT));

  // Live preview of logical labels under front buttons.
  // This mirrors the on-device front button order: Back, Confirm, Left, Right.
  GUI.drawButtonHints(renderer, labelForHardware(CrossPointSettings::FRONT_HW_BACK),
                      labelForHardware(CrossPointSettings::FRONT_HW_CONFIRM),
                      labelForHardware(CrossPointSettings::FRONT_HW_LEFT),
                      labelForHardware(CrossPointSettings::FRONT_HW_RIGHT));
  renderer.displayBuffer();
}

void ButtonRemapActivity::screenTrampoline(UiScreen& screen, void* user) {
  static_cast<ButtonRemapActivity*>(user)->buildScreen(screen);
}

void ButtonRemapActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const int topOffset = metrics.topPadding + UITheme::getInstance().getTitledHeaderHeight() + metrics.tabBarHeight + metrics.verticalSpacing;
  screen.setContentMarginFromScreen(
      fui::Insets{static_cast<int16_t>(safe.y + topOffset),
                  static_cast<int16_t>(renderer.getScreenWidth() - (safe.x + safe.width) + metrics.verticalSpacing),
                  static_cast<int16_t>(renderer.getScreenHeight() - (safe.y + safe.height) + metrics.verticalSpacing),
                  static_cast<int16_t>(safe.x + metrics.verticalSpacing)});

  for (uint8_t i = 0; i < kRoleCount; ++i) {
    const uint8_t assignedButton = tempMapping[i];
    rowItems[i].value = assignedButton == kUnassigned ? tr(STR_UNASSIGNED) : getHardwareName(assignedButton);
  }

  fui::ListProps props;
  props.items = rowItems;
  props.count = kRoleCount;
  props.selectedIndex = currentStep;
  props.inputMask = fui::InputNone;
  props.scrollIndicator = false;
  // Label at the value's font size: both sides of the row read as one unit.
  // maxLines=2 also marks the style caller-owned (see textStyleUnset).
  props.labelText = screen.theme().smallText;
  props.labelText.maxLines = 2;

  // The Andro list shape on a hand-built list (r80). This screen IS a settings
  // sub-menu -- Settings > Controls > Button remap -- but it hosts its own list
  // instead of deriving from UiListActivity, so it never reaches
  // applyThemeListChrome() and drew its four rows in the 10 pt UI face
  // (smallText -> FONT_SMALL, a slot no theme rebinds) while every page above it
  // used the theme's row face. Same three theme knobs, asked through the same
  // getters, and the labelled band is already the theme's titled one (see
  // render()); themes that state no row font (listRowFontId() == 0, i.e. every
  // theme but Andro) fall through to exactly the props this screen built before.
  const BaseTheme& theme = UITheme::getInstance().getTheme();
  const int rowFontId = theme.listRowFontId();
  rowPitch_ = metrics.listRowHeight;
  if (theme.themeListChrome() && rowFontId != 0) {
    // The same coverage guard the themed lists run, from the same rule: a label
    // in a script the Ember faces do not carry keeps the UI face rather than
    // printing holes.
    bool fits = true;
    for (uint8_t i = 0; i < kRoleCount && fits; ++i) {
      fits = uiLabelFitsFace(renderer, rowFontId, rowItems[i].label);
    }
    props.labelText.font = fui::GfxRendererTarget::FONT_BODY;
    uiTarget.setFont(fui::GfxRendererTarget::FONT_BODY, fits ? rowFontId : uiScaleSpec().bodyFontId);
    // The row measure is the theme's pitch, and render() reads the SAME member
    // for the two hint lines under the list -- the old fixed
    // kRoleCount x metrics.listRowHeight put them inside a themed row as soon as
    // the rows grew.
    const int pitch = theme.listRowFillPitch(screen.body().height, static_cast<int>(kRoleCount));
    if (pitch > 0) {
      props.rowHeight = static_cast<int16_t>(pitch);
      props.rowGap = 0;
      rowPitch_ = pitch;
    }
  }
  screen.list(props);
}

void ButtonRemapActivity::applyTempMapping() {
  // Commit temporary mapping into settings (logical role -> hardware).
  SETTINGS.frontButtonBack = tempMapping[0];
  SETTINGS.frontButtonConfirm = tempMapping[1];
  SETTINGS.frontButtonLeft = tempMapping[2];
  SETTINGS.frontButtonRight = tempMapping[3];
}

bool ButtonRemapActivity::validateUnassigned(const uint8_t pressedButton) {
  // Block reusing a hardware button already assigned to another role.
  for (uint8_t i = 0; i < kRoleCount; i++) {
    if (tempMapping[i] == pressedButton && i != currentStep) {
      errorMessage = tr(STR_ALREADY_ASSIGNED);
      errorUntil = millis() + kErrorDisplayMs;
      return false;
    }
  }
  return true;
}

const char* ButtonRemapActivity::getRoleName(const uint8_t roleIndex) {
  switch (roleIndex) {
    case 0:
      return tr(STR_BACK);
    case 1:
      return tr(STR_CONFIRM);
    case 2:
      return tr(STR_DIR_LEFT);
    case 3:
    default:
      return tr(STR_DIR_RIGHT);
  }
}

const char* ButtonRemapActivity::getHardwareName(const uint8_t buttonIndex) const {
  switch (buttonIndex) {
    case CrossPointSettings::FRONT_HW_BACK:
      return tr(STR_HW_BACK_LABEL);
    case CrossPointSettings::FRONT_HW_CONFIRM:
      return tr(STR_HW_CONFIRM_LABEL);
    case CrossPointSettings::FRONT_HW_LEFT:
      return tr(STR_HW_LEFT_LABEL);
    case CrossPointSettings::FRONT_HW_RIGHT:
      return tr(STR_HW_RIGHT_LABEL);
    default:
      return "Unknown";
  }
}
