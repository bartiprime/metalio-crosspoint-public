#pragma once
#include <I18n.h>

#include <algorithm>
#include <atomic>
#include <functional>
#include <string>
#include <vector>

#include "GfxRenderer.h"
#include "MappedInputManager.h"
#include "display/EqualizeBlack.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"
#include "audio/UiSounds.h"

// Modal option picker drawn over the current screen (no clear) via
// fui::optionDialog. Touch hit-testing is the SDK's InteractionBuffer: each
// render registers the option buttons (plus a chrome guard rect) on the render
// task, and handleInput routes touch snapshots against that table on the loop
// task, gated by the uiReady handshake (same pattern as UiListActivity).
// render() builds into InteractionBuffer's non-published generation
// (beginPublishCycle()) and publishes it only once every hit() call for the
// frame is done (publish()), so handleInput()'s routePublished()/
// publishedData() reads on the loop task always see a complete table, never
// one render is mid-rebuilding. uiReady closes when show() replaces the
// popup's data, then stays open across ordinary repaints after the first
// publication so a release cannot be dropped during a highlight repaint.
class OptionPopup {
 public:
  void show(StrId titleId, const StrId* optionIds, int optionCount, int currentIndex,
            std::function<void(int)> onSelect) {
    title = I18N.get(titleId);
    headline.clear();
    ownedStrings.resize(optionCount);
    for (int i = 0; i < optionCount; i++) {
      ownedStrings[i] = I18N.get(optionIds[i]);
    }
    activate(currentIndex, std::move(onSelect));
  }

  void show(const char* titleStr, const char* const* options, int optionCount, int currentIndex,
            std::function<void(int)> onSelect) {
    title = titleStr;
    headline.clear();
    ownedStrings.resize(optionCount);
    for (int i = 0; i < optionCount; i++) {
      ownedStrings[i] = options[i];
    }
    activate(currentIndex, std::move(onSelect));
  }

  // As above, plus a subject line inside the dialog (a book or event title).
  // It wraps to several lines under the caption; the dialog grows to fit.
  void show(const char* titleStr, const char* headlineStr, const char* const* options, int optionCount,
            int currentIndex, std::function<void(int)> onSelect) {
    show(titleStr, options, optionCount, currentIndex, std::move(onSelect));
    headline = headlineStr ? headlineStr : "";
  }

  // Message dialog: a wrapped body under the (optional) title, like the
  // Wi-Fi forget-network prompt. Pass an empty title for a message-only look.
  void showMessage(const char* titleStr, const char* messageStr, const char* const* options, int optionCount,
                   int currentIndex, std::function<void(int)> onSelect) {
    show(titleStr, options, optionCount, currentIndex, std::move(onSelect));
    message = messageStr ? messageStr : "";
  }

  void show(StrId titleId, const std::vector<std::string>& options, int currentIndex,
            std::function<void(int)> onSelect) {
    title = I18N.get(titleId);
    headline.clear();
    ownedStrings = options;
    activate(currentIndex, std::move(onSelect));
  }

