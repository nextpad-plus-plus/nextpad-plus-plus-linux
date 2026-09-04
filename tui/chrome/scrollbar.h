// Adapted from Fresh (crates/fresh-editor/src/view/ui/scrollbar.rs, v0.4.10),
// under the Nextpad++ provision in Fresh's LICENSE ("Permission explicitly
// granted to Nextpad++ to read and adapt the Fresh notepad code to work with
// Nextpad++"). Original copyright (c) Noam Lewis.
//
// Reusable scrollbar for popups and lists: thumb geometry, the exact inverse
// mapping for track clicks (offset_for_thumb_top), pin-the-cursor drags, and
// press/drag/release mouse state.

#ifndef NPP_TUI_CHROME_SCROLLBAR_H
#define NPP_TUI_CHROME_SCROLLBAR_H

#include <cstddef>

#include "cellsurface.h"
#include "geometry.h"

namespace npptui {

struct ScrollbarState {
  size_t totalItems = 0;
  size_t visibleItems = 0;
  size_t scrollOffset = 0;

  ScrollbarState() = default;
  ScrollbarState(size_t total, size_t visible, size_t offset)
      : totalItems(total), visibleItems(visible), scrollOffset(offset) {}

  // (thumbStart, thumbSize) in rows for a track of the given height. A fully
  // visible content fills the whole track; thumb size is capped at 80% of
  // the track and floored at 1 row.
  void thumbGeometry(size_t trackHeight, size_t &thumbStart, size_t &thumbSize) const;

  bool isThumbRow(size_t trackHeight, size_t row) const;

  // Inverse of thumbGeometry: the scroll offset that places the thumb's top
  // at (or nearest to) targetThumbTop. Ceiling division plus a
  // nearest-of-two check, because thumbGeometry floors and not every track
  // row is reachable when there are fewer scroll positions than rows.
  size_t offsetForThumbTop(size_t trackHeight, size_t targetThumbTop) const;

  // Drag that preserves the cursor's position within the thumb: computed
  // from the cursor delta so a zero-movement press cannot drift the offset.
  size_t dragToOffset(size_t trackHeight, size_t dragStartRow, size_t dragStartOffset,
                      size_t currentRow) const;
};

struct ScrollbarDrag {
  size_t startRow = 0;
  size_t startOffset = 0;
};

// Press/drag/release state. Each handler returns true and sets newOffset
// when the event was the scrollbar's to handle: a press on the thumb anchors
// without moving; a press on the track recentres the thumb on the cursor.
struct ScrollbarMouse {
  bool dragging = false;
  ScrollbarDrag dragState{};

  bool press(const ScrollbarState &state, const Rect &track, int col, int row,
             size_t &newOffset);
  bool drag(const ScrollbarState &state, const Rect &track, int row, size_t &newOffset);
  void release() { dragging = false; }
};

struct ScrollbarColors {
  Color track;
  Color thumb;
};

// Vertical scrollbar into a 1-column area, drawn as background fills (glyphs
// leave gaps in some terminals).
void RenderScrollbar(CellSurface &out, const Rect &area, const ScrollbarState &state,
                     const ScrollbarColors &colors);

}  // namespace npptui

#endif
