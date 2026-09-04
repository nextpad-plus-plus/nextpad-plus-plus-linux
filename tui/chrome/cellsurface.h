// A minimal styled cell surface the chrome renderers paint onto (T2.5).
// Concrete implementations: a curses window (main.mm) and a recording
// surface (tests). Chrome stays curses-free, mirroring the RowSurface
// discipline from T2.4 but two-dimensional and styled.

#ifndef NPP_TUI_CHROME_CELLSURFACE_H
#define NPP_TUI_CHROME_CELLSURFACE_H

#include <string>

#include "../term/colorcaps.h"
#include "geometry.h"

namespace npptui {

struct CellStyle {
  Color fg = Color::def();
  Color bg = Color::def();
  bool bold = false;
  bool dim = false;
  bool underline = false;

  bool operator==(const CellStyle &o) const {
    return fg == o.fg && bg == o.bg && bold == o.bold && dim == o.dim &&
           underline == o.underline;
  }
};

struct CellSurface {
  virtual ~CellSurface() = default;
  virtual int width() const = 0;
  virtual int height() const = 0;
  // Paint a UTF-8 run whose first character lands at (x, y); clipping is the
  // implementation's business.
  virtual void put(int x, int y, const std::string &utf8, const CellStyle &style) = 0;
  // Fill a rect with a single character (default implementation: rows of put).
  virtual void fill(const Rect &r, const std::string &ch, const CellStyle &style) {
    std::string row;
    for (int i = 0; i < r.width; i++) row += ch;
    for (int y = r.y; y < r.bottom(); y++) put(r.x, y, row, style);
  }
};

}  // namespace npptui

#endif