  bool handleInput(MappedInputManager& input, const std::function<void()>& requestUpdate) {
    if (!active) return false;

    const freeink::ui::InputSnapshot snap = touchSnapshotFrom(input);
    // AUDIO3: dialogs were one of the silent surfaces. They speak the same five
    // events as every list -- a move ticks, an option takes, a cancel is the back
    // tone -- and never the refused tone: a dialog whose list is one option long
    // has nothing to refuse.
    if (snap.touchPressed || snap.touchReleased || snap.touchHeld) {
      // Interactions are registered on the render task; only route once the
      // first render after show() has populated the table (uiReady handshake).
      if (uiReady) {
        const freeink::ui::ActionEvent event = interactions.routePublished(snap);
        if (event && event.action == ACTION_OPTION) {
          // Tap released on an option: select it, fire, dismiss.
          selectedIndex = event.value;
          active = false;
          uisounds::onEvent(uisounds::Event::Select, uisounds::Tag::Dialog);
          if (onSelectCallback) onSelectCallback(selectedIndex);
          haptic_feedback::deferTouchAction();
          requestUpdate();
          return true;
        }
        if (event && event.action == ACTION_CHROME) {
          // Taps on the dialog chrome (title, padding) keep the popup open.
          return true;
        }
        if (snap.touchReleased && snap.touchX >= 0) {
          // Tap released outside the dialog: dismiss without firing. Swipe-end
          // releases arrive with -1,-1 coords and fall through (no dismiss).
          active = false;
          uisounds::onEvent(uisounds::Event::Back, uisounds::Tag::Dialog);
          haptic_feedback::deferTouchAction();
          requestUpdate();
          return true;
        }
        if (snap.touchPressed) {
          // Touch-down on an option moves the highlight (route() latched the
          // hit as the active interaction; read it back, no re-hit-testing).
          const int16_t idx = interactions.activeIndex();
          if (idx >= 0) {
            const freeink::ui::Interaction& hit = interactions.publishedData()[idx];
            if (hit.action == ACTION_OPTION && selectedIndex != hit.value) {
              selectedIndex = hit.value;
              uisounds::onEvent(uisounds::Event::MenuMove, uisounds::Tag::Dialog);
              requestUpdate();
            }
          }
        }
      }
      return true;
    }

    return handleButtons(input.wasPressed(MappedInputManager::Button::NavPrevious),
                         input.wasPressed(MappedInputManager::Button::NavNext),
                         input.wasReleased(MappedInputManager::Button::Confirm),
                         input.wasReleased(MappedInputManager::Button::Back), requestUpdate);
  }

  bool handleButtons(const bool previous, const bool next, const bool confirm, const bool back,
                     const std::function<void()>& requestUpdate) {
    if (!active) return false;
    const int count = std::min<int>(ownedStrings.size(), MAX_OPTIONS);
    if (previous) {
      selectedIndex = (selectedIndex - 1 + count) % count;
      uisounds::onEvent(uisounds::Event::MenuMove, uisounds::Tag::Dialog);
      requestUpdate();
      return true;
    } else if (next) {
      selectedIndex = (selectedIndex + 1) % count;
      uisounds::onEvent(uisounds::Event::MenuMove, uisounds::Tag::Dialog);
      requestUpdate();
      return true;
    } else if (confirm) {
      active = false;
      uisounds::onEvent(uisounds::Event::Select, uisounds::Tag::Dialog);
      if (onSelectCallback) onSelectCallback(selectedIndex);
      requestUpdate();
      return true;
    } else if (back) {
      active = false;
      uisounds::onEvent(uisounds::Event::Back, uisounds::Tag::Dialog);
      requestUpdate();
      return true;
    }
    return true;
  }

