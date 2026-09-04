// Horizontal menu bar renderer (T3.2): the top row of the UI, one clickable
// title per menu, with the open menu highlighted. Dropdowns are ordinary
// list popups anchored under the title. Curses-free (T2.4 discipline).

#ifndef NPP_TUI_CHROME_MENUBAR_H
#define NPP_TUI_CHROME_MENUBAR_H

#include <string>
#include <vector>

#include "cellsurface.h"
#include "geometry.h"

namespace npptui {

struct SceneMenuBar {
  std::vector<std::string> titles;
  int active = -1;  // index of the open menu, -1 = none
};

struct MenuBarTheme {
  Color barBg = Color::fromRgb(0x2B, 0x2B, 0x2C);
  Color fg = Color::fromRgb(0xC8, 0xC8, 0xC8);
  Color activeBg = Color::fromRgb(0x26, 0x4F, 0x78);
  Color activeFg = Color::fromRgb(0xFF, 0xFF, 0xFF);
};

// Renders the bar across row `y` (full surface width) and appends one
// absolute span per painted title (clipped titles get none). The active
// title takes the highlight colors + bold.
void RenderMenuBar(const SceneMenuBar &bar, int y, CellSurface &out,
                   std::vector<Rect> &spans, const MenuBarTheme &theme);

}  // namespace npptui

#endif
