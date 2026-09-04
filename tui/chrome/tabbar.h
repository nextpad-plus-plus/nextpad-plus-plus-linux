// Tab bar renderer (T2.7a): draws the buffer tabs from a SceneTabs
// projection onto one CellSurface row and reports each tab's clickable span
// for mouse hit-testing. Curses-free (T2.4 discipline).

#ifndef NPP_TUI_CHROME_TABBAR_H
#define NPP_TUI_CHROME_TABBAR_H

#include <vector>

#include "../scene/scene.h"
#include "cellsurface.h"
#include "geometry.h"

namespace npptui {

struct TabBarTheme {
  Color barBg = Color::fromRgb(0x25, 0x25, 0x26);
  Color activeBg = Color::fromRgb(0x1E, 0x1E, 0x1E);  // merges with the editor
  Color activeFg = Color::fromRgb(0xFF, 0xFF, 0xFF);
  Color inactiveBg = Color::fromRgb(0x2D, 0x2D, 0x2D);
  Color inactiveFg = Color::fromRgb(0x96, 0x96, 0x96);
};

// Longest title shown before ellipsizing ("longname…").
constexpr int kMaxTabTitle = 20;

// Renders the bar across row `y` of `out` (full surface width) and appends
// one absolute-coordinate span per *painted* tab to `spans` (tabs that no
// longer fit are clipped and get no span). Active tab: editor-bg + bold;
// dirty tabs carry a bullet.
void RenderTabBar(const SceneTabs &tabs, int y, CellSurface &out,
                  std::vector<Rect> &spans, const TabBarTheme &theme);

}  // namespace npptui

#endif
