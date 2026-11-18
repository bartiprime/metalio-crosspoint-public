#pragma once

#include <BoardConfig.h>

#include <atomic>

#include "activities/Activity.h"
#include "components/UiAppHost.h"
#include "util/ButtonNavigator.h"

// Base for activities hosting a single FreeInkUI list screen. UiAppHost owns
// the app-hosting protocol (render target, FreeInkApp, uiReady handshake);
// this base layers the list protocol on top: the touch-routing / swipe-scroll
// / button-navigation loop (swipes scroll the viewport without moving the
// selection; buttons move the selection and pull the viewport along via
// fui::ListNav), and the render skeleton (chrome, app, footer). Subclasses
// supply the data: item count, screen content, and what activating a row does.
//
// Screens that are not a single list (sliders, tab layouts, state machines)
// should NOT derive from this — they use UiAppHost directly.
class UiListActivity : public Activity, protected UiAppHost {
 public:
  void onEnter() override;
  void loop() override;
  // Answers Button::Back in loop(): the manager must not navigate for this screen.
  bool ownsBackKey() const override { return true; }
  void render(RenderLock&&) override;

 protected:
  // Base-owned row action; subclass-registered actions start at ACTION_USER.
  static constexpr freeink::ui::ActionId ACTION_ROW = 1;
  static constexpr freeink::ui::ActionId ACTION_SWIPE_DELETE = 2;
  static constexpr freeink::ui::ActionId ACTION_USER = 3;
  // Previous / next page button of a paged settings list. Allocated at the TOP of
  // the id range so it cannot collide with the ACTION_USER + n ids subclasses mint.
  static constexpr freeink::ui::ActionId ACTION_PAGE = 0xFD;

  UiListActivity(const char* name, GfxRenderer& renderer, MappedInputManager& mappedInput,
                 bool wantsTouchLongPress = false);

  // --- subclass contract -----------------------------------------------------
  // Current number of list rows (re-read every loop pass; may change).
  virtual int listCount() const = 0;
  // Build the screen: content margin, items, ListProps (call syncListViewport
  // right before screen.list). Runs on the render task via the base trampoline.
  virtual void buildScreen(UiScreen& screen) = 0;
  // Activate a row (touch tap or Confirm on the selection). Handlers that
  // leave this screen should call app.clearTapFlash() so a lingering flash
  // can't gray an unrelated element on the next render.
  virtual void activateIndex(int index) = 0;
  // Touch long-press on a row; only fires when the subclass opted in via the
  // wantsTouchLongPress constructor flag (rows must also carry InputLongPress).
  virtual void onRowLongPress(int index) {}
#if FREEINK_CAP_TOUCH
  virtual bool canSwipeDelete(int index) const { return false; }
  virtual void swipeDelete(int index) {}
  void configureSwipeDelete(UiScreen& screen, freeink::ui::ListProps& props);
  void closeSwipeDelete();
#endif
  // The selection/viewport state the loop, sync, and row dispatch operate on.
  // Default is the single `nav` member; UiTabListActivity redirects it to the
  // active tab's per-tab state.
  virtual freeink::ui::ListNav& activeNav() { return nav; }
  // Bounds-checked ACTION_ROW dispatch. Default: selection follows the tapped
  // row, then long-press/activate. UiTabListActivity remaps row -> ring.
  virtual void onRowAction(const freeink::ui::ActionEvent& event);
  // The button-navigation tail of loop(): press steps the selection, hold
  // jumps by page. UiTabListActivity replaces it with the ring walk.
  virtual void navigateButtons();
  // First hook in loop(); return true when the pass is consumed (popups, extra
  // buttons, gestures). Runs before the base button handling.
  virtual bool handleCustomInput() { return false; }
  // Back/Confirm handling; override wholesale for press/release or hold
  // variants. Return true when a button consumed the pass.
  virtual bool handleButtons();
  virtual void onBackButton() { finish(); }
  // Header band, drawn before the app renders. Default paints GUI.drawHeader
  // with headerTitle(); override either for custom chrome.
  virtual const char* headerTitle() const { return nullptr; }
  virtual void drawChrome();
  // Button hints, drawn after the app renders. Default: Back/Select/Up/Down.
  virtual void drawFooter();

