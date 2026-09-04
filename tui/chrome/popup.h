// Adapted from Fresh (crates/fresh-editor/src/view/popup.rs, v0.4.10), under
// the Nextpad++ provision in Fresh's LICENSE ("Permission explicitly granted
// to Nextpad++ to read and adapt the Fresh notepad code to work with
// Nextpad++"). Original copyright (c) Noam Lewis.
//
// The popup/floating-window primitive: position strategies with edge
// clamping (dock-offset-aware — clamps against the area's absolute edges),
// content-height-driven sizing, list selection with follow-scroll,
// view-only wheel scrolling, text selection, and rendering (border, title,
// close button, list styling, scrollbar) onto an abstract CellSurface.
//
// Deliberately not ported yet (no consumer): Markdown content (arrives with
// the T3.1 md4c cards), description text, the resolver enum (our popups
// resolve through the caller's modal loop), the unfocused-popup focus hints
// (our popups are modal and always focused), OSC 8 link overlays, and the
// PopupManager z-order stack (the modal loop owns one popup at a time).

#ifndef NPP_TUI_CHROME_POPUP_H
#define NPP_TUI_CHROME_POPUP_H

#include <cstddef>
#include <string>
#include <vector>

#include "cellsurface.h"
#include "geometry.h"
#include "scrollbar.h"

namespace npptui {

enum class PopupKind : uint8_t { Completion, Hover, Action, List, Text };

enum class PopupPositionKind : uint8_t {
  AtCursor, BelowCursor, AboveCursor, Fixed, Centered, CenteredOverlay,
  BottomRight, AboveStatusBarAt,
};

struct PopupPosition {
  PopupPositionKind kind = PopupPositionKind::AtCursor;
  int x = 0, y = 0;              // Fixed / AboveStatusBarAt (x)
  int statusRow = 0;             // AboveStatusBarAt
  int widthPct = 0, heightPct = 0;  // CenteredOverlay (clamped 1..100)

  static PopupPosition atCursor() { return {PopupPositionKind::AtCursor}; }
  static PopupPosition belowCursor() { return {PopupPositionKind::BelowCursor}; }
  static PopupPosition aboveCursor() { return {PopupPositionKind::AboveCursor}; }
  static PopupPosition fixed(int x, int y) {
    PopupPosition p{PopupPositionKind::Fixed};
    p.x = x;
    p.y = y;
    return p;
  }
  static PopupPosition centered() { return {PopupPositionKind::Centered}; }
  static PopupPosition centeredOverlay(int wPct, int hPct) {
    PopupPosition p{PopupPositionKind::CenteredOverlay};
    p.widthPct = wPct;
    p.heightPct = hPct;
    return p;
  }
  static PopupPosition bottomRight() { return {PopupPositionKind::BottomRight}; }
  static PopupPosition aboveStatusBarAt(int x, int statusRow) {
    PopupPosition p{PopupPositionKind::AboveStatusBarAt};
    p.x = x;
    p.statusRow = statusRow;
    return p;
  }
};

struct PopupListItem {
  std::string text;
  std::string detail;  // secondary text; empty = none
  std::string icon;    // prefix; empty = none
  std::string data;    // payload for the confirm path; empty = header-only row
  bool disabled = false;
  bool separator = false;  // group divider: rendered as a rule, never selectable

  explicit PopupListItem(std::string t) : text(std::move(t)) {}

  static PopupListItem divider() {
    PopupListItem it("");
    it.separator = true;
    it.disabled = true;
    return it;
  }
};

// (line, col) half-open selection over the popup's wrapped text lines.
struct PopupTextSelection {
  size_t startLine = 0, startCol = 0;
  size_t endLine = 0, endCol = 0;

  void normalized(size_t &sl, size_t &sc, size_t &el, size_t &ec) const;
  bool contains(size_t line, size_t col) const;
};

struct PopupTheme {
  Color borderFg = Color::fromRgb(0x66, 0x66, 0x66);
  Color bg = Color::fromRgb(0x25, 0x25, 0x26);
  Color textFg = Color::fromRgb(0xD4, 0xD4, 0xD4);
  Color selectionBg = Color::fromRgb(0x26, 0x4F, 0x78);
  Color selectionFg = Color::fromRgb(0xFF, 0xFF, 0xFF);
  Color mutedFg = Color::fromRgb(0x85, 0x85, 0x85);  // details, disabled rows
  ScrollbarColors scrollbar{Color::fromRgb(0x30, 0x30, 0x30),
                            Color::fromRgb(0x60, 0x60, 0x60)};
};

// Word-wrap plain text lines to a display width (display-width aware; long
// words hard-split; an empty line stays one line).
std::vector<std::string> WrapTextLines(const std::vector<std::string> &lines,
                                       size_t wrapWidth);

struct Popup {
  PopupKind kind = PopupKind::Text;
  std::string title;  // empty = none
  // Optional input row on the first inner line (command palette, find):
  // rendered as "> " + input + block caret; the list/content starts below it
  // and every height computation accounts for the row.
  bool showInput = false;
  std::string input;
  bool isList = false;
  std::vector<std::string> textLines;   // when !isList
  std::vector<PopupListItem> items;     // when isList
  size_t selected = 0;                  // list selection
  PopupPosition position = PopupPosition::atCursor();
  int width = 50;
  int maxHeight = 15;
  bool bordered = true;
  bool showClose = true;  // "[×]" on the top border (menus turn it off)
  size_t scrollOffset = 0;
  bool hasTextSelection = false;
  PopupTextSelection textSelection{};

  static Popup text(std::vector<std::string> lines);
  static Popup list(std::vector<PopupListItem> listItems);

  const PopupListItem *selectedItem() const;
  size_t itemCount() const;
  size_t visibleHeight() const;  // maxHeight minus borders

  // Selection movement scrolls the view to follow; wheel/scrollbar movement
  // (scrollBy) moves the view ONLY — Enter keeps committing the same row.
  void selectNext();
  void selectPrev();
  bool selectIndex(size_t index);
  void selectFirst();
  void selectLast();
  void pageDown();
  void pageUp();
  void scrollBy(int delta);

  bool needsScrollbar() const;

  // Text selection over the wrapped lines.
  void startSelection(size_t line, size_t col);
  void extendSelection(size_t line, size_t col);
  void clearSelection();
  bool hasSelection() const;
  std::vector<std::string> wrappedTextLines() const;
  bool selectedText(std::string &out) const;

  // Height the content wants at this width (wrapped lines + borders).
  int contentHeight() const;

  // Where to render within terminalArea. cursorX/cursorY are absolute screen
  // coordinates (< 0 = no cursor: falls back to the area's center). Clamps
  // against the area's ABSOLUTE edges so a dock-offset area (x > 0) never
  // pulls the popup under the dock.
  Rect calculateArea(const Rect &terminalArea, int cursorX, int cursorY) const;

  // Paint: clear, border + title + close button, content (scrolled), list
  // styling (icon/detail/disabled/selection), scrollbar when needed.
  void render(const Rect &area, const Rect &bounds, const PopupTheme &theme,
              CellSurface &out) const;
};

}  // namespace npptui

#endif