  bool processRender(GfxRenderer& renderer, const MappedInputManager& input) const {
    if (!active) return false;
    const auto popupLabels = input.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, popupLabels.btn1, popupLabels.btn2, popupLabels.btn3, popupLabels.btn4);
    render(renderer);
    // A popup commits on its own, and a bare displayBuffer() both used a diff-area mode and CONSUMED
    // any one-shot promotion armed for the frame underneath -- stealing the FULL that screen still
    // needs. Park the promotion, paint the box as a HALF (a box over existing content is an edit, not
    // a replacement), then re-arm it for whoever commits next. Same hazard, same remedy as
    // BaseTheme::drawPopup, which was fixed for exactly this and left this path behind.
    const auto parked = renderer.peekPromotedRefresh();
    renderer.cancelPromotedRefresh();
    // EQ1 (r101.9): Settings > Display > Equalize Black ON turns this popup's HALF into
    // EQUALIZE (same 48-frame GC16, LUT-only; see src/display/EqualizeBlack.h). OFF is HALF.
    renderer.displayBuffer(equalizeBlackMode(HalDisplay::HALF_REFRESH, "option-popup"));
    renderer.rearmPromotedRefresh(parked);
    return true;
  }

  void render(const GfxRenderer& renderer) const {
    if (!active) return;
    namespace fui = freeink::ui;

    // Per-render target: a GfxRendererTarget is a renderer reference plus
    // three font ids, so rebuilding it here is trivially cheap and always
    // tracks the live orientation and uiScale fonts; a target held across
    // show() would stale-bind both after a rotation or scale change.
    fui::GfxRendererTarget target = makeUiTarget(renderer);
    const fui::ThemeTokens& theme = refreshSharedUiThemeTokens(target);
    // Frame stores a const DeviceContext&; keep it in a local that outlives
    // the frame (a deviceContext() temporary would dangle).
    const fui::DeviceContext device = target.deviceContext();
    // Routing happens on the loop task against the member buffer; the frame
    // itself never dispatches, so it gets an empty snapshot.
    const fui::InputSnapshot noInput{};

    // Builds into the generation handleInput()'s routePublished()/
    // publishedData() aren't currently reading, so the loop task never sees
    // this table mid-rebuild — see publish() below and
    // InteractionBuffer::beginPublishCycle().
    interactions.beginPublishCycle();
    fui::Frame<INTERACTION_CAPACITY> frame(target, device, noInput, interactions);

    const auto& metrics = UITheme::getInstance().getMetrics();
    const int totalOptions = static_cast<int>(ownedStrings.size());
    const uint8_t count = static_cast<uint8_t>(totalOptions > MAX_OPTIONS ? MAX_OPTIONS : totalOptions);

    // Filled below, once the scroll window (how many rows fit the panel) is known.
    fui::DialogOption options[MAX_OPTIONS];

    fui::OptionDialogProps props;
    props.title = title.empty() ? nullptr : title.c_str();
    props.headline = headline.empty() ? nullptr : headline.c_str();
    if (!message.empty()) {
      props.message = message.c_str();
      props.messageText.font = fui::GfxRendererTarget::FONT_BODY;
      props.messageText.align = fui::TextAlign::Center;
      props.messageText.maxLines = 6;
    }
    props.options = options;  // filled below, after the scroll window is sized
    props.optionCount = 0;
    props.verticalOptions = true;
    // Touch only: physical buttons stay on the legacy wrap/confirm path above,
    // so the buffer never competes with it for focus/confirm dispatch.
    props.inputMask = fui::InputTouch;
    props.titleText.font = fui::GfxRendererTarget::FONT_BODY;
    props.titleText.bold = true;
    props.titleText.align = fui::TextAlign::Center;
    // Captions like "Remove from Recent Books?" overflow the narrow portrait
    // dialog in one line; let them wrap and the panel grow.
    props.titleText.maxLines = 2;
    props.headlineText.font = fui::GfxRendererTarget::FONT_BODY;
    props.headlineText.align = fui::TextAlign::Center;
    props.headlineText.maxLines = 3;
    props.buttonText.font = fui::GfxRendererTarget::FONT_BODY;
    const int16_t innerPadding = static_cast<int16_t>(metrics.optionPopupInnerPadding);
    props.padding = fui::Insets{innerPadding, innerPadding, innerPadding, innerPadding};
    props.gap = static_cast<int16_t>(metrics.optionPopupItemSpacing);
    // Rounded invert-fill themes use a black pill, not the default gray focus cursor.
    if (theme.listSelectionStyle == fui::SelectionStyle::InvertFill && theme.listRowRadius > 0) {
      props.buttonStyles = fui::defaultButtonStyles();
      props.buttonStyles.focused = props.buttonStyles.selected;
      fui::setStyleRadius(props.buttonStyles, theme.listRowRadius);
    }
    // defaultPopupStyles() has no border, so opt in using the per-theme frame metrics.
    props.styles = fui::defaultPopupStyles();
    props.styles.normal.border = fui::Paint::solid(fui::Color::Black);
    props.styles.normal.borderWidth = static_cast<uint8_t>(metrics.popupFrameThickness);
    props.styles.normal.radius = static_cast<uint8_t>(metrics.popupCornerRadius);
    // r78: a theme that states a dialog RULE weight marks the selection by
    // outline at that weight and paints no fill anywhere -- the affordance r70
    // removed from this theme's lists, and the only vocabulary Andro has. Both
    // the option outline and the panel frame then read at ONE weight, so the
    // dialog is rules and whitespace rather than a boxed card with a grey block
    // moving around inside it. Every other theme returns 0 and falls through to
    // exactly the styles it had (popupFrameThickness frame, dithered focus).
    const int dialogRule = UITheme::getInstance().getTheme().dialogRuleThickness();
    if (dialogRule > 0) {
      // The PANEL keeps its opaque surface: this dialog is drawn over live
      // content with no clear, so a transparent card would let the reader's text
      // through it. Only the SELECTION loses its fill. The frame is drawn at the
      // theme's rule weight rather than popupFrameThickness's 2 px hairline, so
      // the card, the selected row and every pager button in this theme read as
      // one weight of ink.
      props.styles.normal.borderWidth = static_cast<uint8_t>(dialogRule);
      fui::StyleSet optionStyles;
      optionStyles.explicitlySet = true;
      optionStyles.normal.background = fui::Paint::none();
      optionStyles.normal.foreground = fui::Paint::solid(fui::Color::Black);
      // The selected option is an OUTLINE at the theme's rule weight; the
      // unselected ones carry no box at all, so the marker is one rectangle on
      // a page of whitespace.
      optionStyles.focused.background = fui::Paint::none();
      optionStyles.focused.foreground = fui::Paint::solid(fui::Color::Black);
      optionStyles.focused.border = fui::Paint::solid(fui::Color::Black);
      optionStyles.focused.borderWidth = static_cast<uint8_t>(dialogRule);
      optionStyles.selected = optionStyles.focused;
      optionStyles.active = optionStyles.focused;
      optionStyles.disabled.background = fui::Paint::none();
      optionStyles.disabled.foreground = fui::Paint::dither(fui::Color::LightGray);
      props.buttonStyles = optionStyles;
    }
    props.styles.selected = props.styles.normal;
    props.styles.focused = props.styles.normal;
    props.styles.active = props.styles.normal;
    props.styles.disabled = props.styles.normal;
    props.buttonHeight =
        fui::clampI16(target.lineHeight(fui::GfxRendererTarget::FONT_BODY) + metrics.optionPopupSelectionVPadding * 2);

    // Fixed fraction of the screen, clamped by the theme's side margins; the
    // old max-text-width sizing is gone, long labels wrap inside the buttons.
    const fui::Rect screen = device.screen();
    const int16_t width =
        fui::clampI16(std::min<int>(screen.width * 3 / 4, screen.width - metrics.optionPopupDialogSideMargin * 2));

    // Scroll window. The dialog lays every option out downwards and has no
    // clipping of its own, so a list taller than the panel used to run off the
    // glass with its lower rows unreachable. Measure the full list; when it does
    // not fit, draw whole rows only and keep the selection inside the window.
    // Each option carries its REAL index as its action value, so touch routing
    // and the button wrap around `count` behave exactly as before.
    int visible = count;
    if (count > 0) {
      // No vertical margin metric exists for this dialog; reuse the side margin
      // so the panel keeps an even border top and bottom.
      const int16_t maxHeight = fui::clampI16(screen.height - metrics.optionPopupDialogSideMargin * 2, 0, screen.height);
      props.optionCount = count;
      if (fui::optionDialogHeight(target, props, width) > maxHeight) {
        props.optionCount = 0;
        const int16_t chromeHeight = fui::optionDialogHeight(target, props, width);
        const int rowPitch = static_cast<int>(props.buttonHeight) + props.gap;
        const int available = static_cast<int>(maxHeight) - chromeHeight - props.gap;
        visible = (rowPitch > 0 && available > 0) ? std::max(1, (available + props.gap) / rowPitch) : 1;
        visible = std::min<int>(visible, count);
      }
    }
    ensureVisible(count, visible);

    for (int i = 0; i < visible; ++i) {
      const int optionIndex = scrollOffset + i;
      options[i].label = ownedStrings[optionIndex].c_str();
      options[i].action = ACTION_OPTION;
      options[i].value = static_cast<int16_t>(optionIndex);
      options[i].state = (optionIndex == selectedIndex) ? fui::StateFocused : fui::StateNormal;
    }
    props.optionCount = static_cast<uint8_t>(visible);

    const int16_t height = fui::clampI16(fui::optionDialogHeight(target, props, width), 0, screen.height);
    const fui::Rect dialogRect = fui::centeredRect(screen, fui::Size{width, height});

    // Chrome guard first, options after: route() scans newest-first, so the
    // option buttons win inside the dialog and the guard absorbs the rest.
    frame.hit(dialogRect, ACTION_CHROME, 0, fui::InputTouch);
    fui::optionDialog(frame, dialogRect, props);
    // Atomically make this generation the one handleInput() reads, now that
    // every hit() call for this frame is done.
    interactions.publish();
    uiReady = true;
  }

  bool isActive() const { return active; }

  // Close without firing the callback (the surface under the popup is going
  // away, e.g. its host screen closes from outside the popup's own input).
  void dismiss() {
    active = false;
    onSelectCallback = nullptr;
  }

 private:
  // Long lists scroll by a window (see render()), so the cap is no longer about
  // what fits the panel: it bounds the stack DialogOption array and the
  // interaction table, and show() truncates anything longer. +1 slot for the
  // chrome guard rect.
  static constexpr int MAX_OPTIONS = 16;
  static constexpr size_t INTERACTION_CAPACITY = MAX_OPTIONS + 1;
  static constexpr freeink::ui::ActionId ACTION_OPTION = 1;
  static constexpr freeink::ui::ActionId ACTION_CHROME = 2;

  void activate(int currentIndex, std::function<void(int)> onSelect) {
    const int count = std::min<int>(ownedStrings.size(), MAX_OPTIONS);
    selectedIndex = currentIndex >= 0 && currentIndex < count ? currentIndex : 0;
    scrollOffset = 0;
    onSelectCallback = std::move(onSelect);
    message.clear();
    uiReady = false;
    active = count > 0;
  }

  // Moves the drawn window so the selected row is on screen. Computed during
  // render (where the panel height is known) rather than at input time, so a
  // selection move needs no second consensus about how many rows fit.
  void ensureVisible(const int count, const int visible) const {
    if (count <= 0 || visible <= 0) {
      scrollOffset = 0;
      return;
    }
    if (selectedIndex < scrollOffset) scrollOffset = selectedIndex;
    if (selectedIndex >= scrollOffset + visible) scrollOffset = selectedIndex - visible + 1;
    const int maxOffset = std::max(0, count - visible);
    scrollOffset = std::clamp(scrollOffset, 0, maxOffset);
  }

  bool active = false;
  std::string title;
  std::string headline;
  std::string message;
  std::vector<std::string> ownedStrings;
  int selectedIndex = 0;
  // First option drawn; render() keeps the selection inside the window.
  mutable int scrollOffset = 0;
  std::function<void(int)> onSelectCallback;
  // Written by the render task (frame registration), routed by the loop task;
  // uiReady closes the rebuild window exactly like UiListActivity::uiReady.
  mutable freeink::ui::InteractionBuffer<INTERACTION_CAPACITY> interactions;
  mutable std::atomic<bool> uiReady{false};
};