  // --- helpers ---------------------------------------------------------------
  // Measure visibleRows for the screen band, apply follow-on-build, clamp the
  // viewport, and write selection/viewport into props. Call from buildScreen
  // right before screen.list(props).
  // SDK resolves fonts, content padding and touch minimum before measuring.
  // selectionOffset reserves leading ring entries such as the tab bar.
  void syncListViewport(UiScreen& screen, freeink::ui::ListProps& props, int selectionOffset = 0);
  // THE list-chrome opt-in (r74 settings tree, r77 every list). A theme may own
  // the header band height, the row label font, the row icon box and a
  // band-filling row pitch; all of it is applied inside syncListViewport so the
  // viewport is measured against the same props list() paints with.
  // ONE code path, two ways in: the settings tree overrides this to true (it was
  // the r74 mechanism), every other list screen -- file browser, chapter
  // selection, bookmarks, clippings, network mode, reader menu, library, catalog
  // -- reads the THEME's answer instead, so an opted-in theme dresses them all
  // and every other theme keeps the r72 geometry untouched.
  // Defined in the .cpp: this header must not pull UITheme.h in for a default body.
  virtual bool usesThemeListChrome() const;
  // Applies the theme's list chrome: label font (subject to the face actually
  // carrying every label's codepoints), row icon box, and a band-filling row
  // pitch. Called from syncListViewport when usesThemeListChrome() is true, i.e.
  // before the viewport is measured.
  void applyThemeListChrome(UiScreen& screen, freeink::ui::ListProps& props);
  // r99.9 (operator request D): THE shared "no truncated label" mechanism. Binds
  // the theme's row face into FONT_BODY at the largest rung of the theme's ladder
  // where every label in the supplied window fits its row on ONE line, so one
  // face (and one row height) covers the whole screen. Runs from
  // applyThemeListChrome(); a screen that materializes its row window AFTER the
  // viewport is measured (library, chapter lists) calls it again once the rows
  // exist, right before list().
  void fitThemeRowLabels(UiScreen& screen, freeink::ui::ListProps& props);
  // Draws a list WITH the row-face fit applied to the window that is actually
  // about to be painted. Every screen that materializes its row window after
  // syncListViewport() (library, chapter list, clippings) must call this instead
  // of screen.list(): the fit at sync time cannot measure rows that do not exist
  // yet, so a late-window screen that calls list() directly silently keeps the
  // theme's PREFERRED face -- which is exactly how the library and the TOC went
  // on truncating after the ladder landed, while the settings tree, whose rows
  // exist at sync time, did not. One wrapper, so the next windowed screen cannot
  // forget the step.
  void drawListWindow(UiScreen& screen, freeink::ui::ListProps& props);
  // Whether this screen's rows are dense content (file names) rather than menu
  // labels, and should take the theme's listRowDenseFontId() as their ceiling.
  virtual bool wantsDenseRowLabels() const { return false; }
  // Height of the band drawChrome() paints, and therefore of the content margin
  // every list screen must leave under it: the theme's titled band on a themed
  // list, else the classic metrics.headerHeight. Subclasses read THIS instead of
  // re-adding topPadding + headerHeight, or the band and their content drift
  // apart (r77's whole failure mode on the non-settings lists).
  int listHeaderBandHeight() const;
  // Whether the theme's row face can draw the labels this page will show. The
  // base scans props.items, or walks props.rowProvider for the rows around the
  // viewport (provider lists never materialize a full row array). A screen that
  // builds its row window AFTER syncListViewport -- the chapter and clipping
  // lists do, because the window depends on the final viewport -- overrides this
  // and scans its own rows: the guard runs BEFORE the viewport is measured, when
  // props.items is still empty.
  virtual bool themeRowLabelsFit(const GfxRenderer& renderer, int fontId, const freeink::ui::ListProps& props);
  // Whether this list gives up a footer band to the theme's page buttons. False
  // -- the default -- because the pager is a SETTINGS-tree affordance: a list
  // that opted into the same band and row face but owns its bottom furniture (a
  // path band, a readout, its own hints) keeps the classic viewport scroll.
  // Settings screens override this to true alongside usesThemeListChrome().
  virtual bool usesListFooterPager() const { return false; }
  // RMENU2: a list that pages even though the ACTIVE theme does not page its
  // settings tree. The reader menu is the case: it composes through the stock
  // theme (r100.8) and must still page, because its rows are now 1.5x the touch
  // minimum. reservePageTabs()'s theme gate exists so a non-paging theme cannot
  // hand the settings tree a pager; a screen that OWNS its pager says so here
  // instead of the theme having to lie about the settings tree. Default false,
  // so every existing screen keeps the gate exactly as it was.
  virtual bool listPagerOptIn() const { return false; }
  // RMENU2: minimum height, in panel pixels, of each page ARROW -- the screen's
  // own touch standard, or 0 (the default) for the band the theme reserved
  // (Andro's 1.4x rule, unchanged for every settings screen). A screen that
  // returns a taller number makes the arrows set the band, which is how the
  // reader menu's arrows meet the same 1.5x floor as its rows.
  virtual int listPagerMinArrowHeight(const UiScreen& screen) const {
    (void)screen;
    return 0;
  }
  // True when the pager is live on this screen: the theme pages settings lists
  // OR this screen opted in. ONE reader for reservePageTabs() and the swipe
  // branch in loop(), so an arrow and a gesture cannot disagree about whether
  // this screen pages at all.
  bool listPagerEnabled() const;
  // Reserves a footer band and lays out the previous / next page BUTTONS of a
  // settings list whose rows do not fit its band. Returns false when the list is a
  // single page (nothing reserved, nothing drawn). The band is taken from the
  // SCREEN's content rect through the same Screen the list is built into, so the
  // list rect and the button band cannot overlap; paintPageTabs() then paints the
  // buttons into the rect recorded here, and fui::button registers each button's
  // hit rect from the very rect it fills -- one geometry, no second tap recipe.
  bool reservePageTabs(UiScreen& screen, freeink::ui::ListProps& props);
  // True when the layout that just ran drew a different number of rows per page
  // than the pager painted its readout with -- i.e. the estimate the first build
  // had to use and the measurement it recorded disagree. render() then composes
  // the frame again before flipping it, so no page count ever reaches the glass
  // that the layout it was painted with contradicts.
  bool pagerStaleAfterLayout();
  // Paints both arrow buttons, their honest end-of-list disabled states and the
  // page readout (top-right, above the arrows), and registers each live button's
  // page target.
  void paintPageTabs(UiScreen& screen, const freeink::ui::ListProps& props);
  static void pageTabTrampoline(const freeink::ui::ActionEvent& event, void* user);
  // The page whose window contains `top`, tail page included. ONE derivation for
  // the painted readout and for the swipe / arrow page step, so the two inputs
  // cannot disagree about which page is showing.
  int pageIndexForTop(int top);
  // THE rows one page moves by -- the page size of the ROW SET, measured before
  // the paint (measureRowSetPageRows), so it cannot depend on the current page.
  // The painted readout, the arrow's target, requestPage() and the viewport
  // alignment all read this one number; a second estimate is how an arrow tap ends
  // up snapping back to the page it started on, and a per-page measurement is how
  // the readout ended up disagreeing with itself.
  int pagerPageRows();
  // How many pages the row set tiles into, and the first row index of a page. Both come
  // from the measured tiling (pageStarts_) when there is one and fall back to the uniform
  // divisor otherwise. U-4: the tiling is per page -- `page * rows` skipped a row whenever
  // a page drew fewer rows than page 0 did.
  int pagerPageCount();
  int pagerPageStart(int page);
  // The page size from the BAND GEOMETRY and the ROW PITCH ONLY: n rows cost
  // n*rowHeight + (n-1)*rowGap, so n = (band + rowGap) / (rowHeight + rowGap) --
  // the SDK's own listVisibleRows() expression, evaluated on the band the rows
  // really fill and on the pitch the list really paints with. It is the same
  // number on every page BY CONSTRUCTION: neither term mentions a row index, a
  // materialised window, a top or a count. 0 = cannot price (no band yet, or a
  // list that never set a pitch), which leaves pagerPageRows() on its backstop.
  void notePagerBand(const freeink::ui::Rect& band, const freeink::ui::ListProps& props);
  // What pagerPageRows() answered from, for the one diagnostic line: names the
  // source instead of leaving the next log to be inferred from the arithmetic.
  const char* pagerRowsSourceName() const;
  // One pass over every row of the list, priced exactly as freeink::ui::list()
  // prices it, filling the page-start map (pageStarts_) and returning the rows page 0
  // holds. 0 = could not measure (no rows, empty band, a row source that ran out), which
  // leaves every reader on the uniform divisor / the nav's fallback.
  int measureRowSetPageRows(UiScreen& screen, const freeink::ui::ListProps& props);
  // How many leading RING entries precede row 0 in the nav's selected index.
  // 0 for a plain list; a tab-ring list (UiTabListActivity) overrides this to 1.
  // requestPage() is given a page number and derives the first ROW of that page,
  // so it must convert to the nav's ring space with this offset -- writing a row
  // index into a ring index selected one row ABOVE the page and the build's page
  // alignment then snapped the viewport back, which read as "both arrows are dead".
  virtual int selectionRingOffset() const { return 0; }
  // Turn to page `index`: moves the selection to that page's first row (the
  // build's alignment re-opens the page holding the selection) and pins the
  // viewport top that build must draw.
  void requestPage(int index);
  // Move the selection to index and pull the viewport to it.
  void moveSelectionTo(int index);

