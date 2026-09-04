// Tab bar renderer (T2.7a) — see tabbar.h.

#include "tabbar.h"

#include "statusbar.h"  // DisplayWidthUtf8

namespace npptui {

// Ellipsize a UTF-8 title to at most `maxWidth` display cells.
static std::string ellipsize(const std::string &title, int maxWidth) {
  if (DisplayWidthUtf8(title) <= maxWidth) return title;
  std::string out;
  int width = 0;
  size_t i = 0;
  while (i < title.size()) {
    size_t len = 1;
    unsigned char c = (unsigned char)title[i];
    if (c >= 0xF0) len = 4;
    else if (c >= 0xE0) len = 3;
    else if (c >= 0xC2) len = 2;
    if (i + len > title.size()) len = 1;
    std::string ch = title.substr(i, len);
    int w = DisplayWidthUtf8(ch);
    if (width + w > maxWidth - 1) break;  // reserve one cell for the ellipsis
    out += ch;
    width += w;
    i += len;
  }
  return out + "…";
}

void RenderTabBar(const SceneTabs &tabs, int y, CellSurface &out,
                  std::vector<Rect> &spans, const TabBarTheme &theme) {
  int width = out.width();
  CellStyle bar;
  bar.bg = theme.barBg;
  out.fill(Rect{0, y, width, 1}, " ", bar);

  int x = 0;
  for (const SceneTabs::Tab &tab : tabs.tabs) {
    std::string label = " " + ellipsize(tab.title, kMaxTabTitle);
    if (tab.dirty) label += " •";
    label += " ";
    int w = DisplayWidthUtf8(label);
    if (x + w > width) break;  // clipped tabs get no span

    CellStyle style;
    style.bg = tab.active ? theme.activeBg : theme.inactiveBg;
    style.fg = tab.active ? theme.activeFg : theme.inactiveFg;
    style.bold = tab.active;
    out.fill(Rect{x, y, w, 1}, " ", style);
    out.put(x, y, label, style);
    spans.push_back(Rect{x, y, w, 1});
    x += w + 1;  // one bar-colored cell between tabs
  }
}

}  // namespace npptui
