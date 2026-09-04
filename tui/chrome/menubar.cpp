// Horizontal menu bar renderer (T3.2) — see menubar.h.

#include "menubar.h"

#include "statusbar.h"  // DisplayWidthUtf8

namespace npptui {

void RenderMenuBar(const SceneMenuBar &bar, int y, CellSurface &out,
                   std::vector<Rect> &spans, const MenuBarTheme &theme) {
  int width = out.width();
  CellStyle barStyle;
  barStyle.bg = theme.barBg;
  out.fill(Rect{0, y, width, 1}, " ", barStyle);

  int x = 1;
  for (size_t i = 0; i < bar.titles.size(); i++) {
    std::string label = " " + bar.titles[i] + " ";
    int w = DisplayWidthUtf8(label);
    if (x + w > width) break;  // clipped titles get no span

    CellStyle style;
    bool active = (int)i == bar.active;
    style.bg = active ? theme.activeBg : theme.barBg;
    style.fg = active ? theme.activeFg : theme.fg;
    style.bold = active;
    out.put(x, y, label, style);
    spans.push_back(Rect{x, y, w, 1});
    x += w + 1;
  }
}

}  // namespace npptui