  // SLIDE1: a row whose activation STEPS a value (SettingsRowKind Stepper) arms
  // this right after it writes the new value. The step's repaint then drives
  // ONE diff-masked DU over the changed rows, instead of the ordinary FAST
  // paint plus the row-9 GL16 completion -- a second whole-panel drive per step,
  // which is the whole-screen flash the operator saw on the Time to sleep row and
  // on any VALUE row. See activities/util/InPlaceValueStep.h.
  // Consumed by render() from below the unchanged-frame bail, so a paint that
  // is skipped can never leave the one-shots armed for someone else's drive.
  void armValueStepRepaint() { valueStepPaint_.store(true, std::memory_order_relaxed); }

  // --- shared state ----------------------------------------------------------
  // Selection + viewport (selected/top/visibleRows/followOnBuild). Access via
  // activeNav() in shared code; `nav` is the single-list default storage.
  freeink::ui::ListNav nav;
  ButtonNavigator buttonNavigator;

 private:
  static void screenTrampoline(UiScreen& screen, void* user);
  static void rowActionTrampoline(const freeink::ui::ActionEvent& event, void* user);
  // Named apart from UiAppHost::routeTouch so the host overload stays visible
  // (not name-hidden) to subclasses with extra touch surfaces.
  bool routeListTouch();

