// Docked side panel renderer (Phase 4.5) — see panel.h.

#include "panel.h"

#include "statusbar.h"  // DisplayWidthUtf8

namespace npptui {

void ScenePanel::moveSelection(int delta, int height) {
  if (rows.empty()) return;
  long next = (long)selected + delta;
  if (next < 0) next = 0;
  if (next >= (long)rows.size()) next = (long)rows.size() - 1;
  selected = (size_t)next;
  clampScroll(height);
}

void ScenePanel::clampScroll(int height) {
  size_t visible = (size_t)visibleRows(height);
  if (visible == 0) return;
  if (selected < scrollOffset) scrollOffset = selected;
  else if (selected >= scrollOffset + visible) scrollOffset = selected - visible + 1;
  if (rows.size() > visible && scrollOffset > rows.size() - visible)
    scrollOffset = rows.size() - visible;
}

// Truncate a UTF-8 string to a display width (no ellipsis — panel rows are
// narrow and the full name is in the buffer list anyway).
static std::string clipTo(const std::string &s, int width) {
  if (width <= 0) return "";
  int used = 0;
  size_t i = 0;
  std::string out;
  while (i < s.size()) {
    size_t len = 1;
    unsigned char c = (unsigned char)s[i];
    if (c >= 0xF0) len = 4;
    else if (c >= 0xE0) len = 3;
    else if (c >= 0xC2) len = 2;
    if (i + len > s.size()) len = 1;
    std::string ch = s.substr(i, len);
    int w = DisplayWidthUtf8(ch);
    if (used + w > width) break;
    out += ch;
    used += w;
    i += len;
  }
  return out;
}

void RenderPanel(const ScenePanel &panel, const Rect &area, CellSurface &out,
                 const PanelTheme &theme) {
  if (area.width < 3 || area.height < 1) return;
  int textWidth = area.width - 1;  // rightmost column = border

  CellStyle base;
  base.bg = theme.bg;
  base.fg = theme.fg;
  out.fill(area, " ", base);

  // Title row.
  CellStyle titleStyle;
  titleStyle.bg = theme.titleBg;
  titleStyle.fg = theme.titleFg;
  titleStyle.bold = panel.focused;
  out.fill(Rect{area.x, area.y, textWidth, 1}, " ", titleStyle);
  out.put(area.x + 1, area.y, clipTo(panel.title, textWidth - 2), titleStyle);

  // Rows.
  int visible = ScenePanel::visibleRows(area.height);
  for (int r = 0; r < visible; r++) {
    size_t idx = panel.scrollOffset + (size_t)r;
    if (idx >= panel.rows.size()) break;
    const PanelRow &row = panel.rows[idx];
    bool isSel = idx == panel.selected;
    CellStyle rowStyle = base;
    if (isSel) {
      rowStyle.bg = theme.selBg;
      rowStyle.fg = theme.selFg;
      rowStyle.bold = panel.focused;  // unfocused: band stays, bold drops
    }
    int y = area.y + 1 + r;
    out.fill(Rect{area.x, y, textWidth, 1}, " ", rowStyle);
    std::string label = row.text + (row.dirty ? " •" : "");
    std::string shown = clipTo(label, textWidth - 2);
    out.put(area.x + 1, y, shown, rowStyle);
    if (!row.detail.empty()) {
      int used = 1 + DisplayWidthUtf8(shown);
      int room = textWidth - used - 2;
      if (room >= 4) {
        CellStyle det = rowStyle;
        det.fg = isSel ? theme.selFg : theme.mutedFg;
        std::string d = clipTo(row.detail, room);
        out.put(area.x + textWidth - 1 - DisplayWidthUtf8(d), y, d, det);
      }
    }
  }

  // Right border, full height.
  CellStyle borderStyle = base;
  borderStyle.fg = theme.borderFg;
  for (int y = area.y; y < area.bottom(); y++)
    out.put(area.x + textWidth, y, "│", borderStyle);
}

}  // namespace npptui
