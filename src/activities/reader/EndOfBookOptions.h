#pragma once

#include <atomic>
#include <string>
#include <vector>

#include "activities/eob/EndOfBookPanel.h"
#include "components/UiAppHost.h"

class GfxRenderer;
class MappedInputManager;

// Shared End-of-Book next-book menu for the EPUB and XTC readers. Collects up to
// MAX_SUGGESTIONS sibling books once per reader session, handles the menu input, and
// draws the end screen. With no suggestions the end screen keeps its historical
// plain-title look and behavior.
class EndOfBookOptions : private UiAppHost {
 public:
  enum class Action { None, Redraw, OpenBook, GoHome, LastPage };

  static constexpr size_t MAX_SUGGESTIONS = 3;

  explicit EndOfBookOptions(GfxRenderer& renderer);

  // Scans the book's folder for suggestions; no-op when already loaded. Call ONLY from
  // the reader's render() (the render task, serialized by RenderLock) — the loaded flag
  // is the release/acquire publication point that lets the main task read the finished
  // list safely.
  void loadOnce(const std::string& currentBookPath);

  // True when the suggestion menu is showing and should own the reader's input.
  bool menuActive() const;

  // Menu input handling, following the standard list idiom: a tap on a row opens it
  // (or Home), side Up/Down and front Left/Right move the selection (wrapping),
  // Confirm opens the selection, and a short Back press returns to the last page of
  // the book. Fills openPath when the result is OpenBook. Returns Action::None when
  // nothing relevant was pressed; callers continue their normal input path (keeping
  // long-press Back to the file browser working).
  Action handleMenuInput(const MappedInputManager& input, std::string* openPath);

  // Draws the full end screen (plain title, or the suggestion menu) onto the
  // renderer's canvas. EOB1: this is the ENTRY frame only -- the caller has
  // cleared the canvas and armed one from-white GC16 for it.
  void render(GfxRenderer& renderer, const MappedInputManager& input);

  // EOB1: repaints ONLY the rows whose state changed, into the retained canvas,
  // and commits the new selection. Returns false when nothing changed (the
  // caller then paints nothing and arms no pin). The chrome (title, "continue
  // with", hints) is never redrawn here: it is byte-identical to what the entry
  // frame left on the glass, which is what keeps the diff mask two row bands.
  bool renderSelection(GfxRenderer& renderer);

  // True while the canvas carries THIS panel at the CURRENT geometry: set by
  // render(), false before the entry frame, when the reader tears the options
  // down, or when the orientation/safe area changed under the retained canvas
  // (a rotation mid-EOB must repaint the whole panel, not two rows of a stale
  // layout).
  bool chromeValid() const;

  // True when the selection moved since the last paint, with the row window
  // [*first, *last] that changed. Read BEFORE any pin is armed, so a repaint
  // that paints nothing cannot leave a one-shot armed for the next paint.
  bool selectionChanged(int* first, int* last) const;

 private:
  // The UiAppHost base hosts the suggestion list (themed rows, touch routing);
  // the title and button hints stay on the legacy UITheme calls.
  static void listScreen(UiScreen& screen, void* user);
  static void onRowEvent(const freeink::ui::ActionEvent& event, void* user);
  void buildListScreen(UiScreen& screen);

  GfxRenderer& renderer;
  std::string folder;
  // Written by the render task in loadOnce(), immutable afterwards; the main task only
  // reads it after isLoaded is observed true (acquire), so no further locking is needed.
  std::vector<std::string> names;
  // Main-task selection updates may overlap a repaint on the render task.
  std::atomic<int> selector{0};
  std::atomic<bool> isLoaded{false};

  // Row storage, built once in loadOnce() (same acquire/release publication
  // point as names — see isLoaded above) rather than per-render in
  // buildListScreen(): names.size() is capped at MAX_SUGGESTIONS and never
  // changes afterward, so a fixed-capacity array avoids any heap allocation
  // for the row list, both at load time and every subsequent repaint.
  static constexpr size_t MAX_ROWS = MAX_SUGGESTIONS + 1;  // + the trailing "Home" row
  std::string rowLabels[MAX_ROWS];
  freeink::ui::ListItem rowItems[MAX_ROWS]{};
  size_t rowCount = 0;
  void buildRowItems();

  // EOB1: the panel's geometry, derived from the theme + fonts every paint (pure
  // arithmetic, no allocation) so a rotation mid-life cannot leave stale rects.
  eobpanel::Layout panelLayout(const GfxRenderer& renderer) const;
  // The props for rows [first, first + count): identical geometry to the entry
  // frame, so a two-row repaint lands in exactly the rects the full paint used.
  freeink::ui::ListProps rowProps(const GfxRenderer& renderer, const eobpanel::Layout& layout,
                                  const int first, const int count, void* windowCtx = nullptr) const;
  // Window rows are pulled one at a time: a whole-array list clamps its own
  // topIndex to `count - visible`, which would draw the wrong two rows.
  static void windowRowProvider(void* ctx, uint16_t index, freeink::ui::ListItem& item);
  void paintRowWindow(GfxRenderer& renderer, const eobpanel::Layout& layout, const int first, const int count);
  void drawChrome(GfxRenderer& renderer, const MappedInputManager& input, const eobpanel::Layout& layout);

  // The selection the canvas currently shows (render task only). Separate from
  // `selector`, which the loop task moves; that difference IS the dirty window.
  int renderedSelector = 0;
  // The geometry the canvas was painted with, and the flag the entry frame sets.
  // Both live on the render task (the reader paints under RenderLock).
  eobpanel::Layout paintedLayout{};
  bool chromePainted = false;

  // Row index dispatched by onRowEvent during the current route() call; -1 otherwise.
  int tappedRow = -1;

  std::string fullPath(size_t index) const;
};