  // --- paged repaint bookkeeping (see render()) ------------------------------
  // Top of the page the panel is showing. -1 = nothing committed yet.
  int lastRenderedTop_ = -1;
  // Hash of the rows the last committed page painted (labels, values, toggles,
  // states of the window from that top).
  uint32_t lastRenderedSignature_ = 0;
  // Hash of the rows the most recent build laid out, and how many rows it
  // covered (0 = the build handed its items to list() after this point, so the
  // hash says nothing and no skip is allowed). Written by syncListViewport(),
  // which every list screen calls right before list().
  uint32_t buildSignature_ = 0;
  int buildSignatureRows_ = 0;
  // Builds since the last render. A paint whose list was never rebuilt cannot
  // prove its content is unchanged, so it must not skip the buffer flip.
  int buildsSinceRender_ = 0;
  // True until the first flip after onEnter(): the entry frame is always
  // committed, and always with the cleaning waveform.
  bool firstPagedRender_ = true;
  // Frames left that must be committed because a tap flash is armed or still on
  // the panel (see render()). 0 = nothing flash-related is pending.
  int flashCommitFrames_ = 0;
  // Bottom page buttons: the band reservePageTabs() took (empty = no buttons).
  freeink::ui::Rect pageTabBar_{};
  // What the pager PAINTED on the last build: the row count a page moves by, the
  // page count, and the list count it was derived from. The readout, the arrows
  // and the viewport all read pagerPageRows(), and before the first layout has
  // reported back that is the fixed-height ESTIMATE -- so the first composed
  // frame can print a page count the very next layout disagrees with ("1/3" on
  // entry, then "2/2" after one tap). render() compares these against the
  // measurement the layout just recorded and composes again when they differ,
  // so the panel only ever commits a frame whose readout matches its layout.
  int paintedPagerRows_ = -1;
  int paintedPagerPages_ = -1;
  int paintedPagerCount_ = -1;
  // The page size of the ROW SET (rows the list fits when the page starts at row
  // 0), measured from every row before the first paint, and the list count it was
  // measured for. This is the divisor the readout and the arrows share: it belongs
  // to the list, not to the page on screen, so paging cannot change it.
  //
  // D1: a measurement may only be REPLACED by one that priced the whole row set.
  // A windowed props (items + itemsWindowFirst/Count) cannot price the rows
  // outside its window, so on a page that does not start at row 0 the scan has no
  // page-0 fill to report and returns 0. Overwriting this with that 0 is what put
  // the per-page drawnRows back in charge of the divisor on the series drill's
  // page 2 -- 8 rows on that page against a 9-row page 0, i.e. ceil(17/8)=3 pages
  // where the list has two, and the viewport re-tiled forever.
  int pageRowsRowSet_ = -1;
  int pageRowsRowSetCount_ = -1;
  // True when the props of the build in flight could not price the whole row set
  // (a window that starts past row 0, or one that stops short of the end). Such a
  // build holds no whole-set measurement of its own, so pagerPageRows() must not
  // derive a divisor from the page on screen either -- see there.
  bool pagerRowsWindowed_ = false;
  // THE page size that cannot depend on the page (H2 round 2): the band the rows
  // fill and the pitch they are painted at, captured by notePagerBand() on every
  // build -- from syncListViewport (so the divisor exists before the first paint)
  // and again from paintPageTabs (the props list() actually lays out, and the band
  // the pager footer left). It is a pure function of two lengths, so a list that
  // materialises ONE page can never compute a different divisor on its page 2.
  //
  // Why this outranks every per-page number: on the series drill the props handed
  // to syncListViewport are built BEFORE the row window exists (props.items is
  // still null at that point), so the scan sees no row at all and the measurement
  // is 0 -- meas=-1 in the log -- on EVERY page. The old fallback was then
  // pageRowsFor()/drawnRows, i.e. "rows the last layout happened to draw": 9 on
  // page 1 and 8 on page 2 of a 17-book series, ceil(17/8)=3 pages where the list
  // has two. Band + pitch answers 9 on both pages without ever looking at a page.
  int pagerBandPx_ = 0;
  int pagerUniformRows_ = 0;
  // Where pagerPageRows() got its last answer: 1 = the row set's own measurement,
  // 2 = the uniform band+pitch divisor, 3 = the nav's fixed-height estimate (a
  // pre-sync backstop), 0 = nothing priceable. Logged as src=, never guessed at.
  int pagerRowsSrc_ = 0;
  // U-4: the first row index of every page of the row set, measured once per build
  // (measureRowSetPageRows). pagerPageRows() stays the page-0 count -- the divisor callers
  // that need one number still read -- but the TILING reads this map, so a page that draws
  // fewer rows than page 0 (a label that wrapped mid-list) cannot leave the rows past its
  // end on no page at all: the next page starts where this one stopped.
  // pageStartCount_ == 0 means "no map" (not measured yet, or the row source ran out
  // mid-scan); every reader then falls back to the uniform divisor, i.e. the old rule.
  static constexpr int kMaxPagerPages = 48;
  int16_t pageStarts_[kMaxPagerPages] = {0};
  int pageStartCount_ = 0;
  int pageStartRowsCount_ = -1;
  int pageStartPageRows_ = 0;
  // The row-face fit r99.9 (request D) resolved last: the labels it measured and
  // the width it measured them at are folded into the signature, so a page turn,
  // a language switch or a screen resize re-measures and anything else reuses the
  // answer. This is what keeps the fit off the compose path.
  uint32_t rowLabelFitSig_ = 0;
  int rowLabelFitFace_ = 0;
  // Page top an explicit swipe/page tab asked for (-1 = none), consumed by the
  // next build before the selection-follow and alignment rules run, so a
  // deliberate page turn is never snapped back to the selection's page.
  std::atomic<int> pendingPageTarget_{-1};
  // SLIDE1: a VALUE step is waiting to be painted (set by armValueStepRepaint()
  // from the loop task, cleared by render() on the paint that consumes it).
  std::atomic<bool> valueStepPaint_{false};
#if FREEINK_CAP_TOUCH
  static void swipeDeleteTrampoline(const freeink::ui::ActionEvent& event, void* user);
  bool handleSwipeDeleteInput();
  std::atomic<int16_t> revealedIndex{-1};
  freeink::ui::ListRevealAction swipeReveal{};
#endif

  const bool wantsTouchLongPress;
};
