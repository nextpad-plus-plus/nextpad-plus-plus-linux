// Docked side panel (Phase 4.5): a persistent left-dock pane — the TUI's
// version of the GUI's side panels (Document List first; Function List and
// Project Panel build on the same frame). Scene discipline: plain data in,
// cells out; no curses, no ObjC. The app owns focus routing, content
// (re)building, and activation.

#ifndef NPP_TUI_CHROME_PANEL_H
#define NPP_TUI_CHROME_PANEL_H

#include <string>
#include <vector>

#include "cellsurface.h"
#include "geometry.h"

namespace npptui {

struct PanelRow {
  std::string text;    // display name
  std::string detail;  // muted suffix (folder, signature); clipped first
  bool dirty = false;  // renders the • marker
};

struct ScenePanel {
  std::string title;
  std::vector<PanelRow> rows;
  size_t selected = 0;
  size_t scrollOffset = 0;
  bool focused = false;  // focused = selection band bright; else muted

  // Rows visible inside `height` (minus the title row).
  static int visibleRows(int height) { return height > 1 ? height - 1 : 0; }
  void moveSelection(int delta, int height);
  void clampScroll(int height);
};

struct PanelTheme {
  Color bg = Color::fromRgb(0x25, 0x25, 0x26);
  Color fg = Color::fromRgb(0xD4, 0xD4, 0xD4);
  Color titleBg = Color::fromRgb(0x2D, 0x2D, 0x2D);
  Color titleFg = Color::fromRgb(0xFF, 0xFF, 0xFF);
  Color selBg = Color::fromRgb(0x26, 0x4F, 0x78);
  Color selFg = Color::fromRgb(0xFF, 0xFF, 0xFF);
  Color mutedFg = Color::fromRgb(0x85, 0x85, 0x85);
  Color borderFg = Color::fromRgb(0x66, 0x66, 0x66);
};

// Paints the panel into `area`: title row, rows with selection band and
// dirty markers, and a │ border down the RIGHT edge separating it from the
// editor. Details are right-trimmed to fit; the selection band spans the
// full row. The unfocused selection keeps the band but drops the bold.
void RenderPanel(const ScenePanel &panel, const Rect &area, CellSurface &out,
                 const PanelTheme &theme);

}  // namespace npptui

#endif
