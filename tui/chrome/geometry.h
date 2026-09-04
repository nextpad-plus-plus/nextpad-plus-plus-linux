// Adapted from Fresh (crates/fresh-editor/src/view/popup.rs and
// view/ui/mod.rs helpers, v0.4.10), under the Nextpad++ provision in Fresh's
// LICENSE. Original copyright (c) Noam Lewis.

#ifndef NPP_TUI_CHROME_GEOMETRY_H
#define NPP_TUI_CHROME_GEOMETRY_H

namespace npptui {

struct Rect {
  int x = 0, y = 0, width = 0, height = 0;
  int right() const { return x + width; }
  int bottom() const { return y + height; }
  bool operator==(const Rect &o) const {
    return x == o.x && y == o.y && width == o.width && height == o.height;
  }
};

inline bool PointInRect(const Rect &r, int col, int row) {
  return col >= r.x && col < r.right() && row >= r.y && row < r.bottom();
}

// Clamp a rectangle to fit within bounds (prevents out-of-bounds painting).
// The result is guaranteed to be fully contained within `bounds`.
inline Rect ClampRectToBounds(const Rect &rect, const Rect &bounds) {
  int maxX = bounds.x + (bounds.width > 0 ? bounds.width - 1 : 0);
  int maxY = bounds.y + (bounds.height > 0 ? bounds.height - 1 : 0);
  int x = rect.x < maxX ? rect.x : maxX;
  int y = rect.y < maxY ? rect.y : maxY;
  if (x < bounds.x) x = bounds.x;
  if (y < bounds.y) y = bounds.y;
  int maxWidth = bounds.right() - x;
  int maxHeight = bounds.bottom() - y;
  Rect out;
  out.x = x;
  out.y = y;
  out.width = rect.width < maxWidth ? rect.width : maxWidth;
  out.height = rect.height < maxHeight ? rect.height : maxHeight;
  if (out.width < 0) out.width = 0;
  if (out.height < 0) out.height = 0;
  return out;
}

}  // namespace npptui

#endif
